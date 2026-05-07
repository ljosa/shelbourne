#ifndef SUN_H
#define SUN_H

/*
 * Compute sunrise and sunset times in UTC (fractional hours).
 * Returns 0 on success, -1 if the sun never rises or never sets.
 *
 * Algorithm: US Naval Observatory simplified solar position.
 * Accuracy: ~2 minutes at mid-latitudes.
 */
int sun_rise_set(int year, int month, int day,
                 double lat, double lon,
                 double *rise_utc, double *set_utc);

#include <time.h>

/*
 * Should the clock be displayed at time `now`?
 * Returns 1 if `now` is between 30 min before sunrise
 * and 30 min after sunset for the given location, 0 otherwise.
 */
int sun_clock_visible_at(time_t now, double lat, double lon);

/* Convenience: equivalent to sun_clock_visible_at(time(NULL), lat, lon). */
int sun_clock_visible(double lat, double lon);

#endif /* SUN_H */
