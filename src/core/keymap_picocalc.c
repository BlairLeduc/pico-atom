/* keymap_picocalc.c — PicoCalc key codes to Atom matrix cells (§10.3).
 *
 * The cells are the kernel ROM's, not a transcription (§16). The MOS
 * scan at #FE71 walks row bit 5 down to bit 0 and column 0 up to 9,
 * counting Y down from #3B, so a key is Y = row * 10 + 9 - col; the
 * table below was read off by pressing every cell of the matrix at the
 * `>` prompt and reading what the MOS put in VRAM, with and without
 * SHIFT, and test_boot.c repeats that for every entry here.
 *
 *          col 0    1    2    3    4    5    6    7    8    9
 *   row 0      -    -   ^v   <>  LOCK   ^    ]    \    [  SPACE
 *   row 1      3    2    1    0   DEL COPY  RET   -    -    -
 *   row 2      -    ,    ;    :    9    8    7    6    5    4
 *   row 3      G    F    E    D    C    B    A    @    /    .
 *   row 4      Q    P    O    N    M    L    K    J    I    H
 *   row 5    ESC    Z    Y    X    W    V    U    T    S    R
 *
 * The five cells marked '-' carry no key on the Atom's keyboard; the
 * MOS decodes them as the control codes #09, #08, #0C, #0B and #0A.
 * The two arrow keys are one cell each and SHIFT picks the direction:
 * up and right unshifted, down and left shifted.
 *
 * Code values are the southbridge firmware's (hardware-notes.md §6;
 * keyboard.h in the PicoCalc repository).
 */

#include "keymatrix.h"

#include <ctype.h>

/* Atom keys as "row, col". */
#define AK_UPDOWN   0, 2
#define AK_LTRT     0, 3
#define AK_LOCK     0, 4
#define AK_CARET    0, 5
#define AK_RBRACKET 0, 6
#define AK_BSLASH   0, 7
#define AK_LBRACKET 0, 8
#define AK_SPACE    0, 9
#define AK_DEL      1, 4
#define AK_COPY     1, 5
#define AK_RETURN   1, 6
#define AK_MINUS    2, 0
#define AK_COMMA    2, 1
#define AK_SEMI     2, 2
#define AK_COLON    2, 3
#define AK_AT       3, 7
#define AK_SLASH    3, 8
#define AK_STOP     3, 9
#define AK_ESC      5, 0

/* Digits: 0-3 on row 1 counting down from column 3, 4-9 on row 2. */
#define AK_0 1, 3
#define AK_1 1, 2
#define AK_2 1, 1
#define AK_3 1, 0
#define AK_4 2, 9
#define AK_5 2, 8
#define AK_6 2, 7
#define AK_7 2, 6
#define AK_8 2, 5
#define AK_9 2, 4

/* Letters: A-G on row 3 from column 6 down, H-Q on row 4 from 9 down,
 * R-Z on row 5 from 9 down. */
#define AK_A 3, 6
#define AK_B 3, 5
#define AK_C 3, 4
#define AK_D 3, 3
#define AK_E 3, 2
#define AK_F 3, 1
#define AK_G 3, 0
#define AK_H 4, 9
#define AK_I 4, 8
#define AK_J 4, 7
#define AK_K 4, 6
#define AK_L 4, 5
#define AK_M 4, 4
#define AK_N 4, 3
#define AK_O 4, 2
#define AK_P 4, 1
#define AK_Q 4, 0
#define AK_R 5, 9
#define AK_S 5, 8
#define AK_T 5, 7
#define AK_U 5, 6
#define AK_V 5, 5
#define AK_W 5, 4
#define AK_X 5, 3
#define AK_Y 5, 2
#define AK_Z 5, 1

#define NOCELL 0, 0

/* PicoCalc codes (keyboard.h). */
#define PC_BACKSPACE 0x08u
#define PC_ENTER     0x0Au
#define PC_ESC       0xB1u
#define PC_LEFT      0xB4u
#define PC_UP        0xB5u
#define PC_DOWN      0xB6u
#define PC_RIGHT     0xB7u
#define PC_BREAK     0xD0u
#define PC_INSERT    0xD1u
#define PC_HOME      0xD2u
#define PC_DEL       0xD4u
#define PC_END       0xD5u
#define PC_PAGE_UP   0xD6u
#define PC_PAGE_DOWN 0xD7u
#define PC_TAB       0x09u

#define LETTER(lc, uc, cell) \
    { lc, cell, 0 }, { uc, cell, KM_SHIFT }

const keymap_t keymap_picocalc[] = {
    /* The MCU sends lower case unshifted and upper case shifted, and
     * the Atom's letters are the other way up: unshifted is capitals,
     * which is what BASIC wants. So PicoCalc Shift is Atom SHIFT. */
    LETTER('a', 'A', AK_A), LETTER('b', 'B', AK_B), LETTER('c', 'C', AK_C),
    LETTER('d', 'D', AK_D), LETTER('e', 'E', AK_E), LETTER('f', 'F', AK_F),
    LETTER('g', 'G', AK_G), LETTER('h', 'H', AK_H), LETTER('i', 'I', AK_I),
    LETTER('j', 'J', AK_J), LETTER('k', 'K', AK_K), LETTER('l', 'L', AK_L),
    LETTER('m', 'M', AK_M), LETTER('n', 'N', AK_N), LETTER('o', 'O', AK_O),
    LETTER('p', 'P', AK_P), LETTER('q', 'Q', AK_Q), LETTER('r', 'R', AK_R),
    LETTER('s', 'S', AK_S), LETTER('t', 'T', AK_T), LETTER('u', 'U', AK_U),
    LETTER('v', 'V', AK_V), LETTER('w', 'W', AK_W), LETTER('x', 'X', AK_X),
    LETTER('y', 'Y', AK_Y), LETTER('z', 'Z', AK_Z),

    /* Characters, by what they are rather than where they sit: the two
     * keyboards shift different things, so ':' is unshifted on the Atom
     * and shifted on the PicoCalc. The Atom has no shifted 0. */
    { '0', AK_0, 0 },
    { '1', AK_1, 0 }, { '!',  AK_1, KM_SHIFT },
    { '2', AK_2, 0 }, { '"',  AK_2, KM_SHIFT },
    { '3', AK_3, 0 }, { '#',  AK_3, KM_SHIFT },
    { '4', AK_4, 0 }, { '$',  AK_4, KM_SHIFT },
    { '5', AK_5, 0 }, { '%',  AK_5, KM_SHIFT },
    { '6', AK_6, 0 }, { '&',  AK_6, KM_SHIFT },
    { '7', AK_7, 0 }, { '\'', AK_7, KM_SHIFT },
    { '8', AK_8, 0 }, { '(',  AK_8, KM_SHIFT },
    { '9', AK_9, 0 }, { ')',  AK_9, KM_SHIFT },
    { '-', AK_MINUS, 0 }, { '=', AK_MINUS, KM_SHIFT },
    { ',', AK_COMMA, 0 }, { '<', AK_COMMA, KM_SHIFT },
    { ';', AK_SEMI,  0 }, { '+', AK_SEMI,  KM_SHIFT },
    { ':', AK_COLON, 0 }, { '*', AK_COLON, KM_SHIFT },
    { '.', AK_STOP,  0 }, { '>', AK_STOP,  KM_SHIFT },
    { '/', AK_SLASH, 0 }, { '?', AK_SLASH, KM_SHIFT },
    { '@', AK_AT, 0 },
    { '^', AK_CARET, 0 },
    { '[', AK_LBRACKET, 0 },
    { '\\', AK_BSLASH, 0 },
    { ']', AK_RBRACKET, 0 },
    { ' ', AK_SPACE, 0 },

    { PC_ENTER,     AK_RETURN, 0 },
    { PC_BACKSPACE, AK_DEL, 0 },
    { PC_DEL,       AK_DEL, 0 },
    { PC_ESC,       AK_ESC, 0 },

    /* REPT is a modifier on the Atom: held while another key is held,
     * that key repeats. So it has to be a key of its own, never an Alt
     * chord, because a key pressed while Alt is down is taken from the
     * Alt layer. The Atom has no Tab; Shift+Tab arrives as Home. */
    { PC_TAB,       NOCELL, KM_REPT },
    { PC_HOME,      NOCELL, KM_REPT },

    /* The MCU sends no Shift+Left or Shift+Right at all (hardware-notes
     * §6.3), so the direction SHIFT would pick is supplied here. */
    { PC_UP,    AK_UPDOWN, 0 },
    { PC_DOWN,  AK_UPDOWN, KM_SHIFT },
    { PC_RIGHT, AK_LTRT,   0 },
    { PC_LEFT,  AK_LTRT,   KM_SHIFT },

    /* The Alt layer (§10.3). The MCU skips its lower-casing when Alt is
     * down, so these arrive as capitals. Alt+, . Space and B never
     * arrive: the MCU keeps them for the backlights and battery. */
    { 'C', AK_COPY, KM_ALT },
    { 'L', AK_LOCK, KM_ALT },
    { 'K', NOCELL,  KM_ALT | KM_BREAK },
    { 'M', NOCELL,  KM_ALT | KM_MENU },
};

const size_t keymap_picocalc_len = sizeof keymap_picocalc / sizeof keymap_picocalc[0];

/* ---- game keymaps (§10.5) ---------------------------------------------- */

#define LINE_CTRL  NOCELL, KM_CTRL | KM_LINE
#define LINE_SHIFT NOCELL, KM_SHIFT | KM_LINE
#define LINE_REPT  NOCELL, KM_REPT

const keylayout_t keylayout_builtin[] = {
    /* Games that move on CTRL and UPDOWN and fire on REPT, as Galaxians'
     * title page asks. Chosen in the menu. Directions on the arrows, fire
     * on ']' at the far side, one hand each, as the Atom had them.
     * Played on the device, fire on Up under the same thumb was worse,
     * and a Shift cannot fire: while one is down the MCU sends nothing
     * for Left or Right, not even their releases (hardware-notes §6.3). */
    {
        .name = "GAMES",
        .n = 3,
        .bind = {
            { PC_LEFT,  AK_UPDOWN, 0 },
            { PC_RIGHT, LINE_CTRL },
            { ']',      LINE_REPT },
        },
    },
};

const size_t keylayout_builtin_len = sizeof keylayout_builtin / sizeof keylayout_builtin[0];

/* strcasecmp is POSIX, not C11. */
static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    }
    return *a == *b;
}

typedef struct { const char *name; uint8_t code; } key_name_t;

static const key_name_t picocalc_keys[] = {
    { "left", PC_LEFT }, { "right", PC_RIGHT }, { "up", PC_UP }, { "down", PC_DOWN },
    { "space", ' ' }, { "enter", PC_ENTER }, { "backspace", PC_BACKSPACE },
    { "tab", PC_TAB }, { "del", PC_DEL }, { "esc", PC_ESC },
};

bool keymap_picocalc_key_named(const char *name, uint8_t *code) {
    /* A printable character names the key it is on, shifted or not. */
    if (name[0] > ' ' && name[0] < 0x7F && name[1] == 0) {
        *code = keymap_picocalc_canonical((uint8_t)name[0]);
        return true;
    }
    for (size_t i = 0; i < sizeof picocalc_keys / sizeof picocalc_keys[0]; i++) {
        if (same_name(name, picocalc_keys[i].name)) {
            *code = picocalc_keys[i].code;
            return true;
        }
    }
    return false;
}

typedef struct { const char *name; uint8_t row, col, flags; } atom_target_t;

/* Every Atom key except BREAK, by the name on its keycap. BREAK is the
 * reset line, and not a game's to have. */
static const atom_target_t atom_targets[] = {
    { "SPACE", AK_SPACE, 0 },     { "RETURN", AK_RETURN, 0 },
    { "UPDOWN", AK_UPDOWN, 0 },   { "LEFTRIGHT", AK_LTRT, 0 },
    { "COPY", AK_COPY, 0 },       { "LOCK", AK_LOCK, 0 },
    { "DELETE", AK_DEL, 0 },      { "ESCAPE", AK_ESC, 0 },
    { "-", AK_MINUS, 0 },  { ",", AK_COMMA, 0 },    { ";", AK_SEMI, 0 },
    { ":", AK_COLON, 0 },  { "@", AK_AT, 0 },       { "/", AK_SLASH, 0 },
    { ".", AK_STOP, 0 },   { "^", AK_CARET, 0 },    { "[", AK_LBRACKET, 0 },
    { "\\", AK_BSLASH, 0 }, { "]", AK_RBRACKET, 0 },
    { "0", AK_0, 0 }, { "1", AK_1, 0 }, { "2", AK_2, 0 }, { "3", AK_3, 0 },
    { "4", AK_4, 0 }, { "5", AK_5, 0 }, { "6", AK_6, 0 }, { "7", AK_7, 0 },
    { "8", AK_8, 0 }, { "9", AK_9, 0 },
    { "A", AK_A, 0 }, { "B", AK_B, 0 }, { "C", AK_C, 0 }, { "D", AK_D, 0 },
    { "E", AK_E, 0 }, { "F", AK_F, 0 }, { "G", AK_G, 0 }, { "H", AK_H, 0 },
    { "I", AK_I, 0 }, { "J", AK_J, 0 }, { "K", AK_K, 0 }, { "L", AK_L, 0 },
    { "M", AK_M, 0 }, { "N", AK_N, 0 }, { "O", AK_O, 0 }, { "P", AK_P, 0 },
    { "Q", AK_Q, 0 }, { "R", AK_R, 0 }, { "S", AK_S, 0 }, { "T", AK_T, 0 },
    { "U", AK_U, 0 }, { "V", AK_V, 0 }, { "W", AK_W, 0 }, { "X", AK_X, 0 },
    { "Y", AK_Y, 0 }, { "Z", AK_Z, 0 },
    { "CTRL", LINE_CTRL }, { "SHIFT", LINE_SHIFT }, { "REPT", LINE_REPT },
};

bool keymap_atom_target_named(const char *name, keymap_t *out) {
    for (size_t i = 0; i < sizeof atom_targets / sizeof atom_targets[0]; i++) {
        const atom_target_t *t = &atom_targets[i];
        if (same_name(name, t->name)) {
            *out = (keymap_t){ 0, t->row, t->col, t->flags };
            return true;
        }
    }
    return false;
}

uint8_t keymap_picocalc_canonical(uint8_t code) {
    if (code >= 'A' && code <= 'Z') return (uint8_t)(code + ('a' - 'A'));

    /* Each key's shifted alternate back to its base (keyboard.ino). */
    switch (code) {
    case '!': return '1';  case '@': return '2';  case '#': return '3';
    case '$': return '4';  case '%': return '5';  case '^': return '6';
    case '&': return '7';  case '*': return '8';  case '(': return '9';
    case ')': return '0';  case '_': return '-';  case '+': return '=';
    case '|': return '\\'; case '?': return '/';  case ':': return ';';
    case '"': return '\''; case '<': return ',';  case '>': return '.';
    case '{': return '[';  case '}': return ']';  case '~': return '`';
    case PC_END:       return PC_DEL;
    case PC_HOME:      return PC_TAB;
    case PC_BREAK:     return PC_ESC;
    case PC_INSERT:    return PC_ENTER;
    case PC_PAGE_UP:   return PC_UP;
    case PC_PAGE_DOWN: return PC_DOWN;
    default:           return code;
    }
}
