#!/usr/bin/env python3
"""Round-trip test for tools/mkfont.py.

The bug this exists for: '#' is both the lit-pixel character and the
comment character, and testing for a comment before testing for a glyph
row silently drops every row that begins with a lit pixel. The file then
looks truncated rather than mis-parsed, so the failure is a confusing
count error a long way from its cause.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

MKFONT = sys.argv[1] if len(sys.argv) > 1 else "tools/mkfont.py"
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"FAIL {msg}")


def run(args, expect_ok=True):
    r = subprocess.run([sys.executable, MKFONT] + args,
                       capture_output=True, text=True)
    if expect_ok and r.returncode != 0:
        check(False, f"{' '.join(args)} failed: {r.stderr.strip()}")
    return r


def pixel_rows(text):
    return [l for l in text.splitlines() if l and all(c in ".#" for c in l)]


def render(values, cols):
    return ["".join("#" if (v >> (cols - 1 - i)) & 1 else "." for i in range(cols))
            for v in values]


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)

    # 1. A full 8x12 source round-trips byte for byte.
    src = [(g * 7 + r * 13) & 0xFF for g in range(64) for r in range(12)]
    (tmp / "a.txt").write_text("\n".join(render(src, 8)))
    out = run([str(tmp / "a.txt"), "--dump"])
    check(pixel_rows(out.stdout) == render(src, 8),
          "8x12 source did not round-trip")

    # 2. Rows beginning with a lit pixel survive. This is the regression.
    lit = [0x80 | ((g + r) & 0x7F) for g in range(64) for r in range(12)]
    (tmp / "lit.txt").write_text("\n".join(render(lit, 8)))
    out = run([str(tmp / "lit.txt"), "--dump"])
    rows = pixel_rows(out.stdout)
    check(len(rows) == 768,
          f"rows starting with a lit pixel were dropped: got {len(rows)} of 768")
    check(all(r[0] == "#" for r in rows),
          "a row that should start lit came back dark")

    # 3. Comments in all three styles are ignored, and prose starting
    #    with '#' is still a comment.
    noisy = ["; semicolon", "// slashes", "# prose comment", "glyph 0 label"]
    noisy += render(src, 8)
    (tmp / "noisy.txt").write_text("\n".join(noisy))
    out = run([str(tmp / "noisy.txt"), "--dump"])
    check(pixel_rows(out.stdout) == render(src, 8), "comments were not ignored")

    # 4. A 5x7 source pads into the 8x12 cell the way the real ROM is
    #    laid out: 3 blank rows above, 7 glyph rows, 2 below, and the
    #    5-wide glyph in bits 5..1 so three spacing columns sit at the
    #    right of the cell.
    narrow = [(g ^ r) & 0x1F for g in range(64) for r in range(7)]
    (tmp / "n.txt").write_text("\n".join(render(narrow, 5)))
    out = run([str(tmp / "n.txt"), "--rows", "7", "--cols", "5",
               "--pad-top", "3", "--pad-bottom", "2", "--dump"])
    rows = pixel_rows(out.stdout)
    check(len(rows) == 768, f"padded source gave {len(rows)} rows, expected 768")
    check(rows[0] == rows[1] == rows[2] == "........",
          "--pad-top 3 should leave three blank rows")
    check(rows[10] == rows[11] == "........",
          "--pad-bottom 2 should leave two blank rows")
    check(all(r[5:] == "..." for r in rows),
          "a 5-wide glyph should leave the rightmost three columns clear")

    # 5. Wrong geometry is rejected rather than quietly mangled.
    r = run([str(tmp / "n.txt"), "--rows", "7", "--cols", "5",
             "--pad-top", "2"], expect_ok=False)
    check(r.returncode != 0, "padding that does not reach 12 rows should be rejected")
    r = run([str(tmp / "a.txt"), "--rows", "7", "--cols", "5",
             "--pad-top", "3", "--pad-bottom", "2"], expect_ok=False)
    check(r.returncode != 0, "a width mismatch should be rejected")
    r = run([str(tmp / "n.txt"), "--rows", "7", "--cols", "5",
             "--pad-top", "3", "--pad-bottom", "2",
             "--left-bit", "3"], expect_ok=False)
    check(r.returncode != 0,
          "a glyph too wide for the chosen --left-bit should be rejected")

    # 6. Emitting writes a .c/.h pair in the shape the build expects.
    run([str(tmp / "a.txt"), "-o", str(tmp / "font.c")])
    csrc = (tmp / "font.c").read_text()
    hsrc = (tmp / "font.h").read_text()
    check("const uint8_t font_6847[768] = {" in csrc,
          "the .c should define the flat font array")
    check("extern const uint8_t font_6847[768];" in hsrc,
          "the .h should declare it extern")
    check("#ifndef PICO_ATOM_MC6847_FONT_H" in hsrc,
          "the .h should have an include guard")
    check(f"0x{src[0]:02X}" in csrc, "the .c should contain the first byte")

    r = run([str(tmp / "a.txt"), "-o", str(tmp / "font.h")], expect_ok=False)
    check(r.returncode != 0, "--output naming a .h should be rejected")

    # 7. A C array round-trips byte for byte, which is how a font2c
    #    output is re-read and proofed.
    out = run([str(tmp / "font.c"), "--dump"])
    rows = pixel_rows(out.stdout)
    check(len(rows) == 768, f"C round trip gave {len(rows)} rows, expected 768")
    run([str(tmp / "font.c"), "-o", str(tmp / "again.c")])
    import re as _re
    first = [int(x, 0) for x in _re.findall(r"0x[0-9a-fA-F]{2}", csrc)]
    again = [int(x, 0) for x in _re.findall(r"0x[0-9a-fA-F]{2}",
                                            (tmp / "again.c").read_text())]
    check(first == again, "regenerating from a .c should be byte-identical")

    # 8. For raw input the byte is stored as-is and --left-bit says only
    #    where the glyph sits when drawing it. 5 is the default, because
    #    that is how a real ROM is laid out.
    (tmp / "lb.hex").write_text(" ".join(["0x2A"] * (64 * 12)))   # #.#.# at bits 5..1
    out = run([str(tmp / "lb.hex"), "--dump"])
    check(pixel_rows(out.stdout)[0] == "#.#.#...",
          f"default --left-bit 5 should draw #.#.#..., got "
          f"{pixel_rows(out.stdout)[0]!r}")
    out = run([str(tmp / "lb.hex"), "--left-bit", "7", "--dump"])
    check(pixel_rows(out.stdout)[0] == "..#.#.#.",
          f"--left-bit 7 should draw the byte unshifted, got "
          f"{pixel_rows(out.stdout)[0]!r}")

    # An 8-wide text source keeps bit 7 as its leftmost pixel, so the
    # default must follow --cols rather than the ROM convention.
    (tmp / "wide.txt").write_text("\n".join(render([0x2A] * (64 * 12), 8)))
    out = run([str(tmp / "wide.txt"), "--dump"])
    check(pixel_rows(out.stdout)[0] == "..#.#.#.",
          f"an 8-wide text source should draw unshifted, got "
          f"{pixel_rows(out.stdout)[0]!r}")

if failures:
    print(f"{len(failures)} failure(s)")
    sys.exit(1)
print("ok")
