#ifndef DISPLAYD_H
#define DISPLAYD_H

#include "shelbourne.h"
#include <stdint.h>

#define DISP_TEXT_LEN 256
#define DISP_MAX_AC   16

enum disp_mode {
    DISP_RADIO,
    DISP_USB,
    DISP_PODCAST,
    DISP_AUX,
    DISP_AIRPLAY,
    DISP_CLOCK,
    DISP_BLANK,
    DISP_AIRCRAFT_SWEEP,  /* radar sweep animation (waiting for data) */
    DISP_AIRCRAFT,        /* radar with aircraft dots + data */
};

/* Compact aircraft data for pipe transfer */
typedef struct {
    char callsign[12];
    char type[8];
    int16_t alt_100;    /* altitude / 100 (saves space), -1 = unknown */
    int16_t gs;         /* ground speed knots, -1 = unknown */
    int16_t track;      /* heading degrees, -1 = unknown */
    int16_t dst_x10;    /* distance in tenths of nm */
    int16_t dir;        /* bearing degrees, -1 = unknown */
} disp_aircraft_t;

typedef struct {
    int mode;
    int playing;
    int track_num;
    int track_total;
    int volume;       /* >= 0: show volume bar at this level; -1: no change */
    uintptr_t logo;   /* logo data pointer (valid in forked child) */
    char text1[DISP_TEXT_LEN];
    char text2[DISP_TEXT_LEN];
    char text3[DISP_TEXT_LEN];
    char temp_str[32]; /* e.g. "38 °F outside", or "" if unavailable */
    /* Aircraft data (only used when mode == DISP_AIRCRAFT) */
    int ac_count;
    int ac_radius_nm;
    disp_aircraft_t ac[DISP_MAX_AC];
} display_msg_t;

/*
 * Fork display process. Returns pipe write fd (non-blocking).
 * Must be called after display_init().
 * lat/lon are used for sun position (clock display).
 */
int displayd_start(shelbourne_t *hw, double lat, double lon);

/* Send display state. Non-blocking; drops if pipe full. */
void displayd_send(int fd, const display_msg_t *msg);

/* Stop display process (closes pipe, waits for child). */
void displayd_stop(int fd);

#endif
