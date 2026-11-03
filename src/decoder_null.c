#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t SYSDVR_SPS[20] = {0,0,0,1,0x67,0x64,0x0C,0x20,0xAC,0x2B,0x40,0x28,0x02,0xDD,0x35,0x01,0x0D,0x01,0xE0,0x80};
const uint8_t SYSDVR_PPS[8]  = {0,0,0,1,0x68,0xEE,0x3C,0xB0};

static int null_decode(decoder *d, const uint8_t *data, size_t len, uint64_t ts) { return 0; }
static void null_close(decoder *d) {}

decoder *decoder_null_new(void)
{
    decoder *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->decode = null_decode; d->close = null_close;
    return d;
}

decoder *decoder_open(const decoder_opts *o, frame_cb cb, void *ctx)
{
    decoder *d = NULL;
    const char *n = o->name ? o->name : "ffmpeg";
    if (!strcmp(n, "null")) d = decoder_null_new();
#ifdef HAVE_FFMPEG
    else if (!strcmp(n, "ffmpeg")) d = decoder_ffmpeg_new(o);
#endif
    else if (!strcmp(n, "v4l2")) d = decoder_v4l2_new(o);
    else fprintf(stderr, "decoder '%s' is not available in this build\n", n);
    if (d) { d->cb = cb; d->cb_ctx = ctx; }
    return d;
}

void decoder_free(decoder *d)
{
    if (!d) return;
    d->close(d);
    free(d);
}
