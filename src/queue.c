#include "queue.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pqueue {
    qpkt *items; int cap, head, count, closed;
    unsigned long dropped;
    pthread_mutex_t m; pthread_cond_t cv;
};

pqueue *pqueue_new(int cap)
{
    pqueue *q = calloc(1, sizeof *q);
    q->items = calloc(cap, sizeof *q->items);
    q->cap = cap;
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->cv, NULL);
    return q;
}

void pqueue_push(pqueue *q, const pkt_header *h, const uint8_t *data)
{
    uint8_t *copy = malloc(h->size ? h->size : 1);
    memcpy(copy, data, h->size);
    pthread_mutex_lock(&q->m);
    if (q->count == q->cap) {
        free(q->items[q->head].data);
        q->head = (q->head + 1) % q->cap;
        q->count--; q->dropped++;
    }
    qpkt *s = &q->items[(q->head + q->count++) % q->cap];
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    s->h = *h; s->data = copy; s->recv_ns = ts.tv_sec * 1000000000ull + ts.tv_nsec;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->m);
}

int pqueue_pop(pqueue *q, qpkt *out)
{
    pthread_mutex_lock(&q->m);
    while (!q->count && !q->closed) pthread_cond_wait(&q->cv, &q->m);
    if (!q->count) { pthread_mutex_unlock(&q->m); return -1; }
    *out = q->items[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_mutex_unlock(&q->m);
    return 0;
}

void pqueue_close(pqueue *q)
{
    pthread_mutex_lock(&q->m);
    q->closed = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->m);
}

unsigned long pqueue_dropped(pqueue *q) { return q->dropped; }

void pqueue_free(pqueue *q)
{
    for (int i = 0; i < q->count; i++) free(q->items[(q->head + i) % q->cap].data);
    free(q->items); free(q);
}
