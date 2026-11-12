#!/bin/sh
set -eu
cd "$(dirname "$0")"
ROOT=/userdata/e1-recorder
mkdir -p "$ROOT" /mnt/sdcard/Recordings
export QT_QPA_FB_DRM=1
export QT_QPA_PLATFORM='linuxfb:fb=/dev/fb0:size=320x320:mmSize=320x320:offset=0x0:tty=/dev/ttyS4'
export QMLSCENE_DEVICE=softwarecontext
export QT_QUICK_BACKEND=software
export QT_QPA_EVDEV_KEYBOARD_PARAMETERS='/dev/input/event3:/dev/input/event1'
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS='/dev/input/event0:rotate=0'
export QT_QPA_FONTDIR=/usr/share/fonts
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins
export QML2_IMPORT_PATH=/usr/qml
BACKEND=
UI=
cleanup() {
    trap - EXIT
    if [ -n "$UI" ]; then kill -TERM "$UI" 2>/dev/null || true; wait "$UI" 2>/dev/null || true; fi
    if [ -n "$BACKEND" ]; then kill -TERM "$BACKEND" 2>/dev/null || true; wait "$BACKEND" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 0' TERM INT HUP
./recorderd /mnt/sdcard/Recordings >> "$ROOT/audio.log" 2>&1 &
BACKEND=$!
echo "$BACKEND" > "$ROOT/audio.pid"
/usr/bin/qmlscene Main.qml &
UI=$!
echo "$UI" > "$ROOT/ui.pid"
wait "$UI" || true
