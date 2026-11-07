#ifndef APTOS_RTC_H
#define APTOS_RTC_H

#include "types.h"

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} rtc_time_t;

void rtc_read(rtc_time_t *t);
u32  rtc_date_stamp(void);
u32  rtc_time_stamp(void);

#endif
