/* aptos 用户态最小时间库：RTC(sys_time) → epoch → struct tm 换算 */
#include "std/time.h"
#include "aptos.h"

/* 公历日序号（1970-01-01 = 0），Howard Hinnant days_from_civil */
static long days_from_civil(long y, int m, int d)
{
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}

time_t time(time_t *tloc)
{
    int r[6];
    if (sys_time(r) != 0)
        return (time_t)-1;
    /* r: y m d h min s */
    long days = days_from_civil(r[0], r[1], r[2]);
    time_t t = days * 86400L + r[3] * 3600L + r[4] * 60L + r[5];
    if (tloc)
        *tloc = t;
    return t;
}

double difftime(time_t t1, time_t t0)
{
    return (double)(t1 - t0);
}

clock_t clock(void)
{
    return 0;  /* 无 CPU 计时，简化返回 0 */
}

struct tm *gmtime(const time_t *tp)
{
    static struct tm tm;
    time_t t = *tp;
    long days = t / 86400;
    int secs = (int)(t % 86400);
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    long y;
    int m, d;
    civil_from_days(days, &y, &m, &d);
    tm.tm_year = (int)(y - 1900);
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = secs / 3600;
    tm.tm_min = (secs % 3600) / 60;
    tm.tm_sec = secs % 60;
    /* 星期：1970-01-01 是周四，day 0 => wday 4 */
    tm.tm_wday = (int)((days + 4) % 7);
    if (tm.tm_wday < 0)
        tm.tm_wday += 7;
    tm.tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm.tm_isdst = 0;
    return &tm;
}

struct tm *localtime(const time_t *tp)
{
    return gmtime(tp);  /* 无时区概念 */
}

time_t mktime(struct tm *tp)
{
    long days = days_from_civil(tp->tm_year + 1900, tp->tm_mon + 1, tp->tm_mday);
    time_t t = days * 86400L + tp->tm_hour * 3600L + tp->tm_min * 60L + tp->tm_sec;
    return t;
}
