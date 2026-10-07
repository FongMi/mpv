/* Standalone driver fixture for the production VO scheduling/reset functions.
 * The fake Surface stores only buffers passed through draw_frame + flip_page.
 */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define HAVE_ANDROID 1
#define VO_CAP_NORETAIN (1 << 0)
#define VO_CAP_NORETAIN_REDRAW (1 << 1)
#define VO_CAP_UNTIMED (1 << 2)
#define VO_CAP_FRAMEDROP (1 << 3)
#define VO_CAP_FRAMEOWNER (1 << 4)
#define VO_MAX_REQ_FRAMES 8
#define IMGFMT_START 1
#define IMGFMT_END 3
#define IMGFMT_RGBA 1
#define STREAM_VIDEO 0
#define STATUS_EOF 1
#define MPV_EVENT_VIDEO_RECONFIG 1
#define MP_TIME_S_TO_NS(x) ((int64_t)((x) * 1000000000))
#define MP_TIME_MS_TO_NS(x) ((int64_t)((x) * 1000000))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define mp_assert assert
#define talloc_free free
#define MP_STATS(vo, msg) ((void)(vo))
#define MP_VERBOSE(vo, ...) ((void)(vo))
#define MP_FATAL(vo, ...) ((void)(vo))
#define talloc_dup(ctx, value) copy_params(value)

struct mp_image_params {
    int imgfmt, w, h, p_w, p_h, color;
    bool force_window;
};
struct mp_image { int channel; struct mp_image_params params; };
struct vo_frame {
    struct mp_image *current;
    int num_vsyncs;
    bool display_synced, can_drop, repeat, redraw, still;
    int64_t pts, duration;
    double vsync_offset, vsync_interval, ideal_frame_vsync;
    double ideal_frame_vsync_duration;
    uint64_t frame_id;
};
struct mp_vo_opts {
    int64_t WinID;
    double timing_offset;
    bool android_keep_video_frame;
};
struct vo_internal {
    int lock, wakeup;
    bool hasframe, hasframe_rendered, has_video_frame, request_redraw;
    bool dropped_frame, paused, expecting_vsync, rendering, visible;
    int64_t video_wid, drop_count, delayed_count, prev_vsync;
    int64_t flip_queue_offset, wakeup_pts;
    uint64_t current_frame_id;
    struct vo_frame *current_frame, *frame_queued;
    bool send_reset;
    void *stats;
    uint64_t timing_offset;
};
struct vo;
struct vo_vsync_info {
    int64_t last_queue_display_time, skipped_vsyncs;
};
struct vo_driver {
    int caps;
    bool (*draw_frame)(struct vo *, struct vo_frame *);
    void (*flip_page)(struct vo *);
    void (*get_vsync)(struct vo *, struct vo_vsync_info *);
    int (*reconfig)(struct vo *, struct mp_image_params *);
    int (*reconfig2)(struct vo *, struct mp_image *);
};
struct vo {
    struct vo_internal *in;
    struct mp_vo_opts *opts;
    struct vo_driver *driver;
    bool config_ok;
    bool has_peak_detect_values;
    int params_mutex, dwidth, dheight;
    struct mp_image_params *params;
};
struct track { int unused; };
struct MPOpts { struct mp_vo_opts *vo; int force_vo; };
struct vo_extra {
    void *input_ctx, *osd, *encode_lavc_ctx;
    void (*wakeup_cb)(void *);
    void *wakeup_ctx;
};
struct MPContext {
    int stop_play, video_status;
    bool playback_initialized, restart_complete, mouse_cursor_visible;
    void *vo_chain, *input, *osd, *encode_lavc_ctx, *global, *mconfig;
    struct MPOpts *opts;
    struct vo *video_out;
    struct track *current_track[1][1];
};

static int submitted_channel, next_channel, draws, flips, reconfigs, destroys;
static bool draw_ok = true, reset_during_flip;
static struct vo *cold_vo;
static struct vo_frame *owned_frame;
static const int pl_color_space_srgb = 1;

static void mp_mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mp_mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static void mp_cond_broadcast(int *cond) { (void)cond; }
static int64_t mp_time_ns(void) { return 1000000000; }
static void wakeup_locked(struct vo *vo) { assert(vo->in->lock); }
static void wakeup_core(struct vo *vo) { (void)vo; }
static void update_display_fps(struct vo *vo) { (void)vo; }
static void check_vo_caps(struct vo *vo) { (void)vo; }
static void reset_vsync_timings(struct vo *vo) { assert(vo->in->lock); }
static void stats_time_start(void *stats, const char *name) { (void)stats; (void)name; }
static void stats_time_end(void *stats, const char *name) { (void)stats; (void)name; }
static void wait_until(struct vo *vo, int64_t target) { (void)target; assert(!vo->in->lock); }
static void update_vsync_timing_after_swap(struct vo *vo, struct vo_vsync_info *vsync)
{
    assert(vo->in->lock);
    vo->in->prev_vsync = vsync->last_queue_display_time;
}
static struct vo_frame *vo_frame_ref(struct vo_frame *frame)
{
    if (!frame)
        return NULL;
    struct vo_frame *ref = malloc(sizeof(*ref));
    *ref = *frame;
    return ref;
}
static struct mp_image_params *copy_params(struct mp_image_params *params)
{
    struct mp_image_params *copy = malloc(sizeof(*copy));
    *copy = *params;
    return copy;
}
static void mp_image_params_get_dsize(struct mp_image_params *params, int *w, int *h)
{
    *w = params->w;
    *h = params->h;
}
static void mp_wakeup_core_cb(void *ctx) { (void)ctx; }
static bool direct_requested, direct_ready;
static bool wants_android_direct_output(struct MPContext *ctx, struct track *track)
{
    (void)ctx;
    assert(!track);
    return direct_requested;
}
static bool should_use_android_direct_output(struct MPContext *ctx, struct track *track)
{
    (void)ctx;
    assert(!track);
    return direct_ready;
}
static struct vo *init_best_video_out(void *global, struct vo_extra *extra)
{
    (void)global; (void)extra;
    return cold_vo;
}
static void uninit_video_out(struct MPContext *ctx) { ctx->video_out = NULL; destroys++; }
static void vo_query_formats(struct vo *vo, uint8_t *formats)
{
    (void)vo;
    formats[IMGFMT_RGBA - IMGFMT_START] = 1;
}
static void mp_image_params_guess_csp(struct mp_image_params *params) { (void)params; }
static void update_window_title(struct MPContext *ctx, bool force) { (void)ctx; (void)force; }
static void update_content_type(struct MPContext *ctx, struct track *track) { (void)ctx; (void)track; }
static void update_screensaver_state(struct MPContext *ctx) { (void)ctx; }
static void vo_set_paused(struct vo *vo, bool paused) { vo->in->paused = paused; }
static void vo_redraw(struct vo *vo) { vo->in->request_redraw = true; }
static void mp_notify(struct MPContext *ctx, int event, void *data) { (void)ctx; (void)event; (void)data; }
static void m_config_notify_change_opt_ptr(void *config, void *opt) { (void)config; (void)opt; }
static int vo_reconfig(struct vo *vo, struct mp_image_params *params);
void vo_seek_reset(struct vo *vo);
static void read_opts(struct vo *vo);
static void update_opts(struct vo *vo) { read_opts(vo); }

static int driver_reconfig(struct vo *vo, struct mp_image_params *params)
{
    (void)vo;
    assert(params->w > 0 && params->h > 0);
    reconfigs++;
    return 0;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    assert(!vo->in->lock);
    draws++;
    next_channel = frame->current ? frame->current->channel : 0;
    if (vo->driver->caps & VO_CAP_FRAMEOWNER) {
        assert(!owned_frame);
        owned_frame = frame;
        frame->current = NULL; // The caller may not read a handed-over frame.
    }
    return draw_ok;
}
static void flip_page(struct vo *vo)
{
    assert(!vo->in->lock);
    flips++;
    if (draw_ok && (!(vo->driver->caps & VO_CAP_NORETAIN) || next_channel))
        submitted_channel = next_channel;
    if (reset_during_flip) {
        vo_seek_reset(vo);
        reset_during_flip = false;
    }
}

#include "vo_android_frame_functions.h"

static int vo_reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct mp_image image = {.params = *params};
    int ret;
    void *args[] = {vo, &image, &ret};
    run_reconfig(args);
    return ret;
}

static void queue_video(struct vo *vo, struct mp_image *image)
{
    struct vo_frame *frame = calloc(1, sizeof(*frame));
    frame->current = image;
    frame->pts = mp_time_ns();
    frame->duration = MP_TIME_MS_TO_NS(40);
    vo_queue_frame(vo, frame);
}

int main(void)
{
    struct mp_vo_opts opts = {.WinID = 10, .android_keep_video_frame = true};
    struct vo_internal state = {0};
    struct vo_driver driver = {.draw_frame = draw_frame, .flip_page = flip_page,
                               .reconfig = driver_reconfig};
    struct vo vo = {.opts = &opts, .in = &state, .driver = &driver, .config_ok = true};
    struct MPOpts player_opts = {.vo = &opts, .force_vo = 1};
    struct MPContext ctx = {.opts = &player_opts, .video_out = &vo, .stop_play = 1};
    struct mp_image old_channel = {.channel = 1}, new_channel = {.channel = 2};
    read_opts(&vo);

    // Cold startup still initializes/draws black; no frame has been submitted.
    assert(handle_force_window(&ctx, true) == 0);
    assert(reconfigs == 1 && state.request_redraw);
    do_redraw(&vo);
    assert(draws == 1 && flips == 1 && submitted_channel == 0);
    assert(!vo_has_video_frame(&vo));

    queue_video(&vo, &old_channel);
    render_frame(&vo);
    assert(submitted_channel == 1 && vo_has_video_frame(&vo));

    // Stop discards queued/timing state but does not replace the Surface buffer.
    queue_video(&vo, &new_channel);
    vo_seek_reset(&vo);
    assert(!state.frame_queued && !state.hasframe && !state.hasframe_rendered);
    assert(state.current_frame && vo_has_video_frame(&vo));
    assert(handle_force_window(&ctx, true) == 0 && reconfigs == 1);
    vo_redraw(&vo);
    do_redraw(&vo);
    assert(!state.request_redraw && draws == 2 && flips == 2);
    assert(submitted_channel == 1);

    // A new format can discard old decoded images without clearing the buffer.
    struct mp_image_params params = {.w = 640, .h = 480};
    vo_reconfig(&vo, &params);
    assert(!state.current_frame && vo_has_video_frame(&vo));
    vo_redraw(&vo);
    do_redraw(&vo);
    assert(draws == 2 && flips == 2 && submitted_channel == 1);
    queue_video(&vo, &new_channel);
    render_frame(&vo);
    assert(draws == 3 && flips == 3 && submitted_channel == 2);
    assert(state.hasframe_rendered && vo_has_video_frame(&vo));

    // Existing reset behavior is preserved when the policy is disabled.
    opts.android_keep_video_frame = false;
    vo_seek_reset(&vo);
    assert(handle_force_window(&ctx, true) == 0 && reconfigs == 3);
    do_redraw(&vo);
    assert(submitted_channel == 0 && !vo_has_video_frame(&vo));
    opts.android_keep_video_frame = true;

    // A draw failure cannot establish initial retention eligibility.
    draw_ok = false;
    queue_video(&vo, &old_channel);
    render_frame(&vo);
    assert(!vo_has_video_frame(&vo));
    draw_ok = true;

    // NORETAIN outputs have no retained decoded image, but do own a video buffer.
    driver.caps = VO_CAP_NORETAIN | VO_CAP_NORETAIN_REDRAW;
    queue_video(&vo, &old_channel);
    render_frame(&vo);
    assert(!state.current_frame && vo_has_video_frame(&vo));
    vo_redraw(&vo);
    do_redraw(&vo); // normal OSD-only redraw must not lose video eligibility
    assert(submitted_channel == 1 && vo_has_video_frame(&vo));
    vo_seek_reset(&vo);
    int previous_flips = flips;
    vo_redraw(&vo);
    do_redraw(&vo);
    assert(flips == previous_flips && submitted_channel == 1);
    assert(handle_force_window(&ctx, true) == 0 && reconfigs == 3);
    queue_video(&vo, &new_channel);
    render_frame(&vo);
    assert(submitted_channel == 2 && !state.current_frame);

    // An OSD update on the same video Surface preserves eligibility.
    read_opts(&vo);
    assert(vo_has_video_frame(&vo));
    // In-place video Surface replacement and detach invalidate eligibility.
    opts.WinID = 11;
    read_opts(&vo);
    assert(!vo_has_video_frame(&vo));
    opts.WinID = 0;
    read_opts(&vo);
    assert(handle_force_window(&ctx, true) == 0);
    assert(!ctx.video_out && destroys == 1);
    ctx.video_out = &vo;
    opts.WinID = 10;
    read_opts(&vo);

    // A concurrent reset during flip must not be overwritten by render completion.
    reset_during_flip = true;
    queue_video(&vo, &old_channel);
    render_frame(&vo);
    assert(vo_has_video_frame(&vo) && !state.hasframe_rendered);
    previous_flips = flips;
    do_redraw(&vo);
    assert(flips == previous_flips);

    // FRAMEOWNER can change or release the frame during draw_frame. Record
    // whether it contains video before handing it over, including for redraw.
    driver.caps = VO_CAP_FRAMEOWNER;
    queue_video(&vo, &new_channel);
    render_frame(&vo);
    assert(vo_has_video_frame(&vo) && submitted_channel == 2);
    assert(owned_frame && !owned_frame->current);
    free(owned_frame);
    owned_frame = NULL;
    do_redraw(&vo);
    assert(vo_has_video_frame(&vo) && submitted_channel == 2);
    assert(owned_frame && !owned_frame->current);
    free(owned_frame);
    owned_frame = NULL;
    driver.caps = 0;
    vo_seek_reset(&vo);
    free(state.current_frame);
    state.current_frame = NULL;

    // A new VO has no eligibility even if it uses the same Surface identifier.
    struct vo_internal next_state = {0};
    vo.in = &next_state;
    read_opts(&vo);
    assert(!vo_has_video_frame(&vo));
    // force-window=no still tears down the output rather than retaining it.
    player_opts.force_vo = 0;
    assert(handle_force_window(&ctx, true) == 0 && !ctx.video_out);
    assert(destroys == 2);
    // Initializing without an attached Surface must not create a GPU output.
    player_opts.force_vo = 1;
    cold_vo = &vo;
    opts.WinID = 0;
    assert(handle_force_window(&ctx, true) == 0 && !ctx.video_out);
    opts.WinID = -1;
    assert(handle_force_window(&ctx, true) == 0 && !ctx.video_out);
    assert(destroys == 2);
    // A forced direct idle window must wait without disabling force-window.
    opts.WinID = 11;
    direct_requested = true;
    assert(handle_force_window(&ctx, true) == 0 && !ctx.video_out);
    assert(player_opts.force_vo == 1);
    direct_ready = true;
    assert(handle_force_window(&ctx, true) == 0 && ctx.video_out == &vo);
    free(vo.params);
    puts("Android VO frame-retention contracts passed.");
    return 0;
}
