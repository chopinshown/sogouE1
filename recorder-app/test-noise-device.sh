#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
ZIG=../.tools/zig-aarch64-macos-0.14.1/zig
"$ZIG" cc -target aarch64-linux-gnu.2.26 -O2 -Wall -Wextra -Werror \
  noise_reduce.c tests/noise_unit.c -lm -o build/noise-unit-device
"$ZIG" cc -target aarch64-linux-gnu.2.26 -O2 -Wall -Wextra -Werror -Ivendor \
  tests/noise_record_unit.c wifi.c netaudio.c rtp_audio.c noise_reduce.c \
  sysroot/usr/lib/libasound.so.2 -lpthread -ldl -lm -o build/noise-record-unit
adb -s 15dacc90f0c2e787 push build/noise-unit-device /tmp/e1-noise-unit
adb -s 15dacc90f0c2e787 push build/noise-record-unit /tmp/e1-noise-record-unit
# This older ADB daemon does not propagate shell exit codes to the client.
adb -s 15dacc90f0c2e787 shell \
  '/tmp/e1-noise-unit && /tmp/e1-noise-record-unit; result=$?; echo E1_NOISE_EXIT=$result' \
  > build/noise-device-test.log
cat build/noise-device-test.log
tr -d '\r' < build/noise-device-test.log | grep -qx E1_NOISE_EXIT=0
