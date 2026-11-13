#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
ZIG=../.tools/zig-aarch64-macos-0.14.1/zig
python3 build-deps.py
"$ZIG" cc -target aarch64-linux-gnu.2.26 -O2 -Wall -Wextra -Werror \
  noise_reduce.c tests/noise_unit.c -lm -o build/noise-unit-device
"$ZIG" cc -target aarch64-linux-gnu.2.26 -O2 -Wall -Wextra -Werror \
  -Ivendor -I../.tools/opus-1.5.2/include -I../.tools/curl-8.23.0/include \
  -I../.tools/libsamplerate-0.2.2/include \
  tests/noise_record_unit.c wifi.c netaudio.c rtp_audio.c noise_reduce.c \
  speech_decimator.c xiaozhi.c vendor/cjson/cJSON.c sysroot/usr/lib/libasound.so.2 \
  build/deps/opus-1.5.2/libopus.a build/deps/libsamplerate-0.2.2/src/libsamplerate.a \
  build/deps/curl-8.23.0/lib/libcurl.a \
  build/deps/mbedtls-3.6.7/library/libmbedtls.a \
  build/deps/mbedtls-3.6.7/library/libmbedx509.a \
  build/deps/mbedtls-3.6.7/library/libmbedcrypto.a \
  -lpthread -ldl -lm -o build/noise-record-unit
adb -s 15dacc90f0c2e787 push build/noise-unit-device /tmp/e1-noise-unit
adb -s 15dacc90f0c2e787 push build/noise-record-unit /tmp/e1-noise-record-unit
# This older ADB daemon does not propagate shell exit codes to the client.
adb -s 15dacc90f0c2e787 shell \
  '/tmp/e1-noise-unit && /tmp/e1-noise-record-unit; result=$?; echo E1_NOISE_EXIT=$result' \
  > build/noise-device-test.log
cat build/noise-device-test.log
tr -d '\r' < build/noise-device-test.log | grep -qx E1_NOISE_EXIT=0
