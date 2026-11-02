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
static int g_exit_code;
static sink *g_sink;
static uint64_t g_recv_ns;                      /* arrival time of the packet being decoded */
static int g_lat;                               /* SYSDVR_LATENCY=1: print latency statistics */

static uint64_t now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}
typedef struct { double sum, max; unsigned n; } lstat;
static void ladd(lstat *s, double v) { s->sum += v; if (v > s->max) s->max = v; s->n++; }
static lstat st_wait, st_dec, st_show;

/* A/V sync: video is never delayed (latency matters most), the audio follows it. Both streams carry
 * timestamps from the same console clock, so the position being heard can be compared with the
 * picture being shown. */
static volatile uint64_t g_video_ts;            /* console timestamp (us) of the last frame handed to the display */
static volatile uint64_t g_video_host_ns;       /* host time when that happened */
static int g_av_offset_us;
static int g_audio_batching = 1;                /* 0..3, packets of audio the console collects before sending */                      /* --av-offset: positive = audio later than video */
/* Audio is inherently a bit behind the picture (the console sends it in batches, ALSA queues it), that
 * is fine. Only runaway drift is corrected: audio too far behind is skipped, audio ahead is delayed. */
#define SYNC_MAX_LAG_US 120000
#define SYNC_MAX_LEAD_US 40000
#define SYNC_MAX_SILENCE_US 300000
static void on_sig(int s) { (void)s; g_stop = 1; }

typedef struct {
    transport *tr;
    pqueue *vq, *aq;
} reader_ctx;

/* Reads packets from the transport and fans them out to the video and audio queues */
static void *reader_thread(void *arg)
{
    reader_ctx *r = arg;
    uint8_t *payload = malloc(PKT_MAX_PAYLOAD);
    int need_resume;
    while (!g_stop) {
        if (r->tr->open(r->tr)) {          /* console not there yet: keep waiting, it may be plugged in later */
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
            continue;
        }
        need_resume = 1;
        for (;;) {
            pkt_header h;
            int rc = r->tr->next_packet(r->tr, &h, payload);
            if (rc) { r->tr->close(r->tr); if (rc < 0) goto out; break; }
            if (h.flags & META_ERROR) { fprintf(stderr, "console reported a capture error\n"); continue; }
            if (h.flags & META_REPLAY) continue;           /* we never enable NAL replay */
            if (!h.size) continue;
            if ((h.flags & META_VIDEO) && r->vq) {
                if (need_resume) {                          /* picture is coming: take the display back */
                    if (g_sink && g_sink->resume) g_sink->resume(g_sink);
                    need_resume = 0;
                }
                pqueue_push(r->vq, &h, payload);
            }
            else if ((h.flags & META_AUDIO) && r->aq) pqueue_push(r->aq, &h, payload);
        }
        fprintf(stderr, "stream lost, reconnecting\n");
        if (g_sink && g_sink->idle) g_sink->idle(g_sink);      /* back to the terminal instead of a frozen frame */
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
    int need_idr = 0, failures = 0;
    while (pqueue_pop(c->q, &p) == 0) {
        /* A dropped P-frame corrupts everything up to the next keyframe: skip until one arrives */
        unsigned long dr = pqueue_dropped(c->q);
        if (dr != seen_drops) { seen_drops = dr; need_idr = 1; }
        if (need_idr && has_idr(p.data, p.h.size)) need_idr = 0;
        if (!need_idr) {
            g_recv_ns = p.recv_ns;
            if (g_lat) ladd(&st_wait, (now_ns() - p.recv_ns) / 1e6);
            if (c->dec->decode(c->dec, p.data, p.h.size, p.h.timestamp) < 0) {
                /* a broken decoder never recovers on its own: exit so that systemd restarts us */
                if (++failures >= 90) { fprintf(stderr, "decoder keeps failing, exiting\n"); g_exit_code = 1; g_stop = 1; }
            } else failures = 0;
        }
        free(p.data);
    }
    return NULL;
}

typedef struct { pqueue *q; void *audio; } aud_ctx;

static void *audio_thread(void *arg)
{
    aud_ctx *c = arg;
    qpkt p;
    int dbg = getenv("SYSDVR_SYNC") != NULL;
    double sum = 0, mn = 1e9, mx = -1e9; unsigned n = 0, dropped = 0, padded = 0;
    while (pqueue_pop(c->q, &p) == 0) {
        int skip = 0;
        uint64_t v = g_video_ts;
        if (c->audio && v && now_ns() - g_video_host_ns < 500000000ull) {       /* ignore stale video clocks */
            /* console timestamp of the sample that is audible right now, if this packet follows contiguously */
            int64_t heard = (int64_t)p.h.timestamp - audio_delay_us(c->audio);
            int64_t skew = heard - (int64_t)v - g_av_offset_us;                 /* >0: audio ahead of the picture */
            if (dbg) {
                double ms = skew / 1000.0; sum += ms; if (ms < mn) mn = ms; if (ms > mx) mx = ms;
                if (++n == 100) {
                    fprintf(stderr, "av skew ms avg %.1f min %.1f max %.1f | corrections: %u silence, %u dropped\n",
                            sum / n, mn, mx, padded, dropped);
                    sum = 0; mn = 1e9; mx = -1e9; n = 0;
                }
            }
            if (skew > SYNC_MAX_LEAD_US) {
                audio_silence(c->audio, skew > SYNC_MAX_SILENCE_US ? SYNC_MAX_SILENCE_US : skew);
                padded++;
            } else if (skew < -SYNC_MAX_LAG_US) {
                skip = 1;                                                      /* audio is late: jump forward */
                dropped++;
            }
        }
        if (c->audio && !skip) audio_write(c->audio, p.data, p.h.size);
        free(p.data);
    }
    return NULL;
}

static void on_frame(void *ctx, const frame *f)
{
    frame g = *f;
    g.recv_ns = g_recv_ns;
    g_video_ts = f->ts;
    g_video_host_ns = now_ns();
    uint64_t t0 = g_lat ? now_ns() : 0;
    if (g_lat) ladd(&st_dec, (t0 - g_recv_ns) / 1e6);        /* arrival -> decoded (includes queue wait) */
    g_sink->show(g_sink, &g);
    if (g_lat) {
        ladd(&st_show, (now_ns() - t0) / 1e6);
        if (st_show.n >= 150) {
            fprintf(stderr, "latency ms (avg/max): queue wait %.2f/%.2f | arrival->decoded %.2f/%.2f | show() %.2f/%.2f\n",
                    st_wait.sum / st_wait.n, st_wait.max, st_dec.sum / st_dec.n, st_dec.max, st_show.sum / st_show.n, st_show.max);
            st_wait = st_dec = st_show = (lstat){0};
        }
    }
}

static void print_version(void)
{
    printf("sysdvr-lite %s%s%s%s\n", SYSDVR_VERSION, SYSDVR_GIT[0] ? " (" : "", SYSDVR_GIT, SYSDVR_GIT[0] ? ")" : "");
    printf("features:"
#ifdef HAVE_FFMPEG
           " ffmpeg"
#endif
#ifdef HAVE_DRM
           " drm"
#endif
#ifdef HAVE_GL
           " gpu"
#endif
#ifdef HAVE_ALSA
           " alsa"
#endif
           " v4l2\n");
}

static void usage(const char *a)
{
    fprintf(stderr,
        "usage: %s [options]       (waits for a Switch running SysDVR in USB mode)\n"
        "  --decoder null|ffmpeg|v4l2   video backend (default ffmpeg; v4l2 = Allwinner Cedrus hardware)\n"
        "  --ffmpeg-name NAME           specific libavcodec decoder\n"
        "  --threads N                  decoder threads (software decode)\n"
        "  --video-device /dev/videoN   for the v4l2 backend\n"
        "  --drm[=/dev/dri/cardN]       show on the HDMI display via KMS (no X needed)\n"
        "  --out FILE                   write raw decoded frames instead of displaying\n"
        "  --no-audio | --no-video\n"
        "  --audio-device NAME          ALSA device\n"
        "  --audio-batching 0..3        audio packets the console sends at once (default 1, measured best A/V skew)\n"
        "  --av-offset MS               shift audio relative to video (positive = audio later)\n"
        "  --keep-screen                do not turn the console screen off while streaming\n"
        "  --test-file FILE.h264        replay an Annex-B file instead of using USB (decoder testing)\n"
        "  --version                    print the version and the features this build has\n", a);
}

int main(int argc, char **argv)
{
    const char *outfile = NULL, *adev = NULL, *drmdev = NULL, *testfile = NULL;
    int use_drm = 0, no_audio = 0, no_video = 0;
    int screen_off = 1;       /* turn the console screen off while streaming, the sysmodule restores it on disconnect */
    decoder_opts dopt = { .name = "ffmpeg", .threads = 2 };

    static const struct option opts[] = {
        {"usb", 0, 0, 'u'}, {"decoder", 1, 0, 'd'}, {"ffmpeg-name", 1, 0, 'f'},
        {"threads", 1, 0, 'T'}, {"video-device", 1, 0, 'V'}, {"out", 1, 0, 'o'}, {"no-audio", 0, 0, 'A'},
        {"no-video", 0, 0, 'N'}, {"drm", 2, 0, 'D'}, {"audio-device", 1, 0, 'a'}, {"screen-off", 0, 0, 's'}, {"keep-screen", 0, 0, 'K'},
        {"av-offset", 1, 0, 'O'}, {"audio-batching", 1, 0, 'B'}, {"test-file", 1, 0, 'F'}, {"version", 0, 0, 'v'}, {0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'u': break;                          /* USB is the only transport, flag kept for old service files */
        case 'd': dopt.name = optarg; break;
        case 'f': dopt.device = optarg; break;    /* ffmpeg backend: decoder name */
        case 'T': dopt.threads = atoi(optarg); break;
        case 'V': dopt.device = optarg; break;    /* v4l2 backend: device node */
        case 'o': outfile = optarg; break;
        case 'D': use_drm = 1; drmdev = optarg; break;
        case 'A': no_audio = 1; break;
        case 'N': no_video = 1; break;
        case 'a': adev = optarg; break;
        case 's': screen_off = 1; break;           /* default since the flag became opt-out */
        case 'K': screen_off = 0; break;
        case 'O': g_av_offset_us = atoi(optarg) * 1000; break;
        case 'B': g_audio_batching = atoi(optarg); if (g_audio_batching < 0 || g_audio_batching > 3) g_audio_batching = 1; break;
        case 'F': testfile = optarg; no_audio = 1; break;
        case 'v': print_version(); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    g_lat = getenv("SYSDVR_LATENCY") != NULL;
    fprintf(stderr, "sysdvr-lite %s%s%s%s\n", SYSDVR_VERSION, SYSDVR_GIT[0] ? " (" : "", SYSDVR_GIT, SYSDVR_GIT[0] ? ")" : "");
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    stream_opts so = { !no_video, !no_audio, g_audio_batching, screen_off, &g_stop };
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

    transport *tr = testfile ? transport_file_new(testfile, &g_stop) : transport_usb_new(&so);
    if (!tr) { fprintf(stderr, "cannot start the transport\n"); return 1; }

    pthread_t th[2]; int n = 0;
    dec_ctx dc = { vq, dec };
    aud_ctx ac = { aq, audio };
    if (vq) pthread_create(&th[n++], NULL, decode_thread, &dc);
    if (aq) pthread_create(&th[n++], NULL, audio_thread, &ac);

    reader_ctx rc = { tr, vq, aq };
    reader_thread(&rc);                              /* runs until SIGINT/SIGTERM */

    g_stop = 1;
    if (vq) pqueue_close(vq);
    if (aq) pqueue_close(aq);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    if (vq) fprintf(stderr, "video packets dropped by the queue: %lu\n", pqueue_dropped(vq));
    transport_free(tr);
    if (dec) decoder_free(dec);
    if (g_sink) g_sink->close(g_sink);
    if (audio) audio_close(audio);
    return g_exit_code;
}
