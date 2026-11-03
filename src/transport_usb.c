#include "transport.h"
#include "watchdog.h"
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VID 0x18D1
#define PID 0x4EE0

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *h;
    uint8_t ep_in, ep_out;
    stream_opts opts;
    uint8_t *buf;            /* one whole transfer: libusb can't read a packet in pieces */
    int hotplug;             /* the plug-in event is registered: waiting for the console wakes up as soon as it arrives */
} usb_ctx;

/* The callback only has to exist: libusb runs it from libusb_handle_events*(), which ends the wait in usb_open() */
static int LIBUSB_CALL on_arrived(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event ev, void *user)
{
    return 0;
}

static int usb_rd(void *c, uint8_t *dst, size_t n)
{
    usb_ctx *u = c; int got = 0;
    /* buffer must be a multiple of max packet size to avoid overflow errors */
    int r = libusb_bulk_transfer(u->h, u->ep_in, u->buf, 512, &got, 1500);
    if (r || (size_t)got != n) return -1;
    memcpy(dst, u->buf, n);
    return 0;
}

static int usb_wr(void *c, const uint8_t *src, size_t n)
{
    usb_ctx *u = c; int sent = 0;
    int r = libusb_bulk_transfer(u->h, u->ep_out, (uint8_t *)src, (int)n, &sent, 1500);
    return (r || (size_t)sent != n) ? -1 : 0;
}

static int find_endpoints(usb_ctx *u)
{
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(libusb_get_device(u->h), &cfg)) return -1;
    const struct libusb_interface_descriptor *i = &cfg->interface[0].altsetting[0];
    u->ep_in = u->ep_out = 0;
    for (int e = 0; e < i->bNumEndpoints; e++) {
        uint8_t a = i->endpoint[e].bEndpointAddress;
        if ((i->endpoint[e].bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
        if (a & 0x80) u->ep_in = a; else u->ep_out = a;
    }
    libusb_free_config_descriptor(cfg);
    return (u->ep_in && u->ep_out) ? 0 : -1;
}

static int usb_open(transport *t)
{
    usb_ctx *u = t->priv;
    if (u->h) { libusb_close(u->h); u->h = NULL; }
    for (int tries = 0; tries < 10 && !*u->opts.stop; tries++) {
        watchdog_beat();                                 /* waiting for the console is not a hang */
        u->h = libusb_open_device_with_vid_pid(u->ctx, VID, PID);
        if (u->h) {
            libusb_set_auto_detach_kernel_driver(u->h, 1);
            if (libusb_claim_interface(u->h, 0) == 0 && find_endpoints(u) == 0) {
                hs_io io = { usb_rd, usb_wr, u };
                if (proto_handshake(&io, STREAM_VIDEO | STREAM_AUDIO, &u->opts) == 0) { fprintf(stderr, "SysDVR console connected\n"); return 0; }
            }
            libusb_close(u->h); u->h = NULL;
        }
        if (tries == 0) fprintf(stderr, "waiting for SysDVR usb device...\n");
        if (u->hotplug) { struct timeval tv = { 1, 0 }; libusb_handle_events_timeout(u->ctx, &tv); }   /* returns at once when the console is plugged in */
        else { struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); }
    }
    return -1;
}

static int usb_next(transport *t, pkt_header *h, uint8_t *payload)
{
    usb_ctx *u = t->priv; int got = 0;
    while (!*u->opts.stop) {
        int r = libusb_bulk_transfer(u->h, u->ep_in, u->buf, PKT_MAX_TRANSFER, &got, 800);
        if (r == LIBUSB_ERROR_TIMEOUT) return got > 0 ? 1 : 2;   /* 2: nothing came, the caller has things to check; a half packet is a broken stream */
        if (r || got < PKT_HDR_SIZE || pkt_parse_header(u->buf, h) || (int)h->size > got - PKT_HDR_SIZE)
            return 1;
        memcpy(payload, u->buf + PKT_HDR_SIZE, h->size);
        return 0;
    }
    return -1;
}

static void usb_close(transport *t)
{
    usb_ctx *u = t->priv;
    if (u->h) libusb_close(u->h);
    u->h = NULL;
}

static void usb_end(transport *t)
{
    usb_ctx *u = t->priv;
    if (u->h) { libusb_reset_device(u->h); libusb_close(u->h); }     /* the console sees the reset and stops streaming */
    u->h = NULL;
}

static void usb_destroy(transport *t);

transport *transport_usb_new(const stream_opts *o)
{
    usb_ctx *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    if (libusb_init(&u->ctx)) { free(u); return NULL; }
    u->opts = *o;
    if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) &&
        libusb_hotplug_register_callback(u->ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, 0, VID, PID,
                                         LIBUSB_HOTPLUG_MATCH_ANY, on_arrived, NULL, NULL) == LIBUSB_SUCCESS)
        u->hotplug = 1;
    u->buf = malloc(PKT_MAX_TRANSFER);
    transport *t = calloc(1, sizeof *t);
    if (!u->buf || !t) { free(u->buf); free(t); libusb_exit(u->ctx); free(u); return NULL; }
    t->priv = u; t->open = usb_open; t->next_packet = usb_next; t->close = usb_close; t->end = usb_end; t->destroy = usb_destroy;
    return t;
}

static void usb_destroy(transport *t)
{
    usb_ctx *u = t->priv;
    if (u->ctx) libusb_exit(u->ctx);
    free(u->buf);
    free(u);
}

void transport_free(transport *t)
{
    if (!t) return;
    t->close(t);
    if (t->destroy) t->destroy(t); else free(t->priv);
    free(t);
}
