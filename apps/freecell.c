#include "cards.h"

#define WIN_W 780
#define WIN_H 582
#define MARGIN 12
#define SLOT_W 66
#define SLOT_H 88
#define SLOT_GAP 78
#define COL_STEP 96
#define TOP_Y 46
#define COL_Y 182
#define FAN 24

#define KIND_NONE 0
#define KIND_CELL 1
#define KIND_COL  2
#define KIND_FOUND 3

static int cells[4];
static int found[4];
static int cols[8][24];
static int col_n[8];

static int pick_kind;
static int pick_index;
static int pick_card;
static int pick_run;
static int moves;
static int won;

static int rng_state;

static int rnd(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static void new_game(void)
{
    int deck[52];
    int t[6];
    int k = 0;

    sys_time(t);
    rng_state = t[5] * 7919 + t[4] * 131 + t[3] * 17 + 1;

    for (int i = 0; i < 52; i++)
        deck[i] = i;
    for (int i = 51; i > 0; i--) {
        int j = rnd() % (i + 1);
        int tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }

    for (int i = 0; i < 4; i++) {
        cells[i] = -1;
        found[i] = -1;
    }
    for (int i = 0; i < 8; i++)
        col_n[i] = 0;

    for (int i = 0; i < 8; i++) {
        int num = (i < 4) ? 7 : 6;
        for (int j = 0; j < num; j++)
            cols[i][col_n[i]++] = deck[k++];
    }

    pick_kind = KIND_NONE;
    pick_card = -1;
    pick_run = 0;
    moves = 0;
    won = 0;
}

static void clear_pick(void)
{
    pick_kind = KIND_NONE;
    pick_card = -1;
    pick_run = 0;
}

static int run_ok(int col, int from)
{
    for (int i = from; i + 1 < col_n[col]; i++) {
        int a = cols[col][i];
        int b = cols[col][i + 1];
        if (card_rank(a) != card_rank(b) + 1)
            return 0;
        if (card_red(a) == card_red(b))
            return 0;
    }
    return 1;
}

static int free_cells(void)
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        if (cells[i] < 0)
            n++;
    return n;
}

static int empty_cols(void)
{
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (col_n[i] == 0)
            n++;
    return n;
}

static int max_run(void)
{
    int n = (free_cells() + 1) * (empty_cols() + 1);
    return (n > 13) ? 13 : n;
}

static int can_tableau(int card, int col)
{
    int top;

    if (col_n[col] == 0)
        return 1;
    top = cols[col][col_n[col] - 1];
    return card_rank(top) == card_rank(card) + 1 && card_red(top) != card_red(card);
}

static int can_foundation(int card, int f)
{
    if (card_suit(card) != f)
        return 0;
    if (found[f] < 0)
        return card_rank(card) == 0;
    return card_rank(card) == card_rank(found[f]) + 1;
}

static int picked_run(int *out)
{
    int n = 0;

    if (pick_kind == KIND_CELL) {
        if (cells[pick_index] >= 0)
            out[n++] = cells[pick_index];
    } else if (pick_kind == KIND_COL) {
        for (int i = col_n[pick_index] - pick_run; i < col_n[pick_index]; i++)
            if (i >= 0)
                out[n++] = cols[pick_index][i];
    } else if (pick_kind == KIND_FOUND) {
        if (found[pick_index] >= 0)
            out[n++] = found[pick_index];
    }
    return n;
}

static void drop_pick(void)
{
    if (pick_kind == KIND_CELL)
        cells[pick_index] = -1;
    else if (pick_kind == KIND_COL)
        col_n[pick_index] -= pick_run;
    else if (pick_kind == KIND_FOUND)
        found[pick_index] = -1;
    clear_pick();
}

static int move_to_cell(int cell)
{
    int card = pick_card;

    if (cells[cell] >= 0 || pick_run != 1 || card < 0)
        return 0;
    drop_pick();
    cells[cell] = card;
    moves++;
    return 1;
}

static int move_to_foundation(int f)
{
    int run[1];
    int card;

    if (picked_run(run) != 1 || !can_foundation(run[0], f))
        return 0;
    card = pick_card;
    drop_pick();
    found[f] = card;
    moves++;
    return 1;
}

static int move_to_tableau(int col)
{
    int run[24];
    int n = picked_run(run);

    if (n == 0)
        return 0;
    if (n > max_run())
        return 0;
    if (!can_tableau(run[0], col))
        return 0;

    drop_pick();
    for (int i = 0; i < n; i++)
        cols[col][col_n[col]++] = run[i];
    moves++;
    return 1;
}

static void check_win(void)
{
    for (int i = 0; i < 4; i++)
        if (found[i] < 0 || card_rank(found[i]) != 12)
            return;
    won = 1;
}

static void auto_play(void)
{
    int moved = 1;

    while (moved) {
        moved = 0;

        if (pick_kind == KIND_NONE) {
            for (int i = 0; i < 4; i++) {
                if (cells[i] >= 0 && can_foundation(cells[i], card_suit(cells[i]))) {
                    found[card_suit(cells[i])] = cells[i];
                    cells[i] = -1;
                    moved = 1;
                    moves++;
                }
            }
            for (int c = 0; c < 8; c++) {
                if (col_n[c] > 0) {
                    int top = cols[c][col_n[c] - 1];
                    if (can_foundation(top, card_suit(top))) {
                        found[card_suit(top)] = top;
                        col_n[c]--;
                        moved = 1;
                        moves++;
                    }
                }
            }
        } else if (pick_run == 1 && pick_card >= 0 &&
                   can_foundation(pick_card, card_suit(pick_card))) {
            int f = card_suit(pick_card);
            int card = pick_card;
            drop_pick();
            found[f] = card;
            moves++;
            moved = 1;
        }
    }
    check_win();
}

static void handle_click(int x, int y)
{
    int i;

    for (i = 0; i < 4; i++) {
        int sx = MARGIN + i * SLOT_GAP;
        if (in_box(x, y, sx, TOP_Y, SLOT_W, SLOT_H)) {
            if (pick_card >= 0) {
                if (pick_kind == KIND_CELL && pick_index == i)
                    clear_pick();
                else if (!move_to_cell(i))
                    clear_pick();
            } else if (cells[i] >= 0) {
                pick_kind = KIND_CELL;
                pick_index = i;
                pick_card = cells[i];
                pick_run = 1;
            }
            return;
        }
    }

    for (i = 0; i < 4; i++) {
        int fx = WIN_W - MARGIN - (4 - i) * SLOT_GAP + 12;
        if (in_box(x, y, fx, TOP_Y, SLOT_W, SLOT_H)) {
            if (pick_card >= 0) {
                if (pick_kind == KIND_FOUND && pick_index == i)
                    clear_pick();
                else if (!move_to_foundation(i))
                    clear_pick();
            } else if (found[i] >= 0) {
                pick_kind = KIND_FOUND;
                pick_index = i;
                pick_card = found[i];
                pick_run = 1;
            }
            return;
        }
    }

    for (i = 0; i < 8; i++) {
        int cx = MARGIN + i * COL_STEP;
        int n = col_n[i];

        if (n == 0) {
            if (in_box(x, y, cx, COL_Y, SLOT_W, SLOT_H)) {
                if (pick_card >= 0 && !move_to_tableau(i))
                    clear_pick();
                return;
            }
            continue;
        }

        for (int j = n - 1; j >= 0; j--) {
            int cy = COL_Y + j * FAN;
            int hit = (j == n - 1) ? SLOT_H : FAN;

            if (!in_box(x, y, cx, cy, SLOT_W, hit))
                continue;

            if (pick_card >= 0) {
                if (pick_kind == KIND_COL && pick_index == i && j >= n - pick_run)
                    clear_pick();
                else if (!move_to_tableau(i))
                    clear_pick();
            } else if (run_ok(i, j)) {
                pick_kind = KIND_COL;
                pick_index = i;
                pick_run = n - j;
                pick_card = cols[i][j];
            }
            return;
        }
    }
}

static void draw(void)
{
    int i;

    sys_clear(C_FELT);
    sys_rect(0, 0, WIN_W, 34, C_FELT2);
    sys_text(MARGIN, 10, "FreeCell", RGB(240, 246, 240), 2);
    {
        char buf[90];
        snprintf(buf, sizeof(buf), "moves %d   free %d   N:new  A:auto  Q:quit",
                 moves, free_cells());
        sys_text(160, 12, buf, RGB(206, 226, 208), 1);
    }

    for (i = 0; i < 4; i++) {
        int sx = MARGIN + i * SLOT_GAP;
        sys_frame(sx, TOP_Y, SLOT_W, SLOT_H, C_SLOT);
        if (cells[i] >= 0)
            card_draw(sx, TOP_Y, cells[i], 0, SLOT_W, SLOT_H);
    }

    for (i = 0; i < 4; i++) {
        int fx = WIN_W - MARGIN - (4 - i) * SLOT_GAP + 12;
        sys_frame(fx, TOP_Y, SLOT_W, SLOT_H, C_SLOT);
        sys_text(fx + 4, TOP_Y + SLOT_H - 14, card_suit_str(i * 4), C_DIM, 1);
        if (found[i] >= 0)
            card_draw(fx, TOP_Y, found[i], 0, SLOT_W, SLOT_H);
    }

    for (i = 0; i < 8; i++) {
        int cx = MARGIN + i * COL_STEP;
        int n = col_n[i];

        if (n == 0)
            sys_frame(cx, COL_Y, SLOT_W, SLOT_H, C_SLOT);

        for (int j = 0; j < n; j++) {
            int cy = COL_Y + j * FAN;
            int sel = (pick_kind == KIND_COL && pick_index == i && j >= n - pick_run);
            card_draw(cx, cy, cols[i][j], sel, SLOT_W, SLOT_H);
        }
    }

    if (pick_kind != KIND_NONE && pick_card >= 0) {
        int mx, my, mb, inside;
        sys_mouse(&mx, &my, &mb, &inside);
        card_draw(mx - SLOT_W / 2, my - SLOT_H / 2, pick_card, 1, SLOT_W, SLOT_H);
    }

    if (won)
        sys_text(WIN_W / 2 - 96, WIN_H - 34, "*** YOU WIN ***", RGB(255, 240, 140), 2);
}

int main(void)
{
    int prev_btn;
    int last_mx = -1, last_my = -1;
    int dirty = 1;

    if (sys_win_open("FreeCell", WIN_W, WIN_H) != 0) {
        printf("freecell: cannot open a window\n");
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
        } else if (k == 'a' || k == 'A') {
            auto_play();
            dirty = 1;
        } else if (k == 'q' || k == 'Q' || k == 27) {
            break;
        }

        sys_mouse(&mx, &my, &btn, &inside);
        if ((btn & MOUSE_BTN_LEFT) && !(prev_btn & MOUSE_BTN_LEFT)) {
            handle_click(mx, my);
            auto_play();
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
