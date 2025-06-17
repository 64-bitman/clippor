#!/bin/sh

trap 'kill "$(jobs -p)" 2> /dev/null; exit' EXIT

LOGFILE=$(mktemp)
LOGFILE2=$(mktemp)
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-runtime-dir-$$}"
mkdir -p "$XDG_RUNTIME_DIR"

# Start Sway in the background
WLR_BACKENDS=headless sway --debug >"$LOGFILE" 2>&1 &
WLR_BACKENDS=headless sway --debug >"$LOGFILE2" 2>&1 &

while true; do
    if grep -q "Starting backend on wayland display" "$LOGFILE"; then
        export WAYLAND_DISPLAY=$(sed -n "s/.*Starting backend on wayland display '\(.*\)'.*/\1/p" "$LOGFILE")
        break
    fi
done
while true; do
    if grep -q "Starting backend on wayland display" "$LOGFILE"; then
        export WAYLAND_DISPLAY2=$(sed -n "s/.*Starting backend on wayland display '\(.*\)'.*/\1/p" "$LOGFILE2")
        break
    fi
done

$@
