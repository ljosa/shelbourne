/*
 * test_sun.c — unit tests for sun_clock_visible_at.
 *
 * Build & run natively (no cross-compile):
 *   make test
 */

#include "sun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

/* Build a UTC time_t for the given calendar moment. */
static time_t utc_time(int year, int month, int day, int hour, int min)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = min;
    /* timegm is non-portable but exists on Linux & macOS. */
    return timegm(&t);
}

static void check(const char *name, int expected, int actual)
{
    if (expected == actual) {
        printf("  PASS  %s\n", name);
    } else {
        printf("  FAIL  %s (expected %d, got %d)\n", name, expected, actual);
        failures++;
    }
}

int main(void)
{
    /* Shelbourne, VT — Mr Noisy & Little Miss Chatterbox. */
    const double lat = 44.0247;
    const double lon = -73.0111;

    printf("Shelbourne, VT (lat=%.4f, lon=%.4f)\n", lat, lon);

    /*
     * May 7, 2026 at this longitude:
     *   sunrise ≈ 09:36 UTC (≈ 5:36 EDT)
     *   sunset  ≈ 00:03 UTC next day (≈ 20:03 EDT)
     * So sunset wraps past midnight UTC. The buggy version returns 0
     * for the entire daylight period because it only handles the
     * sunrise-wrap case.
     */
    check("May 7 13:26 UTC (mid-morning EDT) — daylight, expect visible",
          1, sun_clock_visible_at(utc_time(2026, 5, 7, 13, 26), lat, lon));

    check("May 7 22:00 UTC (afternoon EDT) — daylight, expect visible",
          1, sun_clock_visible_at(utc_time(2026, 5, 7, 22, 0), lat, lon));

    check("May 7 16:00 UTC (noon EDT) — daylight, expect visible",
          1, sun_clock_visible_at(utc_time(2026, 5, 7, 16, 0), lat, lon));

    check("May 7 06:00 UTC (2am EDT, before sunrise) — expect hidden",
          0, sun_clock_visible_at(utc_time(2026, 5, 7, 6, 0), lat, lon));

    check("May 7 03:00 UTC (11pm EDT prior, after sunset) — expect hidden",
          0, sun_clock_visible_at(utc_time(2026, 5, 7, 3, 0), lat, lon));

    /*
     * Dec 21, 2026: rise ≈ 12:24 UTC, set ≈ 21:18 UTC.
     * No UTC midnight wrap — exercises the simple case.
     */
    check("Dec 21 17:00 UTC (noon EST) — daylight, expect visible",
          1, sun_clock_visible_at(utc_time(2026, 12, 21, 17, 0), lat, lon));

    check("Dec 21 06:00 UTC (1am EST) — night, expect hidden",
          0, sun_clock_visible_at(utc_time(2026, 12, 21, 6, 0), lat, lon));

    check("Dec 21 23:00 UTC (6pm EST, after sunset) — expect hidden",
          0, sun_clock_visible_at(utc_time(2026, 12, 21, 23, 0), lat, lon));

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d test(s) failed.\n", failures);
    return 1;
}
