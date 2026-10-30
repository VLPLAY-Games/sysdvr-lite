/* Generates a 720p30 Annex-B H264 test stream (libx264 through libavcodec),
 * similar to what the console emits: High profile, no B-frames, one slice. */
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "test.h264";
    int slices = argc > 2 ? atoi(argv[2]) : 1;
    const AVCodec *c = avcodec_find_encoder_by_name("libx264");
    if (!c) { fprintf(stderr, "no libx264\n"); return 1; }
    AVCodecContext *x = avcodec_alloc_context3(c);
    x->width = 1280; x->height = 720; x->pix_fmt = AV_PIX_FMT_YUV420P;
    x->time_base = (AVRational){1, 30}; x->framerate = (AVRational){30, 1};
    x->bit_rate = 4000000; x->gop_size = 30; x->max_b_frames = 0;
    av_opt_set(x->priv_data, "profile", "high", 0);
    av_opt_set(x->priv_data, "preset", "veryfast", 0);
    av_opt_set(x->priv_data, "tune", "zerolatency", 0);
    char prm[96];
    snprintf(prm, sizeof prm, "slices=%d:threads=%d:sliced-threads=0:repeat-headers=1:aud=0", slices, slices > 1 ? 2 : 1);
    av_opt_set(x->priv_data, "x264-params", prm, 0);
    if (avcodec_open2(x, c, NULL) < 0) return 1;
    AVFrame *f = av_frame_alloc(); f->format = x->pix_fmt; f->width = 1280; f->height = 720;
    av_frame_get_buffer(f, 0);
    AVPacket *p = av_packet_alloc(); FILE *fp = fopen(out, "wb");
    for (int i = 0; i < 300; i++) {
        av_frame_make_writable(f);
        for (int y = 0; y < 720; y++) for (int xx = 0; xx < 1280; xx++)
            f->data[0][y * f->linesize[0] + xx] = ((xx + i * 6) / 4 ^ (y / 4)) & 0xFF;
        for (int y = 0; y < 360; y++) for (int xx = 0; xx < 640; xx++) {
            f->data[1][y * f->linesize[1] + xx] = (128 + (xx + i * 2) / 4) & 0xFF;
            f->data[2][y * f->linesize[2] + xx] = (128 + y / 2 + i) & 0xFF;
        }
        f->pts = i;
        avcodec_send_frame(x, f);
        while (avcodec_receive_packet(x, p) == 0) { fwrite(p->data, 1, p->size, fp); av_packet_unref(p); }
    }
    avcodec_send_frame(x, NULL);
    while (avcodec_receive_packet(x, p) == 0) { fwrite(p->data, 1, p->size, fp); av_packet_unref(p); }
    fclose(fp);
    return 0;
}
