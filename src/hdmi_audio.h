#ifndef HDMI_AUDIO_H
#define HDMI_AUDIO_H
#include <stddef.h>
#include <stdint.h>

typedef enum { HDMI_AUDIO_UNKNOWN, HDMI_AUDIO_SUPPORTED, HDMI_AUDIO_UNSUPPORTED } hdmi_audio_support;
typedef enum { HDMI_WARNING_NONE, HDMI_WARNING_UNSUPPORTED, HDMI_WARNING_UNKNOWN, HDMI_WARNING_OUTPUT } hdmi_warning;
/* The stream is stereo LPCM, 48 kHz, 16 bit. Invalid/incomplete EDID is unknown. */
hdmi_audio_support hdmi_audio_parse_edid(const uint8_t *data, size_t size);
void hdmi_audio_enable(int enabled);
int hdmi_audio_enabled(void);
void hdmi_audio_set_support(hdmi_audio_support support);
void hdmi_audio_set_output(int available);
hdmi_warning hdmi_audio_warning(void);
const char *hdmi_audio_message(hdmi_warning warning, int russian);
/* Per-display/session timer; a new warning gets eight seconds of video time. */
typedef struct { int previous; uint64_t until; } hdmi_warning_timer;
int hdmi_audio_overlay_visible(hdmi_warning_timer *timer, hdmi_warning warning, uint64_t now);
#endif
