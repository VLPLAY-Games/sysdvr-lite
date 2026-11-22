#ifndef WAITING_SCREEN_H
#define WAITING_SCREEN_H
#include "hdmi_audio.h"
/* Static XRGB8888 panel. Render once per state change; KMS keeps scanning it out. */
void waiting_screen_draw(uint8_t *pixels, int width, int height, unsigned pitch,
                         const char *decoder, const char *audio, int gpu, int game_missing,
                         hdmi_warning warning, const char *version);
#endif
