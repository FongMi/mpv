/* Exercise production decoder and player controls without opening a codec.
 * The option cache publishes queued changes; lifecycle calls count reopens.
 */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifndef HAVE_ANDROID
#define HAVE_ANDROID 1
#endif
#define CONTROL_OK 1
#define CONTROL_TRUE 1
#define CONTROL_FALSE 0
#define CONTROL_ERROR -2
#define CONTROL_UNKNOWN -1
#define STREAM_VIDEO 0
#define UPDATE_HWDEC (UINT64_C(1) << 0)
#define UPDATE_VO (UINT64_C(1) << 1)
#define UPDATE_AUDIO (UINT64_C(1) << 2)
#define UPDATE_PRIORITY (UINT64_C(1) << 3)
#define UPDATE_SCREENSAVER (UINT64_C(1) << 4)
#define UPDATE_VOL (UINT64_C(1) << 5)
#define UPDATE_LAVFI_COMPLEX (UINT64_C(1) << 6)
#define UPDATE_VIDEO (UINT64_C(1) << 7)
#define MP_NOPTS_VALUE (-1e20)
#define MPSEEK_ABSOLUTE 1
#define MPSEEK_EXACT 1
#define VOCTRL_UPDATE_RENDER_OPTS 1
#define MPV_EVENT_VIDEO_RECONFIG 1
#define MP_WARN(vd, ...) ((void)(vd))
#define mp_assert assert

#define HWDEC_POLICY_TYPES
#include "hwdec_policy_functions.h"
#undef HWDEC_POLICY_TYPES

typedef struct AVCodecContext { int has_b_frames; } AVCodecContext;
struct m_config_cache {
    struct hwdec_opts *current, *cached;
    int changes[8], num_changes, next_change;
};
typedef struct {
    struct hwdec_opts *hwdec_opts;
    struct m_config_cache *hwdec_opts_cache;
    AVCodecContext *avctx;
    struct { char *method_name; } hwdec;
    bool use_hwdec, hwdec_failed, hwdec_request_reinit, force_eof;
    bool hwdec_notified;
    int hwdec_fail_count, framedrop_flags, extra_hw_frames_hint;
} vd_ffmpeg_ctx;
struct mp_filter { void *priv; };
struct mp_decoder {
    struct mp_filter *f;
    int (*control)(struct mp_filter *, enum dec_ctrl, void *);
};
struct mp_decoder_wrapper { struct mp_filter *f; };
struct priv {
    bool is_group, locked;
    struct mp_decoder_wrapper **children;
    int num_children, cache_lock;
    struct mp_decoder *decoder;
    char *cur_hwdec;
};
struct track { struct mp_decoder_wrapper *dec; };
struct MPOpts { int unused; };
struct MPContext {
    struct MPOpts *opts;
    struct track *current_track[1][1];
    double video_pts;
    void *video_out;
    bool rebuild_chain;
};
struct m_config_option { void *data; };
struct mp_option_callback { struct m_config_option *co; uint64_t flags; };

static int decoder_opens, suspends, resumes, seeks, executed_seeks;
static int output_updates, vo_rebuilds, chain_updates, notifications;
static bool last_output_hwdec;
static bool update_hwdec_options(vd_ffmpeg_ctx *ctx);
static bool m_config_cache_get_next_changed(struct m_config_cache *cache,
                                           void **opt);
static void reinit(struct mp_filter *vd);
static void force_fallback(struct mp_filter *vd);
static int control(struct mp_filter *vd, enum dec_ctrl cmd, void *arg);
int mp_decoder_wrapper_control(struct mp_decoder_wrapper *d,
                               enum dec_ctrl cmd, void *arg);

static void thread_lock(struct priv *p) { assert(!p->locked); p->locked = true; }
static void thread_unlock(struct priv *p) { assert(p->locked); p->locked = false; }
static void mp_mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mp_mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static int update_cached_values(struct priv *p)
{
    assert(p->locked);
    if (!p->decoder)
        return CONTROL_UNKNOWN;
    vd_ffmpeg_ctx *ctx = p->decoder->f->priv;
    p->cur_hwdec = ctx->use_hwdec ? ctx->hwdec.method_name : NULL;
    return CONTROL_OK;
}
static void mp_decoder_wrapper_suspend(struct mp_decoder_wrapper *dec)
{
    assert(dec);
    suspends++;
}
static void mp_decoder_wrapper_resume(struct mp_decoder_wrapper *dec)
{
    assert(dec);
    resumes++;
}
static void queue_seek(struct MPContext *mpctx, int type, double pts,
                       int precision, int flags)
{
    assert(type == MPSEEK_ABSOLUTE && precision == MPSEEK_EXACT && !flags);
    assert(pts == mpctx->video_pts);
    seeks++;
}
static void execute_queued_seek(struct MPContext *mpctx)
{
    (void)mpctx;
    executed_seeks++;
}
static bool update_video_output(struct MPContext *mpctx, void *opt_ptr,
                                bool hwdec_changed)
{
    (void)opt_ptr;
    output_updates++;
    last_output_hwdec = hwdec_changed;
    if (mpctx->rebuild_chain) {
        vo_rebuilds++;
        struct track *track = mpctx->current_track[0][STREAM_VIDEO];
        mp_decoder_wrapper_control(track->dec, VDCTRL_REINIT, NULL);
        return true;
    }
    return false;
}
static void update_vo_chain_el_state(struct MPContext *mpctx)
{
    (void)mpctx;
    chain_updates++;
}
static void mp_notify(struct MPContext *mpctx, int event, void *data)
{
    (void)mpctx;
    assert(event == MPV_EVENT_VIDEO_RECONFIG && !data);
    notifications++;
}
static void reload_audio_output(struct MPContext *ctx) { (void)ctx; }
static void update_priority(struct MPContext *ctx) { (void)ctx; }
static void update_screensaver_state(struct MPContext *ctx) { (void)ctx; }
static void audio_update_volume(struct MPContext *ctx) { (void)ctx; }
static void update_lavfi_complex(struct MPContext *ctx) { (void)ctx; }
static int vo_control(void *vo, int request, void *arg)
{
    (void)vo; (void)request; (void)arg;
    return CONTROL_OK;
}
static void mp_wakeup_core(struct MPContext *ctx) { (void)ctx; }

#include "hwdec_policy_functions.h"

enum change { FALLBACK, API, CODECS, EXTRA_FRAMES };
static char *software_api[] = {"no", NULL};
static char *hardware_api[] = {"mediacodec", "mediacodec-copy", NULL};
static char *copy_api[] = {"mediacodec-copy", NULL};

static bool m_config_cache_get_next_changed(struct m_config_cache *cache,
                                           void **opt)
{
    *opt = NULL;
    if (cache->next_change == cache->num_changes)
        return false;
    struct hwdec_opts *cached = cache->cached;
    struct hwdec_opts *current = cache->current;
    switch (cache->changes[cache->next_change++]) {
    case FALLBACK:
        cached->software_fallback = current->software_fallback;
        *opt = &cached->software_fallback;
        break;
    case API:
        cached->hwdec_api = current->hwdec_api;
        *opt = &cached->hwdec_api;
        break;
    case CODECS:
        cached->hwdec_codecs = current->hwdec_codecs;
        *opt = &cached->hwdec_codecs;
        break;
    case EXTRA_FRAMES:
        cached->hwdec_extra_frames = current->hwdec_extra_frames;
        *opt = &cached->hwdec_extra_frames;
        break;
    default:
        assert(false);
    }
    return true;
}
static void reinit(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    update_hwdec_options(ctx);
    decoder_opens++;
    ctx->use_hwdec = ctx->hwdec_opts->hwdec_api != software_api;
    ctx->hwdec_failed = ctx->hwdec_request_reinit = ctx->force_eof = false;
    ctx->hwdec_fail_count = 0;
}
static void force_fallback(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    ctx->use_hwdec = false;
}

struct fixture {
    struct hwdec_opts current, cached;
    struct m_config_cache cache;
    AVCodecContext avctx;
    vd_ffmpeg_ctx ctx;
    struct mp_filter decoder_filter, wrapper_filter;
    struct mp_decoder decoder;
    struct priv wrapper_priv;
    struct mp_decoder_wrapper wrapper;
    struct track track;
    struct MPOpts opts;
    struct MPContext player;
};

static void init_fixture(struct fixture *f, bool hardware)
{
    memset(f, 0, sizeof(*f));
    f->current = (struct hwdec_opts){
        .hwdec_api = hardware ? hardware_api : software_api,
        .hwdec_codecs = "h264,hevc", .software_fallback = 3,
    };
    f->cached = f->current;
    f->cache.current = &f->current;
    f->cache.cached = &f->cached;
    f->ctx = (vd_ffmpeg_ctx){
        .hwdec_opts = &f->cached, .hwdec_opts_cache = &f->cache,
        .avctx = &f->avctx, .use_hwdec = hardware,
        .hwdec_notified = true, .hwdec.method_name = "mediacodec",
    };
    f->decoder_filter.priv = &f->ctx;
    f->decoder = (struct mp_decoder){.f = &f->decoder_filter, .control = control};
    f->wrapper_priv.decoder = &f->decoder;
    f->wrapper_filter.priv = &f->wrapper_priv;
    f->wrapper.f = &f->wrapper_filter;
    f->track.dec = &f->wrapper;
    f->player.opts = &f->opts;
    f->player.current_track[0][STREAM_VIDEO] = &f->track;
    f->player.video_pts = 52.0;
    f->player.video_out = f;
}
static void pending(struct fixture *f, enum change change)
{
    if (f->cache.next_change == f->cache.num_changes)
        f->cache.next_change = f->cache.num_changes = 0;
    assert(f->cache.num_changes < 8);
    f->cache.changes[f->cache.num_changes++] = change;
}
static void callback(struct fixture *f, uint64_t flags)
{
    struct mp_option_callback update = {.flags = flags};
    mp_option_run_callback(&f->player, &update);
    assert(f->cache.next_change == f->cache.num_changes);
}
static void reset_counts(void)
{
    decoder_opens = suspends = resumes = seeks = executed_seeks = 0;
    output_updates = vo_rebuilds = chain_updates = notifications = 0;
    last_output_hwdec = false;
}
static void assert_reinit(int decoders, bool seek)
{
    assert(decoder_opens == decoders);
    assert(seeks == (int)seek && executed_seeks == (int)seek);
    assert(suspends == (int)seek && resumes == (int)seek);
    assert(output_updates == HAVE_ANDROID && !vo_rebuilds);
    assert(chain_updates == 1 && notifications == 1);
}
static void set_fallback(struct fixture *f, int value)
{
    if (f->current.software_fallback != value) {
        f->current.software_fallback = value;
        pending(f, FALLBACK);
    }
    callback(f, UPDATE_HWDEC);
    assert(f->cached.software_fallback == value);
}
static void set_api(struct fixture *f, char **api)
{
    if (f->current.hwdec_api != api) {
        f->current.hwdec_api = api;
        pending(f, API);
    }
    callback(f, UPDATE_HWDEC);
    assert(f->cached.hwdec_api == api);
}

static void test_mode_switches(void)
{
    struct fixture f;
    init_fixture(&f, false);
    reset_counts();
    // SW -> HW opens MediaCodec once. The next property changes policy only.
    set_api(&f, hardware_api);
    assert_reinit(1, true);
    set_fallback(&f, INT_MAX);
    assert_reinit(1, true);
    assert(f.ctx.use_hwdec);
    for (int i = 0; i < 3; i++)
        handle_err(&f.decoder_filter);
    assert(!f.ctx.hwdec_failed && f.ctx.hwdec_fail_count == 3);

    // HW -> AUTO updates the live error threshold before reselection.
    f.ctx.hwdec_fail_count = 0;
    reset_counts();
    set_fallback(&f, 3);
    assert(!decoder_opens && !seeks && !output_updates && !notifications);
    handle_err(&f.decoder_filter);
    handle_err(&f.decoder_filter);
    assert(!f.ctx.hwdec_failed);
    handle_err(&f.decoder_filter);
    assert(f.ctx.hwdec_failed);

    // A failed decoder must still take the full path, even for policy alone.
    reset_counts();
    set_fallback(&f, INT_MAX);
    assert_reinit(1, true);
    assert(!f.ctx.hwdec_failed);

    // AUTO -> SW preserves reselection; SW -> AUTO preserves both controls.
    reset_counts();
    set_fallback(&f, 3);
    assert(!decoder_opens);
    set_api(&f, software_api);
    assert_reinit(1, true);
    assert(!f.ctx.use_hwdec);
    reset_counts();
    set_fallback(&f, INT_MAX);
    assert_reinit(1, true);
    reset_counts();
    set_fallback(&f, 3);
    assert_reinit(1, true);
    reset_counts();
    set_api(&f, hardware_api);
    assert_reinit(1, true);
    reset_counts();
    set_fallback(&f, INT_MAX);
    assert(!decoder_opens && !seeks && !output_updates && !vo_rebuilds);
}

static void test_pending_selection(void)
{
    for (int field = API; field <= EXTRA_FRAMES; field++) {
        for (int order = 0; order < 2; order++) {
            struct fixture f;
            init_fixture(&f, true);
            f.current.software_fallback = INT_MAX;
            f.current.hwdec_api = copy_api;
            f.current.hwdec_codecs = "all";
            f.current.hwdec_extra_frames = 8;
            pending(&f, order ? field : FALLBACK);
            pending(&f, order ? FALLBACK : field);
            reset_counts();
            callback(&f, UPDATE_HWDEC);
            assert_reinit(1, true);
            assert(f.cached.software_fallback == INT_MAX);
            if (field == API)
                assert(f.cached.hwdec_api == copy_api);
            if (field == CODECS)
                assert(strcmp(f.cached.hwdec_codecs, "all") == 0);
            if (field == EXTRA_FRAMES)
                assert(f.cached.hwdec_extra_frames == 8);
        }
    }
}

static void test_recovery(void)
{
    for (int state = 0; state < 6; state++) {
        struct fixture f;
        init_fixture(&f, true);
        f.current.software_fallback = INT_MAX;
        if (state != 0)
            pending(&f, FALLBACK);
        if (state == 1)
            f.ctx.use_hwdec = false;
        if (state == 2)
            f.ctx.avctx = NULL;
        if (state == 3)
            f.ctx.hwdec_failed = true;
        if (state == 4)
            f.ctx.hwdec_request_reinit = true;
        if (state == 5)
            f.ctx.force_eof = true;
        reset_counts();
        callback(&f, UPDATE_HWDEC);
        assert_reinit(1, true);
    }
    // With no timestamp there is no seek, but recovery still opens a decoder.
    struct fixture f;
    init_fixture(&f, true);
    f.player.video_pts = MP_NOPTS_VALUE;
    reset_counts();
    callback(&f, UPDATE_HWDEC);
    assert_reinit(1, false);

    // UPDATE_VO must survive a successful policy-only UPDATE_HWDEC.
    f.current.software_fallback = INT_MAX;
    pending(&f, FALLBACK);
    reset_counts();
    callback(&f, UPDATE_VO | UPDATE_HWDEC);
    assert(output_updates == 1 && !last_output_hwdec);
    assert(!decoder_opens && !seeks && !vo_rebuilds);

#if HAVE_ANDROID
    // Replacing the video chain already opened the new decoder exactly once.
    f.player.rebuild_chain = true;
    f.current.hwdec_api = copy_api;
    pending(&f, API);
    reset_counts();
    callback(&f, UPDATE_HWDEC);
    assert(output_updates == 1 && vo_rebuilds == 1 && decoder_opens == 1);
    assert(!seeks && !chain_updates && !notifications);
#endif
}

static void test_group(void)
{
    for (int state = 0; state < 4; state++) {
        struct fixture first, last;
        init_fixture(&first, true);
        init_fixture(&last, true);
        first.current.software_fallback = INT_MAX;
        last.current.software_fallback = INT_MAX;
        pending(&first, FALLBACK);
        pending(&last, FALLBACK);
        if (state == 1)
            first.ctx.use_hwdec = false;
        if (state == 2)
            first.ctx.hwdec_failed = true;
        if (state == 3)
            first.decoder.control = NULL;
        struct mp_decoder_wrapper *children[] = {&first.wrapper, &last.wrapper};
        struct priv group = {.is_group = true, .children = children,
                             .num_children = 2};
        struct mp_filter filter = {.priv = &group};
        struct mp_decoder_wrapper wrapper = {.f = &filter};
        first.track.dec = &wrapper;
        reset_counts();
        struct mp_option_callback update = {.flags = UPDATE_HWDEC};
        mp_option_run_callback(&first.player, &update);
        assert(last.cached.software_fallback == INT_MAX);
        assert(last.cache.next_change == last.cache.num_changes);
        if (state == 0) {
            assert(first.cached.software_fallback == INT_MAX);
            assert(!decoder_opens && !seeks && !output_updates);
        } else {
            assert_reinit(state == 3 ? 1 : 2, true);
            if (state != 3)
                assert(first.cached.software_fallback == INT_MAX);
        }
        // Existing GET controls keep their last-child return/value contract.
        first.ctx.hwdec.method_name = "mediacodec";
        last.ctx.hwdec.method_name = "mediacodec-copy";
        char *selected = NULL;
        assert(mp_decoder_wrapper_control(&wrapper, VDCTRL_GET_HWDEC,
                                           &selected) == CONTROL_OK);
        assert(strcmp(selected, "mediacodec-copy") == 0);
    }
}

int main(void)
{
    test_mode_switches();
    test_pending_selection();
    test_recovery();
    test_group();
    puts("Hardware decoder policy contracts passed.");
    return 0;
}
