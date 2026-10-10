/* Local tebox host cast bridge; not for upstream QEMU.
 *
 * UI thread: GPU blit to small FBO + async PBO readback, hand off to sender.
 * Never block display on socket I/O. Target up to 60 FPS while casting.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "ui/sdl2.h"
#include "tebox-cast-hook.h"

#ifdef CONFIG_OPENGL

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <epoxy/gl.h>
#include <glib.h>

#define TEBOX_CAST_MAGIC   0x54434652u
#define TEBOX_CAST_VERSION 1u
#define TEBOX_CAST_BGRA    0u

typedef struct TeboxCastFrame {
    uint8_t *pixels;
    size_t nbytes;
    int width;
    int height;
    uint64_t pts_us;
} TeboxCastFrame;

typedef struct TeboxCastState {
    int fd;
    char path[512];
    uint8_t *readback;
    int out_w;
    int out_h;
    GLuint fbo;
    GLuint tex;
    GLuint pbo[2];
    int pbo_idx;
    bool pbo_pending; /* previous async read not yet harvested */
    int pending_w;
    int pending_h;
    uint64_t pending_pts;
    uint64_t last_us;
    bool logged;

    GMutex lock;
    GCond cond;
    GThread *sender;
    bool stop;
    bool busy;
    TeboxCastFrame pending;
    bool has_pending;
    uint64_t dropped;
    uint64_t sent;
} TeboxCastState;

static TeboxCastState tebox_cast = {
    .fd = -1,
};

bool tebox_cast_enabled(void)
{
    const char *v = getenv("TEBOX_CAST");

    return v && v[0] == '1' && v[1] == '\0';
}

uint64_t tebox_cast_refresh_interval_ms(void)
{
    const char *fps_env;
    int fps;

    if (!tebox_cast_enabled()) {
        return 0;
    }
    fps_env = getenv("TEBOX_CAST_FPS");
    fps = fps_env && fps_env[0] ? atoi(fps_env) : 60;
    if (fps < 1) {
        fps = 1;
    }
    if (fps > 60) {
        fps = 60;
    }
    /* ms, min 10 to match SDL2_REFRESH_INTERVAL_BUSY */
    return MAX(10ull, 1000ull / (uint64_t)fps);
}

static uint64_t tebox_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void tebox_w32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void tebox_w64be(uint8_t *p, uint64_t v)
{
    tebox_w32be(p, (uint32_t)(v >> 32));
    tebox_w32be(p + 4, (uint32_t)v);
}

static void tebox_cast_close_locked(void)
{
    if (tebox_cast.fd >= 0) {
        close(tebox_cast.fd);
        tebox_cast.fd = -1;
    }
}

static bool tebox_cast_connect_locked(void)
{
    struct sockaddr_un addr;
    const char *path = getenv("TEBOX_CAST_SOCK");
    int fd;
    /* ~2 frames at 480x1080 BGRA; larger invites multi-second socket backlog. */
    int sndbuf = 4 * 1024 * 1024;

    if (!path || !path[0]) {
        path = "/tmp/tebox-cast.sock";
    }
    if (tebox_cast.fd >= 0 && !strcmp(tebox_cast.path, path)) {
        return true;
    }
    tebox_cast_close_locked();
    snprintf(tebox_cast.path, sizeof(tebox_cast.path), "%s", path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    /* Blocking send of at most one queued frame; UI drops if sender is busy. */
    tebox_cast.fd = fd;
    if (!tebox_cast.logged) {
        error_report("tebox-cast: connected (low-latency sync readback)");
        tebox_cast.logged = true;
    }
    return true;
}

static bool tebox_write_all_fd(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, buf + off, len - off, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static void tebox_flip_vertical(uint8_t *pixels, int width, int height)
{
    size_t stride = (size_t)width * 4;
    uint8_t *tmp = g_malloc(stride);
    int y;

    for (y = 0; y < height / 2; y++) {
        uint8_t *a = pixels + (size_t)y * stride;
        uint8_t *b = pixels + (size_t)(height - 1 - y) * stride;

        memcpy(tmp, a, stride);
        memcpy(a, b, stride);
        memcpy(b, tmp, stride);
    }
    g_free(tmp);
}

static void tebox_calc_out_size(int src_w, int src_h, int *out_w, int *out_h)
{
    const char *mw = getenv("TEBOX_CAST_MAX_W");
    const char *mh = getenv("TEBOX_CAST_MAX_H");
    int max_w = mw && mw[0] ? atoi(mw) : 360;
    int max_h = mh && mh[0] ? atoi(mh) : 800;
    int w = src_w;
    int h = src_h;

    if (max_w > 0 && w > max_w) {
        h = (int)((int64_t)h * max_w / w);
        w = max_w;
    }
    if (max_h > 0 && h > max_h) {
        w = (int)((int64_t)w * max_h / h);
        h = max_h;
    }
    w &= ~1;
    h &= ~1;
    if (w < 2) {
        w = 2;
    }
    if (h < 2) {
        h = 2;
    }
    *out_w = w;
    *out_h = h;
}

static bool tebox_ensure_scale_fbo(int w, int h)
{
    size_t nbytes = (size_t)w * (size_t)h * 4;

    if (tebox_cast.fbo && tebox_cast.out_w == w && tebox_cast.out_h == h) {
        return true;
    }
    if (tebox_cast.fbo) {
        glDeleteFramebuffers(1, &tebox_cast.fbo);
        tebox_cast.fbo = 0;
    }
    if (tebox_cast.tex) {
        glDeleteTextures(1, &tebox_cast.tex);
        tebox_cast.tex = 0;
    }
    if (tebox_cast.pbo[0]) {
        glDeleteBuffers(2, tebox_cast.pbo);
        tebox_cast.pbo[0] = tebox_cast.pbo[1] = 0;
    }
    tebox_cast.pbo_pending = false;

    glGenTextures(1, &tebox_cast.tex);
    glBindTexture(GL_TEXTURE_2D, tebox_cast.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &tebox_cast.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, tebox_cast.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tebox_cast.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_report("tebox-cast: scale FBO incomplete");
        return false;
    }

    glGenBuffers(2, tebox_cast.pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, tebox_cast.pbo[0]);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)nbytes, NULL, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, tebox_cast.pbo[1]);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)nbytes, NULL, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    tebox_cast.out_w = w;
    tebox_cast.out_h = h;
    g_free(tebox_cast.readback);
    tebox_cast.readback = g_try_malloc(nbytes);
    return tebox_cast.readback != NULL;
}

static void tebox_queue_frame(uint8_t *pixels, size_t nbytes,
                              int w, int h, uint64_t pts)
{
    g_mutex_lock(&tebox_cast.lock);
    if (tebox_cast.busy || tebox_cast.has_pending) {
        /* Keep newest frame only. */
        if (tebox_cast.has_pending) {
            g_free(tebox_cast.pending.pixels);
        } else {
            tebox_cast.dropped++;
            g_mutex_unlock(&tebox_cast.lock);
            g_free(pixels);
            return;
        }
        tebox_cast.dropped++;
    }
    tebox_cast.pending.pixels = pixels;
    tebox_cast.pending.nbytes = nbytes;
    tebox_cast.pending.width = w;
    tebox_cast.pending.height = h;
    tebox_cast.pending.pts_us = pts;
    tebox_cast.has_pending = true;
    g_cond_signal(&tebox_cast.cond);
    g_mutex_unlock(&tebox_cast.lock);
}

static gpointer tebox_sender_thread(gpointer opaque)
{
    (void)opaque;

    g_mutex_lock(&tebox_cast.lock);
    while (!tebox_cast.stop) {
        TeboxCastFrame fr;
        uint8_t *pkt;
        size_t pkt_len;
        int fd;
        int wr;

        while (!tebox_cast.has_pending && !tebox_cast.stop) {
            g_cond_wait(&tebox_cast.cond, &tebox_cast.lock);
        }
        if (tebox_cast.stop) {
            break;
        }

        fr = tebox_cast.pending;
        tebox_cast.pending.pixels = NULL;
        tebox_cast.has_pending = false;
        tebox_cast.busy = true;

        if (!tebox_cast_connect_locked()) {
            g_free(fr.pixels);
            tebox_cast.busy = false;
            continue;
        }
        fd = tebox_cast.fd;
        g_mutex_unlock(&tebox_cast.lock);

        pkt_len = 36 + fr.nbytes;
        pkt = g_try_malloc(pkt_len);
        if (!pkt) {
            g_free(fr.pixels);
            g_mutex_lock(&tebox_cast.lock);
            tebox_cast.busy = false;
            continue;
        }
        tebox_w32be(pkt + 0, TEBOX_CAST_MAGIC);
        tebox_w32be(pkt + 4, TEBOX_CAST_VERSION);
        tebox_w32be(pkt + 8, (uint32_t)fr.width);
        tebox_w32be(pkt + 12, (uint32_t)fr.height);
        tebox_w32be(pkt + 16, (uint32_t)fr.width * 4);
        tebox_w32be(pkt + 20, TEBOX_CAST_BGRA);
        tebox_w64be(pkt + 24, fr.pts_us);
        tebox_w32be(pkt + 32, (uint32_t)fr.nbytes);
        memcpy(pkt + 36, fr.pixels, fr.nbytes);
        g_free(fr.pixels);

        wr = tebox_write_all_fd(fd, pkt, pkt_len) ? 1 : -1;
        g_free(pkt);

        g_mutex_lock(&tebox_cast.lock);
        if (wr < 0) {
            tebox_cast_close_locked();
        } else {
            tebox_cast.sent++;
        }
        tebox_cast.busy = false;
    }

    if (tebox_cast.has_pending) {
        g_free(tebox_cast.pending.pixels);
        tebox_cast.pending.pixels = NULL;
        tebox_cast.has_pending = false;
    }
    tebox_cast_close_locked();
    g_mutex_unlock(&tebox_cast.lock);
    return NULL;
}

static void tebox_cast_ensure_sender(void)
{
    static gsize once;

    if (g_once_init_enter(&once)) {
        g_mutex_init(&tebox_cast.lock);
        g_cond_init(&tebox_cast.cond);
        tebox_cast.sender = g_thread_new("tebox-cast", tebox_sender_thread,
                                         NULL);
        g_once_init_leave(&once, 1);
    }
}

void tebox_cast_on_scanout(struct sdl2_console *scon)
{
    int src_w, src_h, out_w, out_h;
    size_t nbytes;
    GLint draw_fb, read_fb, read_buffer, pack_buffer, alignment;
    GLint row_length, skip_rows, skip_pixels;
    uint64_t now;
    int fps;
    const char *fps_env;
    const char *flip_env;
    uint8_t *copy;
    bool cpu_flip;
    bool sender_busy;

    if (!tebox_cast_enabled()) {
        return;
    }
    if (!scon || !scon->scanout_mode || !scon->guest_fb.framebuffer) {
        return;
    }

    src_w = scon->guest_fb.width;
    src_h = scon->guest_fb.height;
    if (src_w <= 0 || src_h <= 0) {
        return;
    }

    fps_env = getenv("TEBOX_CAST_FPS");
    fps = fps_env && fps_env[0] ? atoi(fps_env) : 60;
    if (fps < 1) {
        fps = 1;
    }
    if (fps > 60) {
        fps = 60;
    }
    now = tebox_now_us();
    if (tebox_cast.last_us &&
        now - tebox_cast.last_us < 1000000ull / (uint64_t)fps) {
        return;
    }

    tebox_cast_ensure_sender();

    /* If previous frame still in flight, skip — never build a backlog. */
    g_mutex_lock(&tebox_cast.lock);
    sender_busy = tebox_cast.busy || tebox_cast.has_pending;
    if (sender_busy) {
        tebox_cast.dropped++;
    }
    g_mutex_unlock(&tebox_cast.lock);
    if (sender_busy) {
        return;
    }

    tebox_calc_out_size(src_w, src_h, &out_w, &out_h);
    if (!tebox_ensure_scale_fbo(out_w, out_h)) {
        return;
    }
    nbytes = (size_t)out_w * (size_t)out_h * 4;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);

    /* Match SDL window: flip blit when guest y0 is not top. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scon->guest_fb.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tebox_cast.fbo);
    if (!scon->y0_top) {
        glBlitFramebuffer(0, 0, src_w, src_h,
                          0, out_h, out_w, 0,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        glBlitFramebuffer(0, 0, src_w, src_h,
                          0, 0, out_w, out_h,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    /* Sync read of the *small* FBO only — lower latency than PBO pipeline. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, tebox_cast.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, out_w, out_h, GL_BGRA, GL_UNSIGNED_BYTE,
                 tebox_cast.readback);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
    glReadBuffer(read_buffer);

    /*
     * After y-flip blit, glReadPixels is still bottom-up in memory.
     * Default CPU flip for upright video. TEBOX_CAST_FLIP=0 to disable.
     */
    flip_env = getenv("TEBOX_CAST_FLIP");
    cpu_flip = !(flip_env && flip_env[0] == '0');
    if (cpu_flip) {
        tebox_flip_vertical(tebox_cast.readback, out_w, out_h);
    }

    copy = g_try_malloc(nbytes);
    if (!copy) {
        return;
    }
    memcpy(copy, tebox_cast.readback, nbytes);
    tebox_queue_frame(copy, nbytes, out_w, out_h, now);
    tebox_cast.last_us = now;
}

void tebox_cast_shutdown(struct sdl2_console *scon)
{
    (void)scon;
    if (tebox_cast.sender) {
        g_mutex_lock(&tebox_cast.lock);
        tebox_cast.stop = true;
        g_cond_signal(&tebox_cast.cond);
        g_mutex_unlock(&tebox_cast.lock);
        g_thread_join(tebox_cast.sender);
        tebox_cast.sender = NULL;
    }
    tebox_cast_close_locked();
    if (tebox_cast.pbo[0]) {
        glDeleteBuffers(2, tebox_cast.pbo);
        tebox_cast.pbo[0] = tebox_cast.pbo[1] = 0;
    }
    if (tebox_cast.fbo) {
        glDeleteFramebuffers(1, &tebox_cast.fbo);
        tebox_cast.fbo = 0;
    }
    if (tebox_cast.tex) {
        glDeleteTextures(1, &tebox_cast.tex);
        tebox_cast.tex = 0;
    }
    g_free(tebox_cast.readback);
    tebox_cast.readback = NULL;
    tebox_cast.out_w = 0;
    tebox_cast.out_h = 0;
    tebox_cast.logged = false;
    tebox_cast.pbo_pending = false;
}

#else /* !CONFIG_OPENGL */

bool tebox_cast_enabled(void)
{
    return false;
}

uint64_t tebox_cast_refresh_interval_ms(void)
{
    return 0;
}

void tebox_cast_on_scanout(struct sdl2_console *scon)
{
    (void)scon;
}

void tebox_cast_shutdown(struct sdl2_console *scon)
{
    (void)scon;
}

#endif
