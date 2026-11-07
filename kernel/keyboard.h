#ifndef APTOS_KEYBOARD_H
#define APTOS_KEYBOARD_H

#include "types.h"

#define KEY_UP      0x101
#define KEY_DOWN    0x102
#define KEY_LEFT    0x103
#define KEY_RIGHT   0x104
#define KEY_HOME    0x105
#define KEY_END     0x106
#define KEY_DELETE  0x107
#define KEY_PGUP    0x108
#define KEY_PGDN    0x109
#define KEY_F1      0x111

#define KMOD_SHIFT  1
#define KMOD_CTRL   2
#define KMOD_ALT    4
#define KMOD_CAPS   8

void keyboard_init(void);
u32  keyboard_mods(void);
int  keyboard_has_key(void);
int  keyboard_getkey(void);

#endif
