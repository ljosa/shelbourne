/*
 * displayd.c — display process (forked child)
 *
 * Renders the OLED framebuffer and handles text scrolling
 * independently, so the audio process is never blocked by
 * the ~5ms SPI flush.
 */

#include "displayd.h"
#include "display.h"
#include "sun.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VOL_BAR_MS       2000
#define SCROLL_POLL_MS   500
#define CLOCK_REFRESH_MS 30000
#define SWEEP_POLL_MS    80    /* ~12 fps for radar sweep */

/* Burlington VT */
#define LAT  44.0247
#define LON -73.0111

static shelbourne_t *disp_hw;
static display_msg_t cur;
static int vol_level;
static int vol_active;
static long vol_show_ms;
static pid_t child_pid;

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void draw_volume_bar(void)
{
    int bar_y = SHELBOURNE_FB_HEIGHT - 16;
    uint8_t *fb = shelbourne_display_buffer(disp_hw);
    if (!fb) return;

    memset(fb + bar_y * SHELBOURNE_FB_WIDTH, 0, 16 * SHELBOURNE_FB_WIDTH);

    int max_w = SHELBOURNE_FB_WIDTH - 32;
    int fill = (vol_level * max_w + 50) / 100;
    for (int y = bar_y + 4; y < bar_y + 12; y++) {
        for (int x = 2; x < 2 + fill; x++)
            fb[y * SHELBOURNE_FB_WIDTH + x] = 0xFF;
        for (int x = 2 + fill; x < 2 + max_w; x++)
            fb[y * SHELBOURNE_FB_WIDTH + x] = 0x30;
    }

    char vol[4];
    snprintf(vol, sizeof(vol), "%d", vol_level);
    fb_putstr(SHELBOURNE_FB_WIDTH - fb_text_width(vol) - 2, bar_y + 3, vol);
}

/* ── Radar rendering ───────────────────────────────────────────── */

#define RADAR_CX     64
#define RADAR_CY     50
#define RADAR_R      44

static int sweep_angle;  /* degrees, advances each frame */

static void fb_pixel(int x, int y, uint8_t val)
{
    if (x < 0 || x >= SHELBOURNE_FB_WIDTH ||
        y < 0 || y >= SHELBOURNE_FB_HEIGHT) return;
    uint8_t *fb = shelbourne_display_buffer(disp_hw);
    if (!fb) return;
    uint8_t bg = fb[y * SHELBOURNE_FB_WIDTH + x];
    if (val > bg)
        fb[y * SHELBOURNE_FB_WIDTH + x] = val;
}

static void fb_line(int x0, int y0, int x1, int y1, uint8_t val)
{
    /* Bresenham */
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_pixel(x0, y0, val);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fb_circle(int cx, int cy, int r, uint8_t val)
{
    /* Midpoint circle */
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        fb_pixel(cx + x, cy + y, val);
        fb_pixel(cx - x, cy + y, val);
        fb_pixel(cx + x, cy - y, val);
        fb_pixel(cx - x, cy - y, val);
        fb_pixel(cx + y, cy + x, val);
        fb_pixel(cx - y, cy + x, val);
        fb_pixel(cx + y, cy - x, val);
        fb_pixel(cx - y, cy - x, val);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

static void draw_radar_base(int radius_nm)
{
    /* Crosshairs */
    fb_line(RADAR_CX - RADAR_R, RADAR_CY,
            RADAR_CX + RADAR_R, RADAR_CY, 80);
    fb_line(RADAR_CX, RADAR_CY - RADAR_R,
            RADAR_CX, RADAR_CY + RADAR_R, 80);
    /* Range rings at 5nm increments */
    for (int r_nm = 5; r_nm <= radius_nm; r_nm += 5) {
        int r_px = r_nm * RADAR_R / radius_nm;
        fb_circle(RADAR_CX, RADAR_CY, r_px, 100);
    }
}

static void draw_sweep(void)
{
    /* Outer circle */
    fb_circle(RADAR_CX, RADAR_CY, RADAR_R, 80);
    /* Crosshairs */
    fb_line(RADAR_CX - RADAR_R, RADAR_CY,
            RADAR_CX + RADAR_R, RADAR_CY, 60);
    fb_line(RADAR_CX, RADAR_CY - RADAR_R,
            RADAR_CX, RADAR_CY + RADAR_R, 60);

    /* Bright sweep line */
    double rad = sweep_angle * 3.14159265 / 180.0;
    int x2 = RADAR_CX + (int)(RADAR_R * sin(rad));
    int y2 = RADAR_CY - (int)(RADAR_R * cos(rad));
    fb_line(RADAR_CX, RADAR_CY, x2, y2, 200);

    /* Trailing glow */
    for (int t = 1; t <= 5; t++) {
        double tr = (sweep_angle - t * 6) * 3.14159265 / 180.0;
        int xt = RADAR_CX + (int)(RADAR_R * sin(tr));
        int yt = RADAR_CY - (int)(RADAR_R * cos(tr));
        int brightness = 160 - t * 30;
        if (brightness < 20) brightness = 20;
        fb_line(RADAR_CX, RADAR_CY, xt, yt, (uint8_t)brightness);
    }

    /* Center dot */
    fb_pixel(RADAR_CX, RADAR_CY, 180);
    fb_pixel(RADAR_CX - 1, RADAR_CY, 180);
    fb_pixel(RADAR_CX + 1, RADAR_CY, 180);
    fb_pixel(RADAR_CX, RADAR_CY - 1, 180);
    fb_pixel(RADAR_CX, RADAR_CY + 1, 180);

    sweep_angle = (sweep_angle + 8) % 360;
}

static void draw_aircraft(void)
{
    int radius_nm = cur.ac_radius_nm;
    if (radius_nm <= 0) radius_nm = 25;

    draw_radar_base(radius_nm);

    for (int i = 0; i < cur.ac_count && i < DISP_MAX_AC; i++) {
        const disp_aircraft_t *ac = &cur.ac[i];
        if (ac->dir < 0 || ac->dst_x10 <= 0) continue;

        double dir_rad = ac->dir * 3.14159265 / 180.0;
        double dst_nm = ac->dst_x10 / 10.0;
        if (dst_nm > radius_nm) continue;

        double r_px = dst_nm / radius_nm * RADAR_R;
        int ax = RADAR_CX + (int)(r_px * sin(dir_rad));
        int ay = RADAR_CY - (int)(r_px * cos(dir_rad));

        /* Speed/heading line — length proportional to speed */
        if (ac->gs > 0 && ac->track >= 0) {
            double trk_rad = ac->track * 3.14159265 / 180.0;
            int line_len = ac->gs / 6;  /* ~12px at 73kts, ~25px at 150kts */
            if (line_len > 30) line_len = 30;
            if (line_len < 4) line_len = 4;
            int lx = ax + (int)(line_len * sin(trk_rad));
            int ly = ay - (int)(line_len * cos(trk_rad));
            fb_line(ax, ay, lx, ly, 200);
        }

        /* Dot (small: 3x3 cross) */
        fb_pixel(ax, ay, 255);
        fb_pixel(ax - 1, ay, 220);
        fb_pixel(ax + 1, ay, 220);
        fb_pixel(ax, ay - 1, 220);
        fb_pixel(ax, ay + 1, 220);

        /* Circle around nearest */
        if (i == 0)
            fb_circle(ax, ay, 5, 200);
    }

    /* Corner data for nearest aircraft */
    if (cur.ac_count > 0) {
        const disp_aircraft_t *nearest = &cur.ac[0];

        /* Top-left: callsign */
        fb_putstr(1, 1, nearest->callsign);

        /* Top-right: altitude + MSL */
        char alt_str[20];
        if (nearest->alt_100 < 0) {
            strcpy(alt_str, "?");
        } else if (nearest->alt_100 == 0) {
            strcpy(alt_str, "GND");
        } else {
            int alt = nearest->alt_100 * 100;
            if (alt >= 1000)
                snprintf(alt_str, sizeof(alt_str), "%d,%03d ft",
                         alt / 1000, alt % 1000);
            else
                snprintf(alt_str, sizeof(alt_str), "%d ft", alt);
        }
        int aw = fb_text_width(alt_str);
        fb_putstr(SHELBOURNE_FB_WIDTH - 1 - aw, 1, alt_str);
        if (nearest->alt_100 > 0) {
            int mw = fb_text_width("MSL");
            fb_putstr(SHELBOURNE_FB_WIDTH - 1 - mw, 15, "MSL");
        }

        /* Bottom-left: type */
        if (nearest->type[0])
            fb_putstr(1, SHELBOURNE_FB_HEIGHT - 13, nearest->type);

        /* Bottom-right: distance + speed */
        char dist_str[16];
        double dst = nearest->dst_x10 / 10.0;
        if (dst < 1.0)
            snprintf(dist_str, sizeof(dist_str), "%.1f nm", dst);
        else
            snprintf(dist_str, sizeof(dist_str), "%.0f nm", dst);
        int dw = fb_text_width(dist_str);
        fb_putstr(SHELBOURNE_FB_WIDTH - 1 - dw,
                  SHELBOURNE_FB_HEIGHT - 24, dist_str);

        if (nearest->gs > 0) {
            char spd_str[16];
            snprintf(spd_str, sizeof(spd_str), "%d kts", nearest->gs);
            int sw = fb_text_width(spd_str);
            fb_putstr(SHELBOURNE_FB_WIDTH - 1 - sw,
                      SHELBOURNE_FB_HEIGHT - 13, spd_str);
        }
    } else {
        fb_center_2x(38, "NO AIRCRAFT");
        fb_center_2x(58, "NEARBY");
    }
}

static int render(void)
{
    int scrolling = 0;
    const uint8_t *logo = (const uint8_t *)cur.logo;

    shelbourne_display_clear(disp_hw);

    switch (cur.mode) {
    case DISP_AIRPLAY:
        fb_center_4x(20, "AirPlay");
        break;

    case DISP_AUX:
        if (logo) display_logo(logo);
        break;

    case DISP_RADIO:
        if (!cur.playing) {
            if (logo) display_logo(logo);
            fb_center_2x(54, cur.text1);
        } else if (cur.text2[0]) {
            if (logo) display_logo(logo);
            scrolling = fb_scroll_2x(54, cur.text2);
        } else {
            if (logo) display_logo(logo);
            fb_center_2x(54, cur.text1);
        }
        break;

    case DISP_USB:
        if (!cur.playing) {
            /* paused */
        } else if (cur.track_total > 0) {
            char idx[16];
            snprintf(idx, sizeof(idx), "%d/%d",
                     cur.track_num, cur.track_total);
            fb_putstr(SHELBOURNE_FB_WIDTH - fb_text_width(idx), 0, idx);
            scrolling = fb_scroll_2x(14, cur.text1);
            int y = 14 + 21;
            if (cur.text2[0])
                y = fb_wrap_2x(y, 70, cur.text2);
            if (cur.text3[0])
                fb_wrap_2x(y + 2, 96, cur.text3);
        } else {
            fb_center_2x(18, "No tracks");
        }
        break;

    case DISP_PODCAST:
        if (logo) display_logo(logo);
        fb_center_2x(54, cur.text1);
        if (cur.text2[0])
            fb_center_2x(74, cur.text2);
        break;

    case DISP_CLOCK:
        if (sun_clock_visible(LAT, LON)) {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            if (tm) {
                char clk[6];
                snprintf(clk, sizeof(clk), "%02d:%02d",
                         tm->tm_hour, tm->tm_min);

                static const char *days[] =
                    {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                static const char *months[] =
                    {"Jan","Feb","Mar","Apr","May","Jun",
                     "Jul","Aug","Sep","Oct","Nov","Dec"};
                char date[20];
                snprintf(date, sizeof(date), "%s %s %d",
                         days[tm->tm_wday], months[tm->tm_mon],
                         tm->tm_mday);

                if (cur.temp_str[0]) {
                    /* Layout A: clock+date top, temp bottom */
                    fb_center_4x(4, clk);
                    fb_center_2x(42, date);
                    fb_center_1x(82, cur.temp_str);
                } else {
                    /* No temperature — original centered layout */
                    fb_center_4x(20, clk);
                    fb_center_2x(62, date);
                }
            }
        }
        break;

    case DISP_AIRCRAFT_SWEEP:
        draw_sweep();
        break;

    case DISP_AIRCRAFT:
        draw_aircraft();
        break;

    case DISP_BLANK:
        break;
    }

    if (vol_active) {
        if (mono_ms() - vol_show_ms >= VOL_BAR_MS)
            vol_active = 0;
        else
            draw_volume_bar();
    }

    shelbourne_display_flush(disp_hw);
    return scrolling;
}

static void displayd_main(shelbourne_t *hw, int pipe_rd)
{
    disp_hw = hw;
    memset(&cur, 0, sizeof(cur));
    cur.mode = DISP_BLANK;
    vol_active = 0;

    fcntl(pipe_rd, F_SETFL, O_NONBLOCK);

    /* Lowest priority so SPI flushes don't starve audio */
    nice(19);

    /* Parent handles signals; child ignores them */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    struct pollfd pfd = { .fd = pipe_rd, .events = POLLIN };
    int scrolling = 0;
    long last_clock_ms = 0;

    render();

    for (;;) {
        int timeout;
        if (cur.mode == DISP_AIRCRAFT_SWEEP)
            timeout = SWEEP_POLL_MS;
        else if (scrolling || vol_active)
            timeout = SCROLL_POLL_MS;
        else if (cur.mode == DISP_CLOCK)
            timeout = CLOCK_REFRESH_MS;
        else
            timeout = -1;

        poll(&pfd, 1, timeout);

        if (pfd.revents & (POLLHUP | POLLERR))
            break;

        /* Drain all pending messages, keep latest state */
        int got_msg = 0;
        if (pfd.revents & POLLIN) {
            display_msg_t msg;
            while (read(pipe_rd, &msg, sizeof(msg))
                   == (ssize_t)sizeof(msg)) {
                if (msg.volume >= 0) {
                    vol_level = msg.volume;
                    vol_active = 1;
                    vol_show_ms = mono_ms();
                }
                memcpy(&cur, &msg, sizeof(cur));
                got_msg = 1;
            }
        }

        int need_redraw = got_msg || scrolling || vol_active;

        if (cur.mode == DISP_AIRCRAFT_SWEEP)
            need_redraw = 1;

        if (cur.mode == DISP_CLOCK && !need_redraw) {
            long now = mono_ms();
            if (now - last_clock_ms >= CLOCK_REFRESH_MS) {
                need_redraw = 1;
                last_clock_ms = now;
            }
        }

        if (need_redraw) {
            scrolling = render();
            if (cur.mode == DISP_CLOCK)
                last_clock_ms = mono_ms();
        }
    }

    /* Blank display on exit */
    shelbourne_display_clear(disp_hw);
    shelbourne_display_flush(disp_hw);
    _exit(0);
}

int displayd_start(shelbourne_t *hw)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[1]);
        displayd_main(hw, pipefd[0]);
        _exit(0);
    }

    child_pid = pid;
    close(pipefd[0]);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
    return pipefd[1];
}

void displayd_send(int fd, const display_msg_t *msg)
{
    write(fd, msg, sizeof(*msg));
}

void displayd_stop(int fd)
{
    close(fd);
    if (child_pid > 0)
        waitpid(child_pid, NULL, 0);
    child_pid = 0;
}
