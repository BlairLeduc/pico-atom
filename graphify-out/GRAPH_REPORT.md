# Graph Report - pico-atom  (2026-09-23)

## Corpus Check
- 112 files · ~118,056 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 25 file(s) not represented in the graph (top: .ppm 22, (none) 2, .cmake 1)

## Summary
- 932 nodes · 2384 edges · 52 communities (45 shown, 7 thin omitted)
- Extraction: 79% EXTRACTED · 21% INFERRED · 0% AMBIGUOUS · INFERRED: 492 edges (avg confidence: 0.85)
- Token cost: 250,732 input · 0 output

## Community Hubs (Navigation)
- Machine Core & Run Loop
- Guest Harness & Boot Tests
- Host Test Scaffolding
- Tape Trap & Tape IO
- Menu & Card Pickers
- MC6847 Rendering & Present Tests
- UEF & Cassette Signal
- SD Card & ROM Loading
- Keymaps & Layout Parsing
- 6502 Interpreter
- Audio & Log Ring
- Core Headers
- Font Tooling (mkfont)
- LCD & Display Driver
- Core 1 Main Loop
- Design Doc Guest Findings
- Snapshot Format
- Inflate (gzip)
- Southbridge & Keyboard Poll
- Field Loop & Tape Traps (design)
- VDG Test Scenes
- Firmware Build & SRAM Rules
- CLAUDE.md Architecture Rules
- Two-Core Pacing & Perf
- Southbridge Keyboard Notes
- Atom Guest Hardware
- Clocks & LCD Panel
- Hardware Scripts
- Board Identification
- Audio/LCD Bring-up Invariants
- Third-Party Material
- SRAM Placement & PSRAM
- ROM Images & SHA-1s
- Dormann & ROM Tests
- PicoCalc Host Platform
- Presenter & Menu Design
- Carrier Board & PWM Audio
- Framebuffer Model & Wire Cost
- Core 1 Peripherals & Flash
- Signal-Level Tape (M8)
- Snapshot Pool & Memory Budget
- Host Build & SDK Isolation
- Processor Boards
- Golden Images
- Milestones
- Perf Summary Script
- Generic Game Layouts
- Device Hook Markers
- Key Mapping Table
- Snapshot Slots
- mkfont Round-trip Test

## God Nodes (most connected - your core abstractions)
1. `pico-atom design document` - 82 edges
2. `main()` - 38 edges
3. `atom_init()` - 27 edges
4. `core1_main()` - 23 edges
5. `main()` - 21 edges
6. `main()` - 17 edges
7. `atom_config_default()` - 16 edges
8. `bus_write()` - 15 edges
9. `menu_run()` - 15 edges
10. `guest_fields()` - 15 edges

## Surprising Connections (you probably didn't know these)
- `guest_boot()` --calls--> `atom_config_default()`  [INFERRED]
  test/host/guest.c → src/core/atom.c
- `fresh()` --calls--> `atom_config_default()`  [INFERRED]
  test/host/test_keymap.c → src/core/atom.c
- `bare_machine()` --calls--> `atom_config_default()`  [INFERRED]
  test/host/test_m6502_behaviour.c → src/core/atom.c
- `bare_machine()` --calls--> `atom_config_default()`  [INFERRED]
  test/host/test_m6502_cycles.c → src/core/atom.c
- `run_imm()` --calls--> `atom_config_default()`  [INFERRED]
  test/host/test_m6502_decimal.c → src/core/atom.c

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Signal-level tape pipeline (gzip -> UEF -> cassette -> port C)** — docs_design_inflate_gzip, docs_design_uef_walker, docs_design_cassette_lazy_update, docs_design_deck_cues, docs_design_turbo_tape [EXTRACTED 1.00]
- **Video presentation path (snapshot -> diff -> LUT -> DMA)** — docs_design_snapshot_pool, docs_design_dirty_band_presenter, docs_design_expansion_lut, docs_design_no_framebuffer, docs_design_st7789p_lcd [EXTRACTED 1.00]
- **Audio path (PC2 integrator -> PCM queue -> PWM DMA, paced)** — docs_design_box_filter_integrator, docs_design_dc_blocker, docs_design_pcm_spsc_queue, docs_design_pwm_audio_dma, docs_design_pace_on_audio [EXTRACTED 1.00]
- **Things derived from clk_sys that must be re-applied after a clock change** — docs_hardware_notes_clk_peri_fixup, docs_hardware_notes_spi_divider_table, docs_hardware_notes_psram_qmi_timing, docs_hardware_notes_cyw43_wifi, docs_hardware_notes_carrier_and_sample_rate [EXTRACTED 1.00]
- **Mechanisms protecting the 3.5 ms audio refill deadline** — docs_hardware_notes_audio_refill_deadline, docs_hardware_notes_chained_dma_audio_ring, docs_hardware_notes_lcd_thread_context_only, docs_hardware_notes_keyboard_polling, docs_hardware_notes_dma_ping_pong_blit [INFERRED 0.85]
- **LCD present pipeline** — docs_hardware_notes_lcd_spi_setup, docs_hardware_notes_cs_high_40ns_rule, docs_hardware_notes_caset_raset_windows, docs_hardware_notes_dma_ping_pong_blit, docs_hardware_notes_band_renderer, docs_hardware_notes_dirty_region_tracking [INFERRED 0.85]
- **Keeping Pico SDK out of src/core** — claude_core_no_sdk, cmakelists_host_target, _github_workflows_ci_host_job, cmakelists_pico_atom_warnings [INFERRED 0.95]
- **User-supplied ROM identification by SHA-1** — readme_rom_images, readme_rom_sha1_list, roms_readme_roms_staging_dir, test_host_cmakelists_guest, test_host_cmakelists_test_keymap_readme_check [INFERRED 0.85]
- **Golden image generation and checking** — test_host_cmakelists_vdg_scenes, test_host_cmakelists_test_mc6847_golden, test_host_cmakelists_vdg_ppm, claude_golden_image_rule [INFERRED 0.95]

## Communities (52 total, 7 thin omitted)

### Community 0 - "Machine Core & Run Loop"
Cohesion: 0.05
Nodes (75): beeper_t, One-pole DC blocker, Hot code SRAM tiers (tier 2 ships), atom_audio_drain(), atom_audio_set_rate(), atom_cassette_eject(), atom_cassette_play(), atom_cassette_rewind() (+67 more)

### Community 1 - "Guest Harness & Boot Tests"
Cohesion: 0.08
Nodes (64): dirent, guest_t, keymatrix_t, atom_cassette_insert(), atom_copy(), atom_vram(), apply_head(), atom_t (+56 more)

### Community 2 - "Host Test Scaffolding"
Cohesion: 0.07
Nodes (43): atom, bus, i8255, m6502, math, romset, stdio, string (+35 more)

### Community 3 - "Tape Trap & Tape IO"
Cohesion: 0.10
Nodes (52): FRESULT, inflate, record_t, bus_write(), atm_header_decode(), atm_header_encode(), atm_name_matches(), atom_tape_decline() (+44 more)

### Community 4 - "Menu & Card Pickers"
Cohesion: 0.09
Nodes (47): menu_settings_t, snapshot, snapshot_status_str(), FILINFO, keylayout_t, fail(), is_map(), keymapio_count() (+39 more)

### Community 5 - "MC6847 Rendering & Present Tests"
Cohesion: 0.10
Nodes (42): mc6847_font, snap_state_t, snappool, snappool_t, band_source_rows(), build_lut(), mc6847_mode_info_t, mc6847_t (+34 more)

### Community 6 - "UEF & Cassette Signal"
Cohesion: 0.12
Nodes (41): cassette, cassette_t, advance(), ATOM_HOT1(), cassette_eject(), cassette_init(), cassette_insert(), cassette_percent() (+33 more)

### Community 7 - "SD Card & ROM Loading"
Cohesion: 0.11
Nodes (36): BYTE, DRESULT, DSTATUS, ff, LBA_t, rom_state_t, sd_status_t, UINT (+28 more)

### Community 8 - "Keymaps & Layout Parsing"
Cohesion: 0.10
Nodes (35): ctype, keylayout_status_t, sha1, sha1_t, atom_cassette_playing(), keylayout_t, keylayout_for_tape(), keylayout_parse() (+27 more)

### Community 9 - "6502 Interpreter"
Cohesion: 0.21
Nodes (26): ATOM_HOT1(), ATOM_HOT2(), atom_t, m6502_t, fetch8(), m6502_base_cycles(), m6502_init(), m6502_is_documented() (+18 more)

### Community 10 - "Audio & Log Ring"
Cohesion: 0.10
Nodes (21): audio_stats_t, dma, gpio, irq, pwm, audio_init(), audio_push(), audio_rate() (+13 more)

### Community 11 - "Core Headers"
Cohesion: 0.22
Nodes (3): stdbool, stddef, stdint

### Community 12 - "Font Tooling (mkfont)"
Cohesion: 0.12
Nodes (19): argparse, pathlib, re, subprocess, sys, tempfile, die(), main() (+11 more)

### Community 13 - "LCD & Display Driver"
Cohesion: 0.16
Nodes (17): mc6847, spi, display_test_pattern(), send_rows(), lcd_blit_begin(), lcd_blit_end(), lcd_blit_row(), lcd_cmd() (+9 more)

### Community 14 - "Core 1 Main Loop"
Cohesion: 0.19
Nodes (19): display_stats_t, keymatrix, multicore, display_invalidate(), display_present(), roms_report_t, core1_main(), keys_for_tape() (+11 more)

### Community 15 - "Design Doc Guest Findings"
Cohesion: 0.14
Nodes (20): pico-atom design document, Atom address map, Alpha/SG6 VRAM byte wiring (bit 6 A/S+INT/EXT, bit 7 INV), AtomDOS (disc phase 3), Atomulator (hoglet67), Box-filter integrator of PC2 at rational sample period, Cassette brought up to date on port C read, Bruce Clark decimal mode test (+12 more)

### Community 16 - "Snapshot Format"
Cohesion: 0.27
Nodes (20): snap_read_fn, snap_write_fn, atom_config_t, atom_t, snap_status_t, cfg_bits(), compatible(), get16() (+12 more)

### Community 17 - "Inflate (gzip)"
Cohesion: 0.28
Nodes (16): gzip inflate into flat buffer, huffman_t, inflate_get_fn, bits(), byte(), gz_status_t, codes(), construct() (+8 more)

### Community 18 - "Southbridge & Keyboard Poll"
Cohesion: 0.24
Nodes (11): i2c, sb_status_t, kbd_overflows(), kbd_poll(), kbd_pop(), sb_acquire(), sb_read(), sb_release() (+3 more)

### Community 19 - "Field Loop & Tape Traps (design)"
Cohesion: 0.17
Nodes (12): Alt layer for missing Atom keys, ATM tape file format, atom_run seam with cycle-debt carry, CPU stall like RDY low during tape call, Deck follows kernel cues (#FC40, #FC79, #F953), Field sync FS (port C bit 7), Flyback-split field loop (atom_run_field), Galaxians (+4 more)

### Community 20 - "VDG Test Scenes"
Cohesion: 0.42
Nodes (11): bits_per_px(), blank_page(), mc6847_mode_info_t, fill_glyphs(), fill_graphics(), fill_sg6(), fill_text(), plot() (+3 more)

### Community 21 - "Firmware Build & SRAM Rules"
Cohesion: 0.18
Nodes (11): CI firmware job (pico2 UF2), Copy machine with atom_copy, never =, Hot code SRAM tiers (hot.h), Measurement discipline, page_t is exactly two pointers, SRAM budget in config.h, pico-atom firmware executable, PICO_ATOM_AUDIO option (+3 more)

### Community 22 - "CLAUDE.md Architecture Rules"
Cohesion: 0.22
Nodes (11): atom_run debt-carry seam, Core 0 never calls printf after guest start, Core 1 never calls sleep_us, Core 0/core 1 ownership split, Hardware invariants (hardware-notes §10), MC6847 character ROM layout, CLAUDE.md project guidance, No decoded framebuffer (+3 more)

### Community 23 - "Two-Core Pacing & Perf"
Cohesion: 0.18
Nodes (11): Two-core split (core 0 guest, core 1 peripherals), CPU budget measured at M7 (158-218 host cycles/insn, 2.2-2.9x headroom), Perf instrumentation counters, Core 0 log ring drained by core 1, Measurement discipline, Pacing on the audio ring (throttling), 1024-sample PCM SPSC queue, PWM audio with chained ping-pong DMA ring (+3 more)

### Community 24 - "Southbridge Keyboard Notes"
Cohesion: 0.22
Nodes (11): LCD Backlight via Southbridge Register 0x05, C64_MTX / C64_JS Raw Keyboard Registers, Die Temperature Sensor, Held-Key Bitmap from Press/Release Events, ClockworkPi keyboard.ino (keyboard MCU firmware), Keyboard Polling from the Frame Loop, Shift Resolved in the MCU (swallowed chords), Official References (ClockworkPi, RP2350 datasheet) (+3 more)

### Community 25 - "Atom Guest Hardware"
Cohesion: 0.24
Nodes (10): Acorn Atom (guest machine), MC6847 golden images, INS8255 PPI at #B000, I/O decode by mask (partial decoding, mirrors), MC6847 VDG, MOS 6502 CPU, Open bus as last value on bus, Port A VDG mode bits (A/G bit 4, GM0-2 bits 5-7) (+2 more)

### Community 26 - "Clocks & LCD Panel"
Cohesion: 0.20
Nodes (10): clk_peri does not follow clk_sys, Clocks, Voltage and Overclocking, Core Voltage Rail Sequencing, CYW43439 Wi-Fi, Hardware Vertical Scroll (VSCRDEF/VSCSAD), LCD Initialisation (MADCTL 0x48, COLMOD 0x55), 320x320 LCD Panel (ST7789P/ST7365P, 480-row frame memory), LCD Wiring and SPI Setup (spi1) (+2 more)

### Community 27 - "Hardware Scripts"
Cohesion: 0.24
Nodes (6): flash.sh script, program(), perf-run.sh script, stop_logger(), uart-log.sh script, uart-type.sh script

### Community 28 - "Board Identification"
Cohesion: 0.25
Nodes (7): board_info_t, clocks, board_identify(), board_init_clocks(), board_log_banner(), sysinfo, unique_id

### Community 29 - "Audio/LCD Bring-up Invariants"
Cohesion: 0.36
Nodes (9): Three Audio Bring-up Findings (clicking), Audio Refill Deadline (~3.5 ms) and IRQ Priority 0x40, Bring-up Order and Trap Checklist, Chained DMA Audio Ring, The 40 ns Chip-Select Rule, Polled DMA Ping-Pong Line-Buffer Blit, LCD Only Touched from Thread Context, Count PCM Underruns Separately from Late Refills (+1 more)

### Community 30 - "Third-Party Material"
Cohesion: 0.25
Nodes (8): FatFs copied from SDK at configure time, PICO_ATOM_CORE_SOURCES, PICO_ATOM_HAVE_FONT detection, ClockworkPi LCD init register values, FatFs R0.15 (ChaN), MC6847 character ROM (XRoar), Third-party material, XRoar emulator

### Community 31 - "SRAM Placement & PSRAM"
Cohesion: 0.25
Nodes (8): Where the Data Lives (PSRAM copy data-bound), Measure on Hardware, in the Mode You Ship, PSRAM (Plus 2 W only), PSRAM QMI Timing Computed Once, Small memcpy Calls Are Call Overhead, Hot Code to SRAM in Tiers (__not_in_flash_func), SRAM Placement Measured on a 6502 Interpreter, XIP Cache Instruction Fetch Cost

### Community 32 - "ROM Images & SHA-1s"
Cohesion: 0.38
Nodes (7): No ROM binaries in the tree, Atomulator (hoglet67), Second floating-point ROM trap, ROM images (user-supplied on SD), Atomulator ROM SHA-1 list, roms/ workstation staging directory, test_keymap checks README SHA-1s

### Community 33 - "Dormann & ROM Tests"
Cohesion: 0.29
Nodes (6): A skip is not a pass, guest library (real MOS on host), test_boot/test_tape/test_snapshot/test_cassette, test_m6502_functional (Dormann), Klaus Dormann 6502 functional tests, fetch-test-suites.sh script

### Community 34 - "PicoCalc Host Platform"
Cohesion: 0.29
Nodes (7): clk_sys 150 MHz stock clock, Held-key bitmap from press/release events, ClockworkPi PicoCalc (host), RP2350 / Pico 2 target, Southbridge I2C keyboard MCU, ST7789P LCD over spi1, 300 MHz turbo build option

### Community 35 - "Presenter & Menu Design"
Cohesion: 0.29
Nodes (7): Dirty-band presenter (24 bands of 8 rows), 256x192 at panel offset (32,64), Internal flash config, menu-only writes, Alt+M overlay menu, 30 Hz presentation decimation, Risks table, Tearing (no TE line), accepted

### Community 36 - "Carrier Board & PWM Audio"
Cohesion: 0.29
Nodes (7): hardware-notes.md, Carrier and Sample Rate (73.2 kHz carrier, 36.6 kHz rate), GPIO Map, Four Things the Hardware Does Not Give You (no TE, no key IRQ, no DAC, no RTC), PicoCalc Carrier Board, PWM Audio Output Stage (slice 5, GP26/27), Readback and Tearing

### Community 37 - "Framebuffer Model & Wire Cost"
Cohesion: 0.29
Nodes (7): Band Renderer / Compact Snapshot, CASET/RASET Windows and RGB565, Dirty-Region Tile Tracking, Choosing a Framebuffer Model, SPI Divider Coarseness (300 MHz is free on both), SRAM Is the Scarce Resource, What the Wire Costs (4.0 Mpx/s effective)

### Community 38 - "Core 1 Peripherals & Flash"
Cohesion: 0.29
Nodes (7): Core 1 Owns Slow Peripherals, Internal Flash, XIP and the Interrupt Hole, OpenOCD reset halt + resume (core 1 lost on reset run), sleep_us on Core 1 Interrupts Core 0, Software PSG Mixer Cost, Lock-Free SPSC Queue, Getting Code On and Output Back (SWD, UF2, UART1)

### Community 39 - "Signal-Level Tape (M8)"
Cohesion: 0.33
Nodes (6): Chuckie Egg UEF hardware check, 300 baud CUTS waveform, LOAD "" is nameless format, not next file, RAM population config (upper RAM #4000-#7FFF), Tape phase 2: UEF at signal level, UEF walker (half-cycles in quarter periods)

### Community 40 - "Snapshot Pool & Memory Budget"
Cohesion: 0.33
Nodes (6): 8 KiB mode expansion LUT, SRAM memory budget (~218 KiB of 520), No decoded framebuffer; band generator, SIO spinlock for state transitions, Three-buffer snapshot pool (free/filling/ready/rendering), SPI wire is the bottleneck, not the 6502

### Community 41 - "Host Build & SDK Isolation"
Cohesion: 0.50
Nodes (5): CI host job (core + tests), src/core must not depend on Pico SDK, Host build (PICO_ATOM_HOST), -Wall -Wextra -Werror warnings, PICO_ATOM_HOST_TESTS list

### Community 42 - "Processor Boards"
Cohesion: 0.40
Nodes (5): Erratum RP2350-E9 (pull-down latch), Pimoroni Pico Plus 2 W, Prefer float over double, The Five Processor Boards, RP2040 versus RP2350

### Community 43 - "Golden Images"
Cohesion: 0.50
Nodes (4): Golden image proves nothing until looked at, test_mc6847_golden, vdg-ppm tool, vdg_scenes library

### Community 44 - "Milestones"
Cohesion: 0.50
Nodes (4): M8 tape at signal level, M9 AtomDOS and 8271 (next), Milestones M0-M8, Tapes (.atm and .uef) usage

## Knowledge Gaps
- **40 isolated node(s):** `fetch-test-suites.sh script`, `perf-summary.sh script`, `LC_ALL`, `Atom address map`, `Bruce Clark decimal mode test` (+35 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 126 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **7 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `pico-atom design document` connect `Design Doc Guest Findings` to `Machine Core & Run Loop`, `PicoCalc Host Platform`, `Presenter & Menu Design`, `Carrier Board & PWM Audio`, `Signal-Level Tape (M8)`, `Snapshot Pool & Memory Budget`, `Inflate (gzip)`, `Field Loop & Tape Traps (design)`, `Two-Core Pacing & Perf`, `Atom Guest Hardware`?**
  _High betweenness centrality (0.212) - this node is a cross-community bridge._
- **Why does `hardware-notes.md` connect `Carrier Board & PWM Audio` to `Design Doc Guest Findings`?**
  _High betweenness centrality (0.112) - this node is a cross-community bridge._
- **Are the 33 inferred relationships involving `main()` (e.g. with `atom_audio_drain()` and `atom_audio_set_rate()`) actually correct?**
  _`main()` has 33 INFERRED edges - model-reasoned connections that need verification._
- **What connects `fetch-test-suites.sh script`, `perf-summary.sh script`, `LC_ALL` to the rest of the system?**
  _40 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Machine Core & Run Loop` be split into smaller, more focused modules?**
  _Cohesion score 0.05201292976785189 - nodes in this community are weakly interconnected._
- **Should `Guest Harness & Boot Tests` be split into smaller, more focused modules?**
  _Cohesion score 0.08077260755048288 - nodes in this community are weakly interconnected._
- **Should `Host Test Scaffolding` be split into smaller, more focused modules?**
  _Cohesion score 0.07017543859649122 - nodes in this community are weakly interconnected._