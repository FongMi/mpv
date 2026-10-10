/* Behavioral contracts for the production ASS color and atlas packing path. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void test_failure(const char *condition, int line)
{
    fprintf(stderr, "assertion failed at line %d: %s\n", line, condition);
    exit(1);
}

#define assert(condition) do { \
    if (!(condition)) test_failure(#condition, __LINE__); \
} while (0)

#define MP_ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define mp_assert assert
#define MP_ASSERT_UNREACHABLE() abort()
#define MP_MSG(sd, level, ...) ((void)(sd), (void)(level))
#define MSGL_V 1
#define MSGL_WARN 2
#define MP_SUB_BB_LIST_MAX 15
#define MP_ASS_RGBA(r, g, b, a) \
    (((unsigned)(r) << 24) | ((g) << 16) | ((b) << 8) | (0xff - (a)))
#define MP_TARRAY_GROW(p, array, n) do { \
    (void)(p); \
    (array) = realloc((array), ((n) + 1) * sizeof(*(array))); \
    assert(array); \
} while (0)

enum pl_color_system {
    PL_COLOR_SYSTEM_UNKNOWN, PL_COLOR_SYSTEM_BT_601, PL_COLOR_SYSTEM_BT_709,
    PL_COLOR_SYSTEM_SMPTE_240M, PL_COLOR_SYSTEM_BT_2020_NC,
    PL_COLOR_SYSTEM_BT_2020_C, PL_COLOR_SYSTEM_BT_2100_PQ,
    PL_COLOR_SYSTEM_BT_2100_HLG, PL_COLOR_SYSTEM_DOLBYVISION,
    PL_COLOR_SYSTEM_RGB, PL_COLOR_SYSTEM_XYZ, PL_COLOR_SYSTEM_YCGCO,
    PL_COLOR_SYSTEM_COUNT,
};
enum pl_color_levels {
    PL_COLOR_LEVELS_RAW = -1,
    PL_COLOR_LEVELS_UNKNOWN, PL_COLOR_LEVELS_LIMITED, PL_COLOR_LEVELS_FULL,
    PL_COLOR_LEVELS_COUNT,
};
enum pl_color_primaries { PL_COLOR_PRIM_UNKNOWN, PL_COLOR_PRIM_DCI_P3 };
enum pl_color_transfer { PL_COLOR_TRC_UNKNOWN, PL_COLOR_TRC_PQ };
enum sub_bitmap_format {
    SUBBITMAP_INVALID = -1, SUBBITMAP_EMPTY, SUBBITMAP_LIBASS, SUBBITMAP_BGRA,
};
enum {
    YCBCR_DEFAULT, YCBCR_UNKNOWN, YCBCR_NONE, YCBCR_BT601_TV, YCBCR_BT601_PC,
    YCBCR_BT709_TV, YCBCR_BT709_PC, YCBCR_SMPTE240M_TV, YCBCR_SMPTE240M_PC,
};
enum { IMGFMT_Y8, IMGFMT_BGRA };
enum sd_ctrl {
    SD_CTRL_SUB_STEP, SD_CTRL_SET_ANIMATED_CHECK, SD_CTRL_RESET_SOFT,
    SD_CTRL_SET_VIDEO_PARAMS, SD_CTRL_UPDATE_OPTS,
};
#define CONTROL_OK 1
#define CONTROL_UNKNOWN 0
#define UPDATE_SUB_FILT 1
#define UPDATE_SUB_HARD 2
#define SUB_SEEK_OFFSET 0.01

struct pl_color_repr { enum pl_color_system sys; enum pl_color_levels levels; };
struct pl_color_space {
    enum pl_color_primaries primaries;
    enum pl_color_transfer transfer;
    float max_luma;
};
typedef struct { float m[3][3]; } pl_matrix3x3;
struct pl_transform3x3 { pl_matrix3x3 mat; float c[3]; };
struct pl_cie_xy { float x, y; };
struct pl_raw_primaries { struct pl_cie_xy white; };
struct mp_csp_params {
    struct pl_color_repr repr;
    struct pl_color_space color;
    enum pl_color_levels levels_out;
    float brightness, contrast, hue, saturation;
    bool gray, is_float;
    int texture_bits, input_bits;
};
#define MP_CSP_PARAMS_DEFAULTS { \
    .repr = { PL_COLOR_SYSTEM_BT_601, PL_COLOR_LEVELS_LIMITED }, \
    .levels_out = PL_COLOR_LEVELS_FULL, .contrast = 1, .saturation = 1, \
    .texture_bits = 8, .input_bits = 8 }

struct mp_image {
    uint8_t *planes[1];
    int stride[1];
};
struct bitmap_packer { int padding; };
struct mp_rect { int x0, y0, x1, y1; };
struct mp_osd_res { int h; };
struct mp_image_params {
    struct pl_color_repr repr;
    struct pl_color_space color;
};
typedef struct ass_image {
    int w, h, stride, dst_x, dst_y;
    uint32_t color;
    uint8_t *bitmap;
    struct ass_image *next;
} ASS_Image;
typedef struct { int YCbCrMatrix; } ASS_Track;
typedef struct { ASS_Image *images; int changed; } ASS_Renderer;

#include "ass_color_types.h"

struct sub_bitmap_copy_cache {
    struct sub_bitmaps result;
    struct sub_bitmap parts[8];
};
struct mp_subtitle_opts {
    int ass_vsfilter_color_compat;
    bool sub_scale_signs;
};
struct mp_subtitle_shared_opts { float sub_scale[2], sub_pos[2]; };
struct sd_ass_priv {
    struct mp_sub_packer *packer;
    ASS_Track *ass_track;
    ASS_Renderer *ass_renderer;
    struct mp_image_params video_params, last_params;
    struct sub_bitmap_copy_cache *copy_cache;
    bool ass_configured, layout_change_pending, color_mangle_changed;
    bool check_animated, clear_once;
};
struct sd {
    struct sd_ass_priv *priv;
    struct mp_subtitle_opts *opts;
    struct mp_subtitle_shared_opts *shared_opts;
    int order;
};

static int av_clip(int value, int low, int high)
{
    return MPMIN(MPMAX(value, low), high);
}

/* The untested XYZ branch needs libplacebo; reject entering that boundary. */
static const struct pl_raw_primaries *pl_raw_primaries_get(int prim)
{
    (void)prim;
    abort();
}

static pl_matrix3x3 pl_get_xyz2rgb_matrix(const struct pl_raw_primaries *prim)
{
    (void)prim;
    abort();
}

static void apply_chromatic_adaptation(struct pl_cie_xy src,
                                      struct pl_cie_xy dst, pl_matrix3x3 *mat)
{
    (void)src; (void)dst; (void)mat;
    abort();
}

/* libplacebo's affine inversion boundary, independent of ASS packing. */
static void pl_transform3x3_invert(struct pl_transform3x3 *mat)
{
    double aug[3][6] = {0};
    float offset[3];
    memcpy(offset, mat->c, sizeof(offset));
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++)
            aug[r][c] = mat->mat.m[r][c];
        aug[r][r + 3] = 1;
    }
    for (int c = 0; c < 3; c++) {
        double scale = aug[c][c];
        assert(fabs(scale) > 1e-6);
        for (int k = 0; k < 6; k++)
            aug[c][k] /= scale;
        for (int r = 0; r < 3; r++) {
            if (r == c)
                continue;
            scale = aug[r][c];
            for (int k = 0; k < 6; k++)
                aug[r][k] -= scale * aug[c][k];
        }
    }
    for (int r = 0; r < 3; r++) {
        mat->c[r] = 0;
        for (int c = 0; c < 3; c++) {
            mat->mat.m[r][c] = aug[r][c + 3];
            mat->c[r] -= mat->mat.m[r][c] * offset[c];
        }
    }
}

static int mp_rect_w(struct mp_rect rect) { return rect.x1 - rect.x0; }
static int mp_rect_h(struct mp_rect rect) { return rect.y1 - rect.y0; }

static int mp_get_sub_bb_list(struct sub_bitmaps *parts, struct mp_rect *bb,
                             int count)
{
    assert(count > 0);
    if (!parts->num_parts)
        return 0;
    *bb = (struct mp_rect){0, 0, 0, 0};
    for (int n = 0; n < parts->num_parts; n++) {
        struct sub_bitmap *part = &parts->parts[n];
        bb->x1 = MPMAX(bb->x1, part->x + part->w);
        bb->y1 = MPMAX(bb->y1, part->y + part->h);
    }
    return 1;
}

static void memcpy_pic(void *dst, const void *src, int w, int h,
                       int dst_stride, int src_stride)
{
    for (int y = 0; y < h; y++)
        memcpy((uint8_t *)dst + y * dst_stride,
               (const uint8_t *)src + y * src_stride, w);
}

static void memset_pic(void *dst, int value, int w, int h, int stride)
{
    for (int y = 0; y < h; y++)
        memset((uint8_t *)dst + y * stride, value, w);
}

/* Atlas placement/allocation is not part of the color conversion contract. */
static int pack_calls;
static bool pack(struct mp_sub_packer *packer, struct sub_bitmaps *res, int format)
{
    pack_calls++;
    if (!res->num_parts)
        return false;
    int bytes = format == IMGFMT_BGRA ? 4 : 1;
    int w = 0, h = 0;
    for (int n = 0; n < res->num_parts; n++) {
        res->parts[n].src_x = w + 1;
        res->parts[n].src_y = 1;
        w += res->parts[n].w + 2;
        h = MPMAX(h, res->parts[n].h + 2);
    }
    if (!packer->cached_img)
        packer->cached_img = calloc(1, sizeof(*packer->cached_img));
    assert(packer->cached_img);
    struct mp_image *image = packer->cached_img;
    image->stride[0] = w * bytes;
    image->planes[0] = realloc(image->planes[0], h * image->stride[0]);
    assert(image->planes[0]);
    memset(image->planes[0], 0, h * image->stride[0]);
    res->packed = image;
    res->packed_w = w;
    res->packed_h = h;
    return true;
}

static ASS_Image *ass_render_frame(ASS_Renderer *renderer, ASS_Track *track,
                                   long long ts, int *changed)
{
    (void)track; (void)ts;
    *changed = renderer->changed;
    renderer->changed = 0;
    return renderer->images;
}

static struct sub_bitmaps *sub_bitmaps_copy(struct sub_bitmap_copy_cache **cache,
                                           struct sub_bitmaps *source)
{
    if (!source || !source->num_parts)
        return NULL;
    assert(source->num_parts <= 8);
    if (!*cache)
        *cache = calloc(1, sizeof(**cache));
    assert(*cache);
    (*cache)->result = *source;
    memcpy((*cache)->parts, source->parts,
           source->num_parts * sizeof(*source->parts));
    (*cache)->result.parts = (*cache)->parts;
    return &(*cache)->result;
}

static void transform_subtitle_layout(struct sub_bitmaps *res,
                                      struct mp_osd_res dim, float pos, float scale)
{
    (void)res; (void)dim; (void)pos; (void)scale;
    abort();
}

static long long ass_step_sub(ASS_Track *track, long long ts, double step)
{
    (void)track; (void)ts; (void)step;
    abort();
}
static void reset(struct sd *sd) { (void)sd; abort(); }
static void filters_destroy(struct sd *sd) { (void)sd; abort(); }
static void filters_init(struct sd *sd) { (void)sd; abort(); }
static void assobjects_destroy(struct sd *sd) { (void)sd; abort(); }
static void assobjects_init(struct sd *sd) { (void)sd; abort(); }

#include "ass_color_functions.h"

struct fixture {
    uint8_t masks[2][3];
    ASS_Image images[2];
    ASS_Track track;
    ASS_Renderer renderer;
    struct bitmap_packer atlas;
    struct mp_sub_packer packer;
    struct mp_subtitle_opts opts;
    struct mp_subtitle_shared_opts shared_opts;
    struct sd_ass_priv ctx;
    struct sd sd;
};

static void fixture_init(struct fixture *f)
{
    *f = (struct fixture){0};
    memcpy(f->masks, (uint8_t[][3]){{255, 128, 64}, {255, 192, 0}},
           sizeof(f->masks));
    f->images[0] = (ASS_Image){
        .w = 3, .h = 1, .stride = 3, .bitmap = f->masks[0],
        .color = MP_ASS_RGBA(255, 255, 0, 255), .next = &f->images[1],
    };
    f->images[1] = (ASS_Image){
        .w = 3, .h = 1, .stride = 3, .dst_x = 3, .bitmap = f->masks[1],
        .color = MP_ASS_RGBA(255, 128, 0, 127),
    };
    f->track.YCbCrMatrix = YCBCR_BT601_TV;
    f->renderer = (ASS_Renderer){ .images = f->images, .changed = 1 };
    f->atlas.padding = 1;
    f->packer.packer = &f->atlas;
    f->opts.ass_vsfilter_color_compat = 1;
    f->ctx = (struct sd_ass_priv){
        .packer = &f->packer, .ass_track = &f->track,
        .ass_renderer = &f->renderer,
        .video_params.repr = {PL_COLOR_SYSTEM_BT_709, PL_COLOR_LEVELS_LIMITED},
    };
    f->sd = (struct sd){&f->ctx, &f->opts, &f->shared_opts, 0};
}

static void fixture_destroy(struct fixture *f)
{
    free(f->packer.cached_parts);
    if (f->packer.cached_img) {
        free(f->packer.cached_img->planes[0]);
        free(f->packer.cached_img);
    }
    free(f->ctx.copy_cache);
}

static void bitmap_pixels(struct sub_bitmaps *res, uint32_t output[6])
{
    memset(output, 0, 6 * sizeof(*output));
    for (int n = 0; n < res->num_parts; n++) {
        struct sub_bitmap *part = &res->parts[n];
        if (res->format == SUBBITMAP_LIBASS) {
            draw_ass_rgba(part->bitmap, part->w, part->h, part->stride,
                          (uint8_t *)output, 6 * 4, part->x, part->y,
                          part->libass.color);
        } else {
            assert(res->format == SUBBITMAP_BGRA);
            memcpy(&output[part->x], part->bitmap, part->w * 4);
        }
    }
}

static void assert_pixels(uint32_t actual[6], uint32_t expected[6])
{
    for (int n = 0; n < 6; n++) {
        if (actual[n] != expected[n]) {
            fprintf(stderr, "pixel %d: actual %08x expected %08x\n",
                    n, actual[n], expected[n]);
            exit(1);
        }
    }
}

static void test_format_parity(void)
{
    struct fixture f;
    fixture_init(&f);
    uint32_t expected[6], actual[6];
    struct sub_bitmaps *res = render_subtitles(&f.sd, SUBBITMAP_LIBASS, false);
    assert(res->parts[0].libass.color == MP_ASS_RGBA(255, 240, 0, 255));
    assert(res->parts[1].libass.color == MP_ASS_RGBA(255, 133, 0, 127));
    bitmap_pixels(res, expected);
    int packed = pack_calls;
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(pack_calls == packed + 1);
    bitmap_pixels(res, actual);
    assert_pixels(actual, expected);
    assert(f.images[0].color == MP_ASS_RGBA(255, 255, 0, 255));
    assert(f.images[1].color == MP_ASS_RGBA(255, 128, 0, 127));
    assert(res->video_color_space);
    packed = pack_calls;
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id == 0);
    assert(pack_calls == packed);
    bitmap_pixels(res, actual);
    assert_pixels(actual, expected);
    res = render_subtitles(&f.sd, SUBBITMAP_LIBASS, false);
    bitmap_pixels(res, actual);
    assert_pixels(actual, expected);
    fixture_destroy(&f);
}

static void test_conversion_bypass(void)
{
    for (int mode = 0; mode < 4; mode++) {
        struct fixture f;
        fixture_init(&f);
        bool converted = mode == 0;
        if (mode == 1)
            f.opts.ass_vsfilter_color_compat = 0;
        if (mode == 2)
            f.track.YCbCrMatrix = YCBCR_NONE;
        if (mode == 3)
            f.ctx.video_params.repr.sys = PL_COLOR_SYSTEM_BT_601;
        uint32_t expected[6], actual[6];
        struct sub_bitmaps *res = render_subtitles(&f.sd, SUBBITMAP_LIBASS,
                                                  converted);
        assert(res->parts[0].libass.color == f.images[0].color);
        assert(res->parts[1].libass.color == f.images[1].color);
        bitmap_pixels(res, expected);
        res = render_subtitles(&f.sd, SUBBITMAP_BGRA, converted);
        bitmap_pixels(res, actual);
        assert_pixels(actual, expected);
        assert(res->video_color_space == !converted);
        fixture_destroy(&f);
    }
}

static void test_static_color_updates(void)
{
    struct fixture f;
    fixture_init(&f);
    uint32_t converted[6], original[6], actual[6];
    struct sub_bitmaps *res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    bitmap_pixels(res, converted);
    int packed = pack_calls;

    /* HDR mastering updates must retain a static subtitle's cached pixels. */
    struct mp_image_params video = f.ctx.video_params;
    video.color.transfer = PL_COLOR_TRC_PQ;
    video.color.primaries = PL_COLOR_PRIM_DCI_P3;
    video.color.max_luma = 1000;
    assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id == 0);
    assert(pack_calls == packed);
    bitmap_pixels(res, actual);
    assert_pixels(actual, converted);

    /* A new video matrix must repack even when libass reports no change. */
    video.repr.sys = PL_COLOR_SYSTEM_BT_601;
    assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id != 0);
    assert(pack_calls == ++packed);
    bitmap_pixels(res, original);
    assert(original[0] == 0xffffff00);
    assert(memcmp(original, converted, sizeof(original)) != 0);

    video.repr.sys = PL_COLOR_SYSTEM_BT_709;
    assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id != 0);
    assert(pack_calls == ++packed);
    bitmap_pixels(res, actual);
    assert_pixels(actual, converted);

    /* Toggling the option must invalidate the same static ASS frame. */
    uint64_t flags = 0;
    f.opts.ass_vsfilter_color_compat = 0;
    assert(control(&f.sd, SD_CTRL_UPDATE_OPTS, &flags) == CONTROL_OK);
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id != 0);
    assert(pack_calls == ++packed);
    bitmap_pixels(res, actual);
    assert_pixels(actual, original);
    f.opts.ass_vsfilter_color_compat = 1;
    assert(control(&f.sd, SD_CTRL_UPDATE_OPTS, &flags) == CONTROL_OK);
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id != 0);
    assert(pack_calls == ++packed);
    bitmap_pixels(res, actual);
    assert_pixels(actual, converted);

    /* Levels participate in the matrix contract; unchanged values do not. */
    f.opts.ass_vsfilter_color_compat = 2;
    assert(control(&f.sd, SD_CTRL_UPDATE_OPTS, &flags) == CONTROL_OK);
    render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    packed = pack_calls;
    video.repr.levels = PL_COLOR_LEVELS_FULL;
    assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id != 0);
    assert(pack_calls == ++packed);
    bitmap_pixels(res, actual);
    assert(memcmp(actual, converted, sizeof(actual)) != 0);
    assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
    res = render_subtitles(&f.sd, SUBBITMAP_BGRA, false);
    assert(res->change_id == 0);
    assert(pack_calls == packed);
    fixture_destroy(&f);
}

static void test_color_updates_without_output(void)
{
    for (int format = SUBBITMAP_LIBASS; format <= SUBBITMAP_BGRA; format++) {
        struct fixture f;
        fixture_init(&f);
        uint32_t converted[6], actual[6];
        struct sub_bitmaps *res = render_subtitles(&f.sd, format, false);
        bitmap_pixels(res, converted);
        int packed = pack_calls;

        /* Repeated controls must retain a change until rendering resumes. */
        struct mp_image_params video = f.ctx.video_params;
        video.repr.sys = PL_COLOR_SYSTEM_BT_601;
        assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
        assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
        f.ctx.ass_renderer = NULL;
        assert(!render_subtitles(&f.sd, format, false));
        assert(pack_calls == packed);
        f.ctx.ass_renderer = &f.renderer;
        res = render_subtitles(&f.sd, format, false);
        assert(res->change_id != 0);
        assert(pack_calls == ++packed);
        bitmap_pixels(res, actual);
        assert(actual[0] == 0xffffff00);

        /* Empty frames invalidate the atlas before the subtitle reappears. */
        video.repr.sys = PL_COLOR_SYSTEM_BT_709;
        assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
        assert(control(&f.sd, SD_CTRL_SET_VIDEO_PARAMS, &video) == CONTROL_OK);
        f.renderer.images = NULL;
        f.renderer.changed = 1;
        assert(!render_subtitles(&f.sd, format, false));
        assert(pack_calls == ++packed);
        f.renderer.images = f.images;
        f.renderer.changed = 1;
        res = render_subtitles(&f.sd, format, false);
        assert(res->change_id != 0);
        assert(pack_calls == ++packed);
        bitmap_pixels(res, actual);
        assert_pixels(actual, converted);
        fixture_destroy(&f);
    }
}

int main(void)
{
    test_format_parity();
    test_conversion_bypass();
    test_static_color_updates();
    test_color_updates_without_output();
    puts("ASS color packing contracts passed");
    return 0;
}
