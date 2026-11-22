#include "hdmi_warning.h"
#include "font.h"
#include "ui_language.h"
#include <string.h>

static uint32_t codepoint(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    if (c >= 0xE0) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
    else if (c >= 0xC0) { c = (c & 31) << 6 | (s[0] & 63); s++; }
    *p = (const char *)s;
    return c;
}

static void text(uint8_t *pixels, int W, int H, unsigned pitch, int x, int y, const char *s, uint32_t color, int scale)
{
    while (*s) {
        const uint16_t *glyph = font_glyph(codepoint(&s));
        for (int row = 0; row < FONT_CELL_H; row++)
            for (int col = 0; col < FONT_CELL_W; col++) {
                if (!(glyph[row] & (1u << (15 - col)))) continue;
                for (int dy = 0; dy < scale; dy++) {
                    int py = y + row * scale + dy;
                    if (py < 0 || py >= H) continue;
                    uint32_t *line = (uint32_t *)(pixels + (size_t)py * pitch);
                    for (int dx = 0; dx < scale; dx++) {
                        int px = x + col * scale + dx;
                        if (px >= 0 && px < W) line[px] = color;
                    }
                }
            }
        x += FONT_CELL_W * scale;
    }
}

void hdmi_warning_draw(uint8_t *pixels, int W, int H, unsigned pitch, hdmi_warning warning)
{
    if (warning == HDMI_WARNING_NONE || W <= 0 || H <= 0) return;
    const char *ru, *en, *detail_ru, *detail_en;
    if (warning == HDMI_WARNING_UNSUPPORTED) {
        ru = "Звук по HDMI не поддерживается";
        en = "HDMI audio is not supported";
        detail_ru = "Нужен стереозвук 48 кГц";
        detail_en = "Stereo 48 kHz audio required";
    } else if (warning == HDMI_WARNING_OUTPUT) {
        ru = "Аудиовыход HDMI недоступен";
        en = "HDMI audio output unavailable";
        detail_ru = "Повторяем подключение";
        detail_en = "Retrying connection";
    } else {
        ru = "Поддержка HDMI-аудио неизвестна";
        en = "HDMI audio support unknown";
        detail_ru = "Нет корректных данных дисплея";
        detail_en = "Display data unavailable or invalid";
    }
    int scale = W >= 1920 && H >= 1000 ? 2 : 1;
    int margin = 16 * scale, pad = 12 * scale, longest = 0;
    int russian = ui_is_russian();
    const char *lines[] = { russian ? ru : en, russian ? detail_ru : detail_en };
    for (int i = 0; i < 2; i++) {
        const char *p = lines[i]; int count = 0;
        while (*p) { codepoint(&p); count++; }
        if (count > longest) longest = count;
    }
    int card_w = longest * FONT_CELL_W * scale + pad * 2;
    int card_h = (FONT_CELL_H + 5) * scale * 2 + pad * 2;
    int x = W - margin - card_w, y = margin;
    if (x < 0) x = 0;
    for (int py = y; py < y + card_h && py < H; py++) {
        uint32_t *line = (uint32_t *)(pixels + (size_t)py * pitch);
        for (int px = x; px < x + card_w && px < W; px++)
            line[px] = px < x + 4 * scale ? 0x00FFC04D : 0x0021262D;
    }
    for (int i = 0; i < 2; i++)
        text(pixels, W, H, pitch, x + pad, y + pad + i * (FONT_CELL_H + 5) * scale,
             lines[i], (i == 0) ? 0x00FFC04D : 0x00E5E7EB, scale);
}
