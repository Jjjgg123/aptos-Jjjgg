#ifndef APTOS_STD_TIME_H
#define APTOS_STD_TIME_H

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec;    /* 0-59 */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0-6, sunday=0 */
    int tm_yday;   /* 0-365 */
    int tm_isdst;
};

time_t   time(time_t *tloc);
double   difftime(time_t t1, time_t t0);
clock_t  clock(void);
struct tm *gmtime(const time_t *tp);
struct tm *localtime(const time_t *tp);
time_t   mktime(struct tm *tp);

#endif
