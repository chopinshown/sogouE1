#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
python3 build-deps.py
../.tools/zig-aarch64-macos-0.14.1/zig cc \
    -target aarch64-linux-gnu.2.26 -O2 -g -Wall -Wextra \
    -Ivendor -I../.tools/opus-1.5.2/include -I../.tools/curl-8.23.0/include \
    -I../.tools/libsamplerate-0.2.2/include \
    recorderd.c wifi.c xiaozhi.c vendor/cjson/cJSON.c sysroot/usr/lib/libasound.so.2 \
    build/deps/opus-1.5.2/libopus.a build/deps/libsamplerate-0.2.2/src/libsamplerate.a \
    build/deps/curl-8.23.0/lib/libcurl.a \
    build/deps/mbedtls-3.6.7/library/libmbedtls.a \
    build/deps/mbedtls-3.6.7/library/libmbedx509.a \
    build/deps/mbedtls-3.6.7/library/libmbedcrypto.a \
    -lpthread -ldl -lm -o build/recorderd
