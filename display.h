#ifndef DISPLAY_H
#define DISPLAY_H

#include "shelbourne.h"
#include <stdint.h>

/* Initialize display module with hardware handle */
void display_init(shelbourne_t *hw);

/* Small text (proportional, ~14px) — for labels, track index, volume */
void fb_putstr(int x, int y, const char *s);
int fb_text_width(const char *s);  /* measure width in pixels */

/* Medium text (proportional, ~19px) — for station names, metadata */
void fb_center_2x(int y, const char *s);
int fb_wrap_2x(int y, int y_max, const char *text);

/*
 * Auto-scrolling medium text.  Centers if text fits; otherwise
 * scrolls left.  Returns 1 if scrolling (caller should schedule
 * another refresh — but throttled, not every frame).
 */
int fb_scroll_2x(int y, const char *text);

/* Large text (proportional bold, ~33px) — for clock display */
void fb_center_4x(int y, const char *s);

/* Blit a full-screen (128x100) logo into framebuffer */
void display_logo(const uint8_t *logo);

#endif /* DISPLAY_H */
