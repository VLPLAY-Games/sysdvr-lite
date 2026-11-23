#ifndef HDMI_WARNING_H
#define HDMI_WARNING_H
#include "hdmi_audio.h"
/* Opaque localized card, top right. Writes only inside the mapped XRGB8888 image. */
void hdmi_warning_draw(uint8_t *pixels, int width, int height, unsigned pitch, hdmi_warning warning);
#endif
