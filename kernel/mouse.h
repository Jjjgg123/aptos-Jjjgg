#ifndef APTOS_MOUSE_H
#define MOUSE_H

#include "types.h"

#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4

void mouse_init(void);
void mouse_set_bounds(int width, int height);
void mouse_set_position(int x, int y);
void mouse_get(int *x, int *y, int *buttons);
int  mouse_x(void);
int  mouse_y(void);
int  mouse_buttons(void);
int  mouse_ready(void);

#define MOUSE_EV_DOWN 1
#define MOUSE_EV_UP   2
int  mouse_next_event(int *x, int *y, int *buttons);

#endif
