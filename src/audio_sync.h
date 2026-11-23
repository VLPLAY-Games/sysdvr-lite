#ifndef AUDIO_SYNC_H
#define AUDIO_SYNC_H
#include <stddef.h>
#include <stdint.h>

/* Stereo S16/48 kHz packets. A timestamp jump is not ordinary A/V drift:
 * the samples already queued in ALSA no longer form a continuous timeline
 * with this packet. Re-anchor at it, without changing the console clock. */
typedef struct { uint64_t end_us; int have_end; } audio_sync;

static inline int audio_sync_ready(audio_sync *s, uint64_t timestamp,
                                   size_t bytes, int64_t delay_us)
{
    uint64_t delta = timestamp >= s->end_us ? timestamp - s->end_us : s->end_us - timestamp;
    /* Packet durations are rounded to microseconds by the console. */
    int continuous = !s->have_end || delta <= 2000;
    uint64_t duration = (uint64_t)(bytes / 4) * 1000000 / 48000;
    s->have_end = timestamp <= UINT64_MAX - duration;
    s->end_us = s->have_end ? timestamp + duration : 0;
    return continuous && delay_us >= 0;
}
#endif
