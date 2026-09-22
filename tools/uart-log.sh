#!/usr/bin/env bash
# uart-log.sh — capture UART1 through the Debug Probe to a file
# (hardware-notes.md §2.7; design.md §12.3: write the numbers to a file).
#
#   tools/uart-log.sh 60 out/run.log      # 60 s, then stop
#   tools/uart-log.sh 0  out/run.log      # until Ctrl-C
#   UART_DEV=/dev/cu.usbmodem1234 tools/uart-log.sh 30 out/run.log
#
# 115200 8-N-1. The descriptor is held open while the baud rate is set and
# while reading, so the adapter cannot revert to its defaults in between.
# The device is discovered, not hard-coded: the single /dev/cu.usbmodem*
# (macOS) or /dev/ttyACM* (Linux) present, or UART_DEV.
set -euo pipefail

secs="${1:?usage: uart-log.sh SECONDS OUTFILE}"
out="${2:?usage: uart-log.sh SECONDS OUTFILE}"

dev="${UART_DEV:-}"
if [ -z "$dev" ]; then
    shopt -s nullglob
    cands=(/dev/cu.usbmodem* /dev/ttyACM*)
    if [ ${#cands[@]} -ne 1 ]; then
        echo "uart-log.sh: found ${#cands[@]} candidate ports (${cands[*]:-none}); set UART_DEV" >&2
        exit 1
    fi
    dev="${cands[0]}"
fi

# Two readers on one port split the byte stream between them and both logs
# come out scrambled, so refuse rather than race a leftover capture.
if command -v lsof >/dev/null 2>&1 && lsof "$dev" >/dev/null 2>&1; then
    echo "uart-log.sh: $dev is already open:" >&2
    lsof "$dev" >&2
    exit 1
fi

mkdir -p "$(dirname "$out")"
exec 3<"$dev"
if [ "$(uname)" = Darwin ]; then
    stty -f "$dev" 115200 raw -echo cs8 -cstopb -parenb clocal
else
    stty -F "$dev" 115200 raw -echo cs8 -cstopb -parenb clocal
fi

cat <&3 >"$out" &
reader=$!
trap 'kill "$reader" 2>/dev/null || true; wait "$reader" 2>/dev/null || true; exec 3<&-' EXIT INT TERM

echo "uart-log.sh: $dev -> $out (${secs}s; 0 = until interrupted)" >&2
if [ "$secs" -gt 0 ]; then
    sleep "$secs"
else
    wait "$reader"
fi
