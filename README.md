# pico-atom

![Atom Emulator](/assets/mono.png)

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
and is yours to fill. The menu's *Machine* page can put any other `.rom` in
`/atom/roms/` in that socket instead:

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

Copy them to the SD card as `/atom/roms/akernel.rom` and so on. 

## Software

The emulator takes three kinds of file, each in its own folder on the card:
`.atm` tapes and `.uef` tape images in `/atom/tapes/`, and disc images
(`.ssd`, `.dsk`, `.40t`, `.dsd`) in `/atom/discs/`. *Using it* below says how
each one loads.

- **The Acorn Atom Software Archive** — the largest collection, several
  thousand programs gathered by the stardot.org.uk community. Download a
  release zip from <https://github.com/hoglet67/AtomSoftwareArchive/releases>.
  It is laid out for AtoMMC, the Atom's SD card interface, so its programs are
  ATM files **without an extension**, in folders by publisher. Copy the ones
  you want to `/atom/tapes/` and add `.atm` to each name: `GALAXI` becomes
  `GALAXI.atm`, and `LOAD "GALAXI"` finds it. The `.DSK` and `.dsd` images in
  the archive go in `/atom/discs/` as they are.
- **Retro Software Releases**, <https://www.retrosoftware.co.uk/wiki/index.php?title=RetroReleases#Freeware> - High-quality freeware games
- **Archive 13 as UEF**, <https://archive.org/details/atom-archive-13-uef> —
  an older release of the same archive converted to tape images, all in one zip,
  with a few `.uef` files to download on their own.
- **Games as UEF**, <https://archive.org/details/acorn-atom-games-uef> —
  commercial games of the 1980s, and newer ones such as Kees van Oss's Atomic
  Chuckie Egg and Galaforce. Choose the `[Atomulator]` or colour versions
  where there is a choice.
- **Atomulator**, <https://atomulator.acornatom.co.uk/>, and
  **acornatom.nl**, <https://www.acornatom.nl/> (in Dutch), carry software,
  documentation and news.
- **The stardot.org.uk forums**, <https://www.stardot.org.uk/forums/>, in the
  Acorn Atom section, are where new Atom software and fixes are announced.

## Using it

![Emulator Menu](/assets/menu.png)

The emulator boots straight to the Atom's `>` prompt.

| Atom key | PicoCalc |
|---|---|
| `REPT` | `Tab` — hold it, then hold the key to repeat |
| `COPY` | `Alt`+`C` |
| `LOCK` | `Alt`+`L` |
| `BREAK` | `Alt`+`K` |
| the emulator's menu | `Alt`+`M` |
| one menu page, then back to the Atom | `F1` Tapes, `F2` Discs, `F3` Snapshots, `F4` Setup, `F5` Machine |
| these keys, on the panel | `Alt`+`H` |
| the About page | `F10` |
| pause | `Alt`+`P` |

`|`, `{`, `}`, `` ` `` and `~` are the Atom's shifted `\`, `[`, `]`, `@` and
`^`, and the Atom shows each as that key in inverse. `|` is BASIC's OR:
`PRINT 5|3` prints 7.

**Pause.** `Alt`+`P` stops the Atom where it is, silences it and dims the
screen. The bottom line says `PAUSED`. Any key carries on, and that key is not
typed. `Alt`+`M` goes to the menu instead, and `F1`–`F5`, `F10` and `Alt`+`H` to its pages. A tape that was playing stops with
the Atom and carries on with it.

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
about 2.7× real time, or about twice that with the PicoCalc at 300 MHz, and
makes no sound. A UEF in the deck takes every load
and every save, so eject it to go back to `.atm` files. The Atom reads a tape
only at 1 MHz, as on a real one: at 2 or 4 MHz the tape will not play, and
the bottom line says `NEEDS 1 MHZ`. `SAVE` records at any speed.

**Recording.** With a UEF in the deck, `SAVE "NAME"` records onto it, as onto
a real cassette. The Atom says `RECORD TAPE`; press a key, and the recording
is added after everything already on the tape. When the `SAVE` finishes, the
tape is written back to the card before the Atom carries on. To start a blank
tape, choose *New tape* on the *Tapes* page: it makes `TAPE01.uef` (or the
next free number) and puts it in the deck. Recording leaves the tape at its
end, so choose *Rewind* before loading what you saved. A program that saves
without the Atom's `SAVE` needs *Record* from the menu, then *Stop
recording*. A tape can hold about 36 minutes of recording. A UEF that is
gzipped, or set read-only on your computer, is protected: the `SAVE` goes
nowhere, and the bottom line says `PROTECTED`.

![Chuckie Egg](/assets/chuckie.png)

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

The menu's *Setup* page switches the screen between colour and **mono**,
which is how most Atoms looked without the colour board. In mono, blue and red
are the darkest grey. It also turns the **border** on or off. The border is
the colour the video chip draws around its picture: black in text modes, and
green or buff in the graphics modes. **Background** puts text on dark green
(or dark orange), as the video chip drew it, instead of black. It changes
nothing in mono, where the chip's dark green was black. **Status line** turns
the line of text along the bottom of the screen on or off. That line shows the
tape in the deck, whether it is playing, stopped or recording, how far through
it is, and the speed while it runs fast, and on the right each disc drive
while it is in use. The backlight is on the same page.
A change made in the menu lasts until the power goes off, unless you choose
*Save settings* on the menu's first page, which writes it into the settings
file below.

**The machine.** The menu's *Machine* page sets what an Atom owner would have
changed with the lid off: 16 or 32 KB of RAM below the screen, a 1 or 2 MHz
clock, AtomDOS and the ROM in the utility socket. It also sets the PicoCalc's
own speed, *Pico clock*: 150 MHz, the chip's rated speed, or 300 MHz, an
overclock. A 4 MHz Atom needs 300 MHz, so the *Clock* row offers 4 MHz only
when 300 MHz is chosen. Left and right choose, and a
`*` marks each row you have changed. Nothing happens until *Apply and
restart*, which switches the Atom off and on again with the new machine, so
the program in memory is lost. The tape goes back in at its start and the
discs stay in their drives. A change to *Pico clock* can only take effect when
the PicoCalc starts, so *Apply* writes the machine into the settings file
below and restarts the PicoCalc. The tape and discs are then the ones the
file names. At 2 MHz BASIC runs twice as fast and the bell
sounds an octave higher, but `WAIT` and the screen keep their speed. At 4 MHz
it is four times as fast and two octaves higher. A snapshot taken at one clock
does not load at another. The *About* page shows the
firmware's version, the board, and each ROM with the first eight digits of its
SHA-1, to check against the list above. The *Setup* page's *Perf line* puts
the emulator's own figures along the top of the screen: how much of the
PicoCalc's first core the Atom takes, how many times real time it could run,
the slowest screen update in the last second, frames dropped, and sound
underruns and late refills since power-on.

**Settings** at power-on come from `/atom/pico-atom.cfg` on the card, if there
is one. Each line sets one thing, and a setting the file leaves out keeps its
default. This file shows every setting, at its default except the backlight,
which is left alone unless the file sets it:

```
# /atom/pico-atom.cfg
screen    = mono       # or colour
border    = on         # or off
background = black     # or dark
status    = on         # the line along the bottom; or off
backlight = 8          # 1-15, as the menu shows it; leave out to keep the last
volume    = 8          # 0-8
keys      = standard   # or a layout's name, as the menu shows it
tape      =            # a file in /atom/tapes/, in the deck at power-on
turbo     = on         # run fast while a .uef plays
drive0    =            # a disc image in /atom/discs/, in drive 0 at power-on
drive1    =
upper_ram = on         # RAM at #4000-#7FFF, the 32 KB Atom games expect
dos       = on         # the disc controller, for AtomDOS
clock     = 1          # the Atom's MHz: 1, 2, or 4 with host_clock = 300
utility   = utility.rom # a file in /atom/roms/ for #A000, or none
perf      = off        # the emulator's figures along the top; or on
host_clock = 150       # the PicoCalc's MHz: 150, or 300, an overclock
via_port_b = on        # the VIA's port B, the user port, on the side pins; or off
via_port_b_pins = gp2 gp3 gp4 gp5 gp21 gp28 nc nc   # PB0 first; nc for none
```

A name without a leading `/` is looked for in the tape or disc folder, and a
path starting with `/` is used as written, spaces included. Upper and lower
case are the same. A `#` at the start of a line or after a space starts a
comment, so a file name may still contain one. If a line is wrong, the PicoCalc
skips that line and still uses the rest. Something the card does not have, such
as a layout, tape or disc, is skipped the same way. The menu's bottom row then
names the first problem, for example `CFG LINE 3: NO SUCH SETTING`.
The file is read only at power-on.

*Save settings* on the menu edits this file rather than replacing it. Each
line it changes keeps its place and its comment, and only the value after the
`=` changes. A setting the file does not mention is added at the end, but only
if it differs from the default. Comments, blank lines and anything it does not
understand are left as they are. It saves the screen, border, background,
status line, perf line, backlight, volume, keys, the tape in the deck, the
discs in the drives, VIA port B and its pins, and the machine as it is running: `upper_ram`, `dos`,
`clock` and `utility`. `host_clock` is saved by the *Machine* page's *Apply*,
and `turbo` is only ever what you wrote. If a
setting appears twice, it saves nothing and says so, since it cannot tell
which line you meant. With no file on the card it makes one.

## Books

The Atom's own manuals are the best place to start, and most of them are on
the Internet Archive.

- **Atomic Theory and Practice**, David Johnson-Davies, Acorn, 1980 —
  <https://archive.org/details/atomic_theory_and_practice>. The manual that
  came with the machine: BASIC from the first `PRINT`, then graphics, sound,
  the assembler and the hardware. Read this first. The copy there is
  hoglet67's edition of 2022.
- **Getting Acquainted with Your Acorn Atom**, Trevor Sharples and Tim
  Hartnell, Interface, 1981 —
  <https://archive.org/details/getting-acquainted-with-your-acorn-atom>.
  A gentler introduction for beginners, with many short programs to type in.
- **Practical Programs for the BBC Computer and Acorn Atom**, David
  Johnson-Davies, 1982 —
  <https://archive.org/details/practicalprogram0000john>. Programs to type in
  and learn from. The Internet Archive lends this one: borrowing needs a free
  account.
- **Splitting the Atom**, J. R. Stevenson and J. C. Rockett, 1982 —
  <https://archive.org/details/splitting-the-atom>. "A manual for informed
  users", for going below BASIC into the machine itself.
- **Acorn Atom Technical Manual**, Acorn, 1980 —
  <https://archive.org/details/acorn-atom-technical-manual>. Acorn's own
  reference for the hardware.
- **Acorn Atom Manuals**, <https://archive.org/details/AcornAtomManuals> —
  Acornsoft's *Forth Theory and Practice*, *Lisp Theory and Practice* and the
  *AtomCalc* manual, for the languages and programs beyond BASIC.

For the magazines, *Atom News*, *Acorn User* and others,
[search the Internet Archive for "Acorn Atom"](https://archive.org/search?query=%22acorn+atom%22).
