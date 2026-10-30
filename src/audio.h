#ifndef AUDIO_H
#define AUDIO_H
#include <stddef.h>
#include <stdint.h>
/* 48 kHz stereo s16le. Returns NULL if built without ALSA / device missing */
void *audio_open(const char *device);
void  audio_write(void *a, const uint8_t *pcm, size_t bytes);
void  audio_close(void *a);
#endif
