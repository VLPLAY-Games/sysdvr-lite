#include "decoder.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { AVCodecContext *ctx; AVPacket *pkt; AVFrame *fr; } ff;

static int ff_decode(decoder *d, const uint8_t *data, size_t len, uint64_t ts)
{
    ff *f = d->priv;
    /* AV_INPUT_BUFFER_PADDING_SIZE: libavcodec may over-read */
    if (av_new_packet(f->pkt, (int)len) < 0) return -1;
    memcpy(f->pkt->data, data, len);
    f->pkt->pts = (int64_t)ts;
    int r = avcodec_send_packet(f->ctx, f->pkt);
    av_packet_unref(f->pkt);
    if (r < 0) return r;
    while (avcodec_receive_frame(f->ctx, f->fr) == 0) {
        frame out = { .width = f->fr->width, .height = f->fr->height, .dmabuf_fd = -1,
                      .ts = (uint64_t)f->fr->pts };
        out.fmt = f->fr->format == AV_PIX_FMT_NV12 ? PIX_NV12 : PIX_I420;
        for (int i = 0; i < 3; i++) { out.data[i] = f->fr->data[i]; out.stride[i] = f->fr->linesize[i]; }
        if (f->fr->format == AV_PIX_FMT_YUV420P || f->fr->format == AV_PIX_FMT_YUVJ420P ||
            f->fr->format == AV_PIX_FMT_NV12)
            d->cb(d->cb_ctx, &out);
    }
    return 0;
}

static void ff_close(decoder *d)
{
    ff *f = d->priv;
    av_frame_free(&f->fr); av_packet_free(&f->pkt);
    avcodec_free_context(&f->ctx);
    free(f);
}

decoder *decoder_ffmpeg_new(const decoder_opts *o)
{
    const AVCodec *c = o->device ? avcodec_find_decoder_by_name(o->device)
                                 : avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!c) { fprintf(stderr, "ffmpeg decoder not found\n"); return NULL; }
    ff *f = calloc(1, sizeof *f);
    f->ctx = avcodec_alloc_context3(c);
    f->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    f->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    f->ctx->thread_count = o->threads > 0 ? o->threads : 2;
    f->ctx->thread_type = FF_THREAD_SLICE;       /* frame threads add latency */
    f->ctx->width = 1280; f->ctx->height = 720;
    if (avcodec_open2(f->ctx, c, NULL) < 0) { fprintf(stderr, "avcodec_open2 failed\n"); free(f); return NULL; }
    f->pkt = av_packet_alloc(); f->fr = av_frame_alloc();
    fprintf(stderr, "ffmpeg decoder: %s, threads=%d\n", c->name, f->ctx->thread_count);
    decoder *d = calloc(1, sizeof *d);
    d->decode = ff_decode; d->close = ff_close; d->priv = f;
    return d;
}
