#!/bin/sh
# Fetch and build the 6502 test suites the host tests use (design.md §15.1).
#
# The binaries are not committed: the tree ships no binaries it did not
# build. Without them test_m6502_functional reports as skipped rather
# than as passing.
#
#   ./tools/fetch-test-suites.sh [dir]        # default: test/suites
#   PICO_ATOM_TEST_ROMS=test/suites ctest --test-dir build/host
set -eu

dir="${1:-test/suites}"
mkdir -p "$dir"

base="https://github.com/Klaus2m5/6502_65C02_functional_tests/raw/master/bin_files"

echo "fetching 6502_functional_test.bin -> $dir"
curl -sSLf -o "$dir/6502_functional_test.bin" "$base/6502_functional_test.bin"

# Bruce Clark's decimal test is public domain and distributed as source,
# so its source is in the tree (test/asm/) and only the binary is built.
src="$(dirname "$0")/../test/asm/6502_decimal_test.s"
if command -v ca65 >/dev/null 2>&1 && command -v ld65 >/dev/null 2>&1; then
    echo "assembling 6502_decimal_test.bin -> $dir"
    obj="$dir/6502_decimal_test.o"
    ca65 -o "$obj" "$src"
    ld65 -t none -o "$dir/6502_decimal_test.bin" "$obj"
    rm -f "$obj"
    decimal="Built:   6502_decimal_test.bin
  load and start #0200, result in #000B (0 = pass). Every flag checked,
  invalid BCD operands included."
else
    decimal="Not built: 6502_decimal_test.bin
  ca65 and ld65 (cc65) are not on PATH. Install cc65 and run this again;
  until then test_m6502_functional runs only Dormann's suite, and invalid
  BCD operands are untested."
fi

cat <<NOTE

Fetched: 6502_functional_test.bin
  64 KiB image, load #0000, start #0400, success trap at #3469.
  Built with disable_decimal = 0, so it exercises decimal ADC/SBC too.

$decimal
NOTE
