/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "test_utils.h"

#define MP_TRACE(...) ((void)0)
#define MP_STATS(...) ((void)0)

// Only external AO/VO, speed-policy and notification dependencies are stubbed.
// The complete scheduler, its thresholds and status values are extracted.
// Video sync modes and arithmetic helpers come from common/common.h.
struct MPOpts {
    double playback_speed;
};

struct vo_opts {
    int video_sync;
};

struct vo {
    struct vo_opts *opts;
    bool visible;
    double interval, delay;
    int discards;
};

struct vo_chain {
    bool is_sparse;
};

struct frame_info {
    double approx_duration;
    int num_vsyncs;
};

struct MPContext {
    struct MPOpts *opts;
    struct vo *video_out;
    struct vo_chain *vo_chain;
    bool display_sync_active, spdif;
    double display_sync_error, audio_drift_compensation, avd_filtered;
    double speed_factor_v, speed_factor_a, last_av_difference;
    double total_avsync_change, time_frame;
    int num_past_frames, mistimed_frames_total, video_status;
    int speed_updates, drift_updates;
    struct frame_info past_frames[2];
};

struct vo_frame {
    bool can_drop, display_synced;
    double vsync_interval, vsync_offset, ideal_frame_duration;
    double ideal_frame_vsync, ideal_frame_vsync_duration, approx_duration;
    int num_vsyncs;
};

static bool vo_is_visible(struct vo *vo)
{
    return vo->visible;
}

static bool using_spdif_passthrough(struct MPContext *ctx)
{
    return ctx->spdif;
}

static double vo_get_vsync_interval(struct vo *vo)
{
    return vo->interval * 1e9;
}

static double find_best_speed(struct MPContext *ctx, double vsync)
{
    return 1;
}

static double vo_get_delay(struct vo *vo)
{
    return vo->delay;
}

static void vo_discard_timing_info(struct vo *vo)
{
    vo->discards++;
}

static void update_av_diff(struct MPContext *ctx, double time)
{
}

static void adjust_audio_drift_compensation(struct MPContext *ctx, double vsync)
{
    ctx->drift_updates++;
}

static void update_playback_speed(struct MPContext *ctx)
{
    ctx->speed_updates++;
}

#include "player_video_functions.h"

static void test_timing(double hz, double duration, double speed, int request,
                        double offset, bool can_drop, int past_frames)
{
    struct MPOpts opts = {.playback_speed = speed};
    struct vo_opts vo_opts = {.video_sync = VS_DISP_VDROP};
    struct vo vo = {
        .opts = &vo_opts,
        .visible = true,
        .interval = 1 / hz,
        .delay = .1,
    };
    struct vo_chain chain = {0};
    struct MPContext ctx = {
        .opts = &opts,
        .video_out = &vo,
        .vo_chain = &chain,
        .display_sync_active = true,
        .display_sync_error = offset / hz,
        .num_past_frames = past_frames,
        .video_status = STATUS_PLAYING,
        .past_frames = {{.approx_duration = duration}},
    };
    ctx.last_av_difference = request
        ? -(request + (request > 0 ? .125 : -.125)) / hz : 0;
    struct MPContext reference = ctx;
    struct vo_frame normal = {0};
    struct vo_frame frame = {.can_drop = can_drop};
    handle_display_sync_frame(&reference, &normal);
    handle_display_sync_frame(&ctx, &frame);

    int base = MPMAX(lrint((duration / speed + offset / hz) * hz), 0);
    int repeat = request;
    if (!can_drop || fabs(ctx.last_av_difference) < .020)
        repeat = 0;
    repeat = MPCLAMP(repeat, -base, base * 10);
    int count = base + repeat;
    if (past_frames <= 1)
        count = MPMAX(count, 1);
    double expected = normal.ideal_frame_vsync_duration;
    if (repeat > 0)
        expected *= normal.num_vsyncs / (double)count;

    assert_int_equal(frame.num_vsyncs, count);
    assert_float_equal(frame.ideal_frame_vsync, normal.ideal_frame_vsync, 1e-12);
    assert_float_equal(frame.ideal_frame_vsync_duration, expected, 1e-12);
    assert_float_equal(ctx.display_sync_error, reference.display_sync_error, 1e-12);
    assert_float_equal(ctx.time_frame, reference.time_frame + repeat / hz, 1e-12);
    assert_int_equal(ctx.mistimed_frames_total, repeat != 0);
    if (repeat > 0) {
        assert_float_equal(frame.num_vsyncs * frame.ideal_frame_vsync_duration,
                           normal.num_vsyncs * normal.ideal_frame_vsync_duration,
                           1e-12);
        // Match VO's repeated accumulation, including a signed starting phase.
        double position = frame.ideal_frame_vsync;
        for (int n = 0; n < count; n++) {
            assert_float_equal(position, normal.ideal_frame_vsync + n * expected,
                               1e-12);
            position += frame.ideal_frame_vsync_duration;
        }
    }
}

static void test_admission(void)
{
    struct MPOpts opts = {.playback_speed = 1};
    struct vo_opts vo_opts = {.video_sync = VS_DISP_VDROP};
    struct vo vo = {.opts = &vo_opts, .visible = true, .interval = -1};
    struct vo_chain chain = {0};
    struct MPContext ctx = {
        .opts = &opts,
        .video_out = &vo,
        .vo_chain = &chain,
        .num_past_frames = 3,
        .video_status = STATUS_PLAYING,
        .past_frames = {{.approx_duration = 1.0 / 30}},
    };
    struct vo_frame frame = {0};
    handle_display_sync_frame(&ctx, &frame);
    assert_false(frame.display_synced);
    vo.interval = 1.0 / 120;
    vo.visible = false;
    handle_display_sync_frame(&ctx, &frame);
    assert_false(frame.display_synced);
    vo.visible = true;
    vo_opts.video_sync = VS_DISP_RESAMPLE;
    ctx.spdif = true;
    handle_display_sync_frame(&ctx, &frame);
    assert_false(frame.display_synced);
}

int main(void)
{
    const int requests[] = {0, 1, 2, 3, 4, -3, -99, 99};
    for (size_t n = 0; n < MP_ARRAY_SIZE(requests); n++)
        test_timing(120, 1.0 / 30, 1, requests[n], 0, true, 3);
    test_timing(30, 4.0 / 30, 1, 1, 0, true, 3);
    test_timing(30, 4.0 / 30, 1, 2, 0, true, 3);
    test_timing(120, 1.0 / 30, 1, 4, .2, true, 3);
    test_timing(120, 1.0 / 30, 1, 4, -.2, true, 3);
    test_timing(120, 1.0 / 30, 1, 4, 0, false, 3);
    // Preserve the rounded no-repeat span, not an invented exact frame span.
    test_timing(120, 3.6 / 120, 1, 4, .2, true, 3);
    test_timing(120, 1.0 / 30, 2, 4, 0, true, 3);
    // A zero pre-repeat count clamps positive repeats away before the floor.
    test_timing(120, .4 / 120, 1, 4, 0, true, 1);
    test_admission();
    return 0;
}
