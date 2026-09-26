/* allegro.h — just enough of Allegro 4 for Atomulator's CPU, 8255, VIA
 * and 8271 to compile headless, for the trace-diff harness (design.md
 * §15.1). Atomulator reaches Allegro in those files only through the key
 * array and the joystick; the driver owns both. */
#ifndef PICO_ATOM_TRACE_ALLEGRO_H
#define PICO_ATOM_TRACE_ALLEGRO_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    KEY_NONE, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S,
    KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_ESC, KEY_MINUS, KEY_EQUALS, KEY_BACKSPACE, KEY_TAB, KEY_OPENBRACE,
    KEY_CLOSEBRACE, KEY_ENTER, KEY_SEMICOLON, KEY_QUOTE, KEY_BACKSLASH,
    KEY_COMMA, KEY_STOP, KEY_SLASH, KEY_SPACE, KEY_END, KEY_UP, KEY_RIGHT,
    KEY_CAPSLOCK, KEY_LSHIFT, KEY_RSHIFT, KEY_LCONTROL, KEY_RCONTROL,
    KEY_ALT, KEY_ALTGR, KEY_DEL, KEY_COLON, KEY_F12,
    KEY_MAX = 128
};

extern volatile char key[KEY_MAX];

typedef struct { int b; } JOYSTICK_BUTTON_INFO;
typedef struct { JOYSTICK_BUTTON_INFO button[4]; } JOYSTICK_INFO;
extern JOYSTICK_INFO joy[1];
extern int joy_left, joy_right, joy_up, joy_down;

#endif
