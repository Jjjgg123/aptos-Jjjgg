#include "cards.h"

#define WIN_W 748
#define WIN_H 600
#define MARGIN 10
#define CW 60
#define CH 80
#define COL_STEP 72
#define COL_Y 118
#define FAN 22

static int cols[10][32];
static int face_up[10][32];
static int col_n[10];

static int stock[64];
static int stock_pos;
static int stock_count;

static int completed;
static int moves;

static int pick_col = -1;
static int pick_from = -1;

static int rng_state;

static int rnd(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static void new_game(void)
{
    int deck[104];
    int t[6];
    int k = 0;

    sys_time(t);
    rng_state = t[5] * 7919 + t[4] * 131 + t[3] * 17 + 1;

    for (int d = 0; d < 8; d++)
        for (int r = 0; r < 13; r++)
            deck[d * 13 + r] = r * 4;
    for (int i = 103; i > 0; i--) {
        int j = rnd() % (i + 1);
        int tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }

    for (int i = 0; i < 10; i++)
        col_n[i] = 0;

    for (int i = 0; i < 10; i++) {
        int num = (i < 4) ? 6 : 5;
        for (int j = 0; j < num; j++) {
            cols[i][col_n[i]] = deck[k++];
            face_up[i][col_n[i]] = (j == num - 1);
            col_n[i]++;
        }
    }

    stock_count = 0;
    while (k < 104 && stock_count < 64)
        stock[stock_count++] = deck[k++];
    stock_pos = 0;

    completed = 0;
    moves = 0;
    pick_col = -1;
    pick_from = -1;
}

static void clear_pick(void)
{
    pick_col = -1;
    pick_from = -1;
}

static void flip_tops(void)
{
    for (int c = 0; c < 10; c++)
        if (col_n[c] > 0 && !face_up[c][col_n[c] - 1])
            face_up[c][col_n[c] - 1] = 1;
}

static int run_ok(int col, int from)
{
    if (!face_up[col][from])
        return 0;
    for (int i = from; i + 1 < col_n[col]; i++) {
        int a = cols[col][i];
        int b = cols[col][i + 1];
        if (!face_up[col][i + 1])
            return 0;
        if (card_suit(a) != card_suit(b) || card_rank(a) != card_rank(b) + 1)
            return 0;
    }
    return 1;
}

static void score_runs(void)
{
    int again = 1;

    while (again) {
        again = 0;
        for (int c = 0; c < 10; c++) {
            int base;
            int ok = 1;

            if (col_n[c] < 13)
                continue;
            base = col_n[c] - 13;
            for (int i = 0; i < 13; i++) {
                if (!face_up[c][base + i] || card_rank(cols[c][base + i]) != 12 - i) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                col_n[c] -= 13;
                completed++;
                moves++;
                again = 1;
                flip_tops();
            }
        }
    }
    if (pick_col >= 0 && pick_from > col_n[pick_col])
        clear_pick();
}

static int can_place(int card, int col)
{
    int top;

    if (col_n[col] == 0)
        return 1;
    if (!face_up[col][col_n[col] - 1])
        return 0;
    top = cols[col][col_n[col] - 1];
    return card_rank(top) == card_rank(card) + 1;
}

static void move_pick(int dest)
{
    int run[16];
    int n = col_n[pick_col] - pick_from;

    for (int i = 0; i < n; i++)
        run[i] = cols[pick_col][pick_from + i];

    col_n[pick_col] = pick_from;
    for (int i = 0; i < n; i++) {
        cols[dest][col_n[dest]] = run[i];
        face_up[dest][col_n[dest]] = 1;
        col_n[dest]++;
    }
    moves++;
    clear_pick();
    flip_tops();
}

static void deal_stock(void)
{
    if (stock_count - stock_pos < 10)
        return;
    for (int c = 0; c < 10; c++)
        if (col_n[c] == 0)
            return;
    for (int c = 0; c < 10; c++) {
        cols[c][col_n[c]] = stock[stock_pos++];
        face_up[c][col_n[c]] = 1;
        col_n[c]++;
    }
    moves++;
    flip_tops();
}

static void handle_click(int x, int y)
{
    int sx = WIN_W - MARGIN - CW;
    int sy = 44;

    if (in_box(x, y, sx - 110, sy, CW + 110, CH)) {
        if (pick_col >= 0)
            clear_pick();
        else
            deal_stock();
        return;
    }

    for (int c = 0; c < 10; c++) {
        int cx = MARGIN + c * COL_STEP;
        int n = col_n[c];

        if (n == 0) {
            if (in_box(x, y, cx, COL_Y, CW, CH) && pick_col >= 0 && pick_col != c)
                move_pick(c);
            continue;
        }

        for (int j = n - 1; j >= 0; j--) {
            int cy = COL_Y + j * FAN;
            int hit = (j == n - 1) ? CH : FAN;

            if (!in_box(x, y, cx, cy, CW, hit))
                continue;

            if (pick_col >= 0) {
                if (pick_col == c && j >= pick_from)
                    clear_pick();
                else if (pick_col != c && can_place(cols[pick_col][pick_from], c))
                    move_pick(c);
                else
                    clear_pick();
            } else if (run_ok(c, j)) {
                pick_col = c;
                pick_from = j;
            }
            return;
        }
    }
}

static void draw(void)
{
    int i;

    sys_clear(C_FELT);
    sys_rect(0, 0, WIN_W, 32, C_FELT2);
    sys_text(MARGIN, 9, "Spider", RGB(240, 246, 240), 2);
    {
        char buf[100];
        snprintf(buf, sizeof(buf),
                 "spades only   runs %d/8   moves %d   deals left %d   N:new  Q:quit",
                 completed, moves, (stock_count - stock_pos) / 10);
        sys_text(138, 11, buf, RGB(206, 226, 208), 1);
    }

    if (completed >= 8)
        sys_text(MARGIN + 4, 42, "*** YOU WIN ***", RGB(255, 240, 140), 2);
    else {
        for (i = 0; i < completed && i < 8; i++)
            card_back(MARGIN + 2 + i * 14, 42, 12, 52);
    }

    {
        int sx = WIN_W - MARGIN - CW;
        int sy = 42;
        if (stock_count - stock_pos >= 10) {
            card_back(sx, sy, CW, CH);
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "x%d", (stock_count - stock_pos) / 10);
                sys_text(sx + 4, sy + CH - 12, buf, RGB(245, 245, 245), 1);
            }
        } else {
            sys_frame(sx, sy, CW, CH, C_SLOT);
            sys_text(sx + 8, sy + CH / 2 - 4, "empty", C_DIM, 1);
        }
        sys_text(sx - 108, sy + 36, "click to deal", C_DIM, 1);
    }

    for (i = 0; i < 10; i++) {
        int cx = MARGIN + i * COL_STEP;
        int n = col_n[i];

        if (n == 0)
            sys_frame(cx, COL_Y, CW, CH, C_SLOT);

        for (int j = 0; j < n; j++) {
            int cy = COL_Y + j * FAN;
            int sel = (pick_col == i && j >= pick_from);
            if (face_up[i][j])
                card_draw(cx, cy, cols[i][j], sel, CW, CH);
            else
                card_back(cx, cy, CW, CH);
        }
    }
}

int main(void)
{
    int prev_btn;
    int last_mx = -1, last_my = -1;
    int dirty = 1;

    if (sys_win_open("Spider", WIN_W, WIN_H) != 0) {
        printf("spider: cannot open a window\n");
        return 1;
    }

    new_game();

    {
        int ix, iy, ib, ins;
        sys_mouse(&ix, &iy, &ib, &ins);
        prev_btn = ib;
    }

    for (;;) {
        int k = sys_getkey();
        int mx, my, btn, inside;

        if (k == 'n' || k == 'N') {
            new_game();
            dirty = 1;
        } else if (k == 'q' || k == 'Q' || k == 27) {
            break;
        }

        sys_mouse(&mx, &my, &btn, &inside);
        if ((btn & MOUSE_BTN_LEFT) && !(prev_btn & MOUSE_BTN_LEFT)) {
            handle_click(mx, my);
            score_runs();
            dirty = 1;
        }
        prev_btn = btn;

        if (dirty) {
            draw();
            sys_flush();
            dirty = 0;
            last_mx = mx;
            last_my = my;
        } else if (mx != last_mx || my != last_my) {
            sys_pump();
            last_mx = mx;
            last_my = my;
        }

        sys_sleep(16);
    }

    return 0;
}
