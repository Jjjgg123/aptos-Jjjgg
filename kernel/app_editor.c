#include "gui.h"
#include "fs.h"
#include "keyboard.h"
#include "util.h"

#define ED_W 560
#define ED_H 340
#define ED_TOOLBAR 22
#define ED_LINE_H 10
#define ED_BUF 8192

typedef struct {
    char text[ED_BUF];
    int  len;
    int  cur;
    int  scroll;
    char filename[40];
    int  dirty;
} editor_t;

static editor_t ed;
static window_t *win;

#define TEXT_X(w)  ((w)->cx + 4)
#define TEXT_Y(w)  ((w)->cy + ED_TOOLBAR + 2)
#define TEXT_ROWS(w) (((w)->ch - ED_TOOLBAR - 6) / ED_LINE_H)

static void ed_save(void)
{
    if (!ed.filename[0]) {
        strcpy(ed.filename, "/UNTITLED.TXT");
    }
    int n = fs_write(ed.filename, ed.text, (u32)ed.len);
    if (n >= 0) {
        ed.dirty = 0;
        gui_dirty();
    }
}

static int line_start(int target)
{
    int line = 0, i = 0;
    for (; i < ed.len; i++) {
        if (line == target)
            return i;
        if (ed.text[i] == '\n')
            line++;
    }
    return (line == target) ? i : ed.len;
}

static int line_of(int pos)
{
    int line = 0;
    for (int i = 0; i < pos && i < ed.len; i++)
        if (ed.text[i] == '\n')
            line++;
    return line;
}

static int col_of(int pos)
{
    int start = line_start(line_of(pos));
    return pos - start;
}

static int line_count(void)
{
    int n = 1;
    for (int i = 0; i < ed.len; i++)
        if (ed.text[i] == '\n')
            n++;
    return n;
}

static void ensure_visible(window_t *w)
{
    int line = line_of(ed.cur);
    int rows = TEXT_ROWS(w);
    if (rows < 1)
        rows = 1;
    if (line < ed.scroll)
        ed.scroll = line;
    if (line >= ed.scroll + rows)
        ed.scroll = line - rows + 1;
    if (ed.scroll < 0)
        ed.scroll = 0;
}

static void ed_insert(int ch)
{
    if (ed.len >= ED_BUF - 1)
        return;
    memmove(ed.text + ed.cur + 1, ed.text + ed.cur, (size_t)(ed.len - ed.cur));
    ed.text[ed.cur] = (char)ch;
    ed.cur++;
    ed.len++;
    ed.dirty = 1;
    gui_dirty();
}

static void ed_backspace(void)
{
    if (ed.cur <= 0)
        return;
    memmove(ed.text + ed.cur - 1, ed.text + ed.cur, (size_t)(ed.len - ed.cur));
    ed.cur--;
    ed.len--;
    ed.dirty = 1;
    gui_dirty();
}

static void ed_delete(void)
{
    if (ed.cur >= ed.len)
        return;
    memmove(ed.text + ed.cur, ed.text + ed.cur + 1, (size_t)(ed.len - ed.cur - 1));
    ed.len--;
    ed.dirty = 1;
    gui_dirty();
}

static void ed_draw(window_t *w)
{
    gfx_fill_rect(w->cx, w->cy, w->cw, ED_TOOLBAR, RGB(42, 47, 58));
    ui_button(w->cx + 4, w->cy + 3, 44, 16, "Save", 0);
    gfx_text(w->cx + 56, w->cy + 7, ed.filename[0] ? ed.filename : "(no name)",
             RGB(220, 226, 236), 1);
    const char *hint = ed.dirty ? "* unsaved   Ctrl+S saves" : "saved";
    int hw = gfx_text_w(hint, 1);
    gfx_text(w->cx + w->cw - hw - 6, w->cy + 7, hint,
             ed.dirty ? RGB(230, 190, 90) : RGB(130, 200, 130), 1);

    int rows = TEXT_ROWS(w);
    int y = TEXT_Y(w);
    int line = 0, col = 0;
    int cur_line = line_of(ed.cur);
    int cur_col = col_of(ed.cur);

    int i = line_start(ed.scroll);
    for (int row = 0; row < rows && i <= ed.len; row++) {
        int j = i;
        char buf[128];
        int n = 0;
        while (j < ed.len && ed.text[j] != '\n' && n < 120)
            buf[n++] = ed.text[j++];
        buf[n] = 0;

        if (n)
            gfx_text(TEXT_X(w), y, buf, RGB(206, 214, 226), 1);
        for (int c = 0; c < n; c++)
            if (buf[c] == '\t') {
                gfx_fill_rect(TEXT_X(w) + c * 8, y, 8, 8, RGB(30, 34, 42));
                gfx_text(TEXT_X(w) + c * 8, y, " ", RGB(206, 214, 226), 1);
            }

        if (row + ed.scroll == cur_line) {
            int cx = TEXT_X(w) + cur_col * 8;
            gfx_fill_rect(cx, y - 1, 2, 10, RGB(120, 210, 255));
        }
        y += ED_LINE_H;
        if (j >= ed.len)
            break;
        i = j + 1;
        line++;
        col = 0;
    }
    (void)line;
    (void)col;

    char status[64];
    snprintf(status, sizeof(status), "Ln %d, Col %d   %d bytes", cur_line + 1,
             cur_col + 1, ed.len);
    gfx_text(w->cx + 4, w->cy + w->ch - 10, status, RGB(130, 140, 156), 1);
}

static void ed_key(window_t *w, int key, u32 mods)
{
    (void)mods;
    if (key == 19) {
        ed_save();
        return;
    }
    switch (key) {
    case KEY_LEFT:
        if (ed.cur > 0) ed.cur--;
        break;
    case KEY_RIGHT:
        if (ed.cur < ed.len) ed.cur++;
        break;
    case KEY_UP: {
        int line = line_of(ed.cur);
        int col = col_of(ed.cur);
        if (line > 0) {
            int prev = line_start(line - 1);
            int len = line_start(line) - prev - 1;
            if (len < 0) len = 0;
            ed.cur = prev + (col > len ? len : col);
        }
        break;
    }
    case KEY_DOWN: {
        int line = line_of(ed.cur);
        int col = col_of(ed.cur);
        if (line + 1 < line_count()) {
            int next = line_start(line + 1);
            int end = line_start(line + 2);
            int len = end - next - 1;
            if (line + 2 > line_count()) len = ed.len - next;
            if (len < 0) len = 0;
            ed.cur = next + (col > len ? len : col);
        }
        break;
    }
    case KEY_HOME:
        ed.cur = line_start(line_of(ed.cur));
        break;
    case KEY_END: {
        int line = line_of(ed.cur);
        int start = line_start(line);
        int end = line_start(line + 1);
        ed.cur = (line + 1 >= line_count()) ? ed.len : (end - 1);
        if (ed.cur < start) ed.cur = start;
        break;
    }
    case KEY_DELETE:
        ed_delete();
        break;
    case '\b':
        ed_backspace();
        break;
    case '\n':
        ed_insert('\n');
        break;
    case '\t':
        ed_insert(' ');
        ed_insert(' ');
        break;
    default:
        if (key >= 32 && key < 127)
            ed_insert(key);
        break;
    }
    ensure_visible(w);
    gui_dirty();
}

static void ed_mouse(window_t *w, int x, int y, int buttons, int event)
{
    if (event != GUI_MOUSE_DOWN || !(buttons & MOUSE_LEFT))
        return;

    if (y < ED_TOOLBAR) {
        if (x >= 4 && x < 48)
            ed_save();
        return;
    }

    int row = (y - ED_TOOLBAR - 2) / ED_LINE_H + ed.scroll;
    int col = (x - 4) / 8;
    int line = 0, i = 0;
    for (; i < ed.len; i++) {
        if (line == row)
            break;
        if (ed.text[i] == '\n')
            line++;
    }
    ed.cur = i + col;
    if (ed.cur > ed.len)
        ed.cur = ed.len;
    gui_dirty();
}

static void ed_init(window_t *w)
{
    (void)w;
    gui_dirty();
}

static void ed_close(window_t *w)
{
    (void)w;
    win = NULL;
}

void app_editor_load(const char *filename)
{
    memset(&ed, 0, sizeof(ed));
    if (filename && *filename) {
        strncpy(ed.filename, filename, sizeof(ed.filename) - 1);
        int n = fs_read(ed.filename, ed.text, ED_BUF - 1);
        if (n > 0)
            ed.len = n;
        else if (n < 0)
            ed.len = 0;
    }
    ed.text[ed.len] = 0;
    ed.cur = 0;
    ed.scroll = 0;
    ed.dirty = 0;
    win = gui_open(&app_editor);
}

const app_t app_editor = {
    .title  = "Editor",
    .width  = ED_W,
    .height = ED_H,
    .init   = ed_init,
    .draw   = ed_draw,
    .key    = ed_key,
    .mouse  = ed_mouse,
    .close  = ed_close,
};
