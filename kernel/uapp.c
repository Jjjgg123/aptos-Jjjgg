#include "uapp.h"
#include "font8x8.h"
#include "gfx.h"
#include "gui.h"
#include "heap.h"
#include "klog.h"
#include "mouse.h"
#include "scheduler.h"
#include "util.h"

#define FONT_W 8
#define FONT_H 8
#define UAPP_MAX_W 1024
#define UAPP_MAX_H 768
#define UKEY_BUF   128

/* 同时活着的用户态显示上下文上限。
   每个上下文 = 一块自己的显示空间(canvas) + 一个窗口，互相不干扰。 */
#define UAPP_MAX   6

typedef struct {
    int       used;
    int       pid;          /* 归属任务，syscall 时靠它找回自己那块空间 */
    u32      *canvas;
    int       cw, ch;
    window_t *uwin;
    int       have_window;
    char      title[32];
    app_t     app;          /* 每个实例一份 app_t，title 指向本实例的 title */
    volatile int key[UKEY_BUF];
    volatile u32 key_head, key_tail;
} uapp_ctx_t;

static uapp_ctx_t ctxs[UAPP_MAX];

static uapp_ctx_t *uapp_from_window(window_t *w)
{
    return (w && w->state) ? (uapp_ctx_t *)w->state : NULL;
}

/* 取一个上下文槽。堆是 bump 分配器、没有 kfree，所以已经分配过的
   canvas 一律留着复用（进程退出时只把 pid 置 -1，不丢缓冲）。 */
static uapp_ctx_t *uapp_alloc(int pid)
{
    for (int i = 0; i < UAPP_MAX; i++) {
        uapp_ctx_t *c = &ctxs[i];
        if (!c->used || c->pid < 0) {
            u32 *keep_canvas = c->canvas;
            int keep_cw = c->cw, keep_ch = c->ch;
            memset(c, 0, sizeof(*c));
            c->canvas = keep_canvas;
            c->cw = keep_cw;
            c->ch = keep_ch;
            c->used = 1;
            c->pid = pid;
            return c;
        }
    }
    return NULL;
}

/* 当前系统调用属于哪个任务，就用哪个上下文 */
static uapp_ctx_t *uapp_cur(void)
{
    int pid = scheduler_cur_id();

    if (pid < 0)
        return NULL;
    for (int i = 0; i < UAPP_MAX; i++)
        if (ctxs[i].used && ctxs[i].pid == pid)
            return &ctxs[i];
    return uapp_alloc(pid);
}

static void uapp_touch_ctx(uapp_ctx_t *c)
{
    if (c && c->uwin && c->have_window)
        gui_dirty_window(c->uwin);
    else
        gui_dirty();
}

/* 应用自己的显示空间变化了：只标脏自己那块窗口，不惊动整个桌面 */
static void uapp_draw_cb(window_t *w)
{
    uapp_ctx_t *c = uapp_from_window(w);

    if (!c || !c->canvas || !c->used)
        return;
    /* 外层（draw_window）已经把裁剪区设成"内容区 ∩ 脏区"，这里不要再
       重置裁剪 —— 那会把脏区外的像素也改写进 back buffer，造成分叉。 */
    gfx_blit(w->cx, w->cy, c->cw, c->ch, c->canvas);
}

static void uapp_key_cb(window_t *w, int k, u32 mods)
{
    uapp_ctx_t *c = uapp_from_window(w);
    u32 next;

    (void)mods;
    if (!c)
        return;
    next = (c->key_head + 1) % UKEY_BUF;
    if (next == c->key_tail)
        return;
    c->key[c->key_head] = k;
    c->key_head = next;
}

int uapp_haskey(void)
{
    uapp_ctx_t *c = uapp_cur();
    return c && c->key_head != c->key_tail;
}

int uapp_getkey(void)
{
    uapp_ctx_t *c = uapp_cur();
    int k;

    if (!c || c->key_head == c->key_tail)
        return -1;
    k = c->key[c->key_tail];
    c->key_tail = (c->key_tail + 1) % UKEY_BUF;
    return k;
}

int uapp_focused(void)
{
    uapp_ctx_t *c = uapp_cur();
    return c && c->uwin && gui_focused() == c->uwin;
}

int uapp_width(void)
{
    uapp_ctx_t *c = uapp_cur();
    return c ? c->cw : 0;
}

int uapp_height(void)
{
    uapp_ctx_t *c = uapp_cur();
    return c ? c->ch : 0;
}

int uapp_open_window(void)
{
    uapp_ctx_t *c = uapp_cur();
    return c ? c->have_window : 0;
}

int uapp_open(const char *title, int w, int h)
{
    uapp_ctx_t *c = uapp_cur();

    if (!c) {
        kprintf("[uapp] no context for pid %d\n", scheduler_cur_id());
        return -1;
    }

    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > UAPP_MAX_W) w = UAPP_MAX_W;
    if (h > UAPP_MAX_H) h = UAPP_MAX_H;

    if (!c->canvas || w > c->cw || h > c->ch) {
        c->canvas = (u32 *)kmalloc((size_t)w * (size_t)h * 4);
        if (!c->canvas) {
            kprintf("[uapp] out of memory for a %dx%d canvas\n", w, h);
            c->cw = c->ch = 0;
            return -1;
        }
        c->cw = w;
        c->ch = h;
    }

    memset(c->canvas, 0, (size_t)c->cw * (size_t)c->ch * 4);

    strncpy(c->title, title ? title : "application", sizeof(c->title) - 1);
    c->title[sizeof(c->title) - 1] = 0;

    c->app.title  = c->title;
    c->app.width  = w;
    c->app.height = h;
    c->app.draw   = uapp_draw_cb;
    c->app.key    = uapp_key_cb;
    c->app.mouse  = NULL;
    c->app.tick   = NULL;
    c->app.close  = NULL;

    if (!c->uwin) {
        c->uwin = gui_open(&c->app);
        if (!c->uwin) {
            kprintf("[uapp] could not open a window\n");
            return -1;
        }
        c->uwin->state = c;
    } else {
        c->uwin->state = c;
        gui_resize(c->uwin, w, h);
    }
    c->have_window = 1;

    gui_focus(c->uwin);
    uapp_clear(RGB(28, 34, 46));
    uapp_touch_ctx(c);
    return 0;
}

void uapp_clear(u32 color)
{
    uapp_ctx_t *c = uapp_cur();
    int i, n;

    if (!c || !c->canvas)
        return;
    uapp_touch_ctx(c);
    n = c->cw * c->ch;
    for (i = 0; i < n; i++)
        c->canvas[i] = color;
}

void uapp_rect(int x, int y, int w, int h, u32 color)
{
    uapp_ctx_t *c = uapp_cur();
    u32 *base;

    if (!c || !c->canvas)
        return;
    uapp_touch_ctx(c);

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->cw) w = c->cw - x;
    if (y + h > c->ch) h = c->ch - y;
    if (w <= 0 || h <= 0)
        return;

    base = c->canvas + (size_t)y * c->cw + x;
    for (int row = 0; row < h; row++) {
        u32 *d = base + (size_t)row * c->cw;
        for (int col = 0; col < w; col++)
            d[col] = color;
    }
}

void uapp_line(int x0, int y0, int x1, int y1, u32 color)
{
    uapp_ctx_t *c = uapp_cur();
    int dx, dy, sx, sy, err;

    if (!c || !c->canvas)
        return;
    uapp_touch_ctx(c);

    dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;
    for (;;) {
        if (x0 >= 0 && x0 < c->cw && y0 >= 0 && y0 < c->ch)
            c->canvas[(size_t)y0 * c->cw + x0] = color;
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = err << 1;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
    }
}

void uapp_frame(int x, int y, int w, int h, u32 color)
{
    uapp_rect(x, y, w, 1, color);
    uapp_rect(x, y + h - 1, w, 1, color);
    uapp_rect(x, y, 1, h, color);
    uapp_rect(x + w - 1, y, 1, h, color);
}

static int utf8_decode_u(const char *s, u32 *out)
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

static void uapp_draw_ascii(int x, int y, unsigned char ch, u32 color, int scale)
{
    const unsigned char *glyph = font8x8_basic[ch];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < FONT_W; col++)
            if (bits & (1u << col))
                uapp_rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}

void uapp_text(int x, int y, const char *s, u32 color, int scale)
{
    uapp_ctx_t *c = uapp_cur();
    int gx;
    const char *p;

    if (!c || !c->canvas || !s)
        return;
    uapp_touch_ctx(c);
    if (scale < 1)
        scale = 1;

    gx = x;
    p = s;
    while (*p) {
        u32 cp;
        int n = utf8_decode_u(p, &cp);
        if (cp < 0x80) {
            uapp_draw_ascii(gx, y, (unsigned char)cp, color, scale);
            gx += FONT_W * scale;
        } else if (cp > 0xFFFF) {
            uapp_draw_ascii(gx, y, '?', color, scale);
            gx += FONT_W * scale;
        } else {
            const u8 *g = (const u8 *)gfx_font16_glyph((unsigned short)cp);
            if (!g) {
                uapp_draw_ascii(gx, y, '?', color, scale);
                gx += FONT_W * scale;
            } else {
                /* 16x16 位图按 8 高降采样（每两行 OR 合并），保持 16 宽 */
                for (int r = 0; r < 8; r++) {
                    u16 bits = (u16)(((u16)g[(r * 2) * 2] << 8) | g[(r * 2) * 2 + 1]);
                    bits |= (u16)(((u16)g[(r * 2 + 1) * 2] << 8) | g[(r * 2 + 1) * 2 + 1]);
                    for (int cc = 0; cc < 16; cc++)
                        if (bits & (u16)(1u << (15 - cc)))
                            uapp_rect(gx + cc * scale, y + r * scale, scale, scale, color);
                }
                gx += 16 * scale;
            }
        }
        p += n;
    }
}

void uapp_blit(int dx, int dy, int w, int h, const u32 *src)
{
    uapp_ctx_t *c = uapp_cur();

    if (!c || !c->canvas || !src || w <= 0 || h <= 0)
        return;
    uapp_touch_ctx(c);

    /* 按行裁剪后一次 memcpy：460x336 这种整块上传不再有 15 万次逐像素循环 */
    for (int j = 0; j < h; j++) {
        int yy = dy + j;
        int x0, x1, n;
        if (yy < 0 || yy >= c->ch)
            continue;
        x0 = dx < 0 ? 0 : dx;
        x1 = dx + w;
        if (x1 > c->cw)
            x1 = c->cw;
        if (x1 <= x0)
            continue;
        n = x1 - x0;
        memcpy(&c->canvas[(size_t)yy * c->cw + x0],
               &src[(size_t)j * w + (x0 - dx)],
               (size_t)n * sizeof(u32));
    }
}

void uapp_flush(void)
{
    uapp_ctx_t *c = uapp_cur();

    if (!c || !c->uwin || !c->canvas)
        return;
    /* 只标脏、不再直接 present：所有提交统一走脏区契约，
       避免"应用自己提交一遍、系统再合成一遍"造成 back 与屏幕分叉。 */
    gui_dirty_window(c->uwin);
    gui_cursor_refresh();
}

void uapp_mouse(int *x, int *y, int *buttons, int *inside)
{
    uapp_ctx_t *c = uapp_cur();
    int mx, my, mb;

    mouse_get(&mx, &my, &mb);
    if (c && c->uwin) {
        mx -= c->uwin->cx;
        my -= c->uwin->cy;
    }
    if (x)
        *x = mx;
    if (y)
        *y = my;
    if (buttons)
        *buttons = mb;
    if (inside)
        *inside = (c && c->uwin && mx >= 0 && my >= 0 &&
                   mx < c->cw && my < c->ch) ? 1 : 0;
}

void uapp_close(void)
{
    uapp_ctx_t *c = uapp_cur();

    if (!c)
        return;
    if (c->uwin) {
        gui_close(c->uwin);
        c->uwin = NULL;
    }
    /* canvas / cw / ch 全部保留：堆没有回收机制，这些缓冲留着给
       同一个槽下次复用，避免每次开程序都再吃掉几 MB。 */
    c->have_window = 0;
    c->pid = -1;
}
