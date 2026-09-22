/* MC6847 internal character ROM.
 *
 * Taken verbatim from XRoar's src/mc6847/font-6847.c, which font2c
 * generated; this file is byte-identical to it, header aside.
 *
 *   XRoar — a Dragon and Tandy 8-bit computer emulator
 *   Copyright 2003-2026 Ciaran Anscomb <xroar@6809.org.uk>
 *   https://www.6809.org.uk/xroar/
 *   GNU General Public License, version 3 or later — the same terms
 *   this project is under, so the data travels with the repository.
 *
 * See THIRD-PARTY.md. design.md §16 asks for the character ROM to be
 * verified by image rather than trusted; that was done with
 * tools/vdg-ppm, and test_mc6847.c pins the layout against the data.
 *
 * 64 glyphs x 12 rows, flat: glyph g row r is font_6847[g * 12 + r].
 * The glyph is 5 px wide in bits 5..1, and its 7 rows sit at rows 3..9
 * of the cell. Glyph order is the MC6847's own, not plain ASCII:
 * index 0..31 -> ASCII $40..$5F, index 32..63 -> $20..$3F.
 */

#include <stdint.h>

extern const uint8_t font_6847[768];

