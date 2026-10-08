#!/usr/bin/env python3
"""Stream the generator's TSV files into a 20-post JSON seed. No database dependency."""
import argparse
import heapq
import json
from pathlib import Path

MAX_ID = 2**63 - 1


def positive_id(raw):
    if not raw.isascii() or not raw.isdigit() or not 0 < int(raw) <= MAX_ID:
        raise ValueError(f"invalid positive id: {raw!r}")
    return int(raw)


def export_seed(source):
    latest = []
    maximum = 0
    with (source / "posts.tsv").open(encoding="utf-8") as file:
        for line in file:
            raw_id, raw_user, body, timestamp = line.rstrip("\r\n").split("\t")
            pid, uid = positive_id(raw_id), positive_id(raw_user)
            maximum = max(maximum, pid)
            if not 1 <= len(body) <= 500:
                raise ValueError("seed post body must contain 1–500 characters")
            row = (timestamp, pid, uid, body)
            if len(latest) < 20:
                heapq.heappush(latest, row)
            elif row[:2] > latest[0][:2]:
                heapq.heapreplace(latest, row)
    if maximum == MAX_ID:
        raise ValueError("seed post ids leave no next id")

    # Keep names only for authors of retained posts, rather than all 50,000 users.
    author_ids = {row[2] for row in latest}
    names = {}
    with (source / "users.tsv").open(encoding="utf-8") as file:
        for line in file:
            raw_id, name, _ = line.rstrip("\r\n").split("\t")
            uid = positive_id(raw_id)
            if uid in author_ids:
                names[uid] = name
    if names.keys() != author_ids:
        raise ValueError("a retained post has no author")
    likes = {row[1]: set() for row in latest}
    with (source / "likes.tsv").open(encoding="utf-8") as file:
        for line in file:
            raw_user, raw_post, _ = line.rstrip("\r\n").split("\t")
            pid = positive_id(raw_post)
            if pid in likes:
                likes[pid].add(positive_id(raw_user))
    return {
        "next_id": maximum + 1,
        "posts": [{"id": pid, "body": body, "created_at": timestamp, "author": names[uid],
                   "liked_user_ids": sorted(likes[pid]), "like_count": len(likes[pid])}
                  for timestamp, pid, uid, body in sorted(latest, reverse=True)],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tsv_directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    seed = export_seed(args.tsv_directory)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(seed, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print(f"Exported {len(seed['posts'])} posts and "
          f"{sum(len(p['liked_user_ids']) for p in seed['posts'])} likes; next id {seed['next_id']}")


if __name__ == "__main__":
    main()
