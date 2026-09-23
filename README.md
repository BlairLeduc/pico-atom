# pico-atom
An Acorn Atom emulator for the PicoCalc

## Documentation

- [Design](docs/design.md) — architecture, memory budget, subsystem design,
  milestones and risks.
- [PicoCalc hardware notes](docs/hardware-notes.md) — the host platform
  reference this design is built against.
- [Third-party material](THIRD-PARTY.md) — what in here is not ours, and under
  what terms.

## ROM images

Acorn's ROMs are still copyrighted, so **this repository ships none of them**
and never will. The emulator reads them off the SD card at `/atom/roms/`
(design document §11.1); without them it boots to a screen explaining which
are missing rather than into a dead machine.

You need five files. Four are the machine; `utility.rom` is the `#A000` socket
and is yours to fill:

| File | Guest address | What it is |
|---|---|---|
| `akernel.rom` | `#F000`–`#FFFF` | Atom MOS — the kernel, with the 6502 vectors |
| `abasic.rom` | `#C000`–`#CFFF` | Atom BASIC |
| `afloat.rom` | `#D000`–`#DFFF` | Floating-point ROM |
| `dosrom.rom` | `#E000`–`#EFFF` | AtomDOS |
| `utility.rom` | `#A000`–`#AFFF` | Utility socket — optional, any 4 KiB ROM |

### Where to find them

The set most people use is the one bundled with **Atomulator**, David Banks'
(hoglet67) maintained fork of Tom Walker's Atom emulator. Its `roms/` directory
carries the images under exactly the filenames above:

- <https://github.com/hoglet67/Atomulator/tree/master/roms>
- <https://atomulator.acornatom.co.uk/> — the project's own site

Other places the same images circulate: the **stardot.org.uk** forums' Acorn
Atom section, and MAME's `atom` set, which packs BASIC and the MOS into one
8 KiB `abasic.ic20` rather than splitting them (concatenating `abasic.rom` and
`akernel.rom`, in that order, reproduces it exactly).

`axr1.rom` — Acorn's AXR1 extension ROM — also ships with Atomulator and is a
reasonable thing to drop in as `utility.rom`.

### Verifying what you got

Atom ROM images have been re-dumped, renamed and re-split many times, and a
subtly wrong one produces a machine that boots and then misbehaves. Check what
you have against these, which are the Atomulator images:

```
2621f27d652d4673e0a79aa669e729b8c3051ab6  akernel.rom
a8ea19f10d4c98fbc1b666e5968f06d46af9a84c  abasic.rom
ebcde5b36cb3a3344567cbba4c7b9fde015f4802  afloat.rom
71ea0a4b8d9c3caf9718fc7cc279f4306a23b39c  dosrom.rom
f8417787c28818a7646b9b59d706ef890255049f  axr1.rom
```

Each is exactly 4096 bytes. `shasum roms/*.rom` prints the same format.

One trap worth naming: collections that use descriptive filenames often carry
**two** floating-point ROMs, and it is the *second* — `Atom_FloatingPoint2.rom`,
SHA-1 `ebcde5b3…` — that matches `afloat.rom` and MAME's `afloat.ic21`. The
first is a different revision.

### Where to put them

Copy them to the SD card as `/atom/roms/akernel.rom` and so on. The repository
also has an ignored [`roms/`](roms/) directory you can stage them in; see
[`roms/README.md`](roms/README.md).

## Using it

The emulator boots straight to the Atom's `>` prompt.

| Atom key | PicoCalc |
|---|---|
| `REPT` | `Tab` — hold it, then hold the key to repeat |
| `COPY` | `Alt`+`C` |
| `LOCK` | `Alt`+`L` |
| `BREAK` | `Alt`+`K` |
| the emulator's menu | `Alt`+`M` |

**Tapes** are `.atm` files in `/atom/tapes/` on the card. `LOAD "NAME"` and
`*RUN "NAME"` find the file whose header carries that name, or failing that the
file called `NAME.atm`; `SAVE "NAME"` writes one. `LOAD ""` takes whichever
tape the menu's *Tapes* page has inserted.

**Game keymaps** put a game's keys under one hand. Atom games scan the keyboard
themselves, and many were laid out for keys that sit far apart on the PicoCalc.
The menu's *Keys* item picks a layout. It lays a few keys over the standard map,
so every other key still types. The built-in *Games* layout, for Galaxians and
games like it, puts left on `Left`, right on `Right` and fire on `]`; choose it
in the menu before playing. Further layouts are text files in
`/atom/keymaps/`, one binding per line:

```
# Games: move on the arrows, fire on ]
name  = GAMES
left  = UPDOWN
right = CTRL
]     = REPT
```

A key on the left is a PicoCalc key: `left`, `right`, `up`, `down`, `space`,
`enter`, `backspace`, `tab`, `del`, `esc`, or any single character, shifted or
not. A target on the right is an Atom key by the name on its keycap (`A`–`Z`,
`0`–`9`, the punctuation, `SPACE`, `RETURN`, `UPDOWN`, `LEFTRIGHT`, `COPY`,
`LOCK`, `DELETE`, `ESCAPE`) or one of the lines `CTRL`, `SHIFT` and `REPT`.
An optional `tapes = NAME, NAME` line names programs whose loading selects the
layout. A file that does
not parse is left out, and the menu names the file and the line.

**Snapshots** save and restore the whole machine from the menu, in four slots
kept in `/atom/snaps/`. They contain no ROM bytes, and load only over the same
ROMs.
