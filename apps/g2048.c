#include "aptos.h"

/* g2048.app — 2048 的用户态版本（解耦样板）
 * 内核不再内置 2048；本程序只依赖 libaptos 的窗口/绘图/键鼠 syscall。
 */

#define KEY_UP    0x101
#define KEY_DOWN  0x102
#define KEY_LEFT  0x103
#define KEY_RIGHT 0x104

#define G_SIZE 4
#define G_CELL 54
#define G_GAP  6
#define G_W (G_SIZE * G_CELL + (G_SIZE + 1) * G_GAP)
#define G_H G_W
#define G_HEADER 20

#define WIN_W (G_W + 4)
#define WIN_H (G_H + G_HEADER + 14)

static int board[G_SIZE * G_SIZE];
static int score;
static int best;
static int over;
static int reached;
static unsigned rng_state;

static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

static void put_tile(void)
{
    int empty[G_SIZE * G_SIZE];
    int n = 0;
    for (int i = 0; i < G_SIZE * G_SIZE; i++)
        if (!board[i])
            empty[n++] = i;
    if (!n)
        return;
    board[empty[rnd() % (unsigned)n]] = (rnd() % 10 == 0) ? 4 : 2;
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
    int res[G_SIZE];
    int n = 0, changed = 0;

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

    n = 0;
    for (int i = 0; i < G_SIZE; i++)
        if (tmp[i])
            res[n++] = tmp[i];
    while (n < G_SIZE)
        res[n++] = 0;

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
        put_tile();
        if (!can_move())
            over = 1;
    }
}

static void reset(void)
{
    for (int i = 0; i < G_SIZE * G_SIZE; i++)
        board[i] = 0;
    score = 0;
    over = 0;
    reached = 0;
    rng_state = (unsigned)sys_ticks() * 2654435761u + 7717u;
    put_tile();
    put_tile();
}

static int tile_bg(int v)
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

static void draw(void)
{
    char info[64];
    char t[8];
    int gx = 2, gy = G_HEADER;

    snprintf(info, sizeof(info), "SCORE %d    BEST %d", score, best);
    sys_text(4, 2, info, RGB(220, 226, 236), 1);

    if (reached)
        sys_text(180, 2, "2048!", RGB(240, 220, 120), 1);
    else if (over)
        sys_text(150, 2, "NO MOVES - press R", RGB(240, 130, 130), 1);
    else
        sys_text(150, 2, "arrows play", RGB(130, 142, 160), 1);

    sys_rect(gx, gy, G_W, G_H, RGB(58, 54, 48));

    for (int y = 0; y < G_SIZE; y++) {
        for (int x = 0; x < G_SIZE; x++) {
            int v = board[y * G_SIZE + x];
            int px = gx + G_GAP + x * (G_CELL + G_GAP);
            int py = gy + G_GAP + y * (G_CELL + G_GAP);

            sys_rect(px, py, G_CELL, G_CELL, tile_bg(v));
            if (!v)
                continue;
            snprintf(t, sizeof(t), "%d", v);
            /* 用户态没有 gfx_text_w，按 8px/字符估算居中 */
            int tw = 0;
            for (const char *p = t; *p; p++)
                tw += 8;
            {
                int fg = (v <= 4) ? RGB(90, 84, 76) : RGB(250, 248, 244);
                sys_text(px + (G_CELL - tw) / 2, py + (G_CELL - 8) / 2, t, fg, 1);
            }
        }
    }
    sys_flush();
}

int main(void)
{
    int frames = 0;
    int prev_btn;

    if (sys_win_open("2048", WIN_W, WIN_H) != 0) {
        printf("2048: cannot open a window\n");
        return 1;
    }

    reset();
    {
        int ix, iy, ib, ins;
        sys_mouse(&ix, &iy, &ib, &ins);
        prev_btn = ib;
    }
    printf("2048: user-mode window %dx%d opened\n", WIN_W, WIN_H);

    for (;;) {
        int k = sys_getkey();
        int mx, my, btn, inside;

        if (k == 'r' || k == 'R') {
            reset();
        } else if (k == 'q' || k == 'Q' || k == 27) {
            break;
        } else if (!over) {
            if (k == KEY_UP)         move(0);
            else if (k == KEY_RIGHT) move(1);
            else if (k == KEY_DOWN)  move(2);
            else if (k == KEY_LEFT)  move(3);
        }

        sys_mouse(&mx, &my, &btn, &inside);
        prev_btn = btn;

        draw();
        if (frames < 3) {
            int n = 0;
            for (int i = 0; i < G_SIZE * G_SIZE; i++)
                if (board[i])
                    n++;
            printf("2048: frame %d score=%d tiles=%d\n", frames + 1, score, n);
            frames++;
        }
        sys_sleep(16);
    }
    return 0;
}
