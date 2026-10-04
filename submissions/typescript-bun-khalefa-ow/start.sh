#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
cd "$(dirname "$0")"
exec "$(command -v bun || echo /usr/local/bin/bun)" run server.ts
