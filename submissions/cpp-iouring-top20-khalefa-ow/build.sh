#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ "$(uname -s)" != Linux ]; then
  echo 'The server requires Linux 6.0+ (Ubuntu 24.04).' >&2
  exit 1
fi
mkdir -p vendor/sqlite vendor/liburing bin

fetch() {
  local url="$1" archive="$2" hash="$3"
  if ! [ -f "$archive" ] || ! printf '%s  %s\n' "$hash" "$archive" | sha256sum -c --status; then
    curl --fail --location --retry 3 --connect-timeout 20 --max-time 180 -o "$archive" "$url"
  fi
  printf '%s  %s\n' "$hash" "$archive" | sha256sum -c --quiet
}

fetch https://www.sqlite.org/2026/sqlite-autoconf-3530400.tar.gz \
  vendor/sqlite-3530400.tar.gz 0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
fetch https://codeload.github.com/axboe/liburing/tar.gz/refs/tags/liburing-2.6 \
  vendor/liburing-2.6.tar.gz 682f06733e6db6402c1f904cbbe12b94942a49effc872c9e01db3d7b180917cc
tar -xzf vendor/sqlite-3530400.tar.gz -C vendor/sqlite --strip-components=1 \
  sqlite-autoconf-3530400/sqlite3.c sqlite-autoconf-3530400/sqlite3.h
tar -xzf vendor/liburing-2.6.tar.gz -C vendor/liburing --strip-components=1
(
  cd vendor/liburing
  ./configure --cc=gcc --cxx=g++
  make -C src -j2
)

# Keep C and C++ objects link-compatible on hosts where gcc and g++ come from different
# package versions. The benchmark optimization here is the feed cache, not cross-language LTO.
OPT=(-O3 -march=native -fno-plt)
SQLITE_FLAGS=(-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE)
gcc "${OPT[@]}" "${SQLITE_FLAGS[@]}" -c vendor/sqlite/sqlite3.c -o vendor/sqlite/sqlite3.o
g++ "${OPT[@]}" -std=c++17 -Wall -Wextra -Wpedantic -Wno-deprecated-declarations \
  -Ivendor/sqlite -Ivendor/liburing/src/include src/server.cpp vendor/sqlite/sqlite3.o \
  vendor/liburing/src/liburing.a -lcrypto -lpthread -o bin/server
g++ "${OPT[@]}" -DCHALLENGE_CACHE_FEED=1 -std=c++17 -Wall -Wextra -Wno-deprecated-declarations \
  -Ivendor/sqlite ../cpp-iouring-khalefa-ow/tests/core.cpp vendor/sqlite/sqlite3.o \
  -lcrypto -lpthread -o bin/core-test
echo "Built $PWD/bin/server and bin/core-test"
