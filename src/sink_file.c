#include "sink.h"
#include <stdio.h>
#include <stdlib.h>

static void show(sink *s, const frame *f)
{
    FILE *fp = s->priv;
    int cw = f->width / 2, ch = f->height / 2;
    for (int y = 0; y < f->height; y++) fwrite(f->data[0] + y * f->stride[0], 1, f->width, fp);
    if (f->fmt == PIX_NV12) {
        for (int y = 0; y < ch; y++) fwrite(f->data[1] + y * f->stride[1], 1, f->width, fp);
    } else {
        for (int p = 1; p <= 2; p++)
            for (int y = 0; y < ch; y++) fwrite(f->data[p] + y * f->stride[p], 1, cw, fp);
    }
}
static void cls(sink *s) { fclose(s->priv); }

sink *sink_file_new(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return NULL;
    sink *s = calloc(1, sizeof *s);
    s->priv = fp; s->show = show; s->close = cls;
    return s;
}
