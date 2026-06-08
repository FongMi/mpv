/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "video/out/android_common.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "context.h"

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    EGLint egl_native_visual_id;
};

static void android_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (!vo_android_has_native_window(ctx->vo) ||
        p->egl_surface == EGL_NO_SURFACE)
        return;

    eglSwapBuffers(p->egl_display, p->egl_surface);
}

static void android_destroy_egl_surface(struct ra_ctx *ctx, EGLSurface surface)
{
    struct priv *p = ctx->priv;
    if (!surface || surface == EGL_NO_SURFACE)
        return;

    eglDestroySurface(p->egl_display, surface);
}

static void android_destroy_current_egl_surface(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (!p->egl_surface || p->egl_surface == EGL_NO_SURFACE)
        return;

    eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    android_destroy_egl_surface(ctx, p->egl_surface);
    p->egl_surface = EGL_NO_SURFACE;
}

static EGLSurface android_create_egl_surface(struct ra_ctx *ctx,
                                            ANativeWindow *native_window)
{
    struct priv *p = ctx->priv;
    if (!native_window) {
        MP_ERR(ctx, "Missing native window\n");
        return EGL_NO_SURFACE;
    }

    ANativeWindow_setBuffersGeometry(native_window, 0, 0,
                                     p->egl_native_visual_id);

    EGLSurface egl_surface = eglCreateWindowSurface(p->egl_display,
                                                    p->egl_config,
                                                    (EGLNativeWindowType)native_window,
                                                    NULL);
    if (egl_surface == EGL_NO_SURFACE)
        MP_ERR(ctx, "Could not create EGL surface!\n");

    return egl_surface;
}

static bool android_make_current(struct ra_ctx *ctx, EGLSurface surface)
{
    struct priv *p = ctx->priv;
    if (!eglMakeCurrent(p->egl_display, surface, surface, p->egl_context)) {
        MP_ERR(ctx, "Failed to set context!\n");
        return false;
    }

    return true;
}

static bool android_check_visible(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    return vo_android_has_native_window(ctx->vo) &&
           p->egl_surface != EGL_NO_SURFACE;
}

static void android_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_gl_ctx_uninit(ctx);

    android_destroy_current_egl_surface(ctx);
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);

    vo_android_uninit(ctx->vo);
}

static bool android_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);

    if (!vo_android_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "EGL failed to initialize.\n");
        goto fail;
    }

    EGLConfig config;
    if (!mpegl_create_context(ctx, p->egl_display, &p->egl_context, &config))
        goto fail;
    p->egl_config = config;

    eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_NATIVE_VISUAL_ID,
                       &p->egl_native_visual_id);
    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    p->egl_surface = android_create_egl_surface(ctx, native_window);
    if (p->egl_surface == EGL_NO_SURFACE)
        goto fail;
    if (!android_make_current(ctx, p->egl_surface))
        goto fail;

    mpegl_load_functions(&p->gl, ctx->log);

    struct ra_ctx_params params = {
        .check_visible = android_check_visible,
        .swap_buffers = android_swap_buffers,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto fail;

    return true;
fail:
    android_uninit(ctx);
    return false;
}

static bool android_reconfig(struct ra_ctx *ctx)
{
    if (!vo_android_has_native_window(ctx->vo))
        return true;

    int w, h;
    if (!vo_android_surface_size(ctx->vo, &w, &h))
        return false;

    // Update window geometry to prevent screen tearing
    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (native_window) {
        int32_t current_format = ANativeWindow_getFormat(native_window);
        ANativeWindow_setBuffersGeometry(native_window, w, h, current_format);
    }

    ctx->vo->dwidth = w;
    ctx->vo->dheight = h;
    ra_gl_ctx_resize(ctx->swapchain, w, h, 0);
    return true;
}

static bool android_detach_window(struct ra_ctx *ctx)
{
    android_destroy_current_egl_surface(ctx);
    vo_android_set_native_window(ctx->vo, NULL);
    return true;
}

static bool android_update_window(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (ctx->vo->opts->WinID == 0 || ctx->vo->opts->WinID == -1)
        return android_detach_window(ctx);

    ANativeWindow *native_window = vo_android_create_native_window(ctx->vo);
    if (!native_window)
        return false;

    EGLSurface old_surface = p->egl_surface;
    EGLSurface new_surface = android_create_egl_surface(ctx, native_window);
    if (new_surface == EGL_NO_SURFACE)
        goto fail;
    if (!android_make_current(ctx, new_surface)) {
        android_destroy_egl_surface(ctx, new_surface);
        goto fail;
    }

    p->egl_surface = new_surface;
    android_destroy_egl_surface(ctx, old_surface);
    vo_android_set_native_window(ctx->vo, native_window);
    if (!android_reconfig(ctx))
        return false;

    return true;

fail:
    ANativeWindow_release(native_window);
    return false;
}

static int android_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    switch (request) {
    case VOCTRL_UPDATE_WINDOW:
        if (!android_update_window(ctx))
            return VO_FALSE;
        *events |= VO_EVENT_RESIZE | VO_EVENT_EXPOSE;
        return VO_TRUE;
    }

    return VO_NOTIMPL;
}

const struct ra_ctx_fns ra_ctx_android = {
    .type           = "opengl",
    .name           = "android",
    .description    = "Android/EGL",
    .reconfig       = android_reconfig,
    .control        = android_control,
    .init           = android_init,
    .uninit         = android_uninit,
};
