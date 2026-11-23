#define _GNU_SOURCE
#include "banner.h"
#include "ui_language.h"
#include "hdmi_audio.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYSDVR_VERSION
#define SYSDVR_VERSION ""
#endif

static char g_decoder[32] = "", g_audio[80] = "";

void banner_set(const char *decoder, const char *audio)
{
    snprintf(g_decoder, sizeof g_decoder, "%s", decoder ? decoder : "");
    snprintf(g_audio, sizeof g_audio, "%s", audio ? audio : "");
}

/* the screen height in pixels, or 0 */
static int screen_height(void)
{
    FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    int w = 0, h = 0;
    if (f) { if (fscanf(f, "%d,%d", &w, &h) != 2) h = 0; fclose(f); }
    return h;
}

/* A Cyrillic console font that gives about 26 rows or more on this screen (the biggest that does) */
static const char *pick_font(int height)
{
    static const struct { int h; const char *file; } F[] = {
        { 118, "CyrSlav-DejaVu118x64.psf.gz" }, { 89, "CyrSlav-DejaVu89x48.psf.gz" }, { 74, "CyrSlav-DejaVu74x40.psf.gz" },
        { 59, "CyrSlav-DejaVu59x32.psf.gz" }, { 51, "CyrSlav-DejaVu51x28.psf.gz" }, { 43, "CyrSlav-DejaVu43x24.psf.gz" },
        { 36, "CyrSlav-DejaVu36x20.psf.gz" }, { 30, "CyrSlav-DejaVu30x16.psf.gz" }, { 16, "CyrSlav-Fixed16.psf.gz" },
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++)
        if (height <= 0 || height / F[i].h >= 26 || F[i].h == 16) return F[i].file;
    return NULL;
}

static int run(const char *const argv[])
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

void banner_show(void)
{
    int fd = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return;
    int cyrillic = 0;
    int russian = ui_is_russian();
    const char *font = pick_font(screen_height());
    if (russian && font && access("/usr/bin/setfont", X_OK) == 0) {
        char path[160];
        snprintf(path, sizeof path, "/usr/share/consolefonts/%s", font);
        if (access(path, R_OK) == 0) {
            const char *const argv[] = { "/usr/bin/setfont", "-C", "/dev/tty1", path, NULL };
            cyrillic = run(argv) == 0;
        }
    }
    char buf[4096];
    hdmi_warning warning = hdmi_audio_warning();
    int use_ru = russian && cyrillic;
    int n = snprintf(buf, sizeof buf, "\033[2J\033[H\033[0msysdvr-lite %s\n\n%s\n%s\n%s: %s   %s: %s\n",
        SYSDVR_VERSION,
        use_ru ? "Док готов. Можно подключать Switch." : "Dock ready. Connect your Switch.",
        use_ru ? "Запустите игру. Подключите USB. SysDVR в режиме USB." : "Start a game. Connect USB. SysDVR USB mode.",
        use_ru ? "Декодер" : "Decoder", g_decoder, use_ru ? "Звук" : "Audio", g_audio);
    if (warning != HDMI_WARNING_NONE)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "\n[!] %s\n", hdmi_audio_message(warning, use_ru));
    if (n > 0) { ssize_t w = write(fd, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1); (void)w; }
    close(fd);
}
