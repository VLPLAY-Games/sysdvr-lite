#include "proto.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int pkt_parse_header(const uint8_t *b, pkt_header *h)
{
    h->magic = rd32(b);
    h->size = rd32(b + 4);
    h->timestamp = (uint64_t)rd32(b + 8) | (uint64_t)rd32(b + 12) << 32;
    h->flags = b[16];
    h->replay_slot = b[17];
    if (h->magic != PKT_MAGIC || h->size > PKT_MAX_PAYLOAD)
        return -1;
    return 0;
}

int hello_parse(const uint8_t *b, uint8_t v[2])
{
    if (memcmp(b, "SysDVR|", 7) != 0 || b[9] != 0)
        return -1;
    v[0] = b[7];
    v[1] = b[8];
    return 0;
}

int version_supported(const uint8_t v[2])
{
    return v[0] == '0' && (v[1] == '2' || v[1] == '3');
}

int version_is_v3(const uint8_t v[2]) { return v[0] == '0' && v[1] == '3'; }

void req_build(uint8_t o[REQ_SIZE], uint8_t v[2], int kind, int audio_batching, int turn_off_screen)
{
    memset(o, 0, REQ_SIZE);
    o[0] = o[1] = o[2] = o[3] = 0xAA;
    o[4] = v[0];                       /* version is compared with memcmp: "02" in memory order */
    o[5] = v[1];
    o[6] = ((kind & STREAM_VIDEO) ? META_VIDEO : 0) | ((kind & STREAM_AUDIO) ? META_AUDIO : 0);
    o[7] = 1 << 1;                     /* VideoFlags: InjectPPSSPS (NAL replay left off) */
    o[8] = (uint8_t)audio_batching;
    o[9] = (version_is_v3(v) && turn_off_screen) ? 1 : 0;   /* FeatureFlags bit0 */
}
