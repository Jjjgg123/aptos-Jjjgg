#include "console.h"
#include "gfx.h"
#include "klog.h"
#include "util.h"

#define BG      RGB(10, 14, 22)
#define FG      RGB(198, 208, 220)
#define ACCENT  RGB(90, 190, 255)
#define DIM     RGB(110, 120, 135)

static int ready;
static int dirty;

void console_init(void)
{
    ready = 1;
    dirty = 1;
    klog_screen(1);
    console_update();
}

void console_shutdown(void)
{
    ready = 0;
    klog_screen(0);
}

void console_pump(void)
{
    dirty = 1;
}

void console_update(void)
{
    if (!ready || !dirty)
        return;
    dirty = 0;

    int w = gfx_width(), h = gfx_height();
    int line_h = 10;
    int top = 34;
    int rows = (h - top - 8) / line_h;
    if (rows < 1)
        rows = 1;

    gfx_reset_clip();
    gfx_clear(BG);

    gfx_text(10, 8, "aptos", ACCENT, 2);
    gfx_text(10 + 4 * 8 * 2 + 8, 14, "v2.0  -  32-bit x86 kernel", DIM, 1);
    gfx_hline(0, 30, w, RGB(30, 40, 55));

    int total = klog_count();
    int first = total > rows ? total - rows : 0;
    int y = top;
    for (int i = first; i < total; i++) {
        gfx_text(10, y, klog_line(i), FG, 1);
        y += line_h;
    }

    const char *last = klog_line(total - 1);
    int cx = 10 + (int)strlen(last) * 8;
    if (cx < w - 12 && y - line_h >= top)
        gfx_fill_rect(cx, y - line_h + 1, 7, 7, ACCENT);

    gfx_text(10, h - 12, "serial: COM1 115200 8N1   |   screen: VBE linear framebuffer",
             DIM, 1);

    gfx_present();
}
