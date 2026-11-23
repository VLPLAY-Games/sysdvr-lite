#include "audio_sync.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    audio_sync s = {0};
    /* Preserve startup and normal timing, including microsecond rounding. */
    assert(audio_sync_ready(&s, 0, 8192, 0));
    assert(audio_sync_ready(&s, 42667, 8192, 18000));
    assert(audio_sync_ready(&s, 85333, 4096, 10000));
    assert(audio_sync_ready(&s, 106667, 8192, 12000));
    /* A missing 42.67 ms block must not trigger a new silence/drop correction. */
    assert((int64_t)192000 - 150544 > 40000); /* Old calculation would pad 41.456 ms. */
    assert(!audio_sync_ready(&s, 192000, 8192, 0));
    assert(audio_sync_ready(&s, 234667, 8192, 17000));
    /* Clock failure suppresses correction, not packet delivery or future sync. */
    assert(!audio_sync_ready(&s, 277333, 8192, -1));
    assert(audio_sync_ready(&s, 320000, 8192, 20000));
    /* A new console/session clock re-anchors once, then resumes normally. */
    assert(!audio_sync_ready(&s, 1000, 8192, 20000));
    assert(audio_sync_ready(&s, 43667, 8192, 18000));
    puts("audio sync discontinuity tests passed");
}
