#ifndef APTOS_TIMER_H
#define APTOS_TIMER_H

#include "types.h"

void timer_init(u32 hz);
u32  timer_hz(void);
u32  timer_ticks(void);
u32  timer_uptime_ms(void);
void timer_sleep_ms(u32 ms);

#endif
