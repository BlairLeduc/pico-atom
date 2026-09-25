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

`.uef` images, gzipped or not, go in the same folder. A UEF is played the way a
real recorder plays it: the Atom reads the signal, so games with loaders of
their own load too. Choose the UEF on the menu's *Tapes* page. It goes in
stopped, and the menu says the name of its first file. Then type
`LOAD "NAME"` or `*RUN "NAME"` with that name, as the game's instructions say.
An empty name will not do: on an Atom, `LOAD ""` means a different, nameless
kind of file. When the Atom says `PLAY TAPE`, press a key and the tape starts.
The deck stops again when a `LOAD` finishes and starts at the next
`PLAY TAPE`, which is what you would do by hand on a real Atom. A loader that
reads the tape without going through the Atom's `LOAD` needs *Play* from the
menu. While the tape plays, the Atom runs as fast as the PicoCalc allows,
about 2.7× real time, and makes no sound. A UEF in the deck takes every load,
so eject it to go back to `.atm` files. `SAVE` still writes `.atm` files.

For example, Chuckie Egg's `cchuck.uef` names `CHUCKIE` as its first file:

```
LOAD "CHUCKIE"      press a key at PLAY TAPE; about 20 seconds
RUN                 the loader fetches the game itself; about 3½ minutes more
```

If a load never finishes and the tape plays on to its end, the name was wrong
or empty. The Atom passes over every file whose name does not match, without
saying so.

Games that need a 32 KB Atom have it: RAM runs from `#0000` to `#7FFF`.

**Discs** are Acorn disc images in `/atom/discs/`: `.ssd`, `.dsk` or `.40t` for
one side, `.dsd` for two. The menu's *Discs* page puts one in drive 0 or 1. As
on a real Atom, the disc system sleeps until you wake it, and the commands are
the Acornsoft Atom Disc Pack manual's: `*DOS`, then `*CAT`, `*LOAD NAME`,
`*RUN NAME` or `*SAVE NAME start end` (hex, no `#`). A BASIC program loads with
`LOAD "NAME"` and starts with `RUN`. `*DRIVE 1` changes drive; drives 2 and 3 are the second sides of 0 and 1.
Writes go straight into the image file. Set the file read-only on your computer
to write-protect the disc. The Atom then says `DISK PROT`. An empty drive makes
the Atom wait until a disc goes in, as a real drive with its door open would.
AtomDOS needs `dosrom.rom`; without it, discs do nothing.

**Game keymaps** put a game's keys under one hand. Atom games scan the keyboard
themselves, and many were laid out for keys that sit far apart on the PicoCalc.
The menu's *Keys* item picks a layout. It lays a few keys over the standard map,
so every other key still types. The built-in *Games* layout, for Galaxians and
games like it, puts left on `Left`, right on `Right` and fire on `]`; choose it
in the menu before playing. Further layouts are text files in
`/atom/keymaps/`, one binding per line:

```
# Bouncing Babies: SHIFT moves left, REPT moves right
name  = BABIES
left  = SHIFT
right = REPT
tapes = BABIES
```

A key on the left is a PicoCalc key: `left`, `right`, `up`, `down`, `space`,
`enter`, `backspace`, `tab`, `del`, `esc`, or any single character, shifted or
not. A target on the right is an Atom key by the name on its keycap (`A`–`Z`,
`0`–`9`, the punctuation, `SPACE`, `RETURN`, `UPDOWN`, `LEFTRIGHT`, `COPY`,
`LOCK`, `DELETE`, `ESCAPE`) or one of the lines `CTRL`, `SHIFT` and `REPT`.
The optional `tapes` line names up to four programs whose loading selects the
layout. A line starting with `#` is a comment, except `# = ...`, which binds
the `#` key. A file's `name` must differ from every other layout's, built-in
ones included. A file that does not parse is left out, and the menu names the
file and the line.

**Snapshots** save and restore the whole machine from the menu, in four slots
kept in `/atom/snaps/`. They contain no ROM bytes, and load only over the same
ROMs.

The menu's *Display* page switches the screen between colour and **mono**,
which is how most Atoms looked without the colour board. In mono, blue and red
are the darkest grey. It also turns the **border** on or off. The border is
the colour the video chip draws around its picture: black in text modes, and
green or buff in the graphics modes. The backlight is on the same page.
A change made in the menu lasts until the power goes off. To choose how the
PicoCalc starts, use the settings file below.

**Settings** at power-on come from `/atom/pico-atom.cfg` on the card, if there
is one. Each line sets one thing, and a setting the file leaves out keeps its
default. This file shows every setting, at its default except the backlight,
which is left alone unless the file sets it:

```
# /atom/pico-atom.cfg
screen    = mono       # or colour
border    = on         # or off
backlight = 8          # 1-15, as the menu shows it; leave out to keep the last
volume    = 8          # 0-8
keys      = standard   # or a layout's name, as the menu shows it
tape      =            # a file in /atom/tapes/, in the deck at power-on
turbo     = on         # run fast while a .uef plays
drive0    =            # a disc image in /atom/discs/, in drive 0 at power-on
drive1    =
upper_ram = on         # RAM at #4000-#7FFF, the 32 KB Atom games expect
dos       = on         # the disc controller, for AtomDOS
```

A name without a leading `/` is looked for in the tape or disc folder, and a
path starting with `/` is used as written, spaces included. Upper and lower
case are the same. A `#` at the start of a line or after a space starts a
comment, so a file name may still contain one. If a line is wrong, the PicoCalc
skips that line and still uses the rest. Something the card does not have, such
as a layout, tape or disc, is skipped the same way. The menu's bottom row then
names the first problem, for example `CFG LINE 3: NO SUCH SETTING`.
The file is read only at power-on, and the menu does not write to it.
