/* textpage.h — a page of text in alpha-mode VRAM, for the screens the
 * emulator draws itself: the no-ROMs page (§11.1) and the menu (§13).
 *
 * The page is presented through the ordinary renderer, so it costs no
 * new drawing code and dismissing it is display_invalidate() (§13). The
 * MC6847 has upper case only; lower case is folded up.
 */
#ifndef PICO_ATOM_TEXTPAGE_H
#define PICO_ATOM_TEXTPAGE_H

#include <stdbool.h>
#include <stdint.h>

#define TEXT_COLS 32
#define TEXT_ROWS 16

void textpage_clear(uint8_t *vram);
void textpage_put(uint8_t *vram, int row, int col, const char *s, bool inverse);

/* A whole row: the text, then blanks to the edge, all in one video. */
void textpage_line(uint8_t *vram, int row, const char *s, bool inverse);

#endif /* PICO_ATOM_TEXTPAGE_H */
