#!/bin/sh
# Writable application override. Factory executable and firmware stay intact.
HELLO_ROOT=/userdata/e1-hello
if [ -f "$HELLO_ROOT/disabled" ]; then
    exec /usr/bin/SogouE1
fi
VERSION=$(cat "$HELLO_ROOT/current.version" 2>/dev/null)
case "$VERSION" in
    ''|*[!0-9.]*|.*) exec /usr/bin/SogouE1 ;;
esac
APP="$HELLO_ROOT/releases/$VERSION/run.sh"
if [ ! -x "$APP" ]; then
    exec /usr/bin/SogouE1
fi
echo $$ > "$HELLO_ROOT/launcher.pid"
CHILD=
stop_hello() {
    if [ -n "$CHILD" ]; then
        kill -TERM "$CHILD" 2>/dev/null
        wait "$CHILD" 2>/dev/null
    fi
    exit 0
}
trap stop_hello TERM INT
"$APP" >> "$HELLO_ROOT/runtime.log" 2>&1 &
CHILD=$!
echo "$CHILD" > "$HELLO_ROOT/ui.pid"
wait "$CHILD"
RESULT=$?
echo "E1_HELLO_EXIT $VERSION $RESULT" >> "$HELLO_ROOT/runtime.log"
# A normal Back action or failed UI restores the factory UI for later boots.
touch "$HELLO_ROOT/disabled"
exec /usr/bin/SogouE1
