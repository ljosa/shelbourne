/*
 * airplay.c — Player-side AirPlay interface (FIFO reader)
 *
 * The actual RAOP server and mDNS advertisement run in the separate
 * airplayd process.  This module simply opens the shared FIFO and
 * provides the same API the player expects: is_streaming / read / volume.
 */

#include "airplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>

#define FIFO_PATH "/tmp/airplay.pcm"
#define VOL_PATH  "/tmp/airplay.vol"

/* How long after last data before we consider the stream ended */
#define STREAM_TIMEOUT_MS 2000

static int fifo_fd = -1;
static struct timespec last_data;
static int ever_received;  /* true once we've ever read data */

int airplay_init(const char *name)
{
    (void)name;

    /* Create FIFO if it doesn't exist yet (airplayd also creates it) */
    mkfifo(FIFO_PATH, 0666);

    /* O_RDWR avoids blocking on open and prevents EOF when writer closes */
    fifo_fd = open(FIFO_PATH, O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0) {
        perror("airplay: open fifo");
        return -1;
    }

    ever_received = 0;
    clock_gettime(CLOCK_MONOTONIC, &last_data);
    printf("AirPlay: FIFO reader ready\n");
    return 0;
}

int airplay_is_streaming(void)
{
    if (fifo_fd < 0) return 0;

    /* Check if there's data available right now */
    struct pollfd pfd = { .fd = fifo_fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
        return 1;

    /* If we've ever received data, check timeout */
    if (ever_received) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed_ms = (now.tv_sec - last_data.tv_sec) * 1000LL
                             + (now.tv_nsec - last_data.tv_nsec) / 1000000LL;
        return elapsed_ms < STREAM_TIMEOUT_MS;
    }

    return 0;
}

int airplay_read(void *buf, int maxlen)
{
    if (fifo_fd < 0) return -1;

    /* Wait up to 50ms for data */
    struct pollfd pfd = { .fd = fifo_fd, .events = POLLIN };
    if (poll(&pfd, 1, 50) <= 0)
        return 0;

    int n = read(fifo_fd, buf, maxlen);
    if (n > 0) {
        clock_gettime(CLOCK_MONOTONIC, &last_data);
        ever_received = 1;
    }
    return n > 0 ? n : 0;
}

int airplay_volume(void)
{
    FILE *f = fopen(VOL_PATH, "r");
    if (!f) return -1;
    int vol = -1;
    if (fscanf(f, "%d", &vol) != 1)
        vol = -1;
    fclose(f);
    return vol;
}

void airplay_stop(void)
{
    if (fifo_fd >= 0) {
        close(fifo_fd);
        fifo_fd = -1;
    }
    ever_received = 0;
    printf("AirPlay: stopped\n");
}
