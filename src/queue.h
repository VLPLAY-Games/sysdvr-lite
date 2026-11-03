#ifndef QUEUE_H
#define QUEUE_H
#include "proto.h"

typedef struct { pkt_header h; uint8_t *data; uint64_t recv_ns; } qpkt;   /* recv_ns: CLOCK_MONOTONIC when queued */

typedef struct pqueue pqueue;
pqueue *pqueue_new(int cap);
/* Drops the oldest entry when full (low latency beats completeness for live video) */
void    pqueue_push(pqueue *q, const pkt_header *h, const uint8_t *data);
/* Blocks; returns 0 and fills out (caller frees out->data), -1 when closed and empty */
int     pqueue_pop(pqueue *q, qpkt *out);
void    pqueue_close(pqueue *q);
void    pqueue_free(pqueue *q);
unsigned long pqueue_dropped(pqueue *q);
#endif
