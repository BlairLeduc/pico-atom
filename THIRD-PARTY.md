# Third-party material

Everything in this repository is pico-atom's own work under the GPL-3.0 in
[`LICENSE`](LICENSE), with the exceptions below. Nothing here is a ROM image;
see [“ROM images”](README.md#rom-images) for those, which the repository never
carries.

## MC6847 character ROM — XRoar

`src/core/mc6847_font.c` and `src/core/mc6847_font.h` hold the MC6847's
internal 64-glyph character generator. The table is taken **verbatim** from
XRoar's `src/mc6847/font-6847.c` and is byte-identical to it; only the comment
header differs.

> XRoar — a Dragon and Tandy 8-bit computer emulator
> Copyright 2003-2026 Ciaran Anscomb <xroar@6809.org.uk>
> <https://www.6809.org.uk/xroar/>

XRoar is distributed under the GNU General Public License, version 3 or later —
the same licence as this project, so the data can travel with the repository.
XRoar generated the file with its own `tools/font2c`; `tools/mkfont.py` here
reads and writes the same structure, so the two remain interchangeable.

The layout that comes with the data is documented in the file header and in
[`CLAUDE.md`](CLAUDE.md): 5 px wide in bits 5..1, glyph rows at 3..9 of the
12-row cell, and the MC6847's own glyph order rather than ASCII.
`test/host/test_mc6847.c` asserts all three against the data itself, so
substituting a differently laid-out ROM fails loudly.

## PicoCalc LCD initialisation values — ClockworkPi

`src/port/lcd.c` sends the panel's gamma, power, VCOM, frame-rate, inversion,
display-function and manufacturer commands with the parameter bytes used by
ClockworkPi's own driver, `Code/picocalc_helloworld/lcdspi/lcdspi.c` in
<https://github.com/clockworkpi/PicoCalc> at commit
`f91519806d4b2e0a62c4638a9f695cd5162c5479`. Only the register values were
taken — the driver around them is this project's — and hardware-notes.md §4.4
directs using them because generic controller defaults may not suit this glass.
The pixel format (`0x55`) and entry mode (`0x06`) differ from that driver's
18-bit setup and come from hardware-notes.md §4.4 instead.

GitHub detects no licence file in that repository at that revision. The
southbridge reply layout in `src/port/southbridge.c` was likewise checked
against the same repository's keyboard firmware, but no code was taken from it.

## FatFs — ChaN

The firmware links FatFs R0.15 (with patch 1) for the SD card. It is **not in
the tree**: CMake copies `ff.c`, `ff.h`, `ffunicode.c` and `diskio.h` out of
the Pico SDK's `lib/tinyusb/lib/fatfs/source/` into the build directory at
configure time. The one FatFs file in the repository is
`src/port/fatfs/ffconf.h`, which started as the SDK's copy and records in its
header which values were changed.

> FatFs — Generic FAT Filesystem Module
> Copyright (C) 2022, ChaN, all right reserved.
> <http://elm-chan.org/fsw/ff/>

FatFs's licence, from the header of `ff.c`, is reproduced in full:

```
Copyright (C) 2022, ChaN, all right reserved.

FatFs module is an open source software. Redistribution and use of FatFs in
source and binary forms, with or without modification, are permitted provided
that the following condition is met:

1. Redistributions of source code must retain the above copyright notice,
   this condition and the following disclaimer.

This software is provided by the copyright holder and contributors "AS IS"
and any warranties related to this software are DISCLAIMED.
The copyright owner or contributors be NOT LIABLE for any damages caused
by use of this software.
```

Its one condition applies to source, and `ffconf.h` carries the notice for
that reason. Binary redistribution has no condition in this version of the
licence; the notice is reproduced here anyway.

## Klaus Dormann's 6502 functional tests

Not in the tree. `tools/fetch-test-suites.sh` downloads the assembled binary
into `test/suites/`, which is gitignored; `test_m6502_functional` reports as
*skipped* when it is absent. The suite is Klaus Dormann's, published under the
GPL-3.0.
