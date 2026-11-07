#include "gui.h"
#include "util.h"
#include "fs.h"
#include "keyboard.h"
#include "util.h"

#define FL_W 430
#define FL_H 320
#define FL_TOOLBAR 22
#define FL_ROW_H 18

static window_t *win;
static int scroll;
static int count;
static char cwd[128] = "/";

static int has_parent(void)
{
    return strcmp(cwd, "/") != 0;
}

static int row_count(void)
{
    return count + (has_parent() ? 1 : 0);
}

static void enter(const char *path)
{
    strncpy(cwd, path, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
    scroll = 0;
    count = fs_count(cwd);
    gui_dirty();
}

void app_files_open_at(const char *path)
{
    enter(path);
}

static void refresh(void)
{
    count = fs_count(cwd);
    if (scroll > row_count() - 1)
        scroll = 0;
    if (scroll < 0)
        scroll = 0;
    gui_dirty();
}

static int rows_visible(window_t *w)
{
    int r = (w->ch - FL_TOOLBAR - 14) / FL_ROW_H;
    return r < 1 ? 1 : r;
}

static void fl_draw(window_t *w)
{
    gfx_fill_rect(w->cx, w->cy, w->cw, FL_TOOLBAR, RGB(42, 47, 58));
    ui_button(w->cx + 4, w->cy + 3, 34, 16, "Up", 0);
    ui_button(w->cx + 42, w->cy + 3, 56, 16, "Refresh", 0);

    fs_stat_t st;
    fs_stat(&st);
    char info[96];
    snprintf(info, sizeof(info), "%d files, %d dirs, %d KiB free", st.files, st.dirs,
             st.free_kb);
    gfx_text(w->cx + 104, w->cy + 7, info, RGB(180, 190, 206), 1);

    gfx_fill_rect(w->cx, w->cy + FL_TOOLBAR, w->cw, 14, RGB(34, 38, 47));
    gfx_text(w->cx + 6, w->cy + FL_TOOLBAR + 3, cwd, RGB(120, 200, 255), 1);

    int rows = rows_visible(w);
    int y = w->cy + FL_TOOLBAR + 14;
    int total = row_count();

    for (int r = 0; r < rows; r++) {
        int idx = scroll + r;
        if (idx >= total)
            break;

        if (r & 1)
            gfx_fill_rect(w->cx + 1, y - 1, w->cw - 2, FL_ROW_H, RGB(34, 38, 47));

        if (has_parent() && idx == 0) {
            gfx_text(w->cx + 6, y + 4, "[DIR]  ..", RGB(150, 200, 255), 1);
            gfx_text(w->cx + w->cw - 60, y + 4, "up", RGB(120, 130, 146), 1);
            y += FL_ROW_H;
            continue;
        }

        fs_dirent_t e;
        if (fs_list(cwd, idx - (has_parent() ? 1 : 0), &e) != 0)
            break;

        char label[64];
        if (e.flags & FS_FLAG_DIR) {
            snprintf(label, sizeof(label), "[DIR]  %s", e.name);
            gfx_text(w->cx + 6, y + 4, label, RGB(255, 214, 120), 1);
        } else {
            snprintf(label, sizeof(label), "       %s", e.name);
            gfx_text(w->cx + 6, y + 4, label, RGB(226, 232, 242), 1);
            char size[24];
            utoa(e.size, size, 10);
            strcat(size, " B");
            int sw = gfx_text_w(size, 1);
            gfx_text(w->cx + w->cw - sw - 8, y + 4, size, RGB(150, 160, 176), 1);
        }
        y += FL_ROW_H;
    }

    if (total == 0)
        gfx_text(w->cx + 10, w->cy + FL_TOOLBAR + 20, "(empty directory)",
                 RGB(150, 160, 176), 1);

    gfx_text(w->cx + 4, w->cy + w->ch - 10,
             "folder = open, .lst = install, other file = edit", RGB(120, 130, 146), 1);
}

static int is_package_name(const char *name)
{
    int n = (int)strlen(name);
    if (n < 5)
        return 0;
    return strcasecmp(name + n - 4, ".LST") == 0;
}

static void fl_mouse(window_t *w, int x, int y, int buttons, int event)
{
    (void)w;
    if (event != GUI_MOUSE_DOWN || !(buttons & MOUSE_LEFT))
        return;

    if (y < FL_TOOLBAR) {
        if (x >= 4 && x < 38) {
            char up[160];
            fs_parent_path(cwd, up, sizeof(up));
            enter(up);
        } else if (x >= 42 && x < 98) {
            refresh();
        }
        return;
    }

    if (y < FL_TOOLBAR + 14)
        return;

    int row = (y - FL_TOOLBAR - 14) / FL_ROW_H + scroll;
    if (row >= row_count())
        return;

    if (has_parent() && row == 0) {
        char up[160];
        fs_parent_path(cwd, up, sizeof(up));
        enter(up);
        return;
    }

    fs_dirent_t e;
    if (fs_list(cwd, row - (has_parent() ? 1 : 0), &e) != 0)
        return;

    char path[256];
    snprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", e.name);

    if (e.flags & FS_FLAG_DIR) {
        enter(path);
    } else if (is_package_name(e.name)) {
        extern void term_install_package(const char *path);
        gui_open(&app_terminal);
        term_install_package(path);
        gui_dirty();
    } else {
        app_editor_load(path);
    }
}

static void fl_key(window_t *w, int key, u32 mods)
{
    (void)mods;
    int rows = rows_visible(w);
    int total = row_count();

    if (key == KEY_DOWN && scroll + rows < total) {
        scroll++;
        gui_dirty();
    } else if (key == KEY_UP && scroll > 0) {
        scroll--;
        gui_dirty();
    } else if (key == KEY_PGUP) {
        scroll -= rows;
        if (scroll < 0) scroll = 0;
        gui_dirty();
    } else if (key == KEY_PGDN) {
        scroll += rows;
        if (scroll > total - 1) scroll = total - 1;
        if (scroll < 0) scroll = 0;
        gui_dirty();
    } else if (key == '\b') {
        char up[160];
        fs_parent_path(cwd, up, sizeof(up));
        enter(up);
    }
}

static void fl_init(window_t *w)
{
    (void)w;
    strcpy(cwd, "/");
    scroll = 0;
    refresh();
}

static void fl_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_files = {
    .title  = "Files",
    .width  = FL_W,
    .height = FL_H,
    .init   = fl_init,
    .draw   = fl_draw,
    .key    = fl_key,
    .mouse  = fl_mouse,
    .close  = fl_close,
};
