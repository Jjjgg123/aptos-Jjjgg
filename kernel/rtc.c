#include "rtc.h"
#include "io.h"

static u8 cmos_read(u8 reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd_to_bin(u8 v)
{
    return (v & 0x0F) + ((v >> 4) * 10);
}

void rtc_read(rtc_time_t *t)
{
    while (cmos_read(0x0A) & 0x80)
        ;

    u8 sec = cmos_read(0x00);
    u8 min = cmos_read(0x02);
    u8 hour = cmos_read(0x04);
    u8 day = cmos_read(0x07);
    u8 month = cmos_read(0x08);
    u8 year = cmos_read(0x09);
    u8 century = cmos_read(0x32);
    u8 status_b = cmos_read(0x0B);

    int pm = 0;
    if (!(status_b & 0x04)) {
        sec = (u8)bcd_to_bin(sec);
        min = (u8)bcd_to_bin(min);
        day = (u8)bcd_to_bin(day);
        month = (u8)bcd_to_bin(month);
        year = (u8)bcd_to_bin(year);
        century = (u8)bcd_to_bin(century);

        u8 h = hour & 0x7F;
        pm = (hour & 0x80) != 0;
        h = (u8)bcd_to_bin(h);
        hour = h;
    } else {
        pm = (hour & 0x80) != 0;
        hour &= 0x7F;
    }

    if (pm && hour < 12)
        hour = (u8)(hour + 12);
    if (!pm && hour == 12 && !(status_b & 0x04))
        hour = 0;

    t->second = sec;
    t->minute = min;
    t->hour = hour & 0x3F;
    t->day = day;
    t->month = month;
    t->year = century ? (century * 100 + year) : (2000 + year);
}

u32 rtc_date_stamp(void)
{
    rtc_time_t t;
    rtc_read(&t);
    return (u32)t.year * 10000u + (u32)t.month * 100u + (u32)t.day;
}

u32 rtc_time_stamp(void)
{
    rtc_time_t t;
    rtc_read(&t);
    return (u32)t.hour * 100u + (u32)t.minute;
}
