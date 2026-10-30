#include "audio.h"
#include <alsa/asoundlib.h>
#include <stdio.h>

void *audio_open(const char *device)
{
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, device ? device : "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) return NULL;
    /* small buffer: a long one only adds delay, the console delivers audio every ~21-85 ms */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 50000) < 0) {
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

void audio_write(void *a, const uint8_t *pcm, size_t bytes)
{
    snd_pcm_sframes_t r = snd_pcm_writei(a, pcm, bytes / 4);
    if (r < 0) snd_pcm_recover(a, (int)r, 1);
}

int64_t audio_delay_us(void *a)
{
    snd_pcm_sframes_t d = 0;
    if (snd_pcm_delay(a, &d) < 0 || d < 0) return 0;
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

void audio_close(void *a) { snd_pcm_drain(a); snd_pcm_close(a); }
