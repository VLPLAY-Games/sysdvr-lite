#define _GNU_SOURCE
#include "audio.h"
#include "decoder.h"
#include "queue.h"
#include "sink.h"
#include "transport.h"
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile int g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

typedef struct {
    transport *tr;
    pqueue *vq, *aq;
} reader_ctx;

/* Reads packets from a transport and fans them out. One thread per transport:
 * TCP gives two (video, audio), USB gives one multiplexed. */
static void *reader_thread(void *arg)
{
    reader_ctx *r = arg;
    uint8_t *payload = malloc(PKT_MAX_PAYLOAD);
    while (!g_stop) {
        if (r->tr->open(r->tr)) {          /* console not there yet: keep waiting, it may be plugged in later */
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
            continue;
        }
        for (;;) {
            pkt_header h;
            int rc = r->tr->next_packet(r->tr, &h, payload);
            if (rc) { r->tr->close(r->tr); if (rc < 0) goto out; break; }
            if (h.flags & META_ERROR) { fprintf(stderr, "console reported a capture error\n"); continue; }
            if (h.flags & META_REPLAY) continue;           /* we never enable NAL replay */
            if (!h.size) continue;
            if ((h.flags & META_VIDEO) && r->vq) pqueue_push(r->vq, &h, payload);
            else if ((h.flags & META_AUDIO) && r->aq) pqueue_push(r->aq, &h, payload);
        }
        fprintf(stderr, "stream lost, reconnecting\n");
    }
out:
    free(payload);
    return NULL;
}

typedef struct { pqueue *q; decoder *dec; } dec_ctx;

static int has_idr(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1F) == 5) return 1;
    return 0;
}

static void *decode_thread(void *arg)
{
    dec_ctx *c = arg;
    qpkt p;
    c->dec->decode(c->dec, SYSDVR_SPS, sizeof SYSDVR_SPS, 0);
    c->dec->decode(c->dec, SYSDVR_PPS, sizeof SYSDVR_PPS, 0);
    unsigned long seen_drops = 0;
    int need_idr = 0;
    while (pqueue_pop(c->q, &p) == 0) {
        /* A dropped P-frame corrupts everything up to the next keyframe: skip until one arrives */
        unsigned long dr = pqueue_dropped(c->q);
        if (dr != seen_drops) { seen_drops = dr; need_idr = 1; }
        if (need_idr && has_idr(p.data, p.h.size)) need_idr = 0;
        if (!need_idr) c->dec->decode(c->dec, p.data, p.h.size, p.h.timestamp);
        free(p.data);
    }
    return NULL;
}

typedef struct { pqueue *q; void *audio; } aud_ctx;

static void *audio_thread(void *arg)
{
    aud_ctx *c = arg;
    qpkt p;
    while (pqueue_pop(c->q, &p) == 0) {
        if (c->audio) audio_write(c->audio, p.data, p.h.size);
        free(p.data);
    }
    return NULL;
}

static sink *g_sink;
static void on_frame(void *ctx, const frame *f) { g_sink->show(g_sink, f); }

static void usage(const char *a)
{
    fprintf(stderr,
        "usage: %s (--tcp HOST | --usb) [options]\n"
        "  --decoder null|ffmpeg|v4l2   video backend (default ffmpeg)\n"
        "  --ffmpeg-name NAME           specific libavcodec decoder (e.g. h264_v4l2m2m)\n"
        "  --threads N                  decoder threads (soft decode)\n"
        "  --video-device /dev/videoN   for the v4l2 backend\n"
        "  --drm[=/dev/dri/cardN]       show on the HDMI display via KMS (no X needed)\n"
        "  --out FILE                   write raw decoded frames instead of displaying\n"
        "  --no-audio | --no-video\n"
        "  --audio-device NAME          ALSA device\n"
        "  --screen-off                 turn the console screen off while streaming (proto v3)\n", a);
}

int main(int argc, char **argv)
{
    const char *host = NULL, *outfile = NULL, *adev = NULL, *drmdev = NULL;
    int use_drm = 0;
    int usb = 0, no_audio = 0, no_video = 0, screen_off = 0;
    decoder_opts dopt = { .name = "ffmpeg", .threads = 2 };

    static const struct option opts[] = {
        {"tcp", 1, 0, 't'}, {"usb", 0, 0, 'u'}, {"decoder", 1, 0, 'd'}, {"ffmpeg-name", 1, 0, 'f'},
        {"threads", 1, 0, 'T'}, {"video-device", 1, 0, 'V'}, {"out", 1, 0, 'o'}, {"no-audio", 0, 0, 'A'},
        {"no-video", 0, 0, 'N'}, {"drm", 2, 0, 'D'}, {"audio-device", 1, 0, 'a'}, {"screen-off", 0, 0, 's'}, {0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 't': host = optarg; break;
        case 'u': usb = 1; break;
        case 'd': dopt.name = optarg; break;
        case 'f': dopt.device = optarg; break;   /* ffmpeg backend: decoder name */
        case 'T': dopt.threads = atoi(optarg); break;
        case 'V': dopt.device = optarg; break;   /* v4l2 backend: device node */
        case 'o': outfile = optarg; break;
        case 'D': use_drm = 1; drmdev = optarg; break;
        case 'A': no_audio = 1; break;
        case 'N': no_video = 1; break;
        case 'a': adev = optarg; break;
        case 's': screen_off = 1; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!host == !usb) { usage(argv[0]); return 2; }
#ifndef HAVE_LIBUSB
    if (usb) { fprintf(stderr, "built without libusb\n"); return 1; }
#endif
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    stream_opts so = { !no_video, !no_audio, 3, screen_off, &g_stop };
    pqueue *vq = no_video ? NULL : pqueue_new(8);    /* ~270 ms at 30 fps, then oldest is dropped */
    pqueue *aq = no_audio ? NULL : pqueue_new(32);

    decoder *dec = NULL;
    if (vq) {
        g_sink = outfile ? sink_file_new(outfile) : NULL;
#ifdef HAVE_DRM
        if (!g_sink && use_drm) g_sink = sink_drm_new(drmdev, VIDEO_W, VIDEO_H);
#endif
        if (!g_sink && !use_drm && !outfile) g_sink = sink_null_new();
        if (!g_sink) { perror("sink"); return 1; }
        dec = decoder_open(&dopt, on_frame, NULL);
        if (!dec) return 1;
    }
    void *audio = NULL;
    if (aq) {
        audio = audio_open(adev);
        if (!audio) fprintf(stderr, "audio output unavailable, audio is discarded\n");
    }

    pthread_t th[5]; int n = 0;
    dec_ctx dc = { vq, dec };
    aud_ctx ac = { aq, audio };
    if (vq) pthread_create(&th[n++], NULL, decode_thread, &dc);
    if (aq) pthread_create(&th[n++], NULL, audio_thread, &ac);

    transport *trs[2] = {0}; reader_ctx rcs[2]; int nt = 0;
    pthread_t rth[2];
    if (usb) {
#ifdef HAVE_LIBUSB
        trs[nt] = transport_usb_new(&so);
        rcs[nt] = (reader_ctx){ trs[nt], vq, aq }; nt++;
#endif
    } else {
        if (vq) { trs[nt] = transport_tcp_new(host, STREAM_VIDEO, &so); rcs[nt] = (reader_ctx){ trs[nt], vq, NULL }; nt++; }
        if (aq) { trs[nt] = transport_tcp_new(host, STREAM_AUDIO, &so); rcs[nt] = (reader_ctx){ trs[nt], NULL, aq }; nt++; }
    }
    for (int i = 0; i < nt; i++) pthread_create(&rth[i], NULL, reader_thread, &rcs[i]);
    for (int i = 0; i < nt; i++) pthread_join(rth[i], NULL);

    g_stop = 1;
    if (vq) pqueue_close(vq);
    if (aq) pqueue_close(aq);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    if (vq) fprintf(stderr, "video packets dropped by the queue: %lu\n", pqueue_dropped(vq));
    for (int i = 0; i < nt; i++) transport_free(trs[i]);
    if (dec) decoder_free(dec);
    if (g_sink) g_sink->close(g_sink);
    if (audio) audio_close(audio);
    return 0;
}
