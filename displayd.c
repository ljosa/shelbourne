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
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VOL_BAR_MS       2000
#define SCROLL_POLL_MS   500
#define CLOCK_REFRESH_MS 30000

/* Burlington VT */
#define LAT  44.48
#define LON -73.21

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
                snprintf(clk, sizeof(clk), "%d:%02d",
                         tm->tm_hour, tm->tm_min);
                fb_center_4x(20, clk);

                static const char *days[] =
                    {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                static const char *months[] =
                    {"Jan","Feb","Mar","Apr","May","Jun",
                     "Jul","Aug","Sep","Oct","Nov","Dec"};
                char date[20];
                snprintf(date, sizeof(date), "%s %s %d",
                         days[tm->tm_wday], months[tm->tm_mon],
                         tm->tm_mday);
                fb_center_2x(62, date);
            }
        }
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
        if (scrolling || vol_active)
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
