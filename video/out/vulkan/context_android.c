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

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "video/out/android_common.h"
#include "common.h"
#include "context.h"
#include "utils.h"

struct priv {
    struct mpvk_ctx vk;
};

static VkSurfaceKHR android_create_surface(struct ra_ctx *ctx,
                                           ANativeWindow *native_window,
                                           int msgl)
{
    struct priv *p = ctx->priv;
    struct mpvk_ctx *vk = &p->vk;
    if (!native_window) {
        MP_MSG(ctx, msgl, "Missing Android native window\n");
        return VK_NULL_HANDLE;
    }

    VkAndroidSurfaceCreateInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
         .window = native_window,
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult res = vkCreateAndroidSurfaceKHR(vk->vkinst->instance, &info,
                                             NULL, &surface);
    if (res != VK_SUCCESS) {
        MP_MSG(ctx, msgl, "Failed creating Android surface\n");
        return VK_NULL_HANDLE;
    }

    return surface;
}

static void android_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_vk_ctx_uninit(ctx);
    mpvk_uninit(&p->vk);

    vo_android_uninit(ctx->vo);
}

static bool android_check_visible(struct ra_ctx *ctx)
{
    return vo_android_has_native_window(ctx->vo) &&
           ra_vk_ctx_has_surface(ctx);
}

static bool android_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    struct mpvk_ctx *vk = &p->vk;
    int msgl = ctx->opts.probing ? MSGL_V : MSGL_ERR;

    if (!vo_android_init(ctx->vo))
        goto fail;

    if (!mpvk_init(vk, ctx, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME))
        goto fail;

    struct ra_ctx_params params = {
        .check_visible = android_check_visible,
    };

    vk->surface = android_create_surface(ctx, vo_android_native_window(ctx->vo),
                                         msgl);
    if (vk->surface == VK_NULL_HANDLE)
        goto fail;

    if (!ra_vk_ctx_init(ctx, vk, params, VK_PRESENT_MODE_FIFO_KHR))
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

    ra_vk_ctx_resize(ctx, w, h);
    return true;
}

static bool android_detach_window(struct ra_ctx *ctx)
{
    if (!ra_vk_ctx_detach_surface(ctx))
        return false;

    vo_android_set_native_window(ctx->vo, NULL);
    return true;
}

static bool android_update_window(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct mpvk_ctx *vk = &p->vk;
    if (ctx->vo->opts->WinID == 0 || ctx->vo->opts->WinID == -1)
        return android_detach_window(ctx);

    ANativeWindow *native_window = vo_android_create_native_window(ctx->vo);
    if (!native_window)
        return false;

    VkSurfaceKHR surface = android_create_surface(ctx, native_window, MSGL_ERR);
    if (surface == VK_NULL_HANDLE)
        goto fail;

    if (!ra_vk_ctx_replace_surface(ctx, surface, VK_PRESENT_MODE_FIFO_KHR)) {
        vkDestroySurfaceKHR(vk->vkinst->instance, surface, NULL);
        goto fail;
    }

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

const struct ra_ctx_fns ra_ctx_vulkan_android = {
    .type           = "vulkan",
    .name           = "androidvk",
    .description    = "Android/Vulkan",
    .reconfig       = android_reconfig,
    .control        = android_control,
    .init           = android_init,
    .uninit         = android_uninit,
};
