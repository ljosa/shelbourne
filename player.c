/*
 * player.c — MP3/radio/AUX player using the Shelbourne library
 *
 * Controls:
 *   Volume Up/Down  = Adjust volume (+/- 5, minimum 5)
 *   Preset 1-4      = Select source (press again to cycle)
 *   AUX             = Switch to AUX input
 *   Power           = Return to idle (clock) screen
 */

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "shelbourne.h"
#include "station_logos.h"
#include "display.h"
#include "net.h"
#include "sun.h"
#include "airplay.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

enum source_type { SRC_RADIO, SRC_DIR, SRC_PODCAST };

typedef struct {
    enum source_type type;
    const char *name;
    const char *host;
    int         port;
    const char *path;
    const char *dir;
    const char *feed_url;   /* RSS feed URL for podcasts */
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

/* Preset 6: Weather podcast */
static const source_t p6_sources[] = {
    { SRC_PODCAST, "Eye on Sky", .feed_url = "podcasts.vpr.net/eye-on-the-sky", .logo = logo_eots },
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
    { p6_sources, sizeof(p6_sources)/sizeof(p6_sources[0]), 0 },
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

    /* Podcast */
    char podcast_ep_url[512];    /* resolved episode MP3 URL */
    char podcast_ep_host[256];   /* host for HTTP connection */
    char podcast_ep_path[512];   /* path for HTTP GET */
    int  podcast_ep_port;
    char podcast_date[32];       /* episode date string */

    /* Audio */
    int playing;

    /* Volume */
    int volume;
    int vol_gain;
    struct timespec vol_changed_at;
    int vol_bar_active;

    /* Commands */
    volatile int cmd_quit;
    volatile int cmd_off;
    volatile int cmd_aux;
    volatile int cmd_preset;  /* 0-5, or -1 for none */
    volatile int cmd_airplay; /* set when AirPlay stream detected */

    /* Display */
    int display_dirty;          /* immediate refresh (keypress, mode change) */
    int scroll_dirty;           /* deferred refresh (scroll animation) */
    long last_scroll_flush_ms;  /* throttle scroll-driven flushes */
} state;

#define SCROLL_FLUSH_INTERVAL_MS 300  /* max ~3 fps for scroll updates */

static long player_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

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
    fb_putstr(SHELBOURNE_FB_WIDTH - fb_text_width(vol) - 2, bar_y + 3, vol);
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

static void display_update(void)
{
    shelbourne_display_clear(state.hw);
    const source_t *src = active_source();

    if (airplay_is_streaming() && state.playing) {
        /* AirPlay mode */
        fb_center_4x(20, "AirPlay");

    } else if (state.active_preset < 0) {
        /* AUX mode — full-screen logo */
        display_logo(logo_aux);

    } else if (src && (src->type == SRC_RADIO || src->type == SRC_PODCAST)) {
        /* Radio/podcast — half-height logo + text below */
        if (!state.playing) {
            /* Not playing yet — show logo + name (for "connecting" phase) */
            if (src->logo)
                display_logo(src->logo);
            fb_center_2x(54, src->name);
        } else if (src->type == SRC_PODCAST) {
            /* Podcast — logo + name + date */
            if (src->logo)
                display_logo(src->logo);
            fb_center_2x(54, src->name);
            if (state.podcast_date[0])
                fb_center_2x(74, state.podcast_date);
        } else if (state.stream_title[0]) {
            /* Radio with ICY metadata — logo + scrolling title */
            if (src->logo)
                display_logo(src->logo);
            if (fb_scroll_2x(54, state.stream_title))
                state.scroll_dirty = 1;
        } else {
            /* Radio, no metadata yet — logo + name */
            if (src->logo)
                display_logo(src->logo);
            fb_center_2x(54, src->name);
        }

    } else if (src && src->type == SRC_DIR) {
        /* USB mode */
        if (!state.playing) {
            /* Paused */
        } else if (state.num_tracks > 0) {
            track_t *t = &state.tracks[state.current_track];

            char idx[16];
            snprintf(idx, sizeof(idx), "%d/%d",
                     state.current_track + 1, state.num_tracks);
            fb_putstr(SHELBOURNE_FB_WIDTH - fb_text_width(idx), 0, idx);

            /* Title — scroll if too long */
            if (fb_scroll_2x(14, t->title))
                state.scroll_dirty = 1;

            int y = 14 + 21;  /* below title */
            if (t->artist[0])
                y = fb_wrap_2x(y, 70, t->artist);
            if (t->album[0])
                fb_wrap_2x(y + 2, 96, t->album);
        } else {
            fb_center_2x(18, "No tracks");
        }

    } else {
        /* Idle — show clock during daytime, blank at night */
        if (sun_clock_visible(44.48, -73.21)) {
            time_t now = ntp_time();
            struct tm *tm = localtime(&now);
            if (tm) {
                char clk[6];
                snprintf(clk, sizeof(clk), "%d:%02d", tm->tm_hour, tm->tm_min);
                fb_center_4x(20, clk);

                const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
                char date[20];
                snprintf(date, sizeof(date), "%s %s %d",
                         days[tm->tm_wday], months[tm->tm_mon], tm->tm_mday);
                fb_center_2x(62, date);
            }
        }
        /* else: display stays blank (cleared at top of function) */
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
            state.cmd_off = 1;
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

    /* Detect incoming AirPlay stream */
    if (airplay_is_streaming() && !state.cmd_airplay)
        state.cmd_airplay = 1;
}

/* ── Interrupt check macro ─────────────────────────────────────── */

#define INTERRUPTED() \
    (state.cmd_quit || state.cmd_off || state.cmd_aux || \
     state.cmd_preset >= 0 || state.cmd_airplay)

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
            else if (strncmp(loc, "https://", 8) == 0) { loc += 8; port = 80; }
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
            return n;

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

    while (!INTERRUPTED()) {
        /* Show logo + name immediately (before slow connect) */
        printf("Connecting to %s...\n", src->name);
        state.stream_title[0] = '\0';
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
        buf_fill = 0;

        /* Pre-buffer */
        printf("Buffering...\n");
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
        display_update();

        while (!INTERRUPTED()) {
            keypad_poll();

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

            while (buf_fill >= 4 && !INTERRUPTED()) {
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

                    if (state.display_dirty) {
                        state.display_dirty = 0;
                        state.scroll_dirty = 0;
                        display_update();
                        state.last_scroll_flush_ms = player_mono_ms();
                    } else if (state.scroll_dirty) {
                        long now = player_mono_ms();
                        if (now - state.last_scroll_flush_ms >= SCROLL_FLUSH_INTERVAL_MS) {
                            state.scroll_dirty = 0;
                            display_update();
                            state.last_scroll_flush_ms = now;
                        }
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
                state.scroll_dirty = 0;
                display_update();
                state.last_scroll_flush_ms = player_mono_ms();
            } else if (state.scroll_dirty) {
                long now = player_mono_ms();
                if (now - state.last_scroll_flush_ms >= SCROLL_FLUSH_INTERVAL_MS) {
                    state.scroll_dirty = 0;
                    display_update();
                    state.last_scroll_flush_ms = now;
                }
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

    while (!state.cmd_quit && state.cmd_preset < 0 && !state.cmd_off) {
        if (shelbourne_aux_pump(state.hw, state.vol_gain) < 0) {
            fprintf(stderr, "AUX pump error\n");
            break;
        }
        keypad_poll();

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

/* ── Podcast ───────────────────────────────────────────────────── */

/*
 * Fetch RSS feed via curl (HTTPS required), find the first
 * <enclosure url="..."/> and <pubDate>...</pubDate> in the first <item>.
 */
static int podcast_fetch_latest(const source_t *src)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -skL 'https://%s' 2>/dev/null",
             src->feed_url);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to run curl for podcast feed\n");
        return -1;
    }

    char *rss = malloc(128 * 1024);
    if (!rss) { pclose(fp); return -1; }

    int rss_len = 0;
    while (rss_len < 128 * 1024 - 1) {
        int n = fread(rss + rss_len, 1, 128 * 1024 - 1 - rss_len, fp);
        if (n <= 0) break;
        rss_len += n;
    }
    rss[rss_len] = '\0';
    pclose(fp);

    if (rss_len == 0) {
        fprintf(stderr, "Empty response from podcast feed\n");
        free(rss);
        return -1;
    }

    /* Find first <item> */
    char *item = strstr(rss, "<item");
    if (!item) { free(rss); return -1; }

    /* Find <enclosure url="..." */
    char *enc = strstr(item, "<enclosure");
    if (!enc) { free(rss); return -1; }
    char *url_attr = strstr(enc, "url=\"");
    if (!url_attr) { free(rss); return -1; }
    url_attr += 5;
    char *url_end = strchr(url_attr, '"');
    if (!url_end) { free(rss); return -1; }

    int url_len = url_end - url_attr;
    if (url_len >= (int)sizeof(state.podcast_ep_url))
        url_len = sizeof(state.podcast_ep_url) - 1;
    memcpy(state.podcast_ep_url, url_attr, url_len);
    state.podcast_ep_url[url_len] = '\0';

    /* Parse URL into host/port/path */
    char *ep = state.podcast_ep_url;
    state.podcast_ep_port = 80;
    if (strncmp(ep, "http://", 7) == 0) ep += 7;
    else if (strncmp(ep, "https://", 8) == 0) { ep += 8; state.podcast_ep_port = 80; }

    char *ep_slash = strchr(ep, '/');
    if (ep_slash) {
        strncpy(state.podcast_ep_path, ep_slash, sizeof(state.podcast_ep_path) - 1);
        state.podcast_ep_path[sizeof(state.podcast_ep_path) - 1] = '\0';
        int hn = ep_slash - ep;
        if (hn >= (int)sizeof(state.podcast_ep_host))
            hn = sizeof(state.podcast_ep_host) - 1;
        memcpy(state.podcast_ep_host, ep, hn);
        state.podcast_ep_host[hn] = '\0';
    }

    /* Extract port from host if present */
    char *colon = strchr(state.podcast_ep_host, ':');
    if (colon) {
        state.podcast_ep_port = atoi(colon + 1);
        *colon = '\0';
    }

    /* Find <pubDate> within the <item> */
    state.podcast_date[0] = '\0';
    char *pub = strstr(item, "<pubDate>");
    if (pub) {
        pub += 9;
        char *pub_end = strstr(pub, "</pubDate>");
        if (pub_end) {
            /* Extract "13 Mar 2026" from RFC 2822 date */
            char *comma = strchr(pub, ',');
            if (comma && comma < pub_end) pub = comma + 2;
            int plen = pub_end - pub;
            if (plen > 11) plen = 11;
            if (plen >= (int)sizeof(state.podcast_date))
                plen = sizeof(state.podcast_date) - 1;
            memcpy(state.podcast_date, pub, plen);
            state.podcast_date[plen] = '\0';
            while (plen > 0 && state.podcast_date[plen-1] == ' ')
                state.podcast_date[--plen] = '\0';
        }
    }

    printf("Podcast episode: %s\n", state.podcast_ep_url);
    printf("Podcast date: %s\n", state.podcast_date);
    free(rss);
    return 0;
}

static int podcast_connect(void)
{
    int fd = http_get(state.podcast_ep_host, state.podcast_ep_port,
                      state.podcast_ep_path);
    if (fd < 0)
        fprintf(stderr, "Podcast connect failed\n");
    else
        printf("Podcast stream ready\n");
    return fd;
}

static void play_podcast(const source_t *src)
{
    /* Show logo immediately */
    state.podcast_date[0] = '\0';
    display_update();

    printf("Fetching podcast feed...\n");
    if (podcast_fetch_latest(src) < 0) {
        fprintf(stderr, "Failed to fetch podcast feed\n");
        display_update();
        for (int w = 0; w < 50 && !INTERRUPTED(); w++) {
            usleep(100000);
            keypad_poll();
        }
        return;
    }

    display_update();  /* show date */

    int sock = podcast_connect();
    if (sock < 0) {
        fprintf(stderr, "Failed to connect to podcast\n");
        for (int w = 0; w < 30 && !INTERRUPTED(); w++) {
            usleep(100000);
            keypad_poll();
        }
        return;
    }

    mp3dec_t mp3d;
    mp3dec_init(&mp3d);
    audio_reset();

    uint8_t stream_buf[MP3_BUF_SIZE * 2];
    int buf_fill = 0;

    /* Pre-buffer */
    printf("Buffering podcast...\n");
    {
        struct timeval tv_buf = {.tv_sec = 5};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_buf, sizeof(tv_buf));
        while (buf_fill < MP3_BUF_SIZE) {
            int n = recv(sock, stream_buf + buf_fill,
                         MP3_BUF_SIZE - buf_fill, 0);
            if (n <= 0) break;
            buf_fill += n;
        }
    }
    printf("Buffered %d bytes\n", buf_fill);
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);

    state.playing = 1;
    display_update();

    int stream_ended = 0;

    while (!INTERRUPTED()) {
        keypad_poll();

        /* Top up buffer */
        if (!stream_ended && buf_fill < (int)sizeof(stream_buf)) {
            int n = recv(sock, stream_buf + buf_fill,
                         sizeof(stream_buf) - buf_fill, 0);
            if (n > 0) {
                buf_fill += n;
            } else if (n == 0) {
                stream_ended = 1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                stream_ended = 1;
            }
        }

        /* Decode */
        mp3dec_frame_info_t info;
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

        int decoded_any = 0;
        while (buf_fill >= 4 && !INTERRUPTED()) {
            int samples = mp3dec_decode_frame(&mp3d,
                stream_buf, buf_fill, pcm, &info);

            if (info.frame_bytes > 0) {
                decoded_any = 1;
                buf_fill -= info.frame_bytes;
                memmove(stream_buf, stream_buf + info.frame_bytes, buf_fill);

                if (samples > 0 && info.hz > 0)
                    convert_and_write(pcm, samples, info.channels, info.hz);

                shelbourne_audio_wait(state.hw);

                if (!stream_ended && buf_fill < MP3_BUF_SIZE) {
                    int n = recv(sock, stream_buf + buf_fill,
                                 sizeof(stream_buf) - buf_fill, 0);
                    if (n > 0) buf_fill += n;
                    else if (n == 0) stream_ended = 1;
                }

                keypad_poll();

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

        if (stream_ended && !decoded_any && buf_fill < 4) {
            printf("Podcast finished\n");
            break;
        }
    }

    shelbourne_audio_drain(state.hw);
    close(sock);
    state.playing = 0;

    /* Return to idle (clock) when podcast finishes naturally */
    if (!INTERRUPTED()) {
        state.active_preset = NUM_PRESETS;
    }
}

/* ── AirPlay playback ──────────────────────────────────────────── */

static void play_airplay(void)
{
    printf("Entering AirPlay mode\n");
    state.playing = 1;
    display_update();

    audio_reset();

    int16_t pcm_buf[4096];  /* temp buffer for PCM from ring */

    while (!state.cmd_quit && !state.cmd_off && state.cmd_preset < 0
           && !state.cmd_aux) {
        if (!airplay_is_streaming()) {
            printf("AirPlay: stream ended\n");
            break;
        }

        int n = airplay_read(pcm_buf, sizeof(pcm_buf));
        if (n > 0) {
            /* AirPlay delivers 16-bit stereo, typically 44100 Hz */
            int samples = n / 4;  /* 4 bytes per sample (2 ch × 16 bit) */
            convert_and_write((mp3d_sample_t *)pcm_buf, samples, 2, 44100);
            shelbourne_audio_wait(state.hw);
        } else if (n == 0) {
            /* No data available, poll briefly */
            usleep(5000);
        } else {
            break;  /* stream ended */
        }

        keypad_poll();

        if (state.display_dirty) {
            state.display_dirty = 0;
            display_update();
        }
    }

    shelbourne_audio_drain(state.hw);
    state.playing = 0;
    printf("Leaving AirPlay mode\n");
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
    setenv("TZ", "US/Eastern", 1);
    tzset();

    memset(&state, 0, sizeof(state));
    state.active_preset = NUM_PRESETS;  /* idle — no source selected */
    state.cmd_preset = -1;
    state.radio_fd = -1;
    state.volume = DEFAULT_VOL;
    state.vol_gain = shelbourne_volume_to_gain(DEFAULT_VOL);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Kill stock audio processes (harmless if none running).
     * Don't freeze shepherdd — with the shepherd override config,
     * it manages player + airplayd and should stay running. */
    system("killall -9 APServer BoseApp STSCertified UpnpSource "
           "WebServer CLIServer NetManager scmmond "
           "SoftwareUpdate IoT TPDA LegacyProduct "
           "PtsServer microbswitch httpd 2>/dev/null");
    sleep(1);
    state.hw = shelbourne_init();
    if (!state.hw) {
        fprintf(stderr, "Hardware init failed\n");
        return 1;
    }

    /* Initialize display module */
    display_init(state.hw);

    /* LED always off */
    shelbourne_led_set(state.hw, SHELBOURNE_LED_OFF);

    /* Start AirPlay receiver (always advertising) */
    if (airplay_init("Mr Noisy") < 0)
        fprintf(stderr, "Warning: AirPlay init failed\n");

    /* Start with blank display */
    shelbourne_display_clear(state.hw);
    shelbourne_display_flush(state.hw);

    /* Main loop */
    while (!state.cmd_quit) {
        const source_t *src = active_source();

        if (state.cmd_airplay) {
            state.cmd_airplay = 0;
            play_airplay();
            continue;
        } else if (state.active_preset < 0) {
            play_aux();
        } else if (src && src->type == SRC_RADIO) {
            play_radio(src);
        } else if (src && src->type == SRC_PODCAST) {
            play_podcast(src);
        } else if (src && src->type == SRC_DIR) {
            play_usb(src);
        } else {
            /* Idle — show clock */
            display_update();
            while (!INTERRUPTED()) {
                usleep(100000);
                keypad_poll();
                /* Check for incoming AirPlay stream */
                if (airplay_is_streaming()) {
                    state.cmd_airplay = 1;
                    break;
                }
                /* Refresh clock every ~10s */
                static int idle_ticks;
                if (++idle_ticks >= 100) {
                    idle_ticks = 0;
                    state.display_dirty = 1;
                }
                if (state.display_dirty) {
                    state.display_dirty = 0;
                    display_update();
                }
            }
        }

        /* Handle commands in priority order */
        if (state.cmd_off) {
            state.cmd_off = 0;
            state.active_preset = NUM_PRESETS;  /* idle */
        } else if (state.cmd_aux) {
            state.cmd_aux = 0;
            state.active_preset = -1;
        } else if (state.cmd_preset >= 0) {
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

    airplay_stop();
    shelbourne_shutdown(state.hw);
    return 0;
}
