# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

An **Acorn Atom emulator for the ClockworkPi PicoCalc**, in C against the
Raspberry Pi Pico SDK.

**Implementation status: M0–M5 are done** (`docs/design.md` §17).

What exists: the two-target build, `src/core/config.h`, the page-table bus, a
6502 that passes Klaus Dormann's functional test, the 8255 PPI wired to the
keyboard matrix and field sync, MC6847 mode decode, expansion LUT and row
generation for all nine modes, the character ROM, and §15.1's golden images in
`test/golden/`, checked by eye before they were committed.

M3's code exists: the flyback-split field loop (`atom_run_field`), the §4.2
snapshot pool, the §8.4 dirty-band presenter on core 1, and drivers for the
southbridge and LCD. Verified on a Plus 2 W on 2026-09-22: the test pattern's
four corners, colour order and orientation were checked by eye on the panel;
present times are measured and recorded in design.md §8.4 (full redraw
11.5 ms, wire-bound); the southbridge answers with zero I²C errors.

M4 is done: the Atom boots on the board. Verified on a Plus 2 W on
2026-09-22: ROMs load off SD with matching SHA-1s, `PRINT 2+2` typed on the
PicoCalc keyboard answers on the panel, the cursor shows, and `CLEAR 0` +
`PLOT` draws an SG6 element. On the host, `test_boot` runs the real MOS
through the southbridge event path. The keyboard matrix and the VRAM byte
wiring (bit 7 `INV`, bit 6 SG6) were both settled by executing the ROMs
(§16). The 6522 VIA exists and is fitted by default, because the MOS hangs
before its first prompt without it. After `CLEAR 0` the cursor is
invisible over graphics cells; that follows from the wiring, not a bug.

M5 is done: the Atom has sound. PC2 is integrated into PCM by a box filter at
the exact rational sample period (`beeper.c`, design.md §9.3), and core 0
paces the guest on the PCM queue in place of the microsecond timer (§12.2).
On the host, `test_audio` checks every sample against an independent model of
a guest-executed square wave, and measures its pitch. `test_boot` rings the
real MOS bell: 387.64 Hz measured, against 387.60 Hz from the cycle count of
the loop at `#FD18`. On a Plus 2 W on 2026-09-22 a BASIC loop rang the bell
continuously for 10 minutes. Underruns and late refills stayed at zero, and
the consumed sample rate measured against the microsecond timer held at
36,620–36,621 Hz. By ear, the bell was heard on the speaker and read as
~387 Hz on an uncalibrated phone app.

**M6 is next** — tape phase 1 (ATM via OS traps), snapshots, the menu.

What does not exist: tape, snapshots, the menu, the status band, volume
control from the UI.

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

## Build and test

One CMake source tree, two targets, both under `-Wall -Wextra -Werror`
(`docs/design.md` §14).

```sh
# host: src/core/ with the system compiler, no Pico SDK, under CTest
cmake -S . -B build/host -DPICO_ATOM_HOST=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host -j
ctest --test-dir build/host --output-on-failure

# firmware: needs PICO_SDK_PATH and arm-none-eabi-gcc on PATH
cmake -S . -B build/pico -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build build/pico -j          # -> build/pico/pico-atom.uf2
```

On hardware, with the Debug Probe's SWD and UART both connected:

```sh
tools/uart-log.sh 30 out/run.log &   # capture UART1 first, so the banner is in it
tools/flash.sh                       # program, verify, reset both cores together
tools/uart-type.sh 'PRINT 2+2\r'     # type at the guest over the same UART
```

Characters sent to UART1 are typed at the guest as PicoCalc key events
(`PICO_ATOM_UART_KEYS` in `main.c`). That is how a hardware run is driven
without anyone at the keyboard. Send slowly: `uart-type.sh` paces at 0.25 s a
character because the MOS takes about eight fields a key.

**Core 0 never calls `printf` after the guest starts.** Its lines go through
`log_printf` into a ring that core 1 drains into the UART FIFO. A blocking
heartbeat costs ~30 ms, and the PCM queue's low-water slack is ~11 ms.

`flash.sh` resets with `reset halt` + `resume`, never `reset run`; the reason is
in the script and in hardware-notes.md §2.7. `uart-log.sh` refuses to open a
port something else is reading — two readers split the byte stream and both
logs come out scrambled. `out/` is ignored scratch space for logs.

`test_m6502_functional` runs Klaus Dormann's suite and reports as **skipped**
unless the binary is present. `./tools/fetch-test-suites.sh` downloads it into
`test/suites/` (gitignored — the tree ships no binaries it did not build); the
test finds it there without any environment variable. A skip is not a pass:
treat a skipped functional test as an unverified CPU.

Bruce Clark's decimal test is **not yet wired up** — it is distributed as
source, not as a binary. Until it is, decimal mode is covered by the functional
test's decimal section plus exhaustive valid-BCD checks in
`test/host/test_m6502_decimal.c`; invalid-BCD operands are the remaining gap.

## Where things are

`docs/design.md` §14 has the full intended tree. What is built so far:

| Path | Holds |
|---|---|
| `src/core/config.h` | every fixed capacity; §5's budget lives or dies here |
| `src/core/m6502.*` | the interpreter — switch dispatch, explicit cycle accounting |
| `src/core/bus.*` | `bus_read`/`bus_write` inline fast path, slow path in the `.c` |
| `src/core/i8255.*` | the PPI; generic Intel part, Atom wiring as accessors |
| `src/core/mc6847.*` | mode decode, palette, expansion LUT, row generation |
| `src/core/atom.*` | `atom_t`, the page table, config, the run loop, §4.1's API |
| `src/core/beeper.*` | PC2 → PCM: box filter at the rational sample period, DC blocker (§9.3) |
| `src/core/snappool.*` | §4.2's three-buffer handoff; state machine only, the port holds the lock |
| `src/core/via6522.*` | the VIA; fitted by default because the MOS reads its PCR on every character |
| `src/core/keymatrix.*` | held-key set, paced replay of southbridge events into the matrix (§10.2) |
| `src/core/keymap_picocalc.c` | PicoCalc code -> Atom cell; the cells are the kernel ROM's, by execution |
| `src/core/sha1.*`, `romset.*` | identify ROM images by hash; the slot table from §11.1 |
| `src/port/board.*` | clocks and board identification |
| `src/port/southbridge.*` | i2c1 register layer; refuses to read `RST` (`0x08`), which resets the MCU |
| `src/port/lcd.*` | panel init, windows, fills, polled-DMA ping-pong blit |
| `src/port/display.*` | the §8.4 presenter; owns the renderer, its LUT, the shadow and line buffers |
| `src/port/sd.*`, `diskio.c` | spi0 SD driver and FatFs's disk layer; FatFs itself is copied from the SDK at configure time |
| `src/port/roms.*` | loads `/atom/roms/` into the machine before the guest starts; the no-ROMs page |
| `src/port/kbd.*` | core 1 drains the southbridge FIFO into an SPSC ring for core 0 |
| `src/port/audio.*` | PWM slice, chained ping-pong DMA, PCM queue; the throttle (§9.4, §12.2) |
| `src/port/log.*` | core 0's UART lines, formatted into a ring that core 1 drains |
| `src/port/main.c` | core 0's field loop, core 1's bring-up and live present; M3 measurement behind `PICO_ATOM_MEASURE_PRESENT`; `PICO_ATOM_AUDIO=0` for timer pacing |
| `test/host/` | CTest binaries, one per area, plus `test_util.h`; `test_boot` runs the real MOS when `roms/` holds the images, and measures its bell |
| `test/host/vdg_scenes.*` | the VRAM behind the golden images, shared by the test and `vdg-ppm` |
| `test/golden/` | §15.1's reference PPMs, all nine modes, both colour sets |
| `tools/fetch-test-suites.sh` | pulls the Dormann binary into `test/suites/` |
| `tools/mkfont.py` | character ROM -> `mc6847_font.h`, and `--dump` to proof it |
| `tools/vdg-ppm.c` | renders the scenes to PPM; `vdg-ppm test/golden` regenerates the goldens |
| `tools/uart-log.sh`, `uart-type.sh` | capture UART1 to a file; type at the guest over it |

**Device hooks that do not exist yet are marked in place**, as `/* M2: ... */`
and `/* M9: ... */` comments at the point in `src/core/bus.c` where the call
belongs. Grep for `M[0-9]:` before assuming a device is missing entirely — the
decode is already written and only the device is absent. Keep that convention
when you stub something.

## Conventions

- **A golden image proves nothing until someone has looked at it.** It is
  generated by the code it checks. After `vdg-ppm test/golden`, view every
  image that changed before committing it. On a mismatch
  `test_mc6847_golden` writes `<name>.actual.ppm` to its working directory.

- **Comments cite the document, not the reasoning they replace.** `(§7.1)` or
  `(hardware-notes.md §5.3)` next to a decision that looks arbitrary. The
  cross-references are load-bearing in the docs and they are load-bearing here.
- **Guest addresses are `#XXXX` in comments and `0x` in code**, the same split
  the design document uses.
- **No dynamic allocation in `src/core/`.** State lives in `atom_t` or in
  statically sized buffers from `config.h`.
- **`atom_run` is the seam.** It executes whole instructions until at least the
  requested cycles have elapsed and returns the true count; the caller carries
  the overshoot as debt (`atom_t.budget`). Do not add a "run exactly N cycles"
  variant — the debt-carry pattern is what keeps long-run timing from drifting.
- **Tests use no framework.** `test_util.h` has `CHECK` and `TEST_DONE`; a test
  returning `77` is reported as skipped, which is how a missing third-party
  binary is handled. One binary per area, registered in
  `test/host/CMakeLists.txt`.
- Prefer asserting behaviour by **executing** it over comparing two tables in
  the same repository. `test_m6502_cycles.c` does both, and only the first
  catches an addressing-mode bug.

## Architecture: the parts that are easy to violate

Read `docs/design.md` before writing emulator code. These are the decisions that
a plausible-looking change silently breaks:

- **`src/core/` must not depend on the Pico SDK.** The 6502, bus, 8255, MC6847,
  tape and keymap are portable C with no dynamic allocation, validated on a
  workstation before any hardware exists — the 6502 passes Dormann's functional
  test today. The core building clean on both toolchains is the mechanism that
  keeps SDK dependencies from leaking down, so run the host build too, not just
  the firmware one. `src/port/` is the only place SDK headers belong.
- **The SPI wire is the bottleneck, not the 6502.** A 1 MHz Atom costs an
  estimated 8–17% of one core; a full-screen redraw is ~12.3 ms against a
  16.7 ms field. Optimisation effort belongs on pixels transmitted, not on the
  interpreter.
- **There is no decoded framebuffer, by design.** The MC6847 image is a pure
  function of 6 KiB of guest VRAM plus five mode bits, so bands are generated
  from a snapshot through a mode LUT straight into DMA line buffers. Adding a
  framebuffer would reintroduce the second copy of the screen this design exists
  to avoid.
- **Power-on RAM is zero-filled and the checkerboard is not modelled.** Real
  hardware comes up with uninitialised RAM showing a checkerboard of `0x00`
  and `0xFF` whose pattern depends on the RAM chips fitted. That is
  per-machine noise no software can depend on, so `atom_init` zero-fills and
  leaves it there.
- **Core 0 owns the 6502, 8255 and audio; core 1 owns the LCD, I²C and SD.**
  Handoff is an immutable snapshot with explicit ownership — core 1 never reads
  guest RAM while the 6502 runs.
- **Fixed capacities live in one header** (`src/core/config.h`). SRAM is the
  scarce resource; the budget in §5 is only a link-time fact if capacities stay
  in one place. Check growth with `arm-none-eabi-size build/pico/pico-atom.elf`;
  at M5 `.bss` is ~130 KiB of the 520 KiB budget.
- **`page_t` is exactly two pointers**, with `atom_t.page_flags[]` alongside it.
  Adding a third field pads the descriptor to twelve bytes and puts the page
  table 1 KiB over §5's line for it; the flags are slow-path only, so they do
  not belong in the hot struct.
- **The renderer is not part of `atom_t`.** `mc6847_t` and its 8 KiB LUT live
  in `src/port/display.c` on core 1; core 0 carries VRAM and the port latches,
  and the mode byte is read with `atom_vdg_mode()` at snapshot time. Putting a
  `mc6847_t` back in `atom_t`, or rebuilding a LUT from a bus write, makes
  core 0 rewrite a table core 1 is expanding through — a cross-core race —
  and costs a second 8 KiB LUT (design.md §4.2).

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
- **The port A mode bits are settled: `A/G` is bit 4**, `GM0`–`GM2` are bits
  5–7, read off the Atom circuit diagram. §2.3 had this right and §2.4 had it
  backwards; §2.4 has been corrected and §16's row now records the answer. The
  constants are `VDG_PORT_A_*_BIT` in `mc6847.h`. This was runtime
  configuration while it was unverified and is a constant now that it is not —
  that is the intended lifecycle for a §16 item, not an exception to it.
- **No ROM binaries in the tree.** Acorn's ROMs are copyrighted; the user
  supplies them on SD card under `/atom/roms/`. `README.md` says which five
  files, where to get them (hoglet67's Atomulator) and their SHA-1s;
  `design.md` §11.1 maps them onto the §2.2 address map. The repo-root
  `roms/` is a workstation staging area — ignored except for its `README.md`,
  and nothing reads it at run time.
- **The MC6847 character ROM has a layout that is easy to get wrong.** It is
  `src/core/mc6847_font.c`, a flat `const uint8_t font_6847[768]` with an
  `extern` in the matching `.h`. It is **XRoar's `src/mc6847/font-6847.c`
  taken verbatim**, byte-identical bar the comment header, under the
  GPL-3.0-or-later — keep the attribution in that header and in
  `THIRD-PARTY.md` if you ever regenerate it, since `mkfont.py` emits its own
  banner and would drop it. Three things about the layout:

  - A glyph is **5 px wide in bits 5..1**, and the byte is drawn as it
    stands, bit 7 leftmost: the glyph lands in columns 2..6, with two spacing
    columns on the left and one on the right. Bits 7, 6 and 0 are unused. Do
    not shift it — that puts every character against the left edge of its
    cell, which is how the shift was found and removed.
  - The 7 glyph rows sit at **rows 3..9** of the 12-row cell.
  - Glyph order is the **MC6847's own, not ASCII**: index 0–31 are `$40`–`$5F`
    (`@A`–`Z[\]^_`), index 32–63 are `$20`–`$3F` (space onwards). Use
    `mc6847_glyph_ascii()` rather than open-coding it.

  `test_mc6847.c` asserts all three against the data itself, so a differently
  laid-out ROM fails loudly instead of rendering plausible-but-wrong glyphs.

  **A screen of `@` is correct, not a bug.** Glyph 0 is `@`, so zeroed VRAM
  renders as `@` throughout until the MOS clears the screen by writing spaces.
  Do not make the renderer substitute blanks — that would hide a ROM that
  never cleared the screen, and there is a test pinning the behaviour.
  `MC6847_GLYPH_SPACE` exists for tooling that wants a legible blank page,
  such as the `vdg-ppm` font sheets; the emulator never uses it.

  Both builds detect `src/core/mc6847_font.c` by existence, compile it in, and
  define `PICO_ATOM_HAVE_FONT`. Without it alpha mode draws a hollow box per
  cell and says so at boot. **Do not fabricate a font**; §16 wants it
  transcribed and then verified by image:

  ```sh
  ./tools/mkfont.py src/core/mc6847_font.c --dump   # proof glyphs as text
  ./build/host/test/host/vdg-ppm out/               # render sheets to PPM
  ```

## Measurement discipline

Every performance figure in `docs/design.md` is currently an arithmetic estimate
derived from `hardware-notes.md`, not a measurement, and is labelled as such.
When replacing one with a real number (`hardware-notes.md` §9.1): profile in the
mode you ship, carry a control quantity that should not change, expect ~2 %
run-to-run spread, compare on one board, and write results to a file — a number
on a 320×320 panel cannot be copied off it.
