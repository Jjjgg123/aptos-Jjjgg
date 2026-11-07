#include "gui.h"
#include "heap.h"
#include "keyboard.h"
#include "util.h"

#define PT_W 420
#define PT_H 300
#define PT_TOOLBAR 26
#define CANVAS_W PT_W
#define CANVAS_H (PT_H - PT_TOOLBAR)

static u32 *canvas;
static int brush = 3;
static int color_index = 1;
static int drawing;
static int last_x = -1, last_y = -1;
static window_t *win;

static const u32 palette[] = {
    RGB(240, 240, 240), RGB(20, 20, 24),   RGB(220, 70, 70),  RGB(240, 150, 60),
    RGB(245, 220, 90),  RGB(120, 210, 110), RGB(80, 200, 210), RGB(90, 150, 245),
    RGB(150, 110, 235), RGB(230, 110, 190), RGB(150, 110, 70), RGB(120, 130, 145),
};
#define PALETTE_N ((int)ARRAY_LEN(palette))

static void canvas_clear(void)
{
    if (!canvas)
        return;
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++)
        canvas[i] = RGB(250, 250, 252);
}

static void canvas_dot(int cx, int cy, u32 col)
{
    if (!canvas)
        return;
    for (int y = -brush; y <= brush; y++) {
        for (int x = -brush; x <= brush; x++) {
            if (x * x + y * y > brush * brush)
                continue;
            int px = cx + x, py = cy + y;
            if (px < 0 || py < 0 || px >= CANVAS_W || py >= CANVAS_H)
                continue;
            canvas[py * CANVAS_W + px] = col;
        }
    }
}

static void canvas_line(int x0, int y0, int x1, int y1, u32 col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        canvas_dot(x0, y0, col);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void pt_draw(window_t *w)
{
    gfx_fill_rect(w->cx, w->cy, w->cw, PT_TOOLBAR, RGB(42, 47, 58));

    int x = w->cx + 4;
    for (int i = 0; i < PALETTE_N; i++) {
        gfx_fill_rect(x, w->cy + 5, 16, 16, palette[i]);
        if (i == color_index)
            gfx_rect(x - 1, w->cy + 4, 18, 18, RGB(250, 250, 250));
        else
            gfx_rect(x, w->cy + 5, 16, 16, RGB(70, 76, 90));
        x += 20;
    }

    ui_button(x + 4, w->cy + 5, 42, 16, "Clear", 0);
    x += 52;
    ui_button(x + 4, w->cy + 5, 20, 16, "-", 0);
    ui_button(x + 28, w->cy + 5, 20, 16, "+", 0);
    char b[16];
    snprintf(b, sizeof(b), " %d", brush);
    gfx_text(x + 52, w->cy + 9, b, RGB(220, 226, 236), 1);

    if (canvas)
        gfx_blit(w->cx, w->cy + PT_TOOLBAR, CANVAS_W, CANVAS_H, canvas);
}

static void pt_mouse(window_t *w, int x, int y, int buttons, int event)
{
    (void)w;

    if (event == GUI_MOUSE_UP) {
        drawing = 0;
        last_x = last_y = -1;
        return;
    }

    if (y < PT_TOOLBAR) {
        if (event != GUI_MOUSE_DOWN)
            return;
        if (x >= 4 && x < 4 + PALETTE_N * 20) {
            color_index = (x - 4) / 20;
            if (color_index >= PALETTE_N) color_index = PALETTE_N - 1;
            gui_dirty();
            return;
        }
        int bx = 4 + PALETTE_N * 20;
        if (x >= bx + 4 && x < bx + 46) {
            canvas_clear();
            gui_dirty();
            return;
        }
        if (x >= bx + 52 && x < bx + 72) {
            if (brush > 1) brush--;
            gui_dirty();
            return;
        }
        if (x >= bx + 76 && x < bx + 96) {
            if (brush < 16) brush++;
            gui_dirty();
            return;
        }
        return;
    }

    int cx = x;
    int cy = y - PT_TOOLBAR;
    if (event == GUI_MOUSE_DOWN) {
        drawing = 1;
        canvas_dot(cx, cy, palette[color_index]);
        last_x = cx;
        last_y = cy;
        gui_dirty();
    } else if (event == GUI_MOUSE_MOVE && drawing) {
        if (last_x >= 0)
            canvas_line(last_x, last_y, cx, cy, palette[color_index]);
        else
            canvas_dot(cx, cy, palette[color_index]);
        last_x = cx;
        last_y = cy;
        gui_dirty();
    }
}

static void pt_key(window_t *w, int key, u32 mods)
{
    (void)w;
    (void)mods;
    if (key == 'c' || key == 'C')
        canvas_clear();
    else if (key == '+' || key == '=')
        brush = brush < 16 ? brush + 1 : brush;
    else if (key == '-' || key == '_')
        brush = brush > 1 ? brush - 1 : brush;
    else
        return;
    gui_dirty();
}

static void pt_init(window_t *w)
{
    (void)w;
    if (!canvas)
        canvas = (u32 *)kmalloc((size_t)CANVAS_W * CANVAS_H * 4);
    canvas_clear();
    drawing = 0;
    last_x = last_y = -1;
    gui_dirty();
}

static void pt_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_paint = {
    .title  = "Paint",
    .width  = PT_W,
    .height = PT_H,
    .init   = pt_init,
    .draw   = pt_draw,
    .key    = pt_key,
    .mouse  = pt_mouse,
    .close  = pt_close,
};
