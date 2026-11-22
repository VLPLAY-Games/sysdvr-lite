#pragma once
#include <switch.h>
#ifdef SYSDVR_AUDIO_DIAGNOSTIC
void AudioDiagInit(void);
void AudioDiagBegin(void);
void AudioDiagEnd(void);
void AudioDiagPoll(void);
void AudioDiagObserve(const u8 *data, size_t size);
void AudioDiagClose(void);
#else
static inline void AudioDiagInit(void) {}
static inline void AudioDiagBegin(void) {}
static inline void AudioDiagEnd(void) {}
static inline void AudioDiagPoll(void) {}
static inline void AudioDiagObserve(const u8 *data, size_t size) { (void)data; (void)size; }
static inline void AudioDiagClose(void) {}
#endif
