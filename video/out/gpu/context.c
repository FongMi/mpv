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

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "options/options.h"
#include "options/m_option.h"
#include "video/mp_image.h"
#include "video/out/vo.h"

#include "context.h"
#include "spirv.h"

#if HAVE_VULKAN
#include "video/out/vulkan/context.h"
#endif

/* OpenGL */
extern const struct ra_ctx_fns ra_ctx_glx;
extern const struct ra_ctx_fns ra_ctx_x11_egl;
extern const struct ra_ctx_fns ra_ctx_drm_egl;
extern const struct ra_ctx_fns ra_ctx_wayland_egl;
extern const struct ra_ctx_fns ra_ctx_wgl;
extern const struct ra_ctx_fns ra_ctx_angle;
extern const struct ra_ctx_fns ra_ctx_dxgl;
extern const struct ra_ctx_fns ra_ctx_android;

/* Vulkan */
extern const struct ra_ctx_fns ra_ctx_vulkan_wayland;
extern const struct ra_ctx_fns ra_ctx_vulkan_win;
extern const struct ra_ctx_fns ra_ctx_vulkan_xlib;
extern const struct ra_ctx_fns ra_ctx_vulkan_android;
extern const struct ra_ctx_fns ra_ctx_vulkan_display;
extern const struct ra_ctx_fns ra_ctx_vulkan_mac;

/* Direct3D 11 */
extern const struct ra_ctx_fns ra_ctx_d3d11;

/* No API */
extern const struct ra_ctx_fns ra_ctx_wldmabuf;

/* Autoprobe dummy. Always fails to create. */
static bool dummy_init(struct ra_ctx *ctx)
{
    return false;
}

static void dummy_uninit(struct ra_ctx *ctx)
{
}

static const struct ra_ctx_fns ra_ctx_dummy = {
    .type           = "auto",
    .name           = "auto",
    .description    = "Auto detect",
    .init           = dummy_init,
    .uninit         = dummy_uninit,
};

static const struct ra_ctx_fns *const contexts[] = {
    &ra_ctx_dummy,
// Direct3D contexts:
#if HAVE_D3D11
    &ra_ctx_d3d11,
#endif

// Vulkan contexts:
#if HAVE_VULKAN
#if HAVE_WIN32_DESKTOP
    &ra_ctx_vulkan_win,
#endif
#if HAVE_WAYLAND
    &ra_ctx_vulkan_wayland,
#endif
#if HAVE_X11
    &ra_ctx_vulkan_xlib,
#endif
#if HAVE_COCOA && HAVE_SWIFT
    &ra_ctx_vulkan_mac,
#endif
#endif

// OpenGL contexts:
#if HAVE_EGL_ANDROID
    &ra_ctx_android,
#endif
#if HAVE_EGL_ANGLE_WIN32
    &ra_ctx_angle,
#endif
#if HAVE_GL_WIN32
    &ra_ctx_wgl,
#endif
#if HAVE_GL_DXINTEROP
    &ra_ctx_dxgl,
#endif
#if HAVE_EGL_WAYLAND
    &ra_ctx_wayland_egl,
#endif
#if HAVE_EGL_X11
    &ra_ctx_x11_egl,
#endif
#if HAVE_GL_X11
    &ra_ctx_glx,
#endif
#if HAVE_EGL_DRM
    &ra_ctx_drm_egl,
#endif

// Vulkan contexts (fallbacks):
#if HAVE_VULKAN
#if HAVE_ANDROID
    &ra_ctx_vulkan_android,
#endif
#if HAVE_VK_KHR_DISPLAY
    &ra_ctx_vulkan_display,
#endif
#endif
};

static const struct ra_ctx_fns *const no_api_contexts[] = {
    &ra_ctx_dummy,
/* No API contexts: */
#if HAVE_DMABUF_WAYLAND
    &ra_ctx_wldmabuf,
#endif
};

static bool get_desc(struct m_obj_desc *dst, int index)
{
    if (index >= MP_ARRAY_SIZE(contexts))
        return false;
    const struct ra_ctx_fns *ctx = contexts[index];
    *dst = (struct m_obj_desc) {
        .name = ctx->name,
        .description = ctx->description,
    };
    return true;
}

static bool check_unknown_entry(const char *name)
{
    return false;
}

const struct m_obj_list ra_ctx_obj_list = {
    .get_desc = get_desc,
    .check_unknown_entry = check_unknown_entry,
    .description = "GPU contexts",
    .allow_trailer = true,
    .disallow_positional_parameters = true,
    .use_global_options = true,
};

static bool get_type_desc(struct m_obj_desc *dst, int index)
{
    int api_index = 0;

    for (int i = 0; i < MP_ARRAY_SIZE(contexts); i++) {
        if (i && strcmp(contexts[i - 1]->type, contexts[i]->type))
            api_index++;

        if (api_index == index) {
            *dst = (struct m_obj_desc) {
                .name = contexts[i]->type,
                .description = "",
            };
            return true;
        }
    }

    return false;
}

static void print_context_apis(struct mp_log *log)
{
    mp_info(log, "Available GPU APIs and contexts:\n");
    for (int n = 0; n < MP_ARRAY_SIZE(contexts); n++) {
        mp_info(log, "  %s %s\n", contexts[n]->type, contexts[n]->name);
    }
}

const struct m_obj_list ra_ctx_type_obj_list = {
    .get_desc = get_type_desc,
    .check_unknown_entry = check_unknown_entry,
    .description = "GPU APIs",
    .allow_trailer = true,
    .disallow_positional_parameters = true,
    .use_global_options = true,
    .print_help_list = print_context_apis,
};

#define OPT_BASE_STRUCT struct ra_ctx_opts
const struct m_sub_options ra_ctx_conf = {
    .opts = (const m_option_t[]) {
        {"gpu-context",
            OPT_SETTINGSLIST(context_list, &ra_ctx_obj_list)},
        {"gpu-api",
            OPT_SETTINGSLIST(context_type_list, &ra_ctx_type_obj_list)},
        {"gpu-debug", OPT_BOOL(debug)},
        {"gpu-sw", OPT_BOOL(allow_sw)},
        {0}
    },
    .size = sizeof(struct ra_ctx_opts),
    .change_flags = UPDATE_VO,
};

static struct ra_ctx *create_in_contexts(struct vo *vo, const char *name,
                                         struct m_obj_settings *context_type_list,
                                         const struct ra_ctx_fns *const ctxs[], size_t size,
                                         struct ra_ctx_opts opts)
{
    for (int i = 0; i < size; i++) {
        if (strcmp(name, ctxs[i]->name) != 0)
            continue;
        if (context_type_list) {
            bool found = false;
            for (int j = 0; context_type_list[j].name; j++) {
                if (strcmp(context_type_list[j].name, "auto") == 0 ||
                    strcmp(context_type_list[j].name, ctxs[i]->type) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
        }
        struct ra_ctx *ctx = talloc_ptrtype(NULL, ctx);
        *ctx = (struct ra_ctx) {
            .vo = vo,
            .global = vo->global,
            .log = mp_log_new(ctx, vo->log, ctxs[i]->type),
            .opts = opts,
            .fns = ctxs[i],
        };

        MP_VERBOSE(ctx, "Initializing GPU context '%s'\n", ctx->fns->name);
        if (ctxs[i]->init(ctx))
            return ctx;
        talloc_free(ctx);
    }
    return NULL;
}

struct ra_ctx *ra_ctx_create_by_name(struct vo *vo, const char *name)
{
    struct ra_ctx_opts dummy = {0};
    struct ra_ctx *ctx = create_in_contexts(vo, name, NULL, contexts,
                                            MP_ARRAY_SIZE(contexts), dummy);
    if (ctx)
        return ctx;
    return create_in_contexts(vo, name, NULL, no_api_contexts,
                              MP_ARRAY_SIZE(no_api_contexts), dummy);
}

// Create a VO window and create a RA context on it.
//  vo_flags: passed to the backend's create window function
struct ra_ctx *ra_ctx_create(struct vo *vo, struct ra_ctx_opts opts)
{
    bool ctx_auto = !opts.context_list ||
                    (opts.context_list[0].name &&
                     strcmp(opts.context_list[0].name, "auto") == 0);

    if (ctx_auto) {
        MP_VERBOSE(vo, "Probing for best GPU context.\n");
        opts.probing = true;
    }

    // Hack to silence backend (X11/Wayland/etc.) errors. Kill it once backends
    // are separate from `struct vo`
    bool old_probing = vo->probing;
    vo->probing = opts.probing;

    struct ra_ctx *ctx = NULL;
    if (opts.probing) {
        struct m_obj_settings context_type_list[2] = {{.name = "auto"}, {0}};

        for (int i = 0;
             opts.context_type_list ? opts.context_type_list[i].name != NULL : i == 0;
             i++) {
            for (int j = 0; j < MP_ARRAY_SIZE(contexts); j++) {
                if (opts.context_type_list)
                    context_type_list[0].name = opts.context_type_list[i].name;

                ctx = create_in_contexts(vo, contexts[j]->name, context_type_list,
                                         contexts, MP_ARRAY_SIZE(contexts),
                                         opts);
                if (ctx)
                    goto done;
            }
        }
    }
    for (int i = 0; opts.context_list && opts.context_list[i].name; i++) {
        ctx = create_in_contexts(vo, opts.context_list[i].name,
                                 opts.context_type_list, contexts,
                                 MP_ARRAY_SIZE(contexts), opts);
        if (ctx)
            goto done;
    }

done:
    if (ctx) {
        vo->probing = old_probing;
        vo->context_name = ctx->fns->name;
        return ctx;
    }

    vo->probing = old_probing;

    // If we've reached this point, then none of the contexts matched the name
    // requested, or the backend creation failed for all of them.
    if (!vo->probing)
        MP_ERR(vo, "Failed initializing any suitable GPU context!\n");
    return NULL;
}

void ra_ctx_destroy(struct ra_ctx **ctx_ptr)
{
    struct ra_ctx *ctx = *ctx_ptr;
    if (!ctx)
        return;

    if (ctx->spirv && ctx->spirv->fns->uninit)
        ctx->spirv->fns->uninit(ctx);

    ctx->fns->uninit(ctx);
    talloc_free(ctx);

    *ctx_ptr = NULL;
}

bool ra_ctx_get_pl_swapchain(struct ra_ctx *ctx, pl_swapchain *swapchain)
{
    *swapchain = NULL;
#if HAVE_VULKAN
    struct mpvk_ctx *vk = ra_vk_ctx_get(ctx);
    if (vk) {
        *swapchain = vk->swapchain;
        return true;
    }
#endif

    return false;
}

static struct pl_color_space color_hint_platform_state(
    const struct pl_color_space *color)
{
    // Surface negotiation can signal only static HDR10 metadata. Keep
    // per-frame HDR10+ and Dolby Vision metadata out of the cache key.
    return (struct pl_color_space) {
        .primaries = color->primaries,
        .transfer = color->transfer,
        .hdr = {
            .prim = color->hdr.prim,
            .min_luma = color->hdr.min_luma,
            .max_luma = color->hdr.max_luma,
            .max_cll = color->hdr.max_cll,
            .max_fall = color->hdr.max_fall,
        },
    };
}

enum ra_color_hint_result ra_swapchain_set_color(
    struct ra_swapchain *sw, pl_swapchain pl_sw,
    struct mp_image_params *params)
{
    bool has_params = params != NULL;
    bool cache_enabled = sw->color_hint_cache_enabled;
    struct pl_color_space request_color;
    if (cache_enabled) {
        request_color = params ? color_hint_platform_state(&params->color)
                               : (struct pl_color_space){0};
    }
    if (cache_enabled && sw->color_hint_valid &&
        sw->color_hint_has_params == has_params &&
        sw->color_hint_swapchain == pl_sw &&
        (!has_params ||
         pl_color_space_equal(&sw->color_hint_request_color, &request_color)))
    {
        if (params) {
            // Preserve per-frame HDR metadata while restoring any platform
            // colorspace normalization from the original request.
            params->color.primaries = sw->color_hint_output_primaries;
            params->color.transfer = sw->color_hint_output_transfer;
        }
        return sw->color_hint_result;
    }

    enum ra_color_hint_result result = RA_COLOR_HINT_NONE;
    if (!params) {
        if (sw->fns->set_color)
            sw->fns->set_color(sw, NULL);
        if (pl_sw)
            pl_swapchain_colorspace_hint(pl_sw, NULL);
    } else if (sw->fns->set_color && sw->fns->set_color(sw, params)) {
        result = RA_COLOR_HINT_EXTERNAL;
    } else if (pl_sw) {
        pl_swapchain_colorspace_hint(pl_sw, &params->color);
        result = RA_COLOR_HINT_SWAPCHAIN;
    } else if (sw->fns->set_color) {
        sw->fns->set_color(sw, NULL);
    }

    // Cache rejected hints too; Android surface changes invalidate the result.
    if (cache_enabled) {
        sw->color_hint_valid = true;
        sw->color_hint_has_params = has_params;
        sw->color_hint_swapchain = pl_sw;
        sw->color_hint_request_color = request_color;
        sw->color_hint_output_primaries =
            params ? params->color.primaries : PL_COLOR_PRIM_UNKNOWN;
        sw->color_hint_output_transfer =
            params ? params->color.transfer : PL_COLOR_TRC_UNKNOWN;
        sw->color_hint_result = result;
    }
    return result;
}

void ra_swapchain_invalidate_color(struct ra_swapchain *sw)
{
    if (sw)
        sw->color_hint_valid = false;
}
