// GPU/codec stubs for the extracted gpu-next slot lifecycle. The parameter
// structures and comparison functions are extracted from mpv as well.
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define mp_assert assert
#define MP_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MP_ERR(p, ...) ((void)(p))

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
struct priv { void *gpu, *stats; struct ra_ctx *ra_ctx; };

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
    puts("gpu-next mapper aspect, format, metadata and failure tests passed");
    return 0;
}
