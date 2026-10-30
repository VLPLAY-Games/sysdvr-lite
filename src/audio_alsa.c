#include "audio.h"
#include <alsa/asoundlib.h>
#include <stdio.h>

void *audio_open(const char *device)
{
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, device ? device : "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) return NULL;
    /* 100 ms latency: the console already does not give real time audio */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 100000) < 0) {
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

void audio_close(void *a) { snd_pcm_drain(a); snd_pcm_close(a); }
