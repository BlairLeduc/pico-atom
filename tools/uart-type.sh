#!/usr/bin/env bash
# uart-type.sh — type at the guest over UART1 (design.md §12.3).
#
#   tools/uart-type.sh 'PRINT 2+2\r'
#   tools/uart-type.sh $'10 P.$7;\r20 G.10\rRUN\r'
#
# The firmware turns each received character into the PicoCalc key events
# for it (PICO_ATOM_UART_KEYS in src/port/main.c), so a hardware run can be
# driven from the machine capturing it. Upper case types Atom capitals,
# lower case types Atom lower case, and ^A-^Z arrive as CTRL chords —
# except ^J and ^M, which are LF and CR and type RETURN. \x1e is not a
# key: it plays or stops the tape in the deck, as the menu does, and a
# build with PICO_ATOM_BOOT_TAPE puts one there at boot.
#
# One character every 0.25 s: the MOS takes about eight fields a key
# (config.h, ATOM_KEY_MIN_FIELDS + ATOM_KEY_GAP_FIELDS), and the firmware
# only reads the UART while the key queue has room, so faster than that
# fills the 32-byte UART FIFO and characters are lost.
#
# Writing while uart-log.sh reads is fine: that script refuses a second
# *reader*; this only writes. The port's settings are the ones uart-log.sh
# applied, so start the capture first.
set -euo pipefail

text="${1:?usage: uart-type.sh TEXT}"
delay="${UART_TYPE_DELAY:-0.25}"

dev="${UART_DEV:-}"
if [ -z "$dev" ]; then
    shopt -s nullglob
    cands=(/dev/cu.usbmodem* /dev/ttyACM*)
    if [ ${#cands[@]} -ne 1 ]; then
        echo "uart-type.sh: found ${#cands[@]} candidate ports (${cands[*]:-none}); set UART_DEV" >&2
        exit 1
    fi
    dev="${cands[0]}"
fi

# printf interprets \r and \n in TEXT, so a caller can write either.
text="$(printf '%b' "$text"; printf x)"
text="${text%x}"

exec 4>"$dev"
for ((i = 0; i < ${#text}; i++)); do
    printf '%s' "${text:$i:1}" >&4
    sleep "$delay"
done
exec 4>&-
