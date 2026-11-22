/* Temporary hardware experiment: no PCM gain, no protocol changes, no heap allocation.
 * Built only with --audio-diagnostic. The normal module does not include this code. */
#include "audio_diag.h"
#ifdef SYSDVR_AUDIO_DIAGNOSTIC
#include <stdatomic.h>
#include "third_party/nanoprintf.h"
#include <string.h>

static Mutex lock;
static bool initialized, audio_ready, log_ready, changed;
static atomic_bool active;
static AudioTarget saved_target;
static FsFileSystem log_fs;
static s64 log_offset;
static u64 last_report;
static s32 last_volume = -1, last_speaker_volume = -1;
static atomic_uint_fast64_t energy, samples;
static atomic_int peak;

static void log_line(const char *message)
{
    if (!log_ready) return;
    size_t len = strlen(message);
    FsFile file;
    if (R_FAILED(fsFsOpenFile(&log_fs, "/sysdvr-audio-diag.txt", FsOpenMode_Write | FsOpenMode_Append, &file))) return;
    if (R_SUCCEEDED(fsFileWrite(&file, log_offset, message, len, FsWriteOption_Flush)))
        log_offset += len;
    fsFileClose(&file); /* DBI/MTP must be able to open the log between writes. */
}

static void restore_locked(void)
{
    if (!atomic_exchange(&active, false)) return;
    AudioTarget current = AudioTarget_Invalid;
    Result query = audctlGetDefaultTarget(&current);
    Result restore = 0;
    if (changed) restore = audctlSetDefaultTarget(saved_target, 100000000, 100000000);
    char line[192];
    npf_snprintf(line, sizeof line, "disconnect saved=%u current=%u query=%x restore=%x\n",
             saved_target, current, query, restore);
    log_line(line);
    changed = false;
}

void AudioDiagInit(void)
{
    mutexInit(&lock);
    initialized = true;
    audio_ready = R_SUCCEEDED(audctlInitialize());
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&log_fs))) {
        fsFsCreateFile(&log_fs, "/sysdvr-audio-diag.txt", 0, 0);
        FsFile file;
        if (R_SUCCEEDED(fsFsOpenFile(&log_fs, "/sysdvr-audio-diag.txt", FsOpenMode_Write | FsOpenMode_Append, &file))) {
            log_ready = R_SUCCEEDED(fsFileGetSize(&file, &log_offset));
            fsFileClose(&file);
        }
        if (!log_ready) fsFsClose(&log_fs);
    }
    log_line(audio_ready ? "audio diagnostic v3 (default target): audctl ready; PCM gain disabled\n" : "audio diagnostic v3 (default target): audctl unavailable; routing unchanged\n");
}

void AudioDiagBegin(void)
{
    if (!initialized || !audio_ready) return;
    mutexLock(&lock);
    if (atomic_load(&active)) { mutexUnlock(&lock); return; }
    AudioTarget original = AudioTarget_Invalid, after = AudioTarget_Invalid;
    Result query = audctlGetDefaultTarget(&original);
    Result route = query;
    changed = false;
    if (R_SUCCEEDED(query) && original != AudioTarget_Invalid) {
        saved_target = original;
        route = audctlSetDefaultTarget(AudioTarget_Headphone, 100000000, 100000000);
        changed = R_SUCCEEDED(route) && original != AudioTarget_Headphone;
        atomic_store(&active, true);
        audctlGetActiveOutputTarget(&after);
        atomic_store(&energy, 0); atomic_store(&samples, 0); atomic_store(&peak, 0);
        last_report = 0; last_volume = -1; last_speaker_volume = -1;
    }
    char line[192];
    npf_snprintf(line, sizeof line, "connect original_default=%u query=%x set_default_headphone=%x active_immediate=%u\n",
             original, query, route, after);
    log_line(line);
    mutexUnlock(&lock);
}

void AudioDiagEnd(void)
{
    if (!initialized || !audio_ready) return;
    mutexLock(&lock);
    restore_locked();
    mutexUnlock(&lock);
}

void AudioDiagObserve(const u8 *data, size_t size)
{
    if (!atomic_load(&active)) return;
    uint64_t sum = 0; int maximum = 0;
    for (size_t i = 0; i + 1 < size; i += 2) {
        int value = (int16_t)((u16)data[i] | (u16)data[i + 1] << 8);
        sum += (int64_t)value * value;
        int magnitude = value < 0 ? -value : value;
        if (magnitude > maximum) maximum = magnitude;
    }
    atomic_fetch_add(&energy, sum); atomic_fetch_add(&samples, size / 2);
    int old = atomic_load(&peak);
    while (old < maximum && !atomic_compare_exchange_weak(&peak, &old, maximum)) {}
}

void AudioDiagPoll(void)
{
    if (!initialized || !audio_ready) return;
    mutexLock(&lock);
    if (!atomic_load(&active)) { mutexUnlock(&lock); return; }
    u32 usb_state = 0;
    if (R_SUCCEEDED(usbDsGetState(&usb_state)) && usb_state != 5) {
        log_line("USB unconfigured: restoring output even if grc is blocked in HOME\n");
        restore_locked();
        mutexUnlock(&lock); return;
    }
    s32 volume = -1, speaker_volume = -1, minimum = -1, maximum = -1;
    AudioTarget default_target = AudioTarget_Invalid;
    bool muted = false;
    AudioTarget target = AudioTarget_Invalid;
    Result vr = audctlGetTargetVolume(&volume, AudioTarget_Headphone);
    Result sr = audctlGetTargetVolume(&speaker_volume, AudioTarget_Speaker);
    Result dr = audctlGetDefaultTarget(&default_target);
    Result tr = audctlGetActiveOutputTarget(&target);
    Result mr = audctlIsTargetMute(&muted, AudioTarget_Headphone);
    audctlGetTargetVolumeMin(&minimum); audctlGetTargetVolumeMax(&maximum);
    u64 now = armGetSystemTick();
    if (volume != last_volume || speaker_volume != last_speaker_volume || now - last_report >= armNsToTicks(2E+9)) {
        uint64_t count = atomic_exchange(&samples, 0), sum = atomic_exchange(&energy, 0);
        int maximum_sample = atomic_exchange(&peak, 0);
        char line[256];
        npf_snprintf(line, sizeof line, "headphone=%d speaker=%d range=%d..%d vr=%x sr=%x default=%u dr=%x active=%u tr=%x mute=%d mr=%x samples=%u mean_square=%u peak=%d\n",
                 volume, speaker_volume, minimum, maximum, vr, sr, default_target, dr, target, tr, muted, mr,
                 (unsigned)count, (unsigned)(count ? sum / count : 0), maximum_sample);
        log_line(line);
        last_volume = volume; last_speaker_volume = speaker_volume; last_report = now;
    }
    mutexUnlock(&lock);
}

void AudioDiagClose(void)
{
    AudioDiagEnd();
    if (audio_ready) audctlExit();
    if (log_ready) fsFsClose(&log_fs);
    initialized = audio_ready = log_ready = false;
}
#endif
