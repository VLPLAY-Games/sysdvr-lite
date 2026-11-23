#include "hdmi_audio.h"
#include <stdatomic.h>
#include <string.h>

static atomic_int enabled, support = HDMI_AUDIO_UNKNOWN, output_ok = 1;

static int checksum_ok(const uint8_t *p)
{
    unsigned sum = 0;
    for (int i = 0; i < 128; i++) sum += p[i];
    return !(sum & 255);
}

hdmi_audio_support hdmi_audio_parse_edid(const uint8_t *data, size_t size)
{
    static const uint8_t header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
    if (!data || size < 128 || memcmp(data, header, sizeof header) || !checksum_ok(data))
        return HDMI_AUDIO_UNKNOWN;
    size_t blocks = (size_t)data[126] + 1;
    if (blocks > size / 128) return HDMI_AUDIO_UNKNOWN;
    int supported = 0, uncertain = 0;
    for (size_t i = 1; i < blocks; i++) {
        const uint8_t *p = data + i * 128;
        if (!checksum_ok(p)) return HDMI_AUDIO_UNKNOWN;
        if (p[0] != 2) { uncertain = 1; continue; } /* Other extension formats are not decoded here. */
        if (p[1] < 3) { uncertain = 1; continue; }
        if (p[3] & 0x40) supported = 1; /* CTA basic audio includes stereo 16-bit LPCM at 48 kHz. */
        unsigned end = p[2];
        if (!end) continue; /* No data block collection or detailed timings. */
        if (end < 4 || end > 127) return HDMI_AUDIO_UNKNOWN;
        for (unsigned pos = 4; pos < end;) {
            unsigned tag = p[pos] >> 5, len = p[pos] & 31;
            if (pos + 1 + len > end) return HDMI_AUDIO_UNKNOWN;
            if (tag == 1) {
                if (!len || len % 3) return HDMI_AUDIO_UNKNOWN;
                for (unsigned j = pos + 1; j < pos + 1 + len; j += 3) {
                    unsigned format = (p[j] >> 3) & 15, channels = (p[j] & 7) + 1;
                    if (format == 1 && channels >= 2 && (p[j + 1] & 4) && (p[j + 2] & 1)) supported = 1;
                }
            }
            pos += 1 + len;
        }
    }
    return supported ? HDMI_AUDIO_SUPPORTED : uncertain ? HDMI_AUDIO_UNKNOWN : HDMI_AUDIO_UNSUPPORTED;
}

void hdmi_audio_enable(int value) { atomic_store(&enabled, value); }
int hdmi_audio_enabled(void) { return atomic_load(&enabled); }
void hdmi_audio_set_support(hdmi_audio_support value) { atomic_store(&support, value); }
void hdmi_audio_set_output(int available) { atomic_store(&output_ok, available); }
int hdmi_audio_output_available(void) { return atomic_load(&output_ok); }
hdmi_warning hdmi_audio_warning(void)
{
    if (!atomic_load(&enabled)) return HDMI_WARNING_NONE;
    int s = atomic_load(&support);
    if (s == HDMI_AUDIO_UNSUPPORTED) return HDMI_WARNING_UNSUPPORTED;
    if (!atomic_load(&output_ok)) return HDMI_WARNING_OUTPUT;
    return s == HDMI_AUDIO_SUPPORTED ? HDMI_WARNING_NONE : HDMI_WARNING_UNKNOWN;
}

const char *hdmi_audio_message(hdmi_warning warning, int russian)
{
    switch (warning) {
    case HDMI_WARNING_UNSUPPORTED:
        return russian ? "Дисплей не поддерживает звук HDMI (стерео 48 кГц)."
                       : "Display does not support HDMI audio (stereo 48 kHz).";
    case HDMI_WARNING_UNKNOWN:
        return russian ? "Не удалось определить поддержку HDMI-аудио."
                       : "Could not determine HDMI audio support.";
    case HDMI_WARNING_OUTPUT:
        return russian ? "Аудиовыход HDMI недоступен. Повторяем подключение."
                       : "HDMI audio output unavailable. Retrying connection.";
    default: return NULL;
    }
}

int hdmi_audio_overlay_visible(hdmi_warning_timer *timer, hdmi_warning warning, uint64_t now)
{
    if (timer->previous != (int)warning) {
        timer->previous = warning;
        timer->until = warning == HDMI_WARNING_NONE ? 0 : now + 8000000000ull;
    }
    return warning != HDMI_WARNING_NONE && now < timer->until;
}
