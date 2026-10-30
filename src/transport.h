#ifndef TRANSPORT_H
#define TRANSPORT_H
#include "proto.h"

/* A transport hides how bytes reach us. TCP has one socket per stream,
 * USB multiplexes video+audio on one bulk pipe. */
typedef struct transport transport;
struct transport {
    /* connect + hello + handshake, 0 on success */
    int  (*open)(transport *t);
    /* Reads next packet into payload (>= PKT_MAX_PAYLOAD bytes). Returns 0 ok,
     * 1 = stream dropped (caller should reopen), -1 fatal/cancelled */
    int  (*next_packet)(transport *t, pkt_header *h, uint8_t *payload);
    void (*close)(transport *t);
    void *priv;
};

typedef struct {
    int want_video, want_audio;
    int audio_batching;
    int turn_off_screen;
    volatile int *stop;
} stream_opts;

transport *transport_tcp_new(const char *host, stream_kind kind, const stream_opts *o);
transport *transport_usb_new(const stream_opts *o);   /* NULL if built without libusb */
void       transport_free(transport *t);

/* Shared handshake logic: io callbacks supplied by the transport */
typedef struct {
    int (*read_exact)(void *ctx, uint8_t *buf, size_t n);
    int (*write_all)(void *ctx, const uint8_t *buf, size_t n);
    void *ctx;
} hs_io;
int proto_handshake(const hs_io *io, int kind, const stream_opts *o);

#endif
