#ifndef SINK_H
#define SINK_H
#include "decoder.h"

/* A message for the display when there is no picture (UTF-8, the font has ASCII and Cyrillic). The sink draws
 * several of them one under another: the first is the main one, the others are smaller and dimmer. */
typedef struct {
    const char *title;
    const char *lines[6];          /* body, NULL ends the list */
    const char *footer;
} notice_text;

typedef struct sink sink;
struct sink {
    void (*show)(sink *s, const frame *f);
    /* Optional: the stream stopped / came back. idle gives the display back to the console, so the
     * last frame does not stay frozen on screen; resume takes it again. Both may be NULL. */
    void (*idle)(sink *s);
    void (*resume)(sink *s);
    /* Optional: 1 while there is a display to show the picture on. When it is gone the client ends the stream, so
     * the console wakes up instead of streaming to nobody with its screen off. */
    int  (*present)(sink *s);
    /* Optional: draw the messages instead of the picture. The next frame replaces them. */
    void (*notice)(sink *s, const notice_text *blocks, int count);
    /* Optional: the slow one-time setup (the GPU: loading the driver takes seconds on the first run after a boot),
     * to be done in the thread that will draw, before the console is read. Done on the first frame otherwise,
     * which stalled the USB reading for that long and made the console drop the first session after every boot. */
    void (*warmup)(sink *s);
    void (*close)(sink *s);
    void *priv;
};
/* null: only prints FPS stats; file: writes raw planes to path */
sink *sink_null_new(void);
sink *sink_file_new(const char *path);
/* drm: KMS output without X, GPU (or CPU fallback) NV12->XRGB conversion */
/* scale: fit the picture to the display when no mode has the video's size (otherwise it is centered 1:1) */
sink *sink_drm_new(const char *dev, int video_w, int video_h, int scale);
#endif
