#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    rtp_audio.c tests/rtp_unit.c -lm -o build/rtp-unit
./build/rtp-unit
"${PYTHON:-python3}" -m unittest discover -s tests -p 'test_rtp.py' -v
