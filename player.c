/*
 * player.c — MP3/radio/AUX player using the Shelbourne library
 *
 * Usage on device:
 *   /mnt/nv/player [/media/sda1]
 *
 * Controls:
 *   Volume Up/Down = Adjust volume (+/- 5)
 *   Preset 1-6     = Radio stations
 *   AUX            = Switch to AUX input / next track (USB)
 *   Power          = Pause/Play toggle
 */

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "shelbourne.h"

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
#include <unistd.h>

/* ── Configuration ─────────────────────────────────────────────── */

#define MAX_FILES       500
#define MAX_PATH_LEN    512
#define MAX_TAG         256
#define MP3_BUF_SIZE    (16 * 1024)
#define DEFAULT_VOL     50

/* ── Radio stations ────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *host;
    int         port;
    const char *path;
} radio_station_t;

static const radio_station_t radio_stations[] = {
    { "BBC World",   "stream.live.vc.bbcmedia.co.uk", 80, "/bbc_world_service" },
    { "BBC Radio 4", "stream.live.vc.bbcmedia.co.uk", 80, "/bbc_radio_fourfm" },
    { "BBC Radio 3", "stream.live.vc.bbcmedia.co.uk", 80, "/bbc_radio_three" },
    { "BBC Radio 6", "stream.live.vc.bbcmedia.co.uk", 80, "/bbc_6music" },
    { "WNYC FM",     "fm939.wnyc.org",                80, "/wnycfm" },
    { "Classic FM",  "media-ice.musicradio.com",       80, "/ClassicFMMP3" },
};
#define NUM_STATIONS (sizeof(radio_stations) / sizeof(radio_stations[0]))

/* ── Track info ────────────────────────────────────────────────── */

typedef struct {
    char path[MAX_PATH_LEN];
    char filename[MAX_PATH_LEN];
    char title[MAX_TAG];
    char artist[MAX_TAG];
    char album[MAX_TAG];
} track_t;

/* ── Player state ──────────────────────────────────────────────── */

enum player_mode { MODE_USB, MODE_RADIO, MODE_AUX };

static struct {
    shelbourne_t *hw;
    enum player_mode mode;
    enum player_mode prev_mode;  /* mode to return to from AUX */

    /* USB */
    track_t tracks[MAX_FILES];
    int num_tracks;
    int current_track;

    /* Radio */
    int current_station;
    int radio_fd;

    /* Audio */
    int playing;

    /* Volume (software mixer) */
    int volume;
    int vol_gain;     /* fixed-point 16-bit: 0=mute, 65536=unity */

    /* Commands from keypad */
    volatile int cmd_next;
    volatile int cmd_prev;
    volatile int cmd_quit;
    volatile int cmd_pause;
    volatile int cmd_mode_switch;
    volatile int cmd_aux;
    volatile int cmd_station;  /* -1 = no change, 0-5 = preset */

    /* Display */
    int display_dirty;
} state;

/* ── Resampler output buffer ──────────────────────────────────── */

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

/*
 * Convert minimp3 output (16-bit PCM) to 32-bit 48kHz stereo,
 * apply volume gain, and write to shelbourne audio buffer.
 */
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

    /* Flush accumulated frames to shelbourne buffer */
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
#define DISP_COLS (SHELBOURNE_FB_WIDTH / CHAR_W)   /* 21 */
#define DISP_ROWS (SHELBOURNE_FB_HEIGHT / CHAR_H)  /* 12 */

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

/* ── Text drawing (on shelbourne display buffer) ──────────────── */

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

static void fb_hline(int y, uint8_t val)
{
    uint8_t *fb = shelbourne_display_buffer(state.hw);
    if (!fb) return;
    if (y >= 0 && y < SHELBOURNE_FB_HEIGHT)
        memset(&fb[y * SHELBOURNE_FB_WIDTH], val, SHELBOURNE_FB_WIDTH);
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

/* ── DNS resolver (static binary can't use gethostbyname) ──────── */

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

    int ancount = (resp[4] << 8) | resp[5];
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

static void scan_usb(const char *dir)
{
    state.num_tracks = 0;
    scan_recursive(dir, 0);

    for (int i = 0; i < state.num_tracks - 1; i++)
        for (int j = i + 1; j < state.num_tracks; j++)
            if (strcmp(state.tracks[i].path, state.tracks[j].path) > 0) {
                track_t tmp = state.tracks[i];
                state.tracks[i] = state.tracks[j];
                state.tracks[j] = tmp;
            }

    printf("Found %d MP3 track(s)\n", state.num_tracks);
    for (int i = 0; i < state.num_tracks; i++) {
        printf("  %3d. %s", i + 1, state.tracks[i].title);
        if (state.tracks[i].artist[0])
            printf(" - %s", state.tracks[i].artist);
        printf("\n");
    }
}

/* ── Display ───────────────────────────────────────────────────── */

static void display_update(void)
{
    shelbourne_display_clear(state.hw);

    /* Volume bar at bottom (always visible) */
    {
        char vol[8];
        snprintf(vol, sizeof(vol), "V%d", state.volume);
        fb_putstr(SHELBOURNE_FB_WIDTH - strlen(vol) * CHAR_W, CHAR_H * 11, vol);
    }

    if (state.mode == MODE_USB) {
        fb_putstr(0, 0, "USB");
        if (state.num_tracks > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d/%d",
                     state.current_track + 1, state.num_tracks);
            fb_putstr(SHELBOURNE_FB_WIDTH - strlen(buf) * CHAR_W, 0, buf);
        }
        fb_hline(CHAR_H, 0x40);

        if (state.num_tracks > 0) {
            track_t *t = &state.tracks[state.current_track];
            char line[DISP_COLS + 1];

            snprintf(line, sizeof(line), "%.*s", DISP_COLS, t->title);
            fb_center(CHAR_H * 2, line);

            if (t->artist[0]) {
                snprintf(line, sizeof(line), "%.*s", DISP_COLS, t->artist);
                fb_center(CHAR_H * 4, line);
            }
            if (t->album[0]) {
                snprintf(line, sizeof(line), "%.*s", DISP_COLS, t->album);
                fb_center(CHAR_H * 6, line);
            }
        } else {
            fb_center(CHAR_H * 4, "No tracks");
        }

        fb_center(CHAR_H * 9, state.playing ? "> Playing" : "Stopped");

    } else if (state.mode == MODE_RADIO) {
        fb_putstr(0, 0, "RADIO");
        char buf[8];
        snprintf(buf, sizeof(buf), "P%d", state.current_station + 1);
        fb_putstr(SHELBOURNE_FB_WIDTH - strlen(buf) * CHAR_W, 0, buf);
        fb_hline(CHAR_H, 0x40);

        fb_center(CHAR_H * 3, radio_stations[state.current_station].name);
        fb_center(CHAR_H * 6, state.playing ? "Streaming..." : "Stopped");

        uint8_t *fb = shelbourne_display_buffer(state.hw);
        if (fb) {
            for (int i = 0; i < (int)NUM_STATIONS && i < 6; i++) {
                char p[4];
                snprintf(p, sizeof(p), "%d", i + 1);
                int x = i * (SHELBOURNE_FB_WIDTH / 6) + 4;
                if (i == state.current_station) {
                    for (int r = CHAR_H * 10 - 1; r < CHAR_H * 11 + 1; r++)
                        for (int xx = x - 2; xx < x + CHAR_W + 1 && xx < SHELBOURNE_FB_WIDTH; xx++)
                            if (r >= 0 && r < SHELBOURNE_FB_HEIGHT && xx >= 0)
                                fb[r * SHELBOURNE_FB_WIDTH + xx] = 0x60;
                }
                fb_putstr(x, CHAR_H * 10, p);
            }
        }

    } else if (state.mode == MODE_AUX) {
        fb_putstr(0, 0, "AUX");
        fb_hline(CHAR_H, 0x40);
        fb_center(CHAR_H * 4, "AUX Input");
        fb_center(CHAR_H * 6, "Listening...");
    }

    shelbourne_display_flush(state.hw);
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
                state.volume += 5;
                state.vol_gain = shelbourne_volume_to_gain(state.volume);
                printf("Volume: %d\n", state.volume);
                state.display_dirty = 1;
            }
            break;
        case SHELBOURNE_KEY_VOLUME_DOWN:
            if (state.volume > 0) {
                state.volume -= 5;
                state.vol_gain = shelbourne_volume_to_gain(state.volume);
                printf("Volume: %d\n", state.volume);
                state.display_dirty = 1;
            }
            break;
        case SHELBOURNE_KEY_PRESET_1: state.cmd_station = 0; break;
        case SHELBOURNE_KEY_PRESET_2: state.cmd_station = 1; break;
        case SHELBOURNE_KEY_PRESET_3: state.cmd_station = 2; break;
        case SHELBOURNE_KEY_PRESET_4: state.cmd_station = 3; break;
        case SHELBOURNE_KEY_PRESET_5: state.cmd_station = 4; break;
        case SHELBOURNE_KEY_PRESET_6: state.cmd_station = 5; break;
        case SHELBOURNE_KEY_AUX:
            if (state.mode == MODE_AUX)
                state.cmd_mode_switch = 1;  /* exit AUX */
            else
                state.cmd_aux = 1;          /* enter AUX */
            break;
        default:
            break;
        }
    }
}

/* ── LEDs ──────────────────────────────────────────────────────── */

static void leds_update(void)
{
    if (state.playing) {
        if (state.mode == MODE_USB)
            shelbourne_led_set(state.hw, SHELBOURNE_LED_WHITE);
        else
            shelbourne_led_set(state.hw, SHELBOURNE_LED_YELLOW);
    } else {
        shelbourne_led_set(state.hw, SHELBOURNE_LED_OFF);
    }
}

/* ── USB playback ──────────────────────────────────────────────── */

static void play_usb_track(void)
{
    if (state.num_tracks == 0) return;

    track_t *t = &state.tracks[state.current_track];
    printf("[%d/%d] %s\n", state.current_track + 1, state.num_tracks,
           t->title);
    display_update();
    leds_update();

    FILE *f = fopen(t->path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", t->path);
        return;
    }

    mp3dec_t mp3d;
    mp3dec_init(&mp3d);
    audio_reset();

    uint8_t file_buf[MP3_BUF_SIZE * 2];
    int buf_fill = 0;
    state.playing = 1;
    display_update();
    leds_update();

    while (!state.cmd_quit && !state.cmd_next && !state.cmd_prev &&
           !state.cmd_mode_switch && !state.cmd_aux && state.cmd_station < 0) {

        /* Handle pause */
        while (state.cmd_pause) {
            state.cmd_pause = 0;
            state.playing = 0;
            printf("Paused\n");
            display_update();
            leds_update();
            while (!state.cmd_pause && !state.cmd_quit && !state.cmd_next &&
                   !state.cmd_prev && !state.cmd_mode_switch &&
                   !state.cmd_aux && state.cmd_station < 0) {
                usleep(50000);
                keypad_poll();
            }
            if (state.cmd_pause) {
                state.cmd_pause = 0;
                state.playing = 1;
                printf("Playing\n");
                audio_reset();
                display_update();
                leds_update();
            }
        }
        if (state.cmd_quit || state.cmd_next || state.cmd_prev ||
            state.cmd_mode_switch || state.cmd_aux || state.cmd_station >= 0) break;

        /* Refill buffer from file */
        if (buf_fill < MP3_BUF_SIZE) {
            int n = fread(file_buf + buf_fill, 1,
                          sizeof(file_buf) - buf_fill, f);
            if (n <= 0) break;
            buf_fill += n;
        }

        /* Decode frame */
        mp3dec_frame_info_t info;
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

        int samples = mp3dec_decode_frame(&mp3d,
            file_buf, buf_fill, pcm, &info);

        if (info.frame_bytes == 0) break;

        buf_fill -= info.frame_bytes;
        memmove(file_buf, file_buf + info.frame_bytes, buf_fill);

        if (samples > 0 && info.hz > 0) {
            if (convert_and_write(pcm, samples, info.channels, info.hz) < 0)
                break;
        }

        shelbourne_audio_wait(state.hw);
        keypad_poll();
    }

    shelbourne_audio_drain(state.hw);
    fclose(f);
    state.playing = 0;
    state.display_dirty = 0;
    display_update();
    leds_update();
}

/* ── Radio streaming ───────────────────────────────────────────── */

static int radio_connect(const radio_station_t *st)
{
    struct in_addr resolved;
    if (dns_resolve(st->host, &resolved) < 0) {
        fprintf(stderr, "DNS lookup failed for %s\n", st->host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(st->port);
    addr.sin_addr = resolved;

    struct timeval tv = {.tv_sec = 10};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Connect to %s:%d failed: %s\n",
                st->host, st->port, strerror(errno));
        close(fd);
        return -1;
    }

    char req[512];
    int len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: ShelbournePlayer/1.0\r\n"
        "Accept: */*\r\n"
        "\r\n",
        st->path, st->host);

    if (send(fd, req, len, 0) != len) {
        close(fd);
        return -1;
    }

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

    printf("Radio connected: %s\n", st->name);
    return fd;
}

static void play_radio(void)
{
    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    uint8_t stream_buf[MP3_BUF_SIZE * 2];
    int buf_fill = 0;

    while (!state.cmd_quit && !state.cmd_mode_switch && !state.cmd_aux) {
        const radio_station_t *st = &radio_stations[state.current_station];
        printf("Connecting to %s...\n", st->name);
        display_update();

        int sock = radio_connect(st);
        if (sock < 0) {
            fprintf(stderr, "Failed, retrying in 3s...\n");
            shelbourne_led_set(state.hw, SHELBOURNE_LED_YELLOW);
            sleep(3);
            shelbourne_led_set(state.hw, SHELBOURNE_LED_OFF);

            if (state.cmd_station >= 0) {
                state.current_station = state.cmd_station;
                state.cmd_station = -1;
            }
            keypad_poll();
            continue;
        }

        audio_reset();
        state.radio_fd = sock;
        buf_fill = 0;

        /* Pre-buffer */
        printf("Buffering...\n");
        display_update();
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
        printf("Buffered %d bytes, starting playback\n", buf_fill);

        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);

        state.playing = 1;
        display_update();
        leds_update();

        while (!state.cmd_quit && !state.cmd_mode_switch && !state.cmd_aux &&
               state.cmd_station < 0 && !state.cmd_next && !state.cmd_prev) {

            keypad_poll();

            /* Handle pause */
            while (state.cmd_pause) {
                state.cmd_pause = 0;
                state.playing = 0;
                printf("Radio paused\n");
                display_update();
                leds_update();
                while (!state.cmd_pause && !state.cmd_quit &&
                       !state.cmd_mode_switch && !state.cmd_aux &&
                       state.cmd_station < 0 &&
                       !state.cmd_next && !state.cmd_prev) {
                    usleep(50000);
                    keypad_poll();
                }
                if (state.cmd_pause) {
                    state.cmd_pause = 0;
                    state.playing = 1;
                    printf("Radio playing\n");
                    audio_reset();
                    display_update();
                    leds_update();
                }
            }
            if (state.cmd_quit || state.cmd_mode_switch || state.cmd_aux ||
                state.cmd_station >= 0 || state.cmd_next || state.cmd_prev)
                break;

            /* Top up buffer from network */
            if (buf_fill < (int)sizeof(stream_buf)) {
                int n = recv(sock, stream_buf + buf_fill,
                             sizeof(stream_buf) - buf_fill, 0);
                if (n > 0) {
                    buf_fill += n;
                } else if (n == 0) {
                    fprintf(stderr, "Stream ended, reconnecting...\n");
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "Stream error: %s, reconnecting...\n",
                            strerror(errno));
                    break;
                }
            }

            /* Decode all available frames */
            {
                mp3dec_frame_info_t info;
                mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

                while (buf_fill >= 4 &&
                       !state.cmd_quit && !state.cmd_mode_switch &&
                       !state.cmd_aux && state.cmd_station < 0 &&
                       !state.cmd_next && !state.cmd_prev &&
                       !state.cmd_pause) {

                    int samples = mp3dec_decode_frame(&mp3d,
                        stream_buf, buf_fill, pcm, &info);

                    if (info.frame_bytes > 0) {
                        buf_fill -= info.frame_bytes;
                        memmove(stream_buf, stream_buf + info.frame_bytes, buf_fill);

                        if (samples > 0 && info.hz > 0)
                            convert_and_write(pcm, samples, info.channels, info.hz);

                        shelbourne_audio_wait(state.hw);

                        /* Top up network buffer between frames */
                        if (buf_fill < MP3_BUF_SIZE) {
                            int n = recv(sock, stream_buf + buf_fill,
                                         sizeof(stream_buf) - buf_fill, 0);
                            if (n > 0) buf_fill += n;
                        }
                        keypad_poll();
                    } else if (buf_fill > MP3_BUF_SIZE) {
                        memmove(stream_buf, stream_buf + 1, buf_fill - 1);
                        buf_fill--;
                    } else {
                        break;
                    }
                }
            }
        }

        close(sock);
        state.radio_fd = -1;
        state.playing = 0;

        if (state.cmd_station >= 0) {
            state.current_station = state.cmd_station;
            state.cmd_station = -1;
        } else if (state.cmd_next) {
            state.current_station = (state.current_station + 1) % NUM_STATIONS;
            state.cmd_next = 0;
        } else if (state.cmd_prev) {
            state.current_station = (state.current_station + NUM_STATIONS - 1) % NUM_STATIONS;
            state.cmd_prev = 0;
        }
    }

    state.playing = 0;
    leds_update();
    display_update();
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
    leds_update();

    while (!state.cmd_quit && !state.cmd_mode_switch && state.cmd_station < 0) {
        if (shelbourne_aux_pump(state.hw, state.vol_gain) < 0) {
            fprintf(stderr, "AUX pump error\n");
            break;
        }
        keypad_poll();

        /* Update display if volume changed */
        if (state.display_dirty) {
            state.display_dirty = 0;
            display_update();
        }

        if (state.cmd_pause) {
            state.cmd_pause = 0;
            /* In AUX mode, pause = mute toggle */
            static int aux_muted = 0;
            aux_muted = !aux_muted;
            shelbourne_mute(state.hw, aux_muted);
            printf("AUX %s\n", aux_muted ? "muted" : "unmuted");
        }
    }

    shelbourne_aux_disable(state.hw);
    state.playing = 0;
    printf("Left AUX mode\n");
    leds_update();
    display_update();
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
    const char *media_dir = "/media/sda1";
    if (argc > 1) media_dir = argv[1];

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    memset(&state, 0, sizeof(state));
    state.mode = MODE_USB;
    state.prev_mode = MODE_USB;
    state.cmd_station = -1;
    state.radio_fd = -1;
    state.volume = DEFAULT_VOL;
    state.vol_gain = shelbourne_volume_to_gain(DEFAULT_VOL);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize hardware */
    state.hw = shelbourne_init();
    if (!state.hw) {
        fprintf(stderr, "Hardware init failed\n");
        return 1;
    }

    /* Startup screen */
    shelbourne_display_clear(state.hw);
    fb_center(CHAR_H * 4, "Starting...");
    shelbourne_display_flush(state.hw);

    /* Scan USB */
    scan_usb(media_dir);

    /* Initial mode */
    if (state.num_tracks > 0)
        state.mode = MODE_USB;
    else
        state.mode = MODE_RADIO;

    /* Main loop */
    while (!state.cmd_quit) {
        if (state.mode == MODE_USB) {
            if (state.num_tracks > 0) {
                play_usb_track();

                if (!state.cmd_quit && !state.cmd_next && !state.cmd_prev &&
                    !state.cmd_mode_switch && !state.cmd_aux &&
                    state.cmd_station < 0) {
                    state.current_track++;
                    if (state.current_track >= state.num_tracks)
                        state.current_track = 0;
                }
            } else {
                display_update();
                usleep(100000);
                keypad_poll();
            }

            if (state.cmd_next) {
                state.cmd_next = 0;
                state.current_track = (state.current_track + 1) %
                                       state.num_tracks;
            }
            if (state.cmd_prev) {
                state.cmd_prev = 0;
                state.current_track = (state.current_track +
                                        state.num_tracks - 1) %
                                       state.num_tracks;
            }
        } else if (state.mode == MODE_RADIO) {
            play_radio();
        } else if (state.mode == MODE_AUX) {
            play_aux();
        }

        /* Handle AUX toggle */
        if (state.cmd_aux) {
            state.cmd_aux = 0;
            state.prev_mode = state.mode;
            state.mode = MODE_AUX;
            display_update();
            leds_update();
        }

        /* Handle mode switch (leaving AUX) */
        if (state.cmd_mode_switch) {
            state.cmd_mode_switch = 0;
            if (state.mode == MODE_AUX) {
                state.mode = state.prev_mode;
            } else if (state.mode == MODE_USB && NUM_STATIONS > 0) {
                state.mode = MODE_RADIO;
            } else if (state.mode == MODE_RADIO && state.num_tracks > 0) {
                state.mode = MODE_USB;
            }
            display_update();
            leds_update();
        }

        /* Handle station presets (always switch to radio) */
        if (state.cmd_station >= 0) {
            int st = state.cmd_station;
            state.cmd_station = -1;
            if (st < (int)NUM_STATIONS) {
                state.mode = MODE_RADIO;
                state.current_station = st;
            }
        }
    }

    /* Cleanup */
    printf("\nShutting down...\n");

    shelbourne_display_clear(state.hw);
    fb_center(CHAR_H * 4, "Goodbye");
    shelbourne_display_flush(state.hw);
    sleep(1);

    shelbourne_shutdown(state.hw);
    return 0;
}
