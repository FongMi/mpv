/*
 * Copyright (C) 2010 Benjamin Zores <ben@geexbox.org>
 *
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

/*
 * Blu-ray parser/reader using libbluray
 *  Use 'git clone git://git.videolan.org/libbluray' to get it.
 *
 * TODO:
 *  - Add descrambled keys database support (KEYDB.cfg)
 *
 */

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>

#include <libbluray/bluray.h>
#include <libbluray/meta_data.h>
#include <libbluray/overlay.h>
#include <libbluray/keys.h>
#include <libbluray/bluray-version.h>
#include <libbluray/log_control.h>
#include <libavutil/common.h>

#include "config.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/thread_tools.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/threads.h"
#include "stream.h"
#include "osdep/io.h"
#include "sub/osd.h"
#include "sub/img_convert.h"
#include "video/csputils.h"
#include "video/mp_image.h"

#define BLURAY_SECTOR_SIZE     6144
#define BLURAY_ISO_BLOCK_SIZE  2048

#define BLURAY_DEFAULT_ANGLE      0
#define BLURAY_DEFAULT_CHAPTER    0
#define BLURAY_PLAYLIST_TITLE    -3
#define BLURAY_DEFAULT_TITLE     -2
#define BLURAY_MENU_TITLE        -1

// 90khz ticks
#define BD_TIMEBASE (90000)
#define BD_TIME_TO_S(x) ((x) / (double)(BD_TIMEBASE))
#define BD_TIME_FROM_S(x) ((uint64_t)(x * BD_TIMEBASE))

// Interval between read retries while navigation is idle
#define BLURAY_POLL_TIME_S   0.010
// In menu mode, tolerate this many consecutive idle reads at the end of playlist
#define BLURAY_NAV_EOF_POLLS 100

// copied from aacs.h in libaacs
#define AACS_ERROR_CORRUPTED_DISC -1 /* opening or reading of AACS files failed */
#define AACS_ERROR_NO_CONFIG      -2 /* missing config file */
#define AACS_ERROR_NO_PK          -3 /* no matching processing key */
#define AACS_ERROR_NO_CERT        -4 /* no valid certificate */
#define AACS_ERROR_CERT_REVOKED   -5 /* certificate has been revoked */
#define AACS_ERROR_MMC_OPEN       -6 /* MMC open failed (no MMC drive ?) */
#define AACS_ERROR_MMC_FAILURE    -7 /* MMC failed */
#define AACS_ERROR_NO_DK          -8 /* no matching device key */


#define OPT_BASE_STRUCT struct mp_bluray_opts
const struct m_sub_options stream_bluray_conf = {
    .opts = (const struct m_option[]) {
        {"device", OPT_STRING(bluray_device), .flags = M_OPT_FILE},
        {"angle", OPT_INT(angle), M_RANGE(1, 999)},
        {0},
    },
    .size = sizeof(struct mp_bluray_opts),
    .defaults = &(const struct mp_bluray_opts){
        .angle = 1,
    },
};

// libbluray's support only global debug callback, without per-instance info.
static mp_static_mutex bluray_log_lock = MP_STATIC_MUTEX_INITIALIZER;
static struct mp_log *bluray_log;

static void bluray_logger(const char *msg)
{
    mp_mutex_lock(&bluray_log_lock);
    if (bluray_log)
        mp_msg(bluray_log, MSGL_DEBUG, "%s", msg);
    mp_mutex_unlock(&bluray_log_lock);
}

// One overlay plane (BGRA, premultiplied alpha).
struct bd_overlay_plane {
    uint32_t *work;
    uint32_t *publish;
    int w, h;           // current allocation size (0 if unallocated)
    int disp_w, disp_h; // plane size announced by BD_OVERLAY_INIT
    bool visible;       // publish has any non-zero alpha pixel
};

struct bluray_priv_s {
    BLURAY *bd;
    stream_t *iso_stream;
    mp_mutex iso_stream_lock;
    mp_mutex vm_lock;             // serializes read/event handling with VM controls
    struct mp_log *bluray_log;
    bool probing;               // open is an .iso auto-detection probe
    BLURAY_TITLE_INFO *title_info;
    int num_titles;
    int current_angle;
    int current_title;
    int current_playlist;

    // Cached map from filtered title index (0..num_titles-1) to mpls_id.
    uint32_t *title_to_playlist;

    int cfg_title;
    int cfg_playlist;
    char *cfg_device;
    char *cfg_stream_url;

    struct mp_bluray_opts *opts;
    struct m_config_cache *opts_cache;

    // Disc-menu support. menu_supported is discovered from disc metadata;
    // hdmv_mode becomes true when the navigation VM is actually running.
    // The HDMV graphics controller emits IG-plane primitives through
    // bd_register_overlay_proc (YUV+RLE). BD-J titles bypass it entirely
    // and emit fully-rendered ARGB on the IG plane through
    // bd_register_argb_overlay_proc. Discs that mix HDMV first-play with
    // BD-J menus (or vice versa) need both callbacks registered.
    bool menu_supported;
    atomic_bool hdmv_mode;
    struct bd_overlay_plane ig;
    struct bd_overlay_plane pg;
    mp_mutex overlay_lock;

    uint32_t nav_change_id;          // bumped on overlay FLUSH/HIDE events
    uint32_t discontinuity_id;       // bumped on actions that may hop (SELECT...)
    bool data_delivered;             // any byte returned from fill_buffer yet
    bool still_active;               // holding a finite or indefinite still
    int still_duration;              // finite length in seconds; 0 if infinite
    uint32_t still_id;               // incremented for each new held still
    uint64_t next_read_pos;          // expected bd_tell() of the next read
    bool read_pos_known;             // next_read_pos is valid
    bool resync_owed;                // jump settled, resync seek not acked yet
    bool transition_pending;         // processing events after a held EOF
    bool failed;                     // fatal read or navigation failure
    bool bdj_title;                  // authored BD-J VM owns playlist completion

    // Disc-driven audio/sub selection, mirrored from BD_EVENT_AUDIO_STREAM
    // and BD_EVENT_PG_TEXTST{,_STREAM}. The numbers are 1-based libbluray
    // stream indices.
    int audio_stream_num;
    int sub_stream_num;
    bool sub_visible;
};

extern const stream_info_t stream_info_ffmpeg;
extern const stream_info_t stream_info_cb;
extern const stream_info_t stream_info_bdmv_dir;

// The caller holds overlay_lock. Repeated STILL_TIME events describe the same
// held frame and must not restart its player-side timer.
static void hold_still(struct bluray_priv_s *b, int duration)
{
    if (!b->still_active || b->still_duration != duration)
        b->still_id++;
    b->still_active = true;
    b->still_duration = duration;
}

static void clear_still(struct bluray_priv_s *b)
{
    b->still_active = false;
    b->still_duration = 0;
}

// Lazy (re-)allocation for an overlay plane's working+publish buffer pair.
static bool bd_overlay_ensure(struct bluray_priv_s *priv,
                              struct bd_overlay_plane *p, int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;
    if (p->work && w <= p->w && h <= p->h)
        return true;
    int nw = MPMAX(w, p->w);
    int nh = MPMAX(h, p->h);
    size_t bytes = (size_t)nw * nh * 4;
    uint32_t *work = talloc_realloc(priv, p->work,    uint32_t, nw * nh);
    uint32_t *pub  = talloc_realloc(priv, p->publish, uint32_t, nw * nh);
    if (!work || !pub)
        return false;
    memset(work, 0, bytes);
    memset(pub,  0, bytes);
    p->work    = work;
    p->publish = pub;
    p->w = nw;
    p->h = nh;
    return true;
}

static void bd_overlay_clear(struct bd_overlay_plane *p)
{
    if (p->work)
        memset(p->work, 0, (size_t)p->w * p->h * 4);
}

static void bd_overlay_hide(struct bluray_priv_s *priv, struct bd_overlay_plane *p)
{
    p->visible = false;
    priv->nav_change_id++;
}

static void bd_overlay_flush(struct bluray_priv_s *priv, struct bd_overlay_plane *p)
{
    if (!p->work)
        return;
    size_t n = (size_t)p->w * p->h;
    memcpy(p->publish, p->work, n * 4);
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (p->publish[i] & 0xFF000000) {
            any = true;
            break;
        }
    }
    p->visible = any;
    priv->nav_change_id++;
}

static enum pl_color_system bd_overlay_csp(struct bluray_priv_s *priv)
{
    const BLURAY_TITLE_INFO *ti = priv->title_info;
    if (!ti || !ti->clip_count || !ti->clips[0].video_stream_count)
        return PL_COLOR_SYSTEM_BT_709;
    const BLURAY_STREAM_INFO *vs = &ti->clips[0].video_streams[0];

#if BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 5, 0)
    // HEVC on UHD BD (2160p or 1080p) can be either 2020 or 709
    if (vs->coding_type == BLURAY_STREAM_TYPE_VIDEO_HEVC || vs->format == BLURAY_VIDEO_FORMAT_2160P)
        return vs->color_space == BLURAY_COLOR_SPACE_BT2020 ? PL_COLOR_SYSTEM_BT_2020_NC : PL_COLOR_SYSTEM_BT_709;
#endif

    switch (vs->format) {
#if BLURAY_VERSION <= BLURAY_VERSION_CODE(1, 4, 1)
    case BLURAY_VIDEO_FORMAT_2160P:
        return PL_COLOR_SYSTEM_BT_2020_NC;
#endif
    case BLURAY_VIDEO_FORMAT_480I:  // ITU-R BT.601
    case BLURAY_VIDEO_FORMAT_576I:
    case BLURAY_VIDEO_FORMAT_480P:
    case BLURAY_VIDEO_FORMAT_576P:
        return PL_COLOR_SYSTEM_BT_601;
    default:
        return PL_COLOR_SYSTEM_BT_709;
    }
}

static void bd_palette_to_bgra(const BD_PG_PALETTE_ENTRY *pg, uint32_t out[256],
                               enum pl_color_system csp)
{
    struct mp_csp_params params = MP_CSP_PARAMS_DEFAULTS;
    params.repr.sys = csp;
    params.repr.levels = PL_COLOR_LEVELS_LIMITED;
    params.levels_out = PL_COLOR_LEVELS_FULL;
    struct pl_transform3x3 yuv2rgb;
    mp_get_csp_matrix(&params, &yuv2rgb);

    for (int i = 0; i < 256; i++) {
        int yuv[3] = { pg[i].Y, pg[i].Cb, pg[i].Cr };
        int rgb[3];
        mp_map_fixp_color(&yuv2rgb, 8, yuv, 8, rgb);
        int T = pg[i].T;
        // Pre-multiply RGB by alpha so the OSD layer can composite directly.
        int r = rgb[0] * T / 255;
        int g = rgb[1] * T / 255;
        int b = rgb[2] * T / 255;
        out[i] = ((uint32_t)T << 24) | ((uint32_t)r << 16) |
                 ((uint32_t)g << 8)  |  (uint32_t)b;
    }
    // palette index 0xFF is always transparent.
    out[0xFF] = 0;
}

// Composite one RLE-encoded sub-bitmap into the IG plane at (ov->x, ov->y).
static void bd_overlay_draw_rle(struct bd_overlay_plane *p, const BD_OVERLAY *ov,
                                enum pl_color_system csp)
{
    if (!ov->img || !ov->palette || ov->w <= 0 || ov->h <= 0)
        return;
    uint32_t pal[256];
    bd_palette_to_bgra(ov->palette, pal, csp);

    const BD_PG_RLE_ELEM *rle = ov->img;
    for (int y = 0; y < ov->h; y++) {
        int dst_y = ov->y + y;
        bool in_plane = dst_y >= 0 && dst_y < p->h;
        uint32_t *dst_row = in_plane ? p->work + (size_t)dst_y * p->w : NULL;
        int x = 0;
        int cols = ov->w;
        while (x < ov->w && cols-- >= 0) {
            int len = rle->len;
            int color = rle->color;
            rle++;
            if (len == 0)
                continue; // stray EOL, skip
            int dst_x = ov->x + x;
            int run = len;
            if (dst_x < 0) {
                int skip = MPMIN(-dst_x, run);
                run -= skip;
                dst_x += skip;
            }
            if (dst_x + run > p->w)
                run = p->w - dst_x;
            if (dst_row && run > 0) {
                uint32_t c = pal[color & 0xFF];
                for (int i = 0; i < run; i++)
                    dst_row[dst_x + i] = c;
            }
            x += len;
        }
        if (rle->len == 0)
            rle++;
    }
}

// Called by libbluray's HDMV graphics controller for every overlay primitive
// on either plane. We only render the IG (menu) plane; PG (subtitles) flows
// through the regular demuxer/sd_lavc pipeline.
static void bd_yuv_overlay_cb(void *handle, const struct bd_overlay_s *ov)
{
    struct bluray_priv_s *priv = handle;
    if (!ov)
        return;
    if (ov->plane != BD_OVERLAY_IG)
        return;

    struct bd_overlay_plane *p = &priv->ig;
    mp_mutex_lock(&priv->overlay_lock);
    switch (ov->cmd) {
    case BD_OVERLAY_INIT:
        if (bd_overlay_ensure(priv, p, ov->w, ov->h)) {
            // The allocation only ever grows, but the announced size is the
            // coordinate space of the current menu.
            p->disp_w = ov->w;
            p->disp_h = ov->h;
        }
        bd_overlay_clear(p);
        bd_overlay_hide(priv, p);
        break;
    case BD_OVERLAY_CLOSE:
    case BD_OVERLAY_HIDE:
        bd_overlay_hide(priv, p);
        break;
    case BD_OVERLAY_CLEAR:
        bd_overlay_clear(p);
        break;
    case BD_OVERLAY_WIPE:
        if (p->work) {
            for (int y = 0; y < ov->h; y++) {
                int dy = ov->y + y;
                if (dy < 0 || dy >= p->h)
                    continue;
                int dx = MPMAX(ov->x, 0);
                int run = MPMIN(ov->w, p->w - dx);
                if (run > 0)
                    memset(p->work + (size_t)dy * p->w + dx, 0, run * 4);
            }
        }
        break;
    case BD_OVERLAY_DRAW:
        if (p->work)
            bd_overlay_draw_rle(p, ov, bd_overlay_csp(priv));
        break;
    case BD_OVERLAY_FLUSH:
        bd_overlay_flush(priv, p);
        break;
    default:
        break;
    }
    mp_mutex_unlock(&priv->overlay_lock);
}

static inline uint32_t bd_argb_premul(uint32_t src)
{
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0)
        return 0;
    if (a == 255)
        return src;
    uint32_t r = (src >> 16) & 0xFF;
    uint32_t g = (src >>  8) & 0xFF;
    uint32_t b =  src        & 0xFF;
    r = (r * a + 127) / 255;
    g = (g * a + 127) / 255;
    b = (b * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void bd_overlay_draw_argb(struct bd_overlay_plane *p,
                                 const BD_ARGB_OVERLAY *ov)
{
    if (!ov->argb || ov->w <= 0 || ov->h <= 0)
        return;
    int sx = 0, sy = 0;
    int dx = ov->x, dy = ov->y;
    int w = ov->w, h = ov->h;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > p->w) w = p->w - dx;
    if (dy + h > p->h) h = p->h - dy;
    if (w <= 0 || h <= 0)
        return;
    for (int y = 0; y < h; y++) {
        const uint32_t *src = ov->argb + (size_t)(sy + y) * ov->stride + sx;
        uint32_t *dst = p->work + (size_t)(dy + y) * p->w + dx;
        for (int x = 0; x < w; x++)
            dst[x] = bd_argb_premul(src[x]);
    }
}

// BD-J graphics use IG; retain support for ARGB presentation graphics on PG.
static void bd_argb_overlay_cb(void *handle, const BD_ARGB_OVERLAY *ov)
{
    struct bluray_priv_s *priv = handle;
    if (!ov)
        return;
    if (ov->plane != BD_OVERLAY_PG && ov->plane != BD_OVERLAY_IG)
        return;

    struct bd_overlay_plane *p = ov->plane == BD_OVERLAY_IG ? &priv->ig
                                                            : &priv->pg;
    mp_mutex_lock(&priv->overlay_lock);
    switch (ov->cmd) {
    case BD_ARGB_OVERLAY_INIT:
        if (bd_overlay_ensure(priv, p, ov->w, ov->h)) {
            // The allocation only ever grows, but the announced size is the
            // coordinate space of the current menu.
            p->disp_w = ov->w;
            p->disp_h = ov->h;
        }
        bd_overlay_clear(p);
        bd_overlay_hide(priv, p);
        break;
    case BD_ARGB_OVERLAY_CLOSE:
        bd_overlay_hide(priv, p);
        break;
    case BD_ARGB_OVERLAY_DRAW:
        if (p->work)
            bd_overlay_draw_argb(p, ov);
        break;
    case BD_ARGB_OVERLAY_FLUSH:
        bd_overlay_flush(priv, p);
        break;
    default:
        break;
    }
    mp_mutex_unlock(&priv->overlay_lock);
}

inline static int play_playlist(struct bluray_priv_s *priv, int playlist)
{
    return bd_select_playlist(priv->bd, playlist);
}

inline static int play_title(struct bluray_priv_s *priv, int title)
{
    return bd_select_title(priv->bd, title);
}

static bool play_menu(stream_t *s)
{
    struct bluray_priv_s *b = s->priv;
    const BLURAY_DISC_INFO *info = bd_get_disc_info(b->bd);

    bd_register_overlay_proc(b->bd, b, bd_yuv_overlay_cb);
    if (info->bdj_detected)
        bd_register_argb_overlay_proc(b->bd, b, bd_argb_overlay_cb, NULL);
    if (bd_play(b->bd) &&
        (info->first_play_supported || bd_menu_call(b->bd, -1)))
        return true;

    bd_register_overlay_proc(b->bd, NULL, NULL);
    bd_register_argb_overlay_proc(b->bd, NULL, NULL, NULL);
    MP_WARN(s, "Couldn't start Blu-ray navigation.\n");
    return false;
}

static bool start_hdmv_navigation(stream_t *s)
{
    struct bluray_priv_s *b = s->priv;
    if (atomic_load(&b->hdmv_mode))
        return true;
    if (!b->menu_supported)
        return false;

    if (!play_menu(s))
        return false;

    int title = bd_get_current_title(b->bd);
    mp_mutex_lock(&b->overlay_lock);
    if (b->title_info) {
        bd_free_title_info(b->title_info);
        b->title_info = NULL;
    }
    atomic_store(&b->hdmv_mode, true);
    b->current_title = title;
    b->current_playlist = -1;
    clear_still(b);
    b->read_pos_known = false;
    // The old title can already be queued in the player (especially while
    // paused). Hold the new VM until the regular BD resync seek flushes those
    // packets and acknowledges the boundary.
    b->resync_owed = true;
    b->discontinuity_id++;
    b->nav_change_id++;
    mp_mutex_unlock(&b->overlay_lock);
    MP_VERBOSE(s, "bdnav: entered HDMV on demand; current title=%d\n", title);
    return true;
}

static void bluray_stream_close(stream_t *s)
{
    struct bluray_priv_s *priv = s->priv;
    if (!priv)
        return;

    if (priv->title_info)
        bd_free_title_info(priv->title_info);
    if (priv->bd) {
        if (atomic_load(&priv->hdmv_mode)) {
            bd_register_overlay_proc(priv->bd, NULL, NULL);
            bd_register_argb_overlay_proc(priv->bd, NULL, NULL, NULL);
        }
        bd_close(priv->bd);
    }
    if (priv->iso_stream)
        free_stream(priv->iso_stream);
    mp_mutex_lock(&bluray_log_lock);
    // If we created the global log, unset it.
    if (bluray_log == priv->bluray_log)
        bluray_log = NULL;
    mp_mutex_unlock(&bluray_log_lock);
    mp_mutex_destroy(&priv->iso_stream_lock);
    mp_mutex_destroy(&priv->vm_lock);
    mp_mutex_destroy(&priv->overlay_lock);
}

static int read_iso_blocks(void *handle, void *buf, int lba, int num_blocks)
{
    struct bluray_priv_s *b = handle;
    stream_t *src = b ? b->iso_stream : NULL;
    if (!src || lba < 0 || num_blocks <= 0)
        return 0;

    int64_t pos = (int64_t)lba * BLURAY_ISO_BLOCK_SIZE;
    int64_t size = (int64_t)num_blocks * BLURAY_ISO_BLOCK_SIZE;
    if (size > INT_MAX)
        return 0;

    mp_mutex_lock(&b->iso_stream_lock);
    int read = 0;
    if (stream_tell(src) == pos || stream_seek(src, pos))
        read = stream_read(src, buf, (int)size);
    mp_mutex_unlock(&b->iso_stream_lock);

    return read > 0 ? read / BLURAY_ISO_BLOCK_SIZE : 0;
}

static int open_bluray_from_stream(stream_t *s, const char *url)
{
    struct bluray_priv_s *b = s->priv;
    bool media3_source = strncmp(url, "media3iso://", 12) == 0;
    struct stream_open_args args = {
        .global = s->global,
        .cancel = s->cancel,
        .url = url,
        .flags = STREAM_READ | (s->stream_origin & STREAM_ORIGIN_MASK),
        .sinfo = media3_source ? &stream_info_cb : &stream_info_ffmpeg,
    };
    stream_t *iso_stream = NULL;
    int r = stream_create_with_args(&args, &iso_stream);
    if (r != STREAM_OK)
        return r;
    if (!iso_stream)
        return STREAM_ERROR;
    if (media3_source) {
        // Media3 registers this callback only for HTTP(S) disc images.
        iso_stream->is_network = true;
        iso_stream->streaming = true;
    }

    if (!iso_stream->seekable) {
        MP_ERR(s, "Blu-ray ISO stream must be seekable.\n");
        free_stream(iso_stream);
        return STREAM_UNSUPPORTED;
    }

    BLURAY *bd = bd_init();
    if (!bd) {
        free_stream(iso_stream);
        return STREAM_ERROR;
    }

    b->iso_stream = iso_stream;
    if (!bd_open_stream(bd, b, read_iso_blocks)) {
        bd_close(bd);
        b->iso_stream = NULL;
        free_stream(iso_stream);
        return STREAM_UNSUPPORTED;
    }

    b->bd = bd;
    s->is_network = iso_stream->is_network;
    s->streaming = iso_stream->streaming;
    return STREAM_OK;
}

static const char *bd_event_str(uint32_t event)
{
#if BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 3, 0)
    const char *name = bd_event_name(event);
    if (name)
        return name;
#endif
    return "?";
}

static int bluray_stream_fill_buffer(stream_t *s, void *buf, int len)
{
    struct bluray_priv_s *b = s->priv;

    mp_mutex_lock(&b->vm_lock);
    mp_mutex_lock(&b->overlay_lock);
    uint32_t disc_id = b->discontinuity_id;
    bool resync_owed = b->resync_owed;
    mp_mutex_unlock(&b->overlay_lock);

    // Resync pending, skip the read.
    if (resync_owed) {
        mp_mutex_unlock(&b->vm_lock);
        return 0;
    }

    int idle_reads = 0;
    bool hopped = false;
    while (!mp_cancel_test(s->cancel)) {
        BD_EVENT ev;
        // BD-J applets reposition the stream, sometimes with no event.
        // Detect it before reading so the new position's head survives.
        if (!hopped && b->read_pos_known && bd_tell(b->bd) != b->next_read_pos) {
            MP_VERBOSE(s, "position warped (%"PRIu64" -> %"PRIu64"), "
                       "treating as a jump\n", b->next_read_pos, bd_tell(b->bd));
            b->read_pos_known = false;
            mp_mutex_lock(&b->overlay_lock);
            b->transition_pending = true;
            clear_still(b);
            if (b->discontinuity_id == disc_id)
                b->discontinuity_id++;
            mp_mutex_unlock(&b->overlay_lock);
            hopped = true;
        }
        // Once a jump was detected, stop consuming data (it belongs to the
        // new position and the slave demuxer must be reopened first); a
        // zero-length read only polls the remaining events.
        int n = bd_read_ext(b->bd, buf, hopped ? 0 : len, &ev);
        if (n < 0) {
            MP_VERBOSE(s, "bd_read_ext() failed.\n");
            mp_mutex_lock(&b->overlay_lock);
            b->transition_pending = false;
            b->failed |= !mp_cancel_test(s->cancel);
            clear_still(b);
            mp_mutex_unlock(&b->overlay_lock);
            mp_mutex_unlock(&b->vm_lock);
            return -1;
        }

        if (ev.event != BD_EVENT_NONE)
            MP_DBG(s, "event %s(%u) param %u\n", bd_event_str(ev.event), ev.event, ev.param);

        bool stalled = false;  // EOF unless the VM continues
        bool bdj_idle = false; // BD-J title alive with no playlist playing

        switch (ev.event) {
        case BD_EVENT_NONE:
            stalled = n == 0;
            break;
        case BD_EVENT_ERROR:
            MP_ERR(s, "Blu-ray navigation error (%u).\n", ev.param);
            mp_mutex_lock(&b->overlay_lock);
            b->transition_pending = false;
            b->failed |= !mp_cancel_test(s->cancel);
            clear_still(b);
            mp_mutex_unlock(&b->overlay_lock);
            mp_mutex_unlock(&b->vm_lock);
            return -1;
        case BD_EVENT_READ_ERROR:
            MP_WARN(s, "Blu-ray read error, skipping unit.\n");
            break;
        case BD_EVENT_END_OF_TITLE:
            mp_mutex_lock(&b->overlay_lock);
            b->transition_pending = b->data_delivered;
            clear_still(b);
            mp_mutex_unlock(&b->overlay_lock);
            stalled = true;
            break;
        case BD_EVENT_IDLE:
            bdj_idle = true;
            break;
        case BD_EVENT_STILL:
            mp_mutex_lock(&b->overlay_lock);
            if (!ev.param) {
                // A real still release must reset the slave's latched EOF,
                // even when playback resumes within the same playlist.
                if (b->still_active && b->data_delivered) {
                    b->transition_pending = true;
                    b->discontinuity_id++;
                }
                clear_still(b);
            } else if (!b->still_active) {
                hold_still(b, 0);
            }
            mp_mutex_unlock(&b->overlay_lock);
            break;
        case BD_EVENT_STILL_TIME:
            mp_mutex_lock(&b->overlay_lock);
            hold_still(b, ev.param);
            mp_mutex_unlock(&b->overlay_lock);
            break;
        case BD_EVENT_PLAYLIST: {
            int playlist = ev.param;
            int title = bd_get_current_title(b->bd);
            if (b->title_to_playlist) {
                for (int i = 0; i < b->num_titles; i++) {
                    if (b->title_to_playlist[i] == ev.param) {
                        title = i;
                        break;
                    }
                }
            }
            mp_mutex_lock(&b->overlay_lock);
            int cur_angle = b->current_angle;
            mp_mutex_unlock(&b->overlay_lock);
            BLURAY_TITLE_INFO *ti = bd_get_playlist_info(b->bd, playlist, cur_angle);
            mp_mutex_lock(&b->overlay_lock);
            if (b->title_info)
                bd_free_title_info(b->title_info);
            b->title_info = ti;
            b->current_playlist = playlist;
            b->current_title = title;
            clear_still(b);
            if (atomic_load(&b->hdmv_mode)) {
                b->transition_pending = b->data_delivered;
                b->discontinuity_id++;
            }
            mp_mutex_unlock(&b->overlay_lock);
            break;
        }
        case BD_EVENT_TITLE: {
            const BLURAY_DISC_INFO *info = bd_get_disc_info(b->bd);
            const BLURAY_TITLE *disc_title = NULL;
            if (info) {
                if (ev.param == BLURAY_TITLE_FIRST_PLAY)
                    disc_title = info->first_play;
                else if (ev.param == BLURAY_TITLE_TOP_MENU)
                    disc_title = info->top_menu;
                else if (ev.param <= info->num_titles && info->titles)
                    disc_title = info->titles[ev.param];
            }
            b->bdj_title = disc_title && disc_title->bdj;
            int title = bd_get_current_title(b->bd);
            mp_mutex_lock(&b->overlay_lock);
            if (b->title_info) {
                bd_free_title_info(b->title_info);
                b->title_info = NULL;
            }
            b->current_title = title;
            clear_still(b);
            if (atomic_load(&b->hdmv_mode)) {
                b->transition_pending = b->data_delivered;
                b->discontinuity_id++;
            }
            mp_mutex_unlock(&b->overlay_lock);
            break;
        }
        case BD_EVENT_ANGLE: {
            // The event value is 1-based, current_angle is 0-based.
            int angle = MPMAX(0, (int)ev.param - 1);
            BLURAY_TITLE_INFO *ti = b->title_info ? bd_get_playlist_info(b->bd, b->current_playlist, angle)
                                                  : NULL;
            mp_mutex_lock(&b->overlay_lock);
            b->current_angle = angle;
            if (ti) {
                bd_free_title_info(b->title_info);
                b->title_info = ti;
            }
            if (atomic_load(&b->hdmv_mode))
                b->nav_change_id++;
            mp_mutex_unlock(&b->overlay_lock);
            break;
        }
        case BD_EVENT_AUDIO_STREAM:
            mp_mutex_lock(&b->overlay_lock);
            b->audio_stream_num = ev.param;
            if (atomic_load(&b->hdmv_mode))
                b->nav_change_id++;
            mp_mutex_unlock(&b->overlay_lock);
            break;
        case BD_EVENT_PG_TEXTST_STREAM:
            mp_mutex_lock(&b->overlay_lock);
            b->sub_stream_num = ev.param;
            if (atomic_load(&b->hdmv_mode))
                b->nav_change_id++;
            mp_mutex_unlock(&b->overlay_lock);
            break;
        case BD_EVENT_PG_TEXTST:
            mp_mutex_lock(&b->overlay_lock);
            b->sub_visible = ev.param != 0;
            if (atomic_load(&b->hdmv_mode))
                b->nav_change_id++;
            mp_mutex_unlock(&b->overlay_lock);
            break;
        case BD_EVENT_SEEK:
        case BD_EVENT_DISCONTINUITY:
            mp_mutex_lock(&b->overlay_lock);
            if (b->data_delivered && (b->still_active || b->transition_pending)) {
                b->transition_pending = true;
                b->discontinuity_id++;
            }
            clear_still(b);
            mp_mutex_unlock(&b->overlay_lock);
            break;
        default:
            MP_TRACE(s, "Unhandled event: %s(%u) %u\n",
                     bd_event_str(ev.event), ev.event, ev.param);
            break;
        }

        if (n > 0) {
            // A jump event popped by this read can mean the data already
            // belongs to the new position. Drop non-contiguous reads and
            // drain like any other jump.
            uint64_t pos = bd_tell(b->bd);
            if (b->read_pos_known && pos - n != b->next_read_pos) {
                MP_VERBOSE(s, "read not contiguous (expected %"PRIu64", "
                           "got %"PRIu64"), treating as a jump\n",
                           b->next_read_pos, pos - n);
                b->read_pos_known = false;
                mp_mutex_lock(&b->overlay_lock);
                b->transition_pending = true;
                clear_still(b);
                if (b->discontinuity_id == disc_id)
                    b->discontinuity_id++;
                mp_mutex_unlock(&b->overlay_lock);
                hopped = true;
                goto next_read;
            }
            if (!b->read_pos_known)
                MP_DBG(s, "reads resume at %"PRIu64"\n", pos - n);
            b->next_read_pos = pos;
            b->read_pos_known = true;
            if (b->still_active) {
                mp_mutex_lock(&b->overlay_lock);
                clear_still(b);
                mp_mutex_unlock(&b->overlay_lock);
            }
            b->data_delivered = true;
            mp_mutex_unlock(&b->vm_lock);
            return n;
        }

        mp_mutex_lock(&b->overlay_lock);
        bool still_active = b->still_active;
        if (!hopped && b->data_delivered && b->discontinuity_id != disc_id) {
            MP_VERBOSE(s, "disc jumped (id %u -> %u), draining events\n",
                       disc_id, b->discontinuity_id);
            hopped = true;
        }
        mp_mutex_unlock(&b->overlay_lock);

        // The play position jumped to another title/playlist after data was
        // already delivered: report EOF so demux_disc reopens the slave
        // demuxer before it parses data from the new position. A single jump
        // queues a burst of events (TITLE, PLAYLIST, ...) of which more than
        // one can bump discontinuity_id, so hold the EOF until the burst is
        // fully drained: with a bump still queued, the slave demuxer
        // reopened for this jump would hit that bump's EOF right away while
        // probing the new position, and fail to open.
        if (hopped) {
            if (ev.event != BD_EVENT_NONE)
                goto next_read;
            b->read_pos_known = false;
            mp_mutex_lock(&b->overlay_lock);
            b->resync_owed = true;
            b->transition_pending = false;
            mp_mutex_unlock(&b->overlay_lock);
            MP_VERBOSE(s, "jump settled (id %u), EOF for demuxer resync\n",
                       b->discontinuity_id);
            mp_mutex_unlock(&b->vm_lock);
            return 0;
        }

        // Report EOF so the decoders drain and display the last frame. The
        // player times finite stills; user interaction can release either
        // kind. Both resume through the regular discontinuity/resync path.
        if (still_active) {
            // Drain queued events first, the still may already be released.
            // libbluray re-emits STILL_TIME on every read while holding, it
            // is not progress.
            if (ev.event != BD_EVENT_NONE && ev.event != BD_EVENT_STILL_TIME)
                goto next_read;
            MP_VERBOSE(s, "holding still frame, EOF\n");
            mp_mutex_lock(&b->overlay_lock);
            b->transition_pending = false;
            mp_mutex_unlock(&b->overlay_lock);
            mp_mutex_unlock(&b->vm_lock);
            return 0;
        }

next_read:
        // END_OF_TITLE describes a playlist. A live BD-J title may select
        // its next playlist asynchronously, just as it does during IDLE.
        bdj_idle |= stalled && b->bdj_title && atomic_load(&b->hdmv_mode);
        mp_mutex_unlock(&b->vm_lock);
        if (bdj_idle) {
            idle_reads = 0;
            mp_cancel_wait(s->cancel, BLURAY_POLL_TIME_S);
        } else if (stalled) {
            // Without menus there is nothing that could continue: plain EOF.
            if (!atomic_load(&b->hdmv_mode)) {
                mp_mutex_lock(&b->overlay_lock);
                b->transition_pending = false;
                mp_mutex_unlock(&b->overlay_lock);
                return 0;
            }
            // Retry for a while before concluding that playback ended. The
            // first retry is immediate: a just-resumed VM progresses at once.
            if (++idle_reads > BLURAY_NAV_EOF_POLLS) {
                MP_VERBOSE(s, "Navigation stopped, EOF.\n");
                mp_mutex_lock(&b->overlay_lock);
                b->transition_pending = false;
                mp_mutex_unlock(&b->overlay_lock);
                return 0;
            }
            if (idle_reads > 1)
                mp_cancel_wait(s->cancel, BLURAY_POLL_TIME_S);
        } else {
            // The VM/title made progress; read again immediately.
            idle_reads = 0;
        }
        mp_mutex_lock(&b->vm_lock);
        mp_mutex_lock(&b->overlay_lock);
        resync_owed = b->resync_owed;
        mp_mutex_unlock(&b->overlay_lock);
        if (resync_owed) {
            mp_mutex_unlock(&b->vm_lock);
            return 0;
        }
    }
    mp_mutex_lock(&b->overlay_lock);
    b->transition_pending = false;
    mp_mutex_unlock(&b->overlay_lock);
    mp_mutex_unlock(&b->vm_lock);
    return 0;
}

static int bluray_stream_control_locked(stream_t *s, int cmd, void *arg)
{
    struct bluray_priv_s *b = s->priv;

    switch (cmd) {
    case STREAM_CTRL_GET_NUM_CHAPTERS: {
        mp_mutex_lock(&b->overlay_lock);
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (ti)
            *((unsigned int *) arg) = ti->chapter_count;
        mp_mutex_unlock(&b->overlay_lock);
        return ti ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_GET_CHAPTER_TIME: {
        int chapter = *(double *)arg;
        mp_mutex_lock(&b->overlay_lock);
        const BLURAY_TITLE_INFO *ti = b->title_info;
        int rc = STREAM_UNSUPPORTED;
        if (ti) {
            double time = MP_NOPTS_VALUE;
            if (chapter >= 0 && chapter < ti->chapter_count)
                time = BD_TIME_TO_S(ti->chapters[chapter].start);
            if (time != MP_NOPTS_VALUE) {
                *(double *)arg = time;
                rc = STREAM_OK;
            } else {
                rc = STREAM_ERROR;
            }
        }
        mp_mutex_unlock(&b->overlay_lock);
        return rc;
    }
    case STREAM_CTRL_SET_CURRENT_TITLE: {
        const uint32_t title = *((unsigned int*)arg);
        // demux_disc appends a synthetic "Disc Menu" edition at index num_titles.
        if (title == (uint32_t)b->num_titles) {
            if (atomic_load(&b->hdmv_mode)) {
                if (!bd_menu_call(b->bd, -1))
                    return STREAM_UNSUPPORTED;
                return STREAM_OK;
            }
            return start_hdmv_navigation(s) ? STREAM_OK : STREAM_UNSUPPORTED;
        }
        if (title >= (uint32_t)b->num_titles || !play_title(b, title))
            return STREAM_UNSUPPORTED;
        b->bdj_title = false;
        mp_mutex_lock(&b->overlay_lock);
        b->current_title = title;
        b->transition_pending = false;
        clear_still(b);
        b->read_pos_known = false;
        b->discontinuity_id++;
        b->resync_owed = true;
        mp_mutex_unlock(&b->overlay_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CURRENT_TITLE: {
        mp_mutex_lock(&b->overlay_lock);
        *((unsigned int *) arg) = b->current_title;
        mp_mutex_unlock(&b->overlay_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NUM_TITLES: {
        *((unsigned int *)arg) = b->num_titles;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TIME_LENGTH: {
        mp_mutex_lock(&b->overlay_lock);
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (ti)
            *((double *) arg) = BD_TIME_TO_S(ti->duration);
        mp_mutex_unlock(&b->overlay_lock);
        return ti ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_GET_CURRENT_TIME: {
        *((double *) arg) = BD_TIME_TO_S(bd_tell_time(b->bd));
        return STREAM_OK;
    }
    case STREAM_CTRL_SEEK_TO_TIME: {
        double pts = *((double *) arg);
        if (!isfinite(pts) || pts < 0 ||
            pts >= UINT64_MAX / (double)BD_TIMEBASE ||
            !bd_seek_time_checked(b->bd, BD_TIME_FROM_S(pts)))
            return STREAM_ERROR;
        stream_drop_buffers(s);
        b->read_pos_known = false;
        mp_mutex_lock(&b->overlay_lock);
        b->transition_pending = false;
        clear_still(b);
        mp_mutex_unlock(&b->overlay_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_NAV_DRAIN_ACK:
        mp_mutex_lock(&b->overlay_lock);
        b->resync_owed = false;
        mp_mutex_unlock(&b->overlay_lock);
        return STREAM_OK;
    case STREAM_CTRL_NAV_STILL_SKIP: {
        struct stream_nav_still_skip *req = arg;
        mp_mutex_lock(&b->overlay_lock);
        bool current = b->still_active && b->still_duration > 0 &&
                       b->still_id == req->id;
        mp_mutex_unlock(&b->overlay_lock);
        if (!current)
            return STREAM_ERROR;

        // vm_lock serializes this with reads and controls. libbluray may
        // invoke overlay callbacks, so do not hold overlay_lock here.
        int result = bd_read_skip_still(b->bd);
        mp_mutex_lock(&b->overlay_lock);
        if (result) {
            clear_still(b);
            // The slave demuxer has latched EOF. Release it through the same
            // resync used for authored jumps before reading the next clip.
            b->read_pos_known = false;
            b->discontinuity_id++;
            b->resync_owed = true;
        }
        mp_mutex_unlock(&b->overlay_lock);
        if (!result)
            MP_ERR(s, "failed to skip finite still %u\n", req->id);
        return result ? STREAM_OK : STREAM_ERROR;
    }
    case STREAM_CTRL_GET_NUM_ANGLES: {
        mp_mutex_lock(&b->overlay_lock);
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (ti)
            *((int *) arg) = ti->angle_count;
        mp_mutex_unlock(&b->overlay_lock);
        return ti ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_GET_ANGLE: {
        mp_mutex_lock(&b->overlay_lock);
        *((int *) arg) = b->current_angle + 1;
        mp_mutex_unlock(&b->overlay_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_ANGLE: {
        int angle = *((int *) arg);
        mp_mutex_lock(&b->overlay_lock);
        const BLURAY_TITLE_INFO *ti = b->title_info;
        bool ok = ti && angle >= 1 && angle <= ti->angle_count;
        if (ok)
            b->current_angle = angle - 1;
        int cur = b->current_angle;
        mp_mutex_unlock(&b->overlay_lock);
        if (!ok)
            return STREAM_UNSUPPORTED;
        bd_seamless_angle_change(b->bd, cur);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TITLE_LENGTH: {
        int title = *(double *)arg;
        if (!b->bd || title < 0 || title >= b->num_titles)
            return STREAM_UNSUPPORTED;
        BLURAY_TITLE_INFO *ti = bd_get_title_info(b->bd, title, 0);
        if (!ti)
            return STREAM_UNSUPPORTED;
        *(double *)arg = BD_TIME_TO_S(ti->duration);
        bd_free_title_info(ti);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TITLE_PLAYLIST: {
        int title = *(double *)arg;
        if (!b->bd || title < 0 || title >= b->num_titles)
            return STREAM_UNSUPPORTED;
        BLURAY_TITLE_INFO *ti = bd_get_title_info(b->bd, title, 0);
        if (!ti)
            return STREAM_UNSUPPORTED;
        *(double *)arg = ti->playlist;
        bd_free_title_info(ti);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_LANG: {
        int rc = STREAM_ERROR;
        mp_mutex_lock(&b->overlay_lock);
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (ti && ti->clip_count) {
            struct stream_lang_req *req = arg;
            BLURAY_STREAM_INFO *si = NULL;
            int count = 0;
            switch (req->type) {
            case STREAM_AUDIO:
                count = ti->clips[0].audio_stream_count;
                si = ti->clips[0].audio_streams;
                break;
            case STREAM_SUB:
                count = ti->clips[0].pg_stream_count;
                si = ti->clips[0].pg_streams;
                break;
            }
            for (int n = 0; n < count; n++) {
                BLURAY_STREAM_INFO *i = &si[n];
                if (i->pid == req->id) {
                    snprintf(req->name, sizeof(req->name), "%.4s", i->lang);
                    rc = STREAM_OK;
                    break;
                }
            }
        }
        mp_mutex_unlock(&b->overlay_lock);
        return rc;
    }
    case STREAM_CTRL_GET_DISC_NAME: {
        const struct meta_dl *meta = bd_get_meta(b->bd);
        if (!meta || !meta->di_name || !meta->di_name[0])
            break;
        *(char**)arg = talloc_strdup(NULL, meta->di_name);
        return STREAM_OK;
    }
    case STREAM_CTRL_NAV_CMD: {
        struct stream_nav_cmd *nav = arg;
        if (!atomic_load(&b->hdmv_mode)) {
            switch (nav->action) {
            case STREAM_NAV_MENU_ROOT:
            case STREAM_NAV_MENU_TITLE:
                // Start First Play, or Top Menu when First Play is unavailable.
                return start_hdmv_navigation(s) ? STREAM_OK : STREAM_UNSUPPORTED;
            case STREAM_NAV_MENU_POPUP:
            case STREAM_NAV_PREV_MENU:
                if (!start_hdmv_navigation(s))
                    return STREAM_UNSUPPORTED;
                break;
            default:
                return STREAM_UNSUPPORTED;
            }
        }
        uint32_t key = BD_VK_NONE;
        switch (nav->action) {
        case STREAM_NAV_UP:
            key = BD_VK_UP;
            break;
        case STREAM_NAV_DOWN:
            key = BD_VK_DOWN;
            break;
        case STREAM_NAV_LEFT:
            key = BD_VK_LEFT;
            break;
        case STREAM_NAV_RIGHT:
            key = BD_VK_RIGHT;
            break;
        case STREAM_NAV_SELECT:
            key = BD_VK_ENTER;
            break;
        case STREAM_NAV_MENU_ROOT:
        case STREAM_NAV_MENU_TITLE:
            // BD doesn't distinguish "title menu", both map to disc root.
            if (!bd_menu_call(b->bd, -1))
                return STREAM_UNSUPPORTED;
            break;
        case STREAM_NAV_MENU_POPUP:
            key = BD_VK_POPUP;
            break;
        case STREAM_NAV_PREV_MENU:
            // No dedicated "previous menu" key; popup-toggle is the closest
            // equivalent and behaves like "dismiss current menu" on most
            // discs when already in popup.
            key = BD_VK_POPUP;
            break;
        case STREAM_NAV_MOUSE_MOVE:
            if (bd_mouse_select(b->bd, -1, nav->x, nav->y) < 0)
                return STREAM_UNSUPPORTED;
            break;
        case STREAM_NAV_MOUSE_CLICK:
            if (bd_mouse_select(b->bd, -1, nav->x, nav->y) < 0)
                return STREAM_UNSUPPORTED;
            key = BD_VK_MOUSE_ACTIVATE;
            break;
        default:
            return STREAM_UNSUPPORTED;
        }
        if (key != BD_VK_NONE && bd_user_input(b->bd, -1, key) < 0)
            return STREAM_UNSUPPORTED;
        // Accepted input need not move playback. Keep the still and its timer
        // until reads report an actual release or source transition.
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_STATE: {
        struct stream_nav_state *st = arg;
        int audio_pid = -1;
        int sub_pid = -1;
        bool no_audio = false;
        mp_mutex_lock(&b->overlay_lock);
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (ti && ti->clip_count) {
            const BLURAY_CLIP_INFO *ci = &ti->clips[0];
            no_audio = ci->audio_stream_count == 0;
            if (b->audio_stream_num >= 1 &&
                b->audio_stream_num <= ci->audio_stream_count)
            {
                audio_pid = ci->audio_streams[b->audio_stream_num - 1].pid;
            }
            if (b->sub_stream_num >= 1 &&
                b->sub_stream_num <= ci->pg_stream_count)
            {
                sub_pid = ci->pg_streams[b->sub_stream_num - 1].pid;
            }
        }
        if (!atomic_load(&b->hdmv_mode)) {
            // Plain titles still need jump resync and disc-driven track state.
            *st = (struct stream_nav_state){
                .menu_supported = b->menu_supported,
                .still_active = b->still_active,
                .still_duration = b->still_duration,
                .still_id = b->still_id,
                .discontinuity_id = b->discontinuity_id,
                .transition_pending = b->transition_pending,
                .failed = b->failed,
                .drain_pending = b->resync_owed,
                .no_audio = no_audio,
                .active_audio_id = audio_pid,
                .active_sub_id = sub_pid,
                .sub_visible = b->sub_visible,
                .angle = b->current_angle + 1,
                .num_angles = ti ? ti->angle_count : 0,
            };
            mp_mutex_unlock(&b->overlay_lock);
            return STREAM_OK;
        }
        // HDMV and BD-J interactive graphics use IG. PG graphics still need
        // display, but must not enable menu input or select the menu edition.
        *st = (struct stream_nav_state){
            .nav_active = true,
            .menu_supported = b->menu_supported,
            .menu_active = b->ig.visible,
            .overlay_visible = b->ig.visible || b->pg.visible,
            .still_active = b->still_active,
            .still_duration = b->still_duration,
            .still_id = b->still_id,
            .src_w = MPMAX(b->ig.disp_w, b->pg.disp_w),
            .src_h = MPMAX(b->ig.disp_h, b->pg.disp_h),
            .change_id = b->nav_change_id,
            .discontinuity_id = b->discontinuity_id,
            .transition_pending = b->transition_pending,
            .failed = b->failed,
            .drain_pending = b->resync_owed,
            .no_audio = no_audio,
            .active_audio_id = audio_pid,
            .active_sub_id = sub_pid,
            .sub_visible = b->sub_visible,
            .angle = b->current_angle + 1,
            .num_angles = ti ? ti->angle_count : 0,
        };
        mp_mutex_unlock(&b->overlay_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_OVERLAY: {
        if (!atomic_load(&b->hdmv_mode))
            return STREAM_UNSUPPORTED;
        struct stream_nav_overlay_req *req = arg;
        if (!req->dst || req->w <= 0 || req->h <= 0)
            return STREAM_ERROR;
        mp_mutex_lock(&b->overlay_lock);
        int copy_w = MPMIN(req->w, MPMAX(b->ig.disp_w, b->pg.disp_w));
        int copy_h = MPMIN(req->h, MPMAX(b->ig.disp_h, b->pg.disp_h));
        const uint32_t *ig_src = b->ig.visible ? b->ig.publish : NULL;
        const uint32_t *pg_src = b->pg.visible ? b->pg.publish : NULL;
        for (int y = 0; y < copy_h; y++) {
            uint32_t *dst = (uint32_t *)(req->dst + y * req->stride);
            const uint32_t *ig_row = (ig_src && y < b->ig.h)
                ? ig_src + (size_t)y * b->ig.w : NULL;
            const uint32_t *pg_row = (pg_src && y < b->pg.h)
                ? pg_src + (size_t)y * b->pg.w : NULL;
            int ig_lim = ig_row ? b->ig.w : 0;
            int pg_lim = pg_row ? b->pg.w : 0;
            for (int x = 0; x < copy_w; x++) {
                uint32_t ig = (x < ig_lim) ? ig_row[x] : 0;
                uint32_t pg = (x < pg_lim) ? pg_row[x] : 0;
                uint32_t ia = (ig >> 24) & 0xFF;
                if (ia == 0) {
                    dst[x] = pg;
                } else if (ia == 0xFF || !pg) {
                    dst[x] = ig;
                } else {
                    // out = ig + pg * (1 - ig.a)
                    uint32_t inv = 255 - ia;
                    uint32_t na = ia                  + ((pg >> 24) & 0xFF) * inv / 255;
                    uint32_t nr = ((ig >> 16) & 0xFF) + ((pg >> 16) & 0xFF) * inv / 255;
                    uint32_t ng = ((ig >>  8) & 0xFF) + ((pg >>  8) & 0xFF) * inv / 255;
                    uint32_t nb = ( ig        & 0xFF) + ( pg        & 0xFF) * inv / 255;
                    dst[x] = (MPMIN(na, 255u) << 24) | (MPMIN(nr, 255u) << 16) |
                             (MPMIN(ng, 255u) <<  8) |  MPMIN(nb, 255u);
                }
            }
        }
        req->change_id = b->nav_change_id;
        req->w = copy_w;
        req->h = copy_h;
        mp_mutex_unlock(&b->overlay_lock);
        return STREAM_OK;
    }
    default:
        break;
    }

    return STREAM_UNSUPPORTED;
}

static int bluray_stream_control(stream_t *s, int cmd, void *arg)
{
    struct bluray_priv_s *b = s->priv;
    // These controls only read state guarded by overlay_lock (or immutable
    // title metadata). Keep state polling responsive during ISO reads.
    switch (cmd) {
    case STREAM_CTRL_GET_NUM_CHAPTERS:
    case STREAM_CTRL_GET_CHAPTER_TIME:
    case STREAM_CTRL_GET_CURRENT_TITLE:
    case STREAM_CTRL_GET_NUM_TITLES:
    case STREAM_CTRL_GET_TIME_LENGTH:
    case STREAM_CTRL_GET_NUM_ANGLES:
    case STREAM_CTRL_GET_ANGLE:
    case STREAM_CTRL_GET_LANG:
    case STREAM_CTRL_GET_NAV_STATE:
    case STREAM_CTRL_GET_NAV_OVERLAY:
        return bluray_stream_control_locked(s, cmd, arg);
    }
    mp_mutex_lock(&b->vm_lock);
    int result = bluray_stream_control_locked(s, cmd, arg);
    mp_mutex_unlock(&b->vm_lock);
    return result;
}

static const char *aacs_strerr(int err)
{
    switch (err) {
    case AACS_ERROR_CORRUPTED_DISC: return "opening or reading of AACS files failed";
    case AACS_ERROR_NO_CONFIG:      return "missing config file";
    case AACS_ERROR_NO_PK:          return "no matching processing key";
    case AACS_ERROR_NO_CERT:        return "no valid certificate";
    case AACS_ERROR_CERT_REVOKED:   return "certificate has been revoked";
    case AACS_ERROR_MMC_OPEN:       return "MMC open failed (maybe no MMC drive?)";
    case AACS_ERROR_MMC_FAILURE:    return "MMC failed";
    case AACS_ERROR_NO_DK:          return "no matching device key";
    default:                        return "unknown error";
    }
}

static bool check_disc_info(stream_t *s)
{
    struct bluray_priv_s *b = s->priv;
    const BLURAY_DISC_INFO *info = bd_get_disc_info(b->bd);

    // check Blu-ray
    if (!info->bluray_detected) {
        if (!b->probing)
            MP_ERR(s, "Given stream is not a Blu-ray.\n");
        return false;
    }
    b->probing = false;

    // check AACS
    if (info->aacs_detected) {
        if (!info->libaacs_detected) {
            MP_ERR(s, "AACS encryption detected but cannot find libaacs.\n");
            return false;
        }
        if (!info->aacs_handled) {
            MP_ERR(s, "AACS error: %s\n", aacs_strerr(info->aacs_error_code));
            return false;
        }
    }

    // check BD+
    if (info->bdplus_detected) {
        if (!info->libbdplus_detected) {
            MP_ERR(s, "BD+ encryption detected but cannot find libbdplus.\n");
            return false;
        }
        if (!info->bdplus_handled) {
            MP_ERR(s, "Cannot decrypt BD+ encryption.\n");
            return false;
        }
    }

    return true;
}

static void select_initial_title(stream_t *s, int title_guess) {
    struct bluray_priv_s *b = s->priv;

    if (b->cfg_title == BLURAY_PLAYLIST_TITLE) {
        if (!play_playlist(b, b->cfg_playlist))
            MP_WARN(s, "Couldn't start playlist '%05d'.\n", b->cfg_playlist);
        b->current_title = bd_get_current_title(b->bd);
    } else {
        int title = -1;
        if (b->cfg_title != BLURAY_DEFAULT_TITLE )
            title = b->cfg_title;
        else
            title = title_guess;
        if (title < 0)
            return;

        if (play_title(b, title))
            b->current_title = title;
        else {
            MP_WARN(s, "Couldn't start title '%d'.\n", title);
            b->current_title = bd_get_current_title(b->bd);
        }
    }
}

static int bluray_stream_open_internal(stream_t *s)
{
    struct bluray_priv_s *b = s->priv;

    struct m_config_cache *opts_cache =
        m_config_cache_alloc(s, s->global, &stream_bluray_conf);

    b->opts_cache = opts_cache;
    b->opts = opts_cache->opts;

    mp_mutex_init(&b->overlay_lock);
    mp_mutex_init(&b->iso_stream_lock);
    mp_mutex_init(&b->vm_lock);

    int ret = 0;
    char *device = NULL;
    if (!b->cfg_stream_url || !b->cfg_stream_url[0]) {
        /* find the requested device */
        if (b->cfg_device && b->cfg_device[0]) {
            device = b->cfg_device;
        } else if (b->opts->bluray_device && b->opts->bluray_device[0]) {
            device = b->opts->bluray_device;
        } else {
            device = DEFAULT_OPTICAL_DEVICE;
        }

        if (!device || !device[0]) {
            MP_ERR(s, "No Blu-ray device/location was specified ...\n");
            ret = STREAM_UNSUPPORTED;
            goto err;
        }
    }

    mp_mutex_lock(&bluray_log_lock);
    // libbluray log callback is global and there is no way to separate it per
    // instance, just replace with a new one if already present. The previous
    // log stays alive (it is freed with the stream owning it), so a logging
    // callback racing this replacement never sees a dangling pointer.
    if (bluray_log) {
        MP_WARN(s, "Replacing logger from previous instance.\n");
        bluray_log = NULL;
    }
    uint32_t mask = 0;
    if (mp_msg_test(s->log, MSGL_DEBUG))
        mask |= DBG_CRIT | DBG_BLURAY | DBG_HDMV;
    // When all bits are set (-1) libbluray inits to DBG_CRIT, set all bits,
    // except one, so we avoid this default fallback.
    if (mp_msg_test(s->log, MSGL_TRACE))
        mask |= UINT32_MAX >> 1;
    bd_set_debug_mask(mask);
    if (mask)
        b->bluray_log = bluray_log = mp_log_new(s, s->log, "/libbluray");
    bd_set_debug_handler(bluray_logger);
    mp_mutex_unlock(&bluray_log_lock);

    if (b->cfg_stream_url && b->cfg_stream_url[0]) {
        ret = open_bluray_from_stream(s, b->cfg_stream_url);
        if (ret != STREAM_OK)
            goto err;
    } else {
        /* open device */
        char *device_tmp = mp_get_user_path(NULL, s->global, device);
        BLURAY *bd = bd_open(device_tmp, NULL);
        talloc_free(device_tmp);
        if (!bd) {
            if (!b->probing)
                MP_ERR(s, "Couldn't open Blu-ray device: %s\n", device);
            ret = STREAM_UNSUPPORTED;
            goto err;
        }
        b->bd = bd;
    }

    BLURAY *bd = b->bd;

    if (!check_disc_info(s)) {
        ret = STREAM_UNSUPPORTED;
        goto err;
    }

    const BLURAY_DISC_INFO *info = bd_get_disc_info(bd);

    /* check for available titles on disc */
    int has_interactive_graphics = 0;
    b->num_titles = bd_get_titles_with_menu_info(bd, TITLES_RELEVANT, 0,
                                                &has_interactive_graphics);
    if (!b->num_titles) {
        MP_ERR(s, "Can't find any Blu-ray-compatible title here.\n");
        ret = STREAM_UNSUPPORTED;
        goto err;
    }

    MP_INFO(s, "List of available titles:\n");

    /* parse titles information */
    b->title_to_playlist = talloc_array(b, uint32_t, b->num_titles);
    for (int i = 0; i < b->num_titles; i++) {
        b->title_to_playlist[i] = (uint32_t)-1;
        /* the information we're accessing (duration, playlist, angle count)
         * doesn't depend on the angle */
        BLURAY_TITLE_INFO *ti = bd_get_title_info(bd, i, 0);
        if (!ti)
            continue;

        b->title_to_playlist[i] = ti->playlist;
        char *time = mp_format_time(ti->duration / 90000, false);
        MP_INFO(s, "idx: %3d duration: %s angles: %2d (playlist: %05d.mpls)\n",
                    i, time, ti->angle_count, ti->playlist);
        talloc_free(time);

        bd_free_title_info(ti);
    }

    // these should be set before any callback
    b->current_angle = -1;
    b->current_title = -1;

    // initialize libbluray event queue
    bd_get_event(bd, NULL);

    MP_VERBOSE(s, "First play: %i, Top menu: %i, "
                  "HDMV Titles: %i, BD-J Titles: %i, Other: %i\n",
               info->first_play_supported, info->top_menu_supported,
               info->num_hdmv_titles, info->num_bdj_titles,
               info->num_unsupported_titles);

    b->menu_supported =
        !info->no_menu_support &&
        (info->first_play_supported || info->top_menu_supported) &&
        (has_interactive_graphics || (info->bdj_detected && info->bdj_handled));
    atomic_store(&b->hdmv_mode, b->cfg_title == BLURAY_MENU_TITLE);

    if (atomic_load(&b->hdmv_mode) && !b->menu_supported) {
        MP_WARN(s, "No supported Blu-ray menu. Playing the main title.\n");
        atomic_store(&b->hdmv_mode, false);
        b->cfg_title = BLURAY_DEFAULT_TITLE;
    }

    MP_VERBOSE(s, "bdnav: cfg_title=%d hdmv_mode=%d\n",
               b->cfg_title, atomic_load(&b->hdmv_mode));
    if (atomic_load(&b->hdmv_mode)) {
        if (!play_menu(s)) {
            ret = STREAM_UNSUPPORTED;
            goto err;
        }
        b->current_title = bd_get_current_title(bd);
        MP_VERBOSE(s, "bdnav: HDMV entered; current title=%d\n",
                   b->current_title);
    } else {
        select_initial_title(s, bd_get_main_title(bd));
    }

    // Angle selection is only valid once a playlist has been picked.
    if (!atomic_load(&b->hdmv_mode)) {
        if (!bd_select_angle(bd, b->opts->angle - 1))
            MP_WARN(s, "Couldn't select angle '%d'.\n", b->opts->angle - 1);
    }

    b->current_angle = bd_get_current_angle(bd);

    s->fill_buffer = bluray_stream_fill_buffer;
    s->close       = bluray_stream_close;
    s->control     = bluray_stream_control;
    s->priv        = b;
    s->demuxer     = "+disc";

    MP_VERBOSE(s, "Blu-ray successfully opened.\n");

    return STREAM_OK;

err:
    bluray_stream_close(s);
    return ret;
}

static int bluray_stream_open(stream_t *s)
{
    struct bluray_priv_s *b = talloc_zero(s, struct bluray_priv_s);
    s->priv = b;

    bstr title, bdevice, rest = { .len = 0 };
    bstr_split_tok(bstr0(s->path), "/", &title, &bdevice);

    b->cfg_title = BLURAY_DEFAULT_TITLE;

    struct MPOpts *opts = mp_get_config_group(s, s->global, &mp_opt_root);
    int edition_id = opts->edition_id;
    bool disc_menu = opts->disc_menu;
    talloc_free(opts);

    if (edition_id >= 0) {
        b->cfg_title = edition_id;
    } else if (title.len == 0 || bstr_equals0(title, "longest") ||
               bstr_equals0(title, "first"))
    {
        b->cfg_title = disc_menu ? BLURAY_MENU_TITLE : BLURAY_DEFAULT_TITLE;
    } else if (bstr_equals0(title, "menu")) {
        b->cfg_title = BLURAY_MENU_TITLE;
    } else if (bstr_equals0(title, "mpls")) {
        bstr_split_tok(bdevice, "/", &title, &bdevice);
        long long pl = bstrtoll(title, &rest, 10);
        if (rest.len) {
            MP_ERR(s, "number expected: '%.*s'\n", BSTR_P(rest));
            return STREAM_ERROR;
        } else if (pl < 0 || 99999 < pl) {
            MP_ERR(s, "invalid playlist: '%.*s', must be in the range 0-99999\n",
                            BSTR_P(title));
            return STREAM_ERROR;
        }
        b->cfg_playlist = pl;
        b->cfg_title    = BLURAY_PLAYLIST_TITLE;
    } else if (title.len) {
        long long t = bstrtoll(title, &rest, 10);
        if (rest.len) {
            MP_ERR(s, "number expected: '%.*s'\n", BSTR_P(rest));
            return STREAM_ERROR;
        } else if (t < 0 || 99999 < t) {
            MP_ERR(s, "invalid title: '%.*s', must be in the range 0-99999\n",
                            BSTR_P(title));
            return STREAM_ERROR;
        }
        b->cfg_title = t;
    }

    b->cfg_device = bstrto0(b, bdevice);

    return bluray_stream_open_internal(s);
}

const stream_info_t stream_info_bluray = {
    .name = "bd",
    .open = bluray_stream_open,
    .protocols = (const char*const[]){ "bd", "br", "bluray", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};

int stream_open_bluray_iso(stream_t *stream, const char *url, bool local_path)
{
    struct bluray_priv_s *b = talloc_zero(stream, struct bluray_priv_s);
    stream->priv = b;

    struct MPOpts *opts = mp_get_config_group(stream, stream->global, &mp_opt_root);
    b->cfg_title = opts->edition_id >= 0 ? opts->edition_id
                                         : opts->disc_menu ? BLURAY_MENU_TITLE
                                                           : BLURAY_DEFAULT_TITLE;
    talloc_free(opts);

    if (local_path)
        b->cfg_device = talloc_strdup(b, url);
    else
        b->cfg_stream_url = talloc_strdup(b, url);
    b->probing = true;

    int r = bluray_stream_open_internal(stream);
    if (r != STREAM_OK) {
        talloc_free(b);
        stream->priv = NULL;
        return r;
    }

    stream->info = &stream_info_bdmv_dir;
    return STREAM_OK;
}

static bool check_bdmv(const char *path)
{
    if (strcasecmp(mp_basename(path), "MovieObject.bdmv"))
        return false;

    FILE *temp = fopen(path, "rb");
    if (!temp)
        return false;

    char data[50];
    bool ret = false;

    if (fread(data, 50, 1, temp) == 1) {
        bstr bdata = {data, 50};
        ret = bstr_startswith0(bdata, "MOBJ0100") || // AVCHD
              bstr_startswith0(bdata, "MOBJ0200") || // Blu-ray
              bstr_startswith0(bdata, "MOBJ0300");   // UHD BD
    }

    fclose(temp);
    return ret;
}

// Destructively remove the current trailing path component.
static void remove_prefix(char *path)
{
    size_t len = strlen(path);
#if HAVE_DOS_PATHS
    const char *seps = "/\\";
#else
    const char *seps = "/";
#endif
    while (len > 0 && !strchr(seps, path[len - 1]))
        len--;
    while (len > 0 && strchr(seps, path[len - 1]))
        len--;
    path[len] = '\0';
}

static int bdmv_dir_stream_open(stream_t *stream)
{
    struct bluray_priv_s *priv = talloc_ptrtype(stream, priv);
    stream->priv = priv;
    struct MPOpts *opts = mp_get_config_group(NULL, stream->global, &mp_opt_root);
    int default_title = opts->edition_id >= 0 ? opts->edition_id
                                              : opts->disc_menu ? BLURAY_MENU_TITLE
                                                                : BLURAY_DEFAULT_TITLE;
    *priv = (struct bluray_priv_s){
        .cfg_title = default_title,
    };
    talloc_free(opts);

    if (!stream->access_references)
        goto unsupported;

    char *path = mp_file_get_path(priv, bstr0(stream->url));
    if (!path)
        goto unsupported;

    // Hand the .iso to libbluray as the device. Opening validates the disc
    // info, so it doubles as the probe (see priv->probing).
    if (bstr_case_endswith(bstr0(path), bstr0(".iso"))) {
        priv->cfg_device = talloc_strdup(priv, path);
        priv->probing = stream->autoprobed;
        int r = bluray_stream_open_internal(stream);
        if (r != STREAM_OK) {
            if (priv->probing)
                goto unsupported;
            return r;
        }
        MP_INFO(stream, "Blu-ray ISO image detected. Redirecting to bluray://\n");
        return r;
    }

    // We allow the path to point to a directory containing BDMV/, a
    // directory containing MovieObject.bdmv, or that file itself.
    if (!check_bdmv(path)) {
        // On UNIX, just assume the filename has always this case.
        char *npath = mp_path_join(priv, path, "MovieObject.bdmv");
        if (!check_bdmv(npath)) {
            npath = mp_path_join(priv, path, "BDMV/MovieObject.bdmv");
            if (!check_bdmv(npath))
                goto unsupported;
        }
        path = npath;
    }

    // Go up by 2 levels.
    remove_prefix(path);
    remove_prefix(path);
    priv->cfg_device = path;
    if (strlen(priv->cfg_device) <= 1)
        goto unsupported;

    MP_INFO(stream, "BDMV detected. Redirecting to bluray://\n");
    return bluray_stream_open_internal(stream);

unsupported:
    talloc_free(priv);
    stream->priv = NULL;
    return STREAM_UNSUPPORTED;
}

const stream_info_t stream_info_bdmv_dir = {
    .name = "bdmv/bluray",
    .open = bdmv_dir_stream_open,
    .protocols = (const char*const[]){ "file", "", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};
