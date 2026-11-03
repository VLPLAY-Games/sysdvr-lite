#ifndef AUDIO_H
#define AUDIO_H
#include <stddef.h>
#include <stdint.h>
/* 48 kHz stereo s16le. Returns NULL if built without ALSA / device missing */
void *audio_open(const char *device);
void  audio_write(void *a, const uint8_t *pcm, size_t bytes);
int64_t audio_delay_us(void *a);              /* queued, not yet audible audio */
void  audio_silence(void *a, int64_t us);     /* delay the audio by inserting silence */
/* Called when no audio is waiting: keeps the sound card fed with a short fade to silence while a packet is late, so
 * that it never runs dry (a dry card restarts with a click); the next real audio fades in. Adds no delay. */
void  audio_keepalive(void *a);
int   audio_poll_ms(void *a);                  /* how long the caller may wait for audio before calling audio_keepalive again */
void  audio_close(void *a);
#endif
