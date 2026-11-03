/* Tests of the H264 header parser against broken input. Meant to run under ASan and UBSan
 * (see .github/workflows/ci.yml): the checks below only assert return codes, a crash or an
 * undefined operation inside the parser is what the sanitizers report.
 *
 *   parse-test [stream.h264 [iterations [seed]]]
 *       with a stream, each of its NAL units is also damaged at random `iterations` times (default 100)
 *       and parsed again; a different seed damages it differently */
#include "../src/h264_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* a bit writer, to build parameter sets with chosen values */
typedef struct { uint8_t b[256]; int n; } bw;
static void put(bw *w, int bit) { if (!(w->n & 7)) w->b[w->n >> 3] = 0; w->b[w->n >> 3] |= (uint8_t)(bit << (7 - (w->n & 7))); w->n++; }
static void putn(bw *w, uint32_t v, int n) { while (n--) put(w, (v >> n) & 1); }
static void ue(bw *w, uint32_t v)
{
    uint64_t x = (uint64_t)v + 1; int len = 0;
    while ((x >> len) > 1) len++;
    putn(w, 0, len);
    for (int i = len; i >= 0; i--) put(w, (int)((x >> i) & 1));
}

/* a baseline SPS: 1280x720, POC type 2, `refs` reference frames */
static size_t make_sps(uint8_t *out, uint32_t refs, uint32_t sps_id)
{
    bw w = { { 0 }, 0 };
    putn(&w, 66, 8); putn(&w, 0, 8); putn(&w, 30, 8);
    ue(&w, sps_id); ue(&w, 0); ue(&w, 2); ue(&w, refs); put(&w, 0);
    ue(&w, 79); ue(&w, 44); put(&w, 1); put(&w, 1); put(&w, 0); put(&w, 0);
    put(&w, 1); while (w.n & 7) put(&w, 0);
    out[0] = 0x67; memcpy(out + 1, w.b, (size_t)w.n / 8);
    return 1 + (size_t)w.n / 8;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static h264_sps g_sps; static h264_pps g_pps[256]; static int have_sps, have_pps;

static void on_nal(void *ctx, const uint8_t *nal, size_t len)
{
    int *count = ctx; (*count)++;
    int type = nal[0] & 0x1F;
    if (type == 7 && !have_sps && h264_parse_sps(nal, len, &g_sps) == 0) have_sps = 1;
    if (type == 8 && !have_pps) { h264_pps p; if (h264_parse_pps(nal, len, &p) == 0 && p.pps_id < 256) { g_pps[p.pps_id] = p; have_pps = 1; } }
}

static void parse_any(const uint8_t *nal, size_t len)
{
    int type = len ? nal[0] & 0x1F : 0;
    if (type == 7) { h264_sps s; h264_parse_sps(nal, len, &s); }
    else if (type == 8) { h264_pps p; h264_parse_pps(nal, len, &p); }
    else if ((type == 1 || type == 5) && have_sps && have_pps) {
        h264_slice sl;
        int pid = h264_peek_pps_id(nal, len);
        if (pid >= 0 && pid < 256) h264_parse_slice(nal, len, &g_sps, g_pps, &sl);
    }
}

static uint8_t *g_nals[4096]; static size_t g_len[4096]; static int g_n;
static void keep_nal(void *ctx, const uint8_t *nal, size_t len)
{
    (void)ctx;
    if (g_n < 4096) { g_nals[g_n] = malloc(len); memcpy(g_nals[g_n], nal, len); g_len[g_n++] = len; }
}

int main(int argc, char **argv)
{
    uint8_t buf[256];
    int iters = argc > 2 ? atoi(argv[2]) : 100;
    if (argc > 3) rng ^= (uint64_t)strtoull(argv[3], NULL, 10) * 0x2545F4914F6CDD1Dull;

    /* known good parameter sets: the ones the console's encoder sends */
    static const uint8_t sps[] = { 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
    static const uint8_t pps[] = { 0x68, 0xEE, 0x3C, 0xB0 };
    h264_sps s; h264_pps p;
    CHECK(h264_parse_sps(sps, sizeof sps, &s) == 0);
    CHECK(s.width == 1280 && s.height == 720);
    CHECK(h264_parse_pps(pps, sizeof pps, &p) == 0);

    /* every truncation of them */
    for (size_t n = 0; n < sizeof sps; n++) h264_parse_sps(sps, n, &s);
    for (size_t n = 0; n < sizeof pps; n++) h264_parse_pps(pps, n, &p);

    /* the number of reference frames is limited to 16 */
    size_t n;
    n = make_sps(buf, 1, 0);   CHECK(h264_parse_sps(buf, n, &s) == 0 && s.width == 1280 && s.height == 720);
    n = make_sps(buf, 16, 0);  CHECK(h264_parse_sps(buf, n, &s) == 0);
    n = make_sps(buf, 17, 0);  CHECK(h264_parse_sps(buf, n, &s) != 0);
    n = make_sps(buf, 0xFFFFFFFEu, 0); CHECK(h264_parse_sps(buf, n, &s) != 0);

    /* an Exp-Golomb code of 32 zero bits is invalid (and 1u << 32 is undefined) */
    n = make_sps(buf, 1, 0xFFFFFFFFu); CHECK(h264_parse_sps(buf, n, &s) != 0);
    static const uint8_t zeros[] = { 0x67, 0x42, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00 };
    CHECK(h264_parse_sps(zeros, sizeof zeros, &s) != 0);

    /* an empty or tiny NAL */
    CHECK(h264_parse_sps(sps, 0, &s) != 0);
    CHECK(h264_parse_pps(pps, 0, &p) != 0);
    CHECK(h264_peek_pps_id(pps, 0) < 0);

    /* random damage to the NAL units of a real stream */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
        uint8_t *d = malloc((size_t)sz);
        if (fread(d, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);
        int count = 0;
        h264_split_annexb(d, (size_t)sz, on_nal, &count);
        CHECK(count > 10 && have_sps && have_pps);
        h264_split_annexb(d, (size_t)sz, keep_nal, NULL);
        long runs = 0;
        for (int i = 0; i < g_n; i++)
            for (int k = 0; k < iters; k++, runs++) {
                /* the headers are in the first bytes; the rest of a slice is data the parser never reads */
                size_t len = g_len[i] < 2048 ? g_len[i] : 2048; uint8_t *c = malloc(len);
                memcpy(c, g_nals[i], len);
                for (int b = 1 + (int)(rnd() % 3); b; b--) c[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
                if (rnd() % 4 == 0) len = rnd() % len;
                parse_any(c, len);
                free(c);
            }
        fprintf(stderr, "damaged %d NAL units, %ld parses\n", g_n, runs);
        for (int i = 0; i < g_n; i++) free(g_nals[i]);
        free(d);
    }

    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("parse-test: ok");
    return 0;
}
