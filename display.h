#ifndef DISPLAY_H
#define DISPLAY_H

#include "shelbourne.h"
#include <stdint.h>

/* Font metrics */
#define FONT_W 5
#define FONT_H 7
#define SMALL_CHAR_W 6   /* 1x: 5+1 gap */
#define BIG_CHAR_W 12    /* 2x */
#define BIG_CHAR_H 16
#define BIG_COLS (SHELBOURNE_FB_WIDTH / BIG_CHAR_W)  /* 10 */
#define HUGE_CHAR_W 24   /* 4x */
#define HUGE_CHAR_H 32

/* Initialize display module with hardware handle */
void display_init(shelbourne_t *hw);

/* 1x text (used for small labels like track index) */
void fb_putstr(int x, int y, const char *s);

/* 2x text */
void fb_center_2x(int y, const char *s);

/* 4x text (for clock) */
void fb_center_4x(int y, const char *s);

/* Word-wrap text in 2x font. Returns y after last line. */
int fb_wrap_2x(int y, int y_max, const char *text);

/* Blit a full-screen (128x100) logo into framebuffer */
void display_logo(const uint8_t *logo);

#endif /* DISPLAY_H */
