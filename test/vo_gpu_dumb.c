/* Minimal GPU sampling contracts for buffers larger than their visible image. */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define mp_assert assert
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MPSWAP(type, a, b) do { type tmp = (a); (a) = (b); (b) = tmp; } while (0)

enum {
    PL_COLOR_SYSTEM_RGB,
    PL_COLOR_SYSTEM_XYZ,
    PL_COLOR_SYSTEM_YUV,
    PL_CHROMA_CENTER,
    RA_CTYPE_FLOAT,
};

struct ra_tex {
    struct { int w, h; } params;
};
struct mp_image { int unused; };
struct mp_rect { int x0, y0, x1, y1; };

#include "vo_gpu_dumb_types.h"

struct gl_video {
    struct {
        int w, h, rotate, chroma_location;
        struct { int sys; } repr;
    } image_params;
    struct {
        int chroma_w, chroma_h;
        int components[4][4];
        int component_bits, component_pad, component_type;
    } ra_format;
    struct video_image image;
    int plane_count;
    int texture_w, texture_h;
    struct gl_transform texture_offset;
    struct mp_rect src_rect;
    bool dumb_mode;
    struct image sampled[4];
    int conversions;
};

static float mp_get_csp_mul(int csp, int valid_bits, int component_bits)
{
    (void)csp;
    (void)valid_bits;
    (void)component_bits;
    return 1.0f;
}

static void pl_chroma_location_offset(int location, float *x, float *y)
{
    assert(location == PL_CHROMA_CENTER);
    *x = *y = 0;
}

static void copy_image(struct gl_video *p, int *index, struct image img)
{
    assert(*index < p->plane_count);
    p->sampled[(*index)++] = img;
}

static void pass_convert_yuv(struct gl_video *p)
{
    p->conversions++;
}

#include "vo_gpu_dumb_functions.h"

static void assert_close(float actual, float expected)
{
    if (!(fabsf(actual - expected) <= 0.0001f)) {
        fprintf(stderr, "coordinate %.9g != %.9g\n", actual, expected);
        assert(false);
    }
}

static void setup(struct gl_video *p, struct ra_tex tex[4], int w, int h,
                  int buffer_w, int buffer_h, int rotate, bool flip, int planes)
{
    memset(p, 0, sizeof(*p));
    p->image_params.w = w;
    p->image_params.h = h;
    p->image_params.rotate = rotate;
    p->image_params.chroma_location = PL_CHROMA_CENTER;
    p->image_params.repr.sys = planes == 1 ? PL_COLOR_SYSTEM_RGB
                                         : PL_COLOR_SYSTEM_YUV;
    p->ra_format.chroma_w = planes == 1 ? 1 : 2;
    p->ra_format.chroma_h = planes == 1 ? 1 : 2;
    p->ra_format.component_bits = 8;
    p->ra_format.component_type = RA_CTYPE_FLOAT;
    p->plane_count = planes;
    p->texture_w = rotate % 180 ? h : w;
    p->texture_h = rotate % 180 ? w : h;
    p->texture_offset = identity_trans;
    p->src_rect = (struct mp_rect){0, 0, p->texture_w, p->texture_h};
    p->dumb_mode = true;
    static struct mp_image mpi;
    p->image.mpi = &mpi;
    if (planes == 1) {
        p->ra_format.components[0][0] = 1;
        p->ra_format.components[0][1] = 2;
        p->ra_format.components[0][2] = 3;
    } else {
        p->ra_format.components[0][0] = 1;
        p->ra_format.components[1][0] = 2;
        p->ra_format.components[planes == 2 ? 1 : 2][planes == 2 ? 1 : 0] = 3;
        if (planes == 4)
            p->ra_format.components[3][0] = 4;
    }
    for (int i = 0; i < planes; i++) {
        bool chroma = i == 1 || (i == 2 && planes != 2);
        int cw = chroma ? p->ra_format.chroma_w : 1;
        int ch = chroma ? p->ra_format.chroma_h : 1;
        tex[i].params.w = (buffer_w + cw - 1) / cw;
        tex[i].params.h = (buffer_h + ch - 1) / ch;
        p->image.planes[i] = (struct texplane){
            .w = tex[i].params.w,
            .h = tex[i].params.h,
            .tex = &tex[i],
            .flipped = flip,
        };
    }
}

static void assert_corners(struct gl_video *p, int plane, float x0, float y0,
                           float x1, float y1, int rotate, bool flip)
{
    struct image *s = &p->sampled[plane];
    for (int n = 0; n < 4; n++) {
        float x = (n / 2) * s->w;
        float y = (n % 2) * s->h;
        gl_transform_vec(s->transform, &x, &y);

        int u = n / 2, v = n % 2;
        switch (rotate) {
        case 90:  u = n % 2;       v = 1 - n / 2; break;
        case 180: u = 1 - n / 2;   v = 1 - n % 2; break;
        case 270: u = 1 - n % 2;   v = n / 2;     break;
        }
        if (flip)
            v = 1 - v;
        float expected_x = u ? x1 : x0;
        float expected_y = v ? y1 : y0;
        // Match the final normalized texture coordinates emitted by the quad.
        assert_close(x / s->tex->params.w, expected_x / s->tex->params.w);
        assert_close(y / s->tex->params.h, expected_y / s->tex->params.h);
    }
}

static void test_reported_buffer(void)
{
    struct gl_video p;
    struct ra_tex tex[4];
    setup(&p, tex, 1280, 536, 1280, 544, 0, false, 1);
    pass_render_frame_dumb(&p);
    assert_corners(&p, 0, 0, 0, 1280, 536, 0, false);
}

static void test_padded_rotations(int planes)
{
    for (int rotate = 0; rotate < 360; rotate += 90) {
        for (int flip = 0; flip < 2; flip++) {
            struct gl_video p;
            struct ra_tex tex[4];
            setup(&p, tex, 1280, 536, 1344, 544, rotate, flip, planes);
            pass_render_frame_dumb(&p);
            assert(p.conversions == 1);
            for (int i = 0; i < planes; i++) {
                bool chroma = p.sampled[i].type == PLANE_CHROMA;
                assert_corners(&p, i, 0, 0, chroma ? 640 : 1280,
                               chroma ? 268 : 536, rotate, flip);
                // Image views may crop; the underlying allocation stays intact.
                assert(p.image.planes[i].w == tex[i].params.w);
                assert(p.image.planes[i].h == tex[i].params.h);
            }
        }
    }
}

static void test_crop_window(void)
{
    struct gl_video p;
    struct ra_tex tex[4];
    setup(&p, tex, 1280, 536, 1344, 544, 0, false, 1);
    p.src_rect = (struct mp_rect){64, 10, 1216, 526};
    pass_render_frame_dumb(&p);
    assert_corners(&p, 0, 64, 10, 1216, 526, 0, false);
}

static void test_odd_chroma(bool padded)
{
    for (int rotate = 0; rotate < 360; rotate += 90) {
        for (int flip = 0; flip < 2; flip++) {
            struct gl_video p;
            struct ra_tex tex[4];
            setup(&p, tex, 641, 359, padded ? 672 : 641, padded ? 368 : 359,
                  rotate, flip, 3);
            pass_render_frame_dumb(&p);
            assert_corners(&p, 0, 0, 0, 641, 359, rotate, flip);
            // Quarter turns keep the fractional chroma edge on the other side.
            float offset = rotate % 180 ? 0.5f : 0;
            for (int i = 1; i < 3; i++) {
                assert_corners(&p, i, offset, offset, 320.5f + offset,
                               179.5f + offset, rotate, flip);
            }
        }
    }
}

static void test_full_renderer_keeps_physical_views(void)
{
    struct gl_video p;
    struct ra_tex tex[4];
    setup(&p, tex, 1280, 536, 1344, 544, 90, true, 2);
    p.dumb_mode = false;
    struct image img[4];
    struct gl_transform off[4];
    pass_get_images(&p, &p.image, img, off);
    assert(img[0].w == 544 && img[0].h == 1344);
    assert(img[1].w == 272 && img[1].h == 672);
    assert(img[0].tex == &tex[0] && img[1].tex == &tex[1]);
}

int main(void)
{
    test_reported_buffer();
    test_padded_rotations(1); // hardware RGB plane
    test_padded_rotations(2); // hardware NV12 planes
    test_padded_rotations(4); // planar YUV with alpha
    test_crop_window();
    test_odd_chroma(false);
    test_odd_chroma(true);
    test_full_renderer_keeps_physical_views();
    puts("Minimal GPU crop sampling contracts passed");
    return 0;
}
