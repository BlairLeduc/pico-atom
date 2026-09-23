#!/usr/bin/env bash
# perf-summary.sh — reduce perf-run.sh's logs to one line per workload.
#
#   tools/perf-summary.sh out/m7/tier0-a out/m7/tier1-a ...
#
# Averages the `perf` heartbeats taken after the program was typed
# (perf-run.sh records where that was), dropping the first, which
# straddles the RUN. The audio consumed rate is the control: it is the
# PWM wrap, and it must read the same in every row.
set -euo pipefail

# The capture opens with line noise from the reset, which is not UTF-8.
export LC_ALL=C

for dir in "$@"; do
    for log in "$dir"/*.log; do
        case "$log" in *.flash.log) continue ;; esac
        w="$(basename "$log" .log)"
        start="$(cat "$dir/$w.start" 2>/dev/null || echo 0)"
        awk -v dir="$dir" -v w="$w" -v start="$start" '
            / perf / {
                n++
                if (n <= start + 1) next
                match($0, /guest [0-9.]+%/);        g = substr($0, RSTART + 6, RLENGTH - 7)
                match($0, /headroom [0-9.]+x/);     h = substr($0, RSTART + 9, RLENGTH - 10)
                match($0, /[0-9.]+ host cycles/);   c = substr($0, RSTART, RLENGTH - 12)
                match($0, /[0-9.]+ guest cycles/);  i = substr($0, RSTART, RLENGTH - 13)
                sg += g; sh += h; sc += c; si += i; k++
                if (k == 1 || c < cmin) cmin = c
                if (k == 1 || c > cmax) cmax = c
            }
            / audio / && n > start + 1 {
                match($0, /[0-9]+ Hz consumed/); r = substr($0, RSTART, RLENGTH - 12)
                sr += r; kr++
            }
            END {
                if (!k) { printf "%-24s %-8s no steady heartbeats\n", dir, w; exit }
                printf "%-24s %-8s n=%d  guest %5.1f%%  headroom %5.2fx  host cyc/insn %5.1f (%.1f-%.1f)  guest cyc/insn %.2f  audio %d Hz\n",
                       dir, w, k, sg / k, sh / k, sc / k, cmin, cmax, si / k, kr ? sr / kr : 0
            }' "$log"
    done
done
