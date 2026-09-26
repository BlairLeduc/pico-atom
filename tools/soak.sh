#!/usr/bin/env bash
# soak.sh — §15.3's soak (design.md): 30 minutes on battery with a BASIC
# program driving the display, the sound and the keyboard, UART1 captured
# throughout, then soak-check.py over the log.
#
#   tools/soak.sh build/pico/pico-atom.elf            # 30 minutes
#   tools/soak.sh build/pico/pico-atom.elf 45 out/soak
#
# Before running: the Debug Probe's SWD and UART connected, the USB-C
# power lead out so the PicoCalc runs on its batteries, and the power
# switch on. Nothing here can tell battery from USB; that is the
# operator's part, and soak-check.py asks for it to be recorded.
#
# The program alternates a 256x192 graphics page of random lines with a
# page of scrolling text, and rings the bell between them. It reads the
# keyboard matrix itself (column 9: SPACE 4 . H R), so it never waits on
# a key, while this script types H and 4 at it over the UART every few
# seconds. Not R: BASIC's Escape test (#C504) reads port B bit 5 in
# whatever column is selected, and bit 5 of column 9 is R. The program
# puts column 0 back on the line it reads column 9, for the same reason;
# do not press R during a run either. The typed keys go through the
# firmware's key path (keymatrix), and the program shows the row it
# read. The southbridge is polled for the real keyboard all the while,
# which is what the I2C error count covers; press a few keys on it during
# the run as well.
set -euo pipefail

elf="${1:?usage: soak.sh ELF [MINUTES] [OUTDIR]}"
minutes="${2:-30}"
outdir="${3:-out/soak}"
here="$(cd "$(dirname "$0")" && pwd)"
every="${SOAK_KEY_EVERY:-5}"     # seconds between typed keys

mkdir -p "$outdir"
stamp="$(date +%Y%m%d-%H%M%S)"
log="$outdir/soak-$stamp.log"

# Atom BASIC; upper case types Atom capitals (uart-type.sh). Lines are
# kept under the MOS's line length. Checked on the host first, with
# tools/trace-diff.py and against Atomulator.
program=$'10 N=0\r'\
$'20 CLEAR 4;FOR I=1 TO 60\r'\
$'30 X=ABSRND%256;Y=ABSRND%192;DRAW X,Y\r'\
$'40 NEXT I;PRINT $7;CLEAR 0;FOR I=1 TO 40\r'\
$'50 ?#B000=?#B000&#F0|9;K=?#B001;?#B000=?#B000&#F0\r'\
$'60 PRINT "SOAK ",N," KEY ",K\';NEXT I\r'\
$'70 PRINT $7;N=N+1;GOTO 20\r'\
$'RUN\r'

logger=
stop_logger() {
    [ -n "$logger" ] || return 0
    kill "$logger" 2>/dev/null || true
    wait "$logger" 2>/dev/null || true
    logger=
}
trap stop_logger EXIT

# Capture first, so the banner is in the log (CLAUDE.md).
"$here/uart-log.sh" 0 "$log" &
logger=$!
for _ in $(seq 1 20); do
    sleep 0.5
    [ -e "$log" ] && break
    kill -0 "$logger" 2>/dev/null || { echo "soak.sh: the capture did not start" >&2; exit 1; }
done

"$here/flash.sh" "$elf" >"$outdir/soak-$stamp.flash.log" 2>&1
sleep 6                          # boot, card mount, ROMs, the first prompt
"$here/uart-type.sh" "$program"
start=$(date +%s)
echo "soak.sh: running $minutes minutes from $(date +%H:%M:%S) -> $log"

end=$((start + minutes * 60))
n=0
while [ "$(date +%s)" -lt "$end" ]; do
    sleep "$every"
    if [ $((n % 2)) -eq 0 ]; then "$here/uart-type.sh" 'H'; else "$here/uart-type.sh" '4'; fi
    n=$((n + 1))
done
sleep 10                         # a last heartbeat after the last key
stop_logger

echo "soak.sh: $n keys typed"
"$here/soak-check.py" --minutes "$minutes" "$log" | tee "$outdir/soak-$stamp.txt"
