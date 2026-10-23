#!/bin/sh

if [ "$1" = "--attach" ]; then
    sock="$2"
    printf '\033]0;Bleed kernel log\007'

    i=0
    while [ ! -S "$sock" ] && [ $i -lt 300 ]; do
        sleep 0.1
        i=$((i + 1))
    done

    if command -v socat >/dev/null 2>&1; then
        socat -u UNIX-CONNECT:"$sock" STDOUT
    else
        nc -d -U "$sock"
    fi

    exit 0
fi

sock="$1"
self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cmd="exec sh '$self' --attach '$sock'"

case "$(uname -s)" in
    Darwin)
        win=$(osascript -e 'on run argv' \
                        -e 'tell application "Terminal"' \
                        -e 'do script (item 1 of argv)' \
                        -e 'activate' \
                        -e 'return id of front window' \
                        -e 'end tell' \
                        -e 'end run' "$cmd") || exit 1

        # terminal.app keeps the window once its process ends, so shut it from out here when that happens
        nohup osascript -e 'on run argv' \
                        -e 'set wid to (item 1 of argv) as integer' \
                        -e 'tell application "Terminal"' \
                        -e 'delay 1' \
                        -e 'try' \
                        -e 'repeat while (count processes of tab 1 of window id wid) > 0' \
                        -e 'delay 0.5' \
                        -e 'end repeat' \
                        -e 'close window id wid' \
                        -e 'end try' \
                        -e 'end tell' \
                        -e 'end run' "$win" >/dev/null 2>&1 &
        ;;
    Linux)
        [ -n "$DISPLAY$WAYLAND_DISPLAY" ] || exit 1
        for t in "$TERMINAL" x-terminal-emulator gnome-terminal konsole xfce4-terminal kitty alacritty xterm; do
            [ -n "$t" ] && command -v "$t" >/dev/null 2>&1 || continue
            case "$t" in
                gnome-terminal) "$t" -- sh -c "$cmd" ;;
                xfce4-terminal) "$t" -x sh -c "$cmd" & ;;
                kitty)          "$t" sh -c "$cmd" & ;;
                *)              "$t" -e sh -c "$cmd" & ;;
            esac
            exit 0
        done
        exit 1
        ;;
    *)
        exit 1
        ;;
esac
