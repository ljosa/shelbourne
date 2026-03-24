#ifndef ADSB_H
#define ADSB_H

/*
 * ADS-B aircraft lookup via api.adsb.lol.
 *
 * Fetches nearby aircraft, sorts by distance, and returns structured
 * data suitable for the radar display.
 */

#define ADSB_MAX_AIRCRAFT 16

typedef struct {
    char callsign[12];
    char type[8];       /* ICAO type code */
    int  alt;           /* barometric altitude in feet, -1 = unknown, 0 = ground */
    int  gs;            /* ground speed in knots, -1 = unknown */
    int  track;         /* heading in degrees 0-359, -1 = unknown */
    int  dst_nm_x10;    /* distance in tenths of nautical miles */
    int  dir;           /* bearing from us in degrees 0-359, -1 = unknown */
} adsb_aircraft_t;

typedef struct {
    int count;
    int radius_nm;      /* chosen display radius (5/10/15/20/25) */
    adsb_aircraft_t ac[ADSB_MAX_AIRCRAFT];
} adsb_result_t;

/*
 * Query the ADS-B API for aircraft near (lat, lon) within max_nm.
 * Blocks for network I/O (~1-3 seconds).
 * Returns 0 on success, -1 on failure.
 */
int adsb_query(double lat, double lon, int max_nm, adsb_result_t *out);

#endif /* ADSB_H */
