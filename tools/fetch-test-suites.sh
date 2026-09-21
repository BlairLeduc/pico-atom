#!/bin/sh
# Fetch the 6502 test suites the host tests use (design.md §15.1).
#
# They are not committed: they are third-party artefacts and the tree
# ships no binaries it did not build. Without them test_m6502_functional
# reports as skipped rather than as passing.
#
#   ./tools/fetch-test-suites.sh [dir]        # default: test/suites
#   PICO_ATOM_TEST_ROMS=test/suites ctest --test-dir build/host
set -eu

dir="${1:-test/suites}"
mkdir -p "$dir"

base="https://github.com/Klaus2m5/6502_65C02_functional_tests/raw/master/bin_files"

echo "fetching 6502_functional_test.bin -> $dir"
curl -sSLf -o "$dir/6502_functional_test.bin" "$base/6502_functional_test.bin"

cat <<'NOTE'

Fetched: 6502_functional_test.bin
  64 KiB image, load #0000, start #0400, success trap at #3469.
  Built with disable_decimal = 0, so it exercises decimal ADC/SBC too.

Not fetched: 6502_decimal_test.bin
  Bruce Clark's decimal test is distributed as source (6502.org), not as
  a binary, so it needs an assembler. Until it is built here, decimal
  mode is covered by the functional test's decimal section plus the
  exhaustive valid-BCD checks in test/host/test_m6502_decimal.c. The
  gap is invalid-BCD operands, which is the part Clark's test exists for.
NOTE
