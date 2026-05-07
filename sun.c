/*
 * sun.c — sunrise/sunset calculation
 *
 * Algorithm: US Naval Observatory simplified solar position.
 * Reference: NOAA solar calculator, Ed Williams' sunrise algorithm.
 */

#include "sun.h"

#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RAD (M_PI / 180.0)
#define DEG (180.0 / M_PI)

int sun_rise_set(int year, int month, int day,
                 double lat, double lon,
                 double *rise_utc, double *set_utc)
{
    /* Day of year */
    int n1 = 275 * month / 9;
    int n2 = (month + 9) / 12;
    int n3 = 1 + (year - 4 * (year / 4) + 2) / 3;
    int N  = n1 - n2 * n3 + day - 30;

    double lngHour = lon / 15.0;

    for (int pass = 0; pass < 2; pass++) {
        /* pass 0 = sunrise, pass 1 = sunset */
        double t = N + ((pass == 0 ? 6.0 : 18.0) - lngHour) / 24.0;

        /* Sun's mean anomaly */
        double M = 0.9856 * t - 3.289;

        /* Sun's true longitude */
        double L = M + 1.916 * sin(M * RAD)
                     + 0.020 * sin(2.0 * M * RAD)
                     + 282.634;
        L = fmod(L, 360.0);
        if (L < 0) L += 360.0;

        /* Right ascension */
        double RA = DEG * atan(0.91764 * tan(L * RAD));
        RA = fmod(RA, 360.0);
        if (RA < 0) RA += 360.0;

        /* RA must be in same quadrant as L */
        int Lq  = ((int)(L  / 90.0)) * 90;
        int RAq = ((int)(RA / 90.0)) * 90;
        RA += (Lq - RAq);
        RA /= 15.0;  /* convert to hours */

        /* Sun's declination */
        double sinDec = 0.39782 * sin(L * RAD);
        double cosDec = cos(asin(sinDec));

        /* Hour angle (zenith 90.833° = official sunrise/sunset) */
        double cosH = (cos(90.833 * RAD) - sinDec * sin(lat * RAD))
                      / (cosDec * cos(lat * RAD));

        if (cosH > 1.0 || cosH < -1.0)
            return -1;  /* sun never rises or never sets */

        double H;
        if (pass == 0)
            H = 360.0 - DEG * acos(cosH);  /* sunrise */
        else
            H = DEG * acos(cosH);           /* sunset */
        H /= 15.0;  /* convert to hours */

        /* Local mean time → UTC */
        double T = H + RA - 0.06571 * t - 6.622;
        double UT = T - lngHour;
        if (UT < 0.0)  UT += 24.0;
        if (UT >= 24.0) UT -= 24.0;

        if (pass == 0)
            *rise_utc = UT;
        else
            *set_utc = UT;
    }
    return 0;
}

int sun_clock_visible_at(time_t now, double lat, double lon)
{
    struct tm *utc = gmtime(&now);
    if (!utc) return 1;  /* safe default: show clock */

    double rise_utc, set_utc;
    if (sun_rise_set(utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                     lat, lon, &rise_utc, &set_utc) < 0)
        return 1;  /* polar edge case: show clock */

    /* Current time as fractional UTC hours */
    double now_utc = utc->tm_hour + utc->tm_min / 60.0 + utc->tm_sec / 3600.0;

    /* 30 minutes = 0.5 hours */
    double start = rise_utc - 0.5;
    double end   = set_utc  + 0.5;

    /* Handle wrap-around midnight UTC */
    if (start < 0.0) {
        /* Sunrise window crosses 0 UTC backwards: visible if now > start+24 OR now < end */
        return (now_utc >= start + 24.0 || now_utc <= end);
    }
    if (end < start) {
        /* Sunset window crosses 24 UTC forwards (e.g. US longitudes in summer):
         * visible if now is after start OR before end */
        return (now_utc >= start || now_utc <= end);
    }

    return (now_utc >= start && now_utc <= end);
}

int sun_clock_visible(double lat, double lon)
{
    return sun_clock_visible_at(time(NULL), lat, lon);
}
