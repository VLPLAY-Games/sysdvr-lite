#include "usb_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static usb_stream stream;
static uint8_t payload[PKT_MAX_PAYLOAD], wire[USB_STREAM_CAPACITY];
static size_t packet(uint8_t *p, size_t total, uint8_t flags, uint64_t ts)
{
    assert(total >= PKT_HDR_SIZE && total <= PKT_MAX_TRANSFER);
    memset(p, 0, total);
    memset(p, 0xcc, 4);
    uint32_t n = total - PKT_HDR_SIZE;
    for (int i = 0; i < 4; i++) p[4+i] = (uint8_t)(n >> (8*i));
    for (int i = 0; i < 8; i++) p[8+i] = (uint8_t)(ts >> (8*i));
    p[16] = flags;
    memset(p + PKT_HDR_SIZE, flags, n);
    return total;
}
static void append(const uint8_t *p, size_t n)
{
    usb_stream_compact(&stream);
    assert(stream.end + n <= sizeof stream.data);
    memcpy(stream.data + stream.end, p, n); stream.end += n;
}
static void take(uint8_t flags, uint64_t ts, size_t size)
{
    pkt_header h;
    assert(usb_stream_next(&stream, &h, payload) == 1);
    assert(h.flags == flags && h.timestamp == ts && h.size == size);
    for (size_t i = 0; i < size; i++) assert(payload[i] == flags);
}
int main(void)
{
    pkt_header h;
    const size_t video[] = {13824, 16384, 22528, 12800, 14336};
    for (size_t i = 0; i < sizeof video / sizeof *video; i++) {
        size_t n = packet(wire, video[i], META_VIDEO, 100);
        n += packet(wire+n, 8210, META_AUDIO, 200);
        n += packet(wire+n, 8210, META_AUDIO, 300);
        /* Every possible split, including inside headers and audio payloads. */
        for (size_t split = 0; split <= n; split++) {
            stream.begin = stream.end = 0;
            append(wire, split);
            int first = split >= video[i];
            if (first) take(META_VIDEO, 100, video[i]-PKT_HDR_SIZE);
            int second = split >= video[i]+8210;
            if (second) take(META_AUDIO, 200, 8192);
            int third = split == n;
            if (third) take(META_AUDIO, 300, 8192);
            assert(usb_stream_next(&stream, &h, payload) == 0);
            append(wire+split, n-split);
            if (!first) take(META_VIDEO, 100, video[i]-PKT_HDR_SIZE);
            if (!second) take(META_AUDIO, 200, 8192);
            if (!third) take(META_AUDIO, 300, 8192);
            assert(usb_stream_next(&stream, &h, payload) == 0);
        }
    }
    /* Largest valid packet, split across aligned bulk reads, plus next packet. */
    size_t n = packet(wire, PKT_MAX_TRANSFER, META_VIDEO, 400);
    n += packet(wire+n, 8210, META_AUDIO, 500);
    append(wire, USB_READ_SIZE);
    take(META_VIDEO, 400, PKT_MAX_PAYLOAD);
    assert(usb_stream_next(&stream, &h, payload) == 0);
    append(wire+USB_READ_SIZE, n-USB_READ_SIZE);
    take(META_AUDIO, 500, 8192);
    append((const uint8_t *)"SysDVR|03\0", HELLO_SIZE);
    assert(usb_stream_next(&stream, &h, payload) == 2);
    stream.begin = stream.end = 0;
    memset(wire, 0, PKT_HDR_SIZE); append(wire, PKT_HDR_SIZE);
    assert(usb_stream_next(&stream, &h, payload) == -1);
    stream.begin = stream.end = 0;
    packet(wire, PKT_HDR_SIZE, META_AUDIO, 600);
    wire[4] = wire[5] = wire[6] = wire[7] = 0xff;
    append(wire, PKT_HDR_SIZE);
    assert(usb_stream_next(&stream, &h, payload) == -1);
    puts("USB stream tests passed");
}
