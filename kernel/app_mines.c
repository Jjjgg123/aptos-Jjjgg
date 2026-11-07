#include "gui.h"
#include "keyboard.h"
#include "timer.h"
#include "util.h"

#define MS_COLS 16
#define MS_ROWS 12
#define MS_CELL 16
#define MS_W (MS_COLS * MS_CELL)
#define MS_H (MS_ROWS * MS_CELL)
#define MS_HEADER 20
#define MS_MINES 30

#define ST_HIDDEN  0
#define ST_OPEN    1
#define ST_FLAG    2

static u8 board[MS_ROWS * MS_COLS];
static u8 seen[MS_ROWS * MS_COLS];
static u8 mine[MS_ROWS * MS_COLS];
static u8 count[MS_ROWS * MS_COLS];
static int seeded;
static int over;
static int won;
static int flags;
static int opened;
static u32 rng_state;
static window_t *win;

static u32 rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

static int idx(int x, int y) { return y * MS_COLS + x; }

static void place_mines(int safe_x, int safe_y)
{
    for (int i = 0; i < MS_ROWS * MS_COLS; i++)
        mine[i] = 0;

    int placed = 0;
    while (placed < MS_MINES) {
        int x = (int)(rnd() % MS_COLS);
        int y = (int)(rnd() % MS_ROWS);
        if (x == safe_x && y == safe_y)
            continue;
        if (mine[idx(x, y)])
            continue;
        mine[idx(x, y)] = 1;
        placed++;
    }

    for (int y = 0; y < MS_ROWS; y++) {
        for (int x = 0; x < MS_COLS; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= MS_COLS || ny >= MS_ROWS)
                        continue;
                    if (mine[idx(nx, ny)])
                        n++;
                }
            count[idx(x, y)] = (u8)n;
        }
    }
    seeded = 1;
}

static void reset(void)
{
    for (int i = 0; i < MS_ROWS * MS_COLS; i++) {
        board[i] = ST_HIDDEN;
        seen[i] = 0;
        mine[i] = 0;
        count[i] = 0;
    }
    seeded = 0;
    over = 0;
    won = 0;
    flags = 0;
    opened = 0;
    rng_state = timer_ticks() * 2654435761u + 12345u;
}

static void flood(int x, int y)
{
    if (x < 0 || y < 0 || x >= MS_COLS || y >= MS_ROWS)
        return;
    int i = idx(x, y);
    if (seen[i] || board[i] == ST_FLAG)
        return;
    seen[i] = 1;
    board[i] = ST_OPEN;
    opened++;
    if (count[i] != 0)
        return;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy)
                flood(x + dx, y + dy);
}

static void lose(void)
{
    over = 1;
    for (int i = 0; i < MS_ROWS * MS_COLS; i++)
        if (mine[i])
            board[i] = ST_OPEN;
}

static void ms_open(int x, int y)
{
    int i = idx(x, y);
    if (board[i] == ST_FLAG)
        return;
    if (mine[i]) {
        lose();
        return;
    }
    flood(x, y);
    if (opened >= MS_ROWS * MS_COLS - MS_MINES) {
        over = 1;
        won = 1;
        for (int k = 0; k < MS_ROWS * MS_COLS; k++)
            if (mine[k])
                board[k] = ST_FLAG;
    }
}

static u32 number_color(int n)
{
    static const u32 cols[9] = {
        RGB(30, 34, 42),  RGB(120, 170, 255), RGB(120, 220, 130), RGB(240, 130, 130),
        RGB(180, 130, 240), RGB(240, 190, 100), RGB(120, 220, 220), RGB(230, 230, 230),
        RGB(160, 170, 185)
    };
    return cols[n & 7];
}

static void ms_draw(window_t *w)
{
    int x0 = w->cx + 2;
    int y0 = w->cy + 2;

    char info[64];
    snprintf(info, sizeof(info), "MINES %d   FLAGS %d   LEFT %d", MS_MINES, flags,
             MS_MINES - flags);
    gfx_text(x0, y0, info, RGB(200, 210, 230), 1);
    if (over)
        gfx_text(x0 + 190, y0, won ? "YOU WIN - press R" : "BOOM - press R",
                 won ? RGB(140, 240, 150) : RGB(240, 130, 130), 1);

    int gx = w->cx + 2;
    int gy = w->cy + MS_HEADER;

    for (int y = 0; y < MS_ROWS; y++) {
        for (int x = 0; x < MS_COLS; x++) {
            int i = idx(x, y);
            int px = gx + x * MS_CELL;
            int py = gy + y * MS_CELL;

            if (board[i] == ST_HIDDEN) {
                gfx_fill_rect(px, py, MS_CELL - 1, MS_CELL - 1, RGB(70, 80, 100));
                gfx_fill_rect(px + 2, py + 2, MS_CELL - 5, 2, RGB(96, 108, 132));
            } else if (board[i] == ST_FLAG) {
                gfx_fill_rect(px, py, MS_CELL - 1, MS_CELL - 1, RGB(70, 80, 100));
                gfx_fill_rect(px + 6, py + 3, 2, 9, RGB(230, 230, 230));
                gfx_fill_rect(px + 5, py + 3, 6, 5, RGB(230, 90, 90));
            } else if (mine[i]) {
                gfx_fill_rect(px, py, MS_CELL - 1, MS_CELL - 1, RGB(150, 60, 60));
                gfx_fill_circle(px + MS_CELL / 2, py + MS_CELL / 2, 4, RGB(30, 20, 20));
            } else {
                gfx_fill_rect(px, py, MS_CELL - 1, MS_CELL - 1, RGB(38, 43, 54));
                if (count[i]) {
                    char t[2];
                    t[0] = (char)('0' + count[i]);
                    t[1] = 0;
                    gfx_text(px + 5, py + 4, t, number_color(count[i]), 1);
                }
            }
        }
    }
    gfx_rect(gx - 1, gy - 1, MS_W + 2, MS_H + 2, RGB(60, 70, 90));
    gfx_text(w->cx + 2, w->cy + w->ch - 10,
             "left click opens, right click flags, R restarts", RGB(120, 132, 150), 1);
}

static void ms_mouse(window_t *w, int x, int y, int buttons, int event)
{
    (void)w;
    if (event != GUI_MOUSE_DOWN || over)
        return;

    int gx = x - 2;
    int gy = y - MS_HEADER;
    if (gx < 0 || gy < 0)
        return;
    int cx = gx / MS_CELL;
    int cy = gy / MS_CELL;
    if (cx >= MS_COLS || cy >= MS_ROWS)
        return;

    int i = idx(cx, cy);

    if (buttons & MOUSE_RIGHT) {
        if (board[i] == ST_HIDDEN) {
            board[i] = ST_FLAG;
            flags++;
        } else if (board[i] == ST_FLAG) {
            board[i] = ST_HIDDEN;
            flags--;
        }
        gui_dirty();
        return;
    }

    if (!(buttons & MOUSE_LEFT))
        return;
    if (!seeded)
        place_mines(cx, cy);
    if (board[i] == ST_OPEN)
        return;
    ms_open(cx, cy);
    gui_dirty();
}

static void ms_key(window_t *w, int key, u32 mods)
{
    (void)w;
    (void)mods;
    if (key == 'r' || key == 'R') {
        reset();
        gui_dirty();
    }
}

static void ms_init(window_t *w)
{
    (void)w;
    reset();
    gui_dirty();
}

static void ms_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_mines = {
    .title  = "Minesweeper",
    .width  = MS_W + 4,
    .height = MS_H + MS_HEADER + 14,
    .init   = ms_init,
    .draw   = ms_draw,
    .key    = ms_key,
    .mouse  = ms_mouse,
    .close  = ms_close,
};
