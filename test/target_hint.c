/* The libplacebo defaults are stubbed; hint policy and reference-white ordering
 * are extracted from the production renderer. No GPU is required.
 */
#include <stdbool.h>
#include <stdio.h>
#include <math.h>

#define PL_API_VER 376
#define PL_COLOR_SDR_WHITE 203.0f
#define PL_COLOR_SDR_CONTRAST 1000.0f
#define PL_HDR_METADATA_ANY 0
#define PL_HDR_NITS 0
enum { PL_COLOR_PRIM_UNKNOWN, PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_BT_2020 };
enum { PL_COLOR_TRC_UNKNOWN, PL_COLOR_TRC_SRGB, PL_COLOR_TRC_PQ, PL_COLOR_TRC_SCRGB };
enum gl_target_hint_setting { GL_TARGET_HINT_AUTO = -1, GL_TARGET_HINT_OFF, GL_TARGET_HINT_ON };
enum gl_target_hint_mode { GL_TARGET_HINT_TARGET, GL_TARGET_HINT_SOURCE, GL_TARGET_HINT_SOURCE_DYNAMIC };
enum gl_target_hint_status { GL_TARGET_HINT_DISABLED, GL_TARGET_HINT_NO_SOURCE, GL_TARGET_HINT_VALID };
struct pl_raw_primaries { bool valid; };
struct pl_color_space {
    int primaries, transfer;
    struct { float min_luma, max_luma, max_cll, max_fall; struct pl_raw_primaries prim; } hdr;
};
struct gl_video_opts {
    int target_hint, target_hint_mode, target_trc, target_prim;
    float target_peak;
    struct { bool inverse; } tone_map;
};
struct pl_nominal_luma_params {
    const struct pl_color_space *color;
    int metadata, scaling;
    float *out_min, *out_max;
};
#define pl_nominal_luma_params(...) (&(struct pl_nominal_luma_params){__VA_ARGS__})
static const struct pl_color_space pl_color_space_hdr10 = {.transfer = PL_COLOR_TRC_PQ};
static bool pl_color_transfer_is_hdr(int trc) { return trc == PL_COLOR_TRC_PQ || trc == PL_COLOR_TRC_SCRGB; }
static int mp_get_best_prim_container(const struct pl_raw_primaries *p) { (void)p; return PL_COLOR_PRIM_BT_709; }
static bool pl_primaries_valid(const struct pl_raw_primaries *p) { return p->valid; }
static void pl_color_space_merge(struct pl_color_space *dst, const struct pl_color_space *src) { dst->hdr.prim = src->hdr.prim; }
static void pl_color_space_nominal_luma_ex(const struct pl_nominal_luma_params *p)
{
    if (p->out_min) *p->out_min = 0.08f;
    if (p->out_max) *p->out_max = p->color->hdr.max_luma;
}
static void pl_color_space_infer_map(struct pl_color_space *src, struct pl_color_space *dst)
{
    (void)src;
    if (!dst->hdr.max_luma) dst->hdr.max_luma = PL_COLOR_SDR_WHITE;
    if (!dst->hdr.min_luma)
        dst->hdr.min_luma = pl_color_transfer_is_hdr(dst->transfer) ? 0.005f : dst->hdr.max_luma / PL_COLOR_SDR_CONTRAST;
}

#include "target_hint_functions.h"

static int checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "target hint failed at %d: %s\n", __LINE__, #x); return 1; } checks++; } while (0)
#define EQ(a, b) (fabsf((a) - (b)) < 1e-6f)
int main(void)
{
    struct gl_video_opts opts = {.target_hint = GL_TARGET_HINT_ON, .target_hint_mode = GL_TARGET_HINT_SOURCE};
    struct pl_color_space source = {.transfer = PL_COLOR_TRC_SRGB, .hdr.max_luma = 100.0f};
    struct pl_color_space target = {.transfer = PL_COLOR_TRC_SRGB, .hdr.max_luma = 200.0f};
    struct pl_color_space hint = {0};
    for (int known = 0; known < 2; known++) {
        source.hdr.min_luma = known ? 0.05f : 0;
        for (int peak = 0; peak < 2; peak++) {
            opts.target_peak = peak ? 400.0f : 0;
            for (int ref = 0; ref < 2; ref++) {
                bool min_luma_known = false;
                CHECK(gl_video_prepare_target_hint(&opts, &source, &target, true, &hint, &min_luma_known) == GL_TARGET_HINT_VALID);
                CHECK(min_luma_known == !!known);
                apply_reference_hint(&hint, &target, ref ? 203.0f : 0, min_luma_known);
                float expected_peak = ref ? 203.0f : peak ? 400.0f : 100.0f;
                CHECK(EQ(hint.hdr.max_luma, expected_peak));
                CHECK(EQ(hint.hdr.min_luma, known ? 0.05f : expected_peak / PL_COLOR_SDR_CONTRAST));
            }
        }
    }
    bool known = false;
    opts.target_hint_mode = GL_TARGET_HINT_TARGET;
    target.hdr.min_luma = 0.02f;
    CHECK(gl_video_prepare_target_hint(&opts, &source, &target, true, &hint, &known) == GL_TARGET_HINT_VALID);
    CHECK(known);
    apply_reference_hint(&hint, &target, 203.0f, known);
    CHECK(EQ(hint.hdr.min_luma, 0.02f));

    opts.target_hint_mode = GL_TARGET_HINT_SOURCE_DYNAMIC;
    source.hdr.min_luma = 0;
    target.hdr.min_luma = 0;
    CHECK(gl_video_prepare_target_hint(&opts, &source, &target, true, &hint, &known) == GL_TARGET_HINT_VALID);
    CHECK(known);
    apply_reference_hint(&hint, &target, 203.0f, known);
    CHECK(EQ(hint.hdr.min_luma, 0.08f));

    opts.target_hint_mode = GL_TARGET_HINT_SOURCE;
    source.transfer = target.transfer = PL_COLOR_TRC_PQ;
    target.hdr.min_luma = 0;
    CHECK(gl_video_prepare_target_hint(&opts, &source, &target, true, &hint, &known) == GL_TARGET_HINT_VALID);
    CHECK(!known);
    apply_reference_hint(&hint, &target, 203.0f, known);
    CHECK(EQ(hint.hdr.min_luma, 0.005f));
    CHECK(EQ(hint.hdr.max_luma, 400.0f));

    CHECK(gl_video_prepare_target_hint(&opts, &source, &target, true, &hint, NULL) == GL_TARGET_HINT_VALID);
    CHECK(gl_video_prepare_target_hint(&opts, NULL, &target, true, &hint, &known) == GL_TARGET_HINT_NO_SOURCE);
    opts.target_hint = GL_TARGET_HINT_OFF;
    CHECK(gl_video_prepare_target_hint(&opts, &source, &target, true, &hint, &known) == GL_TARGET_HINT_DISABLED);
    opts.target_hint = GL_TARGET_HINT_AUTO;
    target.transfer = PL_COLOR_TRC_UNKNOWN;
    CHECK(gl_video_prepare_target_hint(&opts, &source, &target, false, &hint, &known) == GL_TARGET_HINT_DISABLED);
    target.transfer = PL_COLOR_TRC_UNKNOWN;
    CHECK(gl_video_prepare_target_hint(&opts, &source, &target, true, &hint, &known) == GL_TARGET_HINT_VALID);
    printf("target hint: %d checks passed\n", checks);
    return 0;
}
