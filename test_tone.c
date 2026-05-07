/*
 * test_tone.c — play a 440 Hz sine wave for 15 seconds.
 *
 * Hold a tuner app near the speaker. Expected readings:
 *   - 440 Hz (A4): hardware actually runs at 48000 Hz
 *   - 429.7 Hz (A4 -40 cents): hardware runs at 46875 Hz (24 MHz / 512)
 *   - 404.2 Hz (G#4 +50 cents): hardware runs at 44100 Hz
 */

#include "shelbourne.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FREQ_HZ   440.0
#define SECONDS   15
#define AMPLITUDE 0.04  /* 4% of full scale */

int main(void)
{
    shelbourne_t *hw = shelbourne_init();
    if (!hw) {
        fprintf(stderr, "shelbourne_init failed\n");
        return 1;
    }

    fprintf(stderr, "Playing %g Hz for %d seconds at %d sps assumed...\n",
            FREQ_HZ, SECONDS, SHELBOURNE_SAMPLE_RATE);

    int total = SHELBOURNE_SAMPLE_RATE * SECONDS;
    int chunk = SHELBOURNE_FRAMES_PER_WRITE;
    int32_t buf[chunk * 2];

    double phase = 0.0;
    double step  = 2.0 * M_PI * FREQ_HZ / SHELBOURNE_SAMPLE_RATE;
    int32_t scale = (int32_t)(0x7fffffff * AMPLITUDE);

    int written = 0;
    while (written < total) {
        int n = (total - written < chunk) ? total - written : chunk;
        for (int i = 0; i < n; i++) {
            int32_t s = (int32_t)(scale * sin(phase));
            buf[i * 2 + 0] = s;
            buf[i * 2 + 1] = s;
            phase += step;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        shelbourne_audio_write(hw, buf, n);
        shelbourne_audio_wait(hw);
        written += n;
    }
    shelbourne_audio_drain(hw);
    shelbourne_shutdown(hw);
    return 0;
}
