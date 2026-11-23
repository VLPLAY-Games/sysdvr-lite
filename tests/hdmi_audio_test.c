#define _POSIX_C_SOURCE 200809L
#include "hdmi_audio.h"
#include "ui_language.h"
#include "hdmi_warning.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t edid[384];
static void checksum(uint8_t *p)
{
    unsigned sum = 0;
    for (int i = 0; i < 127; i++) sum += p[i];
    p[127] = (uint8_t)(0 - sum);
}
static void base(int extensions)
{
    memset(edid, 0, sizeof edid);
    memset(edid + 1, 255, 6);
    edid[18] = 1; edid[19] = 4; edid[126] = extensions;
    checksum(edid);
}
static void cta(void)
{
    base(1); edid[128] = 2; edid[129] = 3; edid[130] = 8;
    edid[132] = 0x23; edid[133] = 0x09; edid[134] = 4; edid[135] = 1;
    checksum(edid + 128);
}
static void parse_tests(void)
{
    assert(hdmi_audio_parse_edid(NULL, 0) == HDMI_AUDIO_UNKNOWN);
    base(0);
    assert(hdmi_audio_parse_edid(edid, 128) == HDMI_AUDIO_UNSUPPORTED);
    edid[0] = 1;
    assert(hdmi_audio_parse_edid(edid, 128) == HDMI_AUDIO_UNKNOWN);
    cta();
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_SUPPORTED);
    for (size_t size = 0; size < 256; size++)
        assert(hdmi_audio_parse_edid(edid, size) == HDMI_AUDIO_UNKNOWN);
    edid[135] = 0; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNSUPPORTED); /* No 16 bit. */
    cta(); edid[134] = 2; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNSUPPORTED); /* No 48 kHz. */
    cta(); edid[133] = 0x08; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNSUPPORTED); /* Mono only. */
    cta(); edid[133] = 0x11; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNSUPPORTED); /* Compressed only. */
    base(1); edid[128] = 2; edid[129] = 3; edid[131] = 0x40; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_SUPPORTED); /* Basic audio, no SAD. */
    cta(); edid[132] = 0x22; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNKNOWN); /* Partial SAD. */
    cta(); edid[130] = 6; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNKNOWN); /* Block overruns collection. */
    cta(); edid[130] = 128; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNKNOWN);
    cta(); edid[150] ^= 1;
    assert(hdmi_audio_parse_edid(edid, 256) == HDMI_AUDIO_UNKNOWN); /* Bad checksum. */
    cta(); edid[126] = 2; checksum(edid); edid[256] = 0x70; checksum(edid + 256);
    assert(hdmi_audio_parse_edid(edid, sizeof edid) == HDMI_AUDIO_SUPPORTED);
    edid[134] = 0; checksum(edid + 128);
    assert(hdmi_audio_parse_edid(edid, sizeof edid) == HDMI_AUDIO_UNKNOWN); /* Unparsed extension. */
    /* Every collection offset/length combination, with valid checksums, under sanitizers. */
    for (unsigned end = 0; end < 256; end++)
        for (unsigned header = 0; header < 256; header++) {
            cta(); edid[130] = end; edid[132] = header; checksum(edid + 128);
            hdmi_audio_parse_edid(edid, 256);
        }
}
static void state_tests(void)
{
    hdmi_audio_enable(1);
    hdmi_audio_set_support(HDMI_AUDIO_SUPPORTED);
    hdmi_audio_set_output(1);
    assert(hdmi_audio_warning() == HDMI_WARNING_NONE);
    hdmi_audio_set_support(HDMI_AUDIO_UNSUPPORTED);
    assert(hdmi_audio_warning() == HDMI_WARNING_UNSUPPORTED);
    hdmi_audio_set_output(0);
    assert(hdmi_audio_warning() == HDMI_WARNING_UNSUPPORTED);
    hdmi_audio_set_support(HDMI_AUDIO_SUPPORTED);
    assert(hdmi_audio_warning() == HDMI_WARNING_OUTPUT);
    hdmi_audio_set_output(1);
    assert(hdmi_audio_warning() == HDMI_WARNING_NONE);
    hdmi_audio_set_support(HDMI_AUDIO_UNKNOWN);
    assert(hdmi_audio_warning() == HDMI_WARNING_UNKNOWN);
    hdmi_audio_enable(0);
    assert(hdmi_audio_warning() == HDMI_WARNING_NONE); /* --no-audio / non-HDMI output. */
    hdmi_warning_timer timer = { -1, 0 };
    uint64_t t = 1000000000ull;
    assert(hdmi_audio_overlay_visible(&timer, HDMI_WARNING_UNSUPPORTED, t));
    assert(hdmi_audio_overlay_visible(&timer, HDMI_WARNING_UNSUPPORTED, t + 7999999999ull));
    assert(!hdmi_audio_overlay_visible(&timer, HDMI_WARNING_UNSUPPORTED, t + 8000000000ull));
    assert(!hdmi_audio_overlay_visible(&timer, HDMI_WARNING_UNSUPPORTED, t + 16000000000ull));
    assert(hdmi_audio_overlay_visible(&timer, HDMI_WARNING_OUTPUT, t + 17000000000ull));
    assert(!hdmi_audio_overlay_visible(&timer, HDMI_WARNING_NONE, t + 18000000000ull));
    timer.previous = -1; /* Reconnect / new stream. */
    assert(hdmi_audio_overlay_visible(&timer, HDMI_WARNING_UNSUPPORTED, t + 19000000000ull));
}
static void draw_tests(const char *preview)
{
    const int sizes[][2] = { {1280,720}, {1920,1080}, {3840,2160}, {31,25} };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int W = sizes[i][0], H = sizes[i][1]; unsigned pitch = W * 4 + 16;
        size_t n = (size_t)pitch * H;
        uint8_t *p = malloc(n + 32); assert(p); memset(p, 0x67, n + 32);
        hdmi_warning_draw(p, W, H, pitch, HDMI_WARNING_NONE);
        for (size_t j = 0; j < n + 32; j++) assert(p[j] == 0x67);
        for (int warning = HDMI_WARNING_UNSUPPORTED; warning <= HDMI_WARNING_OUTPUT; warning++) {
            hdmi_warning_draw(p, W, H, pitch, warning);
            for (size_t j = n; j < n + 32; j++) assert(p[j] == 0x67);
            for (int y = 0; y < H; y++)
                for (unsigned j = W * 4; j < pitch; j++) assert(p[(size_t)y * pitch + j] == 0x67);
        }
        if (i == 0 && preview) {
            memset(p, 0x67, n);
            hdmi_warning_draw(p, W, H, pitch, HDMI_WARNING_UNSUPPORTED);
            FILE *f = fopen(preview, "wb"); assert(f);
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                const uint8_t *px = p + (size_t)y * pitch + x * 4;
                fputc(px[2], f); fputc(px[1], f); fputc(px[0], f);
            }
            fclose(f);
        }
        free(p);
    }
}
int main(int argc, char **argv)
{
    assert(!setenv("SYSDVR_LANGUAGE", "invalid", 1));
    ui_language_init(); assert(!ui_is_russian());
    parse_tests(); state_tests(); draw_tests(argc > 1 ? argv[1] : NULL);
    assert(!setenv("SYSDVR_LANGUAGE", "ru", 1));
    ui_language_init(); assert(ui_is_russian());
    draw_tests(NULL);
    assert(!setenv("SYSDVR_LANGUAGE", "en", 1));
    ui_language_init(); assert(!ui_is_russian());
    puts("HDMI audio tests passed");
    return 0;
}
