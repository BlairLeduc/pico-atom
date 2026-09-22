#!/usr/bin/env python3
"""Convert an MC6847 character ROM into src/core/mc6847_font.h.

design.md §16 requires the character ROM to be transcribed from a primary
source and then verified by golden image, so this tool does two jobs: it
converts a transcription into the header, and it dumps a header (or any
other supported input) back out as text so the transcription can be
proofed against the datasheet figure before it is trusted.

The emulator wants 64 glyphs x 12 rows x 8 bits, bit 7 leftmost. Sources
are often 5x7 in a smaller cell, so --pad-top/--pad-bottom/--align place
a narrower or shorter glyph inside the 8x12 cell.

The real ROM puts 7 glyph rows at rows 3..9 of the 12-row cell, so a 5x7
source wants --rows 7 --cols 5 --pad-top 3 --pad-bottom 2.

Input formats (--format, or guessed from the extension):

  text    '.' and '#' (also ' ' and '*', '0' and '1'), one line per glyph
          row, blank lines and #-comments ignored. Rows are grouped into
          glyphs by --rows.
  hex     whitespace- or comma-separated byte values, any base prefix.
  bin     raw bytes, row-major.
  c       a C array initialiser, e.g. the src/core/mc6847_font.c that
          font2c emits. Only the bytes inside the braces are read.

The glyph is 5 pixels wide and, in a real MC6847 ROM, sits in bits 5..1
of its byte rather than at bit 7. --left-bit says which bit is the
leftmost pixel (default 5); it affects how --dump draws a glyph and
nothing else, since the emitted data preserves the source layout.

Examples:

  ./tools/mkfont.py font.txt -o src/core/mc6847_font.c
  ./tools/mkfont.py rom.bin --rows 7 --pad-top 3 --pad-bottom 2 -o ...
  ./tools/mkfont.py src/core/mc6847_font.c --dump      # proof it by eye
"""

import argparse
import re
import sys
from pathlib import Path

GLYPHS = 64
CELL_ROWS = 12
CELL_COLS = 8
MC6847_GLYPH_W = 5

ON = set("#*1Xx")
OFF = set(". 0_")


def die(msg):
    sys.exit(f"mkfont: {msg}")


def parse_text(data, cols):
    """Rows first, comments second.

    '#' is both the natural lit-pixel character and the natural comment
    character, so a line is tested for being a glyph row *before* it is
    tested for being a comment. Getting that order wrong silently drops
    every glyph row that begins with a lit pixel, which looks like a
    truncated file rather than a parsing bug. Prefer ';' or '//' for
    comments; a '#' comment works as long as it contains something
    outside the pixel alphabet, which ordinary prose does.
    """
    rows = []
    for raw in data.splitlines():
        line = raw.strip()
        if not line:
            continue

        is_pixels = all(c in ON or c in OFF for c in line)
        if is_pixels:
            if len(line) != cols:
                die(f"row {line!r} is {len(line)} wide, expected {cols} "
                    f"(use --cols if the source is narrower)")
            v = 0
            for i, c in enumerate(line):
                if c in ON:
                    v |= 1 << (cols - 1 - i)
            rows.append(v)
            continue

        if line.startswith(("#", ";", "//")):
            continue
        # Anything else is a label and is ignored.
    return rows


def parse_c(data):
    """Bytes from inside the outermost braces of a C array initialiser."""
    start = data.find("{")
    end = data.rfind("}")
    if start < 0 or end < 0:
        die("no C array initialiser found (is this the .h rather than the .c?)")
    body = data[start + 1:end]
    # Strip comments before looking for bytes: the per-glyph labels this
    # tool emits contain decimal indices that would otherwise be read as
    # font data.
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    return [int(t, 16) for t in re.findall(r"0[xX]([0-9a-fA-F]{1,2})\b", body)]


def parse_hex(data):
    tokens = re.findall(r"0[xX][0-9a-fA-F]+|\$[0-9a-fA-F]+|[0-9a-fA-F]{2}", data)
    out = []
    for t in tokens:
        t = t.replace("$", "0x")
        out.append(int(t, 16) if t.lower().startswith("0x") else int(t, 16))
    return out


def place(value, src_cols, left_bit):
    """Put a src_cols-wide row into the byte with its leftmost pixel at
    bit `left_bit`. The result is the byte as it will be *stored*, which
    for a real MC6847 ROM means a 5-wide glyph in bits 5..1."""
    shift = left_bit - (src_cols - 1)
    if shift < 0:
        die(f"a {src_cols}-wide glyph does not fit with its leftmost pixel "
            f"at bit {left_bit}; raise --left-bit or lower --cols")
    return (value << shift) & 0xFF


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="header to write (default: stdout)")
    ap.add_argument("--format", choices=("text", "hex", "bin", "c"))
    ap.add_argument("--rows", type=int, default=CELL_ROWS,
                    help="rows per glyph in the source (default 12)")
    ap.add_argument("--cols", type=int, default=CELL_COLS,
                    help="columns per glyph in the source (default 8)")
    ap.add_argument("--pad-top", type=int, default=0)
    ap.add_argument("--pad-bottom", type=int, default=0)
    ap.add_argument("--left-bit", type=int, default=None,
                    help="bit holding the leftmost pixel of a glyph. Defaults to 5 "
                         "for ROM-shaped input (c/bin/hex) and for a source 5 "
                         "columns or narrower, which is how a real MC6847 ROM is "
                         "laid out; otherwise to --cols minus one.")
    ap.add_argument("--name", default="font_6847",
                    help="symbol name to emit (default font_6847)")
    ap.add_argument("--dump", action="store_true",
                    help="print the glyphs as text instead of writing a header")
    args = ap.parse_args()

    if args.pad_top + args.rows + args.pad_bottom != CELL_ROWS:
        die(f"--pad-top {args.pad_top} + --rows {args.rows} + --pad-bottom "
            f"{args.pad_bottom} is {args.pad_top + args.rows + args.pad_bottom}, "
            f"not the {CELL_ROWS} rows of a cell")
    if not 1 <= args.cols <= CELL_COLS:
        die(f"--cols {args.cols} is outside 1..{CELL_COLS}")

    fmt = args.format
    if fmt is None:
        fmt = {"bin": "bin", "rom": "bin", "txt": "text",
               "hex": "hex", "c": "c", "h": "c"}.get(
            args.input.rsplit(".", 1)[-1].lower(), "text")

    if args.left_bit is None:
        args.left_bit = 5 if (fmt in ("c", "bin", "hex") or args.cols <= 5) \
                          else args.cols - 1
    if not 0 <= args.left_bit <= 7:
        die(f"--left-bit {args.left_bit} is outside 0..7")

    if fmt == "bin":
        values = list(open(args.input, "rb").read())
    else:
        data = open(args.input, "r", encoding="utf-8", errors="replace").read()
        values = {"hex": parse_hex, "c": parse_c}.get(fmt, lambda d: parse_text(d, args.cols))(data)

    want = GLYPHS * args.rows
    if len(values) != want:
        die(f"got {len(values)} glyph rows, expected {want} "
            f"({GLYPHS} glyphs x {args.rows} rows)")

    # Raw formats are already storage bytes; a text source has to be
    # placed into the field --left-bit describes.
    raw = fmt in ("c", "bin", "hex")

    font = []
    for g in range(GLYPHS):
        cell = [0] * args.pad_top
        for r in range(args.rows):
            v = values[g * args.rows + r]
            cell.append(v & 0xFF if raw else place(v, args.cols, args.left_bit))
        cell += [0] * args.pad_bottom
        font.append(cell)

    def ascii_of(index):
        """The MC6847's internal order: 0..31 are $40..$5F, 32..63 $20..$3F."""
        return 0x20 + ((index + 0x20) & 0x3F)

    shift = 7 - args.left_bit

    if args.dump:
        for g, cell in enumerate(font):
            code = ascii_of(g)
            ch = chr(code) if 0x20 <= code < 0x7F else "?"
            print(f"; glyph {g:2d}  index 0x{g:02X}  ascii 0x{code:02X} '{ch}'")
            for row in cell:
                v = (row << shift) & 0xFF
                print("".join("#" if (v >> (7 - i)) & 1 else "." for i in range(8)))
            print()
        return

    n = args.name
    banner = [
        f"/* MC6847 internal character ROM — GENERATED by tools/mkfont.py.",
        f" * Source: {args.input}. Do not edit by hand; regenerate.",
        " *",
        f" * {GLYPHS} glyphs x {CELL_ROWS} rows, flat: glyph g row r is",
        f" * {n}[g * {CELL_ROWS} + r]. The glyph is "
        f"{MC6847_GLYPH_W} px wide in bits {args.left_bit}..{args.left_bit - 4}.",
        " *",
        " * Glyph order is the MC6847's own, not plain ASCII:",
        " *   index  0..31 -> ASCII $40..$5F,  index 32..63 -> $20..$3F.",
        " *",
        " * design.md §16 requires this to be verified by golden image, not",
        " * merely transcribed. Render it with tools/vdg-ppm and look.",
        " */",
    ]

    src = banner + ["", "#include <stdint.h>", "",
                    f"const uint8_t {n}[{GLYPHS * CELL_ROWS}] = {{"]
    for g, cell in enumerate(font):
        code = ascii_of(g)
        ch = chr(code) if 0x20 <= code < 0x7F else "?"
        body = ", ".join(f"0x{v:02X}" for v in cell)
        src.append(f"    /* {g:2d} '{ch}' */ {body},")
    src += ["};", ""]

    guard = "PICO_ATOM_MC6847_FONT_H"
    hdr = banner + ["", f"#ifndef {guard}", f"#define {guard}", "",
                    "#include <stdint.h>", "",
                    f"extern const uint8_t {n}[{GLYPHS * CELL_ROWS}];", "",
                    f"#endif /* {guard} */", ""]

    if not args.output:
        sys.stdout.write("\n".join(src))
        return

    cpath = Path(args.output)
    if cpath.suffix != ".c":
        die(f"--output should name the .c file, got {cpath.name}")
    hpath = cpath.with_suffix(".h")
    cpath.write_text("\n".join(src))
    hpath.write_text("\n".join(hdr))
    print(f"mkfont: wrote {cpath} and {hpath} ({GLYPHS} glyphs)", file=sys.stderr)


if __name__ == "__main__":
    main()
