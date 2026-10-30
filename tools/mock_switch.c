/* Fake SysDVR sysmodule (TCP bridge, protocol 02) for testing without a console.
 * Serves video on :9911 and silent audio on :9922.
 *   mock-switch [file.h264]   Annex-B file to loop; without it sends dummy NALs
 *                             (enough to test networking/queues, not the decoder). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

static uint8_t *g_file; static long g_len;

static void put_hdr(uint8_t *h, uint32_t size, uint64_t ts, uint8_t flags)
{
    memset(h, 0xCC, 4);
    memcpy(h + 4, &size, 4); memcpy(h + 8, &ts, 8);
    h[16] = flags; h[17] = 0xFF;
}

static int all(int fd, const void *b, size_t n)
{
    const uint8_t *p = b;
    while (n) { ssize_t r = send(fd, p, n, MSG_NOSIGNAL); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}

/* returns offset of the next start code at or after pos */
static long next_sc(long pos)
{
    for (long i = pos; i + 3 < g_len; i++)
        if (!g_file[i] && !g_file[i + 1] && g_file[i + 2] == 1) return i;
    return g_len;
}

static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static void serve_video(int fd)
{
    uint8_t pkt[18 + 0x54000]; uint64_t ts = 0;
    long pos = g_file ? next_sc(0) : 0;
    for (;;) {
        uint32_t size;
        if (g_file) {
            /* one frame per packet: collect NALs until the next slice that starts a new picture
             * (first_mb_in_slice == 0 => first byte of the slice payload has its top bit set) */
            long start = pos, end = pos; int seen_vcl = 0;
            for (;;) {
                if (end >= g_len) { pos = next_sc(0); start = pos; end = pos; seen_vcl = 0; }
                int type = g_file[end + 3] & 0x1F;
                if ((type == 1 || type == 5) && seen_vcl && (g_file[end + 4] & 0x80)) break;
                if ((type == 7 || type == 8 || type == 6) && seen_vcl) break;
                if (type == 1 || type == 5) seen_vcl = 1;
                long nxt = next_sc(end + 3);
                end = nxt;
                if (end >= g_len) { if (seen_vcl) break; }
            }
            size = (uint32_t)(end - start);
            if (size > 0x54000) return;
            memcpy(pkt + 18, g_file + start, size);
            pos = end;
        } else {
            size = 2000;
            memset(pkt + 18, 0x55, size);
            memcpy(pkt + 18, "\0\0\1\x41", 4);
        }
        put_hdr(pkt, size, ts, 1 | 4);
        if (all(fd, pkt, 18 + size)) return;
        ts += 33333; sleep_ms(33);
    }
}

static void serve_audio(int fd)
{
    static uint8_t pkt[18 + 0x4000]; uint64_t ts = 0;
    memset(pkt, 0, sizeof pkt);
    for (;;) {
        put_hdr(pkt, 0x1000 * 3, ts, 2 | 4);
        if (all(fd, pkt, 18 + 0x3000)) return;
        ts += 64000; sleep_ms(64);
    }
}

static void *client(void *a)
{
    long v = (long)a >> 32; int fd = (int)((long)a & 0xFFFFFFFF);
    if (all(fd, "SysDVR|02", 10)) goto end;
    uint8_t req[16]; size_t got = 0;
    while (got < 16) { ssize_t r = recv(fd, req + got, 16 - got, 0); if (r <= 0) goto end; got += r; }
    uint32_t ok = (req[0] == 0xAA && req[4] == '0' && req[5] == '2') ? 6 : 1;
    if (all(fd, &ok, 4) || ok != 6) goto end;
    fprintf(stderr, "client handshake ok (%s)\n", v ? "video" : "audio");
    if (v) serve_video(fd); else serve_audio(fd);
end:
    close(fd);
    return NULL;
}

static void *listener(void *a)
{
    int video = (int)(long)a;
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(video ? 9911 : 9922), .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (void *)&sa, sizeof sa) || listen(s, 4)) { perror("bind"); exit(1); }
    for (;;) {
        int fd = accept(s, NULL, NULL); pthread_t t;
        pthread_create(&t, NULL, client, (void *)(((long)video << 32) | fd));
        pthread_detach(t);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror("open"); return 1; }
        fseek(f, 0, SEEK_END); g_len = ftell(f); rewind(f);
        g_file = malloc(g_len);
        if (fread(g_file, 1, g_len, f) != (size_t)g_len) return 1;
        fclose(f);
    }
    pthread_t a, b;
    pthread_create(&a, NULL, listener, (void *)1L);
    pthread_create(&b, NULL, listener, (void *)0L);
    fprintf(stderr, "mock switch listening on 9911/9922\n");
    pthread_join(a, NULL);
    return 0;
}
