#include "aptos.h"

static int rng_state;

static int rnd(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

int main(void)
{
    int t[6];
    int secret, tries = 0;

    sys_time(t);
    rng_state = t[5] * 7919 + t[4] * 131 + t[3] + 1;
    secret = rnd() % 100 + 1;

    printf("\n=== guess the number (1..100) ===\n");
    printf("type a number and press enter, ESC quits\n\n");

    for (;;) {
        int value = 0, digits = 0, k;

        printf("guess> ");
        for (;;) {
            k = sys_readkey();
            if (k == '\n')
                break;
            if (k == 27) {
                printf("\nbye. the number was %d\n", secret);
                return 0;
            }
            if (k == '\b') {
                if (digits) {
                    digits--;
                    value /= 10;
                    printf("\b \b");
                }
                continue;
            }
            if (k >= '0' && k <= '9' && digits < 3) {
                value = value * 10 + (k - '0');
                digits++;
                putchar(k);
            }
        }
        printf("\n");
        if (digits == 0)
            continue;

        tries++;
        if (value == secret) {
            printf("*** correct! %d tries ***\n", tries);
            return 0;
        }
        if (value < secret)
            printf("too low\n");
        else
            printf("too high\n");
    }
}
