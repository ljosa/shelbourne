/*
 * test_rate.c — measure the actual hardware playback rate.
 *
 * Plays N seconds of silence (assuming SHELBOURNE_SAMPLE_RATE), times the
 * actual wall-clock duration, and reports the implied real rate.
 */

#include "shelbourne.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SECONDS 30
#define CHUNK   SHELBOURNE_FRAMES_PER_WRITE

int main(void)
{
    shelbourne_t *hw = shelbourne_init();
    if (!hw) {
        fprintf(stderr, "shelbourne_init failed\n");
        return 1;
    }

    int total_frames = SHELBOURNE_SAMPLE_RATE * SECONDS;
    int32_t buf[CHUNK * 2];
    memset(buf, 0, sizeof(buf));

    /* Prime the buffer briefly so the first wait isn't measuring init. */
    shelbourne_audio_write(hw, buf, CHUNK);
    shelbourne_audio_wait(hw);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int written = 0;
    while (written < total_frames) {
        int n = (total_frames - written < CHUNK) ? total_frames - written : CHUNK;
        shelbourne_audio_write(hw, buf, n);
        shelbourne_audio_wait(hw);
        written += n;
    }
    shelbourne_audio_drain(hw);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;

    double implied_rate = total_frames / elapsed;
    double pitch_ratio  = implied_rate / SHELBOURNE_SAMPLE_RATE;
    double semitones    = 12.0 * log2(pitch_ratio);

    printf("Wrote %d frames at SHELBOURNE_SAMPLE_RATE=%d\n",
           total_frames, SHELBOURNE_SAMPLE_RATE);
    printf("Elapsed: %.3f s (expected %d.000 s)\n", elapsed, SECONDS);
    printf("Implied hardware rate: %.1f Hz\n", implied_rate);
    printf("Pitch shift vs 48kHz assumption: %+.2f semitones\n", semitones);

    shelbourne_shutdown(hw);
    return 0;
}
