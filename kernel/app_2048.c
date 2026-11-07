#include "gui.h"
#include "keyboard.h"
#include "timer.h"
#include "util.h"

#define G_SIZE 4
#define G_CELL 54
#define G_GAP 6
#define G_W (G_SIZE * G_CELL + (G_SIZE + 1) * G_GAP)
#define G_H (G_SIZE * G_CELL + (G_SIZE + 1) * G_GAP)
#define G_HEADER 20

static int board[G_SIZE * G_SIZE];
static int score;
static int best;
static int over;
static int reached;
static u32 rng_state;
static window_t *win;

static u32 rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

static void spawn(void)
{
    int empty[G_SIZE * G_SIZE];
    int n = 0;
    for (int i = 0; i < G_SIZE * G_SIZE; i++)
        if (!board[i])
            empty[n++] = i;
    if (!n)
        return;
    int k = empty[rnd() % (u32)n];
    board[k] = (rnd() % 10 == 0) ? 4 : 2;
}

static int can_move(void)
{
    for (int y = 0; y < G_SIZE; y++)
        for (int x = 0; x < G_SIZE; x++) {
            if (!board[y * G_SIZE + x])
                return 1;
            if (x + 1 < G_SIZE && board[y * G_SIZE + x] == board[y * G_SIZE + x + 1])
                return 1;
            if (y + 1 < G_SIZE && board[y * G_SIZE + x] == board[(y + 1) * G_SIZE + x])
                return 1;
        }
    return 0;
}

static int slide(int *line)
{
    int tmp[G_SIZE];
    int n = 0;
    for (int i = 0; i < G_SIZE; i++)
        if (line[i])
            tmp[n++] = line[i];
    while (n < G_SIZE)
        tmp[n++] = 0;

    for (int i = 0; i + 1 < G_SIZE; i++) {
        if (tmp[i] && tmp[i] == tmp[i + 1]) {
            tmp[i] *= 2;
            tmp[i + 1] = 0;
            score += tmp[i];
            if (tmp[i] > best)
                best = tmp[i];
            if (tmp[i] == 2048)
                reached = 1;
        }
    }

    int res[G_SIZE];
    n = 0;
    for (int i = 0; i < G_SIZE; i++)
        if (tmp[i])
            res[n++] = tmp[i];
    while (n < G_SIZE)
        res[n++] = 0;

    int changed = 0;
    for (int i = 0; i < G_SIZE; i++) {
        if (line[i] != res[i])
            changed = 1;
        line[i] = res[i];
    }
    return changed;
}

static void move(int dir)
{
    int changed = 0;

    for (int i = 0; i < G_SIZE; i++) {
        int line[G_SIZE];
        for (int j = 0; j < G_SIZE; j++) {
            int k;
            if (dir == 0)      k = j * G_SIZE + i;
            else if (dir == 1) k = i * G_SIZE + (G_SIZE - 1 - j);
            else if (dir == 2) k = (G_SIZE - 1 - j) * G_SIZE + i;
            else               k = i * G_SIZE + j;
            line[j] = board[k];
        }

        if (slide(line)) {
            changed = 1;
            for (int j = 0; j < G_SIZE; j++) {
                int k;
                if (dir == 0)      k = j * G_SIZE + i;
                else if (dir == 1) k = i * G_SIZE + (G_SIZE - 1 - j);
                else if (dir == 2) k = (G_SIZE - 1 - j) * G_SIZE + i;
                else               k = i * G_SIZE + j;
                board[k] = line[j];
            }
        }
    }

    if (changed) {
        spawn();
        if (!can_move())
            over = 1;
        gui_dirty();
    }
}

static void reset(void)
{
    for (int i = 0; i < G_SIZE * G_SIZE; i++)
        board[i] = 0;
    score = 0;
    over = 0;
    reached = 0;
    rng_state = timer_ticks() * 2654435761u + 7717u;
    spawn();
    spawn();
}

static u32 tile_bg(int v)
{
    switch (v) {
    case 2:    return RGB(238, 228, 218);
    case 4:    return RGB(237, 224, 200);
    case 8:    return RGB(242, 177, 121);
    case 16:   return RGB(245, 149, 99);
    case 32:   return RGB(246, 124, 95);
    case 64:   return RGB(246, 94, 59);
    case 128:  return RGB(237, 207, 114);
    case 256:  return RGB(237, 204, 97);
    case 512:  return RGB(237, 200, 80);
    case 1024: return RGB(237, 197, 63);
    case 2048: return RGB(237, 194, 46);
    default:   return RGB(60, 58, 50);
    }
}

static void g_draw(window_t *w)
{
    int x0 = w->cx + 2;
    int y0 = w->cy + 2;

    char info[64];
    snprintf(info, sizeof(info), "SCORE %d    BEST %d", score, best);
    gfx_text(x0, y0, info, RGB(220, 226, 236), 1);
    if (reached)
        gfx_text(x0 + 170, y0, "2048!", RGB(240, 220, 120), 1);
    else if (over)
        gfx_text(x0 + 170, y0, "NO MOVES - press R", RGB(240, 130, 130), 1);
    else
        gfx_text(x0 + 170, y0, "arrows to play", RGB(130, 142, 160), 1);

    int gx = w->cx + 2;
    int gy = w->cy + G_HEADER;

    gfx_fill_rect(gx, gy, G_W, G_H, RGB(58, 54, 48));

    for (int y = 0; y < G_SIZE; y++) {
        for (int x = 0; x < G_SIZE; x++) {
            int v = board[y * G_SIZE + x];
            int px = gx + G_GAP + x * (G_CELL + G_GAP);
            int py = gy + G_GAP + y * (G_CELL + G_GAP);

            gfx_fill_rect(px, py, G_CELL, G_CELL, tile_bg(v));
            if (!v)
                continue;
            char t[8];
            snprintf(t, sizeof(t), "%d", v);
            int tw = gfx_text_w(t, 1);
            u32 fg = (v <= 4) ? RGB(90, 84, 76) : RGB(250, 248, 244);
            gfx_text(px + (G_CELL - tw) / 2, py + (G_CELL - 8) / 2, t, fg, 1);
        }
    }
}

static void g_key(window_t *w, int key, u32 mods)
{
    (void)w;
    (void)mods;
    if (key == 'r' || key == 'R') {
        reset();
        gui_dirty();
        return;
    }
    if (over)
        return;
    if (key == KEY_UP)         move(0);
    else if (key == KEY_RIGHT) move(1);
    else if (key == KEY_DOWN)  move(2);
    else if (key == KEY_LEFT)  move(3);
}

static void g_init(window_t *w)
{
    (void)w;
    if (!best)
        best = 0;
    reset();
    gui_dirty();
}

static void g_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_2048 = {
    .title  = "2048",
    .width  = G_W + 4,
    .height = G_H + G_HEADER + 14,
    .init   = g_init,
    .draw   = g_draw,
    .key    = g_key,
    .close  = g_close,
};
