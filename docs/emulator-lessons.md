# Writing an emulator for the PicoCalc: what we learned

A standalone guide for developers and agents starting an emulator of a vintage
machine on the **ClockworkPi PicoCalc**, in C against the Raspberry Pi Pico SDK.
It collects what one complete emulator (an 8-bit home computer with a 6502,
a video display generator, a 1-bit speaker, cassette and floppy disc) taught us
about architecture, accuracy, performance, testing and process. None of it
depends on that project's source. Copy it into a new project as is.

**Companion document.** `hardware-notes.md` is the authority on the host: the
PicoCalc's wiring, peripheral protocols, timing and measured costs.
References written **HW §N** point there. Plain **§N** points within this
document. This guide does not repeat the hardware notes. It says what an
emulator does with them.

**Scope, October 2026.** The figures are measurements from a Pimoroni Pico
Plus 2 W (RP2350B) at 150 MHz unless stated otherwise. Each is the number for
one guest on one board, and is here to calibrate your estimates, not to
replace your own measurements. Where an example comes from that first
emulator it names the guest part (a 6502, an MC6847 VDG, an 8255 PPI, a 6522
VIA, an 8271 FDC) so the lesson can be mapped onto the parts in yours.

**Contents**

1. [The shape of the problem](#1-the-shape-of-the-problem) ·
2. [Architecture](#2-architecture) ·
3. [The CPU](#3-the-cpu) ·
4. [Devices and the bus](#4-devices-and-the-bus) ·
5. [Video](#5-video) ·
6. [Audio](#6-audio) ·
7. [Keyboard](#7-keyboard) ·
8. [Media: ROMs, tape, disc, snapshots, settings](#8-media-roms-tape-disc-snapshots-settings) ·
9. [Timing and clocks](#9-timing-and-clocks) ·
10. [The user interface](#10-the-user-interface) ·
11. [Testing](#11-testing) ·
12. [Measuring](#12-measuring) ·
13. [Working on the hardware](#13-working-on-the-hardware) ·
14. [Process and documentation](#14-process-and-documentation) ·
15. [Checklist for a new emulator](#15-checklist-for-a-new-emulator)

---

## 1. The shape of the problem

**The SPI wire to the LCD is the bottleneck, not the guest CPU.** A 1 MHz
6502 interpreted on a 150 MHz Cortex-M33 takes 35–46 % of one core. A
full redraw of a 256×192 guest screen takes 11.5 ms of a 16.7 ms field,
all of it wire time (HW §4.7). Put optimisation effort on pixels transmitted,
not on the interpreter, until a measurement says otherwise.

**SRAM is the scarce resource**, not CPU (HW §2.3). A whole 8-bit machine
with a 64 KiB address space, three video snapshots, a 64 KiB tape buffer,
the renderer and 25 KiB of interpreter moved into SRAM came to ~255 KiB, 49 %
of an RP2350's 520 KiB. On an RP2040 the same image would leave ~5 KiB.
Decide early whether you support RP2040; it is a memory question first.

**Your estimate of interpreter cost will be optimistic by 3–4×.** The design
estimated 40–80 host cycles per guest instruction. The measurement was
158–218: about 100 Thumb instructions per guest instruction once the run
loop, device ticks, interrupt lines, dispatch and the opcode are counted.
That left roughly 2× headroom at 1 MHz, not 6–12×. Measure before you
promise turbo modes or faster guest clocks (§9.3).

**Pick the clock you ship at by the SPI divider, not by the CPU.** Only
150 MHz and 300 MHz deliver the panel's full 75 MHz SPI rate (HW §3). At
200 MHz the guest runs 1.33× faster and the display 1.5× slower. Ship at
150 and treat 300 as a user's opt-in overclock (§9.4).

---

## 2. Architecture

### 2.1 A portable core and a thin port

Split the code in two:

| Layer | Contains | Rules |
|---|---|---|
| **core** | CPU, bus, every guest chip, media formats, keymap data, snapshot format, settings parser, status text | portable C, **no SDK header, no dynamic allocation**, all state in one machine struct or statically sized buffers |
| **port** | LCD, audio, I²C, SD, the two cores' loops, menus, file I/O | the only place SDK headers appear |

Build the core with the workstation's compiler under CTest, and the firmware
with `arm-none-eabi-gcc`, **both under `-Wall -Wextra -Werror`**, in CI on
every push. The core building clean on both toolchains is the mechanism that
stops SDK dependencies leaking down. A rule in a document does not do that;
a failing build does. It also means the CPU passes its test suites, and the
whole machine boots its real ROMs, on a laptop before any hardware exists.

Put **every fixed capacity in one header** (address space, buffer sizes,
list lengths, queue depths). You will trade them against each other
repeatedly, and the SRAM budget is only a link-time fact if they are in one
place. Check growth with `arm-none-eabi-size` on every build; have CI print it.

### 2.2 A narrow, synchronous core API

The seam between core and port should be a handful of calls:

```c
void     machine_init(machine_t *m, const machine_config_t *cfg);
void     machine_reset(machine_t *m);
uint32_t machine_run(machine_t *m, uint32_t cycles);  /* returns cycles actually run */
uint32_t machine_run_field(machine_t *m);             /* one video field, split at flyback */
void     machine_key_set(machine_t *m, int row, int col, bool down);
size_t   machine_audio_drain(machine_t *m, int16_t *dst, size_t max);
const uint8_t *machine_vram(const machine_t *m);
uint8_t  machine_video_mode(const machine_t *m);
void     machine_copy(machine_t *dst, const machine_t *src);
```

Every test exercises this seam, and so does the firmware. When the field loop
lives in the core (`run_field`), the host tests run the exact code the
firmware runs.

**`run(n)` executes whole instructions until at least `n` cycles have
elapsed and returns the true count.** The caller carries the overshoot as a
debt into the next call. Never add a "run exactly N cycles" variant. The
debt-carry pattern is what keeps long-run timing from drifting, and it
survives splitting a field into several runs (§5.6), because each call
reports its true count against one accumulator.

**Copy a machine with a function, never with `=`**, if the struct holds
pointers into itself (a page table pointing into its own RAM). A struct
assignment leaves the copy reading and writing the original's memory. The
same goes for host callbacks stored in the machine: a copy must not inherit
the original's hook, or the copy will drive the original's pins.

### 2.3 Which core owns what

| Core 0 | Core 1 |
|---|---|
| guest CPU and every guest chip | LCD presenting |
| audio synthesis, and the audio DMA IRQ | southbridge I²C (keyboard, battery, backlight) |
| pacing (§6.3) | SD card work, menus |

Core 1 owns every slow peripheral, so core 0's only blocking point is the
audio queue it paces on. Two invariants:

- **Core 1 never reads guest RAM while the guest runs.** Everything it draws
  arrives in an immutable snapshot (§2.4).
- **Core 1 never calls `sleep_us`/`sleep_ms` in its loop.** Those set an alarm
  whose IRQ runs on core 0, in the middle of the guest (HW §9.7). Wait with
  `busy_wait_us_32`. Fixing this one call was worth 1.11× on the guest, more
  than moving the interpreter into SRAM, and it cut run-to-run spread from
  ±4 to ±0.1 cycles per instruction.

**Core 0 never calls `printf` once the guest runs.** At 115200 baud a
few-hundred-byte heartbeat takes ~30 ms, and the audio queue's low-water slack
was ~11 ms. Format log lines into a ring on core 0 and let core 1 move bytes
into the UART FIFO as it has room. Count dropped lines.

**Keep renderer state off core 0.** The video renderer's lookup tables belong to
the presenting core. If core 0 rebuilt a LUT on a guest register write while
core 1 expanded rows through it, that is a cross-core race no care in the
renderer can see, and a second copy costs SRAM you do not have. Core 0 carries
VRAM and the register latches; the mode byte is read off them when the
snapshot is taken.

### 2.4 The frame handoff: three buffers, four states

Core 0 publishes a snapshot of VRAM plus mode bits (plus a few status bytes)
at each field's end. Core 1 presents the newest. Use a pool of **three**
buffers, each `free`, `filling`, `ready` or `rendering`:

- Core 0 marks its `filling` buffer `ready` and claims a `free` one.
- **Publishing drops any older `ready` buffer on the spot.** At most one is
  ever `ready`, so core 1 needs no sequence numbers, and core 0's next claim
  always finds a `free` buffer.
- Core 1 takes the `ready` one, marks it `rendering`, frees it when done.

Why three: core 1's worst case can exceed a field. With two buffers core 0
would have to block (and corrupt the schedule audio is paced against) or
rewrite a buffer core 1 might be claiming. The third buffer deletes the
class of problem. Do not economise here.

Write the state machine in the core with **no lock of its own**; the port
calls each transition under one SIO spinlock (RP2350 has no compare-and-swap,
and masking interrupts locally is not a multicore lock). A lock-free state
machine can be driven through a long randomised interleaving in a host test.
Count dropped snapshots; that counter is how you learn core 1 is saturated.
Simulation timing is never sacrificed to presentation.

### 2.5 Parking the guest for host work

Anything slow on core 1 that touches the machine (an SD read for a tape or
disc request, the menu, a pause, a restart) uses one mechanism: **core 0
parks at a field boundary and hands the machine to core 1**, feeding the
audio queue silence at the rate it drains so audio neither underruns nor
loses its pacing. Core 1 does its work with the card mounted, then hands the
machine back. Guest time does not advance while parked, so card latency is
invisible to the guest, and pacing needs no catch-up on resume.

Core 1 may write the machine (load ROMs, re-run `init`) only inside that
window. Settings that change on core 1 (a keyboard layout chosen by a tape
load) are applied by core 0 when it takes the machine back, so there is never
a second writer.

---

## 3. The CPU

### 3.1 What accuracy to aim for

**Cycle-correct at instruction granularity** is cheap and is what makes timed
loops, tape loaders and raster tricks work: every documented opcode's cycle
count, page-crossing and branch penalties, exact decimal mode including the
NMOS flags, and the quirks real software depends on (`JMP (xxFF)`, B flag,
read-modify-write double write). Sub-instruction (per-cycle bus) accuracy was
not needed for anything the guest's software archive does; drop it unless a
known title needs it.

**Trap and count undocumented opcodes** in a first version, and put the count
on the heartbeat. Zero over a soak is evidence; a nonzero count tells you which
title needs them.

### 3.2 Implementation

A **`switch`-dispatched interpreter** with explicit cycle accounting. On a
Cortex-M33 a dense switch compiles to a table branch, keeps the CPU state in
registers, and beats a table of function pointers. The CPU has no notion of a
device; the bus does (§4).

Interrupt lines as a **bitmask of asserted sources** (level-sensitive IRQ:
zero deasserts) and NMI as **edge-triggered and latched**. A reset line that
resets the CPU must also reset every chip on the real reset line. A timer
interrupt left enabled across a reset ran garbage on our guest until the VIA
was reset with the CPU. That the VIA is on the reset line was inferred from
what the ROM's reset routine assumes, not read off the schematic; say so
when you do the same.

### 3.3 Test suites, and what a skip means

- **Klaus Dormann's 6502 functional test** must run to completion. It is the
  difference between an emulator and a plausible one.
- **Bruce Clark's decimal test** with **every flag checked**. Dormann's copy
  checks only A and C; Clark's catches a decimal `ADC` that takes N from the
  result rather than the intermediate. It is public domain, so its source can
  live in the tree; assemble it at fetch time (cc65's `ca65`/`ld65`).
- **The cycle table asserted by execution**: run each opcode in each
  addressing mode and count, rather than comparing two tables in the same
  repository. Only the first catches an addressing-mode bug.

Do not commit test binaries you did not build; fetch them with a script into a
gitignored directory the test finds without configuration. A missing binary
makes the test report **skipped** (CTest exit code 77), and **a skipped
functional test is an unverified CPU**, not a pass.

### 3.4 Making it faster, if you must

Measure first (§12). Then, in order of what paid off:

1. **Remove interference.** The `sleep_us`-on-core-1 alarm (§2.3) was worth
   more than any code placement.
2. **Make device ticks countdowns** (§4.3). A VIA tick rewritten as "subtract
   from T1 and from one counter to the next T2/shift-register event, call
   out of line only when one runs out" became nine instructions with no stack
   frame, and the whole machine got 3.6–6.2 % faster than before the VIA was
   completed.
3. **Move hot code to SRAM in measured tiers** (HW §9.2, §9.8). For a 6502
   interpreter this gave 1.12–1.19× for 25 KiB, a third of what it gave a
   tree-walking interpreter: `-O3` inlines the bus into every opcode and makes
   the dispatch 20 KiB, but the opcodes a real program runs fit the 16 KiB XIP
   cache anyway. Move small hot callees first (bus slow path, device ticks,
   ADC/SBC), then the dispatch loop. A 256-byte const table moved to SRAM
   measured nothing. Keep tier membership in one header of macros so a tier
   is a build option, and build each tier in its own build directory.

---

## 4. Devices and the bus

### 4.1 A page table with a NULL slow path

Back the whole address space with a flat array and dispatch every access
through a 256-entry page table:

```c
typedef struct { uint8_t *read; uint8_t *write; } page_t;  /* NULL -> slow path */
```

RAM and ROM are one indexed load or store (`write` NULL for ROM). I/O and
unpopulated pages take the slow path. Keep the descriptor **exactly two
pointers**; put per-page flags (ROM, I/O, VRAM, open bus) in a separate byte
array, because only the slow path reads them and a third field pads the
descriptor and grows the table.

**A device smaller than a page, inside RAM, is silently invisible.** Our
guest's floppy controller sits at eight bytes inside an otherwise-RAM page.
With that page's `write` non-NULL, the controller is never reached and the
disc system simply does not respond. Mark such a page I/O and split it in
the slow path. One page losing its fast path cost nothing measurable.

**Decode I/O by mask, not equality**, where the original decodes partially:
software relies on mirrors. **Unpopulated memory reads open bus**, the last
value on the data bus, not `0xFF`. A ROM may probe an empty socket and act on
what it reads; a reference emulator that returns 0 there will diverge from
you in a trace (§11.4).

**VRAM needs no write hook** if the presenter diffs snapshots (§5.3). That is
cheaper than tracking stores and cannot under-mark.

### 4.2 Model the chip, then wire it

Write each chip as the generic part (an 8255, a 6522, an 8271) with the
machine's wiring as accessors on top. Then the chip's own datasheet is the
test plan, and the wiring is a small, separately checkable table.

Things that a plausible model gets wrong:

- **Output latches separate from input sources.** A port read returns input
  pins alongside the last value latched into output bits. Merge them and a
  read-modify-write in the ROM corrupts outputs.
- **Bit-set/reset control paths** exist and ROMs use them (the 8255's BSR is
  how ours toggled the speaker).
- **One register, two meanings.** If a port carries both the keyboard column
  and the video mode, every keyboard scan writes the video mode too. Compare
  the relevant bits, not the whole byte.
- **An "optional" chip may be load-bearing.** Our guest's ROM read the
  optional VIA on every character it printed and hung before its first prompt
  without one. Fit what the ROM touches.

**Let evidence decide how much of a chip to build.** Before completing the
6522's shift register and handshake lines we searched the whole software
archive (5,610 files) for code that enabled them: none did. We completed the
part anyway so nothing would meet a dead register, but the search told us it
was not urgent and which modes the games actually used.

### 4.3 Bring devices up to date lazily

**Nothing is clocked per instruction unless it must be.** The pattern that
made device after device free:

- **Bring an input up to date when the guest reads it**, the only time it can
  be seen. A cassette waveform, a timer's count, a 2.4 kHz reference: compute
  where it is now from the cycle count.
- **Keep "the first cycle at which anything here can next change".** A read
  before it is one subtract and a branch. Our tape port's first version made
  three calls per port read and cost 18 % on a scrolling workload; the
  next-event version cost 3.5 %.
- **Stop the run slice at a device's next event** instead of checking per
  instruction. The floppy controller cost one compare per slice and nothing
  measurable overall.
- Bring everything up to date at the field boundary too, for the menu,
  snapshots and anything core 1 shows.

When a device's internal counters lag between events, give it an explicit
`sync()` and call it before anything outside the device reads them.

**Make the cycle count the only clock.** Every device times itself in guest
cycles, never in wall time. Then turbo is free (§9.3), pause is free, a
snapshot captures time exactly, and a host-side stall (card I/O) is invisible.

### 4.4 Hooks to the outside world

A device that reaches real pins (a user port on spare GPIOs) gets a callback
in the machine struct, NULL unless the port installs one. The bus calls it
only on the registers that matter (before a read of the input register;
after a write to output, direction or control). Test that the hook fires for
exactly those registers and no others. Output that changes without a
register write (a timer driving a pin) is pushed once a field.

---

## 5. Video

### 5.1 Do you need a framebuffer?

If the guest's image is a **pure function of its VRAM plus a few mode bits**
(true of most 8-bit video chips without sprites or mid-frame tricks), keep
no decoded framebuffer. Snapshot VRAM, and generate rows from the snapshot
straight into two RGB565 DMA line buffers (HW §4.6, §4.10). That saves
memory, but the decisive argument is correctness: there is no second copy of
the screen to keep in sync. A guest with sprites, raster effects or a
per-line palette needs a per-line state record in the snapshot, or a real
framebuffer; decide which by what its software does.

### 5.2 Rows from a lookup table

- **One LUT per mode**, mapping a VRAM byte straight to its already
  horizontally stretched run of RGB565 pixels. Ours was 8 KiB worst case and
  rebuilt only on a mode or colour-set change, a few times per program run.
- **Copy runs with unrolled code, not `memcpy`.** At 16–32 bytes the call is
  the cost (HW §9.4).
- **Vertical stretch is free.** A row repeated ×2 or ×3 re-sends the same line
  buffer to the next window rows without regenerating it.
- **Text modes take their own generator**: per cell, a glyph row from the
  character ROM or a synthesised block-graphics pattern, through a two-colour
  path.
- **The palette is a pointer.** A colour/mono switch rebuilds the LUT once and
  costs nothing per pixel. Derive a mono palette from the video chip's
  datasheet luminance levels, not from a greyscale formula over RGB.

Measured: a full 256×192 redraw cost the same as filling the same rectangle
with a constant colour, to within 0.4 %, in every mode. **Row generation is
hidden behind the DMA**; the present is wire-bound.

### 5.3 Dirty bands, and why the mode belongs in the shadow

Divide the guest screen into **bands of 8 rows**, each with an inclusive
`[min..max]` column span. Per presented snapshot:

1. **Compare the snapshot's mode byte with the presented mode.** If it
   differs, mark every band dirty and rebuild the LUT.
2. Otherwise diff the snapshot against a shadow copy, band by band.
3. Send each dirty band's span as one LCD window.
4. Copy the snapshot into the shadow **and its mode byte into the presented
   mode**.

Step 1 is a correctness requirement: a mode or colour-set change repaints
every pixel while leaving VRAM byte-identical, so a VRAM-only diff finds
nothing to do and leaves the old mode on screen for ever.

Over-mark rather than under-mark: an extra band costs microseconds, a missed
one leaves a stale image for ever. Expect a fixed cost per present (the diff
and shadow copy, ~0.1 ms for 6 KiB) and per band (a window and its row DMAs):
one changed cell cost 0.28 ms, not the 0.05 ms the pixel count predicts.
Measured scrolling text: 0 dropped snapshots over 3,300 fields; presents of
0.1–5.5 ms.

### 5.4 Geometry and what to leave out

Draw the guest **1:1 at its native resolution**, centred, if it fits the
320×320 panel. A 256×192 guest at (32, 64) leaves a 64-row band above and
below. Scaling to 320×240 puts 56 % more pixels on the wire for a 1.25×
stretch with visibly uneven pixel doubling; we planned it and dropped it.

Use the spare bands for a **status line** (tape position, disc activity) and
a **perf line**, each drawn **only when its text changes**. A 40-column line
is 3,840 pixels, under 1 ms. Draw a guest border only when its colour
changes; never as part of every full redraw.

**Tearing**: there is no TE line (HW §1.2, §4.9). Accept it. Small, localised
band presents confine a tear to one band. Hardware vertical scroll (HW §4.8)
was not worth the y-remap hazard for a guest that scrolls by memory moves the
diff already catches cheaply.

### 5.5 Character ROMs

- **Never fabricate a font.** Take a verified extracted table (with its
  licence and attribution kept), render the whole glyph set to an image, and
  read it by eye.
- **Assert the layout against the data**: which bits hold the glyph, which
  rows of the cell, and the chip's glyph order (often not ASCII). Then a
  differently laid-out substitute fails loudly instead of rendering
  plausible-but-wrong glyphs. Our first rendering put every character against
  the left edge of its cell because of an unneeded shift.
- **A screen full of glyph 0 at power-on can be correct.** Zeroed VRAM shows
  glyph 0 everywhere until the ROM clears the screen. Do not make the
  renderer substitute blanks; that would hide a ROM that never ran. Pin the
  behaviour with a test.
- If the font is missing, draw a visible placeholder per cell and say so at
  boot, rather than a blank screen.

### 5.6 Split the field at flyback, and snapshot where the next frame begins

The guest polls a vertical-sync flag to time its screen writes. Two rules:

- **Guest instructions must run while the flag is in its flyback state.**
  Running a whole field and then pulsing the flag leaves a program that polls
  it spinning for ever. Split the field into runs at the flag's edges:
  active lines, flag low, remaining blank. Debt carry (§2.2) keeps the split
  exact.
- **Take the snapshot where the next active line begins, not where the flag
  changes.** Games erase and redraw during the blanking interval after the
  flag falls. Our first snapshot, at the flag's rise with a guessed 6 % low
  time, caught a game's redraw half done every other field, and its objects
  flickered out. Use the video chip's datasheet line counts (for an MC6847,
  262 lines: 192 active, 32 with FS low, 38 blank).

Test both with a guest loop on the host: it must observe the flag low and
escape once per field, and a redraw started after the flag falls must be
finished in every snapshot. Run the old, wrong field shape as a **control that
must fail**, so the test is known to tell the two apart.

### 5.7 Golden images

Render fixed VRAM in every mode and colour set to PPM files and commit them.
**A golden image proves nothing until someone has looked at it**: it is
generated by the code it checks. View every changed image before committing;
on a mismatch have the test write `<name>.actual.ppm` for inspection.

---

## 6. Audio

### 6.1 A 1-bit speaker: integrate, do not sample

Point-sampling a speaker bit aliases audibly. Treat each output sample as the
**time average of the speaker level over the guest cycles it covers**, a box
filter at exactly the sample period:

```
on every write that changes the speaker bit:
    acc += level * (now - last_change); level = new; last_change = now
at each sample boundary:
    acc += level * (boundary - last_change)
    sample = acc / cycles_per_sample; acc = 0; last_change = boundary
```

It costs an add and a multiply per toggle, not per sample, and is exact for
square waves of any frequency.

- **Keep the sample period as a reduced rational** of guest cycles (2048/75 at
  a 1 MHz guest and a 150 MHz host) and advance the boundary by quotient and
  remainder. No division, no accumulated rounding: after any run the sample
  count is exactly `⌊cycles × den / num⌋`. The host's sample rate is itself a
  rational, `clk_sys / (divider × (TOP+1) × oversample)`; never feed the
  truncated integer back into timing (HW §5.2).
- **Stamp an edge with the cycle count at the start of the writing
  instruction.** The offset to the actual store is the same for every edge a
  loop makes, so pitch is exact and only phase moves.
- **Follow it with a one-pole DC blocker** (pole ~0.995), so a speaker left
  high and one left low both settle at 0. Then 0 is the silence value, which
  is what an underrun and a parked guest emit.
- **Fixed point throughout.** No `double` literals near it (HW §2.2).

Test it against an **independent model** built from the edges the guest
actually made, every sample to 1 LSB, and measure the pitch of a ROM routine
(the bell) against the cycle count of its loop: ours was 387.64 Hz measured
against 387.60 Hz computed.

A guest with a sound chip is a synthesiser instead; HW §5.5 has what a
software PSG costs.

### 6.2 Plumbing

Follow HW §5.3–5.4 exactly: two chained DMA channels, a power-of-two aligned
ring with the hardware read wrap, both read address and count reset on
re-arm, `DMA_IRQ_0` at priority `0x40`, refill path in SRAM. At oversample 2,
each frame occupies two ring slots; size the ring for that.

Between the producer and the IRQ put an **SPSC queue of finished PWM compare
words** (convert at push time so the IRQ only copies), ~28 ms deep, starting
playback at ~21 ms. Producer and IRQ are both on core 0, so it needs no lock.

**Two counters, not one** (HW §5.8): PCM underrun samples (producer starved)
and late DMA refills (IRQ starved). A refill is late when its channel is
already running again on entry; leave it alone and take nothing from the
queue, or the samples will be overwritten before they play. **When muted,
keep producing and consuming**, so mute does not change timing.

### 6.3 Pace the guest on the audio queue

The emulator runs faster than real time, so something must hold it back.
**Block core 0 until the PCM queue has room for another field's samples**
(wait with `__wfi()`; the DMA IRQ on the same core wakes it). The PWM wrap is
a hardware clock derived from `clk_sys`, the most stable timebase on the
board, and audio and CPU then share one clock by construction and cannot
drift.

Pacing does not make underrun impossible: a guest that falls below real time
starves the queue by construction, and a flash erase stops everything (HW
§7.2). Keep the underrun counter live, emit silence, and resync without
replaying. For a build with audio disabled, pace on `time_us_64()` against an
**absolute** field deadline, so a late field does not accumulate.

The control quantity for every measurement on the machine is **samples
consumed per second against `time_us_64()`**: it should read the nominal rate
(36,620–36,621 Hz here) whatever the guest does.

---

## 7. Keyboard

### 7.1 The impedance mismatch

A vintage guest scans a key matrix at its own speed and reads modifier lines
separately. The PicoCalc delivers **translated ASCII events over a 10 kHz
I²C bus**, with Shift already resolved by its keyboard MCU and no raw-key
mode (HW §6). So the mapping runs backwards:

```
[state, code] → normalise → held-key set → code → (row, col, modifiers) → guest matrix
```

The rules, every one from measured MCU behaviour (HW §6.1–6.3):

- **Build held-key state from press/release events.** Never consume a
  character stream. Auto-repeat arrives as extra *press* events.
- **Canonicalise** letters and shifted punctuation for held-state identity:
  releasing Shift first gives press `A` / release `a`.
- **Fix the binding at press time and keep it with the held key**, so a
  key's release undoes exactly what its press did even if the layout changed
  in between.
- **Drain the whole FIFO each poll**, from core 1 at ~30 Hz, in thread
  context; the poll also feeds the MCU's 2.5 s bus watchdog.
- **Replay key events into the matrix at the guest's pace.** A ROM keyboard
  routine needs several fields to see a key (ours, ~8 fields per key, with no
  type-ahead). Hold each key down a minimum number of fields and leave a gap
  before the next.

### 7.2 The mapping table

Make the mapping **data**, one row per PicoCalc code: target row, column and
flags (assert the guest's SHIFT, assert CTRL, Alt layer, a standalone line,
reset, open the menu). Then a host test can check it: every code maps to
exactly one cell, and nothing binds a chord the MCU cannot deliver.

**Settle the guest's matrix by executing its ROM**, not from a secondary
document. Press each of the matrix's cells at the prompt, with and without
SHIFT, read what the ROM writes to VRAM, and you have the whole map,
including unused cells and keys whose meaning SHIFT reverses. Keep that sweep
as a regression test that types every table entry through the real ROM.

Guest keys the PicoCalc lacks go on an **Alt layer**, chosen because the
guest probably has no Alt key, so nothing is stolen from it. A key pressed
with Alt down comes from the Alt layer only, so no game layout can take away
the menu or reset.

Constraints from the MCU that bind the table:

- **Shift+Left, Shift+Right, Shift+Space and Shift+Backspace send nothing at
  all**: no press, no repeat, **no release** (HW §6.3). A direction key let go
  while Shift is down stays held in your set. Never make Shift part of a game
  binding that must chord with the arrows.
- **Alt+`,` `.` Space `B` are consumed by the MCU** (backlights, battery).
- Several keys exist only as shifted alternates (Home, End, PgUp/PgDn, Break,
  Insert).
- `F1`–`F5` arrive as `0x81`–`0x85` and `F10` as `0x90` (the MCU's
  Shift+`F5`). A guest without function keys leaves them free for the
  emulator's own pages.
- A guest modifier on its own is a key games read. Our guest's SHIFT line was
  read alone as a jump button, so the host's Shift must assert it whatever
  else is held. A guest key that acts as a held modifier (REPT, in our case)
  cannot be an Alt chord: give it a plain key.
- Characters the MCU sends shifted (`|` `{` `}` `` ` `` `~`) need entries of
  their own, mapped to the guest's shifted cells by what they are. Missing
  `|` meant a BASIC operator could not be typed.

Latency is one poll interval plus one field plus a 4–5 ms transaction,
~55 ms at 30 Hz. Fine for BASIC and for games written against a ROM that
debounced across fields.

### 7.3 Game layouts

Games scan the matrix directly, often with keys laid out for the original
keyboard. Provide **layouts as overlays** on the standard map: a few PicoCalc
keys rebound to guest cells (asserted without SHIFT) or standalone lines,
everything else unchanged so the game's own prompts still type.

- Built-in layouts are **generic and name no game**. Per-game layouts are
  text files on the card, one binding per line, and may name the tapes they
  go with; loading such a tape selects the layout, and the menu says so.
  Loading a tape no layout names leaves the choice alone (a loader may fetch
  its next part under another name).
- Reject a malformed layout file whole and name the line; a partly applied
  layout is a different layout.
- A layout is host state, not guest state: keep it out of snapshots.
- Put fire on a key whose chord with the arrows the MCU delivers. Our first
  cut put fire under the same thumb as the directions; on the device a key
  at the far side of the keyboard played better.

---

## 8. Media: ROMs, tape, disc, snapshots, settings

### 8.1 ROMs

- **Ship no ROM images** if they are copyrighted. The user puts them on the
  SD card; the README says which files, where to get them and their SHA-1s.
- **Identify ROMs by SHA-1**, and know the common alternate packagings (two
  4 KiB images that one collection ships as a single 8 KiB file). A
  near-miss dump boots and then misbehaves, the most expensive class of bug.
- **A missing ROM shows a page naming the missing files**, never a blank
  screen. On this hardware a blank screen is the single most expensive
  failure to debug.
- Show every slot's file, status and hash on an About page, so a bug report
  can be copied off the panel.

### 8.2 Tape, phase 1: trap the OS routines

Fast and simple: when the CPU reaches the ROM's load or save routine, serve
the request from a file and return as the routine would have.

- **Trap the handler, not the public entry point.** If the entry point jumps
  through a RAM vector, trapping the handler means anything that repoints the
  vector (a disc OS, a utility ROM, a game's own loader) is never trapped.
- **Check the handler's first bytes** against the stock ROM's and stand aside
  for any other ROM; what you reproduce is that ROM's contract.
- **Stall the CPU, do not return early.** At the trapped boundary the CPU
  behaves as if RDY were held low: guest time passes, devices tick, no
  instruction runs, and the run loop keeps its contract while the port serves
  the request at the next field boundary (§2.5). A request no file answers is
  **declined** and the ROM routine runs as if there were no trap, so the user
  sees the ordinary "PLAY TAPE" prompt.
- **Leave what the ROM leaves, byte for byte**: parameter block pointers,
  the last block header, checksums, mode bits, registers, flags, I/O latches.
  Code after a load (`*RUN`, the BASIC re-entry, a game loader) may look at
  any of it. Test this by running the ROM's own routine, with only its
  byte-level cassette routines hooked, and requiring the trapped call to leave
  the same machine in every byte below ROM.

### 8.3 Tape, phase 2: the signal

Decode a tape image (UEF, CSW) into **half-cycles clocked in guest cycles**
and present them on the input bit the ROM reads, brought up to date only when
that port is read (§4.3). It works with any loader, protected or headerless,
and turbo is free.

- **Read the ROM's tape routines before writing the decoder**: how it times a
  bit, what it counts, what leader it waits for. Ours timed writes against a
  hardware 2.4 kHz reference input that had not been modelled; a save that
  the phase-1 trap declined would have hung.
- Carry remainders when converting tape units to guest cycles so a half-cycle
  is 208 or 209 cycles and never drifts.
- Decompress gzip straight into the flat buffer, one bit at a time as `puff`
  does; back-references read from the output itself, so no separate window.
- **Follow the ROM's own cues for the motor.** Stop at the "PLAY TAPE" prompt,
  start on the key that answers it, stop when the load returns (except for a
  load that runs code which may read on). Find those PCs with one table
  lookup on the low byte in the run loop.
- **Read semantics off the ROM, not off folklore.** An empty file name was
  "the ROM's nameless format", not "the next file". Find a cue's exit by
  reading the code: the switch-off we first chose ran after every block, not
  once per file.
- **Recording** is the inverse: decode the output bit into standard chunks
  appended to the tape, a byte at a time, keeping the image valid after every
  byte. Leave out the writer's pause between bytes (a 48-byte save went from a
  748-byte image to 104). Drop and count bytes that do not frame. A gzipped or
  read-only image is write-protected.
- **Settle clock questions by execution.** Our guest's ROM writes a tape
  correctly at 2 MHz (it times bits against a reference that keeps wall time)
  but cannot read one (it times input with its own loops). The deck therefore
  plays only at the stock clock, and says so.

### 8.4 Disc controller

- **The model knows the drive geometry and asks the host only for bytes**, a
  track at a time: a read posts a request and the chip stays busy while the
  CPU runs on; a write collects sectors and then posts. The host serves it
  with the guest parked (§2.5), so the card's 17–27 ms per track is invisible
  to the guest.
- **READY is how a DOS notices a disc change.** Ours re-read the catalogue
  only when the drive was not ready, which on real hardware followed the head
  unloading after an idle count. Model the head load and unload, and run a
  pending unload when the user swaps discs with the guest paused, or the old
  catalogue persists.
- Read the register addresses off the DOS ROM, not secondary documents; ours
  were not where the old document put them, and the interrupt was NMI.
- Short images (truncated after the last used sector) read zeros past their
  end and grow when written. A read-only file is a write-protected disc.
- Refuse a snapshot while a command is in progress.

### 8.5 Snapshots

- **Write fields explicitly, little-endian**, never a struct dump: the
  machine struct holds pointers and padding, and a snapshot must outlive the
  build that wrote it.
- **Header with magic, version, lengths and a CRC.** **Encode every field so
  zero is its reset state** and reserve bytes; then a later version can fill
  reserved bytes and an older file still loads as an idle device.
- **Store ROM hashes, not ROM bytes**, and refuse a machine whose ROMs or
  memory map differ. Record the guest clock and refuse the other one.
- **Load in two passes.** The first reads and checks everything without
  touching the machine; only then does the second change anything. A torn,
  foreign or newer file leaves the running machine exactly as it was.
- After a load, release all keys and restart audio from the restored clock.
- Test by execution: save mid-program, restore into a machine that has been
  doing something else, run 150 fields, and require identical state.

### 8.6 Writing files to the card safely

Every write goes to `name.new`, which is closed, then the old file is unlinked
and the new one renamed into place. Rename is not proof of power-loss
atomicity (HW §7.1), so put the **recovery on the load side**: a file that is
missing or fails its check gives way to a whole `.new`. Do all card work at a
defined boundary with the guest parked (HW §7.1).

### 8.7 Settings: a text file on the card, not flash

Keep the user's power-up configuration in a `key = value` text file on the
card, and **write no internal flash at all**:

- One source of truth that the user can read and edit on any computer, and
  that survives reflashing.
- A flash erase takes tens of milliseconds with XIP offline, longer than the
  audio deadline (HW §5.4, §7.2). A card write has no such cost.

Rules that worked:

- **Every default in one function.** The file names only what it changes.
- A wrong line changes nothing and the lines after it still apply; the first
  problem goes to the menu's status row. A **duplicate key is an error**, so
  nothing depends on line order.
- **Saving is a deliberate menu action, never automatic.** Trying a setting
  costs nothing until saved.
- **Edit the user's text, do not regenerate it.** A key's line keeps its
  place, indentation and trailing comment; only the value changes. Keep
  comments, blank lines, unparsed lines and the file's line ending. Append a
  missing key only if its value differs from the default. A value that
  already says the same keeps its spelling, so a second save changes nothing.
  Keep comment columns aligned. Refuse a save when the file has a duplicate
  key.
- **Parse your own output before writing it**, and refuse if it does not give
  the settings back. A rewrite that disagrees with its own reader is a bug,
  caught on the board as well as in tests.
- Read the file before anything that depends on it: the host clock (§9.4)
  is read on core 0 before stdio or any peripheral is up.
- Build-time overrides (`-DBOOT_TAPE=...`, `-DBOOT_CLOCK=...`) win over the
  file, so a run driven over the UART knows what it booted with (§13.2).

---

## 9. Timing and clocks

### 9.1 The guest's field

Pin the field rate and line counts from primary sources and make them
constants once settled (§14.2). Our guest's field rate was configurable while
unverified; it turned out to be 60 Hz on every unit sold, including those in
50 Hz countries, and software timed itself on it.

### 9.2 Power-on state

**Zero-fill RAM** at power-on rather than modelling a chip-dependent
checkerboard no software can rely on, with one exception to look for: **state
the software uses as a seed.** Our guest's BASIC random generator was a shift
register that never leaves zero; from zero-filled RAM `RND` returned 0 for
ever and a game drew all its asteroids at one point. Seed such state from the
board's hardware RNG in firmware, and from a constant in host tests so runs
repeat.

### 9.3 Turbo and faster guest clocks

**Turbo** (run unpaced) while a tape plays: drop the guest's samples and top
the audio queue up with silence to its start depth without blocking. Because
the tape is clocked in guest cycles the guest cannot tell; a 300-baud load
finished 2.7–2.8× faster.

**A faster guest clock** (an owner's modification on the original) needs a
rule for every timed quantity:

| Follows the CPU clock (same cycles, faster in wall time) | Fixed in wall time (more cycles at a higher clock) |
|---|---|
| timers clocked by the CPU's Φ2 | the video field and its line split |
| software delay loops (the bell goes up an octave) | the audio sample period |
| | references from their own crystal, tape half-cycles |
| | disc controller byte, sector, step times |

Compute the second column from the clock once at init and keep each value
where it is used. Change the clock only through a power-on restart, never
under a running program; converting every in-flight count is complexity for
nothing a user notices. Run every clocked host test at each guest clock
(§11.2).

### 9.4 The host clock

Changing `clk_sys` moves five other things (HW §3). For an emulator the safe
shape is:

- `main()` starts at 150 MHz, reads the settings file on core 0 with nothing
  else up, and **only then** moves to 300 MHz if asked: rail to 1.20 V first,
  the flash's QMI divider and RX delay doubled from SRAM so flash stays at the
  bootrom's 50 MHz, then the PLL, then `clk_peri` onto `clk_sys`. Only then
  start stdio, I²C, the LCD and audio, so every driver derives its rate from
  the clock it finds.
- Derive the audio PWM divider as `clk_sys / 150 MHz` so the sample rate is
  identical at both clocks.
- **Set the rail back down at 150 MHz explicitly.** The regulator survives a
  reset (HW §3).
- **Change the host clock by writing the setting and restarting with the
  watchdog**, not by retuning running peripherals.
- At 300 MHz, host cycles per guest instruction were unchanged: CPU-bound code
  is clock-bound even with flash at 50 MHz, and a 4 MHz guest at 300 MHz left
  core 0 the same margin as 2 MHz at 150. Soak it on battery before calling it
  stable, and put the die temperature on the heartbeat.

---

## 10. The user interface

- **Boot straight into the guest.** The emulator is invisible until asked.
- One menu key (an Alt chord) opens a menu that **pauses the guest** (§2.5).
  Function keys open its pages directly from the running guest, and closing a
  page so opened returns to the guest. Inside the menu the function keys do
  nothing.
- **Draw the menu as a guest text page through the guest's own renderer.**
  It costs almost no code. Closing it **does not restore pixels** (the shadow
  holds VRAM bytes, not a panel image); invalidate and redraw the next
  snapshot whole, one full present.
- A **machine page stages** changes (RAM, clock, ROMs, host clock), marks
  staged rows, and applies them only by an explicit **restart that behaves as
  a power-on**. Check the card's ROMs in a first pass before touching the
  machine, and refuse rather than leave a broken one. Warn that the program in
  memory is lost. Refuse while an unsaved recording exists.
- **Pause** on a key: park the guest, dim the backlight (read the current
  level first, since it may be the MCU's own, and restore it), show `PAUSED`.
  Any key resumes and is not passed to the guest; a modifier alone does not
  resume, and neither does the pause chord's own auto-repeat.
- An **About page** with the firmware version, physical board, chip revision,
  clocks, southbridge version, die temperature, every ROM's hash and the
  settings file's state. Generate the version from `git describe --always
  --dirty` **at build time**, rewriting the header only when it changes; a
  configure-time value goes stale on the next commit.
- A **status row** in the menu names the first problem: a bad settings line, a
  missing ROM, a refused save, a protected tape.
- **Firmware names no specific titles.** Built-in behaviour is generic; per-
  title configuration lives in files on the card that the user writes.
- Keep the user's port (GPIO) features honest about what they cost: on the
  development build the UART owns GP4/GP5, and the menu says which of the log
  or typed keys is lost if the user takes them.

---

## 11. Testing

### 11.1 Shape

- **No framework.** A header with `CHECK` and `TEST_DONE`; one binary per
  area under CTest; exit code 77 is a skip.
- **Prefer asserting behaviour by executing it** over comparing two tables in
  the same repository.
- **Give a test a control that must fail**: run the old, wrong behaviour
  alongside and require the test to tell them apart.
- Keep hardware-independent logic (dirty tracking, the snapshot pool's state
  machine, status-line formatting, settings rewriting, parsers) in the core,
  where the host tests reach it.

### 11.2 Run the real software on the host

A small harness builds the real machine on the workstation: it finds the
user's ROMs by SHA-1 in a gitignored staging directory, types keys through
the same matrix path the firmware uses, and reads results out of VRAM. Tests
that need ROMs skip without them. With it, the regression suite runs the
actual OS and BASIC: boot, type `PRINT 2+2`, ring the bell and measure it,
save and load through the ROM's own routines, run the DOS against an
in-memory disc, restore a snapshot mid-session.

Tests whose subject has a clock are registered once per guest clock with an
environment variable the harness reads.

### 11.3 Media round trips by the ROM itself

For tape: record what the ROM's own `SAVE` drives onto the output bit, decode
it with an independent decoder in the test, write it as an image, and load it
back through the ROM's own `LOAD`. Keep the test's decoder as the independent
model the core's recorder is compared against.

### 11.4 Trace-diff against a reference emulator

Run the same ROMs and key script under your core and under an established
emulator, print `PC A X Y S P cycles` plus the opcode bytes before each
instruction, and diff. The first divergence is almost always the bug, and it
finds problems no unit test is shaped to catch.

- Build the reference from its own checkout with stub headers for its GUI
  library; copy none of it into your tree. Keep the harness a tool, not a
  CTest, since it needs that checkout.
- Two emulators keep different time (field lengths differ), so polling loops
  run different counts. **Resync** at the next point where registers and a
  shadow return stack agree, and classify what lay between: a divergence
  right after a read of a timed input (keyboard, vsync, timers, disc) is
  expected; different values, a different path, a lost trace or an
  unexplained cycle difference fails.
- **The reference has bugs.** Ours found six cycle-count errors in the
  reference and none in our core. When they disagree on cycles, check the
  manufacturer's table before believing either. Carry the reference's known
  errata in the tool.
- Plant a bug (a wrong bit in a port's read-back) once to prove the harness
  catches it.
- Keep typed lines under the guest's line length, and wait after RETURN; the
  guest may have no type-ahead.

### 11.5 The soak

A 30-minute run **on battery** with a guest program that exercises display,
sound and keyboard together, with the log captured throughout. A script then
checks the log: one boot, heartbeats covering the run, real-time ratio never
below 0.995, and every failure counter zero on every heartbeat (I²C errors,
keys lost, underruns, late refills, undocumented opcodes, dropped snapshots,
log lines dropped), while presents, key events and speaker edges grow.

- The program must not stall on a missed key: have it read the matrix itself.
- Type keys at it for the whole run, and choose them carefully: our first soak
  stopped because a typed `R` was read as Escape by BASIC's escape test in the
  matrix column the program had left selected.
- Record the power source: the southbridge's charging bit proves USB power;
  its absence does not (HW §6).

---

## 12. Measuring

HW §9.1 has the discipline: profile **in the mode you ship**, carry a
**control quantity** that should not change, expect **~2 % run-to-run
spread**, compare **on one board**, and **write numbers to a file**. For an
emulator specifically:

- **Report headroom, not just real-time ratio.** A paced emulator reads 1.000
  whatever the code costs. Measure the share of core 0 spent inside
  `run_field` and guest cycles per microsecond of it, plus host cycles per
  guest instruction.
- **Script a few workloads** (idle at the prompt, a compute loop, a scrolling
  loop, a sound loop), one boot each, typed over the UART, and reduce the
  heartbeats to one line per workload. Idle at the prompt was the heaviest,
  not compute.
- **Measure every feature against a control build** with only that feature
  removed, in the same sitting. That is how a 3.5 % port hook, a 2.5–4 % VIA
  regression and its fix, and a recorder hook costing nothing were told apart
  from layout noise. Code layout alone moves results by about 1 %.
- **Heartbeat contents**: real-time ratio, core 0 share and headroom, host
  cycles per instruction, longest present, presents/full presents/dropped
  snapshots, underrun samples, late refills, queue depth and low water,
  samples consumed per second (the control), I²C errors, key events dropped,
  undocumented opcodes, log lines dropped, battery and charging, die
  temperature. Every counter must be able to fire; one that cannot is dead
  code.
- An **on-panel perf line** is for watching; the heartbeat is for measuring.
  Measure the perf line's own cost with it on and off.

---

## 13. Working on the hardware

### 13.1 The loop

With a Debug Probe's SWD and UART both connected (HW §2.7):

- **Capture the UART first, then flash**, so the boot banner is in the log.
  Let only one process read the serial port: two readers split the byte
  stream and both logs come out scrambled. Make the capture script refuse a
  port something else holds.
- **Flash with `reset halt` then `resume`**, never `reset run`, or core 1 can
  be lost (HW §2.7).
- **Type at the guest over the UART.** The firmware turns received bytes into
  PicoCalc key events, so a run needs nobody at the keyboard. Pace characters
  to the guest's keyboard routine (0.25 s each for ours). Reserve a byte for
  host actions (play/stop the tape).
- The menu cannot be reached over the UART, so give the build **boot-time
  options** that preload media and override settings (`BOOT_TAPE`,
  `BOOT_DISC`, `BOOT_CLOCK`, `BOOT_HOST_MHZ`, `BOOT_NEW_TAPE`), each in its own
  build directory.
- UART logs have CR line endings and may contain UTF-8; some `grep`
  replacements print nothing on them. Read them with Python if `grep` comes
  back empty.

### 13.2 Two builds

- **Development**: stdio, the log and typed keys on UART1 (GP4/GP5). Every
  tool above needs it.
- **Release**: no UART at all; logging compiles to nothing, UART keys are
  compiled out, and GP4/GP5 are free for a user port. Check it on the panel,
  or read counters over SWD. One build switch sets the defaults that differ,
  so the settings defaults and the settings rewriter agree.

### 13.3 Bring-up

Bring up in HW §10's order, each stage its own smoke test: southbridge,
LCD with a corner-coded test pattern (check orientation, colour order and
all four corners by eye), keyboard, audio, card. Log **physical board
identity separately from the SDK build target**; a `pico2` build running on a
Plus 2 W (RP2350B) picks the wrong ADC input for the temperature sensor unless
it reads the package at run time (HW §8.1).

---

## 14. Process and documentation

### 14.1 Two documents

| Document | Authority on |
|---|---|
| hardware notes | the host: wiring, protocols, timing, measured costs, quirks |
| design | the guest and the shape of the code: hardware model, architecture, memory budget, milestones |

Cross-reference by section number and keep the references accurate; they are
load-bearing. In code, **comments cite the document section** next to any
decision that looks arbitrary, rather than restating the reasoning.

Use one notation for guest values and another for host values (the guest's
own convention, `#XXXX` or `$XXXX`, for guest addresses, and `0x` for host
values). A document that discusses two machines at once otherwise invites a
whole class of reading error.

### 14.2 Unverified constants

Keep a table of every guest constant written from secondary knowledge, with
its primary source and a confidence: **low, medium, high, confirmed**.
Transcribe each from the primary source before it becomes a `#define`, and
record how each was settled. A wrong constant produces a machine that boots
and then misbehaves subtly.

**The lifecycle**: while unverified, a constant is runtime configuration; once
settled, it becomes a constant and the configuration goes. Our video mode bit
order and field rate both went that way.

**The best primary source is often the ROM, executed.** The keyboard matrix,
the OS entry points, the disc controller's addresses, the video byte wiring
and whether the tape reader works at 2 MHz were all settled by running the
original ROM on the host and observing it, not by reading a document. The
disc controller's registers and the video byte's bit order both turned out
different from what had first been written from secondary sources.

### 14.3 Milestones

Each milestone **ends with something that runs and something that is
measured**, and says when, on what board, what was verified, and what was
not. "Built" and "done" are different words: a feature is done when it has
been checked on the device. The milestone that matters is the one where the
guest boots to its prompt on the device and answers typed input; everything
before is scaffolding.

A workable order: skeleton with both builds and CI → CPU passing its suites on
the host → bus, chips and video golden images on the host → board bring-up
with the real field loop → **guest boots** → audio → fast media loading,
snapshots, menu → perf pass → signal-level media → disc → remaining chip
detail → recording, status, saved settings → the rest of the menu → clock
options.

### 14.4 Estimates and measurements

Write the estimate first and keep it when the measurement replaces it; the
pair shows where the model was wrong. Label every remaining estimate as one.
Every measurement says what board, what build, what date and what workload.

### 14.5 Scope

**Drop features explicitly and say why** in the design. We planned and then
dropped video-bus snow (authentic, ugly, and needing a per-cycle beam
position), a scaled display, a colour-set override and a system page; each
entry says why, so nobody re-plans it.

---

## 15. Checklist for a new emulator

Architecture

- [ ] Core/port split; core has no SDK headers and no allocation; both
      targets build under `-Werror` in CI.
- [ ] Fixed capacities in one header; `arm-none-eabi-size` printed on every
      build.
- [ ] `run(n)` returns true cycles; caller carries debt; no "exactly N".
- [ ] Machine copied by function; callbacks not copied.
- [ ] Core 0: guest and audio. Core 1: LCD, I²C, SD. No `sleep_us` on core 1.
      No `printf` on core 0 once the guest runs.
- [ ] Three-buffer snapshot pool; older `ready` dropped at publish; dropped
      count on the heartbeat.
- [ ] Park/handoff at a field boundary for all card work, feeding silence.

CPU and devices

- [ ] Functional test and a decimal test with every flag; a skip is not a
      pass; cycle table asserted by execution.
- [ ] Undocumented opcodes trapped and counted.
- [ ] Page table of two pointers; flags separate; sub-page devices split in
      the slow path; I/O decoded by mask; open bus modelled.
- [ ] Devices brought up to date on read, with a next-event countdown; run
      slices stop at device events.
- [ ] Every chip on the reset line reset with the CPU.

Video

- [ ] No framebuffer if the image is a function of VRAM and mode.
- [ ] Mode byte part of the shadow; mode change repaints all.
- [ ] Field split at the vsync flag's edges; snapshot where the next active
      line begins; tested with a control.
- [ ] Golden images looked at before committing.
- [ ] Character ROM verified by rendering; layout asserted against the data.

Audio

- [ ] Box filter at the exact rational sample period; DC blocker; silence 0.
- [ ] Guest paced on the audio queue; underruns and late refills counted
      separately; muted still consumes.

Keyboard

- [ ] Held set from press/release; canonical codes; binding fixed at press.
- [ ] Matrix settled by executing the ROM; every entry typed through it in a
      test.
- [ ] No binding needs a chord the MCU swallows (and Shift+arrows lose their
      release).

Media and settings

- [ ] ROMs by SHA-1; a missing-ROM page, never a blank screen.
- [ ] Traps on handlers, stalling the CPU, leaving the ROM's state byte for
      byte; tested against the ROM's own routine.
- [ ] Snapshots: explicit fields, zero is reset, ROM hashes, two-pass load.
- [ ] Every write through `.new` and rename, recovery on load.
- [ ] Settings in a card text file, edited in place, parsed back before
      writing; no flash writes.

Process

- [ ] Hardware notes and design kept separate, cross-referenced by section.
- [ ] Unverified-constants table with sources and confidence.
- [ ] Every milestone ends with a device check and a measurement, dated.
- [ ] Perf workloads scripted; each feature measured against a control build.
- [ ] 30-minute battery soak with every counter zero.
