#ifndef USB_STREAM_H
#define USB_STREAM_H
#include "proto.h"
#include <string.h>

/* Bulk reads end on a short USB packet, not necessarily on a SysDVR packet.
 * Keep incomplete packets and any following packets until the caller consumes them. */
#define USB_READ_SIZE ((PKT_MAX_TRANSFER + 511u) & ~511u)
#define USB_STREAM_CAPACITY (PKT_MAX_TRANSFER + USB_READ_SIZE)
typedef struct {
    size_t begin, end;
    uint8_t data[USB_STREAM_CAPACITY];
} usb_stream;

static inline void usb_stream_compact(usb_stream *s)
{
    size_t n = s->end - s->begin;
    if (n && s->begin) memmove(s->data, s->data + s->begin, n);
    s->begin = 0; s->end = n;
}

/* 1: packet, 0: need more bytes, -1: invalid header, 2: new hello. */
static inline int usb_stream_next(usb_stream *s, pkt_header *h, uint8_t *payload)
{
    size_t n = s->end - s->begin;
    const uint8_t *p = s->data + s->begin;
    if (n >= HELLO_SIZE && !hello_parse(p, (uint8_t[2]){0})) return 2;
    if (n < PKT_HDR_SIZE) return 0;
    if (pkt_parse_header(p, h)) return -1;
    size_t total = PKT_HDR_SIZE + h->size;
    if (n < total) return 0;
    memcpy(payload, p + PKT_HDR_SIZE, h->size);
    s->begin += total;
    if (s->begin == s->end) s->begin = s->end = 0;
    return 1;
}
#endif
