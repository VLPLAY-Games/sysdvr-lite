#include "audio.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The output device can be missing at start (the HDMI sound card is not ready yet) or vanish later
 * (the display was switched off). The handle is therefore always valid: a closed device is opened
 * again once a second, and a device that stops answering is closed instead of blocking the thread. */
typedef struct {
    snd_pcm_t *pcm;
    char dev[128];
    uint64_t retry_ns;          /* do not try to open the device again before this time */
    int complained;
} aud;

static uint64_t now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

/* alsa-lib prints its own error for every failed open: ours is the one message that is worth reading */
static void quiet(const char *file, int line, const char *func, int err, const char *fmt, ...) {}

static int dev_open(aud *a)
{
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, a->dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) return -1;
    /* small buffer: a long one only adds delay, the console delivers audio every ~21-85 ms */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 50000) < 0) {
        snd_pcm_close(pcm);
        return -1;
    }
    a->pcm = pcm;
    return 0;
}

static void dev_close(aud *a)
{
    if (a->pcm) snd_pcm_close(a->pcm);
    a->pcm = NULL;
    a->retry_ns = now_ns() + 1000000000ull;
}

static int ensure_open(aud *a)
{
    if (a->pcm) return 0;
    if (now_ns() < a->retry_ns) return -1;
    if (dev_open(a) == 0) {
        fprintf(stderr, "audio: %s opened\n", a->dev);
        a->complained = 0;
        return 0;
    }
    a->retry_ns = now_ns() + 1000000000ull;
    if (!a->complained) { fprintf(stderr, "audio: cannot open %s, will keep trying\n", a->dev); a->complained = 1; }
    return -1;
}

void *audio_open(const char *device)
{
    aud *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    snd_lib_error_set_handler(quiet);
    snprintf(a->dev, sizeof a->dev, "%s", device ? device : "default");
    ensure_open(a);
    return a;
}

void audio_write(void *h, const uint8_t *pcm, size_t bytes)
{
    aud *a = h;
    if (ensure_open(a)) return;
    snd_pcm_uframes_t left = bytes / 4;
    int stalls = 0, recovers = 0;
    while (left > 0) {
        snd_pcm_sframes_t r = snd_pcm_writei(a->pcm, pcm, left);
        if (r == -EAGAIN) {                                     /* buffer is full: wait until the card drains it */
            if (snd_pcm_wait(a->pcm, 100) <= 0 && ++stalls >= 5) goto broken;
            continue;
        }
        if (r < 0) {                                            /* underrun, suspend... or the device is gone */
            if (++recovers > 8 || snd_pcm_recover(a->pcm, (int)r, 1) < 0) goto broken;
            continue;
        }
        pcm += (size_t)r * 4; left -= (snd_pcm_uframes_t)r; stalls = 0;
    }
    return;
broken:
    fprintf(stderr, "audio: %s stopped working, closing it and trying again\n", a->dev);
    dev_close(a);
}

int64_t audio_delay_us(void *h)
{
    aud *a = h;
    snd_pcm_sframes_t d = 0;
    if (!a->pcm || snd_pcm_delay(a->pcm, &d) < 0 || d < 0) return 0;
    return (int64_t)d * 1000000 / 48000;
}

void audio_silence(void *a, int64_t us)
{
    static const uint8_t zero[4800 * 4];
    int64_t frames = us * 48000 / 1000000;
    while (frames > 0) {
        int64_t n = frames > 4800 ? 4800 : frames;
        audio_write(a, zero, (size_t)n * 4);
        frames -= n;
    }
}

void audio_close(void *h)
{
    aud *a = h;
    if (a->pcm) { snd_pcm_nonblock(a->pcm, 0); snd_pcm_drain(a->pcm); snd_pcm_close(a->pcm); }
    free(a);
}
