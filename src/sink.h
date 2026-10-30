#ifndef SINK_H
#define SINK_H
#include "decoder.h"

typedef struct sink sink;
struct sink {
    void (*show)(sink *s, const frame *f);
    /* Optional: the stream stopped / came back. idle gives the display back to the console, so the
     * last frame does not stay frozen on screen; resume takes it again. Both may be NULL. */
    void (*idle)(sink *s);
    void (*resume)(sink *s);
    void (*close)(sink *s);
    void *priv;
};
/* null: only prints FPS stats; file: writes raw planes to path */
sink *sink_null_new(void);
sink *sink_file_new(const char *path);
/* drm: KMS output without X, GPU (or CPU fallback) NV12->XRGB conversion */
sink *sink_drm_new(const char *dev, int video_w, int video_h);
#endif
