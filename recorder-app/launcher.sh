#!/bin/sh
# E1_RECORDER_MANAGED_LAUNCHER v1
ROOT=/userdata/e1-recorder
if [ -f "$ROOT/disabled" ]; then exec /usr/bin/SogouE1; fi
RELEASE=$(cat "$ROOT/current.version" 2>/dev/null)
case "$RELEASE" in ''|*[!0-9a-f.-]*|.*) exec /usr/bin/SogouE1;; esac
APP="$ROOT/releases/$RELEASE/run.sh"
if [ ! -x "$APP" ]; then exec /usr/bin/SogouE1; fi
echo $$ > "$ROOT/launcher.pid"
CHILD=
stop_app() {
    if [ -n "$CHILD" ]; then kill -TERM "$CHILD" 2>/dev/null; wait "$CHILD" 2>/dev/null; fi
    exit 0
}
trap stop_app TERM INT
"$APP" >> "$ROOT/ui.log" 2>&1 &
CHILD=$!
wait "$CHILD"
touch "$ROOT/disabled"
exec /usr/bin/SogouE1
