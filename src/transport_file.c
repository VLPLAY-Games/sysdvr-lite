#define _GNU_SOURCE
#include "transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Replays an Annex-B file as if a console was sending it: one picture per packet, 30 fps,
 * looping. A slice starts a new picture when its first_mb_in_slice is 0 (top bit set). */
typedef struct { uint8_t *d; long len, pos; uint64_t ts; volatile int *stop; struct timespec next; } fctx;

static long next_sc(const fctx *f, long from)
{
    for (long i = from; i + 3 < f->len; i++)
        if (!f->d[i] && !f->d[i + 1] && f->d[i + 2] == 1) return i;
    return f->len;
}

static int file_open(transport *t)
{
    fctx *f = t->priv;
    f->pos = next_sc(f, 0);
    clock_gettime(CLOCK_MONOTONIC, &f->next);
    return f->pos < f->len ? 0 : -1;
}

static int file_next(transport *t, pkt_header *h, uint8_t *payload)
{
    fctx *f = t->priv;
    if (*f->stop) return -1;
    long start = f->pos, end = f->pos; int seen_vcl = 0;
    for (;;) {
        if (end >= f->len) { if (seen_vcl) break; f->pos = start = end = next_sc(f, 0); continue; }
        int type = f->d[end + 3] & 0x1F;
        if ((type == 1 || type == 5) && seen_vcl && (f->d[end + 4] & 0x80)) break;
        if ((type == 6 || type == 7 || type == 8) && seen_vcl) break;
        if (type == 1 || type == 5) seen_vcl = 1;
        end = next_sc(f, end + 3);
    }
    long size = end - start;
    if (size > PKT_MAX_PAYLOAD) return -1;
    memcpy(payload, f->d + start, size);
    f->pos = end >= f->len ? next_sc(f, 0) : end;
    memset(h, 0, sizeof *h);
    h->magic = PKT_MAGIC; h->size = (uint32_t)size; h->timestamp = f->ts; h->flags = META_VIDEO | META_DATA;
    f->ts += 33333;
    f->next.tv_nsec += 33333333;
    if (f->next.tv_nsec >= 1000000000) { f->next.tv_nsec -= 1000000000; f->next.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &f->next, NULL);
    return 0;
}

static void file_close(transport *t) {}
static void file_destroy(transport *t) { fctx *f = t->priv; free(f->d); free(f); }

transport *transport_file_new(const char *path, volatile int *stop)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }
    fctx *f = calloc(1, sizeof *f);
    fseek(fp, 0, SEEK_END); f->len = ftell(fp); rewind(fp);
    f->d = malloc(f->len + 8);
    if (fread(f->d, 1, f->len, fp) != (size_t)f->len) { fclose(fp); free(f->d); free(f); return NULL; }
    fclose(fp);
    f->stop = stop;
    transport *t = calloc(1, sizeof *t);
    t->priv = f; t->open = file_open; t->next_packet = file_next; t->close = file_close; t->destroy = file_destroy;
    return t;
}
