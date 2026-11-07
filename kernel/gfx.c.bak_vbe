#include "gfx.h"
#include "bootinfo.h"
#include "font8x8.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "util.h"

static u32  *back;
static u8   *lfb;
static int   width, height;
static int   pitch;
static int   bpp;
static rect_t clip;

/* ---- 渲染目标 ---------------------------------------------------------
   每个窗口有自己的显示空间（w->buf）。系统在渲染该窗口时把"当前目标"
   切到这块缓冲，绘制原语照旧用屏幕绝对坐标，内部减去窗口原点 (ox,oy)。
   这样应用只管画自己的空间，系统负责合成到屏幕。
   ---------------------------------------------------------------------- */
static u32  *cur_px;
static int   cur_pitch;
static int   cur_w, cur_h;
static int   cur_ox, cur_oy;

void gfx_target_begin(u32 *px, int w, int h, int ox, int oy)
{
    if (!px || w <= 0 || h <= 0)
        return;
    cur_px = px;
    cur_pitch = w;
    cur_w = w;
    cur_h = h;
    cur_ox = ox;
    cur_oy = oy;
}

void gfx_target_end(void)
{
    cur_px = back;
    cur_pitch = width;
    cur_w = width;
    cur_h = height;
    cur_ox = 0;
    cur_oy = 0;
}

/* 16x16 CJK 点阵字体（F16 表，由 gfx_font16_set 注册，NULL 表示未加载） */
static const u8 *font16_data;
static u32 font16_count;

void gfx_font16_set(const void *data)
{
    const u8 *p = (const u8 *)data;
    if (!p || p[0] != 'F' || p[1] != '1' || p[2] != '6' || p[3] != 1) {
        font16_data = NULL;
        font16_count = 0;
        return;
    }
    font16_count = (u32)p[4] | ((u32)p[5] << 8) | ((u32)p[6] << 16) | ((u32)p[7] << 24);
    font16_data = p + 8;
}

static const u8 *font16_lookup(u16 code)
{
    u32 lo = 0, hi = font16_count;
    const u8 *base = font16_data;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const u8 *e = base + mid * 34;
        u16 c = (u16)(e[0] | (e[1] << 8));
        if (c == code)
            return e + 2;
        if (c < code)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

const void *gfx_font16(void)
{
    return font16_data ? (const void *)(font16_data - 8) : NULL;
}

const void *gfx_font16_glyph(unsigned short code)
{
    if (!font16_data)
        return NULL;
    return font16_lookup(code);
}

/* 解码 s 处的第一个 UTF-8 序列；返回消耗字节数，*out 为码点 */
static int utf8_decode(const char *s, u32 *out)
{
    u8 c = (u8)s[0];
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    if (c >= 0xC2 && c < 0xE0 && ((u8)s[1] & 0xC0) == 0x80) {
        *out = ((u32)(c & 0x1F) << 6) | ((u8)s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c < 0xF0 && ((u8)s[1] & 0xC0) == 0x80 &&
        ((u8)s[2] & 0xC0) == 0x80) {
        *out = ((u32)(c & 0x0F) << 12) | (((u8)s[1] & 0x3F) << 6) | ((u8)s[2] & 0x3F);
        return 3;
    }
    *out = '?';
    return 1;
}

/* 16x16 位图按 8 高降采样绘制（每两行 OR 合并），保持 16 宽（两个 ASCII 字符宽） */
static void gfx_char16(int x, int y, const u8 *g, u32 color, int scale)
{
    for (int r = 0; r < 8; r++) {
        u16 bits = (u16)(((u16)g[(r * 2) * 2] << 8) | g[(r * 2) * 2 + 1]);
        bits |= (u16)(((u16)g[(r * 2 + 1) * 2] << 8) | g[(r * 2 + 1) * 2 + 1]);
        for (int c = 0; c < 16; c++) {
            if (bits & (u16)(1u << (15 - c))) {
                if (scale == 1)
                    gfx_pixel(x + c, y + r, color);
                else
                    gfx_fill_rect(x + c * scale, y + r * scale, scale, scale, color);
            }
        }
    }
}

int gfx_width(void)  { return width; }
int gfx_height(void) { return height; }
u32 gfx_bpp(void)    { return (u32)bpp; }
u32 *gfx_backbuffer(void) { return back; }

rect_t gfx_screen_rect(void)
{
    rect_t r;
    r.x0 = 0;
    r.y0 = 0;
    r.x1 = width - 1;
    r.y1 = height - 1;
    return r;
}

static void set_vga332_palette(void)
{
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        u8 r = (u8)((i >> 5) & 0x07);
        u8 g = (u8)((i >> 2) & 0x07);
        u8 b = (u8)(i & 0x03);
        outb(0x3C9, (u8)(r * 255 / 7));
        outb(0x3C9, (u8)(g * 255 / 7));
        outb(0x3C9, (u8)(b * 255 / 3));
    }
}

int gfx_available(void)
{
    return BOOTINFO->fb_addr != 0 && BOOTINFO->fb_width > 0 && BOOTINFO->fb_height > 0;
}

void gfx_init(void)
{
    lfb    = (u8 *)0xF0000000u;
    width  = BOOTINFO->fb_width;
    height = BOOTINFO->fb_height;
    pitch  = BOOTINFO->fb_pitch;
    bpp    = BOOTINFO->fb_bpp;

    if (width <= 0 || height <= 0) {
        width = 640;
        height = 480;
        pitch = 640 * 4;
        bpp = 32;
    }
    if (pitch < width * (bpp / 8))
        pitch = width * (bpp / 8);

    back = (u32 *)kmalloc((size_t)width * (size_t)height * 4);
    if (!back)
        for (;;)
            hlt();
    memset(back, 0, (size_t)width * (size_t)height * 4);

    if (bpp == 8)
        set_vga332_palette();

    gfx_reset_clip();
    gfx_target_end();   /* 默认目标 = 屏幕 backbuffer */
    kprintf("[gfx] %dx%d %dbpp, framebuffer @ 0x%x, pitch %d\n",
            width, height, bpp, BOOTINFO->fb_addr, pitch);
    kprintf("[gfx] backbuffer @ 0x%x (%d bytes)\n", (u32)back,
            width * height * 4);
}

void gfx_set_clip(int x0, int y0, int x1, int y1)
{
    clip.x0 = x0 < 0 ? 0 : x0;
    clip.y0 = y0 < 0 ? 0 : y0;
    clip.x1 = x1 > width - 1 ? width - 1 : x1;
    clip.y1 = y1 > height - 1 ? height - 1 : y1;
}

void gfx_set_clip_rect(rect_t r) { gfx_set_clip(r.x0, r.y0, r.x1, r.y1); }
rect_t gfx_get_clip(void) { return clip; }
void gfx_reset_clip(void) { gfx_set_clip(0, 0, width - 1, height - 1); }

void gfx_pixel(int x, int y, u32 color)
{
    if (x < clip.x0 || x > clip.x1 || y < clip.y0 || y > clip.y1)
        return;
    {
        int tx = x - cur_ox, ty = y - cur_oy;
        if (tx < 0 || tx >= cur_w || ty < 0 || ty >= cur_h)
            return;
        cur_px[ty * cur_pitch + tx] = color;
    }
}

u32 gfx_get_pixel(int x, int y)
{
    int tx = x - cur_ox, ty = y - cur_oy;
    if (tx < 0 || tx >= cur_w || ty < 0 || ty >= cur_h)
        return 0;
    return cur_px[ty * cur_pitch + tx];
}

void gfx_clear(u32 color)
{
    u32 *p = cur_px;
    u32 n = (u32)cur_w * (u32)cur_h;
    while (n--)
        *p++ = color;
}

void gfx_hline(int x, int y, int w, u32 color)
{
    if (y < clip.y0 || y > clip.y1 || w <= 0)
        return;
    if (x < clip.x0) {
        w -= clip.x0 - x;
        x = clip.x0;
    }
    if (x + w - 1 > clip.x1)
        w = clip.x1 - x + 1;
    if (w <= 0)
        return;
    {
        int tx = x - cur_ox, ty = y - cur_oy;
        if (ty < 0 || ty >= cur_h)
            return;
        if (tx < 0) {
            w += tx;
            tx = 0;
        }
        if (tx + w > cur_w)
            w = cur_w - tx;
        if (w <= 0)
            return;
        {
            u32 *p = cur_px + ty * cur_pitch + tx;
            for (int i = 0; i < w; i++)
                p[i] = color;
        }
    }
}

void gfx_vline(int x, int y, int h, u32 color)
{
    if (x < clip.x0 || x > clip.x1 || h <= 0)
        return;
    if (y < clip.y0) {
        h -= clip.y0 - y;
        y = clip.y0;
    }
    if (y + h - 1 > clip.y1)
        h = clip.y1 - y + 1;
    if (h <= 0)
        return;
    {
        int tx = x - cur_ox, ty = y - cur_oy;
        if (tx < 0 || tx >= cur_w)
            return;
        if (ty < 0) {
            h += ty;
            ty = 0;
        }
        if (ty + h > cur_h)
            h = cur_h - ty;
        if (h <= 0)
            return;
        {
            u32 *p = cur_px + ty * cur_pitch + tx;
            for (int i = 0; i < h; i++, p += cur_pitch)
                *p = color;
        }
    }
}

void gfx_fill_rect(int x, int y, int w, int h, u32 color)
{
    int y0 = y, y1 = y + h - 1;
    int x0 = x, x1 = x + w - 1;
    int tx, n;

    if (w <= 0 || h <= 0)
        return;
    if (y0 < clip.y0) y0 = clip.y0;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x0 < clip.x0) x0 = clip.x0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (x0 > x1 || y0 > y1)
        return;

    tx = x0 - cur_ox;
    if (tx < 0) {
        x0 -= tx;
        tx = 0;
        if (x0 > x1)
            return;
    }
    if (tx + (x1 - x0 + 1) > cur_w)
        x1 = x0 + (cur_w - tx) - 1;
    if (x1 < x0)
        return;
    n = x1 - x0 + 1;

    for (int yy = y0; yy <= y1; yy++) {
        int ty = yy - cur_oy;
        u32 *d;
        if (ty < 0 || ty >= cur_h)
            continue;
        d = cur_px + (size_t)ty * cur_pitch + tx;
        for (int i = 0; i < n; i++)
            d[i] = color;
    }
}

void gfx_rect(int x, int y, int w, int h, u32 color)
{
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_line(int x0, int y0, int x1, int y1, u32 color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_fill_circle(int cx, int cy, int r, u32 color)
{
    for (int y = -r; y <= r; y++) {
        int x = 0;
        while (x * x + y * y <= r * r)
            x++;
        gfx_hline(cx - (x - 1), cy + y, 2 * (x - 1) + 1, color);
    }
}

void gfx_gradient(int x, int y, int w, int h, u32 top, u32 bottom)
{
    if (h <= 0)
        return;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (int i = 0; i < h; i++) {
        int r = tr + (br - tr) * i / (h > 1 ? h - 1 : 1);
        int g = tg + (bg - tg) * i / (h > 1 ? h - 1 : 1);
        int b = tb + (bb - tb) * i / (h > 1 ? h - 1 : 1);
        gfx_hline(x, y + i, w, RGB(r, g, b));
    }
}

/* 整块贴图：原来每像素调一次 gfx_pixel（两次边界判定 + 两次减法 + 函数调用），
   460x336 一块就是 15 万次。现在先把这一行裁到"裁剪区 ∩ 目标区域"，剩下的
   连续段直接 memcpy —— 每行只剩一次指针运算。 */
void gfx_blit(int dx, int dy, int w, int h, const u32 *src)
{
    int j;

    if (!src || w <= 0 || h <= 0)
        return;

    for (j = 0; j < h; j++) {
        int yy = dy + j;
        int x0 = dx, x1 = dx + w - 1;
        int tx, ty, n;
        const u32 *s;
        u32 *d;

        if (yy < clip.y0 || yy > clip.y1)
            continue;
        if (x0 < clip.x0) x0 = clip.x0;
        if (x1 > clip.x1) x1 = clip.x1;
        if (x0 > x1)
            continue;

        ty = yy - cur_oy;
        tx = x0 - cur_ox;
        if (ty < 0 || ty >= cur_h)
            continue;
        if (tx < 0) {                    /* 左边界外：像素源也跟着右移 */
            x0 -= tx;
            tx = 0;
            if (x0 > x1)
                continue;
        }
        if (tx + (x1 - x0 + 1) > cur_w)
            x1 = x0 + (cur_w - tx) - 1;
        if (x1 < x0)
            continue;

        n = x1 - x0 + 1;
        s = src + (size_t)j * w + (x0 - dx);
        d = cur_px + (size_t)ty * cur_pitch + tx;
        memcpy(d, s, (size_t)n * sizeof(u32));
    }
}

u32 gfx_blend(u32 fg, u32 bg, int alpha)
{
    if (alpha <= 0)
        return bg;
    if (alpha >= 255)
        return fg;
    u32 r = ((((fg >> 16) & 0xFF) * (u32)alpha + ((bg >> 16) & 0xFF) * (u32)(255 - alpha)) / 255) & 0xFF;
    u32 g = ((((fg >> 8) & 0xFF) * (u32)alpha + ((bg >> 8) & 0xFF) * (u32)(255 - alpha)) / 255) & 0xFF;
    u32 b = (((fg & 0xFF) * (u32)alpha + (bg & 0xFF) * (u32)(255 - alpha)) / 255) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

void gfx_blit_alpha(int dx, int dy, int w, int h, const u32 *src, int alpha)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int px = dx + i, py = dy + j;
            u32 old;
            if (px < clip.x0 || px > clip.x1 || py < clip.y0 || py > clip.y1)
                continue;
            old = gfx_get_pixel(px, py);
            gfx_pixel(px, py, gfx_blend(src[j * w + i], old, alpha));
        }
    }
}

int gfx_char(int x, int y, char ch, u32 color, int scale)
{
    if (scale < 1)
        scale = 1;
    int c = (u8)ch;
    if (c > 127)
        c = '?';
    const unsigned char *glyph = font8x8_basic[c];

    int gw = FONT_W * scale;
    int gh = FONT_H * scale;

    if (x > clip.x1 || y > clip.y1 || x + gw - 1 < clip.x0 || y + gh - 1 < clip.y0)
        return gw;

    for (int row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        if (!bits)
            continue;
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (1u << col)))
                continue;
            if (scale == 1) {
                gfx_pixel(x + col, y + row, color);
            } else {
                gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
    return gw;
}

int gfx_text(int x, int y, const char *s, u32 color, int scale)
{
    if (scale < 1)
        scale = 1;
    int gx = x;
    const char *p = s;
    while (*p) {
        u32 cp;
        int n = utf8_decode(p, &cp);
        if (cp < 0x80) {
            gfx_char(gx, y, (char)cp, color, scale);
            gx += FONT_W * scale;
        } else if (cp > 0xFFFF || !font16_data) {
            gfx_char(gx, y, '?', color, scale);
            gx += FONT_W * scale;
        } else {
            const u8 *g = font16_lookup((u16)cp);
            if (!g) {
                gfx_char(gx, y, '?', color, scale);
                gx += FONT_W * scale;
            } else {
                gfx_char16(gx, y, g, color, scale);
                gx += 16 * scale;
            }
        }
        p += n;
    }
    return gx - x;
}

int gfx_text_w(const char *s, int scale)
{
    if (scale < 1)
        scale = 1;
    int w = 0;
    const char *p = s;
    while (*p) {
        u32 cp;
        int n = utf8_decode(p, &cp);
        w += (cp >= 0x80 && cp <= 0xFFFF && font16_data) ? 16 * scale : FONT_W * scale;
        p += n;
    }
    return w;
}

void gfx_present(void)
{
    u32 n = (u32)width * (u32)height;

    switch (bpp) {
    case 32:
        if (pitch == width * 4) {
            rep_movsl(lfb, back, n);
        } else {
            for (int y = 0; y < height; y++)
                rep_movsl(lfb + (u32)y * pitch, back + (u32)y * width, (u32)width);
        }
        break;
    case 24:
        for (int y = 0; y < height; y++) {
            u8 *d = lfb + (u32)y * pitch;
            const u32 *s = back + (u32)y * width;
            for (int x = 0; x < width; x++) {
                u32 c = s[x];
                *d++ = (u8)(c & 0xFF);
                *d++ = (u8)((c >> 8) & 0xFF);
                *d++ = (u8)((c >> 16) & 0xFF);
            }
        }
        break;
    case 16:
        for (int y = 0; y < height; y++) {
            u16 *d = (u16 *)(lfb + (u32)y * pitch);
            const u32 *s = back + (u32)y * width;
            for (int x = 0; x < width; x++) {
                u32 c = s[x];
                d[x] = (u16)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) |
                             ((c & 0xF8) >> 3));
            }
        }
        break;
    case 8:
        for (int y = 0; y < height; y++) {
            u8 *d = lfb + (u32)y * pitch;
            const u32 *s = back + (u32)y * width;
            for (int x = 0; x < width; x++) {
                u32 c = s[x];
                d[x] = (u8)((((c >> 16) & 0xE0)) | (((c >> 8) & 0xE0) >> 3) |
                            ((c & 0xC0) >> 6));
            }
        }
        break;
    default:
        break;
    }
}

void gfx_present_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width)  w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int j = 0; j < h; j++) {
        const u32 *s = back + (y + j) * width + x;
        u8 *d = lfb + (u32)(y + j) * pitch + (u32)x * (bpp / 8);
        switch (bpp) {
        case 32:
            rep_movsl(d, s, (u32)w);
            break;
        case 24:
            for (int i = 0; i < w; i++) {
                u32 c = s[i];
                d[i * 3 + 0] = (u8)(c & 0xFF);
                d[i * 3 + 1] = (u8)((c >> 8) & 0xFF);
                d[i * 3 + 2] = (u8)((c >> 16) & 0xFF);
            }
            break;
        case 16:
            for (int i = 0; i < w; i++) {
                u32 c = s[i];
                ((u16 *)d)[i] = (u16)((((c >> 16) & 0xF8) << 8) |
                                      (((c >> 8) & 0xFC) << 3) | ((c & 0xF8) >> 3));
            }
            break;
        case 8:
            for (int i = 0; i < w; i++) {
                u32 c = s[i];
                d[i] = (u8)(((c >> 16) & 0xE0) | (((c >> 8) & 0xE0) >> 3) |
                            ((c & 0xC0) >> 6));
            }
            break;
        default:
            break;
        }
    }
}
