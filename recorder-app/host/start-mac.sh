#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -lt 1 ]; then
    echo "Usage: sh $0 E1_WIFI_IP [bridge options]" >&2
    exit 2
fi
mkdir -p "$ROOT/build"
cc -O2 -Wall -Wextra -Werror "$ROOT/host/mac_audio_setup.c" \
    -framework CoreAudio -framework CoreFoundation -o "$ROOT/build/mac-audio-setup"
"$ROOT/build/mac-audio-setup" --configure
PYTHON="$ROOT/../.venv/bin/python"
if [ ! -x "$PYTHON" ]; then PYTHON=python3; fi
exec "$PYTHON" "$ROOT/host/e1_audio.py" --buffer-ms 15 "$@" \
    --receive-device 'BlackHole 2ch' --send-device 'BlackHole 16ch'
