#include "sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct { unsigned frames; double t0; } st;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void show(sink *s, const frame *f)
{
    st *p = s->priv;
    if (!p->t0) p->t0 = now();
    p->frames++;
    double t = now();
    if (t - p->t0 >= 1.0) {
        fprintf(stderr, "decoded %.1f fps (%dx%d)\n", p->frames / (t - p->t0), f->width, f->height);
        p->frames = 0; p->t0 = t;
    }
}
static void cls(sink *s) { free(s->priv); }

sink *sink_null_new(void)
{
    sink *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->priv = calloc(1, sizeof(st));
    if (!s->priv) { free(s); return NULL; }
    s->show = show; s->close = cls;
    return s;
}
