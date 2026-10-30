/* Offline reference decode of an Annex-B file to raw I420, used to verify the hardware path. */
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *x = avcodec_alloc_context3(c);
    avcodec_open2(x, c, NULL);
    AVCodecParserContext *p = av_parser_init(AV_CODEC_ID_H264);
    AVPacket *pkt = av_packet_alloc(); AVFrame *f = av_frame_alloc();
    uint8_t buf[65536 + AV_INPUT_BUFFER_PADDING_SIZE]; size_t n;
    while ((n = fread(buf, 1, 65536, in)) > 0) {
        uint8_t *d = buf;
        while (n) {
            int used = av_parser_parse2(p, x, &pkt->data, &pkt->size, d, (int)n, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            d += used; n -= used;
            if (pkt->size && avcodec_send_packet(x, pkt) == 0)
                while (avcodec_receive_frame(x, f) == 0) {
                    for (int y = 0; y < f->height; y++) fwrite(f->data[0] + y * f->linesize[0], 1, f->width, out);
                    for (int pl = 1; pl < 3; pl++) for (int y = 0; y < f->height / 2; y++) fwrite(f->data[pl] + y * f->linesize[pl], 1, f->width / 2, out);
                }
        }
    }
    avcodec_send_packet(x, NULL);
    while (avcodec_receive_frame(x, f) == 0) {
        for (int y = 0; y < f->height; y++) fwrite(f->data[0] + y * f->linesize[0], 1, f->width, out);
        for (int pl = 1; pl < 3; pl++) for (int y = 0; y < f->height / 2; y++) fwrite(f->data[pl] + y * f->linesize[pl], 1, f->width / 2, out);
    }
    fclose(out);
    return 0;
}
