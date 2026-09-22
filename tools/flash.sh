#!/usr/bin/env bash
# flash.sh — program the firmware over SWD with a CMSIS-DAP Debug Probe
# (hardware-notes.md §2.7), verify it, and reset the target into it.
#
#   tools/flash.sh                     # build/pico/pico-atom.elf onto an RP2350
#   tools/flash.sh path/to/image.elf
#   tools/flash.sh --target rp2040 build/pico2040/pico-atom.elf
#
# The target must be powered for the debug port to answer: switch the
# PicoCalc on. 5 MHz SWD is the rate the hardware notes record as working;
# override with SWD_KHZ. OpenOCD is found on PATH, or under the Pico VS Code
# extension's ~/.pico-sdk/openocd/<version>/, or wherever OPENOCD points.
set -euo pipefail

target=rp2350
image=
while [ $# -gt 0 ]; do
    case "$1" in
        --target) target="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "flash.sh: unknown option $1" >&2; exit 2 ;;
        *) image="$1"; shift ;;
    esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
image="${image:-$root/build/pico/pico-atom.elf}"
if [ ! -f "$image" ]; then
    echo "flash.sh: no image at $image — build the firmware first (CLAUDE.md)" >&2
    exit 1
fi

# Find OpenOCD and the scripts directory that belongs to it. The Raspberry
# Pi fork is the one with target/rp2350.cfg; a stock 0.12 does not have it.
openocd="${OPENOCD:-}"
if [ -z "$openocd" ]; then
    if command -v openocd >/dev/null 2>&1; then
        openocd="$(command -v openocd)"
    else
        openocd="$(ls -d "$HOME"/.pico-sdk/openocd/*/openocd 2>/dev/null | sort -V | tail -n1 || true)"
    fi
fi
if [ -z "$openocd" ] || [ ! -x "$openocd" ]; then
    echo "flash.sh: OpenOCD not found; install the Raspberry Pi fork or set OPENOCD" >&2
    exit 1
fi

scripts=()
if [ -d "$(dirname "$openocd")/scripts" ]; then
    scripts=(-s "$(dirname "$openocd")/scripts")
fi

# Reset with both cores held, then release them together. OpenOCD's reset
# asserts SYSRESETREQ once per core, and on RP2350 that resets only the core
# that asks. A plain "reset run" restarts core 0 first; it boots and launches
# core 1 within milliseconds, and then OpenOCD resets core 1 back into the
# bootrom, where it waits for a launch that has already happened. The
# symptom is a firmware whose core 1 prints its first line and goes silent.
# A power-on reset does not do this; only the debugger's does.
echo "flash.sh: $(basename "$image") -> $target via $openocd"
exec "$openocd" "${scripts[@]}" \
    -f interface/cmsis-dap.cfg \
    -f "target/$target.cfg" \
    -c "adapter speed ${SWD_KHZ:-5000}" \
    -c "program \"$image\" verify" \
    -c "reset halt" \
    -c "resume" \
    -c "exit"
