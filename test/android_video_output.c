/* Host fixture for the production Android output option callback. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef HAVE_ANDROID
#define HAVE_ANDROID 1
#endif
#define STREAM_VIDEO 0
#define STREAM_AUDIO 1
#define STREAM_SUB 2
#define STATUS_SYNCING 0
#define STATUS_EOF 4
#define mp_assert assert
#define VOCTRL_UPDATE_WINDOW 1
#define VOCTRL_UPDATE_OSD_SURFACE 2
#define MP_NOPTS_VALUE -1
#define MPSEEK_ABSOLUTE 1
#define MPSEEK_EXACT 1
#define MP_VERBOSE(ctx, ...) ((void)(ctx))
#define MPV_EVENT_VIDEO_RECONFIG 1

struct m_obj_settings { char *name; };
struct mp_vo_opts {
    int64_t WinID, android_osd_wid;
    int android_dolby_vision_output;
    struct m_obj_settings *video_driver_list;
};
struct ra_ctx_opts { struct m_obj_settings *context_list; };
struct MPOpts {
    struct mp_vo_opts *vo;
    struct ra_ctx_opts *ra_ctx_opts;
    bool stream_auto_sel;
    int force_vo;
};
struct vo { bool config_ok; };
struct mp_decoder_wrapper { int unused; };
struct track {
    struct mp_decoder_wrapper *dec;
    bool attached_picture;
    bool selected;
};
struct vo_chain { struct track *track; };
struct mp_image { int unused; };
struct MPContext {
    struct MPOpts *opts;
    struct track *current_track[1][2];
    struct track *android_dolby_vision_direct_failed_track;
    struct vo *video_out;
    struct vo_chain *vo_chain;
    void *ao_chain;
    int stop_play, play_dir, video_status;
    int num_next_frames, num_past_frames;
    struct mp_image *next_frames[1], *saved_frame;
    double video_pts, delay, time_frame, last_frame_duration;
    double total_avsync_change, last_av_difference, audio_drift_compensation;
    double avd_filtered, display_sync_error;
    int mistimed_frames_total, drop_message_shown;
    bool display_sync_active;
    bool playback_initialized, restart_complete;
};
static struct vo output;
static struct vo_chain chain;
static struct track *last_initialized_track;
static bool simulated_direct_active, direct_wanted;
static int last_control, control_result = 1, destroyed, initialized, seeks, wakeups;

static bool wants_android_dolby_vision_direct_output(struct MPContext *ctx,
                                                    struct track *track)
{
    return direct_wanted;
}
static bool is_android_dolby_vision_direct_output_active(struct MPContext *ctx)
{
    return simulated_direct_active;
}
void reinit_video_chain_src(struct MPContext *ctx, struct track *track);
static void test_video_output_init(struct MPContext *ctx, struct track *track)
{
    initialized++;
    last_initialized_track = track;
    chain.track = track;
    ctx->video_out = &output;
    ctx->vo_chain = &chain;
}
static void reinit_video_chain(struct MPContext *ctx)
{
    if (ctx->current_track[0][STREAM_VIDEO])
        reinit_video_chain_src(ctx, ctx->current_track[0][STREAM_VIDEO]);
}
static void reinit_audio_chain(struct MPContext *ctx)
{
    if (ctx->current_track[0][STREAM_AUDIO])
        ctx->ao_chain = &output;
}
static void reinit_sub_all(struct MPContext *ctx) {}
static void reinit_sub(struct MPContext *ctx, struct track *track) {}
static void reselect_demux_stream(struct MPContext *ctx, struct track *track,
                                  bool refresh_only) {}
static void vo_chain_reset_state(struct vo_chain *vo_c) {}
static void vo_chain_uninit(struct vo_chain *vo_c) {}
static void mp_notify(struct MPContext *ctx, int event, void *data) {}
static void vo_destroy(struct vo *vo) { destroyed++; }
static void mp_decoder_wrapper_set_play_dir(struct mp_decoder_wrapper *dec, int dir) {}
static void mp_image_unrefp(struct mp_image **image) { *image = NULL; }
static int vo_control(struct vo *vo, int request, void *data)
{
    last_control = request;
    return control_result;
}
static void test_force_window_init(struct MPContext *ctx)
{
    initialized++;
    assert(ctx->opts->vo->WinID > 0);
    ctx->video_out = &output;
}
static double get_current_time(struct MPContext *ctx) { return 10; }
static void queue_seek(struct MPContext *ctx, int type, double pts, int exact, int flags)
{
    seeks++;
}
static void execute_queued_seek(struct MPContext *ctx) {}
static void mp_decoder_wrapper_suspend(struct mp_decoder_wrapper *dec) {}
static void mp_wakeup_core(struct MPContext *ctx) { wakeups++; }

#include "android_video_output_functions.h"

static void test_initial_surface(void)
{
    struct m_obj_settings gpu[] = {{"gpu"}, {NULL}};
    struct m_obj_settings android[] = {{"android"}, {NULL}};
    struct mp_vo_opts vo_opts = {.video_driver_list = gpu};
    struct ra_ctx_opts ra_opts = {.context_list = android};
    struct MPOpts opts = {
        .vo = &vo_opts, .ra_ctx_opts = &ra_opts, .stream_auto_sel = true,
    };
    struct track video = {0};
    struct MPContext ctx = {.opts = &opts, .current_track = {{&video}}};
    int before = initialized;

    reinit_video_chain_src(&ctx, &video);
    assert(initialized == before && !ctx.video_out && !ctx.vo_chain);
    assert(ctx.current_track[0][STREAM_VIDEO] == &video);
    assert(test_has_playable_stream(&ctx));
    assert(ctx.video_status == STATUS_SYNCING);
    reset_video_state(&ctx); // A seek must preserve the Surface prerequisite.
    assert(ctx.video_status == STATUS_SYNCING);

    // A preceding file may leave an output whose window now needs cleanup.
    ctx.video_out = &output;
    int old_destroyed = destroyed;
    assert(handle_force_window(&ctx, true) == 0);
    assert(destroyed == old_destroyed + 1 && !ctx.video_out);
    assert(ctx.video_status == STATUS_SYNCING);

    vo_opts.WinID = 90;
    assert(update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == before + 1 && last_initialized_track == &video);
}

static void check_initial_output(struct m_obj_settings *drivers,
                                 struct m_obj_settings *contexts, bool waiting)
{
    struct mp_vo_opts vo_opts = {.video_driver_list = drivers};
    struct ra_ctx_opts ra_opts = {.context_list = contexts};
    struct MPOpts opts = {.vo = &vo_opts, .ra_ctx_opts = &ra_opts};
    struct track video = {0};
    struct MPContext ctx = {.opts = &opts, .current_track = {{&video}}};
    for (int n = 0; n < 2; n++) {
        vo_opts.WinID = n ? -1 : 0;
        ctx.video_out = NULL;
        ctx.vo_chain = NULL;
        int before = initialized;
        reinit_video_chain_src(&ctx, &video);
        assert(initialized == before + !waiting);
        assert(is_android_video_output_waiting_for_surface(&ctx) == waiting);
    }
}

static void test_surface_output_policy(void)
{
    struct m_obj_settings gpu[] = {{"gpu"}, {NULL}};
    struct m_obj_settings gpu_next[] = {{"gpu-next"}, {NULL}};
    struct m_obj_settings android[] = {{"android"}, {NULL}};
    struct m_obj_settings android_vk[] = {{"androidvk"}, {NULL}};
    struct m_obj_settings automatic[] = {{"auto"}, {NULL}};
    struct m_obj_settings render[] = {{"libmpv"}, {NULL}};
    struct m_obj_settings null_vo[] = {{"null"}, {NULL}};
    struct m_obj_settings offscreen[] = {{"displayvk"}, {NULL}};
    struct m_obj_settings fallback[] = {{"gpu"}, {"null"}, {NULL}};
    struct m_obj_settings autoprobe[] = {{"gpu"}, {""}, {NULL}};
    struct m_obj_settings context_fallback[] = {{"android"}, {"displayvk"}, {NULL}};
    struct m_obj_settings empty_context[] = {{"android"}, {""}, {NULL}};
    check_initial_output(gpu, android, true);
    check_initial_output(gpu_next, android_vk, true);
    check_initial_output(gpu, automatic, true);
    check_initial_output(NULL, NULL, true);
    check_initial_output(autoprobe, android, true);
    check_initial_output(render, android, false);
    check_initial_output(null_vo, android, false);
    check_initial_output(gpu, offscreen, false);
    check_initial_output(fallback, android, false);
    check_initial_output(gpu, context_fallback, false);
    check_initial_output(gpu, empty_context, false);
}

static void test_audio_and_pending_lifecycle(void)
{
    struct m_obj_settings gpu[] = {{"gpu"}, {NULL}};
    struct mp_vo_opts vo_opts = {.video_driver_list = gpu};
    struct MPOpts opts = {.vo = &vo_opts, .stream_auto_sel = true};
    struct track video = {0}, audio = {0}, replacement = {0};
    struct MPContext ctx = {.opts = &opts, .video_status = STATUS_EOF};
    int before = initialized;

    // No selected video is audio-only, regardless of output/Surface metadata.
    ctx.current_track[0][STREAM_AUDIO] = &audio;
    test_prepare_outputs(&ctx);
    assert(ctx.ao_chain && !ctx.vo_chain && initialized == before);
    assert(!is_android_video_output_waiting_for_surface(&ctx));
    assert(ctx.video_status == STATUS_EOF && test_has_playable_stream(&ctx));

    // Actual video must keep synchronizing while audio is already prepared.
    ctx.current_track[0][STREAM_VIDEO] = &video;
    test_prepare_outputs(&ctx);
    assert(ctx.ao_chain && !ctx.vo_chain && initialized == before);
    assert(ctx.video_status == STATUS_SYNCING && test_has_playable_stream(&ctx));
    reset_video_state(&ctx);
    assert(ctx.video_status == STATUS_SYNCING);

    // Cover art can wait independently while the audio starts normally.
    video.attached_picture = true;
    ctx.ao_chain = NULL;
    test_prepare_outputs(&ctx);
    assert(ctx.ao_chain && !ctx.vo_chain && initialized == before);
    assert(ctx.video_status == STATUS_EOF && test_has_playable_stream(&ctx));
    ctx.current_track[0][STREAM_AUDIO] = NULL;
    ctx.ao_chain = NULL;
    test_finish_track_switch(&ctx, 0, STREAM_AUDIO, NULL);
    assert(ctx.video_status == STATUS_SYNCING); // An image without audio still waits.
    test_finish_track_switch(&ctx, 0, STREAM_AUDIO, &audio);
    assert(ctx.ao_chain && ctx.video_status == STATUS_EOF);
    ctx.ao_chain = NULL;
    test_finish_track_switch(&ctx, 0, STREAM_AUDIO, NULL);
    assert(ctx.video_status == STATUS_SYNCING);

    // Disabling pending video releases audio without a late Surface restoring it.
    video.attached_picture = false;
    test_finish_track_switch(&ctx, 0, STREAM_AUDIO, &audio);
    assert(ctx.ao_chain && ctx.video_status == STATUS_SYNCING);
    uninit_video_chain(&ctx);
    test_finish_track_switch(&ctx, 0, STREAM_VIDEO, NULL);
    assert(ctx.ao_chain && ctx.video_status == STATUS_EOF);
    vo_opts.WinID = 91;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == before && !ctx.vo_chain);
    vo_opts.WinID = 0;
    test_finish_track_switch(&ctx, 0, STREAM_VIDEO, &video);
    assert(ctx.video_status == STATUS_SYNCING);

    // A stop cancels deferred playback even before the selected track is freed.
    ctx.stop_play = 1;
    vo_opts.WinID = 92;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == before && !ctx.vo_chain);
    assert(!is_android_video_output_waiting_for_surface(&ctx));

    // A track switch can retain its old VO before the stop/Surface command batch.
    ctx.video_out = &output;
    control_result = -1;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == before && !ctx.vo_chain && ctx.video_out == &output);
    control_result = 1;
    direct_wanted = true;
    vo_opts.android_osd_wid = 95;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == before && !ctx.vo_chain && ctx.video_out == &output);
    direct_wanted = false;
    uninit_video_out(&ctx);
    ctx.current_track[0][STREAM_VIDEO] = NULL;
    ctx.current_track[0][STREAM_AUDIO] = NULL;
    ctx.ao_chain = NULL;
    ctx.stop_play = 0;
    reset_video_state(&ctx);
    assert(ctx.video_status == STATUS_EOF && !test_has_playable_stream(&ctx));

    // A new load while waiting owns the later Surface callback, not the old track.
    vo_opts.WinID = 0;
    ctx.current_track[0][STREAM_VIDEO] = &replacement;
    test_prepare_outputs(&ctx);
    assert(initialized == before && ctx.video_status == STATUS_SYNCING);
    vo_opts.WinID = 93;
    vo_opts.android_osd_wid = 0; // GPU video does not require the OSD Surface.
    assert(update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == before + 1 && last_initialized_track == &replacement);

    // Forced direct video-only playback remains selected until both Surfaces exist.
    struct m_obj_settings direct[] = {{"mediacodec_embed"}, {NULL}};
    vo_opts.video_driver_list = direct;
    ctx.video_out = NULL;
    ctx.vo_chain = NULL;
    test_prepare_outputs(&ctx);
    assert(initialized == before + 1 && ctx.video_status == STATUS_SYNCING);
    assert(test_has_playable_stream(&ctx));
    vo_opts.android_osd_wid = 94;
    assert(update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(initialized == before + 2 && last_initialized_track == &replacement);
}

static void test_blank_output_contract(void)
{
    struct m_obj_settings gpu[] = {{"gpu"}, {NULL}};
    struct m_obj_settings direct[] = {{"mediacodec_embed"}, {NULL}};
    struct mp_vo_opts vo_opts = {.video_driver_list = gpu};
    struct MPOpts opts = {.vo = &vo_opts};
    struct MPContext ctx = {.opts = &opts};
    int before = initialized;

    // A lavfi output has no selected track to restore through the wid callback.
    reinit_video_chain_src(&ctx, NULL);
    assert(initialized == before + 1);
    assert(!is_android_video_output_waiting_for_surface(&ctx));
    ctx.video_out = NULL;
    ctx.vo_chain = NULL;
    vo_opts.video_driver_list = direct;
    reinit_video_chain_src(&ctx, NULL);
    assert(initialized == before + 1); // Retain the original forced-direct gate.
}

int main(void)
{
    if (!HAVE_ANDROID) {
        struct m_obj_settings gpu[] = {{"gpu"}, {NULL}};
        struct m_obj_settings android[] = {{"android"}, {NULL}};
        check_initial_output(gpu, android, false);
        check_initial_output(NULL, NULL, false);
        puts("Non-Android output initialization remains independent of wid");
        return 0;
    }

    struct mp_vo_opts vo_opts = {.WinID = 10, .android_osd_wid = 20};
    struct MPOpts opts = {.vo = &vo_opts};
    struct mp_decoder_wrapper decoder = {0};
    struct track track = {.dec = &decoder};
    struct MPContext ctx = {
        .opts = &opts, .current_track = {{&track}},
        .video_out = &output, .vo_chain = &chain, .video_pts = 9,
    };

    // Changing only OSD must not send a video window rebind to the platform.
    assert(!update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(last_control == VOCTRL_UPDATE_OSD_SURFACE && !destroyed && !initialized);
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(last_control == VOCTRL_UPDATE_WINDOW && !destroyed);

    // GPU suspension keeps its chain; OSD changes do not rebind that video window.
    vo_opts.WinID = 0;
    last_control = 0;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(destroyed == 0 && initialized == 0 && last_control == VOCTRL_UPDATE_WINDOW);
    assert(ctx.current_track[0][STREAM_VIDEO] == &track);
    assert(!update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(last_control == VOCTRL_UPDATE_OSD_SURFACE && initialized == 0);

    vo_opts.WinID = 11;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == 0 && seeks == 0 && ctx.vo_chain);

    // Genuine video backend failures still rebuild; OSD policy can reselect Dovi.
    control_result = -1;
    assert(update_video_output(&ctx, &vo_opts.WinID, false));
    assert(destroyed == 1 && initialized == 1);
    control_result = 1;
    direct_wanted = true;
    assert(update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(destroyed == 2 && initialized == 2);

    // Direct MediaCodec releases its output on detach and restores on attach.
    simulated_direct_active = true;
    vo_opts.WinID = 0;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(destroyed == 3 && initialized == 2);
    assert(!update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(initialized == 2);
    vo_opts.WinID = 12;
    assert(update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == 3 && ctx.vo_chain);
    assert(wakeups == 10);
    // Explicit H.264 direct output must wait when video attaches before OSD.
    direct_wanted = simulated_direct_active = false;
    struct m_obj_settings forced[] = {{"mediacodec_embed"}, {NULL}};
    vo_opts.video_driver_list = forced;
    ctx.video_out = NULL;
    ctx.vo_chain = NULL;
    vo_opts.WinID = 13;
    vo_opts.android_osd_wid = 0;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == 3 && ctx.current_track[0][STREAM_VIDEO] == &track);
    vo_opts.android_osd_wid = 21;
    assert(update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(initialized == 4 && ctx.vo_chain);

    // OSD may detach while a direct VO lives without restarting its codec.
    simulated_direct_active = true;
    int old_destroyed = destroyed;
    vo_opts.android_osd_wid = 0;
    assert(!update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(destroyed == old_destroyed && initialized == 4);
    vo_opts.WinID = -1;
    assert(!update_video_output(&ctx, &vo_opts.WinID, false));
    assert(!ctx.video_out && !ctx.vo_chain);
    simulated_direct_active = false;

    // The reverse attachment order also waits, including the -1 sentinel.
    vo_opts.android_osd_wid = 22;
    assert(!update_video_output(&ctx, &vo_opts.android_osd_wid, false));
    assert(initialized == 4);
    vo_opts.WinID = 14;
    assert(update_video_output(&ctx, &vo_opts.WinID, false));
    assert(initialized == 5);
    assert(!direct_wanted); // Explicit output did not enable automatic Dovi selection.

    // An ordered fallback list is not a forced direct-output policy.
    struct m_obj_settings fallback[] = {{"mediacodec_embed"}, {"gpu-next"}, {NULL}};
    vo_opts.video_driver_list = fallback;
    assert(!wants_android_direct_output(&ctx, &track));
    vo_opts.video_driver_list = NULL;
    assert(!wants_android_direct_output(&ctx, &track));
    test_initial_surface();
    test_surface_output_policy();
    test_audio_and_pending_lifecycle();
    test_blank_output_contract();
    puts("Android OSD routing, Surface detach/restore and output policy contracts passed");
    return 0;
}
