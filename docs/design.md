# pico-atom — design

An **Acorn Atom** emulator for the **ClockworkPi PicoCalc**, written in C against
the Raspberry Pi Pico SDK.

Companion document: [`hardware-notes.md`](hardware-notes.md). Section references
of the form §4.7 point there unless stated otherwise; that document is the
authority on the host, this one on the guest and on the shape of the code.

**Status:** M0–M12 are built and verified on a Pimoroni Plus 2 W (§17).
The document began as a design written before any code, and much of it still
reads that way. Where a section has been built, it says so and gives the
measured figures, which replace the arithmetic estimates the design started
with. A figure still labelled an estimate has not been measured. §16 lists
the constants that must be confirmed against primary sources before they are
typed into a header, and records how each one that has been confirmed was
settled.

**Contents**

1. [Goals and non-goals](#1-goals-and-non-goals) ·
2. [The guest machine](#2-the-guest-machine) ·
3. [Host target and build configuration](#3-host-target-and-build-configuration) ·
4. [Architecture](#4-architecture) ·
5. [Memory budget](#5-memory-budget) ·
6. [CPU](#6-cpu-mos-6502) ·
7. [Bus and memory map](#7-bus-and-memory-map) ·
8. [Video](#8-video-mc6847--st7789p) ·
9. [Audio](#9-audio) ·
10. [Keyboard](#10-keyboard) ·
11. [Storage, tape and disc](#11-storage-tape-and-disc) ·
12. [Timing and synchronisation](#12-timing-and-synchronisation) ·
13. [User interface](#13-user-interface) ·
14. [Repository layout and build](#14-repository-layout-and-build) ·
15. [Testing and verification](#15-testing-and-verification) ·
16. [Constants to confirm](#16-constants-to-confirm-before-coding) ·
17. [Milestones](#17-milestones) ·
18. [Risks](#18-risks) ·
19. [References](#19-references)

---

## 1. Goals and non-goals

### Goals

- Run stock Acorn Atom ROMs (MOS, BASIC, floating point) well enough that Atom
  BASIC, the built-in assembler, and the common body of Atom software behave as
  they do on real hardware.
- **Cycle-correct 6502 at instruction granularity.** Every documented
  instruction consumes the right number of cycles, including page-crossing and
  branch penalties, and decimal mode is exact. This is cheap on this host (§6)
  and it is the difference between "mostly works" and "loads tapes".
- **Real-time or better**, with headroom to spare, at the stock 150 MHz system
  clock and without PSRAM — so the same image runs on a Pico 2, a Pico 2 W and a
  Pimoroni Plus 2 W.
- Self-contained operation: ROMs, software and snapshots come off the SD card;
  no host PC needed once the image is flashed.
- A **host-testable core**. The 6502, the bus, the VDG and the tape decoder
  build and run on a workstation with no Pico SDK, under CTest.

### Non-goals (for v1)

- Cycle-exact video bus contention between the 6502 and the MC6847 ("Atom snow",
  §8.6). It was once planned as an option and is dropped.
- A scaled display. The Atom's 256×192 is drawn 1:1 (§8.2).
- Analogue-accurate cassette audio, UHF artefact colour, or composite blur.
- Second-processor, Econet, colour-board or other third-party expansions.
- Wi-Fi. The radio costs SRAM (§2.6) and buys the Atom nothing. Leave GP0–GP1
  and the CYW43 alone.
- Shipping ROM images. Acorn's ROMs are copyrighted; the user supplies them.

### The one-line summary of the engineering problem

The Atom is a 1 MHz machine with 6 KiB of video RAM. **The 6502 is not the
bottleneck on an RP2350 — the SPI wire to the LCD is** (§8.4). Design decisions
throughout this document trade CPU time, of which there is plenty, against pixels
on the wire and against SRAM, of which there is enough but not unlimited.

---

## 2. The guest machine

### 2.1 What an Acorn Atom is

| | |
|---|---|
| CPU | MOS 6502, 1 MHz |
| Video | Motorola **MC6847** VDG, 256×192 maximum, 9 colours |
| Video RAM | `#8000`–`#97FF`, 6 KiB, shared with the CPU |
| I/O | **INS8255** PPI at `#B000` — keyboard, VDG mode, speaker, cassette |
| Optional I/O | 6522 VIA at `#B800`; 8271 FDC at `#0A00` with AtomDOS |
| ROM | 16 KiB at `#C000`–`#FFFF`, plus a utility socket at `#A000` |
| Sound | one bit, PPI port C bit 2, into a loudspeaker |
| Storage | cassette, 300 baud CUTS; optionally 5.25" disc via AtomDOS |

The Atom writes hex as `#XXXX`; this document keeps that convention for guest
addresses and uses `0x` for host values, which removes a whole class of reading
error in a document that talks about both machines at once.

### 2.2 Address map

The emulator models a **fully expanded** Atom by default. Stock machines
populate fewer blocks; the populated set is configuration, not code (§7.2).

| Range | Size | Contents |
|---|---:|---|
| `#0000`–`#00FF` | 256 B | Zero page — MOS and BASIC workspace, user ZP from `#00A0` |
| `#0100`–`#01FF` | 256 B | 6502 stack |
| `#0200`–`#03FF` | 512 B | MOS workspace, indirection vectors, buffers |
| `#0400`–`#3FFF` | 15 KiB | User RAM / BASIC text space (expansion) |
| `#4000`–`#7FFF` | 16 KiB | User RAM on a 32 KiB machine, which games assume (§7.2) |
| `#8000`–`#97FF` | 6 KiB | **Video RAM**, read by the VDG |
| `#9800`–`#9FFF` | 2 KiB | Video RAM aperture, unpopulated on a stock machine |
| `#A000`–`#AFFF` | 4 KiB | Utility ROM socket |
| `#B000`–`#B003` | 4 B | **INS8255 PPI** |
| `#B400`–`#B403` | 4 B | Expansion / printer port |
| `#B800`–`#B80F` | 16 B | 6522 VIA (optional on the real machine; fitted by default here, §7.3) |
| `#C000`–`#CFFF` | 4 KiB | Atom BASIC ROM |
| `#D000`–`#DFFF` | 4 KiB | Floating-point ROM |
| `#E000`–`#EFFF` | 4 KiB | AtomDOS / utility ROM |
| `#F000`–`#FFFF` | 4 KiB | Atom MOS (kernel), with the 6502 vectors at `#FFFA` |

**Address decoding in `#B000`–`#BFFF` is partial**, so the 8255 mirrors every
four bytes through its block. Some software relies on a mirror. The bus layer
therefore decodes I/O by *mask*, not by exact address (§7.3).

### 2.3 The 8255 PPI at `#B000`

This single chip is the whole of the Atom's I/O, and it is where most of the
emulator's device work lands.

| Port | Dir | Bits | Function |
|---|---|---|---|
| A `#B000` | out | 0–3 | Keyboard column select, 0–9 |
| | | 4–7 | VDG mode: `A/G`, `GM0`, `GM1`, `GM2` |
| B `#B001` | in | 0–5 | Keyboard row sense, **active low** |
| | | 6 | `CTRL`, active low |
| | | 7 | `SHIFT` (either key), active low |
| C `#B002` | out | 0 | Cassette data out |
| | out | 1 | Enable 2400 Hz tone to cassette |
| | out | 2 | **Loudspeaker** |
| | out | 3 | VDG `CSS` — colour set select |
| | in | 4 | 2400 Hz cassette input |
| | in | 5 | Cassette data in |
| | in | 6 | `REPT` key, active low |
| | in | 7 | VDG **field sync** (`FS`), the 60 Hz flyback flag |
| Ctrl `#B003` | out | — | 8255 mode/BSR control register |

Three consequences:

- **Port C is bidirectional per nibble.** A read returns the input nibble
  (bits 4–7) alongside the last value latched into the output nibble. The
  8255 model must keep output latches separate from the input sources, or
  read-modify-write sequences in the MOS corrupt the mode bits.
- **Bit-set/reset via `#B003`** is how the MOS toggles the speaker and the
  cassette bits. The BSR path must be implemented, not just the mode path.
- The VDG mode lives in the top nibble of port A and the keyboard column in
  the bottom nibble, so **every keyboard scan writes the video mode too**.
  Mode-change detection must compare the mode nibble, not the whole port.

### 2.4 MC6847 modes

`A/G` (port A bit 4), `GM0`, `GM1` and `GM2` (bits 5, 6 and 7 respectively) and
`CSS` (port C bit 3) select the mode. Note that the `GM` bits ascend with the
bit number, so they read out reversed relative to the `GM2:0` column below.
Atom BASIC's `CLEAR n` maps onto these:

| `A/G` | `GM2:0` | VDG mode | Resolution | Colours | VRAM | Atom |
|:--:|:--:|---|---|---:|---:|---|
| 0 | — | Alpha / SG6 | 32×16 chars, 64×48 blocks | 2 + 4 | 512 B | `CLEAR 0` |
| 1 | 000 | CG1 | 64×64 | 4 | 1024 B | `CLEAR 1a` |
| 1 | 001 | RG1 | 128×64 | 2 | 1024 B | `CLEAR 1b` |
| 1 | 010 | CG2 | 128×64 | 4 | 2048 B | `CLEAR 2a` |
| 1 | 011 | RG2 | 128×96 | 2 | 1536 B | `CLEAR 2b` |
| 1 | 100 | CG3 | 128×96 | 4 | 3072 B | `CLEAR 3a` |
| 1 | 101 | RG3 | 128×192 | 2 | 3072 B | `CLEAR 3b` |
| 1 | 110 | CG6 | 128×192 | 4 | 6144 B | `CLEAR 4a` |
| 1 | 111 | RG6 | 256×192 | 2 | 6144 B | `CLEAR 4b` |

Every mode presents as **256×192** on screen; the VDG stretches horizontally
(×1, ×2 or ×4) and vertically (×1, ×2 or ×3). The emulator's row generator does
the same, so the host display geometry is a single fixed 256×192 rectangle
regardless of guest mode (§8.2).

In alpha mode the Atom wires VRAM data bit 6 to the VDG's `A/S` **and**
`INT/EXT` pins and bit 7 to `INV`, so per character byte:

- bit 6 = 0 → alphanumeric: bits 5–0 select one of 64 glyphs from the VDG's
  internal 5×7-in-8×12 character ROM, and bit 7 = 1 draws it in inverse video.
  The MOS draws its cursor by setting bit 7 of the cell under it, and shows
  lower case as inverse capitals. Over a graphics cell the same bit is only colour, so
  after `CLEAR 0` fills the screen with `#40` the cursor (`#C0`, no elements
  lit) is invisible; it reappears on text.
- bit 6 = 1 → **semigraphics 6**, because `INT/EXT` is high whenever `A/S` is:
  bits 5–0 are six elements, two across and three down (bits 5 and 4 the top
  pair, 1 and 0 the bottom), and the colour is `C1:C0` = bits 7:6 in `CSS`'s
  set. Bit 6 is always 1 here, so only yellow and red (or cyan and orange) are
  reachable. This is where the Atom's chunky block graphics come from:
  `CLEAR 0` is 64×48, and `PLOT` sets one element bit per point.

An earlier version of this section had bits 7 and 6 the other way round, with
bit 7 selecting SG4. That rendered the cursor as an empty graphics cell, which
is how it was caught on hardware; §16 records how it was settled.

Colour sets: alpha `CSS`=0 green-on-black, `CSS`=1 orange-on-black. RG modes
black + green or black + buff. CG modes green/yellow/blue/red or
buff/cyan/magenta/orange. Nine distinct colours in total, so a **16-entry
RGB565 palette** covers everything with room for UI colours.

---

## 3. Host target and build configuration

### 3.1 Board

**Primary target: Raspberry Pi Pico 2 (RP2350A).** The whole design fits in
520 KiB of SRAM with room to spare (§5), so the Plus 2 W's PSRAM is not used;
that keeps one image working on all three RP2350 boards and avoids the QMI
timing hazard of §3.

An RP2040 build is plausible but tight, and has not been tried. Treat it as a
stretch goal and keep the code free of RP2350-only assumptions where that
costs nothing. Today only `board.c` tests `PICO_RP2350`. Three things decide
whether it would work:

- **SRAM.** The shipping image uses ~251 KiB of main SRAM (§5). An RP2040 has
  256 KiB plus two 4 KiB scratch banks, which hold the stacks. That leaves
  about 5 KiB. The 64 KiB UEF deck, the M3 test scene (6 KiB) and the ROM load
  buffer (8 KiB) are the places to find room.
- **The interpreter on a Cortex-M0+.** On the M33 at 150 MHz the guest takes
  35–46 % of core 0 (§6.3). The M0+ lacks Thumb-2's IT blocks, bitfield
  extracts and wide immediates, so the same C will take more cycles per
  instruction. How many more is unmeasured, and it is the number that
  decides the port.
- **The clock trades the CPU against the display.** At the RP2040's supported
  200 MHz the SPI divider lands on **50 MHz** rather than 75 (hardware notes
  §3), and a full redraw grows from 11.5 ms to ~17 ms, past a 16.7 ms field.
  150 MHz keeps the panel at 75 MHz and gives the interpreter a quarter less
  time.

Per hardware notes §2.1, the banner must report **physical board identity
separately from the SDK build target**.

### 3.2 Clocks

Ship at the stock **`clk_sys` = 150 MHz**. That is the rated part speed, and it
is one of only two points where the SPI divider delivers the full **75 MHz** to
the panel (hardware notes §3); 200 MHz would make the 6502 1.33× faster, which
we do not need, and the display 1.5× slower, which we cannot afford.

Build with `PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1` and re-apply the SPI
baud rate after clock setup regardless, because `set_sys_clock_pll` otherwise
parks `clk_peri` on the 48 MHz USB PLL.

**300 MHz is a setting, since M13**: `host_clock = 300` in the settings file
(§11.7), or *Pico clock* on the Machine page (§13.1). It is the only other free
SPI divider point, and it is outside the RP2350 datasheet, so it is off by
default. Its purpose is a 4 MHz guest (§12.1), which does not fit in core 0 at
150. M12's 2 MHz guest runs at the stock 150 MHz.

It is set once, at power-on, before anything is brought up, because every
peripheral works out its rate from the clock it finds (hardware notes §3).
`main()` sets 150 MHz, mounts the card and reads the settings file on core 0,
with nothing printed, then, if the file asks, moves to 300 MHz. Only then do
stdio, the southbridge, the LCD and audio start. Core 1 reads the file again at
its usual point and reports what it finds (§11.7). The move to 300 MHz, in
`board_init_clocks()`:

1. the core rail to `VREG_VOLTAGE_1_20`, and a millisecond to settle;
2. the flash's QMI divider and RX delay doubled, from SRAM with interrupts off,
   so the flash sees at 300 MHz exactly the timing the bootrom gave it at 150.
   The bootrom leaves clkdiv 3 and RX delay 2, 50 MHz, measured on a Plus 2 W
   on 2026-09-27, and at 300 MHz the banner reads clkdiv 6 and RX delay 4. Hot
   code is in SRAM already (§6.3), so only cold code runs at flash speed;
3. the PLL to 300 MHz, and `clk_peri` onto `clk_sys`.

At 150 MHz the rail is put back to its 1.10 V default after the clock is set.
The regulator is not reset with the chip. On a Plus 2 W on 2026-09-27, a
150 MHz build flashed after a 300 MHz run came up at 1.20 V, and so would
the Machine page's restart to 150 MHz.

After that each driver's rate comes out the same as at 150. The LCD's SPI is
75 MHz (÷4), the SD card's 25 MHz, I²C and the UART are divided from
`clk_peri` as always, and the audio PWM's divider is `clk_sys / 150 MHz`, so
the carrier, the sample rate (36,621 Hz) and the DMA's cadence do not move. The
emulator uses no PSRAM and no radio, so the hardware notes' two other items do
not arise. Changing the clock from the Machine page writes the settings file
and restarts the Pico with the watchdog, rather than moving the clock under
running peripherals.

### 3.3 Peripheral assignment

| Resource | Owner | Notes |
|---|---|---|
| `spi1` @ 75 MHz | LCD | CS/DCX/RST as plain GPIO (§4.2) |
| `spi0` @ 400 kHz → 25 MHz | SD card | separate instance, no CS arbitration with the LCD |
| `i2c1` @ 10 kHz | Southbridge | keyboard, backlight, battery |
| PWM slice from `pwm_gpio_to_slice_num(26)` | Audio | both channels, one 32-bit compare word |
| DMA ch. 0 | LCD blit | **polled, no IRQ** (§4.6) |
| Two more DMA channels | Audio ring | chained pair, `DMA_IRQ_0` at priority `0x40` |
| Core 0 | 6502, 8255, VDG snapshot, audio synthesis | |
| Core 1 | LCD blit, southbridge I²C, SD | |

DMA channels and the IRQ owner are claimed explicitly at init (hardware notes
§10). The LCD claims channel 0 by number, and audio claims two unused
channels after it. The SD driver is polled SPI and uses no DMA.

---

## 4. Architecture

### 4.1 Layering

```
      ┌───────────────────────────────────────────────────────┐
      │  port/   PicoCalc + Pico SDK                          │
      │  main · board · lcd · display · audio · kbd · sd · ui │
      └───────────────┬───────────────────────────────────────┘
                      │  narrow, synchronous interface
      ┌───────────────▼───────────────────────────────────────┐
      │  core/   pure C, no SDK, host-buildable               │
      │  m6502 · bus · i8255 · mc6847 · via6522 · tape        │
      │  keymatrix · snapshot                                 │
      └───────────────────────────────────────────────────────┘
```

`core/` knows nothing about DMA, I²C, SPI or `pico/stdlib.h`. Its centre is
this, in `src/core/atom.h`:

```c
void     atom_init(atom_t *m, const atom_config_t *cfg);
void     atom_reset(atom_t *m);
uint32_t atom_run(atom_t *m, uint32_t cycles);   /* returns cycles actually run */
uint32_t atom_run_field(atom_t *m);              /* one field, split at FS (§12.1) */
void     atom_key_set(atom_t *m, uint8_t row, uint8_t col, bool down);
void     atom_key_mods(atom_t *m, bool shift, bool ctrl, bool rept);
void     atom_field_sync(atom_t *m, bool in_flyback);
size_t   atom_audio_drain(atom_t *m, int16_t *dst, size_t max);
const uint8_t *atom_vram(const atom_t *m);       /* 6 KiB */
uint8_t  atom_vdg_mode(const atom_t *m);         /* A/G, GM2:0, CSS packed */
void     atom_copy(atom_t *dst, const atom_t *src);   /* never `=` */
```

Beside it are the tape deck (`atom_cassette_*`, §11.3), the disc requests the
port serves (`atom_disc_*`, §11.4), ROM loading and `atom_seed_rnd` (§7.2).

That interface is the seam every test in §15 exercises, and it is the reason the
6502 can be validated on a workstation against Klaus Dormann's functional tests
before any hardware exists.

### 4.2 Core split

Following hardware notes §9.5 — core 1 owns the slow peripherals:

| | Core 0 | Core 1 |
|---|---|---|
| Owns | 6502, 8255, VDG state, tape, audio synthesis, SD requests | LCD, southbridge I²C, SD transfers |
| Per field | run 16,666 guest cycles, emit PCM, snapshot VRAM | expand snapshot → RGB565 bands → DMA; poll keyboard |
| Blocks on | audio ring fill level (§12.2) | DMA completion (polled), I²C |

Handoff is an **immutable snapshot with explicit ownership**, over a pool of
**three** buffers, each in one of four states:

| State | Meaning |
|---|---|
| `free` | owned by nobody; core 0 may claim it |
| `filling` | core 0 is writing it; core 1 must not look |
| `ready` | complete, awaiting collection |
| `rendering` | core 1 owns it |

Per field, core 0 marks its `filling` buffer `ready` and takes any `free` one;
core 1, when idle, takes the `ready` buffer, marks it `rendering`, and frees it
on completion. Publishing drops any older `ready` buffer unrendered, on the
spot — that is §12.2's superseded-snapshot rule, made explicit, and doing it at
publish rather than at take is what guarantees core 0's next claim always finds
a `free` buffer. At most one buffer is ever `ready`, so core 1 needs no sequence
numbers to find the newest.

The state machine is `src/core/snappool.c` and holds no lock of its own; the
port calls each transition under the spinlock described below. Keeping the
lock out of it is what lets `test_present` drive it on a host through a long
randomised interleaving.

The renderer — `mc6847_t` and its 8 KiB expansion LUT — belongs to core 1 and
is **not** part of `atom_t`. Core 0 carries only VRAM and the port latches; the
mode byte is read off them with `atom_vdg_mode()` when the snapshot is taken.
A LUT that core 0 rebuilt on a port A write while core 1 expanded rows through
it would be a race, and a second copy would cost the 8 KiB §5 does not have.

**Three buffers, not two, and the third one is the whole point.** Core 1's worst
case is ~17–18 ms (§8.4) against core 0's 16.7 ms field, so the consumer can be
a full field behind. With two buffers, core 0 would at that moment have to
either block — which corrupts the simulation schedule that audio is paced
against — or rewrite a `ready` buffer core 1 may be claiming in the same
instant, which is a race no amount of care in the renderer can see. The third
buffer costs 6 KiB out of ~265 KiB spare (§5) and deletes the entire class of
problem; do not economise here.

State transitions are published under a single SIO spinlock. RP2350 has no
compare-and-swap, and "explicit ownership" is not a synchronisation primitive —
local interrupt masking is not a multicore lock (hardware notes §9.5). No locks
are held across a blit, and core 1 never reads guest RAM while the 6502 runs.
Between service iterations core 1 waits 20 µs on the hardware timer rather than
spinning on shared state. It waits with `busy_wait_us_32`, not `sleep_us`,
which sets an alarm whose IRQ lands on core 0 (§6.3, hardware notes §9.7).

Two invariants, both taken straight from the hardware notes' scar tissue:

- **The LCD is touched only from thread context on core 1, never from an
  interrupt handler** (§5.7).
- **Nothing masks interrupts around a blit.** The audio refill deadline is
  ~3.5 ms and it outranks everything (§5.4).

### 4.3 Data flow, one field

```
core 0                                        core 1
──────                                        ──────
run 6502 for one field's cycles
  │  writes #8000-#97FF ──────────┐
  │  writes PC2 → speaker events  │
  │  emits PCM into SPSC ring ────┼──→ audio DMA IRQ (core 0, pri 0x40)
  └─ at field end:                │
       copy 6 KiB VRAM + mode ────┴──→ snapshot[n]
       publish n, set FS flag                │
                                             ├─ diff snapshot[n] vs shadow
                                             ├─ per 8-row band: dirty? span?
                                             ├─ expand rows via mode LUT → line buf
                                             ├─ DMA line buf → panel window
                                             ├─ copy snapshot[n] → shadow
                                             └─ poll southbridge FIFO → key events
                                                        │
                                             ← key events queue ← ─ ─ ─ ─ ─ ┘
```

---

## 5. Memory budget

RP2350A has 520 KiB. Budget code placed in SRAM, static buffers, both stacks,
DMA storage and peak heap together (hardware notes §2.3).

**Measured**, from the linked image (`arm-none-eabi-size -A` and `nm`), a
`pico2` Release build at `PICO_ATOM_RAM_TIER` 2, 2026-09-25:

| Item | Bytes | Symbol, note |
|---|---:|---|
| The machine | 72,952 | `g_atom`: 64 KiB address space, page table, 8271 track buffer, the chips, the core's 1024-sample audio buffer |
| UEF deck | 65,536 | `s_uef`, `ATOM_UEF_MAX` (§11.3) |
| VDG snapshots ×3 | 18,468 | `g_pool` (§4.2) |
| Renderer and its LUT | 8,204 | `s_vdg`, core 1's `mc6847_t` (§8.3) |
| ROM load buffer | 8,192 | `s_image`, two ROMs' room for MAME's `abasic.ic20` (§11.1) |
| Menu's tape and disc lists | 15,360 | `s_list`, `s_discs`, 48 × 160 B each (§13) |
| Presented shadow | 6,144 | `s_shadow` (§8.4) |
| M3 test scene | 6,144 | `s_scene`, the test pattern's VRAM |
| PCM queue | 4,096 | `s_queue` (§9.4) |
| Audio DMA ring, log ring, core 0's drain buffer | 6,144 | 2,048 each |
| RGB565 line buffers | 1,280 | `s_line` |
| Everything else in `.bss` | 15,128 | FatFs, keymaps, settings text, counters |
| `.data`: hot code in SRAM, and initialised data | 27,188 | tier 2 (§6.3) |
| Heap, vector table | 2,336 | |
| Stacks, both cores | 4,096 | in the scratch banks |
| **Total** | **~255 KiB** | **49 % of 520 KiB** |

M11 added about 2.2 KiB to `.bss`, which is now 230,676 bytes: the 2 KiB
buffer `settings_rewrite()` returns its text in, the recorder's state in
`g_atom` (72,952 to 73,016 bytes), and the status line's six bytes in each
of the three snapshots (`g_pool`, 18,492). A recording adds nothing, since it
goes into the UEF deck.

M12 added 1,336 bytes to `.bss`, measured on 2026-09-26 against the M11
tree built with the same toolchain, which comes to 233,012 bytes, not the
figure above: 234,348. The utility ROM list (`s_utils`, 576), the ROM report
with each slot's SHA-1 (`g_roms`, 148), the utility's name in the settings
(`g_file`, +128), the machine's clock and field parts (`g_atom`, +40), the
board for the About page (36), the perf line's words (24), and the restart's
messages.

The MC6847 character ROM is `const` and stays in flash. The largest items that
are not the machine are the UEF deck and buffers that sit idle once the guest
runs (the ROM load buffer and the M3 scene), which is where to look first if
SRAM is ever needed.

The estimate the design started with is kept below, because its rows say what
each item was for. It totalled ~228 KiB, 44 %:

| Item | Bytes | Note |
|---|---:|---|
| Guest address space, flat 64 KiB | 65,536 | direct index; simplest and fastest (§7.1) |
| Page descriptor table, 256 × 2 × 4 B | 2,048 | read and write pointers |
| VDG snapshots ×3, 6 KiB each | 18,432 | core 0 → core 1 handoff (§4.2) |
| Presented shadow | 6,144 | dirty-band diffing |
| Mode expansion LUT | 8,192 | rebuilt on mode/`CSS` change (§8.3) |
| RGB565 line buffers, 2 × 320 × 2 B | 1,280 | DMA ping-pong (§4.6) |
| UEF deck, decompressed | 65,536 | `ATOM_UEF_MAX`, added at M8 (§11.3) |
| 8271 track buffer | 2,560 | `ATOM_FDC_BUF_LEN`, in `atom_t`, added at M9 (§11.4) |
| Menu's disc list | 7,680 | 48 × 160 B, `ATOM_DISC_LIST_MAX` (§13) |
| MC6847 character ROM | 768 | 64 glyphs × 12 rows |
| Audio DMA ring, 2 halves × 128 frames × **2 slots** × 4 B | 2,048 | power-of-two, aligned, hardware wrap |
| PCM software queue, 1024 × 4 B | 4,096 | ~28 ms (§5.8) |
| 6502 + bus hot code in SRAM | ~16,000 | `__not_in_flash_func` (§9.2) |
| Display + audio hot code in SRAM | ~6,000 | |
| FatFs + SD buffers | ~2,500 | |
| Stacks, both cores | 8,192 | |
| Heap, UI, perf counters, misc | ~16,000 | |
| **Total** | **~228 KiB** | **44 % of 520 KiB** |

Two observations:

- There is enough slack to hold a **second 64 KiB guest image** for instant
  snapshot restore, and still be under 62 %.
- An RP2040's 256 KiB of main SRAM would hold the measured image with ~5 KiB
  to spare, not at the ~58 % first written here, which predated the UEF deck
  and the SRAM tier. §3.1 has what a port would need.

Fixed capacities live in one header, `src/core/config.h`, because they will be
traded against each other repeatedly (hardware notes §2.3).

---

## 6. CPU: MOS 6502

### 6.1 Accuracy target

- All 151 documented opcodes, exact cycle counts, including the extra cycle on
  page-crossing indexed reads and on taken branches, and the extra cycle when a
  branch crosses a page.
- **Decimal mode exact**, including the NMOS flag behaviour that Bruce Clark's
  test checks.
- Correct NMOS quirks that software actually depends on: the `JMP (xxFF)` page
  wrap, the `BRK`/`IRQ` flag-B distinction, the read-modify-write double write.
- Undocumented opcodes: **v1 traps and logs them**; the stable subset
  (`LAX`, `SAX`, `DCP`, `ISC`, `SLO`, `RLA`, `SRE`, `RRA`, `ANC`, `ALR`, `ARR`,
  `SBX`) lands in v2. Atom ROMs do not use them; some later software does.

### 6.2 Implementation

A **switch-dispatched interpreter** with explicit cycle accounting, not a
jump-table of function pointers: on Cortex-M33 a dense `switch` compiles to a
table-indexed branch with no call overhead, and it keeps the CPU state in
registers across the whole dispatch.

```c
typedef struct {
    uint16_t pc;
    uint8_t  a, x, y, s, p;
    uint64_t cycles;
    uint8_t  irq_lines;      /* bitmask of asserted IRQ sources */
    bool     nmi_pending;    /* edge-triggered, latched */
    bool     nmi_line;       /* last level seen, for edge detection */
    bool     reset_pending;  /* BREAK key */
    uint32_t undoc_count;    /* undocumented opcodes trapped (§6.1), */
    uint16_t undoc_pc;       /* and the latest one's PC and opcode   */
    uint8_t  undoc_op;
} m6502_t;
```

Cycles accumulate into `m->cycles`; the bus layer (§7) is what advances devices,
so the CPU core itself has no notion of a device. `atom_run(m, n)` executes whole
instructions until at least `n` cycles have elapsed and returns the true count,
which the caller carries forward as a debt — a deterministic pattern that keeps
long-run timing from drifting.

### 6.3 Budget

**Measured at M7** on a Plus 2 W, 2026-09-23, in the mode that ships: paced
on the PCM queue, core 1 presenting, 150 MHz. The heartbeat's `perf` line
reports it (§12.3); `tools/perf-run.sh` types four workloads at the guest,
one boot each, and `tools/perf-summary.sh` reduces the logs.

| Workload | Guest cycles / insn | Host cycles / insn | Core 0 in the guest | Headroom |
|---|---:|---:|---:|---:|
| idle at the `>` prompt | 3.14 | 217 | 46 % | 2.17× |
| BASIC compute loop | 3.40 | 176 | 35 % | 2.89× |
| `PRINT` loop, scrolling | 3.49 | 218 | 42 % | 2.40× |
| bell loop (`PRINT $7`) | 2.50 | 158 | 42 % | 2.37× |

Headroom is guest cycles per microsecond spent inside `atom_run_field`: how
many times real time the guest would run unpaced. It counts everything that
lands on core 0 while the guest runs, audio IRQs and bus contention with
core 1 included, because that is what the shipped machine pays.

**The estimate this replaces was 40–80 host cycles per instruction and
8–17 % of a core; the truth is 158–218 and 35–46 %.** The 6502 is still not
the constraint at 1 MHz, but the margin is about 2×, not 6–12×. That bounds
a turbo mode (§11.3, M8) at roughly 2× before pacing fails, unless the
interpreter itself gets cheaper. The code is about 100 Thumb instructions
per guest instruction — `atom_run`'s loop, the VIA tick and IRQ line, the
dispatch, the opcode — so the cost is the work, not instruction fetch.

**Hot code in SRAM**, measured tier by tier per hardware notes §9.2
(`src/core/hot.h`; `PICO_ATOM_RAM_TIER` in `CMakeLists.txt`). Host cycles
per instruction, each tier built in its own directory and every symbol
checked to have moved from `0x1…` to `0x2…`:

| Tier | What moved | SRAM | compute | idle | scroll | bell |
|---|---|---:|---:|---:|---:|---:|
| 0 | nothing | — | 209.5 | 242.4 | 245.0 | 161.0 |
| 1 | the callees: VIA tick and IRQ line, bus slow path, 8255, beeper, `op_adc`/`op_sbc` | 4.4 KiB | 189.0 (1.11×) | 233.6 (1.04×) | 231.5 (1.06×) | 167.8 (0.96×) |
| 2 | `m6502_step`, `atom_run`, `fetch16` | 20.6 KiB | 176.3 (1.07×) | 217.2 (1.08×) | 218.2 (1.06×) | 158.1 (1.06×) |
| 3 | the 256-byte cycle table | 0.26 KiB | 176.3 (1.00×) | 217.2 (1.00×) | 218.0 (1.00×) | 158.1 (1.00×) |

**Tier 2 ships: 1.12–1.19× for 25 KiB.** Tier 3 bought nothing and was
dropped. The gain is a third of hardware notes §9.2's 1.7×: `m6502_step` is
20 KiB after `-O3` inlines the bus into every opcode, but the part a real
program runs evidently fits the 16 KiB XIP cache. The bell's tier-1
regression is §9.2's relayout effect, and tier 2 recovered it. Tier 0
repeated across two sittings to within 0.3 %; heartbeats within a run spread
±1–2 %. The control, the PWM's consumed sample rate, read 36,620 Hz in every
row, with no underruns and no late refills.

**The larger win was not placement.** Core 1's idle wait was
`sleep_us(20)`, and `sleep_us` sets an alarm in the SDK's default pool, whose
IRQ is core 0's. Every iteration of core 1's loop interrupted the guest:
replacing it with `busy_wait_us_32` took compute from 197 to 176 host cycles
per instruction and idle from 245 to 218 at the same tier — 1.11×, and the
run-to-run spread fell from ±4 to ±0.1 (hardware notes §9.7).

**At M8** the same four workloads were rerun with the cassette in place
(§11.3). Compute, idle and bell are within 1 % of the table above: 178.1,
219.3 and 159.3. Scroll is 225.8, and a control build with only the port C
hook removed reads 218.2, so the hook costs 3.5 % there and headroom falls
from 2.40× to 2.32×. The first version cost 18 %, three calls on every
port C read. Turbo while a tape plays measured 2.7–2.8×, the headroom of the
MOS's tape loops.

**At M10** the whole VIA (§7.4) first cost 2.5–4 % on every workload, against
a control build of M9 run in the same sitting: the shift register, inlined
into the per-instruction tick, grew its prologue, and the tick checked the
shift register after every instruction. Moving the shift register out of line
recovered half. Then the tick became a countdown. It subtracts from T1 and
from one counter to whatever T2 or the shift register does next, and calls
out of line only when one of them runs out. That made it nine instructions
with no stack frame, shorter than M9's, which tested the ACR every time:

| Workload | M9 | M10 first built | M10 shipped |
|---|---:|---:|---:|
| idle | 214.9 | 221.0 | 204.2 (−5.0 %) |
| compute | 171.7 | 177.8 | 161.7 (−5.8 %) |
| scroll | 233.0 | 238.9 | 224.5 (−3.6 %) |
| bell | 155.3 | 161.7 | 145.7 (−6.2 %) |

Host cycles per instruction on a Plus 2 W, 2026-09-23. Each build was run
twice, and so was M9, four times across the sitting, reading within 0.1 every
time. Headroom is now 2.31–3.15×. The control read 36,620 Hz in every row,
with no underruns and no late refills. Removing only the PB7 lines from the
first build recovered another 1 %, but a build with M9's tick shape still
stayed about 1 % above M9, so part of what the countdown won is outside the
tick.

**At M11** the four workloads were run against two controls in one sitting:
the same build with only the recorder's hook on the port C write removed
(§11.3), and the committed tree before M11. Host cycles per instruction on a
Plus 2 W, 2026-09-26:

| Workload | M11 | M11 without the hook | before M11 |
|---|---:|---:|---:|
| idle | 205.9 | 206.5 | 206.2 |
| compute | 163.0 | 164.6 | 164.0 |
| scroll | 224.6 | 225.0 | 222.8 |
| bell | 147.0 | 147.5 | 147.3 |

The hook costs nothing measurable: M11 read 0.2–1.0 % faster than the build
without it, which is layout, not the hook. Against the tree before M11 every
workload is within 1 %, scroll the most at +0.8 %, inside the ~2 % spread of
hardware notes §9.1. M11 and the hookless build were each run twice and read
within 0.1 both times; a second run of the tree before M11 was lost to the
Debug Probe dropping off USB. The control read 36,620 Hz in every row.

**At M12** the four workloads were run in one sitting on a Plus 2 W on
2026-09-26: the M11 tree as the control, M12 with the perf line off and on
(§13.1), and M12 at 2 MHz (§12.1). The overrides `PICO_ATOM_BOOT_CLOCK` and
`PICO_ATOM_BOOT_PERF` set the clock and the line over the settings file.
Host cycles per instruction, and at 2 MHz the share of core 0 and the
headroom, which is times real time:

| Workload | M11 | M12 | M12, perf line on | M12 at 2 MHz | 2 MHz: core 0 | 2 MHz: headroom |
|---|---:|---:|---:|---:|---:|---:|
| idle | 205.9 | 206.6 | 206.6 | 203.3 | 86.1 % | 1.16× |
| compute | 163.0 | 164.1 | 164.1 | 160.7 | 62.8 % | 1.58× |
| scroll | 224.6 | 224.8 | 224.8 | 221.2 | 84.3 % | 1.18× |
| bell | 147.0 | 146.9 | 146.9 | 144.6 | 76.8 % | 1.30× |

M12 is within 0.7 % of M11 on every workload, compute the most, inside the
~2 % spread: the clock's fields and bit 4's divide by a field cost nothing
measurable. The perf line costs core 0 nothing, since core 1 draws it, and
core 1's longest present did not move (11,622 µs off, 11,623 µs on), with no
snapshot dropped. At 2 MHz every workload ran with zero underruns, zero late
refills, zero dropped snapshots and zero I²C errors, at 1,999,300–2,000,700
guest cycles a second. The queue's low water was 385–513 samples, the same
as at 1 MHz. Idle is the heaviest, at 86 % of core 0, at the top of §12.1's
estimate of 63–87 %. The control read 36,620–36,621 Hz in every row.

**At M13** the four workloads ran at 4 MHz on a 300 MHz host (§3.2) on a
Plus 2 W on 2026-09-27, in a build with `PICO_ATOM_BOOT_HOST_MHZ=300`,
`PICO_ATOM_BOOT_CLOCK=4` and the perf line on:

| Workload | host cycles/insn | core 0 | headroom |
|---|---:|---:|---:|
| idle | 201.0 | 85.1 % | 1.17× |
| compute | 158.2 | 61.8 % | 1.61× |
| scroll | 221.2 | 84.3 % | 1.18× |
| bell | 142.9 | 75.9 % | 1.31× |

Host cycles per instruction are 0–1.6 % fewer than M12's at 2 MHz. That is
the hardware notes' §3 finding that CPU-bound code is purely clock-bound,
even with the flash held at 50 MHz. So 4 MHz
at 300 leaves core 0 the margin that 2 MHz at 150 does. Every workload ran with
zero underruns, zero late refills, zero dropped snapshots and zero I²C errors,
at 3,998,870–4,001,112 guest cycles a second. The queue's low water was 385
samples, the same as at 2 MHz. Core 1's longest present was 11,577–11,856 µs,
as at 150 MHz, because the SPI wire runs at 75 MHz either way. The control read
36,620–36,621 Hz in every row. The same firmware at 150 MHz and 1 MHz read
206.7 host cycles per instruction idle, against M12's 206.6.

### 6.4 Interrupts and reset

- `RES` — the Atom's **BREAK key is wired to reset**, so the UI maps a host key
  to `reset_pending` rather than synthesising anything cleverer. The line
  resets the 6522 and the 8271 with the CPU (`atom_reset`); the tape request
  goes too. The VIA's reset is inferred from the kernel, not read off the
  circuit diagram: the reset routine never writes the VIA, yet it `CLI`s at
  `#FF80` with IRQVEC just restored to `#A000`, an empty socket, and BASIC's
  BRK vector and line number (`#01`/`#02`) still the old program's. A T1
  interrupt left enabled across BREAK would run open bus into
  `ERROR n LINE <old line>`, and did before the VIA was reset.
- `IRQ` — level-sensitive, from the 6522 VIA (if fitted). Modelled as a bitmask
  so sources compose correctly; an `irq_lines` of zero deasserts.
- `NMI` — edge-triggered, latched. The 8271's INT drives it (§11.4): AtomDOS
  points `#0200`, the kernel's NMI vector (`#FFC7` is `PHA`, `JMP (#0200)`), at
  its handler `#E87B`, which moves one disc byte per NMI.

---

## 7. Bus and memory map

### 7.1 Fast path

A flat `uint8_t ram[65536]` backs the entire address space. Reads and writes
both go through a 256-entry page table, and for RAM and ROM each is a single
indexed load or store:

```c
typedef struct {
    uint8_t *read;    /* NULL → slow path */
    uint8_t *write;   /* NULL → not writable, or I/O */
} page_t;
```

The descriptor is exactly two pointers, 2 KiB for the table. The flags
(`PAGE_ROM`, `PAGE_IO`, `PAGE_VRAM`, `PAGE_OPEN`) are a separate byte array,
`atom_t.page_flags[]`, because only the slow path reads them and a third field
would pad the descriptor to twelve bytes.

Write dispatch:

```c
static inline void bus_write(atom_t *m, uint16_t a, uint8_t v) {
    page_t *p = &m->page[a >> 8];
    m->open_bus = v;
    if (__builtin_expect(p->write != NULL, 1)) {
        p->write[a & 0xFF] = v;           /* RAM, including VRAM */
    } else {
        bus_write_slow(m, a, v);          /* ROM (ignored) or I/O */
    }
}
```

VRAM needs no write hook: the snapshot-and-diff scheme of §8.4 discovers changes
at field end, which is cheaper than tracking every store and is immune to
under-marking.

Reads take the same shape through `read`. A page whose `read` is NULL, I/O or
unpopulated, takes the slow path.

**A page-granular fast path cannot express a sub-page device, and the Atom has
one.** With AtomDOS enabled the 8271 sits at `#0A00`–`#0A07` (§7.3), inside a
page that is otherwise RAM. If page `#0A` keeps a non-NULL `write`, those eight
addresses take the fast RAM store and the FDC is never reached — silently, with
the disc system simply not responding. So when AtomDOS is enabled, page `#0A` is
marked `PAGE_IO` with `read` and `write` both NULL, and `bus_write_slow` splits it:

```c
/* page #0A, AtomDOS enabled: 8 bytes of FDC, 248 bytes of ordinary RAM */
if ((a & 0xFFF8) == 0x0A00) i8271_write(&m->fdc, FDC_REG(a), v, now);
else                        m->ram[a] = v;
```

One page losing its fast path costs nothing measurable, and it is the only place
in the map where a device hides inside RAM. The same split applies to reads.

### 7.2 RAM population

Which blocks are populated is configuration. Default is a fully expanded
machine:

| Block | Range | Default |
|---|---|---|
| Block zero | `#0000`–`#03FF` | present |
| Text / user space | `#0400`–`#3FFF` | present |
| Upper RAM | `#4000`–`#7FFF` | present |
| Video | `#8000`–`#97FF` | present |
| Video aperture | `#9800`–`#9FFF` | absent |

Upper RAM was added at M8. Chuckie Egg's loader runs code at `#439C`,
and its notes ask for "32 KB RAM (#0000-#7FFF)". Without that block the
game loaded off tape with every checksum good, jumped into open bus and
fell back into BASIC as `ERROR 5`. Atomulator fits the same 32 KiB. A
snapshot records the block in its configuration byte, so one saved before
M8 is refused as another machine (§11.5).

**Power-on RAM is zero-filled** (`atom_init`); a real Atom's comes up holding
whatever its chips settled to, which no program can depend on. One program
state does depend on it: BASIC's `RND` is a 33-bit shift register in `#08`–`#0B`
and bit 0 of `#0C` (`#C986`), and the reset to the prompt leaves it alone. From
zero it stays at zero, so `RND` returned 0 for ever and ASTEROI put every rock
at one position, where their XOR images cancelled. `atom_seed_rnd` writes those
five bytes. The firmware seeds them from the board's `get_rand_64()` before
the first reset; the host tests seed `GUEST_RND_SEED`, so a run repeats.
`test_boot` checks that the seed survives the reset and drives `RND`, with a
zero seed as the control.

Unpopulated pages read as open bus, modelled as the last value on the bus
rather than `0xFF`: `atom_t.open_bus`, set by every read and write (§7.1).

### 7.3 I/O decoding

`#B000`–`#BFFF` is decoded by mask, not equality, because the Atom decodes it
partially and the 8255 mirrors every four bytes:

| Mask test | Device |
|---|---|
| `(a & 0xFC00) == 0xB000` | 8255, register `a & 3` |
| `(a & 0xFC00) == 0xB400` | expansion / printer |
| `(a & 0xFC00) == 0xB800` | 6522 VIA, register `a & 15` |
| `(a & 0xFFF8) == 0x0A00` | 8271 FDC, when AtomDOS is enabled (the default) |

The 8271 at `#0A00` sits inside RAM space, which is unusual and easy to get
wrong: with AtomDOS enabled, eight bytes of page `#0A` stop being RAM. The DOS
ROM touches four of them: `#0A00` (command and status), `#0A01` (parameter and
result), `#0A02` (reset, `#E000`) and `#0A04`, the data register, which its NMI
handler reads and writes (`#E84F`, `#E85A`). `#0A04` is the chip's DACK
input, not a fifth register. Taking A2 as DACK gives the eight bytes; that the
rest of the page is RAM is inferred, since the DOS uses none of it.

**The VIA is fitted by default** even though it was optional on the real
machine, because the MOS depends on it before the first prompt. It keeps the
printer-enabled flag in the VIA's PCR and reads `#B80C` on every character it
writes; if the CA2 bits (`& #0E`) are nonzero it waits on the printer's BUSY
line at `#B801` bit 7. With no chip there, those reads return open bus, which
is `#B8` in this model, and the MOS spins before it has printed `ACORN ATOM`.
Every reference emulator fits the VIA too. Port A's BUSY input reads ready,
since no printer is attached, so CTRL-B cannot hang the machine either.

### 7.4 The 6522 VIA

**The whole part, from M10** (`via6522.c`). Before M10 the ports, both timers
and the interrupt logic were modelled, and the shift register and control
lines were latched but did nothing. Nothing an Atom runs had needed more. The
Atom Software Archive's 5,610 files were searched for VIA writes: none enables
the shift register or a control-line interrupt, and only two utility ROMs write
the PCR. The 97 files that write ACR `#E0` are all Arcade Game Designer games.
They run T1 free as a 25 Hz frame clock (`#9C40` cycles) and poll IFR bit 6,
which the M4 model already did. M10 completes the part anyway, so that nothing
written for the chip meets a register that does not work:

| Part | Behaviour |
|---|---|
| Ports | PA reads its pins and PB its output latch for output bits. With ACR bits 0 or 1, each port reads what its pins held at the last active edge of CA1 or CB1. `#B80F` reads and writes port A without touching the flags or handshaking. |
| CA1, CB1 | Interrupt on the edge PCR bits 0 and 4 choose. CB1 is the shift clock's output while the chip makes that clock. |
| CA2, CB2 | Input on either edge, with an independent flag that a port access leaves set. Handshake output: low on a read or write of port A (only a write, for port B), and high again on CA1 or CB1. Pulse output: one cycle low per access. Held low or high. CB2 is the shift register's data line while it shifts. |
| T1 | One-shot or free-running, period latch + 2. With ACR bit 7 it drives PB7: low from the T1C-H write until a one-shot runs out, or a square wave when free-running. |
| T2 | One-shot on Φ2, or counting falling edges on PB6 (ACR bit 5), flagging when the count reaches zero. |
| Shift register | All eight modes: in or out, clocked by T2 (half-cycles of T2's low latch + 2), by Φ2 (a bit every two cycles), or by an external clock on CB1. Eight bits then flag SR, except free-running out, which recirculates without a flag. |

In the emulator nothing is wired to the control lines, no printer is
emulated, and port B, the user port, reaches the outside only when it is put
on the GPIOs (below). The outside world is therefore a set of calls
(`via6522_set_ca1()` and its kind, and `via6522_set_pb()` for the PB6 count)
and fields to read. `test_via6522` drives the part through them. Timing is to
the instruction, like the rest of the machine. The new state is in the snapshot (§11.5). A
free-running shift under Φ2 is the only mode that has to be ticked
cycle-exactly, and even then it is 16 steps a byte, out of line from the timers'
per-instruction path. That path counts T1 and a single countdown to T2's next
underflow or the shift clock's next edge, and nothing else (§6.3). So T2's
count and the shift clock lag between those events; a read of T2 or SR brings
them up to date, and so does the snapshot.

**Port B can go out to the PicoCalc's GPIOs** (`portb.c`, the Setup
page's *VIA port B*, since 2026-09-28). On a real Atom, port B was the user
port that owners wired up for their own projects: lights, relays, switches,
sensors. Each of PB0–PB7 may be given one of the pins the PicoCalc leaves free
(hardware notes §1.1): GP0–GP5, GP8, GP9, GP20, GP21 or GP28, or none, and no
pin twice. The defaults are PB0–PB5 on GP2, GP3, GP4, GP5, GP21 and GP28, the
side header's pins, with PB6 and PB7 not connected.

**Whether it is on by default depends on the build** (§14). The build that
ships, `-DPICO_ATOM_UART=OFF`, has no UART at all, so GP4 and GP5 are free and
port B is on. The development build, the default, keeps UART1 on GP4 and GP5
for the log and for keys typed over it (hardware notes §2.7), and port B is off.
If it is turned on there and holds GP4, the log does not reach the Debug Probe.
While it holds GP5 the UART's receiver is off, so what arrives on the pin is not
typed at the guest. The menu's status row names whichever is lost.

The pins behave as the 6522's do. A bit DDRB makes an output is driven high
or low from ORB, push-pull, and PB7 follows T1 when ACR bit 7 gives it the
pin. An input has the pad's pull-up, so with nothing attached it reads 1, as
the chip's port B does. The levels are 3.3 V, not the Atom's 5 V, at the
pad's default drive strength. A circuit built for the Atom's user port may
need a level shifter or a buffer.

The core knows nothing of GPIOs. `atom_t.port_b` is a hook, NULL unless the
port installs one. The bus calls it before a read of ORB, to put the pins'
levels on the chip through `via6522_set_pb()`. It calls it again after a write
to ORB, DDRB or ACR, to drive what `via6522_pb_out()` now says. Core 0 also
calls it once a field, for T1's PB7, which moves without a register write. The
pins are sampled only when ORB is read. So PB6 counts T2 pulses only at the
edges those reads catch, and ACR's port B latch takes the level of the last
read, not the level at CB1's edge; CB1 is not wired to a pin at all.
`test_bus` holds the hook to those registers and no others.

Verified on a Plus 2 W on 2026-09-28, in both builds:
- The *VIA port B* page behaves as intended, with the UART warning only in the
  development build.
- An LED on a port B output was switched on and off from BASIC.
- GP2 grounded read as PB0 low in BASIC.

---

## 8. Video: MC6847 → ST7789P

### 8.1 The decision: no framebuffer

The MC6847 produces a deterministic image from 6 KiB of VRAM and five mode bits.
There is no reason to keep a decoded copy of it. **The renderer is a band
generator** (hardware notes §4.10): it expands rows straight from the snapshot
into RGB565 line buffers and DMAs them.

| Model | SRAM | Verdict |
|---|---:|---|
| 8 bpp indexed 256×192 framebuffer + palette | 49 KiB | buys nothing — VRAM is already the source of truth |
| 16 bpp 256×192 framebuffer | 98 KiB | worse, for the same reason |
| **Snapshot + band generator** | **~34 KiB** | chosen |

The saving is real but the decisive argument is correctness: with no decoded
framebuffer there is no second copy of the screen to keep in sync, and a mode
change is a LUT rebuild rather than a full redecode.

### 8.2 Geometry

Native **256×192 at panel offset (32, 64)** — one of the two layouts the
hardware notes record as tested (§4.10). 1:1 pixels, no scaling artefacts, and
it leaves three useful regions:

```
 (0,0)                                    (319,0)
   ┌────────────────────────────────────────┐
   │        top band, 320×64 — idle         │
   ├────┬──────────────────────────────┬────┤ y=64
   │ 32 │    Atom display 256×192      │ 32 │
   ├────┴──────────────────────────────┴────┤ y=256
   │   status band, 320×64 — rarely drawn   │
   └────────────────────────────────────────┘ (319,319)
```

The bands are drawn once at startup and touched only when their contents change,
so they cost nothing per field — the point of hardware notes §4.7's "cost is per
pixel, not per present". As built, the top band is never drawn after
`lcd_init`, and the status band holds M11's status line (below). With the border on (§8.7)
their 48 rows nearest the rectangle are the border's. A nearest-neighbour
320×240 at (0,40) was planned as an option and is dropped: it would put 56 %
more pixels on the wire for a 1.25× stretch, with visibly uneven pixel
doubling. M12's perf line (§13.1) goes in the top band's outer 16 rows,
opposite the status line, drawn by the same code (`display_perf`).

**The status line, built at M11.** The status band is one line of text: 40
cells of the character ROM's 8×12, in the panel's bottom 16 rows, the cells
at y = 306–317 (`ATOM_STATUS_Y`). The border never fills those rows (§8.7), so
the line sits beside the border and takes nothing from it. It says what the
Atom's own screen cannot:

- the deck, when a UEF is in it: `PLAY`, `STOP`, `END` or `REC`, how far
  through the tape it is, the tape's name, and the turbo ratio while it runs
  unpaced (§11.3). While recording, the percentage is how much of the deck's
  64 KiB the tape takes. `FULL` replaces `REC` when that runs out, and
  `PROTECTED` says the last try to record was refused. Bytes the recorder
  could not frame follow as `3 BAD`;
- each drive whose head is loaded, by its image's name, at the right: `D0
  GAMES1` (§11.4). The tape's name is shortened to leave them room.

When there is nothing to say, the line is blank. Core 1 never reads guest
state while the 6502 runs (§4.2), so what the line needs travels in the
snapshot: six bytes, `atom_status_t` (`src/core/status.h`), with the deck's
state and position, the heads, the recorder's errors and the turbo ratio.
The names are core 1's already, from `tapeio` and `discio`. `status_format()`
makes the text, in the core so that `test_status` holds it, and the
presenter's `display_status()` draws it **only when its text changes**. A
change is 3,840 pixels, under 1 ms of wire (§8.4). Core 0 moves the tape's
position and the turbo ratio at most once a wall-clock second; a change of
state shows at once. The line is blanked while the menu is open, since the
menu can change what it says, and the first present after it closes draws it
again. A setting turns the line off (`status`, §11.7, and the
Setup page). It is drawn green on black in the screen's palette, so in
mono it is the screen's grey, and a change of palette redraws it.
On a Plus 2 W on 2026-09-26 the line showed a UEF's `PLAY`, `STOP` and
position, and `D0 …` during a disc access, which went at once on BREAK. Without a character ROM it
is not drawn. The perf summary §12.3 once meant for this band stays in the
heartbeat.

### 8.3 Row generation

One **8 KiB expansion LUT**, rebuilt whenever the mode or `CSS` nibble changes
(a few times per program run, not per field). It maps one VRAM byte directly to
its already-horizontally-stretched run of RGB565 pixels:

| Mode | Source byte covers | ×H | ×V | Screen px per byte | LUT entry |
|---|---:|---:|---:|---:|---:|
| CG1 | 4 px | 4 | 3 | 16 | 32 B |
| RG1 | 8 px | 2 | 3 | 16 | 32 B |
| CG2 | 4 px | 2 | 3 | 8 | 16 B |
| RG2 | 8 px | 2 | 2 | 16 | 32 B |
| CG3 | 4 px | 2 | 2 | 8 | 16 B |
| RG3 | 8 px | 2 | 1 | 16 | 32 B |
| CG6 | 4 px | 2 | 1 | 8 | 16 B |
| RG6 | 8 px | 1 | 1 | 8 | 16 B |

Worst case 256 × 32 = 8,192 bytes. A row is then a tight loop of 16- or 32-byte
copies — unrolled, not `memcpy`, because at that size the call *is* the cost
(hardware notes §9.4).

Vertical stretch is free: the ×V column above is a repeat count, and a repeated
row re-sends the **same line buffer** to the next window rows without
regenerating it. CG1 therefore generates 64 rows and transmits 192.

Alpha/SG6 mode takes a different generator — per character cell, select glyph
row from the character ROM or synthesise the SG6 element pattern, then expand
through the two-colour path with the cell's foreground and background. 32 cells
per row, 12 rows per cell, 16 cell rows.

### 8.4 Presentation and dirty tracking

Bands of **8 display rows** — 24 bands covering 256×192 — each with an inclusive
`[min..max]` byte-column span. Per field, core 1:

1. **Compares the snapshot's mode byte against the presented mode. If it
   differs, every band is marked fully dirty and the LUT is rebuilt.**
2. Otherwise diffs `snapshot[n]` against `shadow`, band by band, deriving the
   span.
3. For each dirty band: `lcd_blit_begin(32, 64 + band*8, span_w, 8)`, generate
   and DMA its rows, `lcd_blit_end()`.
4. Copies `snapshot[n]` into `shadow` **and the mode byte into the presented
   mode**.

Step 1 is not an optimisation, it is a correctness requirement: **a mode or
`CSS` change repaints all 49,152 pixels while leaving all 6 KiB of VRAM
byte-identical.** A VRAM-only diff finds nothing to do and leaves the previous
mode on screen indefinitely — `CLEAR` to a new mode without rewriting VRAM, or
flipping the colour set, would simply not appear. The mode byte is therefore
part of the shadow, not merely part of the snapshot.

Over-mark rather than under-mark; extra bands cost microseconds and a missed
band leaves a stale sprite on screen forever (hardware notes §4.10).

Cost estimate, from the 4.0 Mpx/s effective rate measured in hardware notes §4.7
(that figure includes an expanding pipeline, so it is the right one to use):

| Case | Pixels | Estimated wall time |
|---|---:|---:|
| Full 256×192 redraw | 49,152 | **~12.3 ms** + ~0.5 ms fixed |
| One text line changed (32×12 cells → 2 bands) | 4,096 | ~1.0 ms |
| Cursor blink (one cell) | 96 | ~0.05 ms |
| Full redraw at 25 MHz SPI, for comparison | 49,152 | ~37 ms |

**Measured at M3, 2026-09-22** — Pimoroni Pico Plus 2 W (RP2350 rev 2, id
`7458DC82A89AAC12`), `pico2` build, 150 MHz, spi1 configured at 75 MHz, with
core 0 running the guest's field loop throughout (the mode we ship). Each figure
is the mean of 20 presents; min-to-max spread was under 3 %.

| Case | Pixels | Estimate | Measured |
|---|---:|---:|---:|
| Control: `lcd_fill` of the same rectangle, no row generation | 49,152 | — | 11.48 ms |
| Full redraw, RG6 / CG1 / alpha | 49,152 | ~12.8 ms | **11.52 / 11.52 / 11.51 ms** |
| One text line changed (2 bands) | 4,096 | ~1.0 ms | 1.11 ms |
| One cell changed (2 bands × 8 rows × 8 px) | 128 | ~0.05 ms | 0.28 ms |
| Nothing changed (live, `@` screen) | 0 | — | 0.11 ms |

What the numbers say:

- **Row generation is free.** A full redraw costs the same as the control fill
  to within 0.4 %, in every mode, so expansion is entirely hidden behind the
  DMA. The present is wire-bound, as §8.1 intended, at 4.27 Mpx/s — better than
  the 4.0 Mpx/s the estimate borrowed from a pipeline that did more per pixel.
- **The estimate had no fixed cost for a small present.** ~0.1 ms goes on the
  24-band diff and the 6 KiB shadow copy whether anything changed or not, and
  each dirty band adds a window and eight row DMAs. That is why one cell costs
  0.28 ms, not 0.05; it is still under 2 % of a field.
- A full-screen redraw every field leaves ~5.2 ms of the 16.7 ms field for
  everything else on core 1 — enough for one southbridge poll, which is why the
  poll moves to every second field (below) rather than the redraw rate dropping.

**This was expected to be the machine's real constraint.** A field is 16.7 ms;
a full-screen redraw (11.5 ms, measured above) plus a 4–5 ms southbridge poll
is 15.5–16.5 ms, so core 1 has almost nothing left if the whole screen changes
every field. The mitigations planned
were these, and this is how each was built:

- Poll the keyboard at **30 Hz**. Still well inside the southbridge's 2.5 s
  bus-reset timeout (§6.1), and it halves the fixed I²C cost. **Built**, on a
  33 ms timer in core 1's loop rather than every second field.
- Drop to **30 Hz presentation** when sustained full-screen change is detected
  (hardware notes §9.6, lever 2). **Not built as a mode of its own.** §4.2's
  pool already does it: a snapshot published while core 1 is still presenting
  replaces the one waiting, and the heartbeat counts it as `dropped`. The 6502
  keeps running at 60 fields/s whatever core 1 does.
- Never resend the top or status bands. **Built**: nothing redraws them.

**Measured at M10**, in the perf pass's scroll workload (§6.3), a `PRINT` loop
scrolling Atom BASIC for 55 s: 3,300 fields, 3,300 presents, **0 dropped**,
and one full redraw, at boot; the presents sampled at heartbeats took
0.1–5.5 ms. Scrolling, the case this was designed for, does not saturate core 1. A
program that rewrites a whole graphics screen every field has not been
measured; the `dropped` count is where it would show.

### 8.5 Tearing

There is no TE line (§1.2). Full-screen alternating patterns tear, as the
hardware notes observed directly. The mitigations available are ordering ones,
and the band renderer already applies the useful one: presents are small and
localised, so a tear affects one 8-row band rather than the whole frame. Accept
it. Hardware vertical scroll (§4.8) is deliberately **not** used — it has one
scroll register, the Atom's own scrolling is a memory move that the diff already
catches cheaply, and the y-remap-in-every-blit-path hazard is not worth the
milliseconds here.

### 8.6 Snow

On real hardware the 6502 and the VDG contend for video RAM, so CPU writes
during active display produce visible interference; Atom programs wait for the
`FS` flag in port C bit 7 to avoid it. The emulator models `FS` correctly, so
well-behaved software behaves. **Reproducing the interference is dropped.** It
was planned as an option, off by default. It is authentic, but it is also ugly,
and it would need a per-cycle beam position that the band renderer
(§8.3) does not have.

### 8.7 Monochrome and the border

**Built at M10**, as two settings on the menu's Display page (§13). Both
are on by default (§11.7), as the stock machine most owners had. A third,
the dark background, came after M10 and is off by default.

**Monochrome.** Most Atoms were sold without the colour board, and the stock
machine shows only the VDG's luminance output. The mono palette is the
datasheet's Y levels with no chroma, read off its figure 10 (the video and
chrominance waveforms), at the DC characteristics' typical voltages. Lower
voltage is brighter: 0.72 V for black; 0.65 V (white low) for blue and red;
0.54 V (white medium) for green, cyan, magenta and orange; 0.42 V (white
high) for yellow and buff. They are scaled so black is black and 0.42 V is
white, which gives black and three greys. So on a mono Atom blue and red are
one dark grey, and CG modes with CSS 0 still show four levels. The renderer takes the palette as a pointer, so the
switch rebuilds the LUT once and costs nothing per pixel.

**The border.** The VDG draws a border around its 256×192 active area: black
in the alphanumeric and semigraphics modes, and green or buff (by CSS) in
the graphics modes (`mc6847_border()`). With the setting on, the presenter
draws it as a frame around the Atom rectangle, `ATOM_BORDER_X` (32) wide at
the sides and `ATOM_BORDER_Y` (48) tall above and below: the side strips of
§8.2, and the 48 rows of the top and status bands next to the rectangle. The
panel's outer 16 rows, top and bottom, stay black. That is 43,008 pixels,
seven eighths of the rectangle, so it is filled **only when its colour changes**: a mode
change between text and graphics, or of CSS in graphics. It is not filled on
every full redraw. With the setting off, the border is never filled, and
the panel around the rectangle stays black from `lcd_init`, as before M10.
M11's status line (§8.2) is in the outer 16 rows at the bottom, so the
border keeps its 48.

**The dark background.** The VDG does not put text on black. Its datasheet
(Display Modes, p. 18) says characters "may be either green on a dark green
background or orange on a dark orange background, depending on the state of
the CSS pin". It gives no level for either, and does not say why.

The likely reason is in how the chip makes colour. It puts out luminance
(Y) and two colour-difference signals (φA, φB), not RGB, and a colour is a
combination of their levels (figure 10). An alphanumeric dot is half a cycle
of the 3.58 MHz subcarrier, far finer than a TV's chroma bandwidth, so the
chip holds φA and φB at the cell's colour, green or orange by CSS, and
switches only Y: the ink's level for a dot of the character and black's for
the rest. The dots around the character are black's luminance carrying the
ink's chroma, which a TV shows as a murky dark green, and inverse video
gives dark characters on bright green. The border is different: figure 10
has it, in the alphanumeric modes, at black with φA and φB at their
reference level, no chroma at all, and it stays black with the setting on.
This is an explanation from the chip's outputs; the datasheet does not
state it.

It follows that the level was never the chip's to decide. How dark "black
plus green's chroma" looks was the modulator's, the TV's, or on the Atom
the colour board's. The values here are a quarter of the ink, `#004000` and
`#402000`: a rendering choice, like the primaries, and nothing yet says how
dark a real Atom's was. It also follows that **a mono Atom showed true
black**, since it has Y alone, so the mono palette's dark colours are black
and the setting does nothing in mono.

With the setting on, an alphanumeric cell's background is `VDG_DARK_GREEN`
or `VDG_DARK_ORANGE`, and an inverse cell's ink is too. Semigraphics cells
keep black for the elements that are off. Only alpha cells use it, and they
are drawn per cell without the LUT, so switching it rebuilds nothing.

---

## 9. Audio

### 9.1 What the Atom produces

One bit: port C bit 2, toggled by the CPU in a delay loop. Everything the Atom
can play is a square wave whose frequency is a function of how fast the MOS
loops. There is no PSG and no envelope, which makes this the easiest part of the
machine to emulate well — and the easiest to emulate badly, because naïve
sampling of a 1-bit signal aliases audibly.

### 9.2 Output stage

Per hardware notes §5.1–5.2, at `clk_sys` = 150 MHz, divider 1, `TOP` = 2047:

| | |
|---|---:|
| Carrier | 73.2 kHz, ultrasonic, 11 bits |
| Oversample | 2 (each frame written twice) |
| Sample rate | **150,000,000 / 4096** ≈ 36.62 kHz |
| Guest cycles per sample | 1,000,000 / 36,621 ≈ **27.31** |

The rational cadence is kept as an exact ratio, not as the truncated 36,621, and
it is recomputed from `clock_get_hz(clk_sys)` at init rather than baked in
(hardware notes §5.2).

### 9.3 From one bit to PCM

**Integrate, do not sample.** The guest toggles the speaker at arbitrary cycle
boundaries; a sample is the *time average* of the speaker level over the 27.31
guest cycles it covers.

```
on every write that changes PC2:
    acc += level * (cycle_now - cycle_last_change)
    level = new_level; cycle_last_change = cycle_now
at each sample boundary:
    acc += level * (boundary - cycle_last_change)
    sample = acc / cycles_per_sample        /* fixed point */
    acc = 0; cycle_last_change = boundary
```

This is a box filter at exactly the sample period. It removes the aliasing that
makes point-sampled beeper emulation sound wrong, it costs an add and a multiply
per *toggle* rather than per sample, and it is exact for square waves of any
frequency — which is all the Atom produces. Fixed point throughout; no `double`
literals anywhere near it (hardware notes §2.2).

Cassette bits (port C bits 0–1) are excluded from the mix by default and
available as a monitor option, because hearing your loads is charming for about
ninety seconds. The monitor option has not been built: tape arrived at M6
and M8 without it, and while a UEF plays the guest runs in turbo, silent
(§11.3).

As built, in `src/core/beeper.c`:

- **The sample boundary is tracked in units of 1/den of a guest cycle**, where
  guest cycles per sample is the reduced fraction `num/den` — 2048/75 at
  150 MHz. A boundary is a whole cycle count plus a remainder; advancing by one
  sample adds `q` and `r` with no division. There is no accumulated rounding, so
  the count of samples after any run is exactly `⌊cycles × den / num⌋`.
- **An edge is stamped with the cycle count at the start of the writing
  instruction.** The store lands on its last cycle, a few cycles later, but the
  offset is the same for every edge a given loop makes, so periods — and pitch
  — are exact and only the phase moves.
- **A one-pole DC blocker** (pole 0.995, a corner near 29 Hz) follows the box
  filter, so a speaker left high and one left low both come to rest at 0. That
  makes 0 the silence value, which is what the port pads an underrun with.
- The core holds up to `ATOM_AUDIO_BUF_LEN` (1024) samples between drains; one
  field is 611 at 60 Hz. `atom_run` closes off every sample that ended before its
  last instruction, once per call.

`test/host/test_audio.c` checks every sample of a guest-executed square wave
against an independent box-filter model built from the edges the guest actually
made (to 1 LSB), and measures the pitch of the DC-blocked output against the
loop's hand-counted period: 1002.004 Hz both ways. `test_boot` rings the real
MOS bell with CTRL-G. The kernel's bell at `#FD18` toggles PC2 through the
8255's BSR path, 1290 cycles each half period, for 123 half periods, so
387.60 Hz for 157 ms by the cycle count. The output measures 387.64 Hz over
156 ms.

### 9.4 Plumbing

Straight from hardware notes §5.3–5.4, with no deviation:

- Two DMA channels **chained to each other**, ping-ponging a two-half ring,
  paced by `pwm_get_dreq(slice)`, writing the 32-bit `left | (right << 16)`
  compare word. 128 frames per half → refill every ~3.5 ms. **At oversample 2
  each frame occupies two slots**, so a half is 256 slots and the ring is
  2,048 bytes, not 1,024 — the arithmetic the hardware notes spell out in §5.3.
- The ring is **power-of-two sized and aligned**, with
  `channel_config_set_ring()` on the read address. This is the safety net for
  the interrupt hole that flash program/erase opens (§7.2), which no IRQ design
  avoids.
- On re-arm from the IRQ, reset **both** the read address and the transfer
  count.
- `irq_set_priority(DMA_IRQ_0, 0x40)`, above the default, so the refill preempts
  the 4–5 ms southbridge transaction on core 1.
- Every function on the refill path is `__not_in_flash_func`.

Between core 0's integrator and the DMA IRQ sits a **1024-sample SPSC queue**
(~28 ms), streaming starting once 768 samples are buffered. Mono is duplicated
to both channels.

**Two counters, not one** (hardware notes §5.8): PCM producer underruns and late
DMA refills are separate failure modes with separate causes, and a single
counter hides whichever one is not happening. When muted, samples are still
consumed so that muting does not change application timing.

As built, in `src/port/audio.c`: the queue holds finished compare words, so the
IRQ only copies, and the producer converts at push time. The producer and the
IRQ are both on core 0, so the queue is SPSC between thread and interrupt
context and needs no lock. A refill is **late** when its channel is already
running again on entry. That means the other half drained too and chained
back. A late refill is counted and the live channel is left alone. Un-re-armed, it
replays the other half, and the ring wrap keeps it inside the buffer until its
next completion re-arms it. Nothing comes off the queue on a late refill. That
next completion refills the half, so samples taken now would be overwritten
before they played. Left in the queue, they are only late. Playback
starts when the producer's push takes the queue to 768 samples.

**The UART is not on core 0's path.** A heartbeat is a few hundred bytes, and
115200 baud takes ~30 ms to send it — more than the queue's ~11 ms of low-water
slack — so a blocking `printf` from the field loop would cause the underruns it
reports. Core 0 formats into a ring (`src/port/log.c`) and core 1 moves bytes
into the UART FIFO as it has room.

**Measured** on a Plus 2 W, 2026-09-22 (`out/` logs, not committed). The guest
ran `10 P.$7;` / `20 G.10`, a continuous MOS bell typed in over UART1
(`tools/uart-type.sh`), for 690 s of 5-second heartbeats:

| | |
|---|---:|
| Speaker edges | 507,608 — the bell rang throughout |
| PCM underrun samples | **0** |
| Late DMA refills | **0** |
| Samples consumed per second, against `time_us_64()` | 36,620–36,621 (nominal 36,621.09; the log truncates) |
| Real-time ratio | 0.999–1.000 |
| Queue low water | 385 samples, ~10.5 ms |
| I²C errors, dropped presents, dropped log lines | 0, 0, 0 |

The consumed rate is the control quantity: it is the PWM wrap measured against
an independent timer, and it did not move with the guest's load. By ear, the
MOS bell (`PRINT $7`) is audible from the PicoCalc's speaker. An uncalibrated
phone app read it as ~387 Hz, against 387.60 Hz from the cycle count. That
only confirms the pitch, like hardware notes §5.8's 439 Hz reading. It is not
a calibrated measurement.

---

## 10. Keyboard

### 10.1 The impedance mismatch

The Atom wants a **10×6 matrix scanned at guest speed**, with `SHIFT`, `CTRL`
and `REPT` as separate lines. The PicoCalc gives **translated ASCII events over
a 10 kHz I²C bus**, with the MCU having already resolved shift — and register
`CFG`, which is where you would ask for raw keys, is not in the firmware's
dispatch (hardware notes §6). So the mapping is a reverse translation:

```
PicoCalc event  →  normalise  →  held-key bitmap  →  ASCII→(row,col,shift)  →  Atom matrix
 [state,code]      (§6.2)        256 bits             table                    10×6 + mods
```

### 10.2 Rules, all of them from measured southbridge behaviour

- **Build a held-key bitmap from press/release events** and drive the matrix
  from that. Never consume a character stream: a frame loop reading one
  character per field falls behind the 100 ms repeat and keys stick (§6.2).
- **Auto-repeat arrives as additional `pressed` events**, so a `pressed` event
  is not a fresh down edge. Require a release before re-arming anything
  edge-triggered.
- **Normalise the shifted-release quirk.** The MCU retranslates at each
  transition, so releasing Shift first yields press `A` / release `a`, or press
  `!` / release `1`. Canonicalise letters and shifted punctuation for held-state
  identity while keeping the translated code for anything that wants text.
- **Drain the whole FIFO each poll**, up to its 31 entries, not one event.
- **Guard the bus with an atomic flag** so a battery read and a key poll cannot
  interleave.
- **Poll from core 1's loop at 30 Hz**, in thread context, never from a timer
  IRQ — that specific mistake is hardware notes §5.7's third clicking bug.

### 10.3 Mapping

The Atom matrix is 10 columns × 6 rows plus three standalone lines. The mapping
table is data, `src/core/keymap_picocalc.c`, with one row per PicoCalc keycode:

```c
typedef struct { uint8_t code; uint8_t row, col; uint8_t flags; } keymap_t;
/* flags: KM_SHIFT — assert the Atom SHIFT line with this cell
          KM_CTRL  — assert the Atom CTRL line
          KM_ALT   — the entry is on the Alt layer
          KM_REPT, KM_BREAK, KM_MENU — no cell: REPT, the reset line,
                     the emulator menu
          KM_LINE  — no cell: KM_SHIFT or KM_CTRL alone (§10.5)      */
```

Keys the Atom has and the PicoCalc does not get a **`Alt` layer**. `Alt` is
chosen because the Atom has no Alt key, so nothing is stolen from the guest:

| Atom key | PicoCalc binding | Why |
|---|---|---|
| `COPY` | `Alt`+`C` | no PicoCalc equivalent |
| `REPT` | `Tab` (and `Shift`+`Tab`, which arrives as `Home`) | separate 8255 line, port C bit 6. A modifier on the Atom — held while another key is held — so it cannot be an `Alt` chord: a key pressed with `Alt` down is taken from the `Alt` layer |
| `BREAK` (reset) | `Alt`+`K` | destructive; must not be a single keypress |
| `LOCK` | `Alt`+`L` | |
| `@` `[` `\` `]` `^` | direct where present, `Alt` layer otherwise | confirm against the installed keymap |
| Emulator menu | `Alt`+`M` | §13 |
| Pause | `Alt`+`P` | §13.1, built at M12; `KM_PAUSE`, which asks for the park as `KM_MENU` does |

**`Shift` on its own is the Atom's `SHIFT` line.** Games read port B bit 7 by
itself (Hard Hat Harry jumps on it), so the host's `Shift` asserts the line
while it is down, whatever else is held. The one exception is a character from
the standard map that the MCU sent shifted and the Atom types unshifted, such as
`:`: while that key is down, `SHIFT` stays up. A layout's cell (§10.5) takes the
host's `Shift` as it finds it, as it does `CTRL`.

**The shifted `@` `[` `\` `]` `^`, added at M12.** The MCU
sends `Shift`+`\` as `|`, and until M12 the map had no entry for `|`, so
nothing was typed. Atom BASIC needs it: `|` (`#7C`) is its OR. The operator table at
`#C1DC` reads `- + | :` and then `* / % ! ? &`, so `:` is EOR and `&` is
AND. On the Atom, `|` is `SHIFT`+`\`, and the VDG shows it as an inverse
`\`. M12 maps the characters the MCU sends, by what they are, onto the
Atom's shifted cells: `|` to `SHIFT`+`\`, `{` and `}` to `SHIFT`+`[` and
`]`, `` ` `` to `SHIFT`+`@`, and `~` to `SHIFT`+`^`. This was checked
against the real MOS on 2026-09-26, before it was written here. With those
five entries added, `test_boot`'s sweep of every printing entry passed, and
`PRINT 5|3` typed through the MOS answered 7 (`PRINT 6:3` answered 5). The
entries were then taken out again, for M12 to add, and M12 put them back:
`test_boot` holds both answers, at 1 MHz and at 2.

Three constraints from hardware notes §6.3 bind this table and must be checked
before it is written:

- **`Shift`+`Left`, `Shift`+`Right`, `Shift`+`Space` and `Shift`+`Backspace` do
  not exist.** The MCU replaces a shifted non-letter with its alternate, and
  where the alternate is zero it sends nothing at all. Do not bind them.
- **`Alt`+`,`, `Alt`+`.`, `Alt`+`Space` and `Alt`+`B` are consumed by the MCU**
  for the backlights and battery display. Keep them out of the Alt layer.
- Several keys exist only as shifted alternates: `Home` = `Shift`+`Tab`,
  `End` = `Shift`+`Del`, `PgUp`/`PgDn` = `Shift`+`Up`/`Down`,
  `Break` = `Shift`+`Esc`, `Insert` = `Shift`+`Enter`.

### 10.4 Latency

Worst case is one field of event latency plus one 30 Hz poll interval plus the
4–5 ms transaction: ~55 ms. That is fine for BASIC and acceptable for the Atom's
games, which were written against a matrix scanned by a MOS that itself
debounced across fields. If a specific title feels wrong, the lever is polling
every field (60 Hz) at the cost of 4–5 ms of core 1 per field, and §8.4's budget
says whether that is affordable.

### 10.5 Game keymaps

**Built at M6b**, passed on the host and played on the device (below). The
standard map (§10.3) is right for typing and wrong for play. Atom games do not read characters. They scan the matrix and the
standalone lines directly, and many were laid out for the Atom's own keyboard,
where the keys a game used sat under one hand. Galaxians says so on its title
page: `CTRL` to move right, "the adjacent cursor control" (the `↑↓` key,
row 0 column 2) to move left, and `REPT` to fire. At least one other title
from the same disc uses the same scheme. Through the standard map that is
`Down` for left, `Ctrl` for right and `Tab` for fire. The keys are scattered
across the PicoCalc and do not match the directions they stand for.

**A game keymap is an overlay on the standard map, chosen in the menu.** It
rebinds a handful of PicoCalc keys to **Atom targets**, and every key it does
not mention keeps its standard binding, so `RUN`, `LOAD ""` and the game's own
prompts still type. A target is one of:

- a matrix cell, named by the Atom key (`A`–`Z`, `0`–`9`, `SPACE`, `RETURN`,
  `UPDOWN`, `LEFTRIGHT`, `COPY`, …), asserted **without** SHIFT, because a game
  scanning the matrix sees the cell, not a character; or
- one of the standalone lines: `CTRL`, `SHIFT` or `REPT`.

The first built-in layout is **Games**, for Galaxians and titles like it.
It names no game: the user chooses it in the menu.

| PicoCalc | Atom | Galaxians' meaning |
|---|---|---|
| `Left` | `UPDOWN` cell | move left |
| `Right` | `CTRL` line | move right |
| `]` | `REPT` line | fire |

The directions sit on the direction keys and fire is at the far side of the
keyboard, one hand each. The first cut put fire on `Up`, under the same thumb as
the directions, and on the device that played worse. A Shift was the next
thought and cannot work. While either Shift is down the MCU sends nothing at all
for `Left` or `Right` (hardware notes §6.3): no press, no repeat, and no
release, so the player could not move while firing, and a direction let go
under Shift would stay held. Any key a layout uses for fire must be one whose
chord with the direction keys the MCU delivers.

**Mechanism.** `keymap_t` gains the `KM_CTRL` flag §10.3 sketched, which
asserts the CTRL line (OR-ed with the host's own `Ctrl`) in the same way
`KM_SHIFT` asserts SHIFT, and `KM_LINE`, which marks a target that is a line
and no cell. A layout (`keylayout_t`, `keymatrix.h`) binds **canonical** codes,
so a binding holds whichever way the MCU translated the key. `lookup()` in
`keymatrix.c` consults the layout, then the plain table. With `Alt` down it
consults the Alt layer only, so no layout can take away the menu, `BREAK` or
`COPY`. The binding is chosen once, at press time, and the held set keeps a
copy of it, so a key's release undoes what its press did even if the layout
changed in between. That can happen: a tape load (below) changes the layout
while core 0 is parked, and core 0 does not empty its held set after a tape
call, as it does after the menu (§13). Core 1 writes the choice only while
core 0 is parked, and core 0 applies it with `keymatrix_set_layout()` once it
has the machine back, so there is no cross-core race. Capacities — layouts,
bindings and tape names per layout, and the size of a file — are fixed in
`config.h` (§5).

**Where layouts come from.** Built-in layouts are data in `src/core/`, alongside
the standard map. Further layouts are text files in `/atom/keymaps/` on the card,
read when the menu opens, one binding per line:

```
# Galaxians: move on the arrows, fire on ]
name  = GALAXIANS
left  = UPDOWN
right = CTRL
]     = REPT
tapes = GALAXI
```

That is the built-in Games as a file, under its own name (a card layout may
not reuse a built-in's), plus a `tapes` line the built-in does not have. A
line whose first character is `#` is a comment, unless it binds the `#` key
itself (`# = SPACE`).

The optional `tapes` line names the ATM header names the layout goes with,
compared ignoring case, since the file is typed by hand. Loading one of those
tapes (§11.2) selects the layout, and the menu shows that it did
(`KEYS CHOSEN BY GALAXI`). Only the user's files carry a `tapes` line; a
built-in layout names no game, because the user knows which titles a layout
suits and the firmware does not. The user can always override the choice in
the menu. Loading a tape that no layout names leaves the choice alone, because a game's loader may fetch its next part under
another name. So an ordinary program never has a layout forced on it, but it
may inherit one the user or an earlier tape chose. The parser (`keylayout.c`,
in the core, tested on the host) rejects a line it does not understand, and
`keymapio.c` leaves that file out and names it and the line on the menu's
status row, rather than guessing. The card's layouts are read at boot, so the
first tape load can choose one, and again when the menu opens.

A key on the left of a binding is a PicoCalc key by name (`left`, `right`,
`up`, `down`, `space`, `enter`, `backspace`, `tab`, `del`, `esc`) or any single
printable character, which names the key it is on, shifted or not. A target is
an Atom key by its keycap name (`BREAK` excepted: it is the reset line, and not
a game's to have) or `CTRL`, `SHIFT` or `REPT`.

**In the menu** the item is `KEYS < STANDARD >` on the Setup page
(§13), and left/right cycles through
the built-in layouts, then the card's. The choice is part of the settings, and
M11's *Save settings* writes it to the settings file (§11.6). It is not part of a snapshot (§11.5): a snapshot
is guest state, and a layout is a fact about the host's keyboard.

**On the device.** On a Plus 2 W on 2026-09-23 `LOAD "GALAXI"` over the UART
chose the layout (`keymaps : loading GALAXI chose "CURSOR GAMES"` in the log),
when the built-in still named the tape; it no longer does. Galaxians was then
played with the layout on the keyboard, moving and firing at once, with fire on
`Up`; that run is what moved fire to `]` (above). Bouncing Babies was played
with a layout from a card file, chosen by loading its tape:

```
# Bouncing Babies: SHIFT moves left, REPT moves right
name  = BABIES
left  = SHIFT
right = REPT
tapes = BABIES
```

Its own source reads the keys: left is `?#B001=127`, SHIFT with nothing else in
port B, and right is port C bit 6, REPT. The Atom cannot tell its two SHIFT
keys apart, and the game does not try.

**Still to watch on the device:**

- **Rollover.** Moving and firing at once worked with a direction and `Up`
  held together. `]` sits on another part of the matrix under the MCU, and has
  not been checked for ghosting against the arrows.
- **Key repeat.** A held arrow sends further presses every ~100 ms (hardware
  notes §6.2). The held set absorbs them (§10.2), and nothing showed in play.
- **Pacing.** Replay pacing (`ATOM_KEY_MIN_FIELDS` + `ATOM_KEY_GAP_FIELDS`,
  eight fields) was tuned for the MOS's typing loop (`config.h`). A held
  direction is unaffected. A quick tap of fire becomes at least six fields of
  `REPT`, and a second tap waits two more. If a game feels sluggish, the lever is
  per-layout pacing, measured against the game rather than guessed.

**Tests.** `test_keymap` checks the overlay without a ROM. `Left` under Games
must drive cell (0,2) with SHIFT up, `Right` must drive the CTRL line,
`]` must drive REPT, and an unmentioned key must keep its standard cell. The
held-set tests must also pass with an overlay active. `test_boot` checks it
through the real MOS, with a BASIC loop that reads port B and port C (`?#B001`,
`?#B002`) while the test holds each key, under Games and under the Bouncing
Babies file above, where `Left` must read exactly `127` in port B.

---

## 11. Storage, tape and disc

### 11.1 SD card layout

```
/atom/
  roms/       akernel.rom  abasic.rom  afloat.rom  dosrom.rom  utility.rom
  tapes/      *.atm  *.uef        a .uef may be gzipped (§11.3)
  discs/      *.ssd  *.dsk  *.40t  *.dsd
  snaps/      slot1.psnap … slot4.psnap
  keymaps/    *.map        game keymaps (§10.5)
  pico-atom.cfg
```

ROMs are **not shipped** (§1). If `/atom/roms/` is missing or incomplete, the
emulator shows a text page saying which files are missing (`roms.c`) rather
than booting into a dead machine — a blank screen is the single most expensive
failure mode to debug on this hardware.

The four names above that are not `utility.rom` are Atomulator's, which is the
set most Atom software assumes and the one [`README.md`](../README.md#rom-images)
points a user at. They map onto §2.2 as:

| File | Guest address | SHA-1 of the Atomulator image |
|---|---|---|
| `akernel.rom` | `#F000`–`#FFFF` | `2621f27d652d4673e0a79aa669e729b8c3051ab6` |
| `abasic.rom` | `#C000`–`#CFFF` | `a8ea19f10d4c98fbc1b666e5968f06d46af9a84c` |
| `afloat.rom` | `#D000`–`#DFFF` | `ebcde5b36cb3a3344567cbba4c7b9fde015f4802` |
| `dosrom.rom` | `#E000`–`#EFFF` | `71ea0a4b8d9c3caf9718fc7cc279f4306a23b39c` |
| `utility.rom` | `#A000`–`#AFFF` | user's choice; `axr1.rom` is the usual one |

Each is 4 KiB. **`abasic.rom` and `akernel.rom` are the two halves of MAME's
8 KiB `abasic.ic20`**, in that order — a collection that offers one 8 KiB file
rather than two 4 KiB ones has the same data, split differently, and the loader
should say so rather than reject it. Recording the hashes here is not DRM: Atom
images have been re-dumped and renamed for forty years, and a near-miss
produces a machine that boots and then misbehaves, which §16 already calls the
most expensive class of bug on this project.

SD access follows hardware notes §7.1: 400 kHz for init then 25 MHz requested,
card detect on GP22 active-low with a pull-up, 512-byte blocks, SDSC byte
addressing distinguished from SDHC block addressing, and **all card work done at
a defined application boundary** — the menu, a tape call or a disc request,
each with core 0 parked at a field boundary and feeding the audio queue
silence — because write latency can exceed both the field and the audio
deadline.

### 11.2 Tape, phase 1: OS-level trapping

Fast and simple: stop the CPU when it enters the MOS's load or save routine,
serve the request from a `.atm` file on SD, leave the machine as the routine
would have, and `RTS`. ATM is the natural container — a 16-byte name, then load
address, execution address and length, little-endian, then the data — and it is
how most Atom software is archived.

**The entry points, read off the kernel ROM.** `OSLOAD` (`#FFE0`) is
`JMP (#020C)` and `OSSAVE` (`#FFDD`) is `JMP (#020E)`, and reset copies a table
from `#FF9A` that points those vectors at `#F96E` and `#FAE5`. Both handlers
take `X` pointing at a parameter block in page zero and begin by copying ten
bytes of it to `#C9`–`#D2` (`#F84F`): the name's address, then for a load the
address and a flag byte whose bit 7 set means "at this address, not the file's";
for a save the reload, execution, start and end addresses. A name is at most 13
characters and a CR, or the MOS's `NAME` error.

**The trap is on the handlers, not the `#FFxx` entries.** That is the page-2
variant for free: anything that repoints `#020C` or `#020E` — AtomDOS, a utility
ROM, a game's own loader — never reaches `#F96E`, so it is never trapped. The
trap also checks the first eight bytes of each handler against the stock
kernel's and stands aside for a MOS that differs, because what it reproduces is
that kernel's page-zero contract.

**The CPU stalls; it does not return early.** At a trapped boundary the 6502
behaves as if RDY were held low: guest time passes, the VIA ticks, audio keeps
its cadence, no instruction runs. `atom_run` keeps its contract (§4.1) and the
port serves the request whenever it gets to it — on the device, core 0 parks at
the field boundary and core 1 does the card work (§4.2, §11.1). The wall time
spent parked is not guest time, as with the menu (§13): to the guest the stall
runs to the end of the field it began in. No program can count on the length
of a tape call, which on the real machine is seconds and varies with the tape,
so core 1 advancing the clock while it holds the machine would buy nothing but
a second writer of the cycle count. A request no
file answers to is **declined**, and the ROM routine then runs as though there
were no trap: it prints `PLAY TAPE` and waits on the cassette input, and the
user escapes as on the real machine. BREAK cancels an outstanding request.

**What the trap leaves behind is the ROM's, byte for byte.** `*RUN` jumps
through `#D6` after the load returns; BASIC re-enters at `#CD9B`; a game's
loader may look at anything. So the trap writes what the named-file path
(`#F97A`) or the record loop (`#FAF8`) leaves: the parameter block with its
advanced pointers, the last block's header in `#D4`–`#DB`, the checksum in
`#DC`, the mode bits in `#DD`, the name in `#ED`, port C, and `A`, `X` and `Y`.
Two things about the Atom's own block format are needed for that and are easy
to get wrong. The checksum covers the framing — `****`, the name and CR, and the
header — as well as the data, and the load adds the checksum byte to itself, so
`#DC` ends at twice the sum. The flags byte is built by rotating into `#D2`,
which starts as the high byte of the end address: bit 7 "another block
follows", bit 6 "this block has data", bit 5 "not the first block" — because
the loop re-enters past its `CLC` with the carry still set — and bits 4–0
whatever was rotated out of the old value. Those five bits mean nothing but are
recorded, summed and left in `#DB`, so they are reproduced, taking the file to
have been saved from where it loads, which is all an ATM header can say.

`test_tape` holds all of this to the ROM itself. It runs the kernel's own
`OSSAVE` and `OSLOAD` with the byte-level cassette routines (`#FC7C`, `#FBEE`),
the leader-tone wait (`#FB8E`) and the `PLAY TAPE` keypress hooked, captures the
bytes the ROM would have recorded, reads them back, and requires the trapped
calls to leave the same machine — every byte below `#A000`, the registers, the
flags, the stack and port C — across block boundaries and both address modes.
Only the bit-level scratch at `#C0`–`#C5` and `#EC` is exempt, because the trap
has no bits to put there.

On the card the files live in `/atom/tapes/`. A load finds its file by the name
in the ATM header, compared byte for byte as the MOS compares names, and failing
that by the file name without `.atm`, ignoring case. A save writes
`<name>.atm`, or rewrites whichever file already answers to that name, through
a temporary file. The ROM's nameless format — `SAVE ""`, one headerless block —
is not reproduced: an empty name is an ordinary file with an empty name.

### 11.3 Tape, phase 2: signal level

The honest path, and affordable because §6.3 leaves over half of core 0 idle:
model the cassette input bits (port C bits 4–5) as a real 300 baud CUTS waveform
decoded from a `.uef` file, clocked in guest cycles.

- Works with any MOS, any loader, and anything with non-standard block timing.
- Turbo loading is free: speed the guest clock during a load and the whole thing
  scales, because the decoder is driven by guest cycles rather than wall time.
- Saving is the inverse: sample port C bit 0 at the guest cycle rate and encode.

The cassette *audio* is not synthesised into the speaker mix by default (§9.3).

**The signal, read off the kernel ROM.** The writer at `#FC7C` sends a byte as a
0 start bit, eight data bits LSB first and a 1 stop bit. A 0 is four cycles of
1200 Hz, made by toggling port C bit 0 once per period of the 2.4 kHz
reference. A 1 is eight cycles of 2400 Hz, made by setting bits 0 and 1 so that
the hardware gates the reference itself onto the output. Every bit is timed by
`#FCD8` waiting on the reference, port C bit 4. That input was not modelled
before M8, so a `SAVE` that phase 1 declined would have hung. The reader at `#FBEE`
never looks at bit 4. It counts transitions on bit 5 over 83 turns of a
40-cycle loop, about one bit time, and takes 12 or more as a 1. Before
each byte it waits for eight long half-cycles in a row, which is the start bit
(`#FBF4`). Each block is about two seconds of 2400 Hz leader, `****`, the name,
CR and eight header bytes. Then come half a second of tone, the data and the
checksum, then two seconds of silence (`#FB3B`). The loader waits for 4,096
short half-cycles in a row before it trusts a leader (`#FB8E`).

**As built at M8.** `uef.c` walks an uncompressed image and hands out the
waveform one half-cycle at a time, in quarters of the base period: a 2400 Hz
half-cycle is one unit and a 1200 Hz one two. It reads the chunks a tape needs:
&0100, &0102 and &0104 (data), &0110 and &0111 (carrier), &0112 and &0116
(gaps), &0113 (base frequency), &0114 (security cycles) and &0117 (baud).
Metadata is skipped. UEF's default is 1200 baud, a BBC Micro's. An image
without &0117 is taken to be 300 baud, since a 1200 baud tape could not load on
an Atom anyway. `cassette.c` turns units into guest cycles with the remainder
carried, so a 2400 Hz half-cycle is 208 or 209 cycles and never drifts.
It keeps the 2.4 kHz reference in 32 bits against a point that follows the
clock (§16 has its period). **Nothing is clocked per instruction.** Both
inputs are brought up to date when port C is read, the only time they can be
seen, so a machine with no tape pays nothing and one with a tape pays per read.
A playing tape is also brought up to date once per field, for the menu and for
turbo. The common case is inline in the bus: the cassette keeps the first cycle
at which either bit can next change, bit 4's next edge or the tape's, whichever
is sooner. A read before it is one subtract and a branch, and port C is left as
it is. The MOS polls FS in its tightest loops, so even that shows. It costs 3.5 %
on the scrolling workload, 225.8 host cycles per instruction against 218.2 for
the same build without the hook, and nothing measurable on the others (§6.3).

The deck has play and stop and no motor control, because the Atom has none. A
playing tape keeps playing in guest time until it ends or is stopped, so a
paused guest pauses it. Choosing a UEF in the menu (§13) decompresses it whole
into a 64 KiB buffer, `ATOM_UEF_MAX` (§5). It goes in stopped, and the menu
names its first file, because **an empty name is not "the next file"** on an
Atom. OSLOAD with an empty name takes the branch at `#F92F`, which skips every
block framed with `****` and loads only the nameless format `SAVE ""` writes.
Phase 1 lets `LOAD ""` take the inserted `.atm` as a convenience. A UEF is read
by the ROM itself, so it answers to the names on the tape.

**The deck follows the stock kernel's cues**, the way phase 1's trap follows its
handlers, and stands aside for any other MOS in the same way. A user of the real
machine stops the tape after a load and starts it at the next `PLAY TAPE`.
Under turbo, the two-second gap between two files goes by in under a second of
wall time, so the deck does it: the prompt at `#FC40` with A = 4 stops it, the
key that answers it (`#FC79`) starts it, and OSLOAD's exit at `#F953` stops it
again. The exception is a `*RUN`, whose code may go straight on reading the
tape. The PCs are found by one table lookup on the low byte in `atom_run`, which
costs the same as the two compares phase 1's trap had. A loader that reads the
tape without OSLOAD gets no cue, and the deck is played by hand from the menu,
as on the real machine. UEFs are almost always gzipped. `inflate.c` decodes gzip
straight into that buffer, a bit at a time as puff does, with no separate
window, because back-references can be read from the output itself.
**While a UEF is in the deck the OSLOAD trap stands aside**, so every load
reads the signal. Since M11 the OSSAVE trap does too, and a save is recorded
onto the tape (below).

**Turbo** is what §6.3's headroom buys. While a tape plays, core 0 stops
pacing on the PCM queue and runs the guest flat out. The guest's own samples
are dropped, and the queue is topped up with silence to its start depth without
blocking, so the ring never runs dry. The tape is clocked in guest cycles, so a
load finishes sooner by exactly the headroom and the guest can see no
difference. When the tape stops, the blocking push paces again. Over a
300 baud load, 2–3× is the difference between six minutes and two or three.
At 300 MHz (§3.2) the headroom doubles, and so does turbo: about 5.5× by
§6.3's figures. On a Plus 2 W on 2026-09-27 a game's UEF loaded at about twice
the speed it loads at 150 MHz, timed by eye and not with the perf line.

`test_cassette` holds all of this to the ROM at both ends. The kernel's own
`SAVE` runs with no trap and no hook. What it drives onto port C bits 0–1 is
recorded cycle by cycle, and must decode as §11.3 says with no framing errors.
The recording, written out as a UEF, must come back byte for byte through the
kernel's own `LOAD`, and run. A headerless block read by a loader of its own
at `#3C00` loads too, which phase 1 cannot do: the loader never calls OSLOAD.
The last case is the hardware check's tape, `m8-two-part.uef`, driven exactly
as a user would: `LOAD "LDR"` fetches a BASIC program off the signal, and `RUN`
pokes that loader into RAM. Once the deck is played again, the loader reads
part two. The cues are held there too: the tape goes in stopped, waits through
`PLAY TAPE` for its key, stops when `LOAD` returns, and plays on after a
`*RUN`, which is a machine-code file the ROM's own `*SAVE` recorded.

**On the board**, 2026-09-23, a Plus 2 W: Chuckie Egg from a UEF made by
MakeUEF from a CSW capture. It has &0104 data with an extra short wave per
stop bit, &0113 at 1201.6 Hz before nearly every chunk, and &0114 security
cycles. `LOAD "CHUCKIE"` read the BASIC loader off the signal, and its `RUN`
chained `*RUN "CH-EGG"`, 45 blocks, `#2B00`–`#57FF`, every checksum good.
The game came up in CG6 and was played. The whole tape took 10.5 minutes of
guest time in about four of wall time, turbo holding 2.7–2.8×. The PCM queue's
low water stayed at 384 samples or more, with no underruns and no late refills.
Two things were found on the way, and neither was the signal: `LOAD ""`'s
meaning, above, and the game's need for RAM at `#4000`–`#7FFF` (§7.2).
`test_uef` holds `inflate` to the system's `gzip` over stored, fixed and
dynamic blocks, and the walker and cassette to the spec's arithmetic, edge by
edge, to the cycle.

**Recording, built at M11.** Recording follows the rule for loading. With a
UEF in the deck, OSSAVE's trap stands aside too, and the kernel's own writer
drives the signal onto the tape. With an `.atm` or nothing in the deck, phase 1
still saves to an `.atm`. The recording is **appended to the end of the tape
in the deck**. There is no overwriting in the middle and no position control:
one file follows another, as on a tape that is only ever recorded forward, and
recording leaves the tape at its end, so loading what was just saved means a
rewind first. The menu's Tape page has *New tape*. It creates `TAPEnn.uef` in
`/atom/tapes/`, holding only &0117 at 300 baud, and puts it in the deck.

- **Write protection.** A tape with FAT's read-only attribute is protected,
  as a disc is (§11.4). This is the Atom's version of a cassette's broken-off
  tab. So is a gzipped image. The emulator writes no gzip, and an image off
  the archive is the user's copy, not a blank. `tapeio` inserts a protected
  tape with `atom_cassette_insert` and a writable one with
  `atom_cassette_insert_rw`, which gives the deck its buffer and room. The
  recorder refuses to start on a protected tape, the ROM's writer drives the
  port into nothing, and the status line and the menu's status row say
  `PROTECTED`.
- **The recorder** is the other half of `cassette.c`, in guest cycles like
  the player. The line out is port C bit 0, or the 2.4 kHz reference while
  bit 1 gates it on. It is brought up to date on a port C write, once a field,
  and when recording stops, and never per instruction. A gated run is counted
  from the reference's period: every multiple of 208 cycles is an edge, so a 1
  bit's sixteen short half-cycles cost a few steps, not sixteen. Half-cycles
  are classified by length, with the boundaries halfway between: short (2400
  Hz, 208 cycles) from 156 to 311, long (1200 Hz, 416) from 312 to 519, and
  anything else a gap. The recorder decodes them as the reader at `#FBEE`
  would: a start bit, eight bits and a stop bit, each eight long half-cycles
  for a 0 or sixteen short for a 1. The result is standard chunks after the
  tape's current end: &0110 for carrier, &0100 for data and &0112 for gaps. An
  &0100 chunk's length is kept right after every byte, so the image is whole
  at any moment. That is the decoder `test_cassette` has proved against the
  ROM's `SAVE` since M8, moved into the core and turned from a buffer of edges
  into a state machine.
- **The pause between bytes is not carrier.** The writer leaves the
  reference gated on between the bytes of a block (`#FC88` waits a period
  before each start bit), so every byte is followed by a cycle or two of
  2400 Hz. Written as it stands, that is an &0110 chunk between every two
  &0100 bytes, fifteen bytes of chunk per byte of data, and it is what
  `test_cassette`'s own decoder still writes. The recorder leaves out a run of
  fewer than 32 short half-cycles between two bytes, and the bytes stay in
  one &0100 chunk. The reader needs no tone there: it samples only the eight
  data bits and then waits for eight long half-cycles in a row (`#FBF4`), so
  it has the whole stop bit to get back. A 48-byte `SAVE` makes a 104-byte
  image this way, against 748. The leader and the half second of tone before
  a block's data are longer and are kept.
- **A byte that does not frame is dropped and counted**, in the heartbeat,
  on the status line and on the menu's status row. A saver with its own
  non-CUTS format cannot be written as &0100, and representing it is not
  planned. Nothing in the archive's own savers is known to need it.
- **The deck follows the kernel's cues** as it does for `PLAY TAPE`: the
  prompt at `#FC40` with A = 6 is `RECORD TAPE`, and the key that answers it
  (`#FC79`) starts the recorder. It stops at OSSAVE's exits: `#FB39`, the PLP
  after the named path's record loop, and `#FAB9`, after the nameless path's
  bytes. The plan named `#FB78`, the switch-off after the last block, but
  that runs after every block of a file. The named path's two seconds of
  silence after its last block are recorded before the exit, as an &0112. A
  saver that never calls OSSAVE gets no cue. The menu's *Record*/*Stop
  recording* works the recorder by hand.
- **The card is written when the recorder stops**, not at power-off. At the
  stop cue the CPU stalls on `TAPE_RECORDED`, a tape request that is the
  deck's rather than the guest's, as it stalls for a tape call (§11.2). Core 1
  writes the whole image to `<tape>.new`, unlinks the tape and renames the new
  file into place, as snapshots are written (§11.5), and hands the machine
  back. Inserting a tape whose file is missing takes its `.new`, which is what
  a card pulled between the unlink and the rename leaves. A pulled card or
  power lost after `SAVE` returns cannot lose the file, then. A recording
  stopped by hand is written by the menu at once, and a tape taken out of the
  deck with a recording on it is written before it goes.
- **Capacity.** The recording goes into the deck's 64 KiB buffer after the
  image already there, so no memory is added (§5). A tape that fills it has
  run out: the recorder writes nothing more, the status line says `FULL`, and
  what fits is written to the card at the stop cue as usual. At 300 baud, 30
  bytes a second, a new tape holds about 36 minutes of data.
- **Turbo** applies while recording as while playing. The recorder is
  clocked in guest cycles, so a save finishes sooner by the headroom, and the
  guest cannot tell.

`test_cassette` records through the core's recorder as well as its own. The
test's decoder stays as the independent model the core's output is compared
against: the ROM's `SAVE` onto a new tape must give the same bytes, with no
framing errors, from both. A second file is appended after it, and after a
power cycle both load back through the ROM's `LOAD` by name, the second first,
then the first after a rewind. A protected tape refuses, stays byte for byte
as it was, and `SAVE` still returns. A tape with 40 bytes of room fills,
stays a whole image and is still written out, and `SAVE ""`, which ends at
the other exit with the reference still gated on, loads back through
`LOAD ""`. The recorder's hook sits on the port C write, in the bus's slow
path, behind one test of `rec.on`, and costs nothing measurable when nothing
is recording (§6.3's M11 table).

A build configured with `-DPICO_ATOM_BOOT_NEW_TAPE=ON` boots with a new tape
in the deck, as `PICO_ATOM_BOOT_TAPE` boots with a named one, so a recording
can be made over the UART.

**On the board**, 2026-09-26, a Plus 2 W: *New tape* made `TAPE01.UEF`, a
BASIC program `SAVE`d onto it, and after a power cycle `LOAD` read it back and
it ran.

### 11.4 Disc, phase 3

AtomDOS: the `dosrom.rom` at `#E000` plus an 8271 FDC at `#0A00`, backed by
Acorn disc images in `/atom/discs/`. **Built at M9.** The DOS is dormant until
`*DOS`, the kernel's own command for `#E000` (its command table at `#F8E7`), as
the Acornsoft Atom Disc Pack manual has it: the kernel does not start it at
reset.

**What the DOS asks of the chip, read off its ROM.** Every command goes out with
the drive select in bits 7–6 (`#E7D2`: select 0 is bit 6, select 1 bit 7; drives
2 and 3 are the second sides, chosen by the side-select bit of the drive control
output, table `#E78E`). At `LINK` it resets the chip (`#E000`), SPECIFYs, loads
empty bad-track registers, writes the mode register to `#C1` for non-DMA
operation (`#E874`) and seeks track 0. A transfer is READ DATA or WRITE DATA,
variable length, 256-byte sectors, at most to the end of the track (`#E816`); a
load across tracks is one command per track. Before each, it polls READ DRIVE
STATUS until bit 2 (RDY0) is set (`#E774`) — for either drive, which is only
right if both RDY inputs follow the selected drive, as a shared READY line on a
Shugart cable makes them. So they do here, and an empty drive makes the DOS
wait, as the real one does, until a disc goes in.

**READY is also how the DOS notices a new disc.** It keeps the catalogue in
RAM at `#2000` and re-reads it only when the drive is not ready (`#E731`). On
the real machine READY drops when the 8271 unloads the head, which stops the
motor. That happens after SPECIFY's idle count, `#CA`'s high nibble, 12
revolutions or 2.4 s. The model does the same: a command loads the head, a
completion starts the count, and READY is a disc in the drive with the head
loaded. A disc is changed from the menu with the guest paused, so no guest time
passes, and an unload that was counting down happens then instead. Without
that, a swapped disc listed the old catalogue. A head the DOS has loaded itself
(`#E75B`, writing the drive control output) to wait for READY is not unloaded,
so a disc put into an empty drive lets a waiting `*CAT` go on. A reset of
the chip, BREAK (§6.4) or the DOS's own pulse at `#E000` on `*DOS`, unloads
the head too, on the understanding that the 8271's drive control outputs go
inactive with RESET (not yet checked against the datasheet). On a Plus 2 W on
2026-09-26 BREAK just after a disc access cleared the status line's `D0` at
once. Before this, a
BREAK within the idle count cancelled the count and left the head loaded
until the next command, and the status line (§8.2) went on naming the drive.

In non-DMA mode each byte raises INT with the non-DMA data request, and INT is
the Atom's NMI (§6.4). The DOS's handler checks bit 2 of the status, moves the
byte through `#0A04` with a routine it copied into page zero (`#00F2`), or, if
bit 2 is clear, reads the result. At FM's 125 kbit/s a byte comes every 64 µs,
and the handler takes 52 cycles of it, so a 1 MHz Atom keeps up. Result `#12` is
`DISK PROT` and anything else, after ten tries, is `DISK ERROR nn` (`#E7A9`).

**The model** (`i8271.c`) knows each drive's geometry and decides for itself
what is there: sector not found when the track under the head is not the one
asked for or the sector is past 9, write protect, not ready. It asks the port
only for bytes, a track at most at a time: a read posts a request and waits,
busy, while the CPU runs on; a write collects its sectors from the CPU and then
posts. `main.c` parks core 0 at the field boundary and core 1 serves the request
from the image (`discio.c`), as it serves a tape call (§11.2). Guest time stands
still while it does, so the card's latency is invisible to the guest.

The chip's time is the guest's. A byte every `ATOM_FDC_BYTE_CYCLES` (64), a
sector every tenth of a 300 rpm revolution, a fixed step and settle time.
SPECIFY's parameters are taken and not modelled, since their units are
unconfirmed and nothing the DOS does depends on them. `atom_run` stops its slice
at the chip's next event, so the FDC costs one compare per slice, not per
instruction. Measured on a Plus 2 W on 2026-09-23 with `perf-run.sh`, against an M8
control build on the same board in the same session: scrolling 233.0 host
cycles per instruction against 234.9, and compute 171.7, idle 214.9 and bell
155.4, all within the run-to-run spread (§6.3). The FDC costs nothing
measurable. A command written in the middle of a slice starts at that slice's
end at the latest. Late data is not modelled. Format, verify and read ID are
there for the utilities that call them; the scans and the 128-byte command forms
are not.

The track register is per surface and the head moves by the difference, so a
program that rewrites it, as double-stepping DFS variants do, finds what the
real chip would.

**Images.** `.ssd`, `.dsk` and `.40t` are one side, track after track; `.dsd`
is two, interleaved by track. A short image, cut off after its last used
sector, is a 40-track disc that reads zeros past its end and grows when written
there. A file with the read-only attribute is a write-protected disc. The menu's
Disc page puts an image in drive 0 or 1 (§13).

**Snapshots** carry the chip's setup: mode register, drive control output,
track registers and head positions (§11.5). Without the mode register a restored
machine would be back in DMA mode, and its next read would never raise an NMI.
A snapshot is refused while a command is in progress or its completion has not
been taken.

`test_disc` holds the model to the DOS itself: `*DOS`, then `*CAT`,
`*LOAD` across a track boundary, `*RUN`, `*SAVE`, `DISK PROT` on a protected
disc, drive 1, a missing side, and an empty drive that the DOS waits on until a
disc goes in. `test_i8271` holds the chip's timing and the parts the DOS does
not reach.

### 11.5 Snapshots

Whole-machine state, in `snapshot.c`: a 20-byte header (magic, version,
lengths, CRC-32 of the payload), 96 bytes of CPU, 8255, VIA, 8271 and machine state
written field by field, little-endian, then the whole 64 KiB address space —
65,652 bytes in all. It is never a struct dumped from memory: `atom_t` holds
pointers and padding, and a snapshot has to outlive the build that wrote it.
M10 put the rest of the VIA (§7.4) into state bytes that had been reserved and
written as zero. Each field is encoded so that zero is its reset state, so a
file saved before M10 still loads, as a VIA with idle lines.

The clock (§12.1) is bit 7 of the configuration byte, set for any clock above
1 MHz, and state byte 89, which is 4 at 4 MHz and 0 otherwise. A 4 MHz file
alone says version 2. A build from before M13 would take it for 2 MHz, and
the version makes that build refuse it as newer. Every 1 and 2 MHz file is
still version 1, byte for byte what M12 wrote, and this build reads both
versions.

**ROM bytes are not in it.** ROM pages go out as zeros and the SHA-1 of every ROM
page goes in instead; a snapshot loads only into a machine whose ROMs hash the
same, and only into the same memory map. The field rate is still written,
now always 60 (§16), so that files saved before it became a constant load. Acorn's images are the
user's to supply (§1), and a program resumed over different ROMs is §16's worst
kind of bug.

**Loading is two passes.** The first reads the whole file and checks the header,
the CRC, the configuration and the ROMs without touching the machine; only then
does the second pass change anything, so a torn, foreign or newer file leaves
the running machine exactly as it was. Keys come back up and the loudspeaker
restarts from the restored clock.

On the card they are `/atom/snaps/slot1.psnap` to `slot4.psnap`, saved and
loaded from the menu (§13). A save writes `slotN.new`, closes it, removes the old
file and renames the new one into place. A rename alone is not proof of
power-loss atomicity (hardware notes §7.1), so the recovery policy is on the
load side: a `.psnap` that is missing or fails its check gives way to a whole
`.new`, which is what an interrupted publish leaves.

`test_snapshot` holds it to execution: a machine saved part-way through a
program, restored into a machine that has been doing something else and run
for 150 fields, must arrive at the same RAM, CPU, VIA and 8255 state as the
original run on. It also checks each refusal leaves the machine untouched.

### 11.6 Saving the menu's settings

**Built at M11, on the card, not in flash.** This section first put the
configuration in internal flash, written from the menu with
`flash_safe_execute`. That is dropped. The settings file (§11.7) already says
what the machine powers up with, so the menu saves into it, and the emulator
writes no flash at all:

- One place holds the settings, and the user can read and edit it on any
  computer. A flash copy would be a second source of truth that disagreed with
  the file, and §11.7 would need a rule for which one wins.
- A flash erase takes tens of milliseconds with XIP offline and interrupts
  masked. That is longer than the audio deadline, and it would have been the one
  accepted violation of it (hardware notes §5.4, §7.2). A card write has no such
  cost.
- The settings survive reflashing the firmware.

**Saving is a menu item, *Save settings*, never automatic.** Trying a setting
costs nothing, and closing the menu keeps it until power-off, as now. Writing
the card is a separate, deliberate act. The menu already holds the card
mounted (§13), so no new handoff is needed.

**What is written:** the keys the menu sets. These are `screen`, `border`,
`background`, `status`, `backlight`, `volume`, `keys`, `tape`, `drive0` and
`drive1`. `upper_ram`, `dos` and `turbo` have no menu item, so their lines
are left as the user wrote them. M12 added `perf`, and the Machine page's
`upper_ram`, `dos`, `clock` and `utility` (§13.1). The machine's keys are
saved as it is running, not as the page has them staged. `turbo` stays the
file's alone. A utility ROM is compared by name without regard to case, as
FAT does. The tape is saved by name, not position: at
the next boot it goes in stopped, at its start, as §11.7 says. A tape or
disc in `/atom/tapes/` or `/atom/discs/` is written as a bare name, anything
else as a path (`settings_card_name()`). A layout chosen by a tape's load
(§10.5) is not the user's choice, so while one is in force `keys` is saved
as the file had it. The backlight is saved only once the file or the menu
has set it; until then the southbridge keeps its own level. `main.c` keeps
what the file said at boot, and each save updates it, so the keys the menu
does not set are saved from there.

**The file is edited, not regenerated.** It is the user's text, and a save
must leave it recognisably theirs. `settings_rewrite()` in
`src/core/settings.c` takes the file's text and the settings. It returns the new
text, in a static buffer of `ATOM_SETTINGS_FILE_MAX` (§5), with no I/O:

- A key already in the file keeps its line. It keeps its place, its
  indentation and any trailing comment, and only the value is replaced.
- A key not in the file is appended only if its value differs from what the
  machine powers up with without it. The backlight counts as changed once the
  menu has moved it. So a short file stays short, and a default the user never
  touched keeps following the firmware's.
- Comments, blank lines, lines for keys the menu does not set, and lines that
  do not parse are copied as they stand. The status row already names the
  first problem, and a save does not correct the user's typing.
- The file's own line ending is kept, CRLF or LF, and appended lines use it.
- **A duplicate key refuses the save.** §11.7 already treats it as an error,
  and there is no telling which line the user meant. So does a result longer
  than `ATOM_SETTINGS_FILE_MAX`. The status row says why, and the file is not
  touched.
- A value that already says the same keeps its spelling: `screen = color`
  stays `color`, and a tape named by its full path stays a path. Only a line
  whose value now says something else is edited, so a second save changes
  nothing.
- A line that names a key but does not parse is not that key's line. It is
  copied as it stands, and the key is appended as though it were missing.
- A comment keeps its column when a value changes, if the new value leaves
  room, so a file laid out as names, values and comments stays lined up. A
  value put where there was none goes one space after the `=`, as the other
  values are, not against the comment, and a value too long for the column
  pushes its comment on to one space after it, since a `#` straight after a
  value would join it. A gap with a tab in it is kept as it was. On the board
  the first version put `TAPE01.uef` in the comment column of the README's
  `tape =` line, which is how this was found.
- The new text is parsed with the ordinary parser before anything is
  written, and must give the menu's settings back, or the save is refused as
  `WOULD NOT READ BACK`. A path the format cannot hold, one with ` #` in it,
  is refused this way. A rewrite that disagrees with its own reader is a bug,
  and it is caught on the board as well as in `test_settings`.

`settingsio.c` writes the result to `/atom/pico-atom.new`, unlinks the file and
renames the new one into place, as snapshots are written (§11.5), so a pulled
card leaves the old file or the new one and never half of either. The file is
read from the `.new` at boot if a save was cut off between the unlink and the
rename. On a Plus 2 W on 2026-09-26 settings saved from the menu, with
`TAPE01.UEF` in the deck, came back after a power cycle, the tape in the deck
with them. A card without the file gets a new one: a comment line naming the
emulator, then the lines that differ. With no card, the status row says
`NO CARD` and nothing else changes.

### 11.7 The settings file

`/atom/pico-atom.cfg` (§11.1) is how the user chooses the machine they power up
to. Every default the emulator has is in one place, `settings_default()`
(`src/core/settings.c`), which takes the guest's from `atom_config_default()`
(§7.2). The file names only what it changes, one `key = value` a line, in the
same format as a `.map` file (§10.5). A `#` at the start of a line or after
white space starts a comment, so a value can be annotated and a file name can
still hold a `#`:

| Key | Values | Default |
|---|---|---|
| `screen` | `colour`, `mono` (§8.7) | `mono` |
| `border` | `on`, `off` (§8.7) | `on` |
| `background` | `black`, `dark`: text on dark green or orange (§8.7) | `black` |
| `status` | `on`, `off`: the status line (§8.2) | `on` |
| `backlight` | 1–15, the menu's steps of 16 (hardware notes §4.11) | the southbridge's own |
| `volume` | 0–8 | 8 |
| `keys` | `standard` or a layout's name (§10.5) | `standard` |
| `tape` | a tape in the deck at boot, stopped (§11.2, §11.3) | none |
| `turbo` | `on`, `off`: unpaced while a UEF plays (§11.3) | `on` |
| `drive0`, `drive1` | a disc image in the drive at boot (§11.4) | none |
| `upper_ram` | `on`, `off`: RAM at `#4000`–`#7FFF` (§7.2) | `on` |
| `dos` | `on`, `off`: the 8271 at `#0A00` (§11.4) | `on` |
| `clock` | `1`, `2`, `4`: the guest's MHz (§12.1); 4 runs at 2 unless `host_clock` is 300 | `1` |
| `utility` | a file name in `/atom/roms/` for the socket at `#A000`, or `none` (§13.1); a path is refused | `utility.rom` |
| `perf` | `on`, `off`: the perf line (§13.1) | `off` |
| `host_clock` | `150`, `300`: the Pico's `clk_sys` in MHz, read at power-on before anything else (§3.2) | `150` |
| `via_port_b` | `on`, `off`: VIA port B on the GPIOs (§7.4) | `on` in the build that ships, `off` in the development build |
| `via_port_b_pins` | eight pins, PB0 first, each `gpN` or `nc`, no pin twice (§7.4) | `gp2 gp3 gp4 gp5 gp21 gp28 nc nc` |

A bare file name is looked for in `/atom/tapes/` or `/atom/discs/`, and a path
from the root is taken as it stands. The VIA, the tape trap and the deck's cues
are not settings, although `atom_config_t` has flags for them. The MOS hangs
without the VIA, and without the trap or the cues a tape does not load. The
field rate is not a setting either. Every Atom runs at 60 Hz (§16), and
software counts on it.

**Read once, at boot, by core 1, before the ROMs.** Some of the file is the
machine's configuration. With `dos = off`, `dosrom.rom` is not loaded, so core 1
runs `atom_init()` with the file's `atom_config_t` while core 0 waits. That is
the same window in which `roms_load` writes the machine (§4.2). The layout, the
tape and the discs are applied after the ROMs and the card's layouts are in,
before `ready`. A build's `PICO_ATOM_BOOT_TAPE` or `PICO_ATOM_BOOT_DISC` wins
over the file, because a run driven over the UART must know what it booted with.

**A wrong line changes nothing, and the lines after it still apply.** A `.map`
file is refused whole, because a partly applied layout is a different layout.
A settings line stands alone, so one typing mistake should not cost the rest of
the file. The same goes for a line that parses but names something the card
does not have: an unknown layout, or a tape or disc that will not go in. In each
case the first problem is logged and kept for the menu's status row
(`settingsio.c`). A duplicate key is an error, not last-one-wins, so that
nothing about the file depends on the order of its lines. The parser is tested
on the host (`test_settings`).

**The emulator writes the file only when asked.** It is the user's text, with
the user's comments, and a menu change lasts until power-off. The one
exception, since M11, is the menu's *Save settings*, which edits the file in
place and keeps everything the user wrote (§11.6).

---

## 12. Timing and synchronisation

### 12.1 The guest clock

| | |
|---|---:|
| Guest CPU | 1,000,000 cycles/s |
| VDG field rate | 60 Hz (§16, confirmed) |
| Guest cycles per field | 16,666 |
| VDG lines per field | 262: 192 active, 32 with `FS` low, 38 blank (§16) |
| `FS` (port C bit 7) low for | 2,035 cycles, the bottom border and retrace |
| Then, before the next active line | 2,417 cycles, vertical blank and the top border |

Core 0 runs in **field-sized slices** with cycle-debt carry-forward. The slice
is **split where `FS` falls and where it rises**, and that split is the whole
point:

```c
budget += ACTIVE_CYCLES;            /* 192 lines, FS high */
budget -= atom_run(&m, budget);     /* returns true cycles; debt carries */
atom_field_sync(&m, true);          /* FS low */
budget += FS_LOW_CYCLES;            /* 32 lines */
budget -= atom_run(&m, budget);     /* the guest runs while FS is low */
atom_field_sync(&m, false);         /* FS high */
budget += BLANK_CYCLES;             /* 38 lines */
budget -= atom_run(&m, budget);
snapshot_publish();                 /* where the next active line begins */
```

**Guest instructions must execute while `FS` is low**, or the flag is
unobservable. Asserting and releasing it back to back after a whole field's
run leaves a program that polls `#B002` bit 7 spinning forever, which is what
the real Atom's screen-writing routines do. The debt carry survives the split
because `atom_run` reports its true cycle count on each call, so two runs
against one accumulator behave as one.

This loop is `atom_run_field()` in `src/core/atom.c`, so the firmware and the
host test run the same code. `test/host/test_field.c` runs a guest loop polling
`FS` and asserts that it both observes the low state and escapes once per
field, since asserting that the accessor flips the bit does not catch this. It
also runs the old M0 shape — pulse `FS` after the whole field — as a control
that must see nothing, so the test is known to be able to tell the two apart.

**The field ends where the next active line begins**, and that is where the
snapshot is taken. The beam shows nothing for 70 lines after `FS` falls, about
4,450 cycles, and games spend that time on the screen: ASTEROI waits for `FS`
low, XOR-erases every rock and draws them all again, which takes it about
2,000 cycles. Until the line counts were taken, `FS` was low for an estimated
6 % of a field, 999 cycles, and the field ended when it rose. Every snapshot
caught the redraw half done, and on alternate fields the rocks were missing.
`test_field` runs a guest frame of that shape and requires every field to end
with it finished, with the old split as the control that tears. On a Plus 2 W on 2026-09-24
ASTEROI was played with its rocks drawn whole, and `RND` gave different
numbers after a power cycle; idle, the guest took 43 % of core 0 with 2.30×
headroom, inside §6.3's range.

The line counts are the MC6847 datasheet's (§16). Figure 8 has `FS` low for
32 lines of a 262-line field. Figure 13 has it fall at the end of the 192
active lines, with 26 of bottom border and 6 of retrace after them, and 13 of
vertical blanking and 25 of top border before the next active line. A line
is 63.6 cycles, so `config.h` scales each part from the 16,666-cycle field.

Within a slice, the audio integrator emits a sample every 27.31 guest cycles via
a fixed-point accumulator, so audio and CPU share one clock by construction and
cannot drift apart.

**A 2 MHz guest, built at M12, and 4 MHz at M13.** An Atom's 6502 ran at
1 MHz; running it at 2 MHz was an owner's modification. The Machine page
(§13.1) offers 1 or 2 MHz, as `atom_config_t.clock_mhz` and the settings
file's `clock` (§11.7), and 4 MHz when the host is at 300 MHz (§3.2). §6.3
measures headroom at 1 MHz at 2.31–3.15×, so 4 MHz on a 150 MHz host would
need 1.3–1.7× more than core 0 has. At 300 MHz it takes what 2 MHz takes at
150, measured in §6.3. A file that asks for 4 MHz on a 150 MHz host gets
2 MHz, and the menu's status row says `CFG CLOCK: 4 MHZ NEEDS HOST_CLOCK 300`.
The rule for what changes is what drives each part on the real machine:

| Follows the 6502's clock (the same count in guest cycles, so faster in wall time) | Fixed in wall time (twice as many guest cycles at 2 MHz, four times at 4) |
|---|---|
| the 6522's timers and shift register, which count Φ2 (§7.4): an AGD game's 25 Hz frame clock runs at 50 Hz | the field: 33,333 cycles, `FS` low for 4,071, then 4,834 before the next active line; BASIC's `WAIT` is still 1/60 s |
| software timing loops: the MOS's bell (`#FD18`) sounds an octave up, about 775 Hz | the beeper's sample period, 54.62 cycles (§9.3) |
| | port C bit 4's 2.4 kHz reference, 832 cycles, which comes from the crystal and not from Φ2 (§16) |
| | a UEF's half-cycles on port C bit 5, and the recorder's thresholds (§11.3) |
| | the 8271's byte, sector, step and settle times (§11.4) |

The quantities in the second column are worked out from the clock once in
`atom_init` and kept where they are used. The field's parts are
`atom_t.field_active`, `field_fs_low` and `field_blank`, recomputed from
`cpu_hz / 60` with `config.h`'s formula rather than doubled, so they are
33,333, 4,071 and 4,834 at 2 MHz, 66,666, 8,142 and 9,669 at 4, and exactly
the constants at 1. The beeper
is given `atom_t.cpu_hz`, and its period was already a field. The cassette
keeps `cpu_hz` and the reference's period in `cassette_t`, and the 8271 its
`mhz`, which every one of its times is multiplied by. `ATOM_CPU_HZ` stays the
base clock. The only per-instruction cost is bit 4's divide, which is now by
a field, not a constant. It is on the port C read path, so it is to be
measured against a control, as the port C hook was at M8 (§6.3).

The clock changes only through the Machine page's restart (§13.1), never
under a running program. Every in-flight count is kept in guest cycles: the
beeper's phase, the deck's position, the FDC's next event and the field
split. Converting all of them at a field boundary is a larger change, for
nothing a user would notice. A snapshot records the clock in bit 7 of its
configuration byte, which is free. Zero means 1 MHz, so every older file
loads as 1 MHz. A file taken at another clock is refused (§11.5), and the
status row says `NOT LOADED: ANOTHER CLOCK`. M13 added byte 89 for 4 MHz.

**Two things to settle by execution.** Both go into §16.

- *Does the MOS read a tape at signal level at 2 MHz?* **No, and it writes
  one.** `test_cassette_2mhz` settled it on 2026-09-26. The ROM's `SAVE` at
  2 MHz frames every byte, because it times each bit against port C bit 4,
  which keeps wall time. A 1 MHz machine then loads the recording, and so
  does one recorded through the core's recorder at 2 MHz. The ROM's reader
  times the tape with its own loops. At 2 MHz it reads nothing: played past
  the refusal, the whole tape runs by and no byte of the program arrives.
  So the deck plays a UEF only at 1 MHz. At 2 MHz `atom_cassette_play`
  refuses, the menu's *Play* says `TAPE NEEDS 1 MHZ`, and the status line
  says `NEEDS 1 MHZ` once a `PLAY TAPE` cue or *Play* has been refused.
  Recording works at either clock. The phase-1 trap (§11.2) does not care
  about the clock, and `test_tape` passes at 2 MHz.
- *What is left at 2 MHz?* Headroom at 1 MHz is 2.31–3.15× (§6.3), so at
  2 MHz core 0 would spend an estimated 63–87 % of its time in the guest. Turbo
  under a tape, 2.7–2.8× at 1 MHz, would be about 1.4×. M12 is not done until the four perf workloads run at 2 MHz with
  zero underruns and zero late refills, and the figures go into §6.3. They did on 2026-09-26: idle, the heaviest, took 86 % of core 0.

**At 4 MHz (M13)** the same tests run a third time, as `_4mhz`
(`PICO_ATOM_TEST_MHZ=4`), and hold the same answers. The ROM writes a tape at
4 MHz that a 1 MHz machine loads, and cannot read one: played past the
refusal, 27,032 edges go by and nothing arrives. So the deck still plays only
at 1 MHz. The bell measures 1,551.12 Hz against 1,550.39 Hz from its loop's
cycle count. The field is 66,666 cycles, and `*CAT`, `*LOAD`, snapshots and the
phase-1 trap all work.

### 12.2 Throttling

The emulator runs faster than real time (§6.3), so it must be paced. **Pace on
the audio ring**: core 0 blocks until the PCM queue has room for another field's
worth of samples. The PWM slice's wrap is a hardware 36.62 kHz clock derived
from `clk_sys`, it is the most stable timebase on the machine, and pacing on it
holds the queue at its target depth without a wall-clock timer.

**It does not make underrun impossible, and the design must not claim that.**
Pacing removes one failure mode — the producer racing ahead and overrunning a
full queue — and it keeps the queue full in steady state. It cannot help when
the producer is *prevented* from running: a flash erase takes the interrupt hole
offline for tens of milliseconds and is the one accepted violator of the refill
deadline (hardware notes §5.4, §7.2), and a guest running below real time
starves the queue by construction. So underrun stays a live failure mode with a
defined behaviour: emit the silence value, **count it** (§12.3), and resync
without attempting to replay the missing samples. A counter that could never
fire would be dead code, and §9.4's requirement to count producer starvation
separately from late DMA refills only means something if both can happen.

When audio is muted, samples are still produced and consumed (hardware notes
§5.8) so that muting does not change timing.

As built, core 0 publishes the field's snapshot and then pushes its samples.
`audio_push` waits on `__wfi()` while the queue is full, and the DMA IRQ on the
same core wakes it. The queue's depth then swings between ~385 and 1024 samples
each field. The low-water mark is reported, so the slack is measured rather
than assumed. The fallback path, for a build with
audio disabled entirely, paces on `time_us_64()` against an absolute field
deadline — absolute, not incremental, so a late field does not accumulate.

Presentation is decoupled: core 1 presents the most recent published snapshot
and drops superseded ones (hardware notes §9.6, lever 2). Simulation timing is
never sacrificed to presentation.

### 12.3 Instrumentation

A perf block, reported over UART1 (115200 8-N-1, TX GP4, RX GP5) as a heartbeat
every five seconds (`main.c`). It was also to be summarised in the status band.
M11's status line (§8.2) leaves it out: the line is for the deck and the drives,
and the numbers stay in the heartbeat, where they can be written to a file.
The heartbeat's `cassette` line gained the recorder at M11: whether the tape
is writable or recording, the bytes recorded and those that did not frame,
the image's size against the deck's room, and whether the card has it yet.

| Counter | Why | As built |
|---|---|---|
| Real-time ratio (guest s / wall s) | the headline number; paced, so it reads 1.000 whatever the code costs | `heartbeat` line, `rt` |
| Core 0 in the guest, headroom | what the code costs: the share of wall time inside `atom_run_field`, and guest cycles per microsecond of it over the clock in MHz, so times real time at either clock (§6.3) | `perf` line, which from M12 also names the clock and counts guest cycles a second; on the panel, §13.1's perf line |
| Host cycles per guest instruction | §6.3's figure; `atom_t.instructions` counts the guest's side | `perf` line |
| Present ms, dirty bands, dirty pixels | §8.4's budget, verified | last and max present time; bands and pixels only in the M3 measurement build |
| Presents, full presents, dropped snapshots | is core 1 keeping up? (§8.4) | `heartbeat` line |
| **PCM underrun samples** | producer starvation | `audio` line |
| **Late DMA refills** | consumer starvation — a different bug (§5.8) | `audio` line |
| I²C errors, key events dropped | southbridge health | `heartbeat` line |
| Battery gauge, and whether it is charging | which power a soak ran on, as far as the charger says (§15.3) | `heartbeat` line, since M13 |
| Die temperature | free, and it catches the 300 MHz setting misbehaving (§3.2) | `heartbeat` line, since M13, in whole degrees: core 1 averages sixteen conversions every five seconds (hardware notes §8.1) |

The heartbeat also carries the undocumented opcodes trapped (§6.1), tape calls
and disc sectors, the deck's position and turbo fields while a tape is in, the
PCM queue's depth and low water, and the consumed sample rate, which is the
control quantity for every measurement in this document.

Measurement discipline, from hardware notes §9.1, is part of the design and not
an afterthought: profile **in the mode you ship** (a present that returns early
in one mode has hidden a 25 ms cost before), carry a control quantity that should
not change, expect 2 % run-to-run spread, compare on one board, and **write the
numbers to a file** — a figure on a 320×320 panel cannot be copied off it.

---

## 13. User interface

The emulator is invisible in normal use: boot goes straight to the Atom's `>`
prompt. `Alt`+`M` opens an overlay menu, which **pauses the guest** and stops
audio, making it the safe boundary for SD and flash work (§11).

**As built at M6.** Core 0 parks at a field boundary and hands the machine to
core 1 (the handoff in `main.c`, the same one a tape call uses), feeding the PCM
queue silence so audio neither underruns nor loses its pacing. Core 1 runs the
menu with the card mounted and the keyboard its own: it drains the southbridge
FIFO and consumes the events itself, and core 0 starts its held-key set afresh
on the way back. The menu is a 32×16 text page presented through the ordinary
renderer in place of the Atom's screen. It offers Resume, snapshot save, load
and delete over four slots (§11.5), a tape list whose choice is what an empty
name loads (§11.2), the game keymap (§10.5, added at M6b), Reset, volume and
backlight. `Esc` or `Alt`+`M` closes it. At M8 the tape page gained the deck's
controls, *Eject*, *Play*/*Stop* and *Rewind*, and lists `.uef` images beside
`.atm` files. Inserting a UEF says which name to `LOAD`. The main page shows
the deck's state and how far through the tape it is (§11.3). M9 added a Disc
page: left and right choose drive 0 or 1, and the images in `/atom/discs/` are
listed with the drive each is in and a `P` if it is write-protected. The main
page names each drive's image, and the keys-chosen note moved to the status row. M10
added a Display page. It holds the screen, colour or mono, the VDG border
on or off (§8.7), and the backlight, which moved there from the main page. A
change shows at once, because the page itself is drawn through the renderer it
changes. Snapshots moved to their own page at the same time, to shorten the
main page: left and right choose the slot on any row, and the page lists all
four slots, saved or empty. A volume change takes effect at once and beeps
at the new level: core 0, parked, plays 120 ms of the bell's pitch at the
guest's own loudness in place of the silence it feeds the queue.
The title row ends in the battery's charge, `BAT 87%`, or `CHG 87%` while it
charges: southbridge register `0x0B` (§6), read when the menu opens and every
5 s after, since the MCU refreshes it only every 20 s. A failed read shows
nothing. Verified on a Plus 2 W on 2026-09-26.
M11 added *Save settings* to the main page, which writes the menu's settings
into the settings file (§11.6), *New tape* and *Record*/*Stop recording* to the
Tape page (§11.3), and *Status line* on or off to the Display page (§8.2). The
status row, when the menu opens, says what the recorder last had to say: a
protected tape, a full one, a recording not yet on the card, or bytes that did
not frame.
The table below is the full intent; machine and display settings wait
for the milestones that give them something to set. What the machine powers up with comes from the
settings file (§11.7), and the menu's status row names the file's first problem.

| Menu | Does |
|---|---|
| Tape | attach/detach an image, rewind, play, record, position |
| Disc | attach/detach drive 0/1 (phase 3) |
| Snapshot | save, load, delete |
| Machine | RAM below the screen (16 or 32 KiB), guest clock (1 or 2 MHz), AtomDOS, the utility ROM; applied by a restart (§13.1) |
| Setup | colour or mono, border, background, status line, perf line (§13.1), backlight, volume, the game keymap in force: standard, a built-in layout, or one from the card (§10.5), and *VIA port B...*, a page that turns port B on the GPIOs on and off and gives each bit its pin (§7.4) |
| About | firmware, board, ROM identification by SHA-1, the settings file's state (§13.1) |

Reset and *Save settings* are on the main page. On 2026-09-27 the
Display page became Input/Output and took Volume and Keys from the main
page, and Resume went: `Esc` does the same. On 2026-09-28 Input/Output
became Setup: how the emulator behaves, beside Machine, what the Atom is. Pause is a key,
`Alt`+`P`, and not a menu item (§13.1). On 2026-09-28 `F1`–`F5` became
Tapes, Discs, Snapshots, Setup and Machine, the main page's first five
items in order. A function key opens the menu at its page from the running
Atom and from Pause, as `Alt`+`M` opens it at the main page, and leaving that
page, by `Esc` or by a choice that closes it, goes back to the Atom, not to the
main page. `Alt`+`H` opens a page of the keys the emulator takes for itself,
the same way; it is not on the main page. `F10` (the MCU's Shift+`F5`,
`0x90`) opens the About page the same way; on 2026-09-29 it moved there from
the keys, and the page of keys lists it in the row it had. Inside the menu the function keys do nothing: the arrows and `Enter`
get about it. The keyboard
MCU sends `F1`–`F5` as `0x81`–`0x85`; they are `KM_MENU` entries in the
standard map whose row is the page. The Atom has no function keys, and a
layout cannot name them, so they are the menu's under any layout (§10.5).
Verified on a Plus 2 W on 2026-09-28: `F1`–`F5`, `F10` and `Alt`+`H` from the
Atom, each page going back to the Atom as it closes, the Setup page's new name,
Tapes above Discs, and the About page's temperature.

Of that table, as built: Tape (without a position control, since recording
only appends), Keys, Disc and Snapshot are there; Display has colour or mono,
the border, the background, the status line, the perf line and the
backlight. The Machine page, the perf line, the About page and Pause are
M12's (§13.1). A scaled display, snow and a colour set override were in this
table and are dropped (§8.2, §8.6). The guest's `CSS` bit, port C bit 3,
alone chooses the colour set (§2.3). A System page is dropped too: its
backlight moved to Display at M10, and its volume is on the main page.

The backlight is southbridge register `0x05`, stepped to multiples of 16 and
clamped to 16–240, so a fade is 15 steps and not 256 (hardware notes §4.11).
Pause dims it (§13.1).

The menu was first designed as an overlay drawn into the top and status bands.
As built it replaces the Atom's screen instead: the menu's page goes to the
presenter as a snapshot in alpha mode, through the same renderer. **Closing it
does not "restore" pixels.** `shadow` holds 6 KiB of guest VRAM bytes, not a
decoded 256×192 panel image — in graphics modes one byte expands to 8 or 16
pixels, and alpha mode additionally needs the character ROM and the presented
mode — so there is nothing there to copy back. Instead closing the menu calls
`display_invalidate()`, and the presenter redraws the guest's next snapshot
whole, one 11.5 ms present. That is almost no new code, and the same forced
redraw is what the M3 measurement uses.

### 13.1 M12: the Machine page, the About page, the perf line and Pause

**Built, and checked on the host and on a Plus 2 W on 2026-09-26 (§17).**
This finishes the menu §13's table describes. The main page gains
*Machine…* and *About…*, which makes eleven items. As first designed they
went on rows 2–12, with the tape line on 13 and the drives on 14, but row 14
is the status row. So as built the items start on row 1, under the title,
and run to row 11, with the tape line on row 12 and the drives on row 13.

**The Machine page.** Five settings, and the action that applies them:

```
 RAM             < 32K >        upper_ram: #0000-#7FFF, or #0000-#3FFF
 CLOCK           < 1 MHZ >      clock: 1 or 2, and 4 at 300 MHz (§12.1)
 ATOMDOS         < ON >         dos: the 8271 at #0A00 and dosrom.rom (§11.4)
 UTILITY ROM     < AXR1.ROM >   utility: #A000, a file or NONE
 PICO CLOCK      < 150 MHZ >    host_clock: 150 or 300 (§3.2), since M13
 (APPLY AND RESTART)

 6522 VIA: FITTED, THE MOS NEEDS IT
```

*Pico clock* is the host's, and it is set only at power-on (§3.2). With it
staged, *Apply* writes the staged machine and the clock into the settings
file (§11.6), since that file is all a restart of the Pico reads. It then
restarts the Pico with the watchdog, and the status row says `APPLY SAVES,
RESTARTS THE PICO` beforehand. The other keys in the file are left as they
were. The guest clock offers 4 MHz only while 300 MHz is staged, and staging
150 MHz takes a staged 4 back to 2. The page ends by saying `300 MHZ IS AN
OVERCLOCK, AND 4 MHZ NEEDS IT`.

Left and right stage a value. A row whose staged value differs from the
running machine is marked `*`. **Nothing changes until *Apply and restart*.**
Leaving the page with staged changes discards them, and the status row says
`NOT APPLIED`. Each setting changes the machine under any program in it:
RAM changes the page table, AtomDOS changes a ROM and eight bytes of page
`#0A`, the utility ROM changes the ROM hash, and the clock changes every
count kept in guest cycles (§12.1). On a real Atom, changing any of them
meant switching off and taking the lid off. So the restart is a power-on,
and the status row warns that it loses the program in memory: `APPLY
RESTARTS: PROGRAM LOST` once anything is staged, and the page says so
under the VIA's line.

The VIA is shown and cannot be changed. The MOS spins before its first
prompt without it (§7.3), so a machine without one is a dead machine, and
§11.7 already leaves it out of the settings. The video aperture at
`#9800` (§7.2) is not offered. Nothing uses it, and a stock machine does not
have it. An unexpanded Atom's RAM needs finer blocks than `atom_config_t`
has, and is not offered either.

*The utility ROM* is chosen from `/atom/roms/*.rom`, leaving out the four
names §11.1 fixes, sorted, plus `NONE`. The list is capped at
`ATOM_ROM_LIST_MAX` (16) in `config.h`. The setting's default is
`utility.rom`, so a card set up for M11 behaves the same: when the default
file is missing, the socket is empty, as now, and nothing is reported. A file
the settings name that is missing is reported, as the first problem on the
status row.

*Apply and restart* is the boot's own path. The boot's configuration step
becomes a single function in `main.c`, `machine_power_on()`, that both boot
and the menu call. Core 0 is already parked while the menu is open, so core
1 may write the machine then, as it does at boot (§4.2). The steps:

1. **The ROMs are checked before the machine is touched**, in two passes,
   as a snapshot is loaded (§11.5). Each file the new configuration needs
   is read and hashed. A required one missing, or one of the wrong size,
   refuses the restart (`AKERNEL.ROM MISSING: NOT RESTARTED`) and leaves the
   machine running as it was. The card may have been changed while the
   menu was open. The old machine is not kept for step 2: its ROMs are in
   its `ram[]`, and a second `atom_t` would cost ~70 KiB (§5). A card
   changed between the two passes, milliseconds apart, gives the no-ROMs
   page, as at boot, rather than a guest running without its kernel.
2. `atom_init` with the new `atom_config_t`, `roms_load`, and
   `atom_seed_rnd` from `get_rand_64()` (§7.2).
3. The tape goes back in the deck, stopped at its start, and the discs
   back in their drives, as at boot (§11.7). The layout in force stays.

The restart is refused while the deck is recording (`STOP RECORDING
FIRST`), because the recording is not on the card until it stops (§11.3).
As built it is also refused while a stopped recording has not reached the
card (`RECORDING NOT ON THE CARD YET`), since step 3 reads the tape back
from the card. *Save settings* then writes what the machine is running
(§11.6).

As built, `machine_power_on(cfg, utility, restart)` is the function. The
boot calls it with `restart` false, since at boot there is no machine to
keep: a missing ROM is the no-ROMs page (§11.1), as before. The menu calls
it through `menu_settings_t.restart`. The first pass is `roms_check()` in
`roms.c`. The card is mounted for the whole of it. Seeding RND and the reset
after the kernel moved from core 0's start into this function, so a restart
gets them too, and it sets the beeper's rate once audio is up. Core 0 sees a
restart as a bump of `g_power_ons` when it comes back from the park, and
starts the counters it keeps against the guest's clock again.

**The About page.** Read-only, so that a bug report can be copied off it:

```
 PICO-ATOM <GIT DESCRIBE>
 BOARD PIMORONI_PICO_PLUS2_W 23C
 RP2350 <REV>  150 MHZ  SB <VER>
 MACHINE 32K 1 MHZ DOS VIA

 #F000 AKERNEL.ROM  OK  2621F27D
 #C000 ABASIC.ROM   OK  A8EA19F1
 #D000 AFLOAT.ROM   OK  EBCDE5B3
 #E000 DOSROM.ROM   OK  71EA0A4B
 #A000 AXR1.ROM     ANY <SHA-1>

 SETTINGS OK
```

The version is `git describe --always --dirty`, generated into
`pico_atom_version.h` at build time by `cmake/version.cmake`, which a target
runs on every build and which rewrites the header only when the text
changes. It is not taken at configure time, because a configure-time value
goes stale on the next commit. The startup banner prints it too. The board
line is `board_info_t`'s SDK board name, cut to 21 characters, and since
2026-09-28 the die's temperature at its right end: `board_temp_c()`, whole
degrees and uncalibrated (hardware-notes.md §8.1), read as the page opens and
again every 5 s with the battery while it is open. The menu is core 1's, and
so is the ADC. The southbridge's `VER` is read when the page
opens. Each ROM slot shows its file, its state and the first eight hex
digits of its SHA-1. `OK` means the image §11.1 records, `??` a loaded
image that is not it, `ANY` the utility socket, which takes any image,
`OFF` a slot whose hardware is not fitted, and `--` nothing loaded. The
hashes come from `roms_report_t`, which gains the digest of each slot
(100 bytes), is kept by `main.c` from boot, and is renewed by each restart.
The last line is the settings file's first problem, or `OK`.

**The perf line.** One line of the heartbeat's figures (§12.3), on the
panel, off by default. The setting is `perf` (§11.7) and a row on the
Setup page. It uses the top band's outer 16 rows, opposite the status
line: 40 cells of 8×12 at y = 2–13 (`ATOM_PERF_Y`). The border never fills
those rows (§8.7).

```
C0 43% 2.31X  LCD 11.5MS  DROP 0  UR 0 0
```

The fields are:

- the share of core 0 spent in the guest, and the headroom, both over the
  last second (§6.3);
- the longest present in that second (§8.4);
- the snapshots dropped in that second;
- underrun samples and late refills since boot (§9.4). Both should read 0
  for ever, so a count since boot keeps a single blip on show.

With every count at 0 the line is exactly 40 characters. As built, when the
counts grow the gaps close to one space, so that the last figure is not the
one that falls off, and a figure past its width is shown at its widest
(`test_status`). The headroom is over the clock, so it is times real time at
2 MHz as at 1.

These are host counters, not guest state, so they do not go in the display
snapshot's `atom_status_t`. Core 0 already accumulates them for the
heartbeat. Once a second it closes a window and writes a `perf_line_t` of
32-bit words, each single-copy atomic, as core 1's counters already are for
the heartbeat (`main.c`). A read that straddles a write can mix two seconds,
which shows for a second and is harmless. Core 1 has its own figures, the
present time and the drops. `status_perf_format()` in `status.c` makes the
text, so `test_status` holds it. Core 1 formats it once a second, at the end
of its own second, and `display_perf()` draws it only when the text
changes: 3,840 pixels, under 1 ms of wire (§8.4). It is blanked while the
menu is open, and comes back within a second of it closing.

The line is for watching, and the heartbeat is for measuring. A number to
keep still goes to a file (hardware notes §9.1). The line's own cost is measured: the four
workloads with it on and off in one sitting must agree within the ~2 %
spread. It is also how the 2 MHz margin (§12.1) is watched on the panel.

**Pause.** `Alt`+`P` (§10.3). Keymatrix sets `pause_request` the way `KM_MENU`
sets `menu_request`. Core 0 parks at a field boundary with a new handoff,
`HANDOFF_PAUSE`, and feeds the PCM queue silence, as it does for the menu.
Everything counted in guest cycles stops with it: the deck, the FDC, the
VIA and the beeper. Core 1 does not mount the card or draw a page. The
Atom's last frame stays on the panel, and the status line reads `PAUSED`,
drawn by core 1 whether or not the line is on.

- **The backlight dims.** Core 1 reads register `0x05` and writes the
  lowest step, 16. That is one I²C read and one write (hardware notes
  §4.11), and no write at all if the level is already 16. On resume it
  writes back the level it read. The read matters because the level may
  be the southbridge's own, not one the settings or the menu set (§11.7).
  The keyboard's backlight is left alone.
- **Any key resumes**, and neither its press nor its release reaches the
  guest. Core 0 starts its held-key set afresh, as after the menu.
  `Alt`+`M` while paused goes straight to the menu, with the backlight
  restored first. As built a modifier alone resumes nothing, so that
  `Alt`+`M` can be had, and neither does a `P` while `Alt` is down, which
  is the chord's own auto-repeat. The line reads `PAUSED: ANY KEY
  RESUMES` (`status_paused_format()`).
- Core 1 polls the southbridge at 30 Hz, as the menu does, which also keeps
  the MCU's 2.5 s bus watchdog fed (hardware notes §6.1). It waits with
  `busy_wait_us_32`, never `sleep_us` (hardware notes §9.7).
- Resuming needs no catch-up. The queue was fed silence at the pace it
  drains, so pacing picks up where it left off, and the guest's clock
  never saw the gap.

---

## 14. Repository layout and build

```
pico-atom/
├── CMakeLists.txt              # pico-sdk build; host build behind PICO_ATOM_HOST
├── THIRD-PARTY.md              # material that is not ours, and under what terms
├── .github/workflows/ci.yml    # both targets under -Werror, host tests, SRAM report
├── docs/
│   ├── hardware-notes.md
│   └── design.md
├── roms/                       # workstation staging only, gitignored (§11.1)
├── src/
│   ├── core/                   # portable C11, no SDK, no allocator
│   │   ├── config.h            # every fixed capacity, in one place
│   │   ├── hot.h               # which functions go to SRAM, by measured tier (§6.3)
│   │   ├── m6502.c/.h
│   │   ├── bus.c/.h
│   │   ├── atom.c/.h           # atom_t, the page table, the run loop, §4.1's API
│   │   ├── beeper.c/.h         # PC2 → PCM: box filter at the sample period (§9.3)
│   │   ├── i8255.c/.h
│   │   ├── mc6847.c/.h         # mode decode, palettes, LUT build, row generation
│   │   ├── mc6847_font.c/.h    # 64-glyph character ROM (XRoar's, THIRD-PARTY.md)
│   │   ├── snappool.c/.h       # §4.2's three-buffer handoff; the state machine, no lock
│   │   ├── via6522.c/.h        # the whole 6522 (§7.4)
│   │   ├── i8271.c/.h          # the FDC (§11.4)
│   │   ├── keymatrix.c/.h      # held-key set, layouts, paced replay (§10)
│   │   ├── keymap_picocalc.c   # data table (§10.3), built-in layouts
│   │   ├── keylayout.c         # the .map parser (§10.5)
│   │   ├── tape.c/.h           # phase 1: the OSLOAD/OSSAVE trap, ATM (§11.2)
│   │   ├── uef.c/.h            # phase 2: a UEF image as half-cycles (§11.3)
│   │   ├── cassette.c/.h       # the half-cycles on port C, in guest cycles; the recorder
│   │   ├── inflate.c/.h        # gzip, for UEF images
│   │   ├── snapshot.c/.h       # §11.5's format
│   │   ├── settings.c/.h       # every default, /atom/pico-atom.cfg, and its rewrite (§11.6, §11.7)
│   │   ├── status.c/.h         # the status line's bytes and text (§8.2)
│   │   ├── sha1.c/.h           # ROM identity
│   │   └── romset.c/.h         # the ROM slots of §11.1
│   └── port/                   # PicoCalc + SDK
│       ├── main.c              # bring-up, core 0's field loop, core 1's loop, the park/handoff
│       ├── board.c/.h          # clocks, board identification
│       ├── lcd.c/.h            # ST7789P init, windows, DMA blit
│       ├── display.c/.h        # snapshot diff, band present, mono palette, border, status line
│       ├── audio.c/.h          # PWM slice, chained DMA, SPSC queue
│       ├── log.c/.h            # core 0's UART lines, drained by core 1 (§9.4)
│       ├── southbridge.c/.h    # i2c1 register layer: read, write, busy flag, errors
│       ├── kbd.c/.h            # key events from core 1 to core 0
│       ├── sd.c/.h  diskio.c   # SPI0 and FatFs's disk layer; FatFs is copied from the SDK
│       ├── storage.c/.h        # mount and unmount, once per piece of card work
│       ├── roms.c/.h           # /atom/roms/ into the machine; the no-ROMs page
│       ├── tapeio.c/.h         # tape calls, the tape list, the deck's buffer, recordings (§11.2, §11.3)
│       ├── discio.c/.h         # sector requests off /atom/discs/ (§11.4)
│       ├── snapio.c/.h         # snapshot slots (§11.5)
│       ├── settingsio.c/.h     # /atom/pico-atom.cfg at boot, and Save settings (§11.6, §11.7)
│       ├── keymapio.c/.h       # the card's layouts (§10.5)
│       └── menu.c/.h  textpage.c/.h   # the Alt+M menu (§13)
├── test/
│   ├── host/                   # CTest, one binary per area, no framework
│   │   ├── test_util.h         # CHECK and TEST_DONE; 77 is a skip
│   │   ├── guest.c/.h          # the real machine for the ROM tests: ROMs by SHA-1, keys typed
│   │   ├── test_m6502_functional.c   # Dormann's and Clark's suites, skipped without the binaries
│   │   ├── test_m6502_decimal.c      # exhaustive valid-BCD checks
│   │   ├── test_m6502_cycles.c       # the cycle table, by execution
│   │   ├── test_m6502_behaviour.c
│   │   ├── test_bus.c  test_i8255.c  test_via6522.c  test_i8271.c
│   │   ├── test_mc6847.c       # mode table, LUT, font layout
│   │   ├── test_mc6847_golden.c  # golden images, all nine modes
│   │   ├── vdg_scenes.c/.h     # the VRAM behind them, shared with vdg-ppm
│   │   ├── test_field.c        # §12.1: FS seen low and escaped; a redraw after FS finishes by the field's end
│   │   ├── test_present.c      # §8.4 dirty bands by execution; §4.2 snapshot pool
│   │   ├── test_audio.c        # §9.3, every sample against an independent model
│   │   ├── test_keymap.c  test_settings.c  test_status.c
│   │   ├── test_tape.c  test_uef.c  test_cassette.c
│   │   ├── test_snapshot.c  test_disc.c
│   │   └── test_boot.c         # the real MOS: typing, the bell, RND, layouts
│   ├── asm/                    # Clark's decimal test, public-domain source
│   ├── suites/                 # Dormann's binary, fetched; Clark's, assembled; gitignored
│   └── golden/                 # committed reference PPMs
└── tools/
    ├── build.sh  flash.sh      # the firmware, built and programmed over SWD
    ├── uart-log.sh  uart-type.sh   # capture UART1; type at the guest over it
    ├── perf-run.sh             # §6.3's measurement: workloads typed at the guest
    ├── perf-summary.sh         # the heartbeats, one line per workload
    ├── fetch-test-suites.sh    # Dormann's binary and Clark's, assembled, into test/suites/
    ├── trace-diff.py           # §15.1: trace both machines, diff, classify
    ├── trace/                  # atom-trace.c, atomulator-trace.c and its build, keyscript.h
    ├── soak.sh  soak-check.py  # §15.3: the soak run and its verdict
    ├── mkfont.py               # MC6847 character ROM → header
    └── vdg-ppm.c               # render the scenes; regenerates test/golden/
```

`tools/atm.py` (inspect and build ATM files) was planned and has not been
written.

`CMakeLists.txt` produces two targets from one source tree: the UF2, and a host
test binary that compiles `src/core/` with the system compiler and no SDK. The
core building under `-Wall -Wextra -Werror` on both toolchains is the mechanism
that keeps SDK dependencies from leaking downwards.

The UF2 comes in two builds, since 2026-09-28. The default is the
development build. It puts stdio on UART1 at GP4 and GP5, with the boot banner,
the heartbeat and every log line, and types what arrives there at the guest. The
tools that drive the board need it (hardware notes §2.7). `-DPICO_ATOM_UART=OFF`
is the build that ships. It has no UART: nothing is logged or read there, and
GP4 and GP5 are free for VIA port B, which is on by default (§7.4). A build
switch sets that default, `PICO_ATOM_PORT_B_DEFAULT`, so `settings_default()`
and the settings file's rewrite (§11.6) agree on it.

`src/core/` uses no dynamic allocation. All state is in `atom_t` and in
statically sized buffers from `config.h`, which makes the SRAM budget in §5 a
link-time fact rather than a hope (hardware notes §2.3).

---

## 15. Testing and verification

### 15.1 On the host, before any hardware

| Test | Standard |
|---|---|
| **Klaus Dormann `6502_functional_test`** | must run to completion. Non-negotiable; it is the difference between an emulator and a plausible one. |
| **Bruce Clark decimal mode test** | must pass, including NMOS flag behaviour. **Passes**, with all four flags checked over every operand pair and both carries, invalid BCD included. It is public domain and distributed as source, so the source is in the tree (`test/asm/6502_decimal_test.s`, Dormann's `.a65` in ca65 syntax, NMOS paths only, every `chk_` flag on) and `fetch-test-suites.sh` assembles it with cc65; `test_m6502_functional` runs it. A decimal-mode `ADC` that takes N from the result, not the intermediate, fails it. |
| Cycle-count table | every opcode's cycle count and page-cross penalty asserted against the published table. |
| MC6847 golden images | render fixed VRAM contents in each of the nine modes, both colour sets, compare to committed PPMs. Includes every SG6 pattern and an inverse-video text page. |
| 8255 | port C nibble separation, BSR writes, mode-nibble-vs-column-nibble independence. |
| Keymap | every PicoCalc code maps to exactly one Atom cell; no binding uses a chord the southbridge cannot deliver (§10.3); the table is a bijection where it claims to be. |
| Tape | ATM round trip; CUTS encode → decode round trip at the bit level. |
| Snapshot | save → load → state identical, bit for bit. |

A **trace-diff harness**: run the same ROM image for N instructions under
pico-atom's host build and under a reference Atom emulator, diff the per-instruction
`PC/A/X/Y/S/P/cycles` trace. The first divergence is almost always the bug, and
it finds problems no unit test is shaped to catch. **Built** (2026-09-26), with
Atomulator as the reference:

```sh
cmake --build build/host --target atom-trace
tools/trace/build-atomulator.sh ~/src/Atomulator     # -> out/trace/atomulator-trace
tools/trace-diff.py run --keys 'PRINT 2+2\n'           # boot, type, trace both, diff
```

`atom-trace` runs `atom_run_field` an instruction at a time; `atomulator-trace`
compiles Atomulator's `6502.c`, `8255.c`, `6522via.c` and `8271.c` where they
stand, against a stub Allegro header, and prints from the per-instruction hook
its debugger uses. Nothing of Atomulator is copied into this tree. Both take
the same four ROMs, found by SHA-1, and the same key script, pressed at the
same guest cycle (`tools/trace/keyscript.h`). Each line is the state before an
instruction, the cycle count and the three bytes at PC.

The two machines do not keep the same time: Atomulator's field is 262 lines of
64 cycles, 16,768, against the VDG's 16,667 (§12.1), so a loop polling FS or the
keyboard runs a different number of times on each side. The diff resyncs at the
nearest point where both agree again, in registers and in a shadow of the
return stack (two calls of one routine otherwise look alike), and classes what
lay between. A divergence right after a read of a timed input (`#B001`, `#B002`,
the VIA, the 8271) is `input` and expected, and so are the reset's S and a
polling `loop`. `values`, `path`, a trace that never resyncs, and any
instruction whose cycles differ outside Atomulator's known errata fail the run.
A read of port A or of port C's latch is not a timed input: a wrong bit in port A's
read-back, planted in `i8255.c`, fails the run.

What it found, on the boot, a BASIC program typed and run, `FPRINT SQR(2)*PI`
and `LIST` (7 million instructions each side), and on §15.3's soak program
(20 million): **no divergence in pico-atom**. Every one follows a keyboard or
FS read. It found Atomulator's timing wrong in six places, all against
the MCS6500 manual and our execution-checked table: zero page,X reads 3 cycles
not 4, zero page,X read-modify-writes 5 not 6, `INC`/`DEC abs,X` 6 not 7,
`CPX #` 3 not 2, `CPX abs` 3 not 4, and `TAY` 0 not 2. `trace-diff.py` carries
them as `ATOMULATOR_ERRATA`. The empty utility socket reads open bus here and
0 in Atomulator; its driver fills the socket with what an absolute load would
see here, because the floating-point ROM looks at `#A000` for a ROM.

Running the real ROMs on the host (`test_boot`, `test_tape`, `test_cassette`,
`test_disc`) remains the regression suite: it checks what the machine does,
and needs no second emulator. The harness is a tool, not a CTest, because it
needs an Atomulator checkout.

### 15.2 On hardware

Bring-up follows hardware notes §10, in that order: southbridge I²C, LCD,
keyboard, audio, filesystems. Each stage is a shippable milestone with its own
smoke test before the next one starts.

The per-driver checklist in hardware notes §10 applies verbatim and belongs in
the PR template. The items most likely to bite this project specifically:

- [ ] `spi_set_format()` and the `D/CX` write go **before** CS low — the 40 ns
      CS-high rule, and the one the notes say costs a week.
- [ ] Drain the SPI RX FIFO and clear the overrun flag after every DMA blit;
      restore 8-bit format.
- [ ] The LCD is never touched from an interrupt handler; timers set flags.
- [ ] Nothing masks interrupts around a blit.
- [ ] Audio ring power-of-two **and aligned**, hardware read wrap enabled.
- [ ] Re-arm read address **and** transfer count on chained DMA re-arm.
- [ ] `DMA_IRQ_0` at priority `0x40`; every refill-path function
      `__not_in_flash_func`.
- [ ] Re-apply the SPI baud rate and the audio carrier after any `clk_sys`
      change.
- [ ] Keyboard polled from the frame loop; I²C guarded by a busy flag; held
      state built from press/release.
- [ ] PCM underruns counted separately from late DMA refills.
- [ ] Physical board identity logged separately from the SDK build target.

### 15.3 Soak

A 30-minute battery-powered run with a BASIC program driving the display, sound
and keyboard, with UART capture throughout, asserting: zero I²C errors, zero
late DMA refills, zero PCM underruns, zero dropped key events, and a real-time
ratio that stays ≥ 1.0. The hardware notes' own soak found that all four of
those counters can stay clean while a fifth problem exists, which is why §12.3
has more counters than feel necessary.

`tools/soak.sh ELF [MINUTES]` runs it: capture, flash, type the program, and
type `H` and `4` at it every five seconds for the whole run (not `R`,
which BASIC's Escape test reads while the program has column 9 selected).
The program alternates a 256×192 page of random lines with a page of scrolling text,
rings the bell between them, and reads the keyboard matrix itself, so a missed
key never stalls it. It was checked on the host first, and trace-diffed against
Atomulator (§15.1). `tools/soak-check.py` then holds the log to this section:
one boot, heartbeats covering the run, the four counters zero on every
heartbeat, and `rt` never below 0.995 with a mean of at least 0.999. The guest
is paced by the audio queue (§12.2), so `rt` reads 1.000 or 0.999, the last
digit being truncation. The other counters must stay at zero too: undocumented
opcodes, dropped snapshots, the beeper's overflow, log lines dropped. Presents,
key events and speaker edges must grow. Battery is the operator's part: the
USB-C lead out. Since M13 the log shows it: core 1 reads the southbridge's
gauge every 20 s, as often as the MCU refreshes it, and each heartbeat ends
`battery 87%` or `battery 87% charging`, then the die's temperature (§12.3).
`soak-check.py` fails a run that shows charging, which is USB power, unless
`--usb` says it was meant to be on USB. The converse does not hold. The bit is
the charger's, and it clears once a full battery has finished charging with
USB still in (hardware notes §6). So a run that never shows charging is on
battery or on USB with the charge done. The operator still says which.

**Passed** on a Plus 2 W on battery, 2026-09-26, 17:34–18:05: 375
heartbeats over 31.2 minutes and one boot. `rt` was never below 0.999, mean
0.9996. Zero I²C errors, keys lost, underrun samples and late refills. Zero
undocumented opcodes, dropped snapshots, beeper overflow and log lines
dropped. 112,500 presents, 54,047 speaker edges, 340 keys typed over the UART
and 47 on the PicoCalc's own keyboard. Audio consumed held at 36,620–36,621 Hz,
bar 36,533 Hz on the first heartbeat, while audio started. The log is
`out/soak/soak-20260926-173300.log`. The first attempt stopped after one pass:
the program selected keyboard column 9 and left it selected, and BASIC's
Escape test (`#C504`) reads port B bit 5 in whatever column is selected, so the
`R` typed at it was Escape. The program now puts column 0 back on the line that
reads column 9, and the script types `H` and `4`. Don't press `R` during a run.

**At 300 MHz with a 4 MHz guest (§3.2, M13)** it passed on a Plus 2 W on USB
power, 2026-09-27, 10:46–11:17: 372 heartbeats over 31.2 minutes, one boot,
`battery 100% charging` on every heartbeat. `rt` was never below 0.999, mean
0.9995. Zero I²C errors, keys lost, underrun samples and late refills. Zero
undocumented opcodes, dropped snapshots, beeper overflow and log lines
dropped. 112,500 presents, 163,121 speaker edges, 340 keys typed over the UART
and 20 on the PicoCalc's keyboard. Audio consumed read 36,616–36,625 Hz
after the first heartbeat's 36,431, while audio started. That is wider than
the 150 MHz soak's 36,620–36,621, by ±0.013 %, with no underrun or late
refill. The spread is not explained yet. The capture garbled three
heartbeat lines, which `soak-check.py` now counts as the capture's rather
than as gaps. A heartbeat comes every 300 guest fields, so a stall cannot skip
one, and with `log dropped` at zero a missing one was lost on the host. The
die's temperature was not yet on the heartbeat during this run; it is now
(§12.3).

**On battery at 300 MHz with a 4 MHz guest** it passed on a Plus 2 W,
2026-09-27, 12:54–13:25, with the USB-C lead out: 373 heartbeats over 31.2
minutes, one boot, never charging, the gauge going from 100 % to 98 %. `rt` was
never below 0.999, mean 0.9995. Zero I²C errors, keys lost, underrun samples and
late refills. Zero undocumented opcodes, dropped snapshots, beeper overflow and
log lines dropped. 112,500 presents, 163,115 speaker edges, 340 keys typed over
the UART and 46 on the PicoCalc's keyboard. Audio consumed read 36,616–36,624 Hz
after the first heartbeat's 36,431, the same spread as on USB, so it is not the
supply. The die read 27 °C throughout. The capture garbled two heartbeat
lines. The log is `out/soak/soak-20260927-125312.log`.

---

## 16. Constants to confirm before coding

Everything in this section is stated from secondary knowledge and **must be
verified against a primary source before it becomes a `#define`**. They are
called out here rather than buried because a wrong constant in this list
produces a machine that boots and then misbehaves subtly — the most expensive
class of bug in emulation.

| Item | Source of truth | Confidence as written |
|---|---|---|
| 8255 port bit assignments (§2.3) | Atom service manual / theory of operation | high — cross-check anyway |
| MC6847 mode table (§2.4) | MC6847 datasheet | high |
| VDG mode bit order in port A bits 7–4 | Atom circuit diagram | **confirmed** — `A/G` is bit 4, `GM0`–`GM2` bits 5–7, read off the schematic. §2.3 had this right; an earlier §2.4 had `A/G` at bit 7 and has been corrected |
| Keyboard matrix cell assignments (10×6) | the kernel ROM's scan at `#FE71`, executed | **confirmed** — every cell pressed at the `>` prompt with and without SHIFT and the MOS's output read from VRAM; the table is in `keymap_picocalc.c` and `test_boot` re-checks it against the ROM |
| `OSLOAD`/`OSSAVE` entry addresses and page-2 vectors (§11.2) | the kernel ROM, disassembled and executed | **confirmed** — `#FFE0` is `JMP (#020C)`, `#FFDD` is `JMP (#020E)`, and reset points them at `#F96E` and `#FAE5`; `test_tape` runs both routines and holds the trap to what they leave |
| VDG field rate: 50 or 60 Hz on a UK Atom | Atom circuit diagram, VDG clock source; owners' accounts | **confirmed** — 60 Hz on every Atom, UK machines included: the MC6847 is an NTSC part, and UK owners had to adjust their TV's vertical hold to lock to it. Software times itself on it (BASIC's `WAIT` is one field sync, 1/60 s), so it is `ATOM_FIELD_HZ`, a constant; it was `atom_config_t.field_hz` while unverified |
| `FS` low interval, and the lines from `FS` to the first active line (§12.1) | MC6847 datasheet, `FS` timing | **confirmed** — the datasheet's figure 8: `tWFS` is 32 lines, `tPFS` 262. Figure 13: `FS` falls at the end of the 192 active lines, and the next active line is 13 blank and 25 top-border lines after the 26 + 6 it is low for (`ATOM_FS_LOW_LINES`, `ATOM_BLANK_LINES`). XRoar's `mc6847.h` agrees. It was ~6 % of a field, a guess, and that length did matter: ending the field at `FS`'s rise caught games mid-redraw |
| RAM blocks populated in a stock vs expanded Atom (§7.2) | Atom manual | medium |
| 8271 base address `#0A00` (§7.3) | the DOS ROM, disassembled and executed | **confirmed** — `#0A00`–`#0A02` and data at `#0A04` (`#E84F`), not `#0A00`–`#0A03` as first written; INT on NMI through `#0200`, which the DOS points at `#E87B` (`#EEEF`); `test_disc` runs the DOS against the model |
| VRAM byte wiring in alpha mode (§2.4) | the MOS and BASIC, executed | **confirmed** — bit 6 is `A/S` and `INT/EXT` (SG6), bit 7 is `INV`: the MOS's cursor is `#A0`, and `CLEAR 0` then `PLOT` writes `#40` plus one element bit per point; `test_boot` pins both |
| SG6 colour from bits 7:6 (§2.4) | MC6847 datasheet; a reference emulator | **confirmed** — the datasheet's `C1:C0` = `D7:D6`, so yellow/red (cyan/orange with `CSS`); `CLEAR 0` + `PLOT` compared by eye against another Atom emulator on 2026-09-22 |
| 2.4 kHz cassette reference period, port C bit 4 (§11.3) | Atom circuit diagram | **medium**: 416 cycles, 4 MHz ÷ 1664 = 2403.8 Hz, which MAME's Atom driver also uses. `ATOM_CASSETTE_REF_CYCLES`. The ROM reads a tape by its own loop timing, so only the speed of a signal-level save depends on it |
| MC6847 luminance levels for the mono palette (§8.7) | MC6847 datasheet, figure 10 and the DC characteristics | **high**: 0.72 V for black, 0.65 V for blue and red, 0.54 V for green, cyan, magenta and orange, 0.42 V for yellow and buff (typical values). The first draft, from memory, had blue and red at black's level; figure 10 puts them at white low. `mc6847_palette_mono` |
| The dark green and dark orange behind alphanumeric text (§8.7) | MC6847 datasheet, Display Modes (p. 18), for the colours; nothing yet for their level | **low** for the level: the datasheet names the colours and gives no voltage, and figure 10 shows only the border. A quarter of the ink is a guess, to be judged against a real Atom or a photograph of one. In mono they are black: black's Y with chroma the mono machine does not have. `VDG_DARK_GREEN`, `VDG_DARK_ORANGE` |
| 6522 shift rate under Φ2 (§7.4) | Rockwell R6522 datasheet, figure 23 (SR mode 2) | **high**: a bit every two cycles, 16 for a byte, as b-em shifts. The figure draws CB1's shift clock low for one Φ2 cycle and high for the next, eight pulses after the SR access; the text's "each Φ2 clock pulse" means those CB1 pulses. Nothing on the Atom uses these modes |
| MC6847 character ROM bitmap | datasheet figure or an extracted table | **confirmed** — taken verbatim from XRoar's extracted table and verified by rendering the full glyph set |
| What a 2 MHz Atom kept in wall time (§12.1) | Atom circuit diagram: which parts take Φ2 and which the crystal | **medium**: the VIA on Φ2; the VDG, the 2.4 kHz reference and the 8271 on their own clocks. A 2 MHz Atom was an owner's modification, so there is no single machine to check against |
| Whether the MOS reads a signal-level tape at 2 MHz (§12.1) | the kernel ROM, executed: `test_cassette` at 2 MHz | **confirmed**, 2026-09-26: it cannot. The ROM's `SAVE` at 2 MHz frames every byte and a 1 MHz machine loads it; played to the ROM at 2 MHz the same tape loads nothing. The deck plays only at 1 MHz and records at either (`atom_cassette_play`) |

The last one was settled the way this section asks. The table is XRoar's
`src/mc6847/font-6847.c`, byte for byte (`THIRD-PARTY.md`); the full 64-glyph
set was rendered with `tools/vdg-ppm` and read by eye — `@ABC`…`XYZ[\]^_` then
space through `?`, inverse video and both colour sets correct. What the data
carries rather than the renderer assuming it — 5 px in bits 5..1, rows 3..9 of
the cell, MC6847 glyph order — is asserted against the data in
`test/host/test_mc6847.c`, so a differently laid-out substitute fails loudly
instead of rendering plausible-but-wrong glyphs.

The keyboard matrix was settled from a primary source too, though not a
document: the kernel ROM. Its scan at `#FE71` walks row bit 5 down to bit 0 and
column 0 up to 9, counting `Y` down from `#3B`, so a key is
`Y = row × 10 + 9 − col`. Pressing each of the 60 cells at the prompt, with
and without SHIFT, and reading what the MOS wrote to VRAM gives the whole map,
including the five cells that no key uses (the MOS decodes them as control
codes `#08`–`#0C`) and the two arrow keys, which SHIFT reverses. The table is
in `keymap_picocalc.c`, and `test_boot` types every entry through the real MOS
whenever the ROMs are present.

---

## 17. Milestones

Each milestone ends with something that runs and something that is measured.

| # | Deliverable | Done when |
|---|---|---|
| **M0** | Skeleton: CMake, host + UF2 targets, CI, `config.h` | both targets build clean under `-Werror` — **done**; CI builds both on every push |
| **M1** | 6502 core, host only | Dormann and Clark tests pass; cycle table asserted — **done**, with Dormann's suite passing and the cycle table asserted by execution; Clark's test passes too, every flag checked, since 2026-09-26 (§15.1) |
| **M2** | Bus, 8255, VDG row generation, host only | golden images match for all nine modes — **done**, the images checked by eye before they were committed (`test/golden/`) |
| **M3** | Board bring-up: clocks, I²C, LCD, test pattern; the real core 0 slice loop, split at flyback (§12.1) | 256×192 rectangle at (32,64), all four corners verified; present time measured and compared to §8.4's estimate; a guest loop polling `FS` observes the low state and escapes — **done** 2026-09-22 on a Plus 2 W: the test pattern's corners, colour order and orientation checked by eye on the panel; a full redraw measured at 11.5 ms, wire-bound (§8.4); zero I²C errors |
| **M4** | **Atom boots.** ROMs from SD, display live, keyboard mapped | the `>` prompt accepts `PRINT 2+2` — **done** 2026-09-22 on a Plus 2 W: typed on the PicoCalc keyboard, answer read off the panel; `CLEAR 0` + `PLOT` draws an SG6 element of the right size |
| **M5** | Audio | integrator verified against a known frequency; underrun and late-refill counters both zero over 10 minutes — **done** 2026-09-22 on a Plus 2 W (§9.4) |
| **M6** | Tape phase 1 (ATM via OS traps), snapshots, menu | a downloaded `.atm` game loads and runs — **done** 2026-09-22 on a Plus 2 W: Galaxians, extracted from a `games1.dsk` image to `.atm`, loaded off the card by `LOAD "GALAXI"` (4,864 bytes in 5 ms) and was played; `SAVE`/`LOAD` round-tripped through the card; snapshots saved and restored from the menu; a tape chosen in the menu loaded by `LOAD ""` |
| **M6b** | Game keymaps (§10.5) | Galaxians played with the Games layout: `Left`/`Right` move, `]` fires, moving and firing at once — **done** 2026-09-23 on a Plus 2 W: Galaxians played with the layout, moving and firing at once (fire then on `Up`, moved to `]` after that run); Bouncing Babies played with a card layout its tape load chose |
| **M7** | Perf pass | real-time ratio measured and reported; SRAM placement of hot code measured per hardware notes §9.2, tier by tier, stopping where returns say to — **done** 2026-09-23 on a Plus 2 W: headroom 2.2–2.9× real time, 158–218 host cycles per guest instruction (§6.3); tier 2 ships, 1.12–1.19× for 25 KiB; tier 3 measured nothing; core 1's `sleep_us` was interrupting core 0, and fixing it was worth 1.11× |
| **M8** | Tape phase 2 (UEF at signal level), turbo clock | a UEF image that phase 1 cannot load, loads — **done** 2026-09-23 on a Plus 2 W: Chuckie Egg's two-part UEF, 45 blocks through its own BASIC loader, loaded at 2.7–2.8× under turbo and was played (§11.3). On the host, the kernel's own `SAVE`, recorded at signal level, loads back through its own `LOAD`, and a headerless block loads through a loader phase 1 never sees |
| **M9** | AtomDOS + 8271, 6522 VIA | an `.ssd` boots — **done** 2026-09-23 on a Plus 2 W: `*DOS`, `*CAT` off a 40-track image, `LOAD"INVADER"` read 19 sectors over tracks 35–37 and ran; `*SAVE` wrote the catalogue and two sectors and `*CAT` then listed the file; a disc changed from the menu was noticed and its catalogue read; Galaxians loaded off `games1.dsk` and was played. A track off the card takes 17–19 ms to read and 25–27 ms to write, with the guest parked; underruns and late refills stayed at zero. The VIA was fitted at M4 (§7.3); its shift register and handshake lines were completed at M10 |
| **M10** | The whole 6522 (§7.4); monochrome and the VDG border (§8.7) | every VIA mode driven through its pins on the host, and the new state through a snapshot; the mono palette and the border colour asserted by execution; on a Plus 2 W, both settings switched from the menu and seen on the panel, and the perf workloads measured against M9 — **done** 2026-09-23 on a Plus 2 W: both settings switched from the menu and seen on the panel; the perf workloads 3.6–6.2 % faster than M9, after the first build measured 2.5–4 % slower (§6.3) |
| **M11** | Recording at signal level (§11.3); the status line (§8.2); the menu's settings saved to the settings file (§11.6) | on the host, the ROM's own `SAVE` recorded by the core's recorder onto a new tape, a second file appended, both loaded back by name through the ROM's `LOAD`, and the core's output matching `test_cassette`'s independent decoder; a protected tape refused and left unchanged; `settings_rewrite` keeps comments, trailing comments, unparsed lines and line endings, refuses a duplicate key, and its output parses back to the settings it was given. On a Plus 2 W: a BASIC program `SAVE`d onto a new tape, loaded back by `LOAD` after a power cycle, and the `.uef` loaded in another Atom emulator; the status line seen on the panel through a tape load and a disc access, with the border on and off; a setting changed and saved from the menu, the machine powered up with it, and the file read on a computer with the user's comments intact; the recorder's hook measured against an M10 control with `perf-run.sh` — **done** 2026-09-26, but for the `.uef` in another emulator, which could not be got working. On the host every check passes, `SAVE ""` loading back by `LOAD ""` and a full tape too. On a Plus 2 W: a new tape made from the menu (`TAPE01.UEF`), a BASIC program `SAVE`d onto it, and after a power cycle `LOAD`ed and `RUN`; a read-only tape refusing to record; the status line showing a UEF's `PLAY`, `STOP` and position and `D0 …` after a disc access, gone at once on BREAK, with the border on and off and in colour and mono; settings saved from the menu with `TAPE01.UEF` in the deck, back after a power cycle, and the file read on a computer with its comments intact (a value put into an empty line landed in the comment column, fixed after, §11.6); the recorder's hook costing nothing measurable against a build without it, and M11 within 1 % of the tree before it (§6.3) |

| **M12** | The rest of the menu (§13.1): the Machine page, and a 2 MHz guest clock (§12.1); the About page; the perf line; Pause with the backlight dimmed. BASIC's OR, `|`, and the other shifted `@` `[` `\` `]` `^`, typable (§10.3) | **done** 2026-09-26. On a Plus 2 W on 2026-09-26: the four perf workloads at 2 MHz with zero underruns and zero late refills, written into §6.3; the perf line on and off at 1 MHz and M12 against M11, all within 0.7 %; BASIC's OR typed over the UART, `IF 5|3=7 P.$7` ringing the bell once and `IF 5|3=6` not. Checked by hand on the board the same day: each Machine setting changed, applied and seen from the guest, with a restart refused while recording, and the machine saved from the menu and back after a power cycle; the About page's hashes read against §11.1; Pause dimming the panel and silencing the guest, the resuming key not typed, the backlight restored, a playing tape stopping and carrying on, and `Alt`+`M` from a pause; the perf line on from the Display page. Then about ten minutes paused: the heartbeat across it read rt 0.008 for five guest seconds, about 625 s of wall time, with zero I²C errors before and after and zero underruns and late refills. On the host, all done on 2026-09-26, 31 CTest runs passing: `test_field`, `test_audio`, `test_i8271`, `test_boot`, `test_disc`, `test_tape`, `test_snapshot` and `test_cassette` each run again at 2 MHz as `_2mhz` (`PICO_ATOM_TEST_MHZ=2`). The bell measures 775.41 Hz against 775.19 Hz from its loop. `test_cassette_2mhz` answered §16: the ROM writes a tape at 2 MHz and cannot read one, so the deck plays only at 1 MHz. Asked for: `test_boot`'s sweep types `|`, `{`, `}`, `` ` `` and `~` through the MOS, and `PRINT 5|3` answers 7. `test_field`, `test_audio`, `test_i8271` and `test_boot` pass at 2 MHz as at 1. At 2 MHz, `FS` is seen and escaped once per 33,333-cycle field. The MOS bell is measured at the pitch its loop's cycle count gives at 2 MHz. `*CAT` and `*LOAD` work at 2 MHz. `test_cassette` at 2 MHz settles §16's tape question, and the deck does what the answer requires. `test_snapshot` refuses a file taken at the other clock and leaves the machine unchanged, and loads a pre-M12 file as 1 MHz. `test_settings` reads and rewrites `clock`, `utility`, `perf`, `upper_ram` and `dos`. `test_status` formats the perf line and `PAUSED`. On a Plus 2 W: each Machine setting changed and applied, and the change seen from the guest: RAM at `#4000` present or not, `*DOS` answering or not, the utility ROM's commands, and the bell an octave up with the heartbeat counting 2,000,000 guest cycles a second. A restart refused while recording. The machine saved from the menu and back after a power cycle. The four perf workloads at 2 MHz with zero underruns and zero late refills, written into §6.3. The perf line on and off at 1 MHz, within the ~2 % spread. The About page's hashes read against §11.1. `PRINT 5|3` typed on the PicoCalc keyboard answers 7. `Alt`+`P` dims the panel and silences the guest. A playing tape stops advancing and carries on after resume. The key that resumes is not typed. The backlight goes back to its level, the southbridge's own included. Ten minutes paused with zero I²C errors |
| **M13** | The host clock as a setting, 150 or 300 MHz (§3.2), and a 4 MHz guest on a 300 MHz host (§12.1); the battery gauge on the heartbeat (§15.3) | **built** 2026-09-27. On the host: 39 CTest runs passing, the clock tests a third time as `_4mhz`, `test_snapshot` holding 4 MHz files to version 2 and 1 and 2 MHz ones to M12's bytes, `test_settings` reading and rewriting `host_clock` and `clock = 4`. On a Plus 2 W: 150 MHz unchanged against M12; at 300 MHz the flash's QMI divider read 6 with RX delay 4, 50 MHz as at 150; the four perf workloads at 4 MHz with zero underruns, late refills, dropped snapshots and I²C errors, written into §6.3; §15.3's soak at 300 MHz and 4 MHz passing on USB power and then on battery; the gauge reading `100% charging` on USB and then `100%`, still on USB, once the charge finished; the die temperature on the heartbeat, from 23 °C at 150 MHz idle to 27 °C under load at 300. Checked by hand the same day: *Pico clock* set to 300 MHz on the Machine page and applied, the Pico restarting at 300 MHz with the guest at 1 MHz, and a game loaded from a UEF, at about twice 150 MHz's turbo by eye (§11.3), and played without a problem. Also by hand: the About page reading 300 MHz, 4 MHz offered and applied on the Machine page, and *Pico clock* set back to 150 MHz taking the 4 MHz guest to 2 |

M4 is the milestone that matters; everything before it is scaffolding and
everything after it is refinement.

Work between M10 and M11, each section saying what was
checked on the device: the settings file (§11.7); the field split at the VDG's own 262 lines and
BASIC's `RND` seeded from the board (§12.1, §7.2); powering up in mono with
the border on, and the border drawn as a frame (§8.7); the host's `Shift` as
the Atom's SHIFT line on its own (§10.3); and BREAK resetting the VIA and the
disc controller (§6.4). What was left, recording at signal level, the status
band and saving menu changes, is M11. M12 finishes §13's menu. Snow and the
scaled display, which the menu once listed, are dropped (§8.2, §8.6).

---

## 18. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| **Core 1 saturates during full-screen scrolling** (§8.4) | visible stutter in the most common BASIC operation | the snapshot pool drops what core 1 cannot present, and the heartbeat counts it. **Retired for scrolling**: 0 dropped over 3,300 fields of it at M10. A whole graphics screen rewritten every field is unmeasured |
| **Keyboard chords the southbridge cannot deliver** (§10.3) | some Atom keys unreachable | keymap is a data table validated by a host test that knows the swallowed-chord list; Alt layer as the escape hatch |
| **Wrong keyboard matrix transcription** (§16) | machine types the wrong characters; looks like a CPU bug | **retired**: settled by executing the kernel ROM's scan at M4, and `test_boot` types every entry through the real MOS |
| **Field rate 50 vs 60 Hz** (§16) | timing-sensitive software runs at the wrong speed | was configuration while unverified; settled at 60 Hz and now a constant |
| **MOS vector addresses wrong** (§11.2) | tape loading silently fails or corrupts | **retired**: read off the kernel ROM and held to its own routines by `test_tape`; phase 2 signal-level tape does not depend on them at all |
| **Tearing on full-screen change** (§8.5) | cosmetic | accepted; band presents localise it. No TE line exists to fix it with |
| **SRAM growth past budget** | link failure, or worse, a heap that fails at runtime | `config.h`, and CI prints `arm-none-eabi-size` on every build (it reports, it does not fail); §5 measured 49 % used, 51 % headroom, at 2026-09-25 |
| **Shipping ROMs** | licence violation | user supplies ROMs; the build has no ROM binaries and a boot without them shows a page naming what is missing |
| **300 MHz corrupts data** (§3.2) | silent, intermittent | off by default, and a setting the user chooses. The hardware notes' §3 checklist is applied in `board_init_clocks()`: the rail first, the flash's QMI timing scaled so it runs at the bootrom's 50 MHz, and every derived rate re-derived. §15.3's soak ran at 300 MHz and 4 MHz, on USB and on battery (§17). The die's temperature is on the heartbeat (§12.3): 23 °C idle at 150 MHz, 27 °C under load at 300, on USB at room temperature |
| **A 2 or 4 MHz guest leaves core 0 no margin** (§12.1) | audio underruns in the heaviest workloads | measured, not estimated: 86 % of core 0 idle at 2 MHz on 150 and 85 % at 4 MHz on 300, with zero underruns across the four workloads at each (§6.3), and the perf line (§13.1) shows the margin live. 4 MHz on a 150 MHz host is not offered, and a file that asks for it gets 2 |

---

## 19. References

**The guest**

- Acorn Atom Technical Manual and circuit diagram — 8255 wiring, keyboard
  matrix, VDG mode bit order.
- Motorola **MC6847** datasheet — mode table, SG6 encoding, character ROM,
  field timing.
- MOS/BASIC ROM disassemblies — entry points, page-2 indirection vectors.
- Klaus Dormann, *6502 functional tests*; Bruce Clark, *Decimal mode in the
  NMOS 6502*.
- Existing Atom emulators (Atomulator, Wouter Ras's emulator) — for
  cross-checking behaviour and for the trace-diff harness of §15.1, not for
  copying code. Atomulator, David Banks' fork at
  <https://github.com/hoglet67/Atomulator>, is also where §11.1 sends a user
  for ROM images.
- **XRoar**, Ciaran Anscomb, <https://www.6809.org.uk/xroar/> — the source of
  the MC6847 character ROM table in `src/core/mc6847_font.c`, taken verbatim
  under the GPL-3.0-or-later. See [`THIRD-PARTY.md`](../THIRD-PARTY.md).

**The host** — all collected in [`hardware-notes.md`](hardware-notes.md) §11:
the ClockworkPi PicoCalc repository and mainboard schematic, the ST7365P
controller specification, the keyboard MCU source, and the RP2350 datasheet and
Pico SDK documentation.
