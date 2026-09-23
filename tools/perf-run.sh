#!/usr/bin/env bash
# perf-run.sh — the M7 measurement run (design.md §6.3, §17).
#
#   tools/perf-run.sh out/m7/tier2.elf out/m7/tier2-a       # every workload
#   tools/perf-run.sh out/m7/tier2.elf out/m7/tier2-a idle compute
#
# Each workload is its own boot: flash, capture UART1, type a BASIC
# program at the guest over the same UART, let it run, keep the log. The
# heartbeat's `perf` line is the measurement; perf-summary.sh reduces the
# logs. The board must have the ROMs on its card.
#
# Measured in the mode that ships (hardware-notes.md §9.1): audio paces
# the guest and core 1 presents while it runs. The audio consumed rate
# on the heartbeat's `audio` line is the control quantity.
set -euo pipefail

elf="${1:?usage: perf-run.sh ELF OUTDIR [WORKLOAD...]}"
outdir="${2:?usage: perf-run.sh ELF OUTDIR [WORKLOAD...]}"
shift 2
workloads=("$@")
[ ${#workloads[@]} -gt 0 ] || workloads=(idle compute scroll bell)

here="$(cd "$(dirname "$0")" && pwd)"
dwell="${PERF_DWELL:-40}"   # seconds each program runs: ~8 heartbeats

# Atom BASIC; upper case types Atom capitals (uart-type.sh).
program() {
    case "$1" in
        idle)    printf '' ;;   # the MOS scanning the keyboard at the prompt
        compute) printf '10 A=0;FOR I=1 TO 30000;A=A+I*3/7;NEXT I;GOTO 10\rRUN\r' ;;
        scroll)  printf '10 PRINT "THE QUICK BROWN FOX 0123456789";GOTO 10\rRUN\r' ;;
        bell)    printf '10 PRINT $7;GOTO 10\rRUN\r' ;;
        *) echo "perf-run.sh: unknown workload $1" >&2; exit 2 ;;
    esac
}

mkdir -p "$outdir"

# A failed flash or type exits under set -e; the capture must not outlive
# it, or it holds the port and every later run finds it busy.
logger=
stop_logger() {
    [ -n "$logger" ] || return 0
    kill "$logger" 2>/dev/null || true
    wait "$logger" 2>/dev/null || true
    logger=
}
trap stop_logger EXIT

for w in "${workloads[@]}"; do
    text="$(program "$w")"
    log="$outdir/$w.log"
    # The last capture's reader can outlive its kill by a moment, and
    # uart-log.sh refuses a busy port; flashing before the new capture is
    # running loses the banner and the first heartbeats.
    rm -f "$log"
    for try in 1 2 3 4 5 6 7 8 9 10; do
        "$here/uart-log.sh" 0 "$log" 2>/dev/null &
        logger=$!
        # Up to 5 s for the file: it can take longer than a second to
        # appear, and a capture that is merely slow must not be waited
        # on — a running one never exits.
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            sleep 0.5
            [ -e "$log" ] && break
            kill -0 "$logger" 2>/dev/null || break
        done
        if kill -0 "$logger" 2>/dev/null && [ -e "$log" ]; then break; fi
        kill "$logger" 2>/dev/null || true
        wait "$logger" 2>/dev/null || true
        logger=
        [ "$try" -lt 10 ] || { echo "perf-run.sh: no capture for $w" >&2; exit 1; }
    done
    "$here/flash.sh" "$elf" >"$outdir/$w.flash.log" 2>&1
    sleep 6                      # boot, card mount, ROMs, the first prompt
    [ -n "$text" ] && "$here/uart-type.sh" "$text"
    # Mark where the program started: heartbeats before this are typing.
    grep -c 'perf ' "$log" >"$outdir/$w.start" || true
    sleep "$dwell"
    stop_logger
    echo "perf-run.sh: $w -> $log"
done
