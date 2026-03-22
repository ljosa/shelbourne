#ifndef DISPLAYD_H
#define DISPLAYD_H

#include "shelbourne.h"
#include <stdint.h>

#define DISP_TEXT_LEN 256

enum disp_mode {
    DISP_RADIO,
    DISP_USB,
    DISP_PODCAST,
    DISP_AUX,
    DISP_AIRPLAY,
    DISP_CLOCK,
    DISP_BLANK,
};

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
    char temp_str[32]; /* e.g. "38 ºF outside", or "" if unavailable */
} display_msg_t;

/*
 * Fork display process. Returns pipe write fd (non-blocking).
 * Must be called after display_init().
 */
int displayd_start(shelbourne_t *hw);

/* Send display state. Non-blocking; drops if pipe full. */
void displayd_send(int fd, const display_msg_t *msg);

/* Stop display process (closes pipe, waits for child). */
void displayd_stop(int fd);

#endif
