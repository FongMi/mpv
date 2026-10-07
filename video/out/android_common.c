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

#include <libavcodec/jni.h>
#include <android/native_window_jni.h>
#include <limits.h>

#include "android_common.h"
#include "common/msg.h"
#include "misc/jni.h"
#include "options/m_config.h"
#include "vo.h"

struct vo_android_state {
    struct mp_log *log;
    ANativeWindow *native_window;
    int64_t wid;
    struct vo_android_surface_frame drawn;
    struct vo_android_surface_frame presented;
};

bool vo_android_init(struct vo *vo)
{
    vo->android = talloc_zero(vo, struct vo_android_state);
    struct vo_android_state *ctx = vo->android;

    *ctx = (struct vo_android_state){
        .log = mp_log_new(ctx, vo->log, "android"),
    };

    ANativeWindow *native_window = vo_android_create_native_window(vo);
    if (!native_window)
        goto fail;
    vo_android_set_native_window(vo, native_window);

    return true;
fail:
    talloc_free(ctx);
    vo->android = NULL;
    return false;
}

void vo_android_uninit(struct vo *vo)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx)
        return;

    if (ctx->native_window)
        ANativeWindow_release(ctx->native_window);

    talloc_free(ctx);
    vo->android = NULL;
}

ANativeWindow *vo_android_create_native_window(struct vo *vo)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx)
        return NULL;

    JNIEnv *env = MP_JNI_GET_ENV(ctx);
    if (!env) {
        MP_ERR(ctx, "Could not attach java VM.\n");
        return NULL;
    }

    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_ERR(ctx, "Missing surface pointer\n");
        return NULL;
    }

    jobject surface = (jobject)(intptr_t)vo->opts->WinID;
    ANativeWindow *native_window = ANativeWindow_fromSurface(env, surface);
    if (!native_window)
        MP_ERR(ctx, "Failed to create ANativeWindow\n");
    return native_window;
}

void vo_android_set_native_window(struct vo *vo, ANativeWindow *native_window)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx) {
        if (native_window)
            ANativeWindow_release(native_window);
        return;
    }

    if (ctx->native_window)
        ANativeWindow_release(ctx->native_window);
    ctx->native_window = native_window;
    ctx->wid = native_window ? vo->opts->WinID : 0;
    ctx->drawn = ctx->presented = (struct vo_android_surface_frame){0};
    vo_event(vo, VO_EVENT_WIN_STATE);
}

ANativeWindow *vo_android_native_window(struct vo *vo)
{
    struct vo_android_state *ctx = vo->android;
    return ctx ? ctx->native_window : NULL;
}

bool vo_android_has_native_window(struct vo *vo)
{
    return vo_android_native_window(vo) != NULL;
}

static bool get_surface_frame_request(struct vo *vo,
                                     struct vo_android_surface_frame *out)
{
    struct vo_android_state *ctx = vo->android;
    struct mpv_node *node = &vo->opts->android_surface_frame;
    if (!ctx || !ctx->native_window || ctx->wid != vo->opts->WinID ||
        node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list ||
        node->u.list->num != 4 || !node->u.list->values)
        return false;

    struct mpv_node *values = node->u.list->values;
    for (int n = 0; n < 4; n++) {
        if (values[n].format != MPV_FORMAT_INT64)
            return false;
    }
    int64_t token = values[0].u.int64;
    int64_t wid = values[1].u.int64;
    int64_t w = values[2].u.int64;
    int64_t h = values[3].u.int64;
    if (token <= 0 || wid != ctx->wid ||
        w <= 0 || w > INT_MAX || h <= 0 || h > INT_MAX)
        return false;
    *out = (struct vo_android_surface_frame){token, wid, (int)w, (int)h};
    return true;
}

bool vo_android_surface_size(struct vo *vo, int *out_w, int *out_h)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx || !ctx->native_window)
        return false;

    int w = vo->opts->android_surface_size.w,
        h = vo->opts->android_surface_size.h;
    struct vo_android_surface_frame request;
    if (get_surface_frame_request(vo, &request)) {
        w = request.width;
        h = request.height;
    }
    if (!w)
        w = ANativeWindow_getWidth(ctx->native_window);
    if (!h)
        h = ANativeWindow_getHeight(ctx->native_window);

    if (w <= 0 || h <= 0) {
        MP_ERR(ctx, "Failed to get height and width.\n");
        return false;
    }
    *out_w = w;
    *out_h = h;
    return true;
}

static bool surface_frames_equal(const struct vo_android_surface_frame *a,
                                 const struct vo_android_surface_frame *b)
{
    return a->token == b->token && a->wid == b->wid &&
           a->width == b->width && a->height == b->height;
}

void vo_android_surface_frame_drawn(struct vo *vo, int w, int h)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx)
        return;
    ctx->drawn = (struct vo_android_surface_frame){0};
    struct vo_android_surface_frame request;
    if (get_surface_frame_request(vo, &request) &&
        request.token > ctx->presented.token &&
        request.width == w && request.height == h)
        ctx->drawn = request;
}

void vo_android_surface_frame_presented(struct vo *vo, int w, int h)
{
    struct vo_android_state *ctx = vo->android;
    struct vo_android_surface_frame request;
    if (!ctx || !ctx->drawn.token)
        return;
    if (get_surface_frame_request(vo, &request) &&
        surface_frames_equal(&ctx->drawn, &request) &&
        request.width == w && request.height == h)
    {
        ctx->presented = request;
        vo_event(vo, VO_EVENT_WIN_STATE);
    }
    ctx->drawn = (struct vo_android_surface_frame){0};
}

void vo_android_get_surface_frame(struct vo *vo,
                                 struct vo_android_surface_frame *out)
{
    struct vo_android_state *ctx = vo->android;
    struct vo_android_surface_frame request;
    *out = (struct vo_android_surface_frame){0};
    if (get_surface_frame_request(vo, &request) &&
        surface_frames_equal(&request, &ctx->presented))
        *out = ctx->presented;
}
