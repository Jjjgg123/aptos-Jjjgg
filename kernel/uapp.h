#ifndef APTOS_UAPP_H
#define APTOS_UAPP_H

#include "types.h"

int  uapp_open(const char *title, int w, int h);
void uapp_clear(u32 color);
void uapp_rect(int x, int y, int w, int h, u32 color);
void uapp_line(int x0, int y0, int x1, int y1, u32 color);
void uapp_frame(int x, int y, int w, int h, u32 color);
void uapp_text(int x, int y, const char *s, u32 color, int scale);
void uapp_blit(int dx, int dy, int w, int h, const u32 *src);
void uapp_flush(void);
void uapp_close(void);
void uapp_mouse(int *x, int *y, int *buttons, int *inside);
int  uapp_width(void);
int  uapp_height(void);
int  uapp_open_window(void);
int  uapp_haskey(void);
int  uapp_getkey(void);
int  uapp_focused(void);

#endif
