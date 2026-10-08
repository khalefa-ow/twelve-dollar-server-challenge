#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ "$(uname -s)" != Linux ]; then
  echo 'The io_uring server requires Linux 6.0+ (Ubuntu 24.04). Core tests also build on macOS.' >&2
  exit 1
fi
mkdir -p vendor/liburing bin
ARCHIVE=vendor/liburing-2.6.tar.gz
SHA256=682f06733e6db6402c1f904cbbe12b94942a49effc872c9e01db3d7b180917cc
if ! [ -f "$ARCHIVE" ] || ! printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum -c --status; then
  curl --fail --location --retry 3 --connect-timeout 20 --max-time 180 -o "$ARCHIVE" \
    https://codeload.github.com/axboe/liburing/tar.gz/refs/tags/liburing-2.6
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum -c --quiet
tar -xzf "$ARCHIVE" -C vendor/liburing --strip-components=1
(
  cd vendor/liburing
  ./configure --cc=gcc --cxx=g++
  make -C src -j2
)
OPT=(-O3 -march=native -flto=auto -fno-plt)
g++ "${OPT[@]}" -std=c++17 -Wall -Wextra -Wno-deprecated-declarations \
  -Ivendor/liburing/src/include src/server.cpp vendor/liburing/src/liburing.a -lcrypto -o bin/server
g++ "${OPT[@]}" -std=c++17 -Wall -Wextra -Wno-deprecated-declarations \
  tests/core.cpp -lcrypto -o bin/core-test
g++ "${OPT[@]}" -std=c++17 -Wall -Wextra -Wno-deprecated-declarations \
  tests/wal.cpp -lcrypto -o bin/wal-test
echo "Built $PWD/bin/server, bin/core-test and bin/wal-test (liburing + libcrypto only)"
