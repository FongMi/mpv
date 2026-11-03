// Actual core/VO lifecycle functions; codec generation and dispatch services
// are stubbed. Backend references are tested separately in vo_gpu_next.c.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK mp_mutex;
typedef CONDITION_VARIABLE condition;
#define mp_mutex_lock AcquireSRWLockExclusive
#define mp_mutex_unlock ReleaseSRWLockExclusive
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t mp_mutex;
typedef pthread_cond_t condition;
#define mp_mutex_lock pthread_mutex_lock
#define mp_mutex_unlock pthread_mutex_unlock
#endif

#define MP_NOPTS_VALUE (-1e20)
#define talloc_free free
#define TA_FREEP(p) do { free(*(p)); *(p) = NULL; } while (0)

struct vo_frame { int num_vsyncs; bool display_synced; };
struct vo_internal {
    mp_mutex lock;
    void *dispatch;
    bool hasframe, hasframe_rendered, send_reset;
    int drop_count, delayed_count, current_frame_id;
    struct vo_frame *current_frame, *frame_queued;
};
struct vo;
struct vo_driver { int (*control)(struct vo *, int, void *); };
struct vo { struct vo_internal *in; const struct vo_driver *driver; };
struct seek_params { int unused; };
struct track { void *dec, *d_sub; };
struct MPContext {
    void *filter_root, *demuxer, *encode_lavc_ctx;
    struct track **tracks;
    struct vo *video_out;
    int num_tracks, play_dir, step_frames, cache_buffer;
    bool paused_for_cache, hrseek_active, hrseek_lastframe, hrseek_backstep;
    bool ab_loop_clip, restart_complete;
    double playback_pts, cache_update_pts;
    struct seek_params current_seek;
};
static struct {
    mp_mutex lock;
    condition wake;
    bool entered, resume, mapped, dispatch, done;
    bool map_valid, flush_before_draw;
    int serial, buffer_serial, controls, flushes, control_result;
    void (*fn)(void *);
    void *arg;
} worker;

static void signal_worker(void)
{
#ifdef _WIN32
    WakeAllConditionVariable(&worker.wake);
#else
    pthread_cond_broadcast(&worker.wake);
#endif
}

static void wait_worker(void)
{
#ifdef _WIN32
    assert(SleepConditionVariableSRW(&worker.wake, &worker.lock, 3000, 0));
#else
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += 3;
    assert(pthread_cond_timedwait(&worker.wake, &worker.lock, &until) == 0);
#endif
}

#ifdef _WIN32
static DWORD WINAPI render_worker(void *unused)
#else
static void *render_worker(void *unused)
#endif
{
    mp_mutex_lock(&worker.lock);
    worker.entered = true;
    signal_worker();
    while (!worker.resume)
        wait_worker();
    // Mapping the current draw must precede the decoder generation change.
    worker.map_valid = worker.serial == worker.buffer_serial;
    worker.mapped = true;
    signal_worker();
    void (*fn)(void *) = worker.dispatch ? worker.fn : NULL;
    void *arg = worker.arg;
    mp_mutex_unlock(&worker.lock);
    if (fn)
        fn(arg);
    mp_mutex_lock(&worker.lock);
    worker.done = true;
    signal_worker();
    mp_mutex_unlock(&worker.lock);
    return 0;
}

static void mp_dispatch_run(void *queue, void (*fn)(void *), void *arg)
{
    mp_mutex_lock(&worker.lock);
    worker.fn = fn;
    worker.arg = arg;
    worker.dispatch = worker.resume = true;
    signal_worker();
    while (!worker.done)
        wait_worker();
    mp_mutex_unlock(&worker.lock);
}

static void reset_vsync_timings(struct vo *vo) {}
static void wakeup_locked(struct vo *vo) {}
static void vo_seek_reset(struct vo *vo);
static void reset_video_state(struct MPContext *ctx)
{
    if (ctx->video_out)
        vo_seek_reset(ctx->video_out);
}
static void mp_filter_reset(void *filter)
{
    mp_mutex_lock(&worker.lock);
    worker.flushes++;
    worker.flush_before_draw = !worker.mapped;
    worker.serial++;
    worker.resume = true;
    signal_worker();
    while (!worker.done)
        wait_worker();
    mp_mutex_unlock(&worker.lock);
}
static void reset_audio_state(struct MPContext *ctx) {}
static void reset_subtitle_state(struct MPContext *ctx) {}
static void demux_nav_refresh(void *demuxer) {}
static void mp_decoder_wrapper_set_play_dir(void *decoder, int dir) {}
static void sub_set_play_dir(void *sub, int dir) {}
static void update_internal_pause_state(struct MPContext *ctx) {}
static void encode_lavc_discontinuity(void *encoder) {}
static void update_core_idle_state(struct MPContext *ctx) {}

#include "vo_decoder_flush_functions.h"

static int retire_decoder_outputs(struct vo *vo, int request, void *data)
{
    assert(request == VOCTRL_PREPARE_DECODER_FLUSH);
    assert(worker.mapped && worker.serial == worker.buffer_serial);
    assert(!vo->in->frame_queued && data == vo->in->current_frame);
    // The backend is called outside the core VO mutex.
    mp_mutex_lock(&vo->in->lock);
    mp_mutex_unlock(&vo->in->lock);
    worker.controls++;
    return worker.control_result;
}

static void test_flush(int result, bool current)
{
    worker.serial = worker.buffer_serial = 1;
    worker.entered = worker.resume = worker.mapped = worker.dispatch = false;
    worker.done = worker.map_valid = worker.flush_before_draw = false;
    worker.controls = worker.flushes = 0;
    worker.control_result = result;
    struct vo_internal in = {0};
#ifndef _WIN32
    assert(pthread_mutex_init(&in.lock, NULL) == 0);
#endif
    const struct vo_driver driver = {.control = retire_decoder_outputs};
    struct vo vo = {.in = &in, .driver = &driver};
    in.frame_queued = calloc(1, sizeof(*in.frame_queued));
    if (current)
        in.current_frame = calloc(1, sizeof(*in.current_frame));
    struct vo_frame *old = in.current_frame;
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, render_worker, NULL, 0, NULL);
    assert(thread);
#else
    pthread_t thread;
    assert(pthread_create(&thread, NULL, render_worker, NULL) == 0);
#endif
    mp_mutex_lock(&worker.lock);
    while (!worker.entered)
        wait_worker();
    mp_mutex_unlock(&worker.lock);
    struct MPContext ctx = {.video_out = &vo};
    reset_playback_state(&ctx);
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    assert(pthread_join(thread, NULL) == 0);
#endif
    assert(worker.map_valid && !worker.flush_before_draw);
    assert(worker.controls == 1 && worker.flushes == 1);
    assert(!in.frame_queued && in.send_reset);
    assert(in.current_frame == (result == VO_TRUE ? NULL : old));
    // A consuming current cannot be selected by a post-reset redraw. Other
    // drivers retain their original redraw frame and reset repeat schedule.
    if (in.current_frame)
        assert(!in.current_frame->num_vsyncs && !in.current_frame->display_synced);
    free(in.current_frame);
#ifndef _WIN32
    pthread_mutex_destroy(&in.lock);
#endif
}

int main(void)
{
#ifndef _WIN32
    assert(pthread_mutex_init(&worker.lock, NULL) == 0);
    assert(pthread_cond_init(&worker.wake, NULL) == 0);
#endif
    test_flush(VO_TRUE, true);
    test_flush(VO_TRUE, false); // Queued only, no current or mapper required.
    test_flush(VO_FALSE, false);
    test_flush(VO_FALSE, true);
    test_flush(VO_NOTIMPL, true); // Other VOs retain their existing redraw contract.
#ifndef _WIN32
    pthread_cond_destroy(&worker.wake);
    pthread_mutex_destroy(&worker.lock);
#endif
    puts("decoder flush waits for VO retirement; ordinary redraw is preserved");
    return 0;
}
