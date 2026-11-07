#include "gui.h"
#include "keyboard.h"
#include "timer.h"
#include "util.h"

#define SN_COLS 40
#define SN_ROWS 20
#define SN_CELL 10
#define SN_W (SN_COLS * SN_CELL)
#define SN_H (SN_ROWS * SN_CELL)
#define SN_HEADER 18
#define SN_MAX (SN_COLS * SN_ROWS)

static int body[SN_MAX][2];
static int len;
static int dir;
static int pending_dir;
static int food_x, food_y;
static int score;
static int alive;
static u32 rng_state;
static u32 last_step;
static window_t *win;

static u32 rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

static int occupied(int x, int y)
{
    for (int i = 0; i < len; i++)
        if (body[i][0] == x && body[i][1] == y)
            return 1;
    return 0;
}

static void spawn_food(void)
{
    for (int i = 0; i < 4000; i++) {
        int x = (int)(rnd() % SN_COLS);
        int y = (int)(rnd() % SN_ROWS);
        if (!occupied(x, y)) {
            food_x = x;
            food_y = y;
            return;
        }
    }
    food_x = 0;
    food_y = 0;
}

static void reset(void)
{
    len = 5;
    for (int i = 0; i < len; i++) {
        body[i][0] = SN_COLS / 2 - i;
        body[i][1] = SN_ROWS / 2;
    }
    dir = 1;
    pending_dir = 1;
    score = 0;
    alive = 1;
    rng_state = timer_ticks() * 2654435761u + 97u;
    spawn_food();
    last_step = timer_uptime_ms();
}

static int step_delay(void)
{
    int d = 170 - score * 3;
    return d < 60 ? 60 : d;
}

static void step(void)
{
    static const int dx[4] = { 0, 1, 0, -1 };
    static const int dy[4] = { -1, 0, 1, 0 };

    if (!alive)
        return;

    dir = pending_dir;
    int nx = body[0][0] + dx[dir];
    int ny = body[0][1] + dy[dir];

    if (nx < 0 || ny < 0 || nx >= SN_COLS || ny >= SN_ROWS) {
        alive = 0;
        return;
    }
    for (int i = 0; i < len - 1; i++)
        if (body[i][0] == nx && body[i][1] == ny) {
            alive = 0;
            return;
        }

    int ate = (nx == food_x && ny == food_y);
    if (ate && len < SN_MAX) {
        len++;
        score += 10;
    }
    for (int i = len - 1; i > 0; i--) {
        body[i][0] = body[i - 1][0];
        body[i][1] = body[i - 1][1];
    }
    body[0][0] = nx;
    body[0][1] = ny;
    if (ate)
        spawn_food();
}

static void sn_tick(window_t *w)
{
    (void)w;
    if (!alive)
        return;
    if (timer_uptime_ms() - last_step < (u32)step_delay())
        return;
    last_step = timer_uptime_ms();
    step();
    gui_dirty();
}

static void sn_draw(window_t *w)
{
    int x = w->cx + 2;
    int y = w->cy + 2;

    char info[64];
    snprintf(info, sizeof(info), "SCORE %d    LENGTH %d    SPEED %dms", score, len,
             step_delay());
    gfx_text(x, y, info, RGB(140, 220, 140), 1);
    if (!alive)
        gfx_text(x + 170, y, "GAME OVER - press R", RGB(240, 130, 130), 1);

    int fx = w->cx + 2;
    int fy = w->cy + SN_HEADER;

    gfx_fill_rect(fx, fy, SN_W, SN_H, RGB(14, 18, 26));
    for (int i = 1; i < SN_COLS; i++)
        gfx_vline(fx + i * SN_CELL, fy, SN_H, RGB(20, 26, 36));
    for (int j = 1; j < SN_ROWS; j++)
        gfx_hline(fx, fy + j * SN_CELL, SN_W, RGB(20, 26, 36));
    gfx_rect(fx - 1, fy - 1, SN_W + 2, SN_H + 2, RGB(60, 70, 90));

    gfx_fill_circle(fx + food_x * SN_CELL + SN_CELL / 2,
                    fy + food_y * SN_CELL + SN_CELL / 2, SN_CELL / 2 - 1,
                    RGB(230, 80, 80));

    for (int i = len - 1; i >= 0; i--) {
        u32 c = (i == 0) ? RGB(150, 250, 150) : RGB(70, 190, 90);
        gfx_fill_rect(fx + body[i][0] * SN_CELL + 1, fy + body[i][1] * SN_CELL + 1,
                      SN_CELL - 2, SN_CELL - 2, c);
    }
}

static void sn_key(window_t *w, int key, u32 mods)
{
    (void)w;
    (void)mods;
    if (key == 'r' || key == 'R') {
        reset();
        gui_dirty();
        return;
    }
    if (!alive)
        return;

    int nd = dir;
    if (key == KEY_UP)    nd = 0;
    if (key == KEY_RIGHT) nd = 1;
    if (key == KEY_DOWN)  nd = 2;
    if (key == KEY_LEFT)  nd = 3;
    if (nd == dir)
        return;
    if ((nd + 2) % 4 == dir)
        return;
    pending_dir = nd;
}

static void sn_init(window_t *w)
{
    (void)w;
    reset();
    gui_dirty();
}

static void sn_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_snake = {
    .title  = "Snake",
    .width  = SN_W + 4,
    .height = SN_H + SN_HEADER + 4,
    .init   = sn_init,
    .draw   = sn_draw,
    .key    = sn_key,
    .tick   = sn_tick,
    .close  = sn_close,
};
