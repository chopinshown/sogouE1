#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
../.tools/zig-aarch64-macos-0.14.1/zig cc \
    -target aarch64-linux-gnu.2.26 -O2 -g -Wall -Wextra \
    -Ivendor recorderd.c wifi.c sysroot/usr/lib/libasound.so.2 \
    -lpthread -ldl -lm -o build/recorderd
