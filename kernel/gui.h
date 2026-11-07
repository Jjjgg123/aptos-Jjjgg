#ifndef APTOS_GUI_H
#define APTOS_GUI_H

#include "gfx.h"
#include "mouse.h"

struct window;
typedef struct window window_t;

typedef struct app {
    const char *title;
    int  width, height;
    void (*init)(window_t *w);
    void (*draw)(window_t *w);
    void (*key)(window_t *w, int key, u32 mods);
    void (*mouse)(window_t *w, int x, int y, int buttons, int event);
    void (*tick)(window_t *w);
    void (*close)(window_t *w);
} app_t;

struct window {
    const app_t *app;
    int  x, y;
    int  w, h;
    int  cx, cy, cw, ch;
    int  used;
    void *state;
    /* 应用自己的显示空间（GUI_SURFACE 可选特性）：应用只画这块缓冲，
       系统负责把它合成到屏幕上。buf == NULL 时退回"直接画屏幕"的旧路径。 */
    u32 *buf;
    int  bw, bh;
    /* 层级：0=普通窗口，1=对话框/置顶（永远压在普通窗口之上）；
       owner 指向上级窗口：上级关闭时下级一起关，且下级始终显示在上级之上。 */
    int  level;
    window_t *owner;
    /* 窗口类型标志：目前只有"无边框"一种。桌面就是这样一个窗口：
       全屏、没有标题栏、不可拖动、不可关闭、永远排在最底层，
       但它和普通应用一样占一个 z 序位置、有自己的脏区。 */
    int  flags;
};

#define WIN_FRAMELESS 0x1

#define GUI_MOUSE_DOWN 1
#define GUI_MOUSE_UP   2
#define GUI_MOUSE_MOVE 3

#define GUI_TITLE_H 20
#define GUI_BORDER  2
#define GUI_TASKBAR_H 26

void      gui_init(void);
void      gui_dirty(void);
void      gui_dirty_rect(int x0, int y0, int x1, int y1);
void      gui_dirty_window(window_t *w);
window_t *gui_open(const app_t *app);
window_t *gui_open_child(const app_t *app, window_t *owner);
void      gui_resize(window_t *w, int cw, int ch);
void      gui_close(window_t *w);
void      gui_focus(window_t *w);
window_t *gui_focused(void);
void      gui_poll(void);
void      gui_set_input(int enabled);
void      gui_cursor_refresh(void);
void      gui_run(void) __attribute__((noreturn));

void ui_panel(int x, int y, int w, int h, u32 bg);
int  ui_button(int x, int y, int w, int h, const char *label, int active);
void ui_text(int x, int y, const char *s, u32 color);

extern const app_t app_terminal;
extern const app_t app_desktop;
extern const app_t app_files;
extern const app_t app_editor;
extern const app_t app_paint;
extern const app_t app_about;
extern const app_t app_snake;
extern const app_t app_mines;
extern const app_t app_2048;

void app_editor_load(const char *filename);

#endif
