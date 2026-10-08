#!/usr/bin/env python3
"""Linux-only integration checks for the real io_uring server, with no database tools."""
import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent.parent
SECRET = b"twelve-dollar-challenge"


def token(uid=1):
    def b64(value):
        return base64.urlsafe_b64encode(value).rstrip(b"=")
    data = b64(b'{"alg":"HS256"}') + b"." + b64(json.dumps(
        {"sub": str(uid), "username": f"user-{uid}", "exp": int(time.time()) + 3600}).encode())
    return (data + b"." + b64(hmac.new(SECRET, data, hashlib.sha256).digest())).decode()


def wire(method, path, body=b"", uid=None, close=False):
    headers = [f"{method} {path} HTTP/1.1", "Host: localhost", f"Content-Length: {len(body)}"]
    if uid is not None:
        headers.append(f"Authorization: Bearer {token(uid)}")
    if close:
        headers.append("Connection: close")
    return ("\r\n".join(headers) + "\r\n\r\n").encode() + body


async def response(reader):
    header = (await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)).decode().split("\r\n")
    status = int(header[0].split()[1])
    fields = dict(line.lower().split(":", 1) for line in header[1:] if ":" in line)
    assert fields["content-type"].strip().startswith("application/json")
    data = await asyncio.wait_for(reader.readexactly(int(fields["content-length"])), 10)
    return status, json.loads(data)


async def close(writer):
    writer.close()
    try:
        await writer.wait_closed()
    except (ConnectionError, OSError):
        pass


async def exchange(port, message):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write(message)
    await writer.drain()
    answer = await response(reader)
    await close(writer)
    return answer


async def checks(port, idle_count):
    assert await exchange(port, wire("GET", "/feed")) == (200, {"posts": []})
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    message = wire("POST", "/posts", json.dumps({"body": "  fragmented café ✓  "}, ensure_ascii=False).encode(), 1)
    for byte in message:
        writer.write(bytes([byte]))
        await writer.drain()
        await asyncio.sleep(0)
    status, first = await response(reader)
    assert status == 201 and first["post"]["id"] == 1 and first["post"]["body"] == "fragmented café ✓"
    writer.write(wire("POST", "/posts/1/like", uid=1) + wire("GET", "/posts/1") +
                 wire("POST", "/posts/1/like", uid=1))
    await writer.drain()
    a, b, c = [await response(reader) for _ in range(3)]
    assert a[0] == 201 and b[1]["post"]["like_count"] == 1 and c[0] == 200
    # Fill and wrap the entire ring within one pipelined send.
    writer.write(wire("POST", "/posts", b'{"body":"ring wrap"}', 1) * 20 + wire("GET", "/feed"))
    await writer.drain()
    for _ in range(20):
        assert (await response(reader))[0] == 201
    status, feed = await response(reader)
    assert status == 200 and [p["id"] for p in feed["posts"]] == list(range(21, 1, -1))
    assert all(p["like_count"] == 0 for p in feed["posts"])
    await close(writer)
    assert await exchange(port, wire("GET", "/posts/1")) == (404, {"error": "post not found"})
    assert await exchange(port, wire("POST", "/posts/1/like", uid=1)) == (404, {"error": "post not found"})

    # Concurrent mutations still serialize on the event loop, with no lost/duplicate ids.
    barrier = asyncio.Event()
    ready = asyncio.Event()
    count = 0

    async def create(i):
        nonlocal count
        r, w = await asyncio.open_connection("127.0.0.1", port)
        count += 1
        if count == 256:
            ready.set()
        await barrier.wait()
        w.write(wire("POST", "/posts", json.dumps({"body": f"concurrent {i}"}).encode(), i + 1))
        await w.drain()
        status, answer = await response(r)
        assert status == 201
        await close(w)
        return answer["post"]["id"]

    tasks = [asyncio.create_task(create(i)) for i in range(256)]
    await asyncio.wait_for(ready.wait(), 15)
    barrier.set()
    ids = await asyncio.gather(*tasks)
    assert len(set(ids)) == 256
    status, feed = await exchange(port, wire("GET", "/feed"))
    assert status == 200 and [p["id"] for p in feed["posts"]] == list(range(max(ids), max(ids) - 20, -1))
    pid = max(ids)
    answers = await asyncio.gather(*(exchange(port, wire("POST", f"/posts/{pid}/like", uid=1)) for _ in range(64)))
    assert [status for status, _ in answers].count(201) == 1
    assert [status for status, _ in answers].count(200) == 63
    answers = await asyncio.gather(*(exchange(port, wire("POST", f"/posts/{pid}/like", uid=100 + i)) for i in range(64)))
    assert all(status == 201 for status, _ in answers)
    _, feed = await exchange(port, wire("GET", "/feed"))
    assert feed["posts"][0]["like_count"] == 65

    # Backpressure/fairness: more than 32 requests and 64 KiB of responses before the client reads.
    r, w = await asyncio.open_connection("127.0.0.1", port)
    w.write(wire("GET", "/feed") * 80)
    await w.drain()
    await asyncio.sleep(0.1)
    for _ in range(80):
        status, f = await response(r)
        assert status == 200 and f == feed
    await close(w)

    # Chunked create followed by a close request in the same packet.
    r, w = await asyncio.open_connection("127.0.0.1", port)
    body = b'{"body":"chunked"}'
    w.write((f"POST /posts HTTP/1.1\r\nAuthorization: Bearer {token()}\r\n"
             "Transfer-Encoding: chunked\r\n\r\n").encode() + f"{len(body):x}\r\n".encode() +
            body + b"\r\n0\r\n\r\n" + wire("GET", "/health", close=True))
    await w.drain()
    assert (await response(r))[0] == 201
    assert (await response(r))[1]["store"] == "memory-wal"
    assert await asyncio.wait_for(r.read(), 5) == b""
    await close(w)
    for _ in range(128):
        _, w = await asyncio.open_connection("127.0.0.1", port)
        w.write(wire("GET", "/feed")[:20])
        await w.drain()
        w.transport.abort()
    idle = [await asyncio.open_connection("127.0.0.1", port) for _ in range(idle_count)]
    assert (await exchange(port, wire("GET", "/health")))[0] == 200
    for _, w in idle:
        await close(w)
    _, final_feed = await exchange(port, wire("GET", "/feed"))
    latest = final_feed["posts"][0]["id"]
    for uid in (1, 65537):
        assert (await exchange(port, wire("POST", f"/posts/{latest}/like", uid=uid)))[0] == 201
    _, final_feed = await exchange(port, wire("GET", "/feed"))
    print("PASS: 20-post eviction, concurrent mutations, per-user deduplication, pipeline/fragmentation/backpressure" +
          (f", {idle_count} idle sockets" if idle_count else ""))
    return final_feed


def run_server(port, log, store, work, crash=False):
    env = dict(os.environ, TOP20_SEED="", STORE_DIR=str(store), JWT_SECRET=SECRET.decode(),
               HOST="127.0.0.1", PORT=str(port))
    process = subprocess.Popen([str(HERE / "bin/server")], env=env, stdout=log, stderr=log)
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError(f"server exited {process.returncode}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("server did not listen")
        return asyncio.run(work())
    finally:
        if crash:
            process.kill()  # No shutdown handler/checkpoint: every observed success is already synced.
        else:
            process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError("io_uring shutdown did not drain")
        if process.returncode != (-9 if crash else 0):
            raise RuntimeError(f"server exited {process.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--connections", type=int, default=0)
    args = parser.parse_args()
    if args.connections < 0:
        parser.error("--connections must be nonnegative")
    with tempfile.TemporaryDirectory(prefix="top20-transport-") as directory:
        with socket.socket() as socket_probe:
            socket_probe.bind(("127.0.0.1", 0))
            port = socket_probe.getsockname()[1]
        with (Path(directory) / "server.log").open("w+") as log:
            store = Path(directory) / "store"
            try:
                saved = run_server(port, log, store, lambda: checks(port, args.connections), crash=True)

                async def restarted():
                    assert await exchange(port, wire("GET", "/feed")) == (200, saved)
                    latest = saved["posts"][0]["id"]
                    for uid in (1, 65537):
                        status, body = await exchange(port, wire("POST", f"/posts/{latest}/like", uid=uid))
                        assert status == 200 and body["already_liked"]
                    assert (await exchange(port, wire("GET", "/posts/1")))[0] == 404
                    status, body = await exchange(port, wire("POST", "/posts", b'{"body":"after restart"}', 1))
                    assert status == 201 and body["post"]["id"] == latest + 1
                    status, _ = await exchange(port, wire("POST", f"/posts/{latest + 1}/like", uid=2**63 - 1))
                    assert status == 201
                    print("PASS: SIGKILL recovery retains posts, next id, precomputed counts and per-user membership")
                    return (await exchange(port, wire("GET", "/feed")))[1]
                saved = run_server(port, log, store, restarted)
                wal = store / "journal.wal"
                size = wal.stat().st_size
                with wal.open("ab") as file:
                    file.write(b"T20WAL01\x01\x00")  # Partial final frame header.
                    file.flush()
                    os.fsync(file.fileno())

                async def torn_tail():
                    assert await exchange(port, wire("GET", "/feed")) == (200, saved)
                    assert wal.stat().st_size == size
                    latest = saved["posts"][0]["id"]
                    status, body = await exchange(port, wire("POST", f"/posts/{latest}/like", uid=2**63 - 1))
                    assert status == 200 and body["already_liked"]
                    print("PASS: graceful restart and incomplete WAL tail recovery")
                run_server(port, log, store, torn_tail)
            finally:
                log.seek(0)
                print(log.read().strip())


if __name__ == "__main__":
    main()
