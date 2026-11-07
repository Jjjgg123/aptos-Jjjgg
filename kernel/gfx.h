#ifndef APTOS_GFX_H
#define APTOS_GFX_H

#include "types.h"

#define RGB(r, g, b) ((u32)((((u32)(r)) << 16) | (((u32)(g)) << 8) | (u32)(b)))
#define COLOR_BLACK   0x000000
#define COLOR_WHITE   0xFFFFFF
#define COLOR_RED     0xE04A4A
#define COLOR_GREEN   0x4AE04A
#define COLOR_BLUE    0x4A7CE0
#define COLOR_YELLOW  0xE0D24A
#define COLOR_GRAY    0x808080
#define COLOR_DGRAY   0x303030
#define COLOR_LGRAY   0xC0C0C0

#define FONT_W 8
#define FONT_H 8

typedef struct {
    int x0, y0, x1, y1;
} rect_t;

void  gfx_init(void);
int   gfx_available(void);
int   gfx_width(void);
int   gfx_height(void);
u32   gfx_bpp(void);

void  gfx_clear(u32 color);
void  gfx_set_clip(int x0, int y0, int x1, int y1);
void  gfx_set_clip_rect(rect_t r);
rect_t gfx_get_clip(void);
void  gfx_reset_clip(void);
rect_t gfx_screen_rect(void);

void  gfx_pixel(int x, int y, u32 color);
u32   gfx_get_pixel(int x, int y);
void  gfx_hline(int x, int y, int w, u32 color);
void  gfx_vline(int x, int y, int h, u32 color);
void  gfx_fill_rect(int x, int y, int w, int h, u32 color);
void  gfx_rect(int x, int y, int w, int h, u32 color);
void  gfx_line(int x0, int y0, int x1, int y1, u32 color);
void  gfx_fill_circle(int cx, int cy, int r, u32 color);
void  gfx_gradient(int x, int y, int w, int h, u32 top, u32 bottom);

void  gfx_blit(int dx, int dy, int w, int h, const u32 *src);
void  gfx_blit_alpha(int dx, int dy, int w, int h, const u32 *src, int alpha);

int   gfx_char(int x, int y, char ch, u32 color, int scale);
int   gfx_text(int x, int y, const char *s, u32 color, int scale);
int   gfx_text_w(const char *s, int scale);
void  gfx_font16_set(const void *data);
/* 返回已注册的 F16 字体表数据指针（NULL=未加载），供 uapp 等共用 */
const void *gfx_font16(void);
/* 按 Unicode 查询 16x16 字形（34 字节项中的位图，32 字节），未命中返回 NULL */
const void *gfx_font16_glyph(unsigned short code);

u32   gfx_blend(u32 fg, u32 bg, int alpha);

void  gfx_present(void);
void  gfx_present_rect(int x, int y, int w, int h);
u32  *gfx_backbuffer(void);

/* 渲染目标：把后续绘制写进窗口自己的显示空间（w*h，行跨度 w），
   应用继续用屏幕绝对坐标，内部按 (ox,oy) 换算。 */
void  gfx_target_begin(u32 *px, int w, int h, int ox, int oy);
void  gfx_target_end(void);

#endif
