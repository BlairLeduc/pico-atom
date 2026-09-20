# pico-atom — design

An **Acorn Atom** emulator for the **ClockworkPi PicoCalc**, written in C against
the Raspberry Pi Pico SDK.

Companion document: [`hardware-notes.md`](hardware-notes.md). Section references
of the form §4.7 point there unless stated otherwise; that document is the
authority on the host, this one on the guest and on the shape of the code.

**Status:** design, pre-implementation. Nothing here has been measured on
hardware yet; every performance figure below is derived arithmetically from the
measurements in the hardware notes and is labelled as an estimate. §16 lists the
constants that must be confirmed against primary sources before they are typed
into a header.

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
  §8.6). Modelled as an option, off by default.
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
| `#8000`–`#97FF` | 6 KiB | **Video RAM**, read by the VDG |
| `#9800`–`#9FFF` | 2 KiB | Video RAM aperture, unpopulated on a stock machine |
| `#A000`–`#AFFF` | 4 KiB | Utility ROM socket |
| `#B000`–`#B003` | 4 B | **INS8255 PPI** |
| `#B400`–`#B403` | 4 B | Expansion / printer port |
| `#B800`–`#B80F` | 16 B | 6522 VIA (optional) |
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

`A/G`, `GM2..GM0` (port A bits 7..4) and `CSS` (port C bit 3) select the mode.
Atom BASIC's `CLEAR n` maps onto these:

| `A/G` | `GM2:0` | VDG mode | Resolution | Colours | VRAM | Atom |
|:--:|:--:|---|---|---:|---:|---|
| 0 | — | Alpha / SG4 | 32×16 chars | 2 or 8 | 512 B | `CLEAR 0` |
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

In alpha mode the Atom wires VRAM data bits 7 and 6 to the VDG's `A/S` and `INV`
pins, so per character byte:

- bit 7 = 0 → alphanumeric; bit 6 = 1 → inverse video; bits 5–0 select one of 64
  glyphs from the VDG's internal 5×7-in-8×12 character ROM.
- bit 7 = 1 → **semigraphics 4**: bits 3–0 are the four quadrant on/off flags,
  bits 6–4 the colour. This is where the Atom's chunky block graphics come from,
  and it is why plotting in `CLEAR 0` works at all.

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

An RP2040 build is plausible — the budget fits 264 KiB — but the display gets
slower, not the CPU: at the RP2040's supported 200 MHz the SPI divider lands on
**50 MHz** rather than 75 (hardware notes §3), stretching every present by half
again. Treat it as a stretch goal and keep the code free of RP2350-only
assumptions where that costs nothing.

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

A **300 MHz turbo build** is a compile-time option, not a runtime one: it is the
only other free SPI divider point, it needs `VREG_VOLTAGE_1_20` raised before
the PLL and lowered after, and it is outside the RP2350 datasheet. Its purpose
is a 2–4 MHz "turbo Atom" mode, not the stock machine. If it is ever enabled,
every derived clock in the hardware notes' §3 checklist must be recomputed: SPI
baud and the PWM audio carrier are the two this project uses.

### 3.3 Peripheral assignment

| Resource | Owner | Notes |
|---|---|---|
| `spi1` @ 75 MHz | LCD | CS/DCX/RST as plain GPIO (§4.2) |
| `spi0` @ 400 kHz → 25 MHz | SD card | separate instance, no CS arbitration with the LCD |
| `i2c1` @ 10 kHz | Southbridge | keyboard, backlight, battery |
| PWM slice from `pwm_gpio_to_slice_num(26)` | Audio | both channels, one 32-bit compare word |
| DMA ch. 0 | LCD blit | **polled, no IRQ** (§4.6) |
| DMA ch. 1, 2 | Audio ring | chained pair, `DMA_IRQ_0` at priority `0x40` |
| DMA ch. 3 | SD (optional) | |
| Core 0 | 6502, 8255, VDG snapshot, audio synthesis | |
| Core 1 | LCD blit, southbridge I²C, SD | |

DMA channels and the IRQ owner are claimed explicitly at init, not left to
allocation order (hardware notes §10).

---

## 4. Architecture

### 4.1 Layering

```
      ┌──────────────────────────────────────────────────────┐
      │  port/   PicoCalc + Pico SDK                          │
      │  main · board · lcd · display · audio · kbd · sd · ui  │
      └───────────────┬──────────────────────────────────────┘
                      │  narrow, synchronous interface
      ┌───────────────▼──────────────────────────────────────┐
      │  core/   pure C, no SDK, host-buildable               │
      │  m6502 · bus · i8255 · mc6847 · via6522 · tape        │
      │  keymatrix · snapshot                                 │
      └──────────────────────────────────────────────────────┘
```

`core/` knows nothing about DMA, I²C, SPI or `pico/stdlib.h`. It exposes:

```c
void     atom_reset(atom_t *m);
uint32_t atom_run(atom_t *m, uint32_t cycles);   /* returns cycles actually run */
void     atom_key_set(atom_t *m, uint8_t row, uint8_t col, bool down);
void     atom_key_mods(atom_t *m, bool shift, bool ctrl, bool rept);
void     atom_field_sync(atom_t *m, bool in_flyback);
size_t   atom_audio_drain(atom_t *m, int16_t *dst, size_t max);
const uint8_t *atom_vram(const atom_t *m);       /* 6 KiB */
uint8_t  atom_vdg_mode(const atom_t *m);         /* A/G, GM2:0, CSS packed */
```

That interface is the seam every test in §15 exercises, and it is the reason the
6502 can be validated on a workstation against Klaus Dormann's functional tests
before any hardware exists.

### 4.2 Core split

Following hardware notes §9.5 — core 1 owns the slow peripherals:

| | Core 0 | Core 1 |
|---|---|---|
| Owns | 6502, 8255, VDG state, tape, audio synthesis, SD requests | LCD, southbridge I²C, SD transfers |
| Per field | run ~16,667 guest cycles, emit PCM, snapshot VRAM | expand snapshot → RGB565 bands → DMA; poll keyboard |
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
core 1, when idle, takes the newest `ready` buffer, marks it `rendering`, and
frees it on completion. Any older `ready` buffer is dropped unrendered — that is
§12.2's superseded-snapshot rule, made explicit.

**Three buffers, not two, and the third one is the whole point.** Core 1's worst
case is ~17–18 ms (§8.4) against core 0's 16.7 ms field, so the consumer can be
a full field behind. With two buffers, core 0 would at that moment have to
either block — which corrupts the simulation schedule that audio is paced
against — or rewrite a `ready` buffer core 1 may be claiming in the same
instant, which is a race no amount of care in the renderer can see. The third
buffer costs 6 KiB out of ~370 KiB spare and deletes the entire class of
problem; do not economise here.

State transitions are published under a single SIO spinlock. RP2350 has no
compare-and-swap, and "explicit ownership" is not a synchronisation primitive —
local interrupt masking is not a multicore lock (hardware notes §9.5). No locks
are held across a blit, and core 1 never reads guest RAM while the 6502 runs.
Between service iterations core 1 waits on a 20 µs hardware timer rather than
spinning on shared state.

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

| Item | Bytes | Note |
|---|---:|---|
| Guest address space, flat 64 KiB | 65,536 | direct index; simplest and fastest (§7.1) |
| Page descriptor table, 256 × 2 × 4 B | 2,048 | read and write pointers |
| VDG snapshots ×3, 6 KiB each | 18,432 | core 0 → core 1 handoff (§4.2) |
| Presented shadow | 6,144 | dirty-band diffing |
| Mode expansion LUT | 8,192 | rebuilt on mode/`CSS` change (§8.3) |
| RGB565 line buffers, 2 × 320 × 2 B | 1,280 | DMA ping-pong (§4.6) |
| MC6847 character ROM | 768 | 64 glyphs × 12 rows |
| Audio DMA ring, 2 halves × 128 frames × **2 slots** × 4 B | 2,048 | power-of-two, aligned, hardware wrap |
| PCM software queue, 1024 × 4 B | 4,096 | ~28 ms (§5.8) |
| 6502 + bus hot code in SRAM | ~16,000 | `__not_in_flash_func` (§9.2) |
| Display + audio hot code in SRAM | ~6,000 | |
| FatFs + SD buffers | ~2,500 | |
| Stacks, both cores | 8,192 | |
| Heap, UI, perf counters, misc | ~16,000 | |
| **Total** | **~154 KiB** | **30 % of 520 KiB** |

Two observations worth acting on:

- There is enough slack to hold a **second 64 KiB guest image** for instant
  snapshot restore, and still be under 45 %.
- The budget also fits an RP2040's 264 KiB at ~58 %, which is what keeps §3.1's
  stretch goal honest.

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
    bool     nmi_edge;
    bool     reset_pending;  /* BREAK key */
} m6502_t;
```

Cycles accumulate into `m->cycles`; the bus layer (§7) is what advances devices,
so the CPU core itself has no notion of a device. `atom_run(m, n)` executes whole
instructions until at least `n` cycles have elapsed and returns the true count,
which the caller carries forward as a debt — a deterministic pattern that keeps
long-run timing from drifting.

### 6.3 Budget

Estimate, to be replaced with a measurement (hardware notes §9.1):

| | |
|---|---:|
| Guest cycles per second at 1 MHz | 1,000,000 |
| Mean guest cycles per instruction | ~3.2 |
| Guest instructions per second | ~313,000 |
| Host cycles available per guest instruction at 150 MHz | ~480 |
| Expected host cycles per guest instruction, SRAM-resident | 40–80 |
| **Expected core 0 load from the 6502** | **~8–17 %** |

Even at four times that, the 6502 is not the constraint. This is what pays for
cycle-accurate tape at signal level (§11.3) and for a turbo mode.

The interpreter, the bus dispatch and the 8255 handlers all go behind
`__not_in_flash_func`. Per hardware notes §9.2, this is worth ~1.7× on large
branchy code — and an instruction interpreter is the canonical large branchy
workload. The same section's warning applies: **re-measure the whole profile
after every move**, because each relayout changes what is left in flash.

### 6.4 Interrupts and reset

- `RES` — the Atom's **BREAK key is wired to reset**, so the UI maps a host key
  to `reset_pending` rather than synthesising anything cleverer.
- `IRQ` — level-sensitive, OR of the 6522 VIA (if fitted) and the 8271 FDC (if
  fitted). Modelled as a bitmask so sources compose correctly; an `irq_lines`
  of zero deasserts.
- `NMI` — edge-triggered, latched. No standard Atom peripheral drives it; it
  exists for expansion and for the debugger.

---

## 7. Bus and memory map

### 7.1 Fast path

A flat `uint8_t ram[65536]` backs the entire address space. Reads of RAM and ROM
are a single indexed load. Writes and I/O go through a 256-entry page table:

```c
typedef struct {
    uint8_t *write;   /* NULL → not writable, or I/O */
    uint8_t  flags;   /* PAGE_IO | PAGE_VRAM | PAGE_ROM */
} page_t;
```

Write dispatch:

```c
static inline void bus_write(atom_t *m, uint16_t a, uint8_t v) {
    page_t *p = &m->page[a >> 8];
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

Reads take the same shape, with a fast path for everything outside
`#A000`–`#BFFF`.

**A page-granular fast path cannot express a sub-page device, and the Atom has
one.** With AtomDOS enabled the 8271 sits at `#0A00`–`#0A03` (§7.3), inside a
page that is otherwise RAM. If page `#0A` keeps a non-NULL `write`, those four
addresses take the fast RAM store and the FDC is never reached — silently, with
the disc system simply not responding. So when AtomDOS is enabled, page `#0A` is
marked `PAGE_IO` with `write = NULL`, and `bus_write_slow` splits it:

```c
/* page #0A, AtomDOS enabled: 4 bytes of FDC, 252 bytes of ordinary RAM */
if ((a & 0xFFFC) == 0x0A00) fdc_write(m, a & 3, v);
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
| Video | `#8000`–`#97FF` | present |
| Video aperture | `#9800`–`#9FFF` | absent |

Unpopulated pages read as open bus. Modelling open bus as "last value on the
bus" rather than `0xFF` costs one field in the struct and occasionally matters;
do it.

### 7.3 I/O decoding

`#B000`–`#BFFF` is decoded by mask, not equality, because the Atom decodes it
partially and the 8255 mirrors every four bytes:

| Mask test | Device |
|---|---|
| `(a & 0xFC00) == 0xB000` | 8255, register `a & 3` |
| `(a & 0xFC00) == 0xB400` | expansion / printer |
| `(a & 0xFC00) == 0xB800` | 6522 VIA, register `a & 15` |
| `(a & 0xFFFC) == 0x0A00` | 8271 FDC (AtomDOS builds only) |

The 8271 at `#0A00` sits inside RAM space, which is unusual and easy to get
wrong: with AtomDOS enabled, four bytes of page `#0A` stop being RAM.

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
pixel, not per present". A nearest-neighbour 320×240 at (0,40) is offered as an
option; it is 56 % more pixels on the wire for a 1.25× stretch with visibly
uneven pixel doubling.

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

Alpha/SG4 mode takes a different generator — per character cell, select glyph
row from the character ROM or synthesise the SG4 quadrant pattern, then expand
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

**This is the machine's real constraint.** A field is 16.7 ms; a full-screen
redraw plus a 4–5 ms southbridge poll is ~18 ms, so core 1 saturates when the
whole screen changes every field. Mitigations, in the order they are applied:

- Poll the keyboard every **second** field (30 Hz). Still well inside the
  southbridge's 2.5 s bus-reset timeout (§6.1) and it halves the fixed I²C cost.
- Drop to **30 Hz presentation** automatically when sustained full-screen change
  is detected. The 6502 keeps running at 60 fields/s; only presentation is
  decimated (hardware notes §9.6, lever 2). Atom BASIC scrolling is the case
  this exists for.
- Never resend the top or status bands.

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
well-behaved software behaves. Reproducing the *interference* is a v3 option
(`--snow`), off by default: it is authentic, and it is also ugly and slow.

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
ninety seconds.

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
          KM_NONE  — no matrix cell; handled by the UI layer          */
```

Keys the Atom has and the PicoCalc does not get a **`Alt` layer**. `Alt` is
chosen because the Atom has no Alt key, so nothing is stolen from the guest:

| Atom key | PicoCalc binding | Why |
|---|---|---|
| `COPY` | `Alt`+`C` | no PicoCalc equivalent |
| `REPT` | `Alt`+`R` | separate 8255 line, port C bit 6 |
| `BREAK` (reset) | `Alt`+`K` | destructive; must not be a single keypress |
| `LOCK` | `Alt`+`L` | |
| `@` `[` `\` `]` `^` | direct where present, `Alt` layer otherwise | confirm against the installed keymap |
| Emulator menu | `Alt`+`M` | §13 |

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

---

## 11. Storage, tape and disc

### 11.1 SD card layout

```
/atom/
  roms/       akernel.rom  abasic.rom  afloat.rom  dosrom.rom  utility.rom
  tapes/      *.atm  *.uef  *.tap
  discs/      *.ssd  *.dsk
  snaps/      *.psnap
  pico-atom.cfg
```

ROMs are **not shipped** (§1). If `/atom/roms/` is missing or incomplete, the
emulator boots to its menu with an explanatory screen rather than into a dead
machine — a blank screen is the single most expensive failure mode to debug on
this hardware.

SD access follows hardware notes §7.1: 400 kHz for init then 25 MHz requested,
card detect on GP22 active-low with a pull-up, 512-byte blocks, SDSC byte
addressing distinguished from SDHC block addressing, and **all card work done at
a defined application boundary** — the menu, with the guest paused — because
write latency can exceed both the field and the audio deadline.

### 11.2 Tape, phase 1: OS-level trapping

Fast and simple: watch for `PC` entering the MOS `OSLOAD` / `OSSAVE` entry
points, service the request from a `.atm` file on SD, set up the registers and
`RTS`. ATM is the natural container — 16-byte name, load address, execution
address, length, then data — and it is how most Atom software is archived.

The cost is that it only works with a stock MOS, and the entry addresses must be
confirmed against a MOS disassembly rather than remembered (§16). Intercepting
at the **page-2 indirection vectors** rather than the ROM entry points is the
more robust variant and is preferred if the vector table proves to cover the
calls we need.

### 11.3 Tape, phase 2: signal level

The honest path, and affordable because §6.3 leaves 80 % of core 0 idle: model
the cassette input bits (port C bits 4–5) as a real 300 baud CUTS waveform
decoded from a `.uef` or `.tap` file, clocked in guest cycles.

- Works with any MOS, any loader, and anything with non-standard block timing.
- Turbo loading is free: speed the guest clock during a load and the whole thing
  scales, because the decoder is driven by guest cycles rather than wall time.
- Saving is the inverse: sample port C bit 0 at the guest cycle rate and encode.

The cassette *audio* is not synthesised into the speaker mix by default (§9.3).

### 11.4 Disc, phase 3

AtomDOS: the `dosrom.rom` at `#E000` plus an 8271 FDC at `#0A00`, backed by
`.ssd`/`.dsk` images. The 8271 needs command/result phases, a seek model and an
IRQ line; it is a self-contained piece of work and it is not on the critical
path to a useful emulator.

### 11.5 Snapshots

Whole-machine state — 64 KiB guest image, CPU, 8255, VDG mode, tape position —
is ~72 KiB. Written to `/atom/snaps/` from the menu, via a temporary file and an
explicit publish step, because a filesystem rename alone is not proof of
power-loss atomicity (hardware notes §7.1).

### 11.6 Internal flash

Configuration only, written **only from the menu**, with audio stopped and via
`flash_safe_execute` with `flash_safe_execute_core_init()` on core 1. No flash
writes inside the frame loop, ever: erase takes tens of milliseconds with XIP
offline and interrupts masked, which is longer than the audio deadline and is
the one accepted violator of it (hardware notes §5.4, §7.2).

---

## 12. Timing and synchronisation

### 12.1 The guest clock

| | |
|---|---:|
| Guest CPU | 1,000,000 cycles/s |
| VDG field rate | 60 Hz nominal (§16 — confirm) |
| Guest cycles per field | 16,667 |
| `FS` (port C bit 7) low for | the flyback interval, ~6 % of a field |

Core 0 runs in **field-sized slices** with cycle-debt carry-forward:

```c
budget += CYCLES_PER_FIELD;
budget -= atom_run(&m, budget);     /* returns true cycles; debt carries */
atom_field_sync(&m, true);  ... atom_field_sync(&m, false);
snapshot_publish();
```

Within a slice, the audio integrator emits a sample every 27.31 guest cycles via
a fixed-point accumulator, so audio and CPU share one clock by construction and
cannot drift apart.

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
§5.8) so that muting does not change timing. The fallback path, for a build with
audio disabled entirely, paces on `time_us_64()` against an absolute field
deadline — absolute, not incremental, so a late field does not accumulate.

Presentation is decoupled: core 1 presents the most recent published snapshot
and drops superseded ones (hardware notes §9.6, lever 2). Simulation timing is
never sacrificed to presentation.

### 12.3 Instrumentation

A perf block, reported over UART1 (115200 8-N-1, TX GP4, RX GP5) and summarised
in the status band:

| Counter | Why |
|---|---|
| Real-time ratio (guest s / wall s) | the headline number |
| Host cycles per guest instruction | §6.3's estimate, verified |
| Present ms, dirty bands, dirty pixels | §8.4's budget, verified |
| Fields presented / fields simulated | is the 30 Hz decimation engaging? |
| **PCM underrun samples** | producer starvation |
| **Late DMA refills** | consumer starvation — a different bug (§5.8) |
| I²C errors, key events dropped | southbridge health |
| Die temperature | free, and it catches the 300 MHz build misbehaving |

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

| Menu | Does |
|---|---|
| Tape | attach/detach an image, rewind, play, record, position |
| Disc | attach/detach drive 0/1 (phase 3) |
| Snapshot | save, load, delete |
| Machine | RAM population, ROM set, VIA/AtomDOS present, guest clock (1/2/4 MHz) |
| Display | native 256×192 vs scaled 320×240, colour set override, snow |
| System | backlight, volume, perf overlay, board and ROM identification, about |

The backlight is southbridge register `0x05`, stepped to multiples of 16 and
clamped to 16–240, so a fade is 15 steps and not 256 (hardware notes §4.11).
Dimming on pause costs one I²C transaction and is worth it.

The overlay draws into the top and status bands plus, when it needs the space, a
region of the Atom rectangle. **Dismissing it does not "restore" pixels.**
`shadow` holds 6 KiB of guest VRAM bytes, not a decoded 256×192 panel image — in
graphics modes one byte expands to 8 or 16 pixels, and alpha mode additionally
needs the character ROM and the presented mode — so there is nothing there to
copy back. Instead the overlay's bands are marked fully dirty and the normal band
generator repaints them from `shadow` plus the presented mode on the next
present. That is zero new code, and it is why the mode byte belongs in the shadow
state (§8.4).

---

## 14. Repository layout and build

```
pico-atom/
├── CMakeLists.txt              # pico-sdk build; host build behind PICO_ATOM_HOST
├── docs/
│   ├── hardware-notes.md
│   └── design.md
├── src/
│   ├── core/                   # portable C11, no SDK, no allocator
│   │   ├── config.h            # every fixed capacity, in one place
│   │   ├── m6502.c/.h
│   │   ├── bus.c/.h
│   │   ├── atom.c/.h
│   │   ├── i8255.c/.h
│   │   ├── mc6847.c/.h         # mode decode, LUT build, row generation
│   │   ├── mc6847_font.h       # 64-glyph character ROM
│   │   ├── via6522.c/.h
│   │   ├── keymatrix.c/.h
│   │   ├── keymap_picocalc.c   # data table (§10.3)
│   │   ├── tape.c/.h           # ATM, UEF, CUTS encode/decode
│   │   └── snapshot.c/.h
│   ├── port/                   # PicoCalc + SDK
│   │   ├── main.c              # bring-up order per hardware notes §10
│   │   ├── board.c/.h          # clocks, vreg, board identification
│   │   ├── lcd.c/.h            # ST7789P init, windows, DMA blit
│   │   ├── display.c/.h        # snapshot diff, band present, status band
│   │   ├── audio.c/.h          # PWM slice, chained DMA, SPSC queue
│   │   ├── kbd.c/.h            # southbridge poll, normalisation
│   │   ├── sd.c/.h  fs.c/.h    # SPI0, FatFs
│   │   ├── ui.c/.h
│   │   └── perf.c/.h
│   └── roms/embedded.h         # placeholders only; no ROM binaries in-tree
├── test/
│   ├── host/                   # CTest
│   │   ├── test_m6502.c        # Dormann functional, Clark decimal
│   │   ├── test_mc6847.c       # golden images, all nine modes
│   │   ├── test_i8255.c
│   │   ├── test_keymap.c
│   │   └── test_tape.c
│   └── golden/                 # committed reference PPMs
└── tools/
    ├── mkfont.py               # MC6847 character ROM → header
    ├── atm.py                  # inspect/build ATM files
    └── trace-diff.py           # compare a trace against a reference emulator
```

`CMakeLists.txt` produces two targets from one source tree: the UF2, and a host
test binary that compiles `src/core/` with the system compiler and no SDK. The
core building under `-Wall -Wextra -Werror` on both toolchains is the mechanism
that keeps SDK dependencies from leaking downwards.

`src/core/` uses no dynamic allocation. All state is in `atom_t` and in
statically sized buffers from `config.h`, which makes the SRAM budget in §5 a
link-time fact rather than a hope (hardware notes §2.3).

---

## 15. Testing and verification

### 15.1 On the host, before any hardware

| Test | Standard |
|---|---|
| **Klaus Dormann `6502_functional_test`** | must run to completion. Non-negotiable; it is the difference between an emulator and a plausible one. |
| **Bruce Clark decimal mode test** | must pass, including NMOS flag behaviour. |
| Cycle-count table | every opcode's cycle count and page-cross penalty asserted against the published table. |
| MC6847 golden images | render fixed VRAM contents in each of the nine modes, both colour sets, compare to committed PPMs. Includes an SG4 pattern and an inverse-video text page. |
| 8255 | port C nibble separation, BSR writes, mode-nibble-vs-column-nibble independence. |
| Keymap | every PicoCalc code maps to exactly one Atom cell; no binding uses a chord the southbridge cannot deliver (§10.3); the table is a bijection where it claims to be. |
| Tape | ATM round trip; CUTS encode → decode round trip at the bit level. |
| Snapshot | save → load → state identical, bit for bit. |

Add a **trace-diff harness**: run the same ROM image for N instructions under
pico-atom's host build and under a reference Atom emulator, diff the per-instruction
`PC/A/X/Y/S/P/cycles` trace. The first divergence is almost always the bug, and
it finds problems no unit test is shaped to catch.

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
| VDG mode bit order in port A bits 7–4 | Atom circuit diagram | **medium** — the `A/G`/`GM2:0` ordering must be read off the schematic |
| Keyboard matrix cell assignments (10×6) | Atom service manual keyboard table | **low** — transcribe; do not reconstruct |
| `OSLOAD`/`OSSAVE` entry addresses and page-2 vectors (§11.2) | MOS disassembly | **low** |
| VDG field rate: 50 or 60 Hz on a UK Atom | Atom circuit diagram, VDG clock source | **medium** — affects §12.1 throughout |
| RAM blocks populated in a stock vs expanded Atom (§7.2) | Atom manual | medium |
| 8271 base address `#0A00` (§7.3) | AtomDOS documentation | medium |
| MC6847 character ROM bitmap | datasheet figure or an extracted table | transcribe, then verify by golden image |

The verification method for the last one is the honest one: render the full
64-glyph set, photograph a real Atom or compare against a reference emulator's
output, and commit the result as a golden image.

---

## 17. Milestones

Each milestone ends with something that runs and something that is measured.

| # | Deliverable | Done when |
|---|---|---|
| **M0** | Skeleton: CMake, host + UF2 targets, CI, `config.h` | both targets build clean under `-Werror` |
| **M1** | 6502 core, host only | Dormann and Clark tests pass; cycle table asserted |
| **M2** | Bus, 8255, VDG row generation, host only | golden images match for all nine modes |
| **M3** | Board bring-up: clocks, I²C, LCD, test pattern | 256×192 rectangle at (32,64), all four corners verified; present time measured and compared to §8.4's estimate |
| **M4** | **Atom boots.** ROMs from SD, display live, keyboard mapped | the `>` prompt accepts `PRINT 2+2` |
| **M5** | Audio | integrator verified against a known frequency; underrun and late-refill counters both zero over 10 minutes |
| **M6** | Tape phase 1 (ATM via OS traps), snapshots, menu | a downloaded `.atm` game loads and runs |
| **M7** | Perf pass | real-time ratio measured and reported; SRAM placement of hot code measured per hardware notes §9.2, tier by tier, stopping where returns say to |
| **M8** | Tape phase 2 (UEF at signal level), turbo clock | a UEF image that phase 1 cannot load, loads |
| **M9** | AtomDOS + 8271, 6522 VIA | an `.ssd` boots |

M4 is the milestone that matters; everything before it is scaffolding and
everything after it is refinement.

---

## 18. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| **Core 1 saturates during full-screen scrolling** (§8.4) | visible stutter in the most common BASIC operation | 30 Hz presentation decimation is designed in from M3, not retrofitted; measure at M3 before M4 depends on it |
| **Keyboard chords the southbridge cannot deliver** (§10.3) | some Atom keys unreachable | keymap is a data table validated by a host test that knows the swallowed-chord list; Alt layer as the escape hatch |
| **Wrong keyboard matrix transcription** (§16) | machine types the wrong characters; looks like a CPU bug | transcribe from the service manual, test against a reference emulator's matrix, verify on hardware at M4 |
| **Field rate 50 vs 60 Hz** (§16) | timing-sensitive software runs at the wrong speed; audio pitch is wrong | make it a configuration value from the start rather than a constant |
| **MOS vector addresses wrong** (§11.2) | tape loading silently fails or corrupts | phase 2 signal-level tape does not depend on them at all — which is the real argument for doing phase 2 |
| **Tearing on full-screen change** (§8.5) | cosmetic | accepted; band presents localise it. No TE line exists to fix it with |
| **SRAM growth past budget** | link failure, or worse, a heap that fails at runtime | `config.h` plus a linker-map check in CI; §5 has 70 % headroom to start |
| **Shipping ROMs** | licence violation | user supplies ROMs; the build has no ROM binaries and the menu explains their absence |
| **300 MHz turbo build corrupts data** | silent, intermittent | not shipped by default; if enabled, the hardware notes' §3 clock checklist is mandatory and the flash-integrity concern is real |

---

## 19. References

**The guest**

- Acorn Atom Technical Manual and circuit diagram — 8255 wiring, keyboard
  matrix, VDG mode bit order.
- Motorola **MC6847** datasheet — mode table, SG4 encoding, character ROM,
  field timing.
- MOS/BASIC ROM disassemblies — entry points, page-2 indirection vectors.
- Klaus Dormann, *6502 functional tests*; Bruce Clark, *Decimal mode in the
  NMOS 6502*.
- Existing Atom emulators (Atomulator, Wouter Ras's emulator) — for
  cross-checking behaviour and for the trace-diff harness of §15.1, not for
  copying code.

**The host** — all collected in [`hardware-notes.md`](hardware-notes.md) §11:
the ClockworkPi PicoCalc repository and mainboard schematic, the ST7365P
controller specification, the keyboard MCU source, and the RP2350 datasheet and
Pico SDK documentation.
