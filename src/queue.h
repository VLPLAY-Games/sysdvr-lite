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
/* Like pqueue_pop, but gives up after `ms` milliseconds: returns 1 on a timeout (0 = got a packet, -1 = closed and empty) */
int     pqueue_pop_timed(pqueue *q, qpkt *out, int ms);
void    pqueue_close(pqueue *q);
void    pqueue_free(pqueue *q);
unsigned long pqueue_dropped(pqueue *q);
#endif
