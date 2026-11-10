/* SPDX-License-Identifier: LGPL-2.1-or-later */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define mp_assert assert
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MP_TIME_NS_TO_S(ns) ((ns) / 1e9)
#define MP_TIME_S_TO_NS(s) ((s) * INT64_C(1000000000))
#define MP_TIME_MS_TO_NS(ms) ((int64_t)(ms) * 1000000)
#define MP_TRACE(...) ((void)0)
#define MP_STATS(...) ((void)0)
#define stats_time_start(...) ((void)0)
#define stats_time_end(...) ((void)0)
#define VO_CAP_FRAMEDROP (1 << 1)
#define VO_CAP_NORETAIN (1 << 2)
#define VO_CAP_FRAMEOWNER (1 << 5)
#define MP_TARRAY_INSERT_AT(ctx, arr, count, index, value) do { \
    assert((index) == 0 && (count) < 1000); \
    memmove((arr) + 1, (arr), (count) * sizeof(*(arr))); \
    (arr)[0] = (value); (count)++; \
} while (0)

struct vo_frame {
    int64_t pts;
    double duration;
    double vsync_interval, vsync_offset;
    double ideal_frame_vsync, ideal_frame_vsync_duration;
    int num_vsyncs;
    bool display_synced, can_drop, repeat, still;
    void *current;
};


struct vo_internal {
    int lock, wakeup;
    double nominal_vsync_interval, vsync_interval;
    int64_t vsync_samples[1000];
    int num_vsync_samples;
    int64_t num_total_vsync_samples, prev_vsync;
    double base_vsync;
    double next_vsync_deadline;
    int drop_point;
    double estimated_vsync_interval, estimated_vsync_jitter;
    bool expecting_vsync, queue_display_time_only;
    int64_t num_successive_vsyncs, delayed_count;
    int64_t drop_count, flip_queue_offset;
    bool rendering, hasframe, paused, dropped_frame, request_redraw, send_reset;
    bool visible, hasframe_rendered, has_video_frame;
    struct vo_frame *current_frame, *frame_queued;
};

struct vo {
    struct vo_internal *in;
    const struct vo_driver *driver;
};

struct vo_vsync_info;
struct vo_driver {
    int caps;
    bool (*draw_frame)(struct vo *, struct vo_frame *);
    void (*flip_page)(struct vo *);
    void (*get_vsync)(struct vo *, struct vo_vsync_info *);
};

static int64_t clock_now;
static int clock_reads;
static int core_wakeups;
static int64_t mp_time_ns(void)
{
    clock_reads++;
    return clock_now;
}

static void mp_mutex_lock(int *lock)
{
    assert(!*lock);
    *lock = 1;
}

static void mp_mutex_unlock(int *lock)
{
    assert(*lock);
    *lock = 0;
}

static void wakeup_locked(struct vo *vo)
{
    assert(vo->in->lock);
}

static void wakeup_core(struct vo *vo)
{
    assert(vo);
    core_wakeups++;
}

static void forget_frames(struct vo *vo)
{
    assert(vo->in->lock);
    vo->in->current_frame = NULL;
    vo->in->frame_queued = NULL;
}

static struct vo_frame *vo_frame_ref(struct vo_frame *frame)
{
    struct vo_frame *copy = malloc(sizeof(*copy));
    assert(copy);
    *copy = *frame;
    return copy;
}

#define talloc_free free

static void mp_cond_broadcast(int *wakeup) {}
static void update_display_fps(struct vo *vo) {}

static void wait_until(struct vo *vo, int64_t target)
{
    clock_now = MPMAX(clock_now, target);
}

#include "vo_vsync_functions.h"

struct revision {
    int64_t previous, next;
};

// Exact callback values from 17 cold/warm Android presentation revisions.
static const struct revision revisions[] = {
    {1281217084, 1272241978},
    {1345439869, 1345001615},
    {1381679321, 1376360676},
    {1712731744, 1693414817},
    {1857900234, 1833707629},
    {1965070311, 1957639245},
    {2142542994, 2136069920},
    {2144637734, 2128817005},
    {2275632733, 2256524765},
    {2490150363, 2486911690},
    {2689175155, 2686867161},
    {785277162, 778240936},
    {886367552, 876055285},
    {1319935544, 1301011562},
    {1699534713, 1672322446},
    {2101239764, 2099742213},
    {2299110443, 2291434973},
};

static struct vo_internal initial(void)
{
    return (struct vo_internal) {
        .nominal_vsync_interval = 10000000,
        .vsync_interval = 10000000,
        .expecting_vsync = true,
        .hasframe = true,
        .estimated_vsync_jitter = -1,
    };
}

static void sample(struct vo *vo, int64_t time, bool queue_only)
{
    struct vo_vsync_info info = {
        .last_queue_display_time = time,
        .queue_display_time_only = queue_only,
    };
    mp_mutex_lock(&vo->in->lock);
    update_vsync_timing_after_swap(vo, &info);
    mp_mutex_unlock(&vo->in->lock);
}

static void actual_revisions(void)
{
    int failures = 0;
    for (int i = 0; i < (int)(sizeof(revisions) / sizeof(revisions[0])); i++) {
        struct vo_internal in = initial();
        struct vo vo = {.in = &in};
        struct vo_frame frame = {.num_vsyncs = 2, .display_synced = true};
        in.current_frame = &frame;
        in.queue_display_time_only = true;
        in.num_successive_vsyncs = 20;
        in.prev_vsync = in.base_vsync = revisions[i].previous;
        sample(&vo, revisions[i].next, true);
        bool ok = in.base_vsync == revisions[i].next &&
                  in.prev_vsync == revisions[i].next &&
                  in.num_vsync_samples == 0 && in.delayed_count == 0;
        printf("actual revision %d: %s\n", i + 1, ok ? "PASS" : "FAIL");
        failures += !ok;
        if (ok) {
            clock_now = revisions[i].next - 1000000;
            assert(fabs(vo_get_delay(&vo) - .021) < 1e-12);
            in.rendering = true;
            assert(fabs(vo_get_delay(&vo) - .031) < 1e-12);
            in.rendering = false;
            assert(still_displaying(&vo));
            frame.num_vsyncs = 0;
            assert(!still_displaying(&vo));
        }
    }
    fflush(stdout);
    assert(failures == 0);
}

static void modes_and_lifecycle(void)
{
    struct vo_internal in = initial();
    struct vo vo = {.in = &in};
    for (int i = 1; i <= 30; i++)
        sample(&vo, i * INT64_C(10000000), false);
    assert(in.num_vsync_samples == 20 && in.num_total_vsync_samples == 20);
    sample(&vo, 290000000, false);
    assert(in.prev_vsync == 0 && in.base_vsync == 0);

    sample(&vo, 400000000, true);
    assert(in.queue_display_time_only && in.base_vsync == 400000000);
    assert(!in.num_vsync_samples && !in.num_total_vsync_samples);
    assert(!in.drop_point && !in.num_successive_vsyncs);
    assert(in.estimated_vsync_interval == 0 && in.estimated_vsync_jitter == -1);
    assert(in.vsync_interval == in.nominal_vsync_interval);
    sample(&vo, 390000000, true);
    sample(&vo, 410000000, true);
    sample(&vo, 380000000, true);
    assert(in.base_vsync == 380000000 && !in.num_vsync_samples);

    // A missing estimate uses the existing legacy fallback (false), then
    // returning feedback switches back without mixing cadence histories.
    sample(&vo, 420000000, false);
    assert(!in.queue_display_time_only && in.num_successive_vsyncs == 1);
    assert(!in.num_vsync_samples && in.base_vsync == 420000000);
    sample(&vo, 430000000, true);
    assert(in.queue_display_time_only && !in.num_successive_vsyncs);

    struct vo_frame frame = {.num_vsyncs = 2, .display_synced = true};
    in.current_frame = &frame;
    vo_set_paused(&vo, true);
    assert(!in.base_vsync && !in.expecting_vsync);
    in.num_vsync_samples = 3;
    in.num_total_vsync_samples = 3;
    sample(&vo, 440000000, false);
    assert(!in.queue_display_time_only && !in.base_vsync && !in.num_vsync_samples);
    in.num_vsync_samples = 4;
    in.num_total_vsync_samples = 4;
    sample(&vo, 450000000, true);
    assert(in.queue_display_time_only && !in.base_vsync);
    assert(!in.num_vsync_samples && !in.num_total_vsync_samples);
    vo_set_paused(&vo, false);
    in.expecting_vsync = true;
    sample(&vo, 460000000, true);
    assert(in.base_vsync == 460000000);
    vo_discard_timing_info(&vo);
    assert(!in.base_vsync && !in.expecting_vsync);
    clock_reads = 0;
    assert(vo_get_delay(&vo) == 0 && clock_reads == 0);
    in.expecting_vsync = true;
    sample(&vo, 470000000, true);
    vo_seek_reset(&vo);
    assert(in.send_reset && !in.current_frame && !in.base_vsync);

    in.current_frame = &frame;
    in.expecting_vsync = true;
    in.nominal_vsync_interval = 0;
    in.vsync_interval = 1;
    sample(&vo, 480000000, true);
    assert(in.vsync_interval == 1 && vo_get_delay(&vo) == 0);
    in.nominal_vsync_interval = 10000000;
    in.vsync_interval = 10000000;
    sample(&vo, 490000000, true);
    frame.display_synced = false;
    assert(vo_get_delay(&vo) == 0);

    struct vo_internal unknown = initial();
    struct vo other = {.in = &unknown};
    frame.display_synced = true;
    unknown.current_frame = &frame;
    unknown.nominal_vsync_interval = 0;
    unknown.vsync_interval = 1;
    sample(&other, 500000000, true);
    assert(unknown.queue_display_time_only && unknown.vsync_interval == 1);
    assert(vo_get_delay(&other) == 0);
}

static int64_t flip_cost;
static int draws;
static double phase;

static bool draw(struct vo *vo, struct vo_frame *frame)
{
    draws++;
    phase = frame->ideal_frame_vsync;
    return true;
}

static void flip(struct vo *vo)
{
    clock_now += flip_cost;
}

static void estimate(struct vo *vo, struct vo_vsync_info *info)
{
    info->queue_display_time_only = true;
    info->last_queue_display_time = clock_now + 10000000;
}

static const struct vo_driver estimated_driver = {
    .draw_frame = draw,
    .flip_page = flip,
    .get_vsync = estimate,
};

static void queue_source(struct vo_internal *in, bool can_drop)
{
    struct vo_frame frame = {
        .num_vsyncs = 2,
        .vsync_interval = .01,
        .ideal_frame_vsync_duration = .01,
        .display_synced = true,
        .can_drop = can_drop,
    };
    assert(!in->frame_queued);
    assert(!in->current_frame || !in->current_frame->num_vsyncs);
    in->frame_queued = vo_frame_ref(&frame);
}

static void paced_slots(void)
{
    struct vo_internal in = initial();
    struct vo vo = {.in = &in, .driver = &estimated_driver};
    sample(&vo, 990000000, true);
    clock_now = 1000000000;
    flip_cost = 1000000;
    draws = 0;
    queue_source(&in, true);

    assert(render_frame(&vo));
    assert(phase == 0 && in.current_frame->num_vsyncs == 1);
    assert(!render_frame(&vo));
    assert(phase == .01 && draws == 2 && clock_now == 1002000000);

    // Present blocks across several refreshes of the next source frame.
    flip_cost = 62000000;
    queue_source(&in, true);
    assert(render_frame(&vo));
    assert(phase == 0 && clock_now == 1072000000);
    int wakeups = core_wakeups;
    assert(!render_frame(&vo));
    assert(draws == 3 && in.current_frame->num_vsyncs == 0);
    assert(core_wakeups == wakeups + 1 && in.delayed_count == 1);

    // The next source and its repeat both draw at their own phases.
    flip_cost = 1000000;
    queue_source(&in, true);
    assert(render_frame(&vo));
    assert(phase == 0 && clock_now == 1073000000);
    assert(!render_frame(&vo));
    assert(phase == .01 && draws == 5 && clock_now == 1074000000);
    free(in.current_frame);
}

static void paced_slot_discontinuities(void)
{
    struct vo_internal in = initial();
    struct vo vo = {.in = &in, .driver = &estimated_driver};
    sample(&vo, 990000000, true);
    clock_now = 1000000000;
    flip_cost = 1000000;
    queue_source(&in, true);
    assert(render_frame(&vo));

    // An expired repeat must release the queue for the next source.
    clock_now = 1021000000;
    assert(!render_frame(&vo));
    assert(in.delayed_count == 1);
    queue_source(&in, true);
    // Even when the next source is late, draw it before skipping its repeat.
    clock_now = 1041000000;
    assert(render_frame(&vo));
    assert(in.delayed_count == 1 && phase == 0);
    assert(!render_frame(&vo));
    assert(in.delayed_count == 2 && phase == 0);

    // An application that disables frame dropping displays the source.
    queue_source(&in, false);
    clock_now = 1060000000;
    assert(render_frame(&vo));
    assert(phase == 0 && in.delayed_count == 2);
    assert(!render_frame(&vo));

    // Pausing resets the slot deadlines. Resuming uses the current clock.
    vo_set_paused(&vo, true);
    assert(!in.next_vsync_deadline);
    vo_set_paused(&vo, false);
    clock_now = 2000000000;
    queue_source(&in, true);
    assert(render_frame(&vo));
    assert(phase == 0 && in.delayed_count == 2);
    assert(!render_frame(&vo));

    free(in.current_frame);
    in.current_frame = NULL;
    vo_seek_reset(&vo);
    assert(!in.next_vsync_deadline);
}

int main(void)
{
    actual_revisions();
    modes_and_lifecycle();
    paced_slots();
    paced_slot_discontinuities();
    puts("VO queue-only feedback: revisions, legacy, transitions and lifecycle passed");
    return 0;
}
