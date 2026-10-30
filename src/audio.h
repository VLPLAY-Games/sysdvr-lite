#ifndef AUDIO_H
#define AUDIO_H
#include <stddef.h>
#include <stdint.h>
/* 48 kHz stereo s16le. Returns NULL if built without ALSA / device missing */
void *audio_open(const char *device);
void  audio_write(void *a, const uint8_t *pcm, size_t bytes);
int64_t audio_delay_us(void *a);              /* queued, not yet audible audio */
void  audio_silence(void *a, int64_t us);     /* delay the audio by inserting silence */
void  audio_close(void *a);
#endif
