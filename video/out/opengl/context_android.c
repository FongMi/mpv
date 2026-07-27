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
#include <android/data_space.h>
#include <dlfcn.h>
#include <limits.h>

#include "video/out/android_common.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "context.h"
#include "utils.h"

typedef int32_t (*set_buffers_dataspace_fn)(ANativeWindow *window,
                                           int32_t dataspace);

struct android_color {
    EGLint egl_colorspace;
    int32_t dataspace;
};

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    EGLint egl_native_visual_id;
    EGLint egl_colorspace;
    EGLint rejected_egl_colorspace;
    int egl_color_depth;
    set_buffers_dataspace_fn set_buffers_dataspace;
    int32_t dataspace;
    int32_t rejected_dataspace;
    bool supports_pq;
    bool supports_hlg;
    bool supports_p3;
    bool prefer_hdr_output;
    bool hdr_selection_valid;
    bool hdr_selection_supported;
    enum pl_color_transfer hdr_source_transfer;
    enum pl_color_transfer hdr_output_transfer;
    struct android_color hdr_output_color;
};

static bool android_set_egl_colorspace(struct ra_ctx *ctx,
                                       EGLint egl_colorspace);
static bool android_reconfig(struct ra_ctx *ctx);

static int egl_config_color_depth(EGLDisplay display, EGLConfig config)
{
    EGLint red = 0;
    EGLint green = 0;
    EGLint blue = 0;
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue);
    return MPMIN(red, MPMIN(green, blue));
}

static int android_refine_config(void *user_data, EGLConfig *configs,
                                 int num_configs)
{
    struct priv *p = user_data;
    if (!p->prefer_hdr_output)
        return 0;

    int best = -1;
    int best_depth = INT_MAX;
    int best_buffer_size = INT_MAX;
    int best_alpha_size = INT_MAX;
    for (int n = 0; n < num_configs; n++) {
        int depth = egl_config_color_depth(p->egl_display, configs[n]);
        if (depth < 10)
            continue;

        EGLint buffer_size = INT_MAX;
        EGLint alpha_size = INT_MAX;
        if (!eglGetConfigAttrib(p->egl_display, configs[n], EGL_BUFFER_SIZE,
                                &buffer_size) ||
            !eglGetConfigAttrib(p->egl_display, configs[n], EGL_ALPHA_SIZE,
                                &alpha_size))
            continue;
        // EGL does not define the order of matching configs. Prefer the
        // narrowest valid HDR buffer to avoid needless display bandwidth.
        if (buffer_size < best_buffer_size ||
            (buffer_size == best_buffer_size && depth < best_depth) ||
            (buffer_size == best_buffer_size && depth == best_depth &&
             alpha_size < best_alpha_size))
        {
            best = n;
            best_depth = depth;
            best_buffer_size = buffer_size;
            best_alpha_size = alpha_size;
        }
    }

    return best >= 0 ? best : 0;
}

static struct android_color android_hdr_color(
    enum pl_color_transfer transfer, bool use_egl)
{
    bool pq = transfer == PL_COLOR_TRC_PQ;
    return (struct android_color) {
        .egl_colorspace = use_egl
            ? (pq ? EGL_GL_COLORSPACE_BT2020_PQ_EXT
                  : EGL_GL_COLORSPACE_BT2020_HLG_EXT)
            : EGL_NONE,
        .dataspace = pq ? ADATASPACE_BT2020_PQ
                        : ADATASPACE_BT2020_HLG,
    };
}

static bool android_apply_color(struct ra_ctx *ctx,
                                ANativeWindow *native_window,
                                const struct android_color *color)
{
    struct priv *p = ctx->priv;
    if (!android_set_egl_colorspace(ctx, color->egl_colorspace))
        return false;

    if (color->egl_colorspace == EGL_NONE &&
        color->dataspace != p->dataspace)
    {
        if (color->dataspace == p->rejected_dataspace ||
            !p->set_buffers_dataspace)
            return false;

        int32_t ret =
            p->set_buffers_dataspace(native_window, color->dataspace);
        if (ret) {
            MP_VERBOSE(ctx, "Native window rejected data space %d: %d\n",
                       color->dataspace, ret);
            p->rejected_dataspace = color->dataspace;
            return false;
        }
    }

    p->dataspace = color->dataspace;
    return true;
}

static bool android_set_hdr_color(struct ra_ctx *ctx,
                                  ANativeWindow *native_window,
                                  struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    if (p->egl_color_depth < 10)
        return false;

    enum pl_color_transfer source_transfer = params->color.transfer;
    if (p->hdr_selection_valid &&
        p->hdr_source_transfer == source_transfer)
    {
        if (!p->hdr_selection_supported)
            return false;
        if (android_apply_color(ctx, native_window, &p->hdr_output_color)) {
            params->color.primaries = PL_COLOR_PRIM_BT_2020;
            params->color.transfer = p->hdr_output_transfer;
            return true;
        }
        p->hdr_selection_valid = false;
    }

    enum pl_color_transfer transfers[] = {
        source_transfer,
        source_transfer == PL_COLOR_TRC_PQ
            ? PL_COLOR_TRC_HLG
            : PL_COLOR_TRC_PQ,
    };
    // Prefer the source transfer. For each transfer, use the EGL extension
    // first and the native-window dataspace only as its exact fallback.
    for (size_t n = 0; n < MP_ARRAY_SIZE(transfers); n++) {
        enum pl_color_transfer transfer = transfers[n];
        bool supports_egl =
            transfer == PL_COLOR_TRC_PQ ? p->supports_pq : p->supports_hlg;
        struct android_color egl_color =
            android_hdr_color(transfer, true);
        if (supports_egl &&
            android_apply_color(ctx, native_window, &egl_color))
        {
            p->hdr_selection_valid = true;
            p->hdr_selection_supported = true;
            p->hdr_source_transfer = source_transfer;
            p->hdr_output_transfer = transfer;
            p->hdr_output_color = egl_color;
            params->color.primaries = PL_COLOR_PRIM_BT_2020;
            params->color.transfer = transfer;
            return true;
        }

        struct android_color native_color =
            android_hdr_color(transfer, false);
        if (p->set_buffers_dataspace &&
            android_apply_color(ctx, native_window, &native_color))
        {
            p->hdr_selection_valid = true;
            p->hdr_selection_supported = true;
            p->hdr_source_transfer = source_transfer;
            p->hdr_output_transfer = transfer;
            p->hdr_output_color = native_color;
            params->color.primaries = PL_COLOR_PRIM_BT_2020;
            params->color.transfer = transfer;
            return true;
        }
    }

    p->hdr_selection_valid = true;
    p->hdr_selection_supported = false;
    p->hdr_source_transfer = source_transfer;
    return false;
}

static bool android_get_color(struct priv *p, struct mp_image_params *params,
                              struct android_color *out)
{
    *out = (struct android_color) {
        .egl_colorspace = EGL_NONE,
        .dataspace = ADATASPACE_UNKNOWN,
    };
    if (!params)
        return true;

    struct pl_color_space *color = &params->color;
    switch (color->transfer) {
    case PL_COLOR_TRC_PQ:
    case PL_COLOR_TRC_HLG:
        return false;
    default:
        if (pl_color_transfer_is_hdr(color->transfer))
            return false;
        break;
    }

    switch (color->primaries) {
    case PL_COLOR_PRIM_DISPLAY_P3:
        if (!p->supports_p3)
            return false;
        color->transfer = PL_COLOR_TRC_SRGB;
        out->egl_colorspace = EGL_GL_COLORSPACE_DISPLAY_P3_EXT;
        out->dataspace = ADATASPACE_DISPLAY_P3;
        return true;
    case PL_COLOR_PRIM_BT_2020:
        color->transfer = PL_COLOR_TRC_BT_1886;
        out->dataspace = ADATASPACE_BT2020;
        return true;
    default:
        color->primaries = PL_COLOR_PRIM_BT_709;
        color->transfer = PL_COLOR_TRC_SRGB;
        out->dataspace = ADATASPACE_SRGB;
        return true;
    }
}

static bool android_set_color(struct ra_ctx *ctx,
                              struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (!native_window)
        return false;

    if (params &&
        (params->color.transfer == PL_COLOR_TRC_PQ ||
         params->color.transfer == PL_COLOR_TRC_HLG))
        return android_set_hdr_color(ctx, native_window, params);

    struct mp_image_params target = params ? *params :
        (struct mp_image_params){0};
    struct android_color color;
    if (!android_get_color(p, params ? &target : NULL, &color) ||
        !android_apply_color(ctx, native_window, &color))
        return false;

    if (params)
        *params = target;
    return true;
}

static void android_reset_color_state(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    p->egl_colorspace = EGL_NONE;
    p->rejected_egl_colorspace = -1;
    p->dataspace = ADATASPACE_UNKNOWN;
    p->rejected_dataspace = -1;
    p->hdr_selection_valid = false;
    ra_swapchain_invalidate_color(ctx->swapchain);
}

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
                                             ANativeWindow *native_window,
                                             EGLint egl_colorspace)
{
    struct priv *p = ctx->priv;
    if (!native_window) {
        MP_ERR(ctx, "Missing native window\n");
        return EGL_NO_SURFACE;
    }

    ANativeWindow_setBuffersGeometry(native_window, 0, 0,
                                     p->egl_native_visual_id);

    const EGLint attrs[] = {
        EGL_GL_COLORSPACE_KHR, egl_colorspace,
        EGL_NONE,
    };
    EGLSurface egl_surface = eglCreateWindowSurface(p->egl_display,
                                                    p->egl_config,
                                                    (EGLNativeWindowType)native_window,
                                                    egl_colorspace == EGL_NONE
                                                        ? NULL : attrs);
    if (egl_surface == EGL_NO_SURFACE) {
        MP_VERBOSE(ctx, "Could not create EGL surface with color space %d: %d\n",
                   egl_colorspace, eglGetError());
        return EGL_NO_SURFACE;
    }

    if (egl_colorspace != EGL_NONE) {
        EGLint actual = EGL_NONE;
        if (!eglQuerySurface(p->egl_display, egl_surface,
                             EGL_GL_COLORSPACE_KHR, &actual) ||
            actual != egl_colorspace)
        {
            MP_VERBOSE(ctx, "EGL returned color space %d instead of %d\n",
                       actual, egl_colorspace);
            android_destroy_egl_surface(ctx, egl_surface);
            return EGL_NO_SURFACE;
        }
    }

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

static bool android_create_current_egl_surface(struct ra_ctx *ctx,
                                               ANativeWindow *native_window,
                                               EGLint egl_colorspace)
{
    struct priv *p = ctx->priv;
    EGLSurface surface =
        android_create_egl_surface(ctx, native_window, egl_colorspace);
    if (surface == EGL_NO_SURFACE)
        return false;
    if (!android_make_current(ctx, surface)) {
        android_destroy_egl_surface(ctx, surface);
        return false;
    }

    p->egl_surface = surface;
    if (ctx->swapchain && !android_reconfig(ctx)) {
        android_destroy_current_egl_surface(ctx);
        return false;
    }
    return true;
}

static bool android_set_egl_colorspace(struct ra_ctx *ctx,
                                       EGLint egl_colorspace)
{
    struct priv *p = ctx->priv;
    if (egl_colorspace == p->egl_colorspace &&
        p->egl_surface != EGL_NO_SURFACE)
        return true;
    if (egl_colorspace == p->rejected_egl_colorspace)
        return false;

    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (!native_window)
        return false;

    EGLint previous = p->egl_colorspace;
    android_destroy_current_egl_surface(ctx);
    if (!android_create_current_egl_surface(ctx, native_window,
                                            egl_colorspace))
    {
        p->rejected_egl_colorspace = egl_colorspace;
        if (!android_create_current_egl_surface(ctx, native_window, previous))
            MP_ERR(ctx, "Could not restore the previous EGL surface\n");
        return false;
    }

    p->egl_colorspace = egl_colorspace;
    p->rejected_egl_colorspace = -1;
    p->dataspace = ADATASPACE_UNKNOWN;
    p->rejected_dataspace = -1;
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
    p->prefer_hdr_output = ctx->vo->extra.prefer_hdr_output;
    p->set_buffers_dataspace = (set_buffers_dataspace_fn)
        dlsym(RTLD_DEFAULT, "ANativeWindow_setBuffersDataSpace");
    android_reset_color_state(ctx);

    if (!vo_android_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "EGL failed to initialize.\n");
        goto fail;
    }

    const char *extensions = eglQueryString(p->egl_display, EGL_EXTENSIONS);
    p->supports_pq =
        gl_check_extension(extensions, "EGL_EXT_gl_colorspace_bt2020_pq");
    p->supports_hlg =
        gl_check_extension(extensions, "EGL_EXT_gl_colorspace_bt2020_hlg");
    p->supports_p3 =
        gl_check_extension(extensions, "EGL_EXT_gl_colorspace_display_p3");

    EGLConfig config;
    struct mpegl_cb cb = {
        .refine_config = android_refine_config,
        .user_data = p,
    };
    if (!mpegl_create_context_cb(ctx, p->egl_display, cb,
                                 &p->egl_context, &config))
        goto fail;
    p->egl_config = config;
    p->egl_color_depth =
        egl_config_color_depth(p->egl_display, p->egl_config);
    EGLint buffer_size = 0;
    EGLint alpha_size = 0;
    eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_BUFFER_SIZE,
                       &buffer_size);
    eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_ALPHA_SIZE,
                       &alpha_size);
    MP_VERBOSE(ctx, "Android EGL output color depth: %d bits "
                    "(buffer %d, alpha %d)\n",
               p->egl_color_depth, buffer_size, alpha_size);

    eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_NATIVE_VISUAL_ID,
                       &p->egl_native_visual_id);
    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (!android_create_current_egl_surface(ctx, native_window, EGL_NONE))
        goto fail;

    mpegl_load_functions(&p->gl, ctx->log);

    struct ra_ctx_params params = {
        .check_visible = android_check_visible,
        .set_color = android_set_color,
        .swap_buffers = android_swap_buffers,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto fail;

    ctx->swapchain->color_hint_cache_enabled = true;
    ctx->supports_auto_colorspace_hint =
        p->supports_pq || p->supports_hlg || p->supports_p3 ||
        p->set_buffers_dataspace;
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
    android_reset_color_state(ctx);
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
    EGLSurface new_surface =
        android_create_egl_surface(ctx, native_window, EGL_NONE);
    if (new_surface == EGL_NO_SURFACE)
        goto fail;
    if (!android_make_current(ctx, new_surface)) {
        android_destroy_egl_surface(ctx, new_surface);
        goto fail;
    }

    p->egl_surface = new_surface;
    android_destroy_egl_surface(ctx, old_surface);
    vo_android_set_native_window(ctx->vo, native_window);
    android_reset_color_state(ctx);
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
