#define _GNU_SOURCE
#include "sink.h"
#include "font.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#ifdef HAVE_GL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>
#endif

/* Plain KMS output without X/Wayland. The H618 display engine in mainline has no YUV
 * planes, so frames are converted to XRGB8888 on the CPU into a dumb buffer. The video is
 * shown 1:1 (centered), the display mode is chosen to match it, never scaled. */

typedef struct { uint32_t handle, fb, pitch; size_t size; uint8_t *map; int prime; unsigned fbo, tex; void *img; } dbuf;

typedef struct {
    int fd;
    uint32_t crtc, conn;
    drmModeModeInfo mode;
    drmModeCrtc *saved;
    pthread_mutex_t lock;        /* show() runs in the decode thread, idle()/resume() in the reader thread */
    int idle;
    int vw, vh;                  /* size of the video */
    int exact;                   /* the mode has the size of the video */
    int scale;                   /* fit the picture to the display when the sizes differ (GPU path only) */
    int gone;                    /* the display is switched off or unplugged: nothing is drawn */
    uint64_t check_ns;           /* last time the connector was looked at */
    dbuf buf[2];
    int back, flip_pending, first;
    uint64_t inflight_recv;      /* arrival time of the frame in the pending flip */
    double lat_sum, lat_max; unsigned lat_n;
#ifdef HAVE_GL
    int gl_ok, gl_tried, gbm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy; EGLContext ctx;
    unsigned prog; int loc_pos;
    void *src_img[16]; int src_fd[16];
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
#endif
} drm;

static int make_buf(drm *d, dbuf *b)
{
    struct drm_mode_create_dumb cr = { .width = d->mode.hdisplay, .height = d->mode.vdisplay, .bpp = 32 };
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) return -1;
    b->handle = cr.handle; b->pitch = cr.pitch; b->size = cr.size;
    if (drmModeAddFB(d->fd, cr.width, cr.height, 24, 32, cr.pitch, cr.handle, &b->fb)) return -1;
    struct drm_mode_map_dumb mp = { .handle = cr.handle };
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mp)) return -1;
    b->map = mmap(NULL, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, mp.offset);
    if (b->map == MAP_FAILED) return -1;
    memset(b->map, 0, cr.size);
    return 0;
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* NV12 (BT.709 limited range) -> XRGB8888, 1:1, centered, clipped to the buffer */
static void convert(const frame *f, dbuf *b, int bw, int bh)
{
    int ox = (bw - f->width) / 2, oy = (bh - f->height) / 2;
    int w = f->width, h = f->height, sx = 0, sy = 0;
    if (ox < 0) { sx = -ox; w += ox; ox = 0; }
    if (oy < 0) { sy = -oy; h += oy; oy = 0; }
    if (w > bw) w = bw;
    if (h > bh) h = bh;
    sx &= ~1; sy &= ~1;
    for (int y = 0; y < h; y++) {
        const uint8_t *Y = f->data[0] + (size_t)(y + sy) * f->stride[0] + sx;
        const uint8_t *UV = f->data[1] + (size_t)((y + sy) >> 1) * f->stride[1] + (f->fmt == PIX_NV12 ? sx : sx / 2);
        const uint8_t *V = f->data[2] ? f->data[2] + (size_t)((y + sy) >> 1) * f->stride[2] + sx / 2 : NULL;
        uint32_t *out = (uint32_t *)(b->map + (size_t)(y + oy) * b->pitch) + ox;
        for (int x = 0; x < w; x += 2) {
            int u, v;
            if (f->fmt == PIX_NV12) { u = UV[x] - 128; v = UV[x + 1] - 128; }
            else { u = UV[x >> 1] - 128; v = V[x >> 1] - 128; }
            int rv = 459 * v, gv = -55 * u - 136 * v, bu = 541 * u;
            for (int k = 0; k < 2; k++) {
                int yy = 298 * (Y[x + k] - 16) + 128;
                out[x + k] = (uint32_t)clamp8((yy + rv) >> 8) << 16 | clamp8((yy + gv) >> 8) << 8 | clamp8((yy + bu) >> 8);
            }
        }
    }
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
    drm *d = data;
    if (!getenv("SYSDVR_LATENCY") || !d->inflight_recv) return;
    /* vblank time of the flip (CLOCK_MONOTONIC) minus the arrival time of the frame's packet */
    double ms = ((uint64_t)sec * 1000000ull + usec) / 1e3 - d->inflight_recv / 1e6;
    d->lat_sum += ms; if (ms > d->lat_max) d->lat_max = ms;
    if (++d->lat_n == 150) {
        fprintf(stderr, "latency ms: arrival -> on screen (vblank) avg %.2f max %.2f\n", d->lat_sum / d->lat_n, d->lat_max);
        d->lat_sum = d->lat_max = 0; d->lat_n = 0;
    }
}

static void wait_flip(drm *d)
{
    while (d->flip_pending) {
        struct pollfd p = { d->fd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) { d->flip_pending = 0; break; }
        drmEventContext ev = { .version = 3, .page_flip_handler = on_flip };
        drmHandleEvent(d->fd, &ev);
        d->flip_pending = 0;     /* only one flip in flight */
    }
}

static uint64_t now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

/* An exact mode wins (highest refresh rate). Without one, the smallest mode that contains the video:
 * the picture is then shown 1:1 or scaled up as little as possible. SYSDVR_MODE=WxH forces a mode (testing). */
static int choose_mode(const drmModeConnector *c, int vw, int vh, int *exact)
{
    int best = -1;
    for (int i = 0; i < c->count_modes; i++) {
        const drmModeModeInfo *m = &c->modes[i];
        if ((m->flags & DRM_MODE_FLAG_INTERLACE) || m->hdisplay != vw || m->vdisplay != vh) continue;
        if (best < 0 || m->vrefresh > c->modes[best].vrefresh) best = i;
    }
    *exact = best >= 0;
    const char *forced = getenv("SYSDVR_MODE");
    int fw, fh;
    if (forced && sscanf(forced, "%dx%d", &fw, &fh) == 2) {
        for (int i = 0; i < c->count_modes; i++)
            if (c->modes[i].hdisplay == fw && c->modes[i].vdisplay == fh && !(c->modes[i].flags & DRM_MODE_FLAG_INTERLACE)) {
                *exact = fw == vw && fh == vh;
                return i;
            }
    }
    if (best >= 0) return best;
    long area = 0;
    for (int i = 0; i < c->count_modes; i++) {
        const drmModeModeInfo *m = &c->modes[i];
        if ((m->flags & DRM_MODE_FLAG_INTERLACE) || m->hdisplay < vw || m->vdisplay < vh) continue;
        long a = (long)m->hdisplay * m->vdisplay;
        if (best < 0 || a < area || (a == area && m->vrefresh > c->modes[best].vrefresh)) { best = i; area = a; }
    }
    return best >= 0 ? best : 0;                    /* nothing is big enough: the connector's preferred mode */
}

/* Finds the connected display and the mode to use on it (also used again when the display comes back) */
static int pick_display(drm *d, int vw, int vh, int quiet)
{
    drmModeRes *r = drmModeGetResources(d->fd);
    drmModeConnector *c = NULL;
    for (int i = 0; r && i < r->count_connectors; i++) {
        c = drmModeGetConnector(d->fd, r->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) break;
        drmModeFreeConnector(c); c = NULL;
    }
    if (!c) {
        if (!quiet) fprintf(stderr, "drm: no connected display, waiting for one\n");
        drmModeFreeResources(r);
        return -1;
    }
    int exact;
    d->mode = c->modes[choose_mode(c, vw, vh, &exact)];
    d->exact = exact;
    d->conn = c->connector_id;
    drmModeEncoder *e = c->encoder_id ? drmModeGetEncoder(d->fd, c->encoder_id) : NULL;
    d->crtc = e ? e->crtc_id : r->crtcs[0];
    if (e) drmModeFreeEncoder(e);
    if (!d->saved) d->saved = drmModeGetCrtc(d->fd, d->crtc);        /* the console's own state, put back later */
    drmModeFreeConnector(c);
    drmModeFreeResources(r);
    return 0;
}

/* The display was switched off, unplugged or replaced while streaming? Looked at twice a second.
 * Returns 1 when drawing makes sense. Called with the lock held. */
static int display_ok(drm *d, int vw, int vh)
{
    uint64_t t = now_ns();
    if (t - d->check_ns < 500000000ull) return !d->gone;
    d->check_ns = t;
    drmModeConnector *c = drmModeGetConnectorCurrent(d->fd, d->conn);
    int connected = c && c->connection == DRM_MODE_CONNECTED;
    drmModeFreeConnector(c);
    if (!connected) {
        if (!d->gone) { fprintf(stderr, "drm: the display went away\n"); d->gone = 1; }
        return 0;
    }
    if (!d->gone) return 1;
    int ow = d->mode.hdisplay, oh = d->mode.vdisplay;
    if (pick_display(d, vw, vh, 1)) return 0;
    d->gone = 0;
    if (d->mode.hdisplay != ow || d->mode.vdisplay != oh) {         /* the buffers have the old size: start over */
        fprintf(stderr, "drm: a different display (%dx%d -> %dx%d), restarting\n", ow, oh, d->mode.hdisplay, d->mode.vdisplay);
        raise(SIGTERM);
        return 0;
    }
    fprintf(stderr, "drm: the display is back\n");
    d->first = 1;                                                    /* set the mode again */
    return 1;
}

#ifdef HAVE_GL
static const char *VS =
    "attribute vec2 p; varying vec2 t;\n"
    "void main() { t = vec2(p.x * 0.5 + 0.5, 0.5 + p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *FS =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float; varying vec2 t; uniform samplerExternalOES s;\n"
    "void main() { gl_FragColor = vec4(texture2D(s, t).rgb, 1.0); }\n";

static unsigned compile(unsigned type, const char *src)
{
    unsigned sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    int ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(sh, sizeof log, NULL, log); fprintf(stderr, "gl: shader: %s\n", log); return 0; }
    return sh;
}

static int gl_init(drm *d)
{
    d->gbm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (d->gbm_fd < 0) return -1;
    d->gbm = gbm_create_device(d->gbm_fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!d->gbm || !get_dpy) return -1;
    d->dpy = get_dpy(EGL_PLATFORM_GBM_KHR, d->gbm, NULL);
    EGLint maj, min;
    if (!eglInitialize(d->dpy, &maj, &min)) return -1;
    const char *ext = eglQueryString(d->dpy, EGL_EXTENSIONS);
    if (!strstr(ext, "EGL_EXT_image_dma_buf_import") || !strstr(ext, "EGL_KHR_surfaceless_context")) {
        fprintf(stderr, "gl: missing EGL extensions\n"); return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n;
    if (!eglChooseConfig(d->dpy, ca, &cfg, 1, &n) || !n) return -1;
    EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    d->ctx = eglCreateContext(d->dpy, cfg, EGL_NO_CONTEXT, cx);
    if (d->ctx == EGL_NO_CONTEXT || !eglMakeCurrent(d->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, d->ctx)) return -1;
    d->create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    d->destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    d->image_target = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!d->create_image || !d->image_target) return -1;
    fprintf(stderr, "gl: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    unsigned vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return -1;
    d->prog = glCreateProgram();
    glAttachShader(d->prog, vs); glAttachShader(d->prog, fs);
    glBindAttribLocation(d->prog, 0, "p");
    glLinkProgram(d->prog);
    int ok = 0; glGetProgramiv(d->prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "gl: link failed\n"); return -1; }

    /* render targets: the same dumb buffers that KMS scans out, shared through dmabuf */
    for (int i = 0; i < 2; i++) {
        dbuf *b = &d->buf[i];
        if (drmPrimeHandleToFD(d->fd, b->handle, DRM_CLOEXEC | DRM_RDWR, &b->prime)) { perror("gl: prime export"); return -1; }
        EGLint ia[] = { EGL_WIDTH, d->mode.hdisplay, EGL_HEIGHT, d->mode.vdisplay,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, b->prime, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)b->pitch, EGL_NONE };
        b->img = d->create_image(d->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
        if (!b->img) { fprintf(stderr, "gl: cannot import scanout buffer (0x%x)\n", eglGetError()); return -1; }
        glGenTextures(1, &b->tex);
        glBindTexture(GL_TEXTURE_2D, b->tex);
        d->image_target(GL_TEXTURE_2D, b->img);
        glGenFramebuffers(1, &b->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "gl: scanout fbo incomplete\n"); return -1; }
    }
    for (int i = 0; i < 16; i++) d->src_fd[i] = -1;
    return 0;
}

/* Returns 0 if the frame was drawn into the back buffer */
static int gl_draw(drm *d, const frame *f, dbuf *b)
{
    int id = f->buf_id & 15;
    if (d->src_fd[id] != f->dmabuf_fd || !d->src_img[id]) {
        if (d->src_img[id]) d->destroy_image(d->dpy, d->src_img[id]);
        EGLint ia[] = { EGL_WIDTH, f->width, EGL_HEIGHT, f->height,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
            EGL_DMA_BUF_PLANE0_FD_EXT, f->dmabuf_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, f->stride[0],
            EGL_DMA_BUF_PLANE1_FD_EXT, f->dmabuf_fd, EGL_DMA_BUF_PLANE1_OFFSET_EXT, f->stride[0] * f->height, EGL_DMA_BUF_PLANE1_PITCH_EXT, f->stride[1],
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT, EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT, EGL_NONE };
        d->src_img[id] = d->create_image(d->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
        d->src_fd[id] = f->dmabuf_fd;
        if (!d->src_img[id]) { fprintf(stderr, "gl: cannot import decoder buffer (0x%x)\n", eglGetError()); return -1; }
    }
    static unsigned tex;
    if (!tex) glGenTextures(1, &tex);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    int W = d->mode.hdisplay, H = d->mode.vdisplay;
    int pw = f->width, ph = f->height;                      /* size of the picture on screen */
    if (d->scale && (pw != W || ph != H)) {                 /* no mode of the video's size: fit it, keeping the aspect ratio */
        if ((long)W * ph <= (long)H * pw) { pw = W; ph = (int)((long)f->height * W / f->width); }
        else { ph = H; pw = (int)((long)f->width * H / f->height); }
    }
    if (pw != W || ph != H) { glViewport(0, 0, W, H); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }
    glViewport((W - pw) / 2, (H - ph) / 2, pw, ph);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    d->image_target(GL_TEXTURE_EXTERNAL_OES, d->src_img[id]);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUseProgram(d->prog);
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    if (getenv("SYSDVR_GLDEBUG")) {
        unsigned char px[4] = {0};
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        static int n; if (n++ < 3) fprintf(stderr, "gl: err=0x%x fbo=0x%x center=%u,%u,%u,%u mapbyte=%u\n", glGetError(),
            glCheckFramebufferStatus(GL_FRAMEBUFFER), px[0], px[1], px[2], px[3], b->map[(H / 2) * b->pitch + (W / 2) * 4 + 2]);
    }
    return 0;
}
#endif

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

static void show_locked(sink *s, const frame *f)
{
    drm *d = s->priv;
#ifdef HAVE_GL
    if (!d->gl_tried) {     /* EGL contexts are per-thread: create it in the thread that draws */
        d->gl_tried = 1;
        if (!getenv("SYSDVR_NOGL")) {
            if (gl_init(d) == 0) { d->gl_ok = 1; fprintf(stderr, "drm: GPU conversion enabled\n"); }
            else fprintf(stderr, "drm: GPU path unavailable, falling back to CPU conversion\n");
        }
    }
#endif
    static double t_cp, t_cv, t_fl; static int n; static uint8_t *tmp;
    double a = now_ms();
    frame g;
    if (f->uncached && !(d->gl_ok && f->dmabuf_fd >= 0)) {            /* reading dma memory directly is ~3x slower than one linear copy */
        if (!tmp) tmp = malloc((size_t)f->stride[0] * f->height * 3 / 2);
        memcpy(tmp, f->data[0], (size_t)f->stride[0] * f->height * 3 / 2);
        g = *f; g.data[0] = tmp; g.data[1] = tmp + (size_t)f->stride[0] * f->height;
        f = &g;
    }
    double tb = now_ms();
    dbuf *b = &d->buf[d->back];
#ifdef HAVE_GL
    if (d->gl_ok && f->dmabuf_fd >= 0 && f->fmt == PIX_NV12 && !gl_draw(d, f, b)) goto drawn;
#endif
    convert(f, b, d->mode.hdisplay, d->mode.vdisplay);
drawn:;
    double c = now_ms();
    { static int cnt; const char *dump = getenv("SYSDVR_DUMP");
      if (dump && ++cnt == 60) {                  /* debugging aid: save what is being scanned out as raw XRGB */
          FILE *fp = fopen(dump, "wb");
          if (fp) { for (int y = 0; y < d->mode.vdisplay; y++) fwrite(b->map + (size_t)y * b->pitch, 4, d->mode.hdisplay, fp); fclose(fp); }
      } }
    if (d->first) {
        drmModeSetCrtc(d->fd, d->crtc, b->fb, 0, 0, &d->conn, 1, &d->mode);
        d->first = 0;
    } else {
        wait_flip(d);
        d->inflight_recv = f->recv_ns;
        if (!drmModePageFlip(d->fd, d->crtc, b->fb, DRM_MODE_PAGE_FLIP_EVENT, d)) d->flip_pending = 1;
    }
    d->back ^= 1;
    double e = now_ms();
    t_cp += tb - a; t_cv += c - tb; t_fl += e - c;
    if (getenv("SYSDVR_PROF") && ++n == 60) { fprintf(stderr, "drm avg ms: copy %.2f convert %.2f flip %.2f\n", t_cp / 60, t_cv / 60, t_fl / 60); n = 0; t_cp = t_cv = t_fl = 0; }
}

static void show(sink *s, const frame *f)
{
    drm *d = s->priv;
    pthread_mutex_lock(&d->lock);
    if (!d->idle && display_ok(d, d->vw, d->vh)) show_locked(s, f);    /* frames in flight after the stream ended are dropped */
    pthread_mutex_unlock(&d->lock);
}

/* Stream ended: put the console framebuffer back and let go of the display */
static void idle(sink *s)
{
    drm *d = s->priv;
    pthread_mutex_lock(&d->lock);
    if (!d->idle) {
        if (!d->first) {                      /* something was shown, so the mode was changed */
            wait_flip(d);
            if (d->saved && d->saved->buffer_id)
                drmModeSetCrtc(d->fd, d->saved->crtc_id, d->saved->buffer_id, d->saved->x, d->saved->y, &d->conn, 1, &d->saved->mode);
        }
        drmDropMaster(d->fd);                 /* otherwise the kernel console cannot draw */
        d->idle = 1;
        d->first = 1;
    }
    pthread_mutex_unlock(&d->lock);
}

static void resume(sink *s)
{
    drm *d = s->priv;
    pthread_mutex_lock(&d->lock);
    if (d->idle) {
        drmSetMaster(d->fd);
        d->idle = 0;
        d->first = 1;                         /* next frame sets the video mode again */
        int ow = d->mode.hdisplay, oh = d->mode.vdisplay;
        if (pick_display(d, d->vw, d->vh, 1)) d->gone = 1;               /* switched off meanwhile: display_ok() waits for it */
        else {
            d->gone = 0;
            if (d->mode.hdisplay != ow || d->mode.vdisplay != oh) {      /* a different display was plugged in */
                fprintf(stderr, "drm: a different display (%dx%d -> %dx%d), restarting\n", ow, oh, d->mode.hdisplay, d->mode.vdisplay);
                raise(SIGTERM);
            }
        }
    }
    pthread_mutex_unlock(&d->lock);
}

/* ---- messages: CPU drawn into the scanout buffer with the bitmap font ---- */

static uint32_t next_codepoint(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    if (c >= 0xF0)      { c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63); s += 3; }
    else if (c >= 0xE0) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
    else if (c >= 0xC0) { c = (c & 31) << 6 | (s[0] & 63); s += 1; }
    *p = (const char *)s;
    return c;
}

static int text_width(const char *s, int scale)
{
    int n = 0;
    while (*s) { next_codepoint(&s); n++; }
    return n * FONT_CELL_W * scale;
}

static void draw_text(dbuf *b, int bw, int bh, int x, int y, const char *s, uint32_t color, int scale)
{
    while (*s) {
        const uint16_t *g = font_glyph(next_codepoint(&s));
        for (int row = 0; row < FONT_CELL_H; row++)
            for (int col = 0; col < FONT_CELL_W; col++) {
                if (!(g[row] >> (15 - col) & 1)) continue;
                for (int dy = 0; dy < scale; dy++) {
                    int py = y + row * scale + dy;
                    if (py < 0 || py >= bh) continue;
                    uint32_t *line = (uint32_t *)(b->map + (size_t)py * b->pitch);
                    for (int dx = 0; dx < scale; dx++) {
                        int px = x + col * scale + dx;
                        if (px >= 0 && px < bw) line[px] = color;
                    }
                }
            }
        x += FONT_CELL_W * scale;
    }
}

static void notice(sink *s, const notice_text *t, int count)
{
    drm *d = s->priv;
    pthread_mutex_lock(&d->lock);
    if (!d->idle && !d->gone) {
        dbuf *b = &d->buf[d->back];
        int W = d->mode.hdisplay, H = d->mode.vdisplay;
        for (int y = 0; y < H; y++) {
            uint32_t *line = (uint32_t *)(b->map + (size_t)y * b->pitch);
            for (int x = 0; x < W; x++) line[x] = 0x001C1C1C;
        }
        int sc = H >= 1000 ? 2 : 1;                                  /* body scale; the first block has a bigger title */
        int lh = (FONT_CELL_H + 6) * sc;
        int total = 0;
        for (int k = 0; k < count; k++) {
            int n = 0; while (n < 6 && t[k].lines[n]) n++;
            total += FONT_CELL_H * (k ? sc : 2 * sc) + 16 * sc + n * lh + (t[k].footer ? 12 * sc + FONT_CELL_H * sc : 0);
            if (k + 1 < count) total += 34 * sc;
        }
        int y = (H - total) / 2;
        if (y < 0) y = 0;
        for (int k = 0; k < count; k++) {
            int tsc = k ? sc : 2 * sc;
            uint32_t ct = k ? 0x00B4B4B4 : 0x00FFFFFF, cb = k ? 0x00909090 : 0x00D0D0D0, cf = k ? 0x002A9AB8 : 0x0032C8FF;
            draw_text(b, W, H, (W - text_width(t[k].title, tsc)) / 2, y, t[k].title, ct, tsc);
            y += FONT_CELL_H * tsc + 16 * sc;
            for (int i = 0; i < 6 && t[k].lines[i]; i++, y += lh)
                draw_text(b, W, H, (W - text_width(t[k].lines[i], sc)) / 2, y, t[k].lines[i], cb, sc);
            if (t[k].footer) {
                y += 12 * sc;
                draw_text(b, W, H, (W - text_width(t[k].footer, sc)) / 2, y, t[k].footer, cf, sc);
                y += FONT_CELL_H * sc;
            }
            y += 34 * sc;
        }
        const char *dump = getenv("SYSDVR_DUMP");
        if (dump) {                                                  /* debugging aid, like the dump of the 60th frame */
            char path[512]; snprintf(path, sizeof path, "%s.notice", dump);
            FILE *fp = fopen(path, "wb");
            if (fp) { for (int yy = 0; yy < H; yy++) fwrite(b->map + (size_t)yy * b->pitch, 4, W, fp); fclose(fp); }
        }
        if (d->first) { drmModeSetCrtc(d->fd, d->crtc, b->fb, 0, 0, &d->conn, 1, &d->mode); d->first = 0; }
        else {
            wait_flip(d);
            d->inflight_recv = 0;
            if (!drmModePageFlip(d->fd, d->crtc, b->fb, DRM_MODE_PAGE_FLIP_EVENT, d)) d->flip_pending = 1;
        }
        d->back ^= 1;
    }
    pthread_mutex_unlock(&d->lock);
}

/* Reader thread: is a display connected right now? */
static int present(sink *s)
{
    drm *d = s->priv;
    pthread_mutex_lock(&d->lock);
    drmModeConnector *c = drmModeGetConnectorCurrent(d->fd, d->conn);
    int ok = c && c->connection == DRM_MODE_CONNECTED;
    drmModeFreeConnector(c);
    pthread_mutex_unlock(&d->lock);
    return ok;
}

static void cls(sink *s)
{
    drm *d = s->priv;
    if (!d->idle) wait_flip(d);
    if (d->saved && !d->idle && !d->first) {
        drmModeSetCrtc(d->fd, d->saved->crtc_id, d->saved->buffer_id, d->saved->x, d->saved->y, &d->conn, 1, &d->saved->mode);
    }
    if (d->saved) drmModeFreeCrtc(d->saved);
    for (int i = 0; i < 2; i++) {
        if (d->buf[i].map) munmap(d->buf[i].map, d->buf[i].size);
        if (d->buf[i].fb) drmModeRmFB(d->fd, d->buf[i].fb);
    }
    close(d->fd);
    free(d);
}

sink *sink_drm_new(const char *dev, int vw, int vh, int scale)
{
    static int tries;                                   /* called again until a display shows up: complain only once */
    int quiet = tries++ > 0;
    drm *d = calloc(1, sizeof *d);
    d->fd = open(dev ? dev : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (d->fd < 0) { if (!quiet) perror("drm open"); free(d); return NULL; }
    drmSetMaster(d->fd);
    d->vw = vw; d->vh = vh; d->scale = scale;
    if (pick_display(d, vw, vh, quiet)) { close(d->fd); free(d); return NULL; }
    fprintf(stderr, "drm: %s %dx%d@%d%s\n", dev ? dev : "/dev/dri/card0", d->mode.hdisplay, d->mode.vdisplay,
            d->mode.vrefresh, d->exact ? "" : scale ? " (no mode of the video's size, the picture is scaled to fit)"
                                                    : " (no mode of the video's size, the picture is centered)");
    if (make_buf(d, &d->buf[0]) || make_buf(d, &d->buf[1])) { fprintf(stderr, "drm: cannot create buffers\n"); close(d->fd); free(d); return NULL; }
    d->first = 1;
    pthread_mutex_init(&d->lock, NULL);
    sink *s = calloc(1, sizeof *s);
    s->priv = d; s->show = show; s->idle = idle; s->resume = resume; s->present = present; s->notice = notice; s->close = cls;
    return s;
}
