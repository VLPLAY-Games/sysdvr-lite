#define _GNU_SOURCE
#include "status_led.h"
#include <glob.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *led_dir = "/sys/class/leds/green:status";
static char thermal_path[512], saved_trigger[64];
static pthread_t worker;
static atomic_int stopping;
static _Atomic uint64_t last_frame;
static int hot, current_mode = -1;

enum { WAITING, STREAMING, HOT };

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static int write_led(const char *name, const char *value)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", led_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fputs(value, f) >= 0;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

static void find_cpu_temperature(void)
{
    glob_t paths;
    if (glob("/sys/class/thermal/thermal_zone*/type", 0, NULL, &paths)) { globfree(&paths); return; }
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        char type[64] = "";
        FILE *f = fopen(paths.gl_pathv[i], "r");
        if (!f) continue;
        int ok = fscanf(f, "%63s", type) == 1;
        fclose(f);
        if (ok && !strcmp(type, "cpu-thermal")) {
            snprintf(thermal_path, sizeof thermal_path, "%.*s/temp",
                     (int)(strlen(paths.gl_pathv[i]) - strlen("/type")), paths.gl_pathv[i]);
            break;
        }
    }
    globfree(&paths);
}

static int update_led(uint64_t now)
{
    int temperature;
    FILE *f = thermal_path[0] ? fopen(thermal_path, "r") : NULL;
    if (f) {
        if (fscanf(f, "%d", &temperature) == 1) {
            if (temperature >= 80000) hot = 1;
            else if (temperature <= 76000) hot = 0;
        }
        fclose(f);
    }
    uint64_t last = atomic_load(&last_frame);
    int mode = hot ? HOT : last && now - last < 3000000000ull ? STREAMING : WAITING;
    if (mode == current_mode) return 0;
    int rc;
    if (mode == STREAMING) rc = write_led("trigger", "default-on");
    else {
        const char *delay = mode == HOT ? "125" : "500";
        rc = write_led("trigger", "timer");
        if (!rc) rc = write_led("delay_on", delay);
        if (!rc) rc = write_led("delay_off", delay);
    }
    if (!rc) current_mode = mode;
    return rc;
}

static void *led_thread(void *unused)
{
    (void)unused;
    while (!atomic_load(&stopping)) {
        if (update_led(monotonic_ns())) {
            fprintf(stderr, "status LED: cannot update LED, disabling indication\n");
            break;
        }
        struct timespec delay = { 0, 250000000 };
        nanosleep(&delay, NULL);
    }
    return NULL;
}

static void status_led_stop(void)
{
    atomic_store(&stopping, 1);
    pthread_join(worker, NULL);
    if (saved_trigger[0]) write_led("trigger", saved_trigger);
}

void status_led_start(void)
{
    char path[512], line[4096];
    snprintf(path, sizeof path, "%s/trigger", led_dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "status LED: green:status unavailable\n"); return; }
    if (fgets(line, sizeof line, f)) {
        char *start = strchr(line, '['), *end = start ? strchr(start, ']') : NULL;
        if (end && end - start - 1 < (int)sizeof saved_trigger) {
            memcpy(saved_trigger, start + 1, end - start - 1);
            saved_trigger[end - start - 1] = 0;
        }
    }
    fclose(f);
    find_cpu_temperature();
    if (!thermal_path[0]) fprintf(stderr, "status LED: CPU temperature sensor unavailable\n");
    if (pthread_create(&worker, NULL, led_thread, NULL)) {
        fprintf(stderr, "status LED: cannot start monitor\n");
        return;
    }
    atexit(status_led_stop);
}

void status_led_frame(void) { atomic_store(&last_frame, monotonic_ns()); }
void status_led_waiting(void) { atomic_store(&last_frame, 0); }
