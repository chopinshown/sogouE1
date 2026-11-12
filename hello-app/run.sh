#!/bin/sh
set -eu
cd "$(dirname "$0")"
export QT_QPA_FB_DRM=1
export QT_QPA_PLATFORM='linuxfb:fb=/dev/fb0:size=320x320:mmSize=320x320:offset=0x0:tty=/dev/ttyS4'
export QMLSCENE_DEVICE=softwarecontext
export QT_QUICK_BACKEND=software
export QT_QPA_EVDEV_KEYBOARD_PARAMETERS='/dev/input/event3:/dev/input/event1'
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS='/dev/input/event0:rotate=0'
export QT_QPA_FONTDIR=/usr/share/fonts
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins
export QML2_IMPORT_PATH=/usr/qml
exec /usr/bin/qmlscene Main.qml
