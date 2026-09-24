#!/usr/bin/env bash
# build.sh — build the firmware from the current source (CLAUDE.md), and
# say where the .uf2 is.
#
#   tools/build.sh                         # build/pico/pico-atom.uf2
#   tools/build.sh -DPICO_ATOM_BOOT_DISC=/atom/discs/x.ssd build/pico-disc
#
# Arguments starting -D are passed to CMake's configure step; another
# argument names the build directory, which is configured first if it has
# never been. Build each such variant in its own directory
# (hardware-notes.md §9.8). The Pico SDK and the Arm toolchain are the
# newest under the Pico VS Code extension's ~/.pico-sdk/, unless
# PICO_SDK_PATH names one that exists: that variable is often left
# pointing at a version since removed.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dir=
defs=()
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -D*) defs+=("$1"); shift ;;
        -*) echo "build.sh: unknown option $1" >&2; exit 2 ;;
        *) dir="$1"; shift ;;
    esac
done
dir="${dir:-$root/build/pico}"

newest() { ls -d "$1"/*/ 2>/dev/null | sort -V | tail -n1 | sed 's:/$::'; }

if [ -z "${PICO_SDK_PATH:-}" ] || [ ! -d "$PICO_SDK_PATH" ]; then
    PICO_SDK_PATH="$(newest "$HOME/.pico-sdk/sdk")"
fi
if [ -z "$PICO_SDK_PATH" ]; then
    echo "build.sh: no Pico SDK; install one or set PICO_SDK_PATH" >&2
    exit 1
fi
export PICO_SDK_PATH

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    tc="$(newest "$HOME/.pico-sdk/toolchain")"
    if [ -z "$tc" ] || [ ! -x "$tc/bin/arm-none-eabi-gcc" ]; then
        echo "build.sh: arm-none-eabi-gcc not found; put it on PATH" >&2
        exit 1
    fi
    export PATH="$tc/bin:$PATH"
fi
toolchain="$(dirname "$(dirname "$(command -v arm-none-eabi-gcc)")")"

echo "build.sh: $PICO_SDK_PATH, $(basename "$toolchain") -> $dir"
if [ ! -f "$dir/CMakeCache.txt" ] || [ ${#defs[@]} -gt 0 ]; then
    cmake -S "$root" -B "$dir" -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release \
        -DPICO_TOOLCHAIN_PATH="$toolchain" ${defs[@]+"${defs[@]}"}
fi
cmake --build "$dir" -j

arm-none-eabi-size "$dir/pico-atom.elf"
echo "build.sh: $dir/pico-atom.uf2"
