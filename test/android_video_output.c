/* Host fixture for the production Android output option callback. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HAVE_ANDROID 1
#define STREAM_VIDEO 0
#define VOCTRL_UPDATE_WINDOW 1
#define VOCTRL_UPDATE_OSD_SURFACE 2
#define MP_NOPTS_VALUE -1
#define MPSEEK_ABSOLUTE 1
#define MPSEEK_EXACT 1
#define MP_VERBOSE(ctx, ...) ((void)(ctx))

struct m_obj_settings { char *name; };
struct mp_vo_opts {
    int64_t WinID, android_osd_wid;
    int android_dolby_vision_output;
    struct m_obj_settings *video_driver_list;
};
struct MPOpts { struct mp_vo_opts *vo; };
struct vo { int unused; };
struct mp_decoder_wrapper { int unused; };
struct track { struct mp_decoder_wrapper *dec; };
struct MPContext {
    struct MPOpts *opts;
    struct track *current_track[1][1];
    struct track *android_dolby_vision_direct_failed_track;
    struct vo *video_out;
    void *vo_chain;
    double video_pts;
};
static struct vo output;
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
static void uninit_video_out(struct MPContext *ctx)
{
    destroyed++;
    ctx->video_out = NULL;
    ctx->vo_chain = NULL;
}
static void reinit_video_chain(struct MPContext *ctx)
{
    initialized++;
    assert(ctx->opts->vo->WinID > 0);
    ctx->video_out = &output;
    ctx->vo_chain = &output;
}
static int vo_control(struct vo *vo, int request, void *data)
{
    last_control = request;
    return control_result;
}
static void handle_force_window(struct MPContext *ctx, bool force)
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

int main(void)
{
    struct mp_vo_opts vo_opts = {.WinID = 10, .android_osd_wid = 20};
    struct MPOpts opts = {.vo = &vo_opts};
    struct mp_decoder_wrapper decoder = {0};
    struct track track = {.dec = &decoder};
    struct MPContext ctx = {
        .opts = &opts, .current_track = {{&track}},
        .video_out = &output, .vo_chain = &output, .video_pts = 9,
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
    puts("Android OSD routing, Surface detach/restore and output policy contracts passed");
    return 0;
}
