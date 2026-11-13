#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  noise_reduce.c tests/noise_unit.c -lm -o build/noise-unit
./build/noise-unit
