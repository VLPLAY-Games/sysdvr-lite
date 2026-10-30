#define _GNU_SOURCE
#include "transport.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TCP_VIDEO_PORT 9911
#define TCP_AUDIO_PORT 9922

typedef struct {
    char host[128];
    int kind, fd, in_sync;
    stream_opts opts;
} tcp_ctx;

static int rd_exact(void *c, uint8_t *buf, size_t n)
{
    tcp_ctx *t = c;
    while (n) {
        struct pollfd p = { t->fd, POLLIN, 0 };
        int pr = poll(&p, 1, 500);
        if (*t->opts.stop) return -1;
        if (pr < 0 && errno != EINTR) return -1;
        if (pr <= 0) continue;
        ssize_t r = recv(t->fd, buf, n, 0);
        if (r <= 0) return -1;
        buf += r; n -= r;
    }
    return 0;
}

static int wr_all(void *c, const uint8_t *buf, size_t n)
{
    tcp_ctx *t = c;
    while (n) {
        ssize_t r = send(t->fd, buf, n, MSG_NOSIGNAL);
        if (r <= 0) return -1;
        buf += r; n -= r;
    }
    return 0;
}

static int connect_once(tcp_ctx *t)
{
    char port[8];
    snprintf(port, sizeof port, "%d", t->kind == STREAM_VIDEO ? TCP_VIDEO_PORT : TCP_AUDIO_PORT);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(t->host, port, &hints, &res)) return -1;
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) { close(fd); fd = -1; }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    int one = 1, rcv = PKT_MAX_TRANSFER;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    t->fd = fd;
    return 0;
}

static int tcp_open(transport *tr)
{
    tcp_ctx *t = tr->priv;
    if (t->fd >= 0) { close(t->fd); t->fd = -1; }
    for (int i = 0; i < 5 && !*t->opts.stop; i++) {
        fprintf(stderr, "[%s] connecting to %s (%d/5)\n", t->kind == STREAM_VIDEO ? "video" : "audio", t->host, i + 1);
        if (connect_once(t) == 0) {
            hs_io io = { rd_exact, wr_all, t };
            if (proto_handshake(&io, t->kind, &t->opts) == 0) { t->in_sync = 1; return 0; }
            close(t->fd); t->fd = -1;
        }
        sleep(2);
    }
    return -1;
}

/* After a corrupted header skip bytes until 4x 0xCC */
static int resync(tcp_ctx *t)
{
    int cnt = 0; uint8_t b;
    while (cnt < 4) {
        if (rd_exact(t, &b, 1)) return -1;
        cnt = (b == 0xCC) ? cnt + 1 : 0;
    }
    return 0;
}

static int tcp_next(transport *tr, pkt_header *h, uint8_t *payload)
{
    tcp_ctx *t = tr->priv;
    uint8_t hb[PKT_HDR_SIZE];
    for (;;) {
        if (t->in_sync) {
            if (rd_exact(t, hb, PKT_HDR_SIZE)) break;
        } else {
            if (resync(t)) break;
            memset(hb, 0xCC, 4);
            if (rd_exact(t, hb + 4, PKT_HDR_SIZE - 4)) break;
        }
        if (pkt_parse_header(hb, h)) { t->in_sync = 0; continue; }
        if (h->size && rd_exact(t, payload, h->size)) break;
        return 0;
    }
    return *t->opts.stop ? -1 : 1;
}

static void tcp_close(transport *tr)
{
    tcp_ctx *t = tr->priv;
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
}

transport *transport_tcp_new(const char *host, stream_kind kind, const stream_opts *o)
{
    transport *tr = calloc(1, sizeof *tr);
    tcp_ctx *t = calloc(1, sizeof *t);
    snprintf(t->host, sizeof t->host, "%s", host);
    t->kind = kind; t->fd = -1; t->opts = *o;
    tr->priv = t; tr->open = tcp_open; tr->next_packet = tcp_next; tr->close = tcp_close;
    return tr;
}

void transport_free(transport *t)
{
    if (!t) return;
    t->close(t);
    free(t->priv);
    free(t);
}

int proto_handshake(const hs_io *io, int kind, const stream_opts *o)
{
    uint8_t hello[HELLO_SIZE], ver[2];
    if (io->read_exact(io->ctx, hello, HELLO_SIZE) || hello_parse(hello, ver)) {
        fprintf(stderr, "bad hello packet\n");
        return -1;
    }
    if (!version_supported(ver)) {
        fprintf(stderr, "unsupported protocol version %c%c\n", ver[0], ver[1]);
        return -1;
    }
    uint8_t req[REQ_SIZE];
    req_build(req, ver, kind, o->audio_batching, o->turn_off_screen);
    if (io->write_all(io->ctx, req, REQ_SIZE)) return -1;

    uint8_t resp[RESP03_SIZE];
    size_t rn = version_is_v3(ver) ? RESP03_SIZE : RESP02_SIZE;
    if (io->read_exact(io->ctx, resp, rn)) return -1;
    uint32_t code = resp[0] | resp[1] << 8 | resp[2] << 16 | (uint32_t)resp[3] << 24;
    if (code != HANDSHAKE_OK) {
        fprintf(stderr, "handshake rejected, code %u\n", code);
        return -1;
    }
    return 0;
}
