#ifndef AIRPLAY_H
#define AIRPLAY_H

/*
 * AirPlay receiver interface (player side).
 *
 * The actual RAOP server runs in the separate airplayd process.
 * This module reads decoded PCM audio from a shared FIFO.
 */

/* Open the AirPlay FIFO.  name is ignored (kept for API compat).
 * Returns 0 on success, -1 on error. */
int airplay_init(const char *name);

/* Returns 1 if a client is currently streaming audio. */
int airplay_is_streaming(void);

/* Read decoded PCM audio from the AirPlay stream.
 * buf:     output buffer for 16-bit interleaved stereo PCM
 * maxlen:  buffer size in bytes
 * Returns bytes read, 0 on timeout (no data), -1 if stream ended. */
int airplay_read(void *buf, int maxlen);

/* Get the current volume (0-100 scale, or -1 if not set). */
int airplay_volume(void);

/* Close the FIFO. */
void airplay_stop(void);

#endif /* AIRPLAY_H */
