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
/* SYSDVR_AUDIODUMP=prefix: every byte handed to the sound card goes to prefix.out, and every ALSA problem (underrun...)
 * is logged with its time (a debugging aid for crackling and level changes) */
static FILE *g_dump;
static int g_dump_init;

typedef struct {
    snd_pcm_t *pcm;
    char dev[128];
    uint64_t retry_ns;          /* do not try to open the device again before this time */
    int complained;
    int16_t last[2];            /* the last frame written (where a fade to silence starts) */
    int started;                /* real audio has been written: only then is the card kept fed */
    int fade_in;                /* silence was written since the last real audio: its start fades in */
    unsigned long fed;          /* 2 ms chunks of fade/silence written by audio_keepalive (statistics) */
    int run;                    /* ... in a row since the last real audio: after a second the card is left alone */
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

static void write_frames(aud *a, const uint8_t *pcm, size_t bytes);

#define FADE_FRAMES 96          /* 2 ms */

/* real audio from the console; the first 2 ms after a stretch of silence fade in */
void audio_write(void *h, const uint8_t *pcm, size_t bytes)
{
    aud *a = h;
    if (a->fade_in && bytes >= (size_t)FADE_FRAMES * 4) {
        int16_t head[FADE_FRAMES * 2];
        memcpy(head, pcm, sizeof head);
        for (int i = 0; i < FADE_FRAMES; i++) { head[2 * i] = (int16_t)(head[2 * i] * i / FADE_FRAMES); head[2 * i + 1] = (int16_t)(head[2 * i + 1] * i / FADE_FRAMES); }
        write_frames(a, (const uint8_t *)head, sizeof head);
        pcm += sizeof head; bytes -= sizeof head;
    }
    a->fade_in = 0; a->run = 0;
    if (bytes >= 4) { memcpy(a->last, pcm + bytes - 4, 4); a->started = 1; }
    write_frames(a, pcm, bytes);
}

static void write_frames(aud *a, const uint8_t *pcm, size_t bytes)
{
    if (!g_dump_init) {
        g_dump_init = 1;
        const char *p = getenv("SYSDVR_AUDIODUMP");
        char path[256];
        if (p && snprintf(path, sizeof path, "%s.out", p) < (int)sizeof path) g_dump = fopen(path, "wb");
    }
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
            if (g_dump) fprintf(stderr, "audio: alsa error %d (%s) at %.3f s\n", (int)r, snd_strerror((int)r), now_ns() / 1e9);
            if (++recovers > 8 || snd_pcm_recover(a->pcm, (int)r, 1) < 0) goto broken;
            continue;
        }
        if (g_dump) fwrite(pcm, 4, (size_t)r, g_dump);
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

void audio_silence(void *h, int64_t us)
{
    static const uint8_t zero[4800 * 4];
    aud *a = h;
    int64_t frames = us * 48000 / 1000000;
    while (frames > 0) {
        int64_t n = frames > 4800 ? 4800 : frames;
        write_frames(a, zero, (size_t)n * 4);
        frames -= n;
    }
    memset(a->last, 0, sizeof a->last);
}

/* The card must be looked at again about when its queue falls to 8 ms; far from that the caller can sleep */
int audio_poll_ms(void *h)
{
    aud *a = h;
    snd_pcm_sframes_t d = 0;
    if (!a->pcm || !a->started || snd_pcm_delay(a->pcm, &d) < 0 || d < 0) return 250;
    int ms = (int)(d / 48) - 8;
    return ms < 1 ? 1 : ms > 40 ? 40 : ms;
}

void audio_keepalive(void *h)
{
    aud *a = h;
    snd_pcm_sframes_t d = 0;
    if (!a->pcm || !a->started || snd_pcm_delay(a->pcm, &d) < 0 || d < 0 || d > 384) return;     /* more than 8 ms are still queued */
    int16_t chunk[FADE_FRAMES * 2];
    for (int i = 0; i < FADE_FRAMES; i++) {
        chunk[2 * i] = (int16_t)(a->last[0] * (FADE_FRAMES - 1 - i) / FADE_FRAMES);
        chunk[2 * i + 1] = (int16_t)(a->last[1] * (FADE_FRAMES - 1 - i) / FADE_FRAMES);
    }
    memset(a->last, 0, sizeof a->last);               /* the first chunk ramps down, the next ones are zeros */
    a->fade_in = 1; a->fed++;
    if (++a->run > 500) a->started = 0;               /* no audio for a second: the console is paused or gone */
    write_frames(a, (const uint8_t *)chunk, sizeof chunk);
}

void audio_close(void *h)
{
    aud *a = h;
    if (g_dump) fprintf(stderr, "audio: kept fed with %lu chunks of 2 ms\n", a->fed);
    if (a->pcm) { snd_pcm_nonblock(a->pcm, 0); snd_pcm_drain(a->pcm); snd_pcm_close(a->pcm); }
    free(a);
}
