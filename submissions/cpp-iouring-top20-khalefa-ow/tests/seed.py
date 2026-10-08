#!/usr/bin/env python3
"""Check TSV selection, global ids, memberships, and the production C++ seed loader."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("export_seed", HERE / "tools/export_seed.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def main():
    with tempfile.TemporaryDirectory(prefix="top20-seed-check-") as directory:
        directory = Path(directory)
        # Input order and id order differ from time order; two posts tie in each second.
        posts = [(i, 1 + i % 2, f"row-{i} café", f"2025-12-31T23:59:{i % 15:02d}.000Z")
                 for i in range(1, 31)]
        posts.append((9007199254741999, 1, "largest id, oldest timestamp", "2025-01-01T00:00:00.000Z"))
        posts.append((9007199254741001, 2, "large retained id", "2025-12-31T23:59:59.001Z"))
        (directory / "posts.tsv").write_text(
            "".join(f"{pid}\t{uid}\t{body}\t{ts}\n" for pid, uid, body, ts in reversed(posts)), encoding="utf-8")
        (directory / "users.tsv").write_text("1\tfirst\t2025-01-01T00:00:00.000Z\n2\tsecond\t2025-01-01T00:00:00.000Z\n")
        (directory / "likes.tsv").write_text(
            "".join(f"{uid}\t{pid}\t2025-12-31T23:59:59.000Z\n"
                    for pid, _, _, _ in posts for uid in (1, 1, 65536, 65537, 9223372036854775807)))
        seed = module.export_seed(directory)
        expected = sorted(posts, key=lambda p: (p[3], p[0]), reverse=True)[:20]
        assert [p["id"] for p in seed["posts"]] == [p[0] for p in expected]
        assert seed["next_id"] == 9007199254742000
        assert all(len(p["liked_user_ids"]) == 4 for p in seed["posts"])
        assert all(p["like_count"] == 4 for p in seed["posts"])
        seed_path = directory / "seed.json"
        seed_path.write_text(json.dumps(seed, ensure_ascii=False), encoding="utf-8")
        result = subprocess.run([str(HERE / "bin/core-test"), "--inspect-seed", str(seed_path)],
                                capture_output=True, text=True, check=True)
        state = json.loads(result.stdout)
        feed = state["feed"]["posts"]
        assert state["next_id"] == seed["next_id"]
        assert [p["id"] for p in feed] == [p[0] for p in expected]
        assert all(p["like_count"] == 4 for p in feed)
        assert feed[0]["author"] == ("first" if expected[0][1] == 1 else "second")
        assert feed[0]["body"] == expected[0][2]

        valid = json.loads(json.dumps(seed))
        # A counter above 2^53 stays exact; the loader rejects ids outside signed 64 bits.
        invalid = [
            {"next_id": seed["next_id"], "posts": seed["posts"] + [seed["posts"][0]]},
            {"next_id": 1, "posts": seed["posts"]},
            {"next_id": 9223372036854775808, "posts": []},
            {"next_id": seed["next_id"], "posts": [seed["posts"][0], seed["posts"][0]]},
        ]
        for field, value in (("liked_user_ids", [True]), ("liked_user_ids", ["1"]),
                             ("like_count", 3), ("like_count", "4"), ("like_count", 4.0),
                             ("body", ""), ("created_at", "bad"), ("created_at", "2025-02-29T00:00:00.000Z")):
            edited = json.loads(json.dumps(valid))
            edited["posts"][0][field] = value
            invalid.append(edited)
        for candidate in invalid:
            seed_path.write_text(json.dumps(candidate), encoding="utf-8")
            failed = subprocess.run([str(HERE / "bin/core-test"), "--dump-seed", str(seed_path)],
                                    capture_output=True, text=True)
            assert failed.returncode == 1 and "invalid seed:" in failed.stderr
        # Empty seed is supported for a clean experiment.
        seed_path.write_text('{"next_id":1,"posts":[]}')
        result = subprocess.run([str(HERE / "bin/core-test"), "--dump-seed", str(seed_path)],
                                capture_output=True, text=True, check=True)
        assert json.loads(result.stdout) == {"posts": []}
    print("PASS: TSV selection, membership deduplication, exact 64-bit ids, C++ seed loader validation")


if __name__ == "__main__":
    main()
