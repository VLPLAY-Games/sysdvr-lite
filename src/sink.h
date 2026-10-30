#ifndef SINK_H
#define SINK_H
#include "decoder.h"

typedef struct sink sink;
struct sink {
    void (*show)(sink *s, const frame *f);
    void (*close)(sink *s);
    void *priv;
};
/* null: only prints FPS stats; file: writes raw planes to path */
sink *sink_null_new(void);
sink *sink_file_new(const char *path);
/* drm: KMS output without X, CPU NV12->XRGB. TODO: GPU path, sink_sdl for x64 debugging */
sink *sink_drm_new(const char *dev, int video_w, int video_h);
#endif
