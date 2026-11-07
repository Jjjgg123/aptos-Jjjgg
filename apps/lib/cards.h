#ifndef APTOS_CARDS_H
#define APTOS_CARDS_H

#include "aptos.h"

#define CARD_W 66
#define CARD_H 88
#define CARD_SMALL_W 52
#define CARD_SMALL_H 70

#define C_WHITE  RGB(246, 248, 250)
#define C_BLACK  RGB(30, 34, 42)
#define C_RED    RGB(198, 54, 54)
#define C_FELT   RGB(28, 96, 60)
#define C_FELT2  RGB(22, 76, 48)
#define C_SLOT   RGB(16, 62, 38)
#define C_SEL    RGB(250, 220, 90)
#define C_DIM    RGB(150, 158, 170)

static inline int card_rank(int c) { return c / 4; }
static inline int card_suit(int c) { return c % 4; }
static inline int card_red(int c) { return card_suit(c) == 1 || card_suit(c) == 2; }

static inline const char *card_rank_str(int c)
{
    static const char *names[13] = { "A", "2", "3", "4", "5", "6", "7",
                                     "8", "9", "10", "J", "Q", "K" };
    return names[card_rank(c)];
}

static inline const char *card_suit_str(int c)
{
    static const char *names[4] = { "S", "H", "D", "C" };
    return names[card_suit(c)];
}

static inline void card_draw(int x, int y, int card, int selected, int w, int h)
{
    if (card < 0) {
        sys_frame(x, y, w, h, C_SLOT);
        return;
    }

    sys_rect(x + 2, y + 2, w, h, RGB(10, 30, 20));
    sys_rect(x, y, w, h, selected ? C_SEL : C_WHITE);
    sys_frame(x, y, w, h, selected ? C_SEL : RGB(120, 128, 140));

    if (card >= 0) {
        int fg = card_red(card) ? C_RED : C_BLACK;
        const char *rk = card_rank_str(card);
        int rw = (int)strlen(rk) * 16;

        sys_text(x + 4, y + 4, rk, fg, 2);
        sys_text(x + 6 + rw, y + 4, card_suit_str(card), fg, 2);
        sys_text(x + w - 22, y + h - 20, card_suit_str(card), fg, 2);
    }
}

static inline void card_back(int x, int y, int w, int h)
{
    sys_rect(x, y, w, h, RGB(40, 60, 140));
    sys_frame(x, y, w, h, RGB(20, 30, 80));
    for (int i = 4; i < w - 6; i += 8)
        sys_rect(x + i, y + 4, 4, h - 8, RGB(60, 84, 176));
}

static inline int in_box(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

#endif
