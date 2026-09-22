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

Input formats (--format, or guessed from the extension):

  text    '.' and '#' (also ' ' and '*', '0' and '1'), one line per glyph
          row, blank lines and #-comments ignored. Rows are grouped into
          glyphs by --rows.
  hex     whitespace- or comma-separated byte values, any base prefix.
  bin     raw bytes, row-major.

Examples:

  ./tools/mkfont.py font.txt -o src/core/mc6847_font.h
  ./tools/mkfont.py rom.bin --rows 7 --pad-top 2 --pad-bottom 3 -o ...
  ./tools/mkfont.py src/core/mc6847_font.h --dump      # proof it
"""

import argparse
import re
import sys

GLYPHS = 64
CELL_ROWS = 12
CELL_COLS = 8

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


def parse_hex(data):
    tokens = re.findall(r"0[xX][0-9a-fA-F]+|\$[0-9a-fA-F]+|[0-9a-fA-F]{2}", data)
    out = []
    for t in tokens:
        t = t.replace("$", "0x")
        out.append(int(t, 16) if t.lower().startswith("0x") else int(t, 16))
    return out


def place(value, src_cols, align):
    """Put a src_cols-wide glyph row inside an 8-wide cell."""
    if src_cols == CELL_COLS:
        return value & 0xFF
    shift = {
        "left": CELL_COLS - src_cols,
        "right": 0,
        "centre": (CELL_COLS - src_cols) // 2,
    }[align]
    return (value << shift) & 0xFF


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="header to write (default: stdout)")
    ap.add_argument("--format", choices=("text", "hex", "bin"))
    ap.add_argument("--rows", type=int, default=CELL_ROWS,
                    help="rows per glyph in the source (default 12)")
    ap.add_argument("--cols", type=int, default=CELL_COLS,
                    help="columns per glyph in the source (default 8)")
    ap.add_argument("--pad-top", type=int, default=0)
    ap.add_argument("--pad-bottom", type=int, default=0)
    ap.add_argument("--align", choices=("left", "centre", "right"), default="left",
                    help="where a narrow glyph sits in the 8-wide cell")
    ap.add_argument("--base", default="0x20",
                    help="ASCII code of glyph 0, for dump labels only (default 0x20)")
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
        fmt = {"bin": "bin", "rom": "bin", "txt": "text", "h": "hex"}.get(
            args.input.rsplit(".", 1)[-1].lower(), "text")

    if fmt == "bin":
        values = list(open(args.input, "rb").read())
    else:
        data = open(args.input, "r", encoding="utf-8", errors="replace").read()
        values = parse_hex(data) if fmt == "hex" else parse_text(data, args.cols)

    want = GLYPHS * args.rows
    if len(values) != want:
        die(f"got {len(values)} glyph rows, expected {want} "
            f"({GLYPHS} glyphs x {args.rows} rows)")

    font = []
    for g in range(GLYPHS):
        cell = [0] * args.pad_top
        for r in range(args.rows):
            cell.append(place(values[g * args.rows + r], args.cols, args.align))
        cell += [0] * args.pad_bottom
        font.append(cell)

    base = int(args.base, 0)

    if args.dump:
        for g, cell in enumerate(font):
            code = base + g
            ch = chr(code) if 0x20 <= code < 0x7F else "?"
            print(f"# glyph {g:2d}  index 0x{g:02X}  ascii 0x{code:02X} '{ch}'")
            for row in cell:
                print("".join("#" if (row >> (7 - i)) & 1 else "." for i in range(8)))
            print()
        return

    out = [
        "/* mc6847_font.h — MC6847 internal character ROM.",
        " *",
        " * GENERATED by tools/mkfont.py. Do not edit by hand; regenerate.",
        f" * Source: {args.input}",
        " *",
        " * 64 glyphs x 12 rows, one byte per row, bit 7 leftmost.",
        " * design.md §16 requires this to be verified by golden image, not",
        " * merely transcribed — render the full set and compare before",
        " * trusting it.",
        " */",
        "#ifndef PICO_ATOM_MC6847_FONT_H",
        "#define PICO_ATOM_MC6847_FONT_H",
        "",
        "#include <stdint.h>",
        "",
        "static const uint8_t mc6847_font[64][12] = {",
    ]
    for g, cell in enumerate(font):
        code = base + g
        ch = chr(code) if 0x20 <= code < 0x7F else "?"
        body = ", ".join(f"0x{v:02X}" for v in cell)
        out.append(f"    /* {g:2d} '{ch}' */ {{ {body} }},")
    out += ["};", "", "#endif /* PICO_ATOM_MC6847_FONT_H */", ""]

    text = "\n".join(out)
    if args.output:
        open(args.output, "w").write(text)
        print(f"mkfont: wrote {args.output} ({GLYPHS} glyphs)", file=sys.stderr)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
