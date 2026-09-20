# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

An **Acorn Atom emulator for the ClockworkPi PicoCalc**, in C against the
Raspberry Pi Pico SDK.

**The repository is currently documentation only.** There is no source tree, no
`CMakeLists.txt` and no build. Do not invent build, lint or test commands — none
exist yet. The first implementation milestone (M0 in `docs/design.md` §17) is
what creates them.

## The two documents

| File | Authority on |
|---|---|
| `docs/hardware-notes.md` | The **host** — PicoCalc wiring, peripheral protocols, timing, measured costs, observed quirks |
| `docs/design.md` | The **guest** and the shape of the code — Atom hardware model, architecture, memory budget, milestones |

`docs/design.md` uses `§N` to reference `hardware-notes.md` unless it says
otherwise. Preserve that convention when editing; cross-references between the
two documents are load-bearing and there are many of them.

Address notation: **`#XXXX` is a guest (Atom) address, `0x` is a host value.**
The design document is deliberate about this because it discusses two machines
at once.

## Planned build (does not exist yet)

`docs/design.md` §14 specifies one CMake source tree producing two targets: the
UF2, and a host test binary that compiles `src/core/` with the system compiler
and no Pico SDK, gated behind a `PICO_ATOM_HOST` option and run under CTest.
Both toolchains build with `-Wall -Wextra -Werror`. When implementing M0, match
that specification rather than reinventing it.

## Architecture: the parts that are easy to violate

Read `docs/design.md` before writing emulator code. These are the decisions that
a plausible-looking change silently breaks:

- **`src/core/` must not depend on the Pico SDK.** The 6502, bus, 8255, MC6847,
  tape and keymap are portable C with no dynamic allocation, validated on a
  workstation against the Klaus Dormann and Bruce Clark 6502 suites before any
  hardware exists. The core building clean on both toolchains is the mechanism
  that keeps SDK dependencies from leaking down. `src/port/` is the only place
  SDK headers belong.
- **The SPI wire is the bottleneck, not the 6502.** A 1 MHz Atom costs an
  estimated 8–17% of one core; a full-screen redraw is ~12.3 ms against a
  16.7 ms field. Optimisation effort belongs on pixels transmitted, not on the
  interpreter.
- **There is no decoded framebuffer, by design.** The MC6847 image is a pure
  function of 6 KiB of guest VRAM plus five mode bits, so bands are generated
  from a snapshot through a mode LUT straight into DMA line buffers. Adding a
  framebuffer would reintroduce the second copy of the screen this design exists
  to avoid.
- **Core 0 owns the 6502, 8255 and audio; core 1 owns the LCD, I²C and SD.**
  Handoff is an immutable snapshot with explicit ownership — core 1 never reads
  guest RAM while the 6502 runs.
- **Fixed capacities live in one header** (`src/core/config.h`). SRAM is the
  scarce resource; the budget in §5 is only a link-time fact if capacities stay
  in one place.

## Hardware invariants that are not negotiable

These come from `hardware-notes.md` §10 and each one cost someone real debugging
time. They apply to every driver in `src/port/`:

- `spi_set_format()` and the `D/CX` write go **before** CS low (the 40 ns
  CS-high rule). Getting this wrong looks like a wiring or clock fault.
- **Never touch the LCD from an interrupt handler.** Timers set flags; drawing
  happens in thread context.
- **Never mask interrupts around a blit.** Audio has a ~3.5 ms refill deadline.
- Audio DMA ring: power-of-two **and aligned**, with the hardware read wrap. On
  re-arm from the IRQ, reset **both** read address and transfer count.
- `DMA_IRQ_0` at priority `0x40`; every function on the refill path behind
  `__not_in_flash_func`.
- Drain the SPI RX FIFO and clear the overrun flag after every DMA blit; restore
  8-bit format.
- **Re-apply the SPI baud rate and the audio carrier after any `clk_sys`
  change.** `clk_peri` does not follow `clk_sys` by default.
- Poll the keyboard from the frame loop, never a timer IRQ; build held-key state
  from press/release events, not from a character stream.
- Count PCM producer underruns **separately** from late DMA refills — one
  counter cannot substitute for the other.
- Prefer `float` over `double`; a stray `2.0` literal promotes a whole
  expression and RP2040 has no FPU at all.

## Two things to be careful about

- **`docs/design.md` §16 is a list of unverified constants.** The keyboard
  matrix cells and the MOS `OSLOAD`/`OSSAVE` vectors are marked low confidence
  and were written from secondary knowledge. Transcribe them from primary
  sources before they become `#define`s; do not treat the document as
  authoritative for them.
- **No ROM binaries in the tree.** Acorn's ROMs are copyrighted; the user
  supplies them on SD card under `/atom/roms/`.

## Measurement discipline

Every performance figure in `docs/design.md` is currently an arithmetic estimate
derived from `hardware-notes.md`, not a measurement, and is labelled as such.
When replacing one with a real number (`hardware-notes.md` §9.1): profile in the
mode you ship, carry a control quantity that should not change, expect ~2 %
run-to-run spread, compare on one board, and write results to a file — a number
on a 320×320 panel cannot be copied off it.
