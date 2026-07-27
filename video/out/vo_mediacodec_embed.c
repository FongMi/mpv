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

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "common/common.h"
#include "android_osd_overlay.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/hwdec.h"

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;
    struct android_osd_overlay *osd_overlay;
    int64_t video_wid;
    double last_pts;
    int osd_failures;
    bool osd_render_ok;
    bool backend_failed;
};

#define MAX_OSD_FAILURES 3

static void report_backend_error(struct vo *vo, const char *component)
{
    struct priv *p = vo->priv;
    if (p->backend_failed)
        return;

    p->backend_failed = true;
    MP_ERR(vo, "Direct MediaCodec %s failed; requesting GPU fallback.\n",
           component);
    vo_report_backend_error(vo);
}

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_ERR(vo, "No Android Surface is attached for direct MediaCodec "
               "output.\n");
        return NULL;
    }

    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->surface = (void *)(intptr_t)(vo->opts->WinID);

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (vo->opts->android_osd_wid == 0 ||
        vo->opts->android_osd_wid == -1)
    {
        MP_ERR(vo, "No Android OSD Surface is attached for direct "
                   "MediaCodec output.\n");
        return -1;
    }

    p->video_wid = vo->opts->WinID;
    p->last_pts = MP_NOPTS_VALUE;
    p->osd_overlay = android_osd_overlay_create(vo);
    if (!p->osd_overlay ||
        !android_osd_overlay_set_surface(
            p->osd_overlay, vo->opts->android_osd_wid))
    {
        MP_ERR(vo, "Could not initialize the direct-output OSD surface.\n");
        goto fail;
    }

    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_embed",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create hwdevice_ctx\n");
        goto fail;
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    return 0;

fail:
    av_buffer_unref(&p->hwctx.av_device_ref);
    if (vo->hwdec_devs) {
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }
    android_osd_overlay_destroy(p->osd_overlay);
    p->osd_overlay = NULL;
    return -1;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->next_image) {
        AVMediaCodecBuffer *buffer =
            (AVMediaCodecBuffer *)p->next_image->planes[3];
        if (av_mediacodec_release_buffer(buffer, 1) < 0)
            report_backend_error(vo, "video surface");
        mp_image_unrefp(&p->next_image);
    }

    bool osd_ok =
        p->osd_render_ok &&
        android_osd_overlay_present(p->osd_overlay);
    if (osd_ok) {
        p->osd_failures = 0;
    } else if (++p->osd_failures >= MAX_OSD_FAILURES) {
        report_backend_error(vo, "OSD surface");
    }
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (frame->current)
        p->last_pts = frame->current->pts;
    if (!frame->redraw && !frame->repeat && frame->current)
        mpi = mp_image_new_ref(frame->current);

    mp_image_unrefp(&p->next_image);
    p->next_image = mpi;
    p->osd_render_ok =
        p->last_pts == MP_NOPTS_VALUE ||
        android_osd_overlay_render(p->osd_overlay, p->last_pts);
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    if (request == VOCTRL_UPDATE_RENDER_OPTS ||
        request == VOCTRL_UPDATE_OSD_SIZE)
    {
        android_osd_overlay_invalidate_geometry(p->osd_overlay);
        vo->want_redraw = true;
        return VO_TRUE;
    }
    if (request != VOCTRL_UPDATE_WINDOW)
        return VO_NOTIMPL;

    // MediaCodec cannot detach or replace its configured output Surface.
    // The player tears this VO down when the video Surface changes and restores
    // it once the required Android Surfaces are available again.
    if (vo->opts->WinID != p->video_wid)
        return VO_NOTIMPL;

    if (!android_osd_overlay_set_surface(
            p->osd_overlay, vo->opts->android_osd_wid))
        return VO_FALSE;
    vo->want_redraw = true;
    return VO_TRUE;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    android_osd_overlay_invalidate_geometry(p->osd_overlay);
    int width = 0;
    int height = 0;
    if (android_osd_overlay_get_size(p->osd_overlay, &width, &height)) {
        vo->dwidth = width;
        vo->dheight = height;
    }
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);
    android_osd_overlay_destroy(p->osd_overlay);

    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    hwdec_devices_destroy(vo->hwdec_devs);
    vo->hwdec_devs = NULL;
}

const struct vo_driver video_out_mediacodec_embed = {
    .description = "Android (Embedded MediaCodec Surface)",
    .name = "mediacodec_embed",
    .caps = VO_CAP_NORETAIN | VO_CAP_NORETAIN_REDRAW | VO_CAP_NATIVE_DOVI,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
