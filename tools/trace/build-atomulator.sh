#!/bin/sh
# Build atomulator-trace, the reference half of the trace-diff harness
# (design.md §15.1), from an Atomulator checkout. Nothing of Atomulator is
# copied into this tree: its CPU, 8255, VIA and 8271 are compiled where
# they stand, against tools/trace/allegro/allegro.h, and linked with the
# driver.
#
#   tools/trace/build-atomulator.sh ATOMULATOR_DIR [OUT]   # OUT: out/trace
set -eu

src="${1:?usage: $0 ATOMULATOR_DIR [OUT]}/src"
out="${2:-out/trace}"
here="$(cd "$(dirname "$0")" && pwd)"
cc="${CC:-cc}"

[ -f "$src/6502.c" ] || { echo "$src/6502.c: not an Atomulator checkout" >&2; exit 1; }
mkdir -p "$out/obj"

# Atomulator is old C: implicit ints and the like. Its warnings are not
# ours to fix, so they are off for its files and on for the driver.
for f in 6502 8255 6522via 8271; do
    "$cc" -O2 -w -I "$here/allegro" -I "$src" -c "$src/$f.c" -o "$out/obj/$f.o"
done
"$cc" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-strict-prototypes \
    -I "$here/allegro" -I "$src" -I "$here" \
    -c "$here/atomulator-trace.c" -o "$out/obj/atomulator-trace.o"
"$cc" -o "$out/atomulator-trace" "$out"/obj/*.o
echo "$out/atomulator-trace"
