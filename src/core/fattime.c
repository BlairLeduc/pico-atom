/* fattime.c — a file's timestamp, as FatFs packs it (design.md §11.1). */

#include "fattime.h"

/* Howard Hinnant's days_from_civil, inverted: days since 1970-01-01 to
 * a proleptic Gregorian date, in eras of 400 years from 0000-03-01. */
static void civil(int64_t days, int *y, unsigned *m, unsigned *d) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

uint32_t fattime_pack(int64_t local_s) {
    /* 1980-01-01 00:00 and 2107-12-31 23:59:58. */
    if (local_s < 315532800) return FATTIME_PACK(1980, 1, 1, 0, 0, 0);
    if (local_s > 4354819198) return FATTIME_PACK(2107, 12, 31, 23, 59, 58);

    int64_t days = local_s / 86400;
    unsigned sec = (unsigned)(local_s % 86400);
    int y;
    unsigned m, d;
    civil(days, &y, &m, &d);
    return FATTIME_PACK(y, m, d, sec / 3600, sec / 60 % 60, sec % 60);
}
