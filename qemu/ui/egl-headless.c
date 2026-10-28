#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#include "ui/shader.h"
#include "tebox-cast-hook.h"

typedef struct egl_dpy {
    DisplayChangeListener dcl;
    DisplayGLCtx *ctx;
    DisplaySurface *ds;
    QemuGLShader *gls;
    egl_fb guest_fb;
    egl_fb cursor_fb;
    egl_fb blit_fb;
    DisplaySurface *dump_ds;
    bool y_0_top;
    uint32_t pos_x;
    uint32_t pos_y;
} egl_dpy;

static GPtrArray *egl_dpys;

/* ------------------------------------------------------------------ */

static void egl_refresh(DisplayChangeListener *dcl)
{
    qemu_console_hw_update(dcl->con);
}

static void egl_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    /*
     * GL scanout is cast from the guest framebuffer in egl_scanout_flush.
     * This surface is only the CPU shadow; sending it here replaces a good
     * cast frame (screenshot readback leaves it stale or black).
     */
    if (edpy->guest_fb.texture) {
        return;
    }
    /* 2D scanouts (virtio-gpu-rutabaga) have no GL fb. */
    if (edpy->ds && surface_format(edpy->ds) == PIXMAN_x8r8g8b8) {
        tebox_cast_capture_pixels(surface_data(edpy->ds),
                                  surface_width(edpy->ds),
                                  surface_height(edpy->ds),
                                  surface_stride(edpy->ds));
    }
}

static void egl_gfx_switch(DisplayChangeListener *dcl,
                           struct DisplaySurface *new_surface)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->ds = new_surface;
}

static QEMUGLContext egl_create_context(DisplayGLCtx *dgc,
                                        QEMUGLParams *params)
{
    return qemu_egl_create_context(dgc, params, qemu_egl_rn_ctx);
}

static void egl_scanout_disable(DisplayChangeListener *dcl)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    egl_fb_destroy(&edpy->guest_fb);
    egl_fb_destroy(&edpy->blit_fb);
}

static void egl_scanout_texture(DisplayChangeListener *dcl,
                                uint32_t backing_id,
                                bool backing_y_0_top,
                                uint32_t backing_width,
                                uint32_t backing_height,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h,
                                void *d3d_tex2d)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->y_0_top = backing_y_0_top;

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);

    /* source framebuffer */
    egl_fb_setup_for_tex(&edpy->guest_fb,
                         backing_width, backing_height, backing_id, false);

    /* dest framebuffer */
    if (edpy->blit_fb.width  != backing_width ||
        edpy->blit_fb.height != backing_height) {
        egl_fb_destroy(&edpy->blit_fb);
        egl_fb_setup_new_tex(&edpy->blit_fb, backing_width, backing_height);
    }
}

#ifdef CONFIG_GBM

static void egl_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    uint32_t width, height, texture;

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        return;
    }

    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);

    egl_scanout_texture(dcl, texture, false, width, height, 0, 0,
                        width, height, NULL);
}

static void egl_cursor_dmabuf(DisplayChangeListener *dcl,
                              QemuDmaBuf *dmabuf, bool have_hot,
                              uint32_t hot_x, uint32_t hot_y)
{
    uint32_t width, height, texture;
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    if (dmabuf) {
        egl_dmabuf_import_texture(dmabuf);
        texture = qemu_dmabuf_get_texture(dmabuf);
        if (!texture) {
            return;
        }

        width = qemu_dmabuf_get_width(dmabuf);
        height = qemu_dmabuf_get_height(dmabuf);
        egl_fb_setup_for_tex(&edpy->cursor_fb, width, height, texture, false);
    } else {
        egl_fb_destroy(&edpy->cursor_fb);
    }
}

static void egl_release_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    egl_dmabuf_release_texture(dmabuf);
}

#endif

/*
 * PIXMAN_x8r8g8b8 is B,G,R,x in memory. Mali GLES cannot read the
 * imported guest texture or a GL_BGRA intermediate with GL_BGRA, so
 * blit into a fresh GL_RGBA target (same path as tebox cast) and swizzle.
 */
static bool egl_read_guest_rgba(DisplaySurface *dst, egl_fb *src)
{
    int w = surface_width(dst);
    int h = surface_height(dst);
    uint8_t *data = surface_data(dst);
    GLint draw_fb = 0, read_fb = 0, tex_binding = 0, read_buffer = 0;
    GLuint tex = 0, fbo = 0;
    GLenum err;
    int i, n = w * h;

    /*
     * Cast samples this same guest framebuffer on the next flush. Leave
     * its bindings as we found them or that frame is black.
     */
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex_binding);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    while (glGetError() != GL_NO_ERROR) {
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glBlitFramebuffer(0, 0, src->width, src->height,
                      0, 0, w, h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data);
    err = glGetError();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
    glReadBuffer(read_buffer);
    glBindTexture(GL_TEXTURE_2D, tex_binding);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    while (glGetError() != GL_NO_ERROR) {
    }
    if (err != GL_NO_ERROR) {
        return false;
    }
    for (i = 0; i < n; i++) {
        uint8_t r = data[i * 4];
        data[i * 4] = data[i * 4 + 2];
        data[i * 4 + 2] = r;
    }
    return true;
}

/* screendump: GL/dma-buf scanout never installs a console surface. */
static pixman_image_t *egl_gl_readback(DisplayChangeListener *dcl)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    int w, h;

    if (!edpy->guest_fb.texture || !edpy->guest_fb.width ||
        !edpy->guest_fb.height) {
        return NULL;
    }
    w = edpy->guest_fb.width;
    h = edpy->guest_fb.height;
    if (!edpy->dump_ds || surface_width(edpy->dump_ds) != w ||
        surface_height(edpy->dump_ds) != h) {
        qemu_free_displaysurface(edpy->dump_ds);
        edpy->dump_ds = qemu_create_displaysurface(w, h);
    }
    if (!edpy->dump_ds) {
        return NULL;
    }

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    if (!egl_read_guest_rgba(edpy->dump_ds, &edpy->guest_fb)) {
        return NULL;
    }
    return pixman_image_ref(edpy->dump_ds->image);
}

static void egl_cursor_position(DisplayChangeListener *dcl,
                                uint32_t pos_x, uint32_t pos_y)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->pos_x = pos_x;
    edpy->pos_y = pos_y;
}

static void egl_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    bool readback = !g_strcmp0(g_getenv("TEBOX_EGL_READBACK"), "1");

    if (!edpy->guest_fb.texture) {
        return;
    }
    /* Unset used to mean on. That glReadPixels ran on every virgl frame. */
    if (readback && !edpy->ds) {
        readback = false;
    }

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    if (edpy->cursor_fb.texture) {
        /* have cursor -> render using textures */
        egl_texture_blit(edpy->gls, &edpy->blit_fb, &edpy->guest_fb,
                         !edpy->y_0_top);
        egl_texture_blend(edpy->gls, &edpy->blit_fb, &edpy->cursor_fb,
                          !edpy->y_0_top, edpy->pos_x, edpy->pos_y,
                          1.0, 1.0);
    } else if (readback) {
        assert(surface_format(edpy->ds) == PIXMAN_x8r8g8b8);
        egl_fb_blit(&edpy->blit_fb, &edpy->guest_fb, edpy->y_0_top);
        egl_fb_read(edpy->ds, &edpy->blit_fb);
    }
    /* Local tebox cast: headless hosts (CIX R1P) have no SDL window. */
    tebox_cast_capture(edpy->guest_fb.framebuffer, edpy->guest_fb.width,
                       edpy->guest_fb.height, edpy->y_0_top);
    if (readback) {
        qemu_console_update(edpy->dcl.con, x, y, w, h);
    }
}

static const DisplayChangeListenerOps egl_ops = {
    .dpy_name                = "egl-headless",
    .dpy_refresh             = egl_refresh,
    .dpy_gfx_update          = egl_gfx_update,
    .dpy_gfx_switch          = egl_gfx_switch,

    .dpy_gl_scanout_disable  = egl_scanout_disable,
    .dpy_gl_scanout_texture  = egl_scanout_texture,
#ifdef CONFIG_GBM
    .dpy_gl_scanout_dmabuf   = egl_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = egl_cursor_dmabuf,
    .dpy_gl_release_dmabuf   = egl_release_dmabuf,
#endif
    .dpy_gl_cursor_position  = egl_cursor_position,
    .dpy_gl_update           = egl_scanout_flush,
    .dpy_gl_readback         = egl_gl_readback,
};

static bool
egl_is_compatible_dcl(DisplayGLCtx *dgc,
                      DisplayChangeListener *dcl)
{
    if (!dcl->ops->dpy_gl_update) {
        /*
         * egl-headless is compatible with all 2d listeners, as it blits the GL
         * updates on the 2d console surface.
         */
        return true;
    }

    return dcl->ops == &egl_ops;
}

static const DisplayGLCtxOps eglctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = egl_is_compatible_dcl,
    .dpy_gl_ctx_create       = egl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
};

static void early_egl_headless_init(DisplayOptions *opts)
{
    DisplayGLMode mode = DISPLAY_GL_MODE_ON;

    if (opts->has_gl) {
        mode = opts->gl;
    }

    egl_init(opts->u.egl_headless.rendernode, mode, &error_fatal);
}

static void egl_headless_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    egl_dpy *edpy;
    int idx;

    egl_dpys = g_ptr_array_new();

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    for (idx = 0;; idx++) {
        DisplayGLCtx *ctx;

        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        edpy = g_new0(egl_dpy, 1);
        edpy->gls = qemu_gl_init_shader();
        ctx = g_new0(DisplayGLCtx, 1);
        ctx->ops = &eglctx_ops;
        edpy->ctx = ctx;
        qemu_console_set_display_gl_ctx(con, ctx);
        qemu_console_register_listener(con, &edpy->dcl, &egl_ops);
        g_ptr_array_add(egl_dpys, edpy);
    }
}

static void egl_headless_cleanup(void)
{
    if (!egl_dpys) {
        return;
    }

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, qemu_egl_rn_ctx);
    for (guint i = 0; i < egl_dpys->len; i++) {
        egl_dpy *edpy = g_ptr_array_index(egl_dpys, i);

        qemu_console_unregister_listener(&edpy->dcl);
        qemu_console_set_display_gl_ctx(edpy->dcl.con, NULL);
        egl_fb_destroy(&edpy->guest_fb);
        egl_fb_destroy(&edpy->cursor_fb);
        egl_fb_destroy(&edpy->blit_fb);
        qemu_gl_fini_shader(edpy->gls);
        g_free(edpy->ctx);
        g_free(edpy);
    }
    g_clear_pointer(&egl_dpys, g_ptr_array_unref);

    egl_cleanup();
}

static QemuDisplay qemu_display_egl = {
    .type       = DISPLAY_TYPE_EGL_HEADLESS,
    .early_init = early_egl_headless_init,
    .init       = egl_headless_init,
    .cleanup    = egl_headless_cleanup,
};

static void register_egl(void)
{
    qemu_display_register(&qemu_display_egl);
}

type_init(register_egl);

module_dep("ui-opengl");
