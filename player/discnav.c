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

#include <string.h>
#include <stdbool.h>

#include "mpv_talloc.h"

#include "audio/out/ao.h"
#include "common/common.h"
#include "common/msg.h"
#include "input/input.h"
#include "osdep/timer.h"
#include "player/command.h"

#include "stream/stream.h"
#include "demux/demux.h"
#include "sub/dec_sub.h"
#include "sub/osd.h"
#include "sub/osd_state.h"
#include "video/mp_image.h"
#include "video/out/vo.h"

#include "core.h"

struct disc_nav_state {
    // Track Blu-ray OSD graphics separately from the menu input section.
    bool overlay_visible;
    bool menu_input_enabled;

    // Blu-ray graphics staging. The stream supplies composited BGRA PG/IG
    // plane via STREAM_CTRL_GET_NAV_OVERLAY; we copy it into bd_image and
    // forward through osd_set_bitmaps (OSDTYPE_DISC_MENU).
    struct mp_image *bd_image;
    uint32_t bd_last_change_id;
    struct mp_osd_res bd_last_vo_res;

    // Last menu-overlay change_id we forced an OSD redraw for.
    uint32_t last_overlay_change_id;
    bool overlay_change_seen;

    bool drain_was_pending; // a boundary has already been queued for resync
    uint32_t last_drain_disc_id;

    // DVD-only: dvd_subtitle track we force-selected for the menu, and the
    // selection it displaced (restored on menu close).
    struct track *menu_selected_track;
    struct track *menu_saved_track;

    // Last disc-driven audio/sub/angle we acted on.
    int last_audio_id;
    int last_sub_id;
    bool last_sub_visible;
    int last_angle;
    bool track_sync_seen;
    // Last disc-discontinuity id we acted on for track sync.
    uint32_t last_track_disc_id;

    // Last audio id we logged as "not in tracks yet" (avoids log spam).
    int last_missing_audio_id;

    // Last navigation state pushed through property-change notification.
    bool last_navigation_active;
    bool navigation_active_seen;
    bool last_menu_active;
    bool menu_active_seen;
    bool last_menu_supported;
    bool menu_supported_seen;

    // DVD/BD finite-still timer. It starts only after decoders have drained so
    // the authored duration applies to the actually displayed last frame.
    bool timed_still_seen;
    uint32_t timed_still_id;
    double timed_still_remaining;
    double timed_still_last_tick;
};

static bool is_dvd_sub_track(struct track *t)
{
    return t && t->type == STREAM_SUB && t->stream &&
           (!t->stream->dvd_nav_stream || atomic_load(&t->stream->dvd_nav_generation)) &&
           t->stream->codec &&
           t->stream->codec->codec &&
           strcmp(t->stream->codec->codec, "dvd_subtitle") == 0;
}

static struct disc_nav_state *get_state(struct MPContext *mpctx)
{
    if (!mpctx->disc_nav)
        mpctx->disc_nav = talloc_zero(mpctx, struct disc_nav_state);
    return mpctx->disc_nav;
}

// Forget everything tied to the previous playlist entry. Stale state (e.g.
// another disc's discontinuity counter) must not leak into the next file.
void disc_nav_reset(struct MPContext *mpctx)
{
    struct disc_nav_state *st = mpctx->disc_nav;
    if (!st)
        return;
    if (st->overlay_visible)
        osd_set_bitmaps(mpctx->osd, OSDTYPE_DISC_MENU, NULL);
    if (st->menu_input_enabled)
        mp_input_disable_section(mpctx->input, "discnav");
    mp_image_unrefp(&st->bd_image);
    *st = (struct disc_nav_state){0};
}

void disc_nav_destroy(struct MPContext *mpctx)
{
    if (!mpctx->disc_nav)
        return;
    mp_image_unrefp(&mpctx->disc_nav->bd_image);
    TA_FREEP(&mpctx->disc_nav);
}

struct stream *disc_nav_get_stream(struct MPContext *mpctx)
{
    if (!mpctx->demuxer || !mpctx->demuxer->stream)
        return NULL;
    struct stream *s = mpctx->demuxer->stream;
    if (!s->info || !s->info->name)
        return NULL;
    const char *n = s->info->name;
    if (strcmp(n, "dvdnav") == 0 || strcmp(n, "ifo_dvdnav") == 0 ||
        strcmp(n, "bd") == 0 || strcmp(n, "bdmv/bluray") == 0)
    {
        return s;
    }
    return NULL;
}

bool disc_nav_prevents_eof(struct MPContext *mpctx)
{
    struct stream *s = disc_nav_get_stream(mpctx);
    struct stream_nav_state nav = {0};
    return s && stream_control(s, STREAM_CTRL_GET_NAV_STATE, &nav) >= 1 &&
           (nav.still_active || nav.transition_pending || nav.drain_pending || nav.failed);
}

bool disc_nav_mouse_pos_to_src(struct MPContext *mpctx, int src_w, int src_h,
                               int *out_x, int *out_y)
{
    struct vo *vo = mpctx->video_out;
    if (!vo || !vo->config_ok || src_w <= 0 || src_h <= 0)
        return false;
    int wx, wy, hover;
    mp_input_get_mouse_pos(mpctx->input, &wx, &wy, &hover);
    struct mp_rect src, dst;
    struct mp_osd_res osd; // mandatory out param; ignored
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    int dw = dst.x1 - dst.x0;
    int dh = dst.y1 - dst.y0;
    if (dw <= 0 || dh <= 0)
        return false;
    double fx = (wx - dst.x0) / (double)dw;
    double fy = (wy - dst.y0) / (double)dh;
    if (fx < 0 || fx > 1 || fy < 0 || fy > 1)
        return false;
    *out_x = MPMIN((int)(fx * src_w), src_w - 1);
    *out_y = MPMIN((int)(fy * src_h), src_h - 1);
    return true;
}

// Push the current menu/highlight state to the dvd_subtitle decoders.
static void push_dvd_overlay(struct MPContext *mpctx, struct stream_nav_state *nav)
{
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *t = mpctx->tracks[n];
        if (!is_dvd_sub_track(t) || !t->d_sub)
            continue;
        sub_control(t->d_sub, SD_CTRL_APPLY_DVDNAV, nav);
    }
}

static void push_bd_overlay(struct MPContext *mpctx, struct stream *s,
                            struct stream_nav_state *nav, bool visible)
{
    struct disc_nav_state *st = get_state(mpctx);

    if (!visible) {
        if (st->overlay_visible)
            osd_set_bitmaps(mpctx->osd, OSDTYPE_DISC_MENU, NULL);
        st->overlay_visible = false;
        st->bd_last_change_id = 0;
        st->bd_last_vo_res = (struct mp_osd_res){0};
        return;
    }

    if (nav->src_w <= 0 || nav->src_h <= 0)
        return;

    // Skip the work when nothing the renderer cares about changed.
    struct mp_osd_res vo_res = osd_get_vo_res(mpctx->osd);
    if (st->overlay_visible &&
        st->bd_last_change_id == nav->change_id &&
        osd_res_equals(vo_res, st->bd_last_vo_res))
        return;

    if (!st->bd_image ||
        st->bd_image->w != nav->src_w || st->bd_image->h != nav->src_h)
    {
        mp_image_unrefp(&st->bd_image);
        st->bd_image = mp_image_alloc(IMGFMT_BGRA, nav->src_w, nav->src_h);
        if (!st->bd_image)
            return;
        talloc_steal(st, st->bd_image);
    }

    // The OSD/VO may still hold references to the previous frame's pixels.
    if (!mp_image_make_writeable(st->bd_image))
        return;

    struct mp_image *img = st->bd_image;
    struct stream_nav_overlay_req req = {
        .w      = img->w,
        .h      = img->h,
        .stride = img->stride[0],
        .dst    = img->planes[0],
    };
    if (stream_control(s, STREAM_CTRL_GET_NAV_OVERLAY, &req) < 1)
        return;

    struct sub_bitmap part = {
        .bitmap = img->planes[0],
        .stride = img->stride[0],
        .w  = req.w,
        .h  = req.h,
        .dw = req.w,
        .dh = req.h,
    };
    struct sub_bitmaps imgs = {
        .format    = SUBBITMAP_BGRA,
        .parts     = &part,
        .num_parts = 1,
        .packed    = img,
        .packed_w  = img->w,
        .packed_h  = img->h,
        .change_id = nav->change_id ? (int)nav->change_id : 1,
        .video_color_space = true,
    };
    osd_rescale_bitmaps(&imgs, nav->src_w, nav->src_h, vo_res, 0);
    osd_set_bitmaps(mpctx->osd, OSDTYPE_DISC_MENU, &imgs);
    st->overlay_visible = true;
    st->bd_last_change_id = nav->change_id;
    st->bd_last_vo_res = vo_res;
}

// Sync demuxer->edition with what the disc is actually playing.
static void sync_current_edition(struct MPContext *mpctx, struct stream *s,
                                 struct stream_nav_state *nav)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer || demuxer->num_editions <= 0 || !demuxer->desc ||
        strcmp(demuxer->desc->name, "disc") != 0)
        return;

    unsigned num_titles;
    if (stream_control(s, STREAM_CTRL_GET_NUM_TITLES, &num_titles) < 1)
        return;
    bool has_menu_edition = demuxer->num_editions == num_titles + 1;
    int desired = demuxer->edition;
    if (nav->menu_active && has_menu_edition) {
        desired = num_titles;
    } else {
        unsigned title;
        if (stream_control(s, STREAM_CTRL_GET_CURRENT_TITLE, &title) >= 1 &&
            title < num_titles && title < (unsigned)demuxer->num_editions)
        {
            desired = title;
        }
    }
    if (desired >= demuxer->num_editions)
        desired = 0;
    if (desired != demuxer->edition) {
        MP_VERBOSE(s, "current-edition %d->%d "
                   "(menu_active=%d)\n",
                   demuxer->edition, desired, nav->menu_active);
        demuxer->edition = desired;
        mp_notify_property(mpctx, "current-edition");
    }
}

// Catch playlist/title jumps driven by the disc itself (menu buttons, HDMV
// bytecode, end of title, dvdnav HOP_CHANNEL, etc.).
static bool check_async_discontinuity(struct MPContext *mpctx, struct stream *s,
                                      struct stream_nav_state *nav, bool is_dvd)
{
    struct disc_nav_state *st = get_state(mpctx);
    if (!mpctx->demuxer)
        return false;

    if (!nav->drain_pending) {
        st->drain_was_pending = false;
        return false;
    }
    if (st->drain_was_pending &&
        st->last_drain_disc_id == nav->discontinuity_id)
        return false;

    // An explicit seek owns the next VM position, including a track refresh.
    if (mpctx->seek.type)
        return true;

    bool playback_drained = mpctx->video_status == STATUS_EOF &&
                            mpctx->audio_status == STATUS_EOF &&
                            (!mpctx->ao || !ao_is_playing(mpctx->ao));
    bool immediate = nav->drain_immediate ||
                     (nav->drain_user_activation && mpctx->opts->pause);
    if (is_dvd && !immediate && !playback_drained) {
        // The demux thread can reach an authored end-of-title jump while the
        // player still has packets queued. Keep presenting the old domain
        // until those packets drain instead of flushing the title early.
        mp_set_timeout(mpctx, 0.05);
        return true;
    }
    st->drain_was_pending = true;
    st->last_drain_disc_id = nav->discontinuity_id;

    // The disc jumped on its own, resync through the regular seek path.
    // demux_disc releases the held boundary instead of repositioning the VM.
    double t = 0;
    if (stream_control(s, STREAM_CTRL_GET_CURRENT_TIME, &t) < 1)
        t = 0;
    MP_VERBOSE(s, "disc jumped (id %u), resync seek to %f\n",
               nav->discontinuity_id, t);
    queue_seek(mpctx, MPSEEK_ABSOLUTE, t, MPSEEK_KEYFRAME, MPSEEK_FLAG_NAV);
    return false;
}

// Only explicit primary track requests write the DVD VM. Internal navigation
// synchronization calls mp_switch_track_n without FLAG_MARK_SELECTION.
bool disc_nav_select_track(struct MPContext *mpctx, int order,
                            enum stream_type type, struct track *track)
{
    if (order || (type != STREAM_AUDIO && type != STREAM_SUB))
        return true;
    struct sh_stream *sh = track ? track->stream : NULL;
    struct stream *s = disc_nav_get_stream(mpctx);
    if (!s)
        return !sh || !sh->dvd_nav_stream;
    struct stream_nav_state nav = {0};
    if (stream_control(s, STREAM_CTRL_GET_NAV_STATE, &nav) < 1 || !nav.dvd_generation)
        return !sh || !sh->dvd_nav_stream;
    if (track && (!sh || !sh->dvd_nav_stream))
        return true;
    if (!track && type == STREAM_AUDIO)
        return true;
    struct stream_dvd_select select = {
        .generation = sh ? atomic_load(&sh->dvd_nav_generation) : nav.dvd_generation,
        .type = type,
        .logical = sh ? sh->dvd_nav_logical : -1,
    };
    if (stream_control(s, STREAM_CTRL_SET_DVD_STREAM, &select) != STREAM_OK) {
        MP_WARN(mpctx, "DVD rejected track selection from an inactive navigation epoch.\n");
        return false;
    }
    // Disc demuxers cannot use the generic packet-cache refresh. Refill the
    // selected track at the playback position through the normal seek owner.
    if (mpctx->playback_initialized && track && !nav.menu_active &&
        track != mpctx->current_track[0][type] && !mpctx->seek.type)
        queue_seek(mpctx, MPSEEK_ABSOLUTE, get_current_time(mpctx), MPSEEK_EXACT, 0);
    return true;
}

static struct track *find_dvd_track(struct MPContext *mpctx, enum stream_type type,
                                    int logical, uint64_t generation)
{
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        struct sh_stream *sh = track->stream;
        if (track->type == type && sh && sh->dvd_nav_stream &&
            sh->dvd_nav_logical == logical &&
            atomic_load(&sh->dvd_nav_generation) == generation)
            return track;
    }
    return NULL;
}

static void sync_dvd_track_selection(struct MPContext *mpctx,
                                     struct stream_nav_state *nav)
{
    struct disc_nav_state *st = get_state(mpctx);
    // The VM is authoritative after a user request has been accepted too:
    // authored commands may select another logical alias on the same PES.
    for (int type = 0; type < STREAM_TYPE_COUNT; type++) {
        if (type != STREAM_AUDIO && type != STREAM_SUB)
            continue;
        if (mpctx->opts->stream_id[0][type] == -2 ||
            (type == STREAM_SUB && (nav->menu_active || st->menu_selected_track)))
            continue;
        struct track *current = mpctx->current_track[0][type];
        // Explicit external tracks stay outside DVD navigation ownership.
        if (current && (!current->stream || !current->stream->dvd_nav_stream))
            continue;
        int logical = type == STREAM_AUDIO ? nav->active_audio_logical : nav->active_sub_logical;
        bool off = type == STREAM_AUDIO ? nav->no_audio : !nav->sub_visible;
        struct track *track = off ? NULL : find_dvd_track(mpctx, type, logical, nav->dvd_generation);
        if ((off || track) && current != track)
            mp_switch_track_n(mpctx, 0, type, track, 0);
    }
}

static struct track *find_track_by_demuxer_id(struct MPContext *mpctx,
                                              enum stream_type type, int id)
{
    if (id < 0)
        return NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *t = mpctx->tracks[n];
        if (t->type == type && t->stream && t->stream->demuxer_id == id)
            return t;
    }
    return NULL;
}

static void sync_disc_track_selection(struct MPContext *mpctx, struct stream *s,
                                      struct stream_nav_state *nav)
{
    struct disc_nav_state *st = get_state(mpctx);
    if (nav->dvd_generation) {
        sync_dvd_track_selection(mpctx, nav);
        if (st->last_angle != nav->angle) {
            st->last_angle = nav->angle;
            mp_notify_property(mpctx, "angle");
        }
        return;
    }
    bool first = !st->track_sync_seen;
    bool hopped = nav->discontinuity_id != st->last_track_disc_id;
    st->last_track_disc_id = nav->discontinuity_id;
    st->track_sync_seen = true;

    if (!first && hopped && nav->no_audio &&
        mpctx->opts->stream_id[0][STREAM_AUDIO] != -2)
    {
        // The disc hopped into a domain without any audio stream. Deselect audio.
        if (mpctx->current_track[0][STREAM_AUDIO])
            mp_switch_track_n(mpctx, 0, STREAM_AUDIO, NULL, 0);
        st->last_audio_id = -1;
    } else if (!first && (hopped || nav->active_audio_id != st->last_audio_id) &&
        mpctx->opts->stream_id[0][STREAM_AUDIO] == -1)
    {
        struct track *t = find_track_by_demuxer_id(mpctx, STREAM_AUDIO,
                                                  nav->active_audio_id);
        if (t) {
            if (mpctx->current_track[0][STREAM_AUDIO] != t) {
                MP_VERBOSE(s, "disc audio -> demuxer_id 0x%x\n",
                           nav->active_audio_id);
                mp_switch_track_n(mpctx, 0, STREAM_AUDIO, t, 0);
            }
            st->last_audio_id = nav->active_audio_id;
        } else if (nav->active_audio_id >= 0) {
            if (st->last_missing_audio_id != nav->active_audio_id) {
                st->last_missing_audio_id = nav->active_audio_id;
                MP_VERBOSE(s, "disc audio 0x%x not in tracks yet\n",
                           nav->active_audio_id);
            }
        }
    } else if (first) {
        st->last_audio_id = nav->active_audio_id;
        if (mpctx->opts->stream_id[0][STREAM_AUDIO] == -1) {
            struct track *t = find_track_by_demuxer_id(mpctx, STREAM_AUDIO,
                                                       nav->active_audio_id);
            if (t && mpctx->current_track[0][STREAM_AUDIO] != t) {
                MP_VERBOSE(s, "initial disc audio -> demuxer_id 0x%x\n",
                           nav->active_audio_id);
                mp_switch_track_n(mpctx, 0, STREAM_AUDIO, t, 0);
            }
        }
    }

    if (!first && (hopped ||
                   nav->active_sub_id != st->last_sub_id ||
                   nav->sub_visible != st->last_sub_visible) &&
        !nav->menu_active && !st->menu_selected_track &&
        mpctx->opts->stream_id[0][STREAM_SUB] == -1)
    {
        if (nav->sub_visible) {
            struct track *t = find_track_by_demuxer_id(mpctx, STREAM_SUB,
                                                      nav->active_sub_id);
            if (t) {
                if (mpctx->current_track[0][STREAM_SUB] != t) {
                    MP_VERBOSE(s, "disc sub -> demuxer_id 0x%x\n",
                               nav->active_sub_id);
                    mp_switch_track_n(mpctx, 0, STREAM_SUB, t, 0);
                }
                st->last_sub_id = nav->active_sub_id;
                st->last_sub_visible = true;
            } else if (nav->active_sub_id >= 0) {
                MP_TRACE(s, "disc sub 0x%x not in tracks yet\n",
                         nav->active_sub_id);
            }
        } else {
            if (mpctx->current_track[0][STREAM_SUB]) {
                MP_VERBOSE(s, "disc sub -> off\n");
                mp_switch_track_n(mpctx, 0, STREAM_SUB, NULL, 0);
            }
            st->last_sub_id = nav->active_sub_id;
            st->last_sub_visible = false;
        }
    } else if (first) {
        st->last_sub_id = nav->active_sub_id;
        st->last_sub_visible = nav->sub_visible;
    }

    if (first) {
        st->last_angle = nav->angle;
    } else if (nav->angle > 0 && nav->angle != st->last_angle) {
        MP_VERBOSE(s, "disc angle %d -> %d (of %d)\n",
                   st->last_angle, nav->angle, nav->num_angles);
        st->last_angle = nav->angle;
        mp_notify_property(mpctx, "angle");
    }
}

// Make sure a dvd_subtitle track is selected while a menu is visible,
// so the SPU graphics decoded by sd_lavc reach the OSD.
static void ensure_menu_sub_selection(struct MPContext *mpctx, bool menu_on)
{
    struct disc_nav_state *st = get_state(mpctx);
    struct track *cur = mpctx->current_track[0][STREAM_SUB];

    // If the track list got rebuilt under us (e.g. another file/disc loaded
    // mid-session) the cached pointers could be stale. Trust only what we
    // can still see in mpctx->tracks.
    for (int i = 0; i < 2; i++) {
        struct track **slot = i ? &st->menu_saved_track : &st->menu_selected_track;
        if (!*slot)
            continue;
        bool still_present = false;
        for (int n = 0; n < mpctx->num_tracks; n++) {
            if (mpctx->tracks[n] == *slot) {
                still_present = true;
                break;
            }
        }
        if (!still_present)
            *slot = NULL;
    }

    if (menu_on) {
        // Re-checked every frame; other selectors (stream auto-select, slave
        // reopens) can change the sub under us.
        if (st->menu_selected_track && cur == st->menu_selected_track && is_dvd_sub_track(cur))
            return;
        // A dvd_subtitle track is already active; nothing to force.
        if (is_dvd_sub_track(cur))
            return;
        if (mpctx->opts->stream_id[0][STREAM_SUB] == -2)
            return;
        struct track *pick = NULL;
        for (int n = 0; n < mpctx->num_tracks; n++) {
            if (is_dvd_sub_track(mpctx->tracks[n])) {
                pick = mpctx->tracks[n];
                break;
            }
        }
        if (!pick)
            return;
        // Remember the displaced selection only on the first override.
        if (!st->menu_selected_track)
            st->menu_saved_track = cur;
        mp_switch_track_n(mpctx, 0, STREAM_SUB, pick, 0);
        st->menu_selected_track = pick;
    } else {
        if (!st->menu_selected_track)
            return;
        // Only revert if our override is still the active selection.
        if (cur == st->menu_selected_track) {
            struct track *saved = st->menu_saved_track;
            if (saved && saved->stream && saved->stream->dvd_nav_stream &&
                !atomic_load(&saved->stream->dvd_nav_generation))
                saved = NULL;
            mp_switch_track_n(mpctx, 0, STREAM_SUB, saved, 0);
        }
        st->menu_selected_track = NULL;
        st->menu_saved_track = NULL;
    }
}

static bool handle_nav_failure(struct MPContext *mpctx,
                               const struct stream_nav_state *nav)
{
    if (!nav->failed)
        return false;
    // A player-thread VM continuation can fail after the demuxer latched EOF.
    // Wake its owner to publish that failure through the demuxer error path.
    if (mpctx->demuxer)
        demux_drive_nav(mpctx->demuxer);
    mpctx->disc_nav_still_frame = false;
    return true;
}

void disc_nav_update(struct MPContext *mpctx)
{
    struct stream *s = disc_nav_get_stream(mpctx);
    if (!s && !mpctx->disc_nav)
        return;
    struct disc_nav_state *st = get_state(mpctx);
    struct stream_nav_state nav = {0};
    bool have = s && stream_control(s, STREAM_CTRL_GET_NAV_STATE, &nav) >= 1;
    bool is_dvd = have && (strcmp(s->info->name, "dvdnav") == 0 ||
                           strcmp(s->info->name, "ifo_dvdnav") == 0);

    if (have && handle_nav_failure(mpctx, &nav))
        return;

    if (have && check_async_discontinuity(mpctx, s, &nav, is_dvd))
        return;

    if (!have || !nav.still_active || nav.still_duration <= 0) {
        st->timed_still_seen = false;
        st->timed_still_last_tick = 0;
    } else {
        if (!st->timed_still_seen || st->timed_still_id != nav.still_id) {
            st->timed_still_seen = true;
            st->timed_still_id = nav.still_id;
            st->timed_still_remaining = nav.still_duration;
            st->timed_still_last_tick = 0;
        }

        bool drained = mpctx->video_status == STATUS_EOF &&
                       mpctx->audio_status == STATUS_EOF &&
                       (!mpctx->ao || !ao_is_playing(mpctx->ao));
        double now = mp_time_sec();
        if (!drained) {
            st->timed_still_last_tick = 0;
            mp_set_timeout(mpctx, 0.05);
        } else if (mpctx->paused) {
            if (st->timed_still_last_tick > 0)
                st->timed_still_remaining -= now - st->timed_still_last_tick;
            st->timed_still_last_tick = 0;
        } else {
            if (st->timed_still_last_tick > 0)
                st->timed_still_remaining -= now - st->timed_still_last_tick;
            st->timed_still_last_tick = now;

            if (st->timed_still_remaining <= 0) {
                if (!mpctx->seek.type) {
                    struct stream_nav_still_skip req = {.id = nav.still_id};
                    if (stream_control(s, STREAM_CTRL_NAV_STILL_SKIP, &req) >= 1) {
                        st->timed_still_seen = false;
                        st->timed_still_last_tick = 0;
                    }
                    have = stream_control(s, STREAM_CTRL_GET_NAV_STATE, &nav) >= 1;
                    if (have && handle_nav_failure(mpctx, &nav))
                        return;
                }
            } else {
                mp_set_timeout(mpctx, st->timed_still_remaining);
            }
        }
    }

    bool still = have && nav.still_active;
    if (s && still != mpctx->disc_nav_still_frame)
        MP_VERBOSE(s, "still_frame %d->%d\n",
                   mpctx->disc_nav_still_frame, still);
    mpctx->disc_nav_still_frame = still;

    // The BD-J VM progresses only on reads, pump them while playback idles
    // at EOF or a held still, per libbluray's requirement. Our core wakes up
    // at 1s intervals even at pause, but that's not enough for fluid button
    // navigation. So, we always drive the VM at least at 20Hz.
    if (have && mpctx->demuxer) {
        bool bd_idle = (strcmp(s->info->name, "bd") == 0 ||
                        strcmp(s->info->name, "bdmv/bluray") == 0) &&
                       (still || (mpctx->video_status == STATUS_EOF &&
                                  mpctx->audio_status == STATUS_EOF));
        if (bd_idle || (nav.menu_active && mpctx->paused)) {
            if (bd_idle)
                demux_drive_nav(mpctx->demuxer);
            mp_set_timeout(mpctx, 0.05);
        }
    }

    if (!have) {
        if (st->overlay_visible) {
            osd_set_bitmaps(mpctx->osd, OSDTYPE_DISC_MENU, NULL);
            st->overlay_visible = false;
        }
        if (st->menu_input_enabled) {
            mp_input_disable_section(mpctx->input, "discnav");
            st->menu_input_enabled = false;
        }
        st->bd_last_change_id = 0;
        st->bd_last_vo_res = (struct mp_osd_res){0};
        st->menu_selected_track = NULL;
        st->menu_saved_track = NULL;
        st->overlay_change_seen = false;
        if (st->navigation_active_seen) {
            st->navigation_active_seen = false;
            st->last_navigation_active = false;
            mp_notify_property(mpctx, "disc-navigation-active");
        }
        if (st->menu_active_seen && st->last_menu_active) {
            st->last_menu_active = false;
            mp_notify_property(mpctx, "disc-menu-active");
        }
        if (st->menu_supported_seen) {
            st->menu_supported_seen = false;
            st->last_menu_supported = false;
            mp_notify_property(mpctx, "disc-menu-supported");
        }
        return;
    }

    if (!st->navigation_active_seen || nav.nav_active != st->last_navigation_active) {
        st->navigation_active_seen = true;
        st->last_navigation_active = nav.nav_active;
        mp_notify_property(mpctx, "disc-navigation-active");
    }
    if (!st->menu_active_seen || nav.menu_active != st->last_menu_active) {
        st->menu_active_seen = true;
        st->last_menu_active = nav.menu_active;
        mp_notify_property(mpctx, "disc-menu-active");
    }
    if (!st->menu_supported_seen || nav.menu_supported != st->last_menu_supported) {
        st->menu_supported_seen = true;
        st->last_menu_supported = nav.menu_supported;
        mp_notify_property(mpctx, "disc-menu-supported");
    }

    sync_current_edition(mpctx, s, &nav);
    sync_disc_track_selection(mpctx, s, &nav);

    bool is_bd = strcmp(s->info->name, "bd") == 0 || strcmp(s->info->name, "bdmv/bluray") == 0;
    bool visible = nav.menu_active &&
                   (is_bd || (mp_rect_w(nav.hl.rect) > 0 && mp_rect_h(nav.hl.rect) > 0));

    if (is_bd) {
        push_bd_overlay(mpctx, s, &nav, nav.overlay_visible);
    } else {
        push_dvd_overlay(mpctx, &nav);
        ensure_menu_sub_selection(mpctx, nav.menu_active);
    }

    if (!st->overlay_change_seen || nav.change_id != st->last_overlay_change_id) {
        st->overlay_change_seen = true;
        st->last_overlay_change_id = nav.change_id;
        osd_changed(mpctx->osd);
    }

    if (visible != st->menu_input_enabled) {
        MP_VERBOSE(s, "menu input %s\n", visible ? "on" : "off");
        if (visible) {
            mp_input_enable_section(mpctx->input, "discnav",
                MP_INPUT_ON_TOP | MP_INPUT_ALLOW_VO_DRAGGING | MP_INPUT_ALLOW_HIDE_CURSOR);
        } else {
            mp_input_disable_section(mpctx->input, "discnav");
        }
        st->menu_input_enabled = visible;
    }
}
