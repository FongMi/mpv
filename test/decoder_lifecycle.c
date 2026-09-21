#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define mp_assert assert
#define MP_ENHANCEMENT_PAIR_QUEUE_MAX 4
#define MP_WARN(ctx, ...) ((void)(ctx))
#define MP_VERBOSE(ctx, ...) ((void)(ctx))
#define STATUS_SYNCING 0
#define STATUS_EOF 4

struct mp_filter { int unused; };
struct mp_decoder_wrapper { struct mp_filter *f; };
struct output_filter { struct mp_filter *f; };
struct sh_stream { bool lazy, packets; };
struct track {
    struct sh_stream *stream;
    struct mp_decoder_wrapper *dec;
    struct vo_chain *vo_c;
    struct ao_chain *ao_c;
    void *sink;
    bool dec_failed;
};
struct vo_chain { struct track *track; struct output_filter *filter; void *dec_src; };
struct ao_chain { struct track *track; struct output_filter *filter; void *dec_src; };
struct MPContext {
    struct mp_filter *filter_root;
    struct vo_chain *vo_chain;
    struct ao_chain *ao_chain;
    struct track *android_dolby_vision_direct_failed_track;
    int error_playing;
    int video_status, audio_status;
    bool paused;
};
static struct mp_filter filter;
static struct mp_decoder_wrapper decoder = {.f = &filter};
static bool create_ok, decoder_ok, chain_ok, direct_active, direct_wanted;
static int errors, video_uninit, audio_uninit, output_uninit, reinitializations;
static int resets, video_resets, audio_resets, disconnected, force_window;
static int decoder_frees, wakeups;
static struct track *failed_track;

int init_video_decoder(struct MPContext *ctx, struct track *track);
static struct sh_stream *sh_stream_dependent_sibling(struct sh_stream *stream) { return NULL; }
static struct mp_decoder_wrapper *mp_decoder_wrapper_create(struct mp_filter *f,
                                                            struct sh_stream *stream)
{
    return create_ok ? &decoder : NULL;
}
static bool mp_decoder_wrapper_reinit(struct mp_decoder_wrapper *dec) { return decoder_ok; }
static void mp_decoder_wrapper_set_extra_hw_frames(struct mp_decoder_wrapper *dec, int count) {}
static void mp_decoder_wrapper_set_spdif_flag(struct mp_decoder_wrapper *dec, bool enabled) {}
static void mp_pin_disconnect(void *pin) { disconnected++; }
static void error_on_track(struct MPContext *ctx, struct track *track)
{
    errors++;
    failed_track = track;
}
static void uninit_video_chain(struct MPContext *ctx)
{
    video_uninit++;
    if (ctx->vo_chain)
        ctx->vo_chain->track->dec = NULL;
    ctx->vo_chain = NULL;
}
static void uninit_audio_chain(struct MPContext *ctx)
{
    audio_uninit++;
    ctx->ao_chain->track->dec = NULL;
    ctx->ao_chain = NULL;
}
static bool is_android_dolby_vision_direct_output_active(struct MPContext *ctx)
{
    return direct_active;
}
static bool should_use_android_dolby_vision_direct_output(struct MPContext *ctx,
                                                         struct track *track)
{
    return direct_wanted && ctx->android_dolby_vision_direct_failed_track != track;
}
static void uninit_video_out(struct MPContext *ctx)
{
    output_uninit++;
    uninit_video_chain(ctx);
    direct_active = false;
}
static int handle_force_window(struct MPContext *ctx, bool force)
{
    force_window++;
    return 0;
}
static void reinit_video_chain_src(struct MPContext *ctx, struct track *track)
{
    assert(!should_use_android_dolby_vision_direct_output(ctx, track));
    reinitializations++;
    assert(init_video_decoder(ctx, track));
}
static bool vo_chain_init_decoder(struct MPContext *ctx, struct vo_chain *chain)
{
    if (chain_ok)
        chain->track->dec = &decoder;
    return chain_ok;
}
static bool ao_chain_init_decoder(struct MPContext *ctx, struct ao_chain *chain)
{
    if (chain_ok)
        chain->track->dec = &decoder;
    return chain_ok;
}
static void mp_filter_reset(struct mp_filter *f) { resets++; }
static void reset_video_state(struct MPContext *ctx) { video_resets++; ctx->video_status = STATUS_SYNCING; }
static void reset_audio_state(struct MPContext *ctx) { audio_resets++; ctx->audio_status = STATUS_SYNCING; }
static void talloc_free(struct mp_filter *f) { assert(f == &filter); decoder_frees++; }
static bool demux_stream_eof_is_lazy(struct sh_stream *stream) { return stream->lazy; }
static bool demux_stream_has_packets(struct sh_stream *stream) { return stream->packets; }
static void mp_wakeup_core(struct MPContext *ctx) { wakeups++; }

#include "decoder_lifecycle_functions.h"

static void test_init_ownership(bool audio)
{
    struct MPContext ctx = {.filter_root = &filter};
    struct sh_stream stream = {0};
    struct track track = {.stream = &stream, .sink = &stream};
    int (*init)(struct MPContext *, struct track *) =
        audio ? init_audio_decoder : init_video_decoder;
    create_ok = false;
    assert(!init(&ctx, &track));
    assert(track.dec_failed && !track.sink && !errors);
    create_ok = true;
    decoder_ok = false;
    assert(!init(&ctx, &track));
    assert(track.dec_failed && !errors);
    track.dec = NULL;
    decoder_ok = true;
    assert(init(&ctx, &track));
    assert(!track.dec_failed && !errors);
}

static void test_lazy_failure(void)
{
    struct sh_stream stream = {0};
    struct track track = {.stream = &stream};
    struct output_filter output = {.f = &filter};
    struct vo_chain video = {.track = &track, .filter = &output};
    struct ao_chain audio = {.track = &track, .filter = &output};
    struct MPContext ctx = {.filter_root = &filter, .vo_chain = &video, .ao_chain = &audio};
    chain_ok = true;
    reinit_video_decoder(&ctx);
    track.dec = NULL;
    reinit_audio_decoder(&ctx);
    assert(resets == 2 && video_resets == 1 && audio_resets == 1 && !errors);
    track.dec = NULL;
    chain_ok = false;
    reinit_audio_decoder(&ctx);
    assert(!ctx.ao_chain && audio_uninit == 1 && errors == 1 && failed_track == &track);
    direct_active = direct_wanted = true;
    track.dec_failed = true;
    reinit_video_decoder(&ctx);
    assert(!ctx.vo_chain && output_uninit == 1 && reinitializations == 1);
    assert(ctx.android_dolby_vision_direct_failed_track == &track);
    assert(!track.dec_failed && errors == 1 && !ctx.error_playing);
    track.dec = NULL;
    ctx.vo_chain = &video;
    reinit_video_decoder(&ctx);
    assert(!ctx.vo_chain && errors == 2 && failed_track == &track);
    assert(resets == 2 && video_uninit == 2 && force_window == 2);
}

static void test_paused_lazy_restart(void)
{
    for (int audio = 0; audio < 2; audio++) {
        struct sh_stream stream = {.lazy = true};
        struct track track = {.stream = &stream, .dec = &decoder};
        struct output_filter output = {.f = &filter};
        struct vo_chain video = {.track = &track, .filter = &output};
        struct ao_chain sound = {.track = &track, .filter = &output};
        struct MPContext ctx = {
            .vo_chain = audio ? NULL : &video,
            .ao_chain = audio ? &sound : NULL,
            .video_status = STATUS_EOF, .audio_status = STATUS_EOF,
            .paused = true,
        };
        chain_ok = true;
        int freed = decoder_frees, woke = wakeups;
        handle_lazy_streams(&ctx);
        assert(!track.dec && decoder_frees == freed + 1);
        handle_lazy_streams(&ctx);
        assert(!track.dec && wakeups == woke);
        stream.packets = true;
        handle_lazy_streams(&ctx);
        assert(track.dec && wakeups == woke + 1);
        assert((audio ? ctx.audio_status : ctx.video_status) == STATUS_SYNCING);
        assert(ctx.paused);
    }
}

int main(void)
{
    test_init_ownership(false);
    test_init_ownership(true);
    test_lazy_failure();
    test_paused_lazy_restart();
    puts("Decoder init ownership, lazy EOF failure and same-track direct fallback passed.");
    return 0;
}
