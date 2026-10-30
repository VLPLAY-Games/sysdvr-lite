#ifndef PROTO_H
#define PROTO_H
#include <stdint.h>
#include <stddef.h>

/* SysDVR wire protocol, see sysdvr/protocol.md and sysmodule/source/capture.h */

#define PKT_MAGIC        0xCCCCCCCCu
#define REQ_MAGIC        0xAAAAAAAAu
#define VBUF_SZ          0x54000
#define PKT_HDR_SIZE     18
#define PKT_MAX_PAYLOAD  VBUF_SZ                 /* audio max (0x1000*6) is smaller */
#define PKT_MAX_TRANSFER (PKT_MAX_PAYLOAD + PKT_HDR_SIZE)

#define HELLO_SIZE       10
#define REQ_SIZE         16
#define RESP02_SIZE      4
#define RESP03_SIZE      72
#define HANDSHAKE_OK     6u

#define META_VIDEO   (1 << 0)
#define META_AUDIO   (1 << 1)
#define META_DATA    (1 << 2)
#define META_REPLAY  (1 << 3)
#define META_MULTINAL (1 << 4)
#define META_ERROR   (1 << 5)

#define VIDEO_W 1280
#define VIDEO_H 720

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint64_t timestamp;   /* usec */
    uint8_t  flags;
    uint8_t  replay_slot;
} pkt_header;

typedef enum { STREAM_VIDEO = 1, STREAM_AUDIO = 2 } stream_kind;

/* Little-endian explicit (de)serialisation, no struct packing games */
int  pkt_parse_header(const uint8_t *buf, pkt_header *h);   /* 0 ok, -1 invalid */
void req_build(uint8_t out[REQ_SIZE], uint8_t version[2], int kind,
               int audio_batching, int turn_off_screen);
int  hello_parse(const uint8_t *buf, uint8_t version[2]);   /* 0 ok */
int  version_supported(const uint8_t v[2]);
int  version_is_v3(const uint8_t v[2]);

#endif
