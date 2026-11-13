#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
# Native host test: requires a C compiler, pkg-config and the host Opus library.
cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    $(pkg-config --cflags opus) speech_decimator.c tests/speech_unit.c \
    $(pkg-config --libs opus) -lm -o build/speech-unit-host
./build/speech-unit-host
