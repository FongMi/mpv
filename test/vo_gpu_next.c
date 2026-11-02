// GPU/codec stubs for the extracted gpu-next slot lifecycle. The parameter
// structures and comparison functions are extracted from mpv as well.
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../video/out/gpu_next/motion_clock.h"

#define mp_assert assert
#define MP_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MP_ERR(p, ...) ((void)(p))
#define MP_INFO(p, ...) ((void)(p))

enum mp_imgfmt { IMGFMT_MEDIACODEC, IMGFMT_RGB0, IMGFMT_YUV420P };
enum pl_color_primaries { PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_BT_2020 };
enum pl_color_transfer { PL_COLOR_TRC_BT_1886, PL_COLOR_TRC_PQ };
enum pl_color_system { PL_COLOR_SYSTEM_BT_709, PL_COLOR_SYSTEM_DOLBYVISION };
enum mp_csp_light { MP_CSP_LIGHT_AUTO };
enum pl_chroma_location { PL_CHROMA_LEFT, PL_CHROMA_CENTER };
enum mp_stereo3d_mode { MP_STEREO3D_MONO };

struct pl_hdr_metadata { float max_luma; };
struct pl_color_space {
    enum pl_color_primaries primaries;
    enum pl_color_transfer transfer;
    struct pl_hdr_metadata hdr;
};
struct pl_color_repr {
    enum pl_color_system sys;
    const void *dovi;
};
struct mp_rect { int x0, y0, x1, y1; };
struct mp_pass_perf { int count; };
typedef void *pl_tex;

#include "vo_gpu_next_contracts.h"

struct mp_image {
    struct mp_image_params params;
    bool consumed; // MediaCodec output buffers may only be released once.
};
struct ra { bool uses_pl; };
struct ra_ctx { struct ra *ra; };
struct ra_hwdec { struct ra *ra; };
struct timer_pool { int unused; };
struct gl_next_opts { int motion_fps; };
struct priv {
    void *gpu, *stats;
    struct ra_ctx *ra_ctx;
    void *motion;
    struct mp_motion_clock motion_clock;
    int motion_fps;
    enum motion_state motion_state;
    bool paused;
    uint64_t motion_captured, motion_presented;
    int64_t motion_epoch;
    struct gl_next_opts *next_opts;
    bool motion_bypass_reported;
    uint64_t motion_last_captured, motion_last_presented;
    struct mp_motion_stats motion_last_stats;
    int64_t motion_stats_time;
};
struct vo_frame {
    double ideal_frame_duration;
    double vsync_interval;
    double approx_duration;
};

enum { CONF_TYPE_INT, CONF_TYPE_INT64, CONF_TYPE_STRING, CONF_TYPE_BOOL,
       CONF_TYPE_NODE };
enum { M_PROPERTY_GET, M_PROPERTY_GET_TYPE, M_PROPERTY_OK = 1,
       M_PROPERTY_UNAVAILABLE = -1, VOCTRL_GET_MOTION_INFO };
struct m_option { int type; };
struct m_property { int unused; };
struct m_sub_property {
    const char *name;
    struct m_option type;
    union { int int_; int64_t int64; char *string; bool bool_; } value;
    bool unavailable;
};
struct vo { struct priv *priv; };
typedef struct { struct vo *video_out; } MPContext;

static struct mp_motion_stats motion_stats;
static struct m_sub_property motion_properties[24];
static bool gpu_supported = true;
static bool motion_control_available = true;
static int motion_control_calls;
static bool motion_create_fail;
static int motion_create_calls, motion_destroy_calls, motion_queue_resets;

static struct mp_motion_stats mp_motion_get_stats(void *motion)
{
    return motion ? motion_stats : (struct mp_motion_stats){0};
}

static bool mp_motion_supported(void *gpu)
{
    return gpu && gpu_supported;
}

static void *mp_motion_create(void *gpu, int max_dimension)
{
    assert(gpu && max_dimension == 480);
    motion_create_calls++;
    motion_stats = (struct mp_motion_stats){0};
    return motion_create_fail ? NULL : (void *)2;
}

static void mp_motion_destroy(void **motion)
{
    motion_destroy_calls++;
    *motion = NULL;
}

static void reset_frame_queue(struct priv *p)
{
    (void)p;
    motion_queue_resets++;
}

static int64_t mp_time_ns(void) { return 123456789; }
static void get_motion_info(struct priv *p, struct voctrl_motion_info *info);

static int vo_control(struct vo *vo, int request, void *data)
{
    assert(request == VOCTRL_GET_MOTION_INFO);
    motion_control_calls++;
    if (!motion_control_available)
        return -1;
    get_motion_info(vo->priv, data);
    return 1;
}

static int m_property_read_sub(const struct m_sub_property *props, int action,
                               void *arg)
{
    (void)arg;
    assert(action == M_PROPERTY_GET);
    int count = 0;
    do {
        assert(count < MP_ARRAY_SIZE(motion_properties));
        motion_properties[count] = props[count];
    } while (props[count++].name);
    return M_PROPERTY_OK;
}

static struct {
    int creates, frees, maps, unmaps, timer_creates, timer_destroys;
    bool fail_create;
} calls;

static bool pl_color_space_equal(const struct pl_color_space *a,
                                  const struct pl_color_space *b)
{
    return a->primaries == b->primaries && a->transfer == b->transfer &&
           a->hdr.max_luma == b->hdr.max_luma;
}

static bool pl_color_repr_equal(const struct pl_color_repr *a,
                                 const struct pl_color_repr *b)
{
    return a->sys == b->sys && a->dovi == b->dovi;
}

static bool mp_rect_equals(const struct mp_rect *a, const struct mp_rect *b)
{
    return a->x0 == b->x0 && a->y0 == b->y0 &&
           a->x1 == b->x1 && a->y1 == b->y1;
}

static void *ra_pl_get(struct ra *ra) { return ra->uses_pl ? ra : NULL; }
static void pl_tex_destroy(void *gpu, pl_tex *tex)
{
    (void)gpu;
    *tex = NULL;
}

static void ra_hwdec_mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    if (mapper->src) {
        calls.unmaps++;
        mapper->src = NULL;
    }
}

static void ra_hwdec_mapper_free(struct ra_hwdec_mapper **mapper)
{
    if (*mapper) {
        ra_hwdec_mapper_unmap(*mapper);
        calls.frees++;
        free(*mapper);
        *mapper = NULL;
    }
}

static struct ra_hwdec_mapper *ra_hwdec_mapper_create(
    struct ra_hwdec *hwdec, const struct mp_image_params *params)
{
    calls.creates++;
    if (calls.fail_create)
        return NULL;
    struct ra_hwdec_mapper *mapper = calloc(1, sizeof(*mapper));
    assert(mapper);
    mapper->owner = hwdec;
    mapper->ra = hwdec->ra;
    mapper->src_params = mapper->dst_params = *params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    return mapper;
}

static int ra_hwdec_mapper_map(struct ra_hwdec_mapper *mapper,
                               struct mp_image *image)
{
    calls.maps++;
    if (image->consumed)
        return -1;
    image->consumed = true;
    mapper->src = image;
    mapper->dst_params_ready = true;
    return 0;
}

static struct timer_pool *timer_pool_create(struct ra *ra)
{
    (void)ra;
    calls.timer_creates++;
    return calloc(1, sizeof(struct timer_pool));
}
static void timer_pool_destroy(struct timer_pool *timer)
{
    if (timer)
        calls.timer_destroys++;
    free(timer);
}
static void timer_pool_start(struct timer_pool *timer) { (void)timer; }
static void timer_pool_stop(struct timer_pool *timer) { (void)timer; }
static struct mp_pass_perf timer_pool_measure(struct timer_pool *timer)
{
    (void)timer;
    return (struct mp_pass_perf){0};
}
static void stats_time_start(void *stats, const char *name)
{
    (void)stats;
    (void)name;
}
static void stats_time_end(void *stats, const char *name)
{
    (void)stats;
    (void)name;
}

#include "vo_gpu_next_functions.h"

static const struct mp_image_params original = {
    .imgfmt = IMGFMT_MEDIACODEC,
    .hw_subfmt = IMGFMT_YUV420P,
    .w = 1920, .h = 1080, .p_w = 1, .p_h = 1,
    .crop = {0, 0, 1920, 1080},
};

static void test_aspect(struct priv *p, struct ra_hwdec *hwdec)
{
    memset(&calls, 0, sizeof(calls));
    struct hwdec_slot slot = {0};
    struct mp_image image = {.params = original};
    assert(slot_preload(p, &slot, hwdec, &image, &image, true) == 1);
    struct ra_hwdec_mapper *mapper = slot.mapper;
    struct timer_pool *timer = slot.timer;
    // Mapping may expose dimensions and a representation unlike the source.
    mapper->dst_params.w = 2048;
    mapper->dst_params.h = 1088;
    mapper->dst_params.crop.x1 = 1920;
    mapper->dst_params.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
    int texture;
    slot.tex[0] = &texture;

    // Ratio changes reconfigure the queue while the same output is mapped.
    // Alternate metadata as old/new queue entries reach the same slot.
    const int ratios[][2] = {{5, 4}, {1, 1}, {5, 4}, {0, 0}, {1, 1}};
    for (int n = 0; n < (int)MP_ARRAY_SIZE(ratios); n++) {
        struct mp_image_params params = original;
        params.p_w = ratios[n][0];
        params.p_h = ratios[n][1];
        slot.acquired = n == 1;
        assert(slot_reconfig(p, &slot, hwdec, &params));
        assert(slot.acquired == (n == 1));
        slot.acquired = false;
        assert(calls.creates == 1 && calls.frees == 0);
        assert(calls.timer_creates == 1 && calls.timer_destroys == 0);
        assert(calls.maps == 1 && calls.unmaps == 0);
        assert(slot.mapper == mapper && slot.timer == timer);
        assert(slot.owner == &image && slot.mapper->src == &image);
        assert(slot.tex[0] == &texture);
        assert(mapper->src_params.p_w == params.p_w);
        assert(mapper->src_params.p_h == params.p_h);
        assert(mapper->dst_params.p_w == params.p_w);
        assert(mapper->dst_params.p_h == params.p_h);
        assert(mapper->dst_params.imgfmt == IMGFMT_RGB0);
        assert(mapper->dst_params.w == 2048 && mapper->dst_params.h == 1088);
        assert(mapper->dst_params.crop.x1 == 1920);
        assert(mapper->dst_params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION);
        image.params = params;
        assert(slot_preload(p, &slot, hwdec, &image, &image, true) == 1);
    }

    struct mp_image next = {.params = image.params};
    assert(slot_preload(p, &slot, hwdec, &next, &next, true) == 1);
    assert(calls.maps == 2 && calls.unmaps == 1);
    slot_release_owner(p, &slot, &image);
    assert(slot.owner == &next);
    slot_release_owner(p, &slot, &next);
    assert(!slot.owner && !slot.mapper->src);
    slot_uninit(p, &slot);
    assert(calls.frees == 1 && calls.timer_destroys == 1);
}

static void test_hwdec_change(struct priv *p, struct ra_hwdec *hwdec)
{
    memset(&calls, 0, sizeof(calls));
    struct hwdec_slot slot = {0};
    struct mp_image image = {.params = original};
    assert(slot_preload(p, &slot, hwdec, &image, &image, false) == 1);
    struct ra_hwdec replacement = {.ra = hwdec->ra};
    assert(slot_reconfig(p, &slot, &replacement, &original));
    assert(calls.creates == 2 && calls.frees == 1 && calls.unmaps == 1);
    assert(!slot.owner && slot.mapper->owner == &replacement);
    slot_uninit(p, &slot);
}

static void test_format(struct priv *p, struct ra_hwdec *hwdec)
{
    struct mp_image_params changes[] = {
        original, original, original, original, original, original, original,
    };
    changes[0].w = 1280;
    changes[1].h = 720;
    changes[2].imgfmt = IMGFMT_YUV420P;
    changes[3].hw_subfmt = IMGFMT_RGB0;
    changes[4].color.transfer = PL_COLOR_TRC_PQ;
    changes[5].crop.x0 = 8;
    changes[6].rotate = 90;
    for (int n = 0; n < (int)MP_ARRAY_SIZE(changes); n++) {
        memset(&calls, 0, sizeof(calls));
        struct hwdec_slot slot = {0};
        struct mp_image image = {.params = original};
        assert(slot_preload(p, &slot, hwdec, &image, &image, false) == 1);
        assert(slot_reconfig(p, &slot, hwdec, &changes[n]));
        assert(calls.creates == 2 && calls.frees == 1);
        assert(calls.unmaps == 1 && !slot.owner);
        assert(calls.timer_creates == 2 && calls.timer_destroys == 1);
        slot_uninit(p, &slot);
    }
}

static void test_metadata_and_failure(struct priv *p, struct ra_hwdec *hwdec)
{
    memset(&calls, 0, sizeof(calls));
    struct hwdec_slot slot = {0};
    assert(slot_reconfig(p, &slot, hwdec, &original));
    struct ra_hwdec_mapper *mapper = slot.mapper;
    int dovi;
    struct mp_image_params params = original;
    params.repr.dovi = &dovi;
    params.color.hdr.max_luma = 1000;
    assert(slot_reconfig(p, &slot, hwdec, &params));
    assert(calls.creates == 1 && slot.mapper == mapper);
    assert(mapper->src_params.repr.dovi == &dovi);
    assert(mapper->dst_params.repr.dovi == &dovi);
    assert(mapper->src_params.color.hdr.max_luma == 1000);
    assert(mapper->dst_params.color.hdr.max_luma == 1000);

    params.w = 1280;
    calls.fail_create = true;
    assert(!slot_reconfig(p, &slot, hwdec, &params));
    assert(!slot.mapper && !slot.timer && !slot.owner);
    slot_uninit(p, &slot);
}

static const struct m_sub_property *motion_property(const char *name)
{
    for (int n = 0; motion_properties[n].name; n++) {
        if (strcmp(motion_properties[n].name, name) == 0)
            return &motion_properties[n];
    }
    abort();
}

static void check_motion_event(const char *const *properties)
{
    for (int n = 0; properties[n]; n++) {
        if (strcmp(properties[n], "motion-interpolation-info") == 0)
            return;
    }
    abort();
}

static void test_motion_info(void)
{
    check_motion_event(event_MPV_EVENT_TICK);
    check_motion_event(event_MPV_EVENT_VIDEO_RECONFIG);
    check_motion_event(event_MP_EVENT_CORE_IDLE);
    MPContext context = {0};
    struct m_option type;
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET_TYPE, &type) == M_PROPERTY_OK);
    assert(type.type == CONF_TYPE_NODE && motion_control_calls == 0);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_UNAVAILABLE);
    assert(motion_control_calls == 0);
    struct priv p = {
        .gpu = (void *)1, .motion = (void *)2, .motion_fps = 120,
        .motion_state = MOTION_ACTIVE, .motion_captured = UINT64_C(1) << 40,
        .motion_presented = (UINT64_C(1) << 40) + 99,
        .motion_epoch = 1000,
    };
    struct vo vo = {.priv = &p};
    context.video_out = &vo;
    motion_control_available = false;
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_UNAVAILABLE);
    motion_control_available = true;
    motion_stats = (struct mp_motion_stats) {
        .analyzed_pairs = 17, .generated_frames = 31, .repeated_frames = 7,
        .capture_ns = 100, .coarse_ns = 200, .refinement_ns = 300,
        .warp_ns = 400, .synthesis_ns = 500,
        .analysis_width = 480, .analysis_height = 270,
        .synthesis_width = 1920, .synthesis_height = 1080,
    };
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(motion_property("supported")->type.type == CONF_TYPE_BOOL);
    assert(motion_property("supported")->value.bool_);
    assert(motion_property("target-fps")->value.int_ == 120);
    assert(strcmp(motion_property("state")->value.string, "active") == 0);
    assert(motion_property("timestamp-ns")->value.int64 == 123456789);
    assert(motion_property("epoch-ns")->value.int64 == 1000);
    assert(motion_property("source-captures")->type.type == CONF_TYPE_INT64);
    assert(motion_property("source-captures")->value.int64 == (INT64_C(1) << 40));
    assert(motion_property("swapchain-submissions")->value.int64 == (INT64_C(1) << 40) + 99);
    assert(motion_property("analyzed-pairs")->value.int64 == 17);
    assert(motion_property("synthesis-submissions")->value.int64 == 31);
    assert(motion_property("repeated-outputs")->value.int64 == 7);
    assert(motion_property("gpu-capture-ns")->value.int64 == 100);
    assert(motion_property("gpu-coarse-pass-ns")->value.int64 == 200);
    assert(motion_property("gpu-refinement-pass-ns")->value.int64 == 300);
    assert(motion_property("gpu-warp-pass-ns")->value.int64 == 400);
    assert(motion_property("gpu-synthesis-ns")->value.int64 == 500);
    assert(motion_property("last-analysis-width")->value.int_ == 480);
    assert(motion_property("last-analysis-height")->value.int_ == 270);
    assert(motion_property("last-synthesis-width")->value.int_ == 1920);
    assert(motion_property("last-synthesis-height")->value.int_ == 1080);
    p.paused = true;
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(strcmp(motion_property("state")->value.string, "paused") == 0);
    assert(motion_property("epoch-ns")->value.int64 == 1000);
    p = (struct priv){.gpu = (void *)1};
    gpu_supported = false;
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(!motion_property("supported")->value.bool_);
    assert(strcmp(motion_property("state")->value.string, "disabled") == 0);
    assert(motion_property("synthesis-submissions")->value.int64 == 0);
    assert(motion_property("epoch-ns")->value.int64 == 0);
    assert(motion_property("gpu-synthesis-ns")->unavailable);
    assert(motion_property("last-synthesis-width")->unavailable);
    p.motion_fps = 60;
    p.motion_state = MOTION_UNSUPPORTED;
    assert(!motion_backend_failed(&p));
    report_motion_stats(&vo, true);
    assert(p.motion_presented == 1);
    report_motion_stats(&vo, false);
    assert(p.motion_presented == 1);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(strcmp(motion_property("state")->value.string, "unsupported") == 0);
    p.motion_state = MOTION_ERROR;
    assert(motion_backend_failed(&p));
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(strcmp(motion_property("state")->value.string, "error") == 0);
    gpu_supported = true;
    puts("motion info: backend capability, state, 64-bit counters, timing and unavailable data passed");
}

static void test_motion_capability_transition(void)
{
    struct gl_next_opts options = {.motion_fps = 60};
    struct priv p = {
        .gpu = (void *)1, .next_opts = &options,
        .motion_captured = 20, .motion_presented = 30,
    };
    struct vo vo = {.priv = &p};
    gpu_supported = false;
    update_motion_options(&vo);
    assert(p.motion_state == MOTION_UNSUPPORTED && !p.motion);
    assert(p.motion_fps == 60 && !motion_backend_failed(&p));
    assert(p.motion_captured == 0 && p.motion_presented == 0);
    assert(p.motion_epoch == 123456789);
    assert(motion_create_calls == 0);
    assert(motion_destroy_calls == 1 && motion_queue_resets == 1);
    update_motion_options(&vo);
    assert(motion_destroy_calls == 1 && motion_queue_resets == 1);
    gpu_supported = true;
    options.motion_fps = 120;
    update_motion_options(&vo);
    assert(p.motion_state == MOTION_READY && p.motion);
    assert(p.motion_fps == 120 && !motion_backend_failed(&p));
    assert(motion_create_calls == 1);
    motion_create_fail = true;
    options.motion_fps = 60;
    update_motion_options(&vo);
    assert(p.motion_state == MOTION_ERROR && !p.motion);
    assert(motion_backend_failed(&p));
    assert(motion_create_calls == 2);
    options.motion_fps = 0;
    update_motion_options(&vo);
    assert(p.motion_state == MOTION_DISABLED && !p.motion);
    assert(!motion_backend_failed(&p));
    assert(motion_create_calls == 2);
    assert(motion_destroy_calls == 4 && motion_queue_resets == 4);
    motion_create_fail = false;
    puts("motion capability: unsupported GPU passes through; initialization failures remain fatal");
}

static void test_motion_cadence(double display_fps, double source_fps,
                               int target_fps, double speed,
                               bool interpolate, int expected_changes)
{
    struct priv p = {.motion = (void *)1, .motion_fps = target_fps};
    struct vo_frame frame = {
        .ideal_frame_duration = 1.0 / (source_fps * speed),
        .approx_duration = 1.0 / source_fps,
        .vsync_interval = 1.0 / display_fps,
    };
    bool needed = motion_interpolation_needed(&p, &frame);
    assert(needed == interpolate);
    double previous = -1;
    int changes = 0;
    for (int i = 0; i < (int) display_fps * 3; i++) {
        double pts = i * speed / display_fps;
        if (!needed)
            pts = floor(pts * source_fps + 1e-7) / source_fps;
        double output = needed ? motion_output_pts(&p, &frame, pts) : pts;
        if (output > previous)
            changes++;
        assert(output >= previous && output <= pts + 1e-8);
        previous = output;
    }
    printf("motion cadence: source=%.0f speed=%.1f target=%d display=%.0f "
           "timestamps=%d in 3 seconds\n",
           source_fps, speed, target_fps, display_fps, changes);
    assert(changes == expected_changes);
}

int main(void)
{
    struct ra ra = {.uses_pl = true};
    struct ra_ctx ctx = {.ra = &ra};
    struct ra_hwdec hwdec = {.ra = &ra};
    struct priv p = {.ra_ctx = &ctx};
    test_aspect(&p, &hwdec);
    test_format(&p, &hwdec);
    test_hwdec_change(&p, &hwdec);
    test_metadata_and_failure(&p, &hwdec);
    test_motion_info();
    test_motion_capability_transition();
    test_motion_cadence(120, 120, 60, 1, false, 360);
    test_motion_cadence(120, 60, 60, 1, false, 180);
    test_motion_cadence(120, 24, 60, 1, true, 180);
    test_motion_cadence(144, 24, 60, 1, true, 180);
    test_motion_cadence(60, 24, 120, 1, true, 180);
    test_motion_cadence(60, 60, 120, 1, false, 180);
    test_motion_cadence(120, 60, 120, 1, true, 360);
    test_motion_cadence(120, 24, 60, 3, false, 216);
    test_motion_cadence(60, 24, 120, 3, false, 180);
    test_motion_cadence(120, 120, 60, 0.5, false, 180);
    puts("gpu-next motion target preserves source cadence and playback speed");
    puts("gpu-next mapper aspect, format, metadata and failure tests passed");
    return 0;
}
