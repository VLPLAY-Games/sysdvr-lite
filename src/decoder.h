#ifndef DECODER_H
#define DECODER_H
#include <stddef.h>
#include <stdint.h>

typedef enum { PIX_I420 = 0, PIX_NV12 = 1 } pix_format;

/* Decoded picture. Pointers are valid only during the callback. A hardware
 * backend that can export dmabuf should fill dmabuf_fd (>=0) so the sink can
 * scan it out without a CPU copy. */
typedef struct {
    int width, height;
    pix_format fmt;
    const uint8_t *data[3];
    int stride[3];
    int dmabuf_fd;           /* NV12 dmabuf (plane1 at stride*height) or -1 */
    int buf_id;              /* stable id of the underlying buffer, key for caching imports */
    int uncached;            /* data lives in non-cacheable (dma) memory: copy before heavy CPU reads */
    uint64_t ts;
} frame;

typedef void (*frame_cb)(void *ctx, const frame *f);

typedef struct decoder decoder;
struct decoder {
    /* Feed one chunk of Annex-B H264 (SPS/PPS may be included). 0 ok, <0 error */
    int  (*decode)(decoder *d, const uint8_t *data, size_t len, uint64_t ts);
    void (*close)(decoder *d);
    frame_cb cb; void *cb_ctx;
    void *priv;
};

typedef struct { const char *name; int threads; const char *device; } decoder_opts;

/* Backends, selected by name with --decoder:
 *   null    - discards data (protocol/network benchmarking)
 *   ffmpeg  - libavcodec software decoder (or any ffmpeg decoder via --ffmpeg-name)
 *   v4l2    - native V4L2 stateless (Cedrus on H616/H618), no ffmpeg
 */
decoder *decoder_open(const decoder_opts *o, frame_cb cb, void *ctx);
decoder *decoder_null_new(void);
decoder *decoder_ffmpeg_new(const decoder_opts *o);
decoder *decoder_v4l2_new(const decoder_opts *o);
void     decoder_free(decoder *d);

/* Hardcoded by the console encoder, same as client/Core/StreamInfo.cs */
extern const uint8_t SYSDVR_SPS[20], SYSDVR_PPS[8];
#endif
