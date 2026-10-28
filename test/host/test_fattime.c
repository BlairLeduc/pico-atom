/* test_fattime.c — the calendar behind a file's timestamp (fattime.h,
 * design.md §11.1). Each date is checked against the system's gmtime,
 * an independent calendar, as well as a few by hand.
 */

#include <time.h>

#include "fattime.h"
#include "test_util.h"

static uint32_t from_gmtime(int64_t s) {
    time_t t = (time_t)s;
    const struct tm *tm = gmtime(&t);   /* C11's: gmtime_r is POSIX */
    return FATTIME_PACK(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
                        tm->tm_min, tm->tm_sec);
}

int main(void) {
    CHECK(fattime_pack(315532800) == FATTIME_PACK(1980, 1, 1, 0, 0, 0), "FAT's epoch");
    CHECK(fattime_pack(951782400) == FATTIME_PACK(2000, 2, 29, 0, 0, 0), "2000 is a leap year");
    CHECK(fattime_pack(4107542399) == FATTIME_PACK(2100, 2, 28, 23, 59, 58),
          "2100 is not, and odd seconds round down");
    CHECK(fattime_pack(1793091845) == FATTIME_PACK(2026, 10, 27, 9, 4, 4), "a day this year");
    CHECK(fattime_pack(0) == FATTIME_PACK(1980, 1, 1, 0, 0, 0), "before 1980 is clamped");
    CHECK(fattime_pack(-86400) == FATTIME_PACK(1980, 1, 1, 0, 0, 0), "and before 1970");
    CHECK(fattime_pack(4354819199) == FATTIME_PACK(2107, 12, 31, 23, 59, 58), "FAT's last second");
    CHECK(fattime_pack(4354819200) == FATTIME_PACK(2107, 12, 31, 23, 59, 58),
          "after 2107 is clamped");
    CHECK(FATTIME_UNSET == FATTIME_PACK(2026, 1, 1, 0, 0, 0), "the unset clock's date");

    /* Every day of FAT's range, at a time that moves through the day. */
    if (sizeof(time_t) >= 8) {
        int bad = 0;
        for (int64_t s = 315532800 + 1; s <= 4354819198; s += 86400 + 3607) {
            if (fattime_pack(s) != from_gmtime(s)) bad++;
        }
        CHECK(bad == 0, "1980-2107 agrees with gmtime");
    }
    TEST_DONE();
}
