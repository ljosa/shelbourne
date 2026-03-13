/*
 * player.c — MP3/radio/AUX player using the Shelbourne library
 *
 * Controls:
 *   Volume Up/Down  = Adjust volume (+/- 5, minimum 5)
 *   Preset 1-4      = Select source (press again to cycle)
 *   AUX             = Switch to AUX input
 *   Power           = Pause/Play (radio/USB), Mute (AUX)
 */

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "shelbourne.h"
#include "station_logos.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Configuration ─────────────────────────────────────────────── */

#define MAX_FILES       500
#define MAX_PATH_LEN    512
#define MAX_TAG         256
#define MP3_BUF_SIZE    (16 * 1024)
#define DEFAULT_VOL     50
#define MIN_VOL         5
#define VOL_STEP        5
#define VOL_BAR_MS      2000   /* volume bar display duration */
#define NUM_PRESETS     6

/* ── Source model ──────────────────────────────────────────────── */

enum source_type { SRC_RADIO, SRC_DIR };

typedef struct {
    enum source_type type;
    const char *name;
    const char *host;
    int         port;
    const char *path;
    const char *dir;
    const uint8_t *logo;
} source_t;

/* Preset 1: Vermont / BBC talk */
static const source_t p1_sources[] = {
    { SRC_RADIO, "VPR News",    "vpr.streamguys1.com",           80, "/vpr64",            .logo = logo_vpr },
    { SRC_RADIO, "WDEV",        "playerservices.streamtheworld.com", 80, "/api/livestream-redirect/WDEVFM.mp3", .logo = logo_wdev },
    { SRC_RADIO, "BBC World",   "stream.live.vc.bbcmedia.co.uk", 80, "/bbc_world_service", .logo = logo_bbc_ws },
};

/* Preset 2: NRK (Norway) */
static const source_t p2_sources[] = {
    { SRC_RADIO, "NRK P1",     "lyd.nrk.no", 80, "/icecast/mp3/high/s0w7hwn47m/p1", .logo = logo_nrk_p1 },
    { SRC_RADIO, "NRK P2",     "lyd.nrk.no", 80, "/icecast/mp3/high/s0w7hwn47m/p2", .logo = logo_nrk_p2 },
    { SRC_RADIO, "NRK P3",     "lyd.nrk.no", 80, "/icecast/mp3/high/s0w7hwn47m/p3", .logo = logo_nrk_p3 },
};

/* Preset 3: Rock + SomaFM */
static const source_t p3_sources[] = {
    { SRC_RADIO, "WBTZ",        "playerservices.streamtheworld.com", 80, "/api/livestream-redirect/WBTZFM.mp3", .logo = logo_wbtz },
    { SRC_RADIO, "KROQ",        "live.amperwave.net",             80, "/direct/audacy-kroqfmmp3-imc", .logo = logo_kroq },
    { SRC_RADIO, "Groove Salad","ice1.somafm.com",                80, "/groovesalad-128-mp3", .logo = logo_groovesalad },
    { SRC_RADIO, "Indie Pop",   "ice1.somafm.com",                80, "/indiepop-128-mp3",    .logo = logo_indiepop },
    { SRC_RADIO, "DEF CON",     "ice1.somafm.com",                80, "/defcon-128-mp3",      .logo = logo_defcon },
};

/* Preset 4: USB music */
static const source_t p4_sources[] = {
    { SRC_DIR, "USB Music", .dir = "/media/sda1", .logo = logo_usb },
};

typedef struct {
    const source_t *sources;
    int num_sources;
    int current;
} preset_t;

static preset_t presets[NUM_PRESETS] = {
    { p1_sources, sizeof(p1_sources)/sizeof(p1_sources[0]), 0 },
    { p2_sources, sizeof(p2_sources)/sizeof(p2_sources[0]), 0 },
    { p3_sources, sizeof(p3_sources)/sizeof(p3_sources[0]), 0 },
    { p4_sources, sizeof(p4_sources)/sizeof(p4_sources[0]), 0 },
    { NULL, 0, 0 },
    { NULL, 0, 0 },
};

/* ── Track info ────────────────────────────────────────────────── */

typedef struct {
    char path[MAX_PATH_LEN];
    char filename[MAX_PATH_LEN];
    char title[MAX_TAG];
    char artist[MAX_TAG];
    char album[MAX_TAG];
} track_t;

/* ── Player state ──────────────────────────────────────────────── */

static struct {
    shelbourne_t *hw;
    int active_preset;   /* 0-5, or -1 for AUX */

    /* USB */
    track_t tracks[MAX_FILES];
    int num_tracks;
    int current_track;
    const char *usb_dir;  /* directory being played */

    /* Radio */
    int radio_fd;
    int icy_metaint;         /* bytes between metadata blocks (0 = no ICY) */
    int icy_remaining;       /* bytes until next metadata block */
    char stream_title[256];  /* current ICY StreamTitle */

    /* Audio */
    int playing;

    /* Volume */
    int volume;
    int vol_gain;
    struct timespec vol_changed_at;
    int vol_bar_active;

    /* Commands */
    volatile int cmd_quit;
    volatile int cmd_pause;
    volatile int cmd_aux;
    volatile int cmd_preset;  /* 0-5, or -1 for none */

    /* Display */
    int display_dirty;
    struct timespec logo_shown_at;  /* when logo was first displayed */
    int logo_expired;               /* set once after 5s logo timeout */
} state;

/* ── Resampler ─────────────────────────────────────────────────── */

#define OUTBUF_FRAMES (SHELBOURNE_FRAMES_PER_WRITE * 4)
static int32_t outbuf[OUTBUF_FRAMES * 2];
static int outbuf_fill = 0;
static double resample_frac = 0.0;

static void audio_reset(void)
{
    outbuf_fill = 0;
    resample_frac = 0.0;
    shelbourne_audio_reset(state.hw);
}

static int convert_and_write(const mp3d_sample_t *pcm,
                             int in_frames, int channels, int sample_rate)
{
    if (in_frames == 0) return 0;
    double step = (double)sample_rate / SHELBOURNE_SAMPLE_RATE;

    while (resample_frac < in_frames - 1) {
        int idx = (int)resample_frac;
        double frac = resample_frac - idx;
        for (int ch = 0; ch < 2; ch++) {
            int src_ch = (ch < channels) ? ch : 0;
            int32_t s0 = pcm[idx * channels + src_ch];
            int32_t s1 = pcm[(idx + 1) * channels + src_ch];
            int32_t s = (int32_t)(s0 + frac * (s1 - s0));
            outbuf[outbuf_fill * 2 + ch] =
                (int32_t)(((int64_t)(s << 16) * state.vol_gain) >> 16);
        }
        outbuf_fill++;
        resample_frac += step;

        if (outbuf_fill >= OUTBUF_FRAMES) {
            if (shelbourne_audio_write(state.hw, outbuf, outbuf_fill) < 0)
                return -1;
            if (shelbourne_audio_wait(state.hw) < 0)
                return -1;
            outbuf_fill = 0;
        }
    }
    resample_frac -= in_frames;
    if (resample_frac < 0) resample_frac = 0;

    if (outbuf_fill > 0) {
        if (shelbourne_audio_write(state.hw, outbuf, outbuf_fill) < 0)
            return -1;
        outbuf_fill = 0;
    }
    return 0;
}

/* ── 5x7 bitmap font ──────────────────────────────────────────── */

#define FONT_W 5
#define FONT_H 7
#define CHAR_W 6
#define CHAR_H 8
#define DISP_COLS (SHELBOURNE_FB_WIDTH / CHAR_W)

/* 2x scaled font metrics */
#define BIG_CHAR_W 12
#define BIG_CHAR_H 16
#define BIG_COLS   (SHELBOURNE_FB_WIDTH / BIG_CHAR_W)  /* 10 */

static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5f,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7f,0x14,0x7f,0x14}, /* # */
    {0x24,0x2a,0x7f,0x2a,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1c,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1c,0x00}, /* ) */
    {0x14,0x08,0x3e,0x08,0x14}, /* * */
    {0x08,0x08,0x3e,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3e,0x51,0x49,0x45,0x3e}, /* 0 */
    {0x00,0x42,0x7f,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4b,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7f,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3c,0x4a,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1e}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3e}, /* @ */
    {0x7e,0x11,0x11,0x11,0x7e}, /* A */
    {0x7f,0x49,0x49,0x49,0x36}, /* B */
    {0x3e,0x41,0x41,0x41,0x22}, /* C */
    {0x7f,0x41,0x41,0x22,0x1c}, /* D */
    {0x7f,0x49,0x49,0x49,0x41}, /* E */
    {0x7f,0x09,0x09,0x09,0x01}, /* F */
    {0x3e,0x41,0x49,0x49,0x7a}, /* G */
    {0x7f,0x08,0x08,0x08,0x7f}, /* H */
    {0x00,0x41,0x7f,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3f,0x01}, /* J */
    {0x7f,0x08,0x14,0x22,0x41}, /* K */
    {0x7f,0x40,0x40,0x40,0x40}, /* L */
    {0x7f,0x02,0x0c,0x02,0x7f}, /* M */
    {0x7f,0x04,0x08,0x10,0x7f}, /* N */
    {0x3e,0x41,0x41,0x41,0x3e}, /* O */
    {0x7f,0x09,0x09,0x09,0x06}, /* P */
    {0x3e,0x41,0x51,0x21,0x5e}, /* Q */
    {0x7f,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7f,0x01,0x01}, /* T */
    {0x3f,0x40,0x40,0x40,0x3f}, /* U */
    {0x1f,0x20,0x40,0x20,0x1f}, /* V */
    {0x3f,0x40,0x38,0x40,0x3f}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7f,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7f,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7f,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7f}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7e,0x09,0x01,0x02}, /* f */
    {0x0c,0x52,0x52,0x52,0x3e}, /* g */
    {0x7f,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7d,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3d,0x00}, /* j */
    {0x7f,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7f,0x40,0x00}, /* l */
    {0x7c,0x04,0x18,0x04,0x78}, /* m */
    {0x7c,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7c,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7c}, /* q */
    {0x7c,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3f,0x44,0x40,0x20}, /* t */
    {0x3c,0x40,0x40,0x20,0x7c}, /* u */
    {0x1c,0x20,0x40,0x20,0x1c}, /* v */
    {0x3c,0x40,0x30,0x40,0x3c}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0c,0x50,0x50,0x50,0x3c}, /* y */
    {0x44,0x64,0x54,0x4c,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7f,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x10,0x08,0x08,0x10,0x08}, /* ~ */
};

/* ── Text drawing ──────────────────────────────────────────────── */

static void fb_putchar(int x, int y, char c)
{
    uint8_t *fb = shelbourne_display_buffer(state.hw);
    if (!fb) return;
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < FONT_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            int px = x + col, py = y + row;
            if (px >= 0 && px < SHELBOURNE_FB_WIDTH &&
                py >= 0 && py < SHELBOURNE_FB_HEIGHT)
                fb[py * SHELBOURNE_FB_WIDTH + px] = (bits & (1 << row)) ? 0xFF : 0x00;
        }
    }
}

static void fb_putstr(int x, int y, const char *s)
{
    for (int i = 0; s[i] && x + CHAR_W <= SHELBOURNE_FB_WIDTH; i++) {
        fb_putchar(x, y, s[i]);
        x += CHAR_W;
    }
}

static void fb_center(int y, const char *s)
{
    int len = strlen(s);
    if (len > DISP_COLS) len = DISP_COLS;
    int x = (SHELBOURNE_FB_WIDTH - len * CHAR_W) / 2;
    fb_putstr(x, y, s);
}

/* 2x scaled text */
static void fb_putchar_2x(int x, int y, char c)
{
    uint8_t *fb = shelbourne_display_buffer(state.hw);
    if (!fb) return;
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < FONT_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            uint8_t val = (bits & (1 << row)) ? 0xFF : 0x00;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int px = x + col * 2 + dx, py = y + row * 2 + dy;
                    if (px >= 0 && px < SHELBOURNE_FB_WIDTH &&
                        py >= 0 && py < SHELBOURNE_FB_HEIGHT)
                        fb[py * SHELBOURNE_FB_WIDTH + px] = val;
                }
        }
    }
}

static void fb_putstr_2x(int x, int y, const char *s)
{
    for (int i = 0; s[i] && x + BIG_CHAR_W <= SHELBOURNE_FB_WIDTH; i++) {
        fb_putchar_2x(x, y, s[i]);
        x += BIG_CHAR_W;
    }
}

static void fb_center_2x(int y, const char *s)
{
    int len = strlen(s);
    if (len > BIG_COLS) len = BIG_COLS;
    int x = (SHELBOURNE_FB_WIDTH - len * BIG_CHAR_W) / 2;
    fb_putstr_2x(x, y, s);
}

/* ── Volume bar ────────────────────────────────────────────────── */

static void draw_volume_bar(void)
{
    uint8_t *fb = shelbourne_display_buffer(state.hw);
    if (!fb) return;

    int bar_y = SHELBOURNE_FB_HEIGHT - 14;
    int bar_x = 4;
    int bar_max_w = SHELBOURNE_FB_WIDTH - 32;
    int fill_w = (state.volume * bar_max_w) / 100;

    /* Clear bar area */
    for (int y = bar_y; y < SHELBOURNE_FB_HEIGHT; y++)
        memset(&fb[y * SHELBOURNE_FB_WIDTH], 0, SHELBOURNE_FB_WIDTH);

    /* Outline */
    for (int x = bar_x; x < bar_x + bar_max_w; x++) {
        fb[bar_y * SHELBOURNE_FB_WIDTH + x] = 0x30;
        fb[(SHELBOURNE_FB_HEIGHT - 3) * SHELBOURNE_FB_WIDTH + x] = 0x30;
    }
    for (int y = bar_y; y <= SHELBOURNE_FB_HEIGHT - 3; y++) {
        fb[y * SHELBOURNE_FB_WIDTH + bar_x] = 0x30;
        fb[y * SHELBOURNE_FB_WIDTH + bar_x + bar_max_w - 1] = 0x30;
    }

    /* Fill */
    for (int y = bar_y + 2; y < SHELBOURNE_FB_HEIGHT - 4; y++)
        for (int x = bar_x + 2; x < bar_x + 2 + fill_w; x++)
            fb[y * SHELBOURNE_FB_WIDTH + x] = 0xFF;

    /* Volume number */
    char vol[4];
    snprintf(vol, sizeof(vol), "%d", state.volume);
    fb_putstr(SHELBOURNE_FB_WIDTH - (int)strlen(vol) * CHAR_W - 2, bar_y + 3, vol);
}

static int vol_bar_visible(void)
{
    if (!state.vol_bar_active) return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - state.vol_changed_at.tv_sec) * 1000L +
              (now.tv_nsec - state.vol_changed_at.tv_nsec) / 1000000;
    if (ms >= VOL_BAR_MS) {
        state.vol_bar_active = 0;
        return 0;
    }
    return 1;
}

/* ── Display ───────────────────────────────────────────────────── */

static const source_t *active_source(void)
{
    if (state.active_preset < 0 || state.active_preset >= NUM_PRESETS)
        return NULL;
    preset_t *p = &presets[state.active_preset];
    if (p->num_sources == 0) return NULL;
    return &p->sources[p->current];
}

static void display_logo(const uint8_t *logo)
{
    uint8_t *fb = shelbourne_display_buffer(state.hw);
    if (fb && logo)
        memcpy(fb, logo, SHELBOURNE_FB_SIZE);
}

static void display_update(void)
{
    shelbourne_display_clear(state.hw);
    const source_t *src = active_source();

    if (state.active_preset < 0) {
        /* AUX mode — full-screen logo */
        display_logo(logo_aux);

    } else if (src && src->type == SRC_RADIO) {
        /* Radio mode — blank when paused, logo for first 5s, then metadata */
        if (!state.playing) {
            /* Paused/stopped — blank screen (already cleared above) */
        } else {
            int show_meta = 0;
            if (state.stream_title[0]) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long ms = (now.tv_sec - state.logo_shown_at.tv_sec) * 1000L +
                          (now.tv_nsec - state.logo_shown_at.tv_nsec) / 1000000;
                if (ms >= 5000) show_meta = 1;
            }

            if (show_meta) {
                /* Station name at top in big font */
                fb_center_2x(2, src->name);

                /* Stream title — word-wrap across up to 4 lines */
                const char *title = state.stream_title;
                int y = 28;
                while (*title && y < 80) {
                    int len = strlen(title);
                    int line_chars = len < DISP_COLS ? len : DISP_COLS;
                    /* Break at last space within line width */
                    if (len > DISP_COLS) {
                        int brk = DISP_COLS;
                        while (brk > 0 && title[brk] != ' ') brk--;
                        if (brk > 0) line_chars = brk;
                    }
                    char line[DISP_COLS + 1];
                    memcpy(line, title, line_chars);
                    line[line_chars] = '\0';
                    fb_center(y, line);
                    title += line_chars;
                    while (*title == ' ') title++;
                    y += CHAR_H + 2;
                }
            } else if (src->logo) {
                display_logo(src->logo);
            } else {
                fb_center_2x(18, src->name);
            }
        }

    } else if (src && src->type == SRC_DIR) {
        /* USB mode — blank when paused */
        if (!state.playing) {
            /* Paused — blank screen (already cleared above) */
        } else if (state.num_tracks > 0) {
            char hdr[8];
            snprintf(hdr, sizeof(hdr), "P%d", state.active_preset + 1);
            fb_putstr(0, 2, hdr);

            char idx[12];
            snprintf(idx, sizeof(idx), "%d/%d",
                     state.current_track + 1, state.num_tracks);
            fb_putstr(SHELBOURNE_FB_WIDTH - (int)strlen(idx) * CHAR_W, 2, idx);

            track_t *t = &state.tracks[state.current_track];

            /* Title in big font (truncate to 10 chars) */
            char title[BIG_COLS + 1];
            snprintf(title, sizeof(title), "%.*s", BIG_COLS, t->title);
            fb_center_2x(18, title);

            if (t->artist[0]) {
                char line[DISP_COLS + 1];
                snprintf(line, sizeof(line), "%.*s", DISP_COLS, t->artist);
                fb_center(40, line);
            }
            if (t->album[0]) {
                char line[DISP_COLS + 1];
                snprintf(line, sizeof(line), "%.*s", DISP_COLS, t->album);
                fb_center(50, line);
            }
        } else {
            fb_center_2x(18, "No tracks");
        }
    }

    if (vol_bar_visible())
        draw_volume_bar();

    shelbourne_display_flush(state.hw);
}

/* ── Utility ───────────────────────────────────────────────────── */

static void trim(char *s)
{
    int len = strlen(s);
    while (len > 0 && (unsigned char)s[len-1] <= ' ')
        s[--len] = '\0';
}

static int ends_with_ci(const char *s, const char *suffix)
{
    int slen = strlen(s), xlen = strlen(suffix);
    if (slen < xlen) return 0;
    for (int i = 0; i < xlen; i++) {
        char a = s[slen - xlen + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* ── DNS resolver ──────────────────────────────────────────────── */

static int dns_resolve(const char *hostname, struct in_addr *out)
{
    if (inet_aton(hostname, out))
        return 0;

    struct in_addr ns_addr;
    inet_aton("192.168.2.1", &ns_addr);

    FILE *rc = fopen("/etc/resolv.conf", "r");
    if (rc) {
        char line[256];
        while (fgets(line, sizeof(line), rc)) {
            char ip[64];
            if (sscanf(line, "nameserver %63s", ip) == 1) {
                inet_aton(ip, &ns_addr);
                break;
            }
        }
        fclose(rc);
    }

    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x12; pkt[1] = 0x34;
    pkt[2] = 0x01; pkt[3] = 0x00;
    pkt[4] = 0x00; pkt[5] = 0x01;
    int off = 12;

    const char *p = hostname;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (off + 1 + len >= (int)sizeof(pkt) - 5) return -1;
        pkt[off++] = len;
        memcpy(pkt + off, p, len);
        off += len;
        p += len + (*p && dot ? 1 : 0);
    }
    pkt[off++] = 0;
    pkt[off++] = 0; pkt[off++] = 1;
    pkt[off++] = 0; pkt[off++] = 1;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct timeval tv = {.tv_sec = 3};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in ns = {0};
    ns.sin_family = AF_INET;
    ns.sin_port = htons(53);
    ns.sin_addr = ns_addr;

    sendto(sock, pkt, off, 0, (struct sockaddr *)&ns, sizeof(ns));

    uint8_t resp[512];
    int rlen = recv(sock, resp, sizeof(resp), 0);
    close(sock);
    if (rlen < 12) return -1;

    if (resp[0] != 0x12 || resp[1] != 0x34) return -1;
    if (!(resp[2] & 0x80)) return -1;
    if ((resp[3] & 0x0F) != 0) return -1;

    int ancount = (resp[6] << 8) | resp[7];
    if (ancount == 0) return -1;

    int roff = 12;
    while (roff < rlen && resp[roff] != 0) {
        if ((resp[roff] & 0xC0) == 0xC0) { roff += 2; break; }
        roff += 1 + resp[roff];
    }
    if (roff < rlen && resp[roff] == 0) roff++;
    roff += 4;

    for (int i = 0; i < ancount && roff < rlen; i++) {
        while (roff < rlen) {
            if ((resp[roff] & 0xC0) == 0xC0) { roff += 2; break; }
            if (resp[roff] == 0) { roff++; break; }
            roff += 1 + resp[roff];
        }
        if (roff + 10 > rlen) return -1;
        uint16_t rtype = (resp[roff] << 8) | resp[roff+1];
        uint16_t rdlen = (resp[roff+8] << 8) | resp[roff+9];
        roff += 10;
        if (rtype == 1 && rdlen == 4) {
            memcpy(out, resp + roff, 4);
            return 0;
        }
        roff += rdlen;
    }
    return -1;
}

/* ── ID3 tag reading ───────────────────────────────────────────── */

static int read_id3v1(const char *path, track_t *t)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, -128, SEEK_END);
    unsigned char buf[128];
    if (fread(buf, 1, 128, f) != 128) { fclose(f); return -1; }
    fclose(f);
    if (buf[0] != 'T' || buf[1] != 'A' || buf[2] != 'G') return -1;
    memcpy(t->title, buf + 3, 30); t->title[30] = '\0'; trim(t->title);
    memcpy(t->artist, buf + 33, 30); t->artist[30] = '\0'; trim(t->artist);
    memcpy(t->album, buf + 63, 30); t->album[30] = '\0'; trim(t->album);
    return 0;
}

static int read_id3v2(const char *path, track_t *t)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char hdr[10];
    if (fread(hdr, 1, 10, f) != 10) { fclose(f); return -1; }
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        fclose(f); return -1;
    }

    int version = hdr[3];
    uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                        ((uint32_t)(hdr[7] & 0x7F) << 14) |
                        ((uint32_t)(hdr[8] & 0x7F) << 7) |
                        ((uint32_t)(hdr[9] & 0x7F));
    if (tag_size > 1024 * 1024) tag_size = 1024 * 1024;

    unsigned char *data = malloc(tag_size);
    if (!data) { fclose(f); return -1; }
    size_t got = fread(data, 1, tag_size, f);
    fclose(f);
    if (got < 10) { free(data); return -1; }

    int fhdr_size = (version == 2) ? 6 : 10;
    size_t pos = 0;

    while (pos + fhdr_size < got) {
        char frame_id[5] = {0};
        uint32_t frame_size;

        if (version == 2) {
            memcpy(frame_id, data + pos, 3);
            frame_size = ((uint32_t)data[pos+3] << 16) |
                         ((uint32_t)data[pos+4] << 8) |
                         (uint32_t)data[pos+5];
        } else {
            memcpy(frame_id, data + pos, 4);
            if (version == 4) {
                frame_size = ((uint32_t)(data[pos+4] & 0x7F) << 21) |
                             ((uint32_t)(data[pos+5] & 0x7F) << 14) |
                             ((uint32_t)(data[pos+6] & 0x7F) << 7) |
                             ((uint32_t)(data[pos+7] & 0x7F));
            } else {
                frame_size = ((uint32_t)data[pos+4] << 24) |
                             ((uint32_t)data[pos+5] << 16) |
                             ((uint32_t)data[pos+6] << 8) |
                             (uint32_t)data[pos+7];
            }
        }

        if (frame_id[0] == '\0' || frame_size == 0) break;
        if (pos + fhdr_size + frame_size > got) break;

        unsigned char *fdata = data + pos + fhdr_size;
        char *dst = NULL;
        if ((version == 2 && strcmp(frame_id, "TT2") == 0) ||
            strcmp(frame_id, "TIT2") == 0) dst = t->title;
        else if ((version == 2 && strcmp(frame_id, "TP1") == 0) ||
                 strcmp(frame_id, "TPE1") == 0) dst = t->artist;
        else if ((version == 2 && strcmp(frame_id, "TAL") == 0) ||
                 strcmp(frame_id, "TALB") == 0) dst = t->album;

        if (dst && frame_size > 1) {
            int enc = fdata[0];
            if (enc == 1 || enc == 2) {
                int start = 1, text_len = frame_size - 1;
                if (text_len >= 2 && ((fdata[1] == 0xFF && fdata[2] == 0xFE) ||
                                       (fdata[1] == 0xFE && fdata[2] == 0xFF))) {
                    start = 3; text_len -= 2;
                }
                int le = (start >= 3 && fdata[1] == 0xFF);
                int j = 0;
                for (int k = 0; k < text_len - 1 && j < MAX_TAG - 1; k += 2) {
                    unsigned char lo = fdata[start + k + (le ? 0 : 1)];
                    unsigned char hi = fdata[start + k + (le ? 1 : 0)];
                    if (hi == 0 && lo >= 0x20 && lo < 0x7F) dst[j++] = lo;
                    else if (hi == 0 && lo == 0) break;
                }
                dst[j] = '\0';
            } else {
                int n = frame_size - 1;
                if (n > MAX_TAG - 1) n = MAX_TAG - 1;
                memcpy(dst, fdata + 1, n);
                dst[n] = '\0';
                trim(dst);
            }
        }

        pos += fhdr_size + frame_size;
    }

    free(data);
    return (t->title[0] || t->artist[0] || t->album[0]) ? 0 : -1;
}

static void read_tags(track_t *t)
{
    if (read_id3v2(t->path, t) != 0)
        read_id3v1(t->path, t);
    if (!t->title[0]) {
        strncpy(t->title, t->filename, MAX_TAG - 1);
        char *dot = strrchr(t->title, '.');
        if (dot) *dot = '\0';
    }
}

/* ── File scanning ─────────────────────────────────────────────── */

static void scan_recursive(const char *dir, int depth)
{
    if (depth > 5) return;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && state.num_tracks < MAX_FILES) {
        if (ent->d_name[0] == '.') continue;

        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_recursive(path, depth + 1);
        } else if (ends_with_ci(ent->d_name, ".mp3")) {
            track_t *t = &state.tracks[state.num_tracks];
            strncpy(t->path, path, MAX_PATH_LEN - 1);
            strncpy(t->filename, ent->d_name, MAX_PATH_LEN - 1);
            t->title[0] = t->artist[0] = t->album[0] = '\0';
            read_tags(t);
            state.num_tracks++;
        }
    }
    closedir(d);
}

static void scan_dir(const char *dir)
{
    if (state.usb_dir && strcmp(state.usb_dir, dir) == 0)
        return;  /* already scanned */

    state.num_tracks = 0;
    scan_recursive(dir, 0);

    /* Sort by path */
    for (int i = 0; i < state.num_tracks - 1; i++)
        for (int j = i + 1; j < state.num_tracks; j++)
            if (strcmp(state.tracks[i].path, state.tracks[j].path) > 0) {
                track_t tmp = state.tracks[i];
                state.tracks[i] = state.tracks[j];
                state.tracks[j] = tmp;
            }

    state.usb_dir = dir;
    state.current_track = 0;
    printf("Found %d MP3 track(s) in %s\n", state.num_tracks, dir);
}

/* ── Keypad ────────────────────────────────────────────────────── */

static void keypad_poll(void)
{
    shelbourne_key_event_t events[16];
    int n = shelbourne_keypad_poll(state.hw, events, 16);

    for (int i = 0; i < n; i++) {
        if (!events[i].pressed) continue;

        switch (events[i].key) {
        case SHELBOURNE_KEY_POWER:
            state.cmd_pause = 1;
            break;
        case SHELBOURNE_KEY_VOLUME_UP:
            if (state.volume < 100) {
                state.volume += VOL_STEP;
                if (state.volume > 100) state.volume = 100;
                state.vol_gain = shelbourne_volume_to_gain(state.volume);
                clock_gettime(CLOCK_MONOTONIC, &state.vol_changed_at);
                state.vol_bar_active = 1;
                state.display_dirty = 1;
                printf("Volume: %d\n", state.volume);
            }
            break;
        case SHELBOURNE_KEY_VOLUME_DOWN:
            if (state.volume > MIN_VOL) {
                state.volume -= VOL_STEP;
                if (state.volume < MIN_VOL) state.volume = MIN_VOL;
                state.vol_gain = shelbourne_volume_to_gain(state.volume);
                clock_gettime(CLOCK_MONOTONIC, &state.vol_changed_at);
                state.vol_bar_active = 1;
                state.display_dirty = 1;
                printf("Volume: %d\n", state.volume);
            }
            break;
        case SHELBOURNE_KEY_PRESET_1: state.cmd_preset = 0; break;
        case SHELBOURNE_KEY_PRESET_2: state.cmd_preset = 1; break;
        case SHELBOURNE_KEY_PRESET_3: state.cmd_preset = 2; break;
        case SHELBOURNE_KEY_PRESET_4: state.cmd_preset = 3; break;
        case SHELBOURNE_KEY_PRESET_5: state.cmd_preset = 4; break;
        case SHELBOURNE_KEY_PRESET_6: state.cmd_preset = 5; break;
        case SHELBOURNE_KEY_AUX:
            state.cmd_aux = 1;
            break;
        default:
            break;
        }
    }

    /* Auto-hide volume bar */
    if (state.vol_bar_active && !vol_bar_visible()) {
        state.display_dirty = 1;
    }
}

/* ── Interrupt check macro ─────────────────────────────────────── */

#define INTERRUPTED() \
    (state.cmd_quit || state.cmd_aux || state.cmd_preset >= 0)

/* ── Radio streaming ───────────────────────────────────────────── */

static int radio_connect(const source_t *src)
{
    char host[256], path[512];
    int port;
    strncpy(host, src->host, sizeof(host) - 1); host[sizeof(host) - 1] = '\0';
    strncpy(path, src->path, sizeof(path) - 1); path[sizeof(path) - 1] = '\0';
    port = src->port;

    struct timeval tv = {.tv_sec = 10};

    for (int redirects = 0; redirects < 5; redirects++) {
        struct in_addr resolved;
        if (dns_resolve(host, &resolved) < 0) {
            fprintf(stderr, "DNS lookup failed for %s\n", host);
            return -1;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr = resolved;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "Connect to %s:%d failed: %s\n",
                    host, port, strerror(errno));
            close(fd);
            return -1;
        }

        char req[1024];
        int len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: ShelbournePlayer/1.0\r\n"
            "Icy-MetaData: 1\r\n"
            "Accept: */*\r\n"
            "\r\n",
            path, host);

        if (send(fd, req, len, 0) != len) { close(fd); return -1; }

        /* Read HTTP headers */
        char hdr[4096];
        int hlen = 0;
        while (hlen < (int)sizeof(hdr) - 1) {
            char c;
            if (recv(fd, &c, 1, 0) != 1) break;
            hdr[hlen++] = c;
            if (hlen >= 4 &&
                hdr[hlen-4] == '\r' && hdr[hlen-3] == '\n' &&
                hdr[hlen-2] == '\r' && hdr[hlen-1] == '\n')
                break;
        }
        hdr[hlen] = '\0';

        /* Check for redirect */
        if (!strstr(hdr, " 301") && !strstr(hdr, " 302")) {
            /* Parse icy-metaint for metadata stripping */
            state.icy_metaint = 0;
            char *mi = strstr(hdr, "icy-metaint:");
            if (mi) {
                state.icy_metaint = atoi(mi + 12);
                printf("ICY metaint: %d\n", state.icy_metaint);
            }
            state.icy_remaining = state.icy_metaint;
            state.stream_title[0] = '\0';
            printf("Radio connected: %s\n", src->name);
            return fd;
        }

        char *loc = strstr(hdr, "Location: ");
        if (!loc) loc = strstr(hdr, "location: ");
        if (!loc) {
            printf("Radio connected: %s\n", src->name);
            return fd;
        }
        loc += 10;
        char *end = strstr(loc, "\r\n");
        if (end) *end = '\0';
        printf("Redirect: %s\n", loc);
        close(fd);

        if (loc[0] == '/') {
            /* Relative redirect — same host, new path */
            strncpy(path, loc, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            /* Absolute redirect — parse host/port/path */
            if (strncmp(loc, "http://", 7) == 0) { loc += 7; port = 80; }
            else if (strncmp(loc, "https://", 8) == 0) { loc += 8; port = 443; }
            else port = 80;

            char *slash = strchr(loc, '/');
            if (slash) {
                strncpy(path, slash, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
                *slash = '\0';
            } else {
                strcpy(path, "/");
            }
            char *colon = strchr(loc, ':');
            if (colon) {
                *colon = '\0';
                port = atoi(colon + 1);
            }
            strncpy(host, loc, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
    }

    fprintf(stderr, "Too many redirects for %s\n", src->name);
    return -1;
}

/* ── ICY metadata stripping ────────────────────────────────────── */

static void parse_icy_metadata(const char *meta, int meta_len)
{
    /* Format: StreamTitle='Artist - Song';StreamUrl=''; */
    const char *p = strstr(meta, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *end = strchr(p, '\'');
    if (!end) return;
    int len = end - p;
    if (len <= 0) return;
    if (len > (int)sizeof(state.stream_title) - 1)
        len = sizeof(state.stream_title) - 1;
    if (len == (int)strlen(state.stream_title) &&
        memcmp(state.stream_title, p, len) == 0)
        return;  /* unchanged */
    memcpy(state.stream_title, p, len);
    state.stream_title[len] = '\0';
    printf("ICY title: %s\n", state.stream_title);
    state.display_dirty = 1;
}

/*
 * Blocking read of exactly `len` bytes. Used for ICY metadata which is
 * tiny (1-257 bytes) and immediately follows audio data in the TCP stream.
 * Temporarily clears O_NONBLOCK if needed.
 */
static int recv_exact(int fd, void *buf, int len)
{
    int flags = fcntl(fd, F_GETFL);
    int was_nonblock = flags & O_NONBLOCK;
    if (was_nonblock)
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    int got = 0;
    while (got < len) {
        int n = recv(fd, (uint8_t *)buf + got, len - got, 0);
        if (n <= 0) {
            if (was_nonblock) fcntl(fd, F_SETFL, flags);
            return -1;
        }
        got += n;
    }

    if (was_nonblock)
        fcntl(fd, F_SETFL, flags);
    return got;
}

/*
 * Read from stream socket, stripping ICY metadata blocks.
 * Returns bytes of audio data written to buf, or <=0 on error/EOF.
 * Safe with both blocking and non-blocking sockets.
 */
static int icy_recv(int fd, uint8_t *buf, int maxlen)
{
    if (state.icy_metaint == 0)
        return recv(fd, buf, maxlen, 0);

    /* Only read up to the next metadata boundary */
    int want = state.icy_remaining;
    if (want > maxlen) want = maxlen;

    int n = recv(fd, buf, want, 0);
    if (n <= 0) return n;
    state.icy_remaining -= n;

    /* If we hit the metadata boundary, consume the metadata block */
    if (state.icy_remaining == 0) {
        uint8_t mlen_byte;
        if (recv_exact(fd, &mlen_byte, 1) < 0)
            return n;  /* return audio we got; metadata read fails next time */

        int meta_len = mlen_byte * 16;
        if (meta_len > 0) {
            char meta[4096];
            if (recv_exact(fd, meta, meta_len) >= 0) {
                meta[meta_len] = '\0';
                parse_icy_metadata(meta, meta_len);
            }
        }
        state.icy_remaining = state.icy_metaint;
    }

    return n;
}

static void play_radio(const source_t *src)
{
    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    uint8_t stream_buf[MP3_BUF_SIZE * 2];
    int buf_fill = 0;

    while (!state.cmd_quit && !state.cmd_aux && state.cmd_preset < 0) {
        printf("Connecting to %s...\n", src->name);
        display_update();

        int sock = radio_connect(src);
        if (sock < 0) {
            fprintf(stderr, "Failed, retrying in 3s...\n");
            for (int w = 0; w < 30 && !INTERRUPTED(); w++) {
                usleep(100000);
                keypad_poll();
            }
            continue;
        }

        audio_reset();
        state.radio_fd = sock;
        state.stream_title[0] = '\0';
        buf_fill = 0;

        /* Pre-buffer */
        printf("Buffering...\n");
        display_update();
        {
            struct timeval tv_buf = {.tv_sec = 5};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_buf, sizeof(tv_buf));
            while (buf_fill < MP3_BUF_SIZE) {
                int n = icy_recv(sock, stream_buf + buf_fill,
                                 MP3_BUF_SIZE - buf_fill);
                if (n <= 0) break;
                buf_fill += n;
            }
        }
        printf("Buffered %d bytes\n", buf_fill);
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);

        state.playing = 1;
        state.logo_expired = 0;
        clock_gettime(CLOCK_MONOTONIC, &state.logo_shown_at);
        display_update();

        while (!INTERRUPTED()) {
            keypad_poll();

            /* Handle pause */
            while (state.cmd_pause && !INTERRUPTED()) {
                state.cmd_pause = 0;
                state.playing = 0;
                display_update();
                while (!state.cmd_pause && !INTERRUPTED()) {
                    usleep(50000);
                    keypad_poll();
                    if (state.display_dirty) {
                        state.display_dirty = 0;
                        display_update();
                    }
                }
                if (state.cmd_pause) {
                    state.cmd_pause = 0;
                    state.playing = 1;
                    audio_reset();
                    display_update();
                }
            }
            if (INTERRUPTED()) break;

            /* Top up buffer */
            if (buf_fill < (int)sizeof(stream_buf)) {
                int n = icy_recv(sock, stream_buf + buf_fill,
                                 sizeof(stream_buf) - buf_fill);
                if (n > 0) {
                    buf_fill += n;
                } else if (n == 0) {
                    fprintf(stderr, "Stream ended\n");
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "Stream error: %s\n", strerror(errno));
                    break;
                }
            }

            /* Decode frames */
            mp3dec_frame_info_t info;
            mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

            while (buf_fill >= 4 && !INTERRUPTED() && !state.cmd_pause) {
                int samples = mp3dec_decode_frame(&mp3d,
                    stream_buf, buf_fill, pcm, &info);

                if (info.frame_bytes > 0) {
                    buf_fill -= info.frame_bytes;
                    memmove(stream_buf, stream_buf + info.frame_bytes, buf_fill);

                    if (samples > 0 && info.hz > 0)
                        convert_and_write(pcm, samples, info.channels, info.hz);

                    shelbourne_audio_wait(state.hw);

                    if (buf_fill < MP3_BUF_SIZE) {
                        int n = icy_recv(sock, stream_buf + buf_fill,
                                         sizeof(stream_buf) - buf_fill);
                        if (n > 0) buf_fill += n;
                    }

                    keypad_poll();

                    /* Trigger display refresh when logo timeout expires */
                    if (!state.logo_expired && state.stream_title[0]) {
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        long ms = (now.tv_sec - state.logo_shown_at.tv_sec) * 1000L +
                                  (now.tv_nsec - state.logo_shown_at.tv_nsec) / 1000000;
                        if (ms >= 5000) {
                            state.logo_expired = 1;
                            state.display_dirty = 1;
                        }
                    }

                    if (state.display_dirty) {
                        state.display_dirty = 0;
                        display_update();
                    }
                } else if (buf_fill > MP3_BUF_SIZE) {
                    memmove(stream_buf, stream_buf + 1, buf_fill - 1);
                    buf_fill--;
                } else {
                    break;
                }
            }
        }

        close(sock);
        state.radio_fd = -1;
        state.playing = 0;
    }
}

/* ── USB playback ──────────────────────────────────────────────── */

static void play_usb(const source_t *src)
{
    scan_dir(src->dir);

    while (!INTERRUPTED() && state.num_tracks > 0) {
        track_t *t = &state.tracks[state.current_track];
        printf("[%d/%d] %s\n", state.current_track + 1, state.num_tracks,
               t->title);

        state.playing = 1;
        display_update();

        FILE *f = fopen(t->path, "rb");
        if (!f) {
            fprintf(stderr, "Cannot open %s\n", t->path);
            state.current_track = (state.current_track + 1) % state.num_tracks;
            continue;
        }

        mp3dec_t mp3d;
        mp3dec_init(&mp3d);
        audio_reset();

        uint8_t file_buf[MP3_BUF_SIZE * 2];
        int buf_fill = 0;

        while (!INTERRUPTED()) {
            /* Handle pause */
            while (state.cmd_pause && !INTERRUPTED()) {
                state.cmd_pause = 0;
                state.playing = 0;
                display_update();
                while (!state.cmd_pause && !INTERRUPTED()) {
                    usleep(50000);
                    keypad_poll();
                    if (state.display_dirty) {
                        state.display_dirty = 0;
                        display_update();
                    }
                }
                if (state.cmd_pause) {
                    state.cmd_pause = 0;
                    state.playing = 1;
                    audio_reset();
                    display_update();
                }
            }
            if (INTERRUPTED()) break;

            /* Refill buffer */
            if (buf_fill < MP3_BUF_SIZE) {
                int n = fread(file_buf + buf_fill, 1,
                              sizeof(file_buf) - buf_fill, f);
                if (n <= 0) break;  /* end of track */
                buf_fill += n;
            }

            /* Decode */
            mp3dec_frame_info_t info;
            mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

            int samples = mp3dec_decode_frame(&mp3d,
                file_buf, buf_fill, pcm, &info);

            if (info.frame_bytes == 0) break;

            buf_fill -= info.frame_bytes;
            memmove(file_buf, file_buf + info.frame_bytes, buf_fill);

            if (samples > 0 && info.hz > 0)
                convert_and_write(pcm, samples, info.channels, info.hz);

            shelbourne_audio_wait(state.hw);
            keypad_poll();

            if (state.display_dirty) {
                state.display_dirty = 0;
                display_update();
            }
        }

        shelbourne_audio_drain(state.hw);
        fclose(f);
        state.playing = 0;

        if (!INTERRUPTED()) {
            /* Advance to next track */
            state.current_track = (state.current_track + 1) % state.num_tracks;
        }
    }

    if (state.num_tracks == 0) {
        display_update();
        while (!INTERRUPTED()) {
            usleep(100000);
            keypad_poll();
            if (state.display_dirty) {
                state.display_dirty = 0;
                display_update();
            }
        }
    }
}

/* ── AUX loopback ──────────────────────────────────────────────── */

static void play_aux(void)
{
    printf("Entering AUX mode\n");

    if (shelbourne_aux_enable(state.hw) < 0) {
        fprintf(stderr, "Failed to enable AUX\n");
        return;
    }

    state.playing = 1;
    display_update();

    while (!state.cmd_quit && state.cmd_preset < 0) {
        if (shelbourne_aux_pump(state.hw, state.vol_gain) < 0) {
            fprintf(stderr, "AUX pump error\n");
            break;
        }
        keypad_poll();

        if (state.cmd_pause) {
            state.cmd_pause = 0;
            static int aux_muted = 0;
            aux_muted = !aux_muted;
            shelbourne_mute(state.hw, aux_muted);
            printf("AUX %s\n", aux_muted ? "muted" : "unmuted");
        }

        if (state.display_dirty) {
            state.display_dirty = 0;
            display_update();
        }

        /* AUX button while in AUX = no-op (already in AUX) */
        if (state.cmd_aux) state.cmd_aux = 0;
    }

    shelbourne_aux_disable(state.hw);
    state.playing = 0;
    printf("Left AUX mode\n");
}

/* ── Signal handling ───────────────────────────────────────────── */

static void sig_handler(int sig)
{
    (void)sig;
    state.cmd_quit = 1;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    memset(&state, 0, sizeof(state));
    state.active_preset = NUM_PRESETS;  /* idle — no source selected */
    state.cmd_preset = -1;
    state.radio_fd = -1;
    state.volume = DEFAULT_VOL;
    state.vol_gain = shelbourne_volume_to_gain(DEFAULT_VOL);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize hardware */
    shelbourne_stop_stock_processes();
    state.hw = shelbourne_init();
    if (!state.hw) {
        fprintf(stderr, "Hardware init failed\n");
        return 1;
    }

    /* LED always off */
    shelbourne_led_set(state.hw, SHELBOURNE_LED_OFF);

    /* Start with blank display */
    shelbourne_display_clear(state.hw);
    shelbourne_display_flush(state.hw);

    /* Main loop */
    while (!state.cmd_quit) {
        const source_t *src = active_source();

        if (state.active_preset < 0) {
            play_aux();
        } else if (src && src->type == SRC_RADIO) {
            play_radio(src);
        } else if (src && src->type == SRC_DIR) {
            play_usb(src);
        } else {
            /* Empty preset or no source — wait for input */
            display_update();
            while (!INTERRUPTED() && !state.cmd_aux) {
                usleep(100000);
                keypad_poll();
            }
        }

        /* Handle AUX */
        if (state.cmd_aux) {
            state.cmd_aux = 0;
            state.active_preset = -1;
        }

        /* Handle preset */
        if (state.cmd_preset >= 0) {
            int p = state.cmd_preset;
            state.cmd_preset = -1;

            if (p < NUM_PRESETS && presets[p].num_sources > 0) {
                if (p == state.active_preset) {
                    /* Same preset — cycle source */
                    presets[p].current =
                        (presets[p].current + 1) % presets[p].num_sources;
                    printf("Preset %d: source %d/%d (%s)\n",
                           p + 1, presets[p].current + 1,
                           presets[p].num_sources,
                           presets[p].sources[presets[p].current].name);
                }
                state.active_preset = p;
            }
        }
    }

    /* Cleanup */
    printf("\nShutting down...\n");
    shelbourne_display_clear(state.hw);
    fb_center_2x(20, "Goodbye");
    shelbourne_display_flush(state.hw);
    sleep(1);

    shelbourne_shutdown(state.hw);
    return 0;
}
