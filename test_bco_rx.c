/*
 * test_bco_rx.c — check whether the BCO's AirPlay audio reaches snd_shelby.
 *
 * Switches the snd_shelby source to "airplay" (RX mode 5, like AUX uses
 * mode 2), reads input buffers for N seconds and prints the peak and RMS
 * level once per second.  With "play" it also loops the input to the
 * speaker, the way shelbourne_aux_pump() does.  Restores TX mode on exit.
 *
 * Usage: test_bco_rx [seconds] [play|measure] [mode]
 *        mode is a snd_shelby source string, default "airplay" (or "aux")
 */

#include "shelbourne.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SOURCE_SYSFS "/sys/devices/virtual/sound/snd_shelby/source"
#define AUDIO_DEV    "/dev/snd/snd_shelby"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int write_source(const char *mode)
{
    int fd = open(SOURCE_SYSFS, O_WRONLY);
    if (fd < 0)
        return -1;
    /* Exact length, no newline: the driver uses strncmp */
    ssize_t n = write(fd, mode, strlen(mode));
    close(fd);
    return n == (ssize_t)strlen(mode) ? 0 : -1;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int play = argc > 2 && strcmp(argv[2], "play") == 0;
    const char *mode = argc > 3 ? argv[3] : "airplay";

    /* Restore TX mode even when killed */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    shelbourne_t *hw = shelbourne_init();
    if (!hw) {
        fprintf(stderr, "shelbourne_init failed\n");
        return 1;
    }

    if (write_source(mode) < 0) {
        fprintf(stderr, "cannot set source to %s: %s\n", mode, strerror(errno));
        shelbourne_shutdown(hw);
        return 1;
    }
    usleep(200000);

    int rx = open(AUDIO_DEV, O_RDWR);
    if (rx < 0) {
        fprintf(stderr, "cannot open %s for RX: %s\n", AUDIO_DEV, strerror(errno));
        shelbourne_aux_disable(hw);
        shelbourne_shutdown(hw);
        return 1;
    }
    printf("source=%s, reading %d s%s\n", mode, seconds, play ? " (looping to speaker)" : "");

    struct timespec t0, now, last;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    last = t0;

    uint8_t raw[SHELBOURNE_WRITE_SIZE];
    long frames = 0, reads = 0, short_reads = 0;
    double sumsq = 0;
    int32_t peak = 0;

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (stop || now.tv_sec - t0.tv_sec >= seconds)
            break;

        ssize_t n = read(rx, raw, sizeof(raw));
        if (n < 0) {
            if (errno == EINTR)
                continue;   /* loop re-checks stop */
            fprintf(stderr, "read: %s\n", strerror(errno));
            break;
        }
        reads++;
        if (n != (ssize_t)sizeof(raw))
            short_reads++;

        int32_t *s = (int32_t *)raw;
        int count = n / (int)sizeof(int32_t);
        for (int i = 0; i < count; i++) {
            int32_t v = s[i] < 0 ? -s[i] : s[i];
            if (v > peak)
                peak = v;
            double f = s[i] / 2147483648.0;
            sumsq += f * f;
        }
        frames += count / 2;

        if (play && count > 0) {
            shelbourne_audio_write(hw, s, count / 2);
            shelbourne_audio_wait(hw);
        }

        if (now.tv_sec != last.tv_sec) {
            double rms = frames ? sqrt(sumsq / (frames * 2)) : 0;
            printf("t=%2lds reads=%ld short=%ld frames=%ld peak=%.4f rms_dbfs=%.1f\n",
                   (long)(now.tv_sec - t0.tv_sec), reads, short_reads, frames,
                   peak / 2147483648.0, rms > 0 ? 20 * log10(rms) : -999.0);
            fflush(stdout);
            reads = short_reads = frames = 0;
            sumsq = 0;
            peak = 0;
            last = now;
        }
    }

    close(rx);
    /* aux_disable writes "wifi", clears txRunning and restarts TX */
    shelbourne_aux_disable(hw);
    shelbourne_shutdown(hw);
    return 0;
}
