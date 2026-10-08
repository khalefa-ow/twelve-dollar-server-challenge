#!/usr/bin/env python3
"""Linux integration checks; runs the actual io_uring binary on its own fresh database."""
import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent.parent
ROOT = HERE.parent.parent
SECRET = b"twelve-dollar-challenge"


def token(uid=1, username="golden_ember_1"):
    def b64(raw):
        return base64.urlsafe_b64encode(raw).rstrip(b"=")
    head = b64(b'{"alg":"HS256","typ":"JWT"}')
    payload = b64(json.dumps({"sub": str(uid), "username": username,
                              "exp": int(time.time()) + 3600}, separators=(",", ":")).encode())
    data = head + b"." + payload
    return (data + b"." + b64(hmac.new(SECRET, data, hashlib.sha256).digest())).decode()


def wire(method, path, body=b"", auth=None, close=False):
    headers = [f"{method} {path} HTTP/1.1", "Host: localhost", f"Content-Length: {len(body)}"]
    if auth:
        headers.append(f"Authorization: Bearer {auth}")
    if close:
        headers.append("Connection: close")
    return ("\r\n".join(headers) + "\r\n\r\n").encode() + body


async def response(reader):
    raw = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
    lines = raw.decode().split("\r\n")
    status = int(lines[0].split()[1])
    headers = dict(line.lower().split(":", 1) for line in lines[1:] if ":" in line)
    assert headers["content-type"].strip().startswith("application/json")
    body = await asyncio.wait_for(reader.readexactly(int(headers["content-length"])), 10)
    return status, json.loads(body)


async def connect(port):
    return await asyncio.open_connection("127.0.0.1", port)


async def close_writer(writer):
    writer.close()
    try:
        await writer.wait_closed()
    except (ConnectionError, OSError):
        pass


async def checks(port, db_path, idle_count):
    reader, writer = await connect(port)
    # Fragment every byte, including the JSON UTF-8 and Content-Length body boundary.
    data = wire("POST", "/posts", json.dumps({"body": "  fragmented café ✓  "}, ensure_ascii=False).encode(), token())
    for byte in data:
        writer.write(bytes([byte]))
        await writer.drain()
        await asyncio.sleep(0)
    status, created = await response(reader)
    assert status == 201 and created["post"]["body"] == "fragmented café ✓"
    pid = created["post"]["id"]

    # Same-socket writes/reads must preserve order across the commit boundary.
    writer.write(wire("POST", f"/posts/{pid}/like", auth=token()) +
                 wire("GET", f"/posts/{pid}") +
                 wire("POST", f"/posts/{pid}/like", auth=token()) +
                 wire("GET", "/health"))
    await writer.drain()
    a, b, c, d = [await response(reader) for _ in range(4)]
    assert a[0] == 201 and not a[1]["already_liked"]
    assert b[0] == 200 and b[1]["post"]["like_count"] == 1
    assert c[0] == 200 and c[1]["already_liked"]
    assert d[0] == 200

    # More than one fairness pass, plus several partial sends to a slow reader.
    writer.write(wire("GET", "/feed") * 80)
    await writer.drain()
    await asyncio.sleep(0.1)
    for _ in range(80):
        status, feed = await response(reader)
        assert status == 200 and len(feed["posts"]) == 20
    await close_writer(writer)

    # Chunked body, including trailers and a request following the body in the same send.
    reader, writer = await connect(port)
    auth = token()
    payload = b'{"body":"chunked body"}'
    writer.write((f"POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {auth}\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n").encode() +
                 f"{len(payload):x}\r\n".encode() + payload + b"\r\n0\r\nX-Note: trailer\r\n\r\n" +
                 wire("GET", "/health", close=True))
    await writer.drain()
    assert (await response(reader))[0] == 201
    assert (await response(reader))[0] == 200
    assert await asyncio.wait_for(reader.read(), 5) == b""
    await close_writer(writer)

    # Release many clients together so the completion loop can group writes naturally.
    barrier = asyncio.Event()
    ready_count = 0
    all_ready = asyncio.Event()

    async def create(i):
        nonlocal ready_count
        r, w = await connect(port)
        ready_count += 1
        if ready_count == 256:
            all_ready.set()
        await barrier.wait()
        text = f"parallel request {i}"
        w.write(wire("POST", "/posts", json.dumps({"body": text}).encode(), token(i + 1, f"user-{i}")))
        await w.drain()
        status, result = await response(r)
        assert status == 201 and result["post"]["body"] == text
        # Inspect the file immediately after each success, not after the whole test finishes.
        with sqlite3.connect(db_path) as db:
            row = db.execute("SELECT body FROM posts WHERE id=?", (result["post"]["id"],)).fetchone()
            assert row == (text,), "201 arrived before row was committed"
        await close_writer(w)
        return result["post"]["id"]

    tasks = [asyncio.create_task(create(i)) for i in range(256)]
    await asyncio.wait_for(all_ready.wait(), 15)
    barrier.set()
    ids = await asyncio.gather(*tasks)
    assert len(set(ids)) == 256

    # Concurrent duplicate likes must produce one 201 and the rest 200.
    async def like(_):
        r, w = await connect(port)
        w.write(wire("POST", f"/posts/{ids[0]}/like", auth=token()))
        await w.drain()
        answer = await response(r)
        await close_writer(w)
        return answer[0]
    statuses = await asyncio.gather(*(like(i) for i in range(64)))
    assert statuses.count(201) == 1 and statuses.count(200) == 63

    # Disconnects while receives/sends/writes are outstanding must not recycle CQE pointers.
    for i in range(256):
        r, w = await connect(port)
        data = wire("GET", "/feed") if i % 2 else wire("POST", "/posts", b'{"body":"disconnect"}', token())
        w.write(data if i % 3 else data[:20])
        await w.drain()
        w.transport.abort()
    await asyncio.sleep(0.2)

    if idle_count:
        idle = []
        for _ in range(idle_count):
            r, w = await connect(port)
            idle.append((r, w))
        r, w = await connect(port)
        w.write(wire("GET", "/feed"))
        await w.drain()
        assert (await response(r))[0] == 200
        await close_writer(w)
        for _, w in idle:
            await close_writer(w)

    r, w = await connect(port)
    w.write(wire("GET", "/health"))
    await w.drain()
    assert (await response(r))[0] == 200
    await close_writer(w)
    print("PASS: fragmentation, pipelining, chunked bodies, concurrent commits, duplicates, disconnects" +
          (f", {idle_count} idle sockets" if idle_count else ""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--connections", type=int, default=0)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="iouring-check-") as scratch:
        db_path = Path(scratch) / "feed.db"
        shutil.copyfile(ROOT / "seed/feed.db", db_path)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = dict(os.environ, SQLITE_PATH=str(db_path), JWT_SECRET=SECRET.decode(), HOST="127.0.0.1", PORT=str(port))
        with (Path(scratch) / "server.log").open("w+") as log:
            process = subprocess.Popen([str(HERE / "bin/server")], env=env, stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if process.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read())
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError("server did not listen")
                asyncio.run(checks(port, db_path, args.connections))
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise RuntimeError("server shutdown did not drain io_uring operations")
                log.seek(0)
                output = log.read()
                print(output.strip())
                if process.returncode != 0:
                    raise RuntimeError(f"server exited {process.returncode}")
                match = re.search(r"largest (\d+)", output)
                if match:
                    print(f"Observed largest commit group: {match[1]} requests")


if __name__ == "__main__":
    main()
