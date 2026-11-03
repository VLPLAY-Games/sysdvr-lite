#define _GNU_SOURCE
#include "watchdog.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_US   60000000ull       /* systemd restarts the client when it hears nothing for this long */
#define READER_LIMIT 40000000000ull    /* ns: the reader loop must have turned round within this time */
#define DECODE_LIMIT 10000000000ull    /* ns: one picture must not take longer than this to decode */

static volatile int *g_stop;
static volatile int g_armed;
static volatile uint64_t g_reader_ns, g_decode_ns;     /* last reader beat, start of the picture being decoded (0 = idle) */
static pthread_t g_thread;
static int g_running;
static char g_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static socklen_t g_pathlen;

static uint64_t now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void notify(const char *msg)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    memcpy(sa.sun_path, g_path, g_pathlen);
    if (sa.sun_path[0] == '@') sa.sun_path[0] = 0;                  /* abstract socket */
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa, offsetof(struct sockaddr_un, sun_path) + g_pathlen);
    close(fd);
}

static int healthy(void)
{
    if (!g_armed) return 1;
    uint64_t t = now_ns(), r = g_reader_ns, d = g_decode_ns;
    if (t - r > READER_LIMIT) return 0;
    if (d && t - d > DECODE_LIMIT) return 0;
    return 1;
}

static void *beat_thread(void *arg)
{
    (void)arg;
    uint64_t next = now_ns();
    while (!*g_stop && g_running) {
        struct timespec ts = { 0, 250000000 };
        nanosleep(&ts, NULL);
        if (now_ns() < next) continue;
        next = now_ns() + TIMEOUT_US * 1000ull / 3;                 /* three heartbeats per timeout */
        if (healthy()) notify("WATCHDOG=1");
        else fprintf(stderr, "watchdog: no progress, systemd will restart the client\n");
    }
    return NULL;
}

void watchdog_start(volatile int *stop)
{
    const char *p = getenv("NOTIFY_SOCKET");
    if (!p || !*p || strlen(p) >= sizeof g_path) return;
    g_pathlen = (socklen_t)strlen(p);
    memcpy(g_path, p, g_pathlen);
    g_stop = stop;
    g_reader_ns = now_ns();
    char msg[64];
    snprintf(msg, sizeof msg, "WATCHDOG_USEC=%llu\nWATCHDOG=1", (unsigned long long)TIMEOUT_US);
    notify(msg);
    g_running = 1;
    if (pthread_create(&g_thread, NULL, beat_thread, NULL)) g_running = 0;
}

void watchdog_armed(void) { g_reader_ns = now_ns(); g_armed = 1; }
void watchdog_beat(void) { g_reader_ns = now_ns(); }
void watchdog_decode_begin(void) { g_decode_ns = now_ns(); }
void watchdog_decode_end(void) { g_decode_ns = 0; }

void watchdog_stop(void)
{
    if (!g_running) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
}
