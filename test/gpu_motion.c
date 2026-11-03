/* SPDX-License-Identifier: LGPL-2.1-or-later */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/vulkan.h>

#if PL_API_VER >= 391
#include <libplacebo/shaders/motion.h>
#endif

#include "video/out/gpu_next/motion.h"
#include "gpu_motion_shaders.h"

#define WIDTH 128
#define HEIGHT 96

static int gpu_errors;
static const char *expected_gpu_error;
static int expected_gpu_errors;
static const char *expected_allocation;
static bool saw_expected_allocation;

static void log_message(void *opaque, enum pl_log_level level, const char *message)
{
    if (expected_allocation && strstr(message, expected_allocation))
        saw_expected_allocation = true;
    if (level <= PL_LOG_ERR) {
        if (expected_gpu_error && !strcmp(message, expected_gpu_error))
            expected_gpu_errors++;
        else
            gpu_errors++;
    }
    pl_log_simple(opaque, level, message);
}

static float noise(int x, int y, int channel)
{
    uint32_t value = x * 73856093u ^ y * 19349663u ^ channel * 83492791u;
    value ^= value >> 13;
    value *= 1274126177u;
    return (value & 255) / 255.0f;
}

static void pattern(float *pixels, int shift)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            for (int c = 0; c < 3; c++)
                pixels[(y * WIDTH + x) * 4 + c] = noise(x - shift, y, c);
            pixels[(y * WIDTH + x) * 4 + 3] = 1;
        }
    }
}

static void copy_texture(pl_gpu gpu, pl_tex source, pl_tex target)
{
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    pl_shader shader = pl_dispatch_begin(dispatch);
    assert(pl_shader_sample_nearest(shader, pl_sample_src(.tex = source)));
    assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
        .shader = &shader, .target = target,
    )));
    pl_dispatch_destroy(&dispatch);
}

static pl_tex upload_size(pl_gpu gpu, const float *pixels, int width, int height)
{
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_SAMPLEABLE);
    assert(format);
    pl_tex texture = pl_tex_create(gpu, pl_tex_params(
        .w = width, .h = height, .format = format,
        .sampleable = true, .host_writable = true, .initial_data = pixels,
    ));
    assert(texture);
    pl_tex source = texture;
    format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_RENDERABLE);
    assert(format);
    texture = pl_tex_create(gpu, pl_tex_params(
        .w = width, .h = height, .format = format,
        .sampleable = true, .renderable = true,
    ));
    assert(texture);
    copy_texture(gpu, source, texture);
    pl_tex_destroy(gpu, &source);
    return texture;
}

static pl_tex upload(pl_gpu gpu, const float *pixels)
{
    return upload_size(gpu, pixels, WIDTH, HEIGHT);
}

static bool map_cadence_frame(pl_gpu gpu, pl_tex *tex,
                              const struct pl_source_frame *source,
                              struct pl_frame *frame)
{
    (void) gpu;
    (void) tex;
    *frame = (struct pl_frame) {.user_data = source->frame_data};
    return true;
}

static void motion_queue_cadence(pl_gpu gpu)
{
    const double duration = 1.0 / 30;
    pl_queue queue = pl_queue_create(gpu);
    assert(queue);
    for (int i = 0; i < 40; i++) {
        pl_queue_push(queue, &(struct pl_source_frame) {
            .pts = i * duration, .duration = duration, .map = map_cadence_frame,
        });
    }

    struct pl_frame_mix mix;
    struct pl_queue_params params = *pl_queue_params(
        .radius = 1, .vsync_duration = 1.0 / 60,
    );
    params.interpolation_threshold = queue_interpolation_threshold(false, 0.01);
    // Simulate falling behind by one display slot for every source frame.
    for (int i = 0; i < 35; i++) {
        params.pts = i * duration;
        assert(pl_queue_update(queue, &mix, &params) == PL_QUEUE_OK);
    }
    assert(fabs(pl_queue_estimate_vps(queue) - 30) < 0.01);
    params.pts = 35.5 * duration;
    assert(pl_queue_update(queue, &mix, &params) == PL_QUEUE_OK);
    assert(mix.num_frames == 1);

    params.interpolation_threshold = queue_interpolation_threshold(true, 0.01);
    assert(pl_queue_update(queue, &mix, &params) == PL_QUEUE_OK);
    assert(mix.num_frames >= 2);
    assert(mix.timestamps[0] < 0 && mix.timestamps[mix.num_frames - 1] > 0);
    pl_queue_destroy(&queue);
}

static void readback(pl_gpu gpu, pl_tex source, float *pixels)
{
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = source->params.w, .h = source->params.h, .format = format,
        .renderable = true, .host_readable = true,
    ));
    assert(target);
    copy_texture(gpu, source, target);
    assert(pl_tex_download(gpu, pl_tex_transfer_params(.tex = target, .ptr = pixels)));
    pl_tex_destroy(gpu, &target);
}

static float signal(float x, float y, int channel)
{
    return 0.5f + 0.2f * sinf(x * (0.13f + channel * 0.03f) + y * 0.07f) +
           0.2f * cosf(y * (0.17f + channel * 0.02f) - x * 0.05f);
}

static float affine_signal(float x, float y, int channel, float alpha)
{
    float a = 1 + 0.015f * alpha;
    float b = -0.04f * alpha;
    float c = 0.03f * alpha;
    float d = 1 - 0.01f * alpha;
    float qx = x - 64.0f - alpha * 4;
    float qy = y - 48.0f + alpha * 2;
    float determinant = a * d - b * c;
    return signal((d * qx - b * qy) / determinant + 64,
                  (a * qy - c * qx) / determinant + 48, channel);
}

static void affine_pair(pl_gpu gpu, struct mp_motion *motion,
                        int width, int height, int scale)
{
    float *first = calloc(width * height * 4, sizeof(float));
    float *second = calloc(width * height * 4, sizeof(float));
    float *output = calloc(width * height * 4, sizeof(float));
    assert(first && second && output);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            for (int c = 0; c < 4; c++) {
                int index = (y * width + x) * 4 + c;
                float sx = (x + 0.5f) / scale - 0.5f;
                float sy = (y + 0.5f) / scale - 0.5f;
                first[index] = c == 3 ? 1 : affine_signal(sx, sy, c, 0);
                second[index] = c == 3 ? 1 : affine_signal(sx, sy, c, 1);
            }
        }
    }
    pl_tex a = upload_size(gpu, first, width, height);
    pl_tex b = upload_size(gpu, second, width, height);
    for (int step = 1; step <= 3; step++) {
        float alpha = step * 0.25f;
        pl_tex result;
        assert(mp_motion_render(motion, a, b, 30, 31, alpha, &result));
        assert(result->params.w == width && result->params.h == height);
        readback(gpu, result, output);
        double motion_error = 0, blend_error = 0;
        int samples = 0;
        int margin = 16 * scale;
        for (int y = margin; y < height - margin; y++) {
            for (int x = margin; x < width - margin; x++) {
                for (int c = 0; c < 3; c++) {
                    int index = (y * width + x) * 4 + c;
                    float sx = (x + 0.5f) / scale - 0.5f;
                    float sy = (y + 0.5f) / scale - 0.5f;
                    float expected = affine_signal(sx, sy, c, alpha);
                    float blend = first[index] * (1 - alpha) + second[index] * alpha;
                    assert(isfinite(output[index]));
                    motion_error += fabs(output[index] - expected);
                    blend_error += fabs(blend - expected);
                    samples++;
                }
            }
        }
        motion_error /= samples;
        blend_error /= samples;
        printf("affine %dx%d alpha=%.2f motion_error=%.6f blend_error=%.6f\n",
               width, height, alpha, motion_error, blend_error);
        fflush(stdout);
        assert(motion_error < 0.02);
        assert(motion_error < blend_error * 0.65);
    }
    mp_motion_reset(motion);
    pl_tex_destroy(gpu, &a);
    pl_tex_destroy(gpu, &b);
    free(first);
    free(second);
    free(output);
}

static void translated_pair(pl_gpu gpu, struct mp_motion *motion)
{
    float *first = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *second = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(first && second && output);
    pattern(first, 0);
    pattern(second, 20);
    pl_tex a = upload(gpu, first);
    pl_tex b = upload(gpu, second);
    for (int step = 1; step <= 3; step++) {
        float alpha = step * 0.25f;
        pl_tex result;
        assert(mp_motion_render(motion, a, b, 10, 11, alpha, &result));
        readback(gpu, result, output);
        double motion_error = 0;
        double blend_error = 0;
        int samples = 0;
        for (int y = 24; y < HEIGHT - 24; y++) {
            for (int x = 40; x < WIDTH - 40; x++) {
                for (int c = 0; c < 3; c++) {
                    int index = (y * WIDTH + x) * 4 + c;
                    float expected = noise(x - step * 5, y, c);
                    float blend = first[index] * (1 - alpha) + second[index] * alpha;
                    assert(isfinite(output[index]));
                    motion_error += fabs(output[index] - expected);
                    blend_error += fabs(blend - expected);
                    samples++;
                }
            }
        }
        motion_error /= samples;
        blend_error /= samples;
        printf("translation alpha=%.2f motion_error=%.6f blend_error=%.6f\n",
               alpha, motion_error, blend_error);
        fflush(stdout);
        assert(motion_error < 0.04);
        assert(motion_error < blend_error * 0.2);
        struct mp_motion_stats stats = mp_motion_get_stats(motion);
        assert(stats.analysis_width == WIDTH && stats.analysis_height == HEIGHT);
        assert(stats.synthesis_width == WIDTH && stats.synthesis_height == HEIGHT);
        assert(stats.analyzed_pairs == 1);
        assert(stats.generated_frames == (uint64_t) step);
        pl_tex repeated;
        assert(mp_motion_render(motion, a, b, 10, 11, alpha, &repeated));
        assert(repeated == result);
        assert(mp_motion_get_stats(motion).generated_frames == stats.generated_frames);
        assert(mp_motion_get_stats(motion).repeated_frames == (uint64_t) step);
    }
    assert(!mp_motion_render(motion, a, b, 10, 11, NAN, &(pl_tex){0}));
    assert(!mp_motion_render(motion, a, b, 10, 11, 0, &(pl_tex){0}));
    assert(!mp_motion_render(motion, a, b, 10, 11, 1, &(pl_tex){0}));

    pl_tex retained, next, borrowed;
    assert(mp_motion_render_owned(motion, a, b, 10, 11, 0.75f, &retained));
    assert(mp_motion_render_owned(motion, a, b, 10, 11, 0.75f, &next));
    assert(retained != next);
    readback(gpu, next, first);
    assert(!memcmp(first, output, WIDTH * HEIGHT * 4 * sizeof(*first)));
    pl_tex_destroy(gpu, &next);

    assert(mp_motion_render(motion, a, b, 10, 11, 0.25f, &borrowed));
    assert(!mp_motion_render_owned(motion, a, a, 10, 10, 0.5f, &borrowed));
    assert(!borrowed);
    mp_motion_reset(motion);
    pl_tex_destroy(gpu, &a);
    pl_tex_destroy(gpu, &b);
    readback(gpu, retained, first);
    assert(!memcmp(first, output, WIDTH * HEIGHT * 4 * sizeof(*first)));
    pl_tex_destroy(gpu, &retained);
    free(first);
    free(second);
    free(output);
}

static void diagonal_pixels(float *pixels, int width, int height, int scale,
                             float shift, bool bands, int variant)
{
    static const float background[6][3] = {
        {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1},
    };
    bool horizontal = variant >= 30;
    if (horizontal)
        variant -= 2;
    int tangent = horizontal ? height : width;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int px = horizontal ? y : x;
            int py = horizontal ? x : y;
            if (variant >= 28 && px < tangent / 3) {
                int row = (int)floorf((py + 0.5f - shift * scale) / 4);
                unsigned hash = (unsigned)(px / 4) * 1664525u +
                                (unsigned)(row + 4096) * 1013904223u;
                hash ^= hash >> 16;
                float value = 0.2f + 0.6f * (hash % 512) / 511;
                for (int c = 0; c < 3; c++)
                    pixels[(y * width + x) * 4 + c] = value;
                pixels[(y * width + x) * 4 + 3] = 1;
                continue;
            }
            float origin = variant == 1 ? 96 : variant == 2 ? 180 : 24;
            float slope = variant == 2 || (variant >= 20 && variant < 24)
                        ? -0.25f : 0.25f;
            float center = (origin + shift) * scale + slope * (px + 0.5f);
            float edge = 2 * scale + 0.5f - fabsf(py + 0.5f - center);
            if (variant >= 4) {
                const int periods[] = {32, 40, 48, 64};
                int index = variant < 20 ? (variant - 4) / 4
                          : variant < 24 ? 0 : variant < 28 ? 2 : 3;
                float period = periods[index];
                float phase = remainderf(py + 0.5f - center - period * 0.5f, period);
                edge = 1.5f - fabsf(phase);
            }
            float coverage = fminf(1, fmaxf(0, edge));
            for (int c = 0; c < 3; c++) {
                float base = bands ? background[px * 6 / tangent][c] : 0;
                pixels[(y * width + x) * 4 + c] =
                    base + coverage * (1 - 2 * base);
                if (variant == 3 || (variant >= 24 && variant < 28))
                    pixels[(y * width + x) * 4 + c] =
                        0.45f + 0.1f * pixels[(y * width + x) * 4 + c];
            }
            pixels[(y * width + x) * 4 + 3] = 1;
        }
    }
}

static void diagonal_edge(pl_gpu gpu, struct mp_motion *motion,
                           bool bands, int variant)
{
    int width = 1920, height = 1080, scale = 4;
    size_t bytes = width * height * 4 * sizeof(float);
    float *pixels = malloc(bytes);
    float *expected = malloc(bytes);
    float *output = malloc(bytes);
    assert(pixels && expected && output);
    const float shifts[] = {1, -1, 2, -2};
    float displacement = variant >= 28 ? (variant % 2 ? -17 : 17)
                       : variant >= 4 ? shifts[(variant - 4) % 4]
                       : variant == 1 ? -16 : 16;
    diagonal_pixels(pixels, width, height, scale, 0, bands, variant);
    pl_tex a = upload_size(gpu, pixels, width, height);
    diagonal_pixels(pixels, width, height, scale, displacement, bands, variant);
    pl_tex b = upload_size(gpu, pixels, width, height);
    for (int step = 1; step <= 3; step++) {
        float alpha = step * 0.25f;
        diagonal_pixels(expected, width, height, scale, displacement * alpha,
                        bands, variant);
        pl_tex result;
        assert(mp_motion_render(motion, a, b, 90, 91, alpha, &result));
        readback(gpu, result, output);
        float maximum = 0;
        double error = 0;
        int count = 0;
        bool horizontal = variant >= 30;
        int top = horizontal ? height / 3 + 16 : (variant >= 28 ? 32 : 16) * scale;
        int bottom = horizontal ? height / 3 + 128 : height - top;
        int left = horizontal ? 32 * scale
                 : variant >= 28 ? width / 3 + 16 : 16 * scale;
        int right = horizontal ? width - 32 * scale
                  : variant >= 28 ? width / 3 + 128 : width - 16 * scale;
        for (int y = top; y < bottom; y++) {
            for (int x = left; x < right; x++) {
                for (int c = 0; c < 3; c++) {
                    int index = (y * width + x) * 4 + c;
                    assert(isfinite(output[index]));
                    float difference = fabsf(output[index] - expected[index]);
                    if (variant == 3 || (variant >= 24 && variant < 28))
                        difference /= 0.1f;
                    maximum = fmaxf(maximum, difference);
                    error += difference;
                    count++;
                }
            }
        }
        printf("diagonal variant=%d %s alpha=%.2f error=%.6f maximum=%.6f\n",
               variant, bands ? "bands" : "flat", alpha, error / count,
               maximum);
        fflush(stdout);
        assert(error / count < 0.02);
        assert(maximum < 0.1);
    }
    mp_motion_reset(motion);
    pl_tex_destroy(gpu, &a);
    pl_tex_destroy(gpu, &b);
    free(pixels);
    free(expected);
    free(output);
}

static void static_and_cut(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output);
    pattern(pixels, 0);
    pl_tex source = upload(gpu, pixels);
    pl_tex result;
    assert(mp_motion_render(motion, source, source, 20, 20, 0.5f, &result));
    readback(gpu, result, output);
    for (int i = 0; i < WIDTH * HEIGHT * 4; i++)
        assert(fabs(output[i] - pixels[i]) < 0.001);
    pl_tex_destroy(gpu, &source);
    for (int i = 0; i < WIDTH * HEIGHT * 4; i++)
        pixels[i] = i % 4 == 3 ? 1 : 0;
    pl_tex black = upload(gpu, pixels);
    for (int i = 0; i < WIDTH * HEIGHT * 4; i++)
        pixels[i] = 1;
    pl_tex white = upload(gpu, pixels);
    for (int step = 1; step <= 3; step++) {
        float alpha = step * 0.25f;
        assert(mp_motion_render(motion, black, white, 21, 22, alpha, &result));
        readback(gpu, result, output);
        float expected = alpha < 0.5f ? 0 : 1;
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            for (int c = 0; c < 3; c++)
                assert(fabs(output[i * 4 + c] - expected) < 0.001);
        }
    }
    pl_tex_destroy(gpu, &black);
    pl_tex_destroy(gpu, &white);
    free(pixels);
    free(output);
    puts("static, scene cut, invalid alpha, and reset: passed");
}

static void straight_edge(pl_gpu gpu, struct mp_motion *motion,
                          int scale, bool horizontal, int thickness)
{
    int width = WIDTH * scale;
    int height = HEIGHT * scale;
    int origin = horizontal ? 24 : 40;
    int tangent = horizontal ? width : height;
    float *pixels[2] = {calloc(width * height * 4, sizeof(float)),
                        calloc(width * height * 4, sizeof(float))};
    float *output = calloc(width * height * 4, sizeof(float));
    assert(pixels[0] && pixels[1] && output);
    for (int frame = 0; frame < 2; frame++) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int axis = horizontal ? y : x;
                bool bright = axis >= (origin + frame * 16) * scale &&
                              axis < (origin + frame * 16 + thickness) * scale;
                for (int c = 0; c < 4; c++)
                    pixels[frame][(y * width + x) * 4 + c] = c == 3 || bright;
            }
        }
    }
    pl_tex a = upload_size(gpu, pixels[0], width, height);
    pl_tex b = upload_size(gpu, pixels[1], width, height);
    for (int step = 1; step <= 3; step++) {
        float alpha = step * 0.25f;
        pl_tex result;
        assert(mp_motion_render(motion, a, b, 60, 61, alpha, &result));
        assert(result->params.w == width && result->params.h == height);
        readback(gpu, result, output);
        double error = 0;
        float maximum_error = 0;
        int worst_x = 0, worst_y = 0;
        int samples = 0;
        for (int v = 16 * scale; v < tangent - 16 * scale; v++) {
            for (int u = (origin - 8) * scale; u < (origin + 40) * scale; u++) {
                int x = horizontal ? v : u;
                int y = horizontal ? u : v;
                float expected = u >= (origin + 4 * step) * scale &&
                                 u < (origin + 4 * step + thickness) * scale;
                float actual = output[(y * width + x) * 4];
                assert(isfinite(actual));
                float difference = fabs(actual - expected);
                if (difference > maximum_error) {
                    maximum_error = difference;
                    worst_x = x;
                    worst_y = y;
                }
                error += difference;
                samples++;
            }
        }
        printf("straight %s scale=%d width=%d alpha=%.2f error=%.6f maximum=%.6f at=%d,%d\n",
               horizontal ? "horizontal" : "vertical", scale, thickness, alpha,
               error / samples, maximum_error, worst_x, worst_y);
        fflush(stdout);
        assert(error / samples < 0.02);
        assert(maximum_error < 0.1);
    }
    mp_motion_reset(motion);
    pl_tex_destroy(gpu, &a);
    pl_tex_destroy(gpu, &b);
    free(pixels[0]);
    free(pixels[1]);
    free(output);
}

static struct pl_shader_desc policy_sampled(const char *name, pl_tex texture)
{
    return (struct pl_shader_desc) {
        .desc = {.name = name, .type = PL_DESC_SAMPLED_TEX},
        .binding = {.object = texture, .sample_mode = PL_TEX_SAMPLE_LINEAR,
                    .address_mode = PL_TEX_ADDRESS_CLAMP},
    };
}

static pl_tex policy_target(pl_gpu gpu)
{
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = WIDTH, .h = HEIGHT, .format = format,
        .sampleable = true, .renderable = true, .storable = true,
    ));
    assert(target);
    return target;
}

static float bilinear(const float *pixels, int width, int height,
                      float x, float y, int channel)
{
    x = fminf(fmaxf(x, 0), width - 1);
    y = fminf(fmaxf(y, 0), height - 1);
    int x0 = floorf(x), y0 = floorf(y);
    int x1 = x0 + 1 < width ? x0 + 1 : x0;
    int y1 = y0 + 1 < height ? y0 + 1 : y0;
    float fx = x - x0, fy = y - y0;
    float a = pixels[(y0 * width + x0) * 4 + channel];
    float b = pixels[(y0 * width + x1) * 4 + channel];
    float c = pixels[(y1 * width + x0) * 4 + channel];
    float d = pixels[(y1 * width + x1) * 4 + channel];
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

static float matching_cost(const float *first, const float *second,
                           float x, float y, float dx, float dy)
{
    float cost = 0;
    for (int oy = -3; oy <= 3; oy++) {
        for (int ox = -3; ox <= 3; ox++) {
            float error[3];
            for (int c = 0; c < 3; c++) {
                error[c] = bilinear(first, 32, 24, x + ox, y + oy, c) -
                    bilinear(second, 32, 24, x + ox + dx, y + oy + dy, c);
                cost += 0.25f * fabsf(error[c]);
            }
            cost += 0.25f * fabsf(error[0] * 0.2126f + error[1] * 0.7152f +
                                  error[2] * 0.0722f);
        }
    }
    return cost / 49;
}

static void subpixel_matching(pl_gpu gpu)
{
    const struct {
        int radius, columns;
        float dx, dy;
        bool flat;
    } cases[] = {
        {2, 4, 1.00f, -0.5f, false},
        {2, 4, 1.25f, -0.5f, false},
        {2, 4, 1.50f, -0.5f, false},
        {6, 4, 5.25f, 5.5f, false},
        {6, 4, -5.25f, 5.5f, false},
        {6, 4, 5.25f, -5.5f, false},
        {6, 4, -5.25f, -5.5f, false},
        {6, 4, 0, 0, true},
        {6, 5, 1.25f, -0.5f, false},
    };
    float first[32 * 24 * 4], second[32 * 24 * 4], output[5 * 3 * 4];
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_SAMPLEABLE);
    assert(format);
    pl_tex target = NULL;
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(dispatch);
    for (int test = 0; test < (int) (sizeof(cases) / sizeof(cases[0])); test++) {
        int columns = cases[test].columns;
        assert(pl_tex_recreate(gpu, &target, pl_tex_params(
            .w = columns, .h = 3, .format = format,
            .renderable = true, .storable = true, .sampleable = true,
        )));
        for (int y = 0; y < 24; y++) {
            for (int x = 0; x < 32; x++) {
                for (int c = 0; c < 3; c++)
                    first[(y * 32 + x) * 4 + c] = cases[test].flat
                                               ? 0.5f : noise(x, y, c) > 0.5f;
                first[(y * 32 + x) * 4 + 3] = 1;
            }
        }
        for (int y = 0; y < 24; y++) {
            for (int x = 0; x < 32; x++) {
                for (int c = 0; c < 4; c++)
                    second[(y * 32 + x) * 4 + c] = bilinear(
                        first, 32, 24, x - cases[test].dx, y - cases[test].dy, c);
            }
        }
        pl_tex a = upload_size(gpu, first, 32, 24);
        pl_tex b = upload_size(gpu, second, 32, 24);
        struct pl_shader_desc descriptors[] = {
            policy_sampled("first", a), policy_sampled("second", b),
            {
                .desc = {.name = "out_image", .type = PL_DESC_STORAGE_IMG,
                         .access = PL_DESC_ACCESS_WRITEONLY},
                .binding.object = target,
            },
        };
        int settings[] = {columns, 3, cases[test].radius};
        float scale[] = {1, 1};
        struct pl_shader_var variables[] = {
            {.var = pl_var_ivec3("settings"), .data = settings},
            {.var = pl_var_vec2("motion_scale"), .data = scale},
        };
        pl_shader shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_sample_header,
        }));
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_match_header, .body = test_match_body,
            .compute = true, .compute_group_size = {64, 1},
            .compute_shmem = (49 + 361) * 16 + (169 + 2 * 64) * 4,
            .output = PL_SHADER_SIG_NONE,
            .descriptors = descriptors, .num_descriptors = 3,
            .variables = variables, .num_variables = 2,
        }));
        assert(pl_dispatch_compute(dispatch, pl_dispatch_compute_params(
            .shader = &shader, .dispatch_size = {columns, 3, 1},
        )));
        readback(gpu, target, output);
        float maximum_excess = 0, maximum_residual_error = 0;
        bool matched_last_batch = false;
        for (int y = 0; y < 3; y++) {
            for (int x = 0; x < columns; x++) {
                float px = (x + 0.5f) / columns * 32 - 0.5f;
                float py = (y + 0.5f) * 8 - 0.5f;
                float best = INFINITY;
                for (int dy = -settings[2]; dy <= settings[2]; dy++) {
                    for (int dx = -settings[2]; dx <= settings[2]; dx++) {
                        if (px + dx < 0 || px + dx > 31 ||
                            py + dy < 0 || py + dy > 23)
                            continue;
                        float cost = matching_cost(first, second, px, py, dx, dy);
                        best = fminf(best, cost);
                    }
                }
                const float *vector = &output[(y * columns + x) * 4];
                for (int c = 0; c < 4; c++)
                    assert(isfinite(vector[c]));
                float cost = matching_cost(first, second, px, py,
                                           vector[0], vector[1]);
                maximum_excess = fmaxf(maximum_excess, cost - best);
                maximum_residual_error = fmaxf(maximum_residual_error,
                                               fabsf(cost - vector[2]));
                matched_last_batch |= vector[1] > 4;
                if (cases[test].flat) {
                    assert(vector[0] == 0 && vector[1] == 0);
                    assert(vector[3] == 0);
                }
            }
        }
        printf("subpixel case=%d radius=%d excess=%.6f residual_error=%.6f\n",
               test, settings[2], maximum_excess, maximum_residual_error);
        fflush(stdout);
        assert(maximum_excess < 0.001);
        assert(maximum_residual_error < 0.001);
        if (cases[test].dy > 4)
            assert(matched_last_batch);
        pl_tex_destroy(gpu, &a);
        pl_tex_destroy(gpu, &b);
    }
    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &target);
}

static void translation_selection(pl_gpu gpu)
{
    const struct {
        int width, height;
        int first, second, expected;
        float best, next;
    } cases[] = {
        {49, 49, 0, 1201, 0, 0.0f, 0.1f},
        {49, 49, 1201, 1265, 1201, 0.0f, 0.1f},
        {49, 49, 0, 1201, 1201, 0.0f, 0.0f},
        {49, 49, 0, 2400, 0, 0.0f, 0.0f},
        {49, 49, 1200, 1201, 1200, 0.0f, 0.1f},
        {49, 49, 0, 1201, 0, 0.25f, 0.250003f},
        {3, 3, 1, 4, 1, 0.0f, 0.1f},
        {7, 5, 34, 0, 34, 0.1f, 0.2f},
    };
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_SAMPLEABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = 1, .h = 1, .format = format,
        .renderable = true, .storable = true, .sampleable = true,
    ));
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(target && dispatch);
    for (int test = 0; test < (int)(sizeof(cases) / sizeof(cases[0])); test++) {
        int width = cases[test].width, height = cases[test].height;
        int count = width * height;
        int zero = (height / 2) * width + width / 2;
        float *pixels = malloc(count * 4 * sizeof(float));
        assert(pixels);
        for (int index = 0; index < count; index++) {
            float score = index == cases[test].first ? cases[test].best
                        : index == cases[test].second ? cases[test].next : 0.75f;
            uint32_t bits;
            memcpy(&bits, &score, sizeof(bits));
            for (int c = 0; c < 4; c++)
                pixels[index * 4 + c] = (bits >> (8 * c)) & 255;
        }
        pl_tex costs = upload_size(gpu, pixels, width, height);
        free(pixels);
        struct pl_shader_desc descriptors[] = {
            policy_sampled("costs", costs),
            {
                .desc = {.name = "out_image", .type = PL_DESC_STORAGE_IMG,
                         .access = PL_DESC_ACCESS_WRITEONLY},
                .binding.object = target,
            },
        };
        pl_shader shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_sample_header,
        }));
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_translation_select_header,
            .body = test_translation_select_body,
            .compute = true, .compute_group_size = {64, 1},
            .compute_shmem = 64 * 16, .output = PL_SHADER_SIG_NONE,
            .descriptors = descriptors, .num_descriptors = 2,
        }));
        assert(pl_dispatch_compute(dispatch, pl_dispatch_compute_params(
            .shader = &shader, .dispatch_size = {1, 1, 1},
        )));
        float output[4];
        readback(gpu, target, output);
        for (int c = 0; c < 4; c++)
            assert(isfinite(output[c]));
        int expected = cases[test].expected;
        assert(output[0] == expected % width - width / 2);
        assert(output[1] == expected / width - height / 2);
        assert(fabsf(output[2] - cases[test].best) < 0.0001f);
        float confidence = expected == zero ? 0.0f
                         : fmaxf(0.0f, (cases[test].next - cases[test].best - 0.000001f) /
                                       fmaxf(cases[test].next, 0.000001f));
        assert(fabsf(output[3] - confidence) < 0.001f);
        printf("translation selection case=%d vector=%.0f,%.0f confidence=%.6f\n",
               test, output[0], output[1], output[3]);
        pl_tex_destroy(gpu, &costs);
    }
    pl_tex_destroy(gpu, &target);
    pl_dispatch_destroy(&dispatch);
}

static void warp_policy(pl_gpu gpu)
{
    float *pixels[2] = {calloc(WIDTH * HEIGHT * 4, sizeof(float)),
                        calloc(WIDTH * HEIGHT * 4, sizeof(float))};
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels[0] && pixels[1] && output);
    pl_tex target = policy_target(gpu);
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(dispatch);
    for (int test = 0; test < 7; test++) {
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                int index = (y * WIDTH + x) * 4;
                pixels[0][index] = test == 3 ? WIDTH * 2 : 8;
                pixels[1][index] = test == 1 ? 8 : -8;
                if (test == 4)
                    pixels[0][index] = NAN;
                if (test == 5) {
                    pixels[0][index] = -0.5f * (x + 0.5f - WIDTH * 0.5f);
                    pixels[1][index] = x + 0.5f - WIDTH * 0.5f;
                }
                pixels[0][index + 2] = test == 2 ? 0.1f : 0;
                pixels[1][index + 2] = test == 6 ? 0.1f : 0;
            }
        }
        pl_tex a = upload(gpu, pixels[0]);
        pl_tex b = upload(gpu, pixels[1]);
        struct pl_shader_desc descriptors[] = {
            policy_sampled("forward", a), policy_sampled("backward", b),
        };
        float map_size[] = {WIDTH, HEIGHT};
        float size[] = {WIDTH * (test == 5 ? 128 : 1), HEIGHT};
        float weight = 0.5f;
        struct pl_shader_var variables[] = {
            {.var = pl_var_vec2("map_size"), .data = map_size},
            {.var = pl_var_vec2("output_size"), .data = size},
            {.var = pl_var_float("weight"), .data = &weight},
        };
        pl_shader shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_synthesis_header, .body = test_warp_body,
            .compute = true, .compute_group_size = {8, 8},
            .output = PL_SHADER_SIG_COLOR,
            .descriptors = descriptors, .num_descriptors = 2,
            .variables = variables, .num_variables = 3,
        }));
        assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
            .shader = &shader, .target = target,
        )));
        readback(gpu, target, output);
        int index = ((HEIGHT / 2) * WIDTH + WIDTH / 4) * 4;
        assert(isfinite(output[index + 2]) && isfinite(output[index + 3]));
        assert(output[index + 2] == 0);
        if (test == 0 || test == 5) {
            // The affine case solves (source - center) * 0.75 = output - center.
            float displacement = test == 5 ? -10.5f : -4.0f;
            assert(fabsf(output[index] - displacement) < 0.001f);
            // The stress case magnifies the residual at 128x output scale.
            assert(output[index + 3] > (test == 5 ? 0.9f : 0.99f));
        } else {
            assert(output[index + 3] == 0);
        }
        printf("motion policy case=%d unique=%.3f recovered=%.3f\n",
               test, output[index + 2], output[index + 3]);
        pl_tex_destroy(gpu, &a);
        pl_tex_destroy(gpu, &b);
    }
    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &target);
    free(pixels[0]);
    free(pixels[1]);
    free(output);
}

static void synthesis_policy(pl_gpu gpu)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output);
    pl_tex target = policy_target(gpu);
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(dispatch);
    // Case 6 has a valid forward trajectory and a conflicting reverse one.
    static const float expected[] = {1, 0, 0, 0, 0, 1, 0.75f, 0.5f, 0.5f, 0.5f};
    for (int test = 0; test < sizeof(expected) / sizeof(expected[0]); test++) {
        pl_tex inputs[5];
        for (int input = 0; input < 5; input++) {
            for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                    int index = (y * WIDTH + x) * 4;
                    for (int c = 0; c < 4; c++)
                        pixels[index + c] = 0;
                    if (input < 2) {
                        float value = test == 1 && input == 1 ? 0.95f : 1;
                        bool bright = x >= 40 + 16 * input && x < 48 + 16 * input;
                        bright |= test >= 5 && input == 0 && x == 50;
                        for (int c = 0; c < 3; c++)
                            pixels[index + c] = bright ? value : 0;
                        pixels[index + 3] = 1;
                    } else if (input < 4) {
                        pixels[index] = test == 7 ? 0 : input == 2 ? -8 : test == 6 ? 4 : 8;
                        if (test == 2 && input == 2)
                            pixels[index] = NAN;
                        pixels[index + 2] = test == 7 && input == 2 ? 0.25f : test >= 4 ? 1 : 0;
                        pixels[index + 3] = test >= 4 || (test == 3 && input == 3) ? 0 : 1;
                        if (test == 8 && input == 2)
                            pixels[index + 2] = INFINITY;
                        if (test == 9 && input == 3)
                            pixels[index + 3] = -INFINITY;
                    }
                }
            }
            inputs[input] = upload(gpu, pixels);
        }
        struct pl_shader_desc descriptors[] = {
            policy_sampled("first", inputs[0]), policy_sampled("second", inputs[1]),
            policy_sampled("forward", inputs[2]), policy_sampled("backward", inputs[3]),
            policy_sampled("scene", inputs[4]),
        };
        float size[] = {WIDTH, HEIGHT};
        float alpha = 0.5f;
        struct pl_shader_var variables[] = {
            {.var = pl_var_vec2("motion_size"), .data = size},
            {.var = pl_var_vec2("output_size"), .data = size},
            {.var = pl_var_float("alpha"), .data = &alpha},
            {.var = pl_var_int("linear_hdr"), .data = &(int){0}},
            {.var = pl_var_vec2("projection_ratio"),
             .data = (float[]){(1.0f - alpha) / alpha, alpha / (1.0f - alpha)}},
        };
        pl_shader shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_sample_header,
        }));
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_synthesis_pixel_header,
            .body = test_synthesis_body,
            .compute = true, .compute_group_size = {8, 8},
            .output = PL_SHADER_SIG_COLOR,
            .descriptors = descriptors, .num_descriptors = 5,
            .variables = variables, .num_variables = 5,
        }));
        assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
            .shader = &shader, .target = target,
        )));
        readback(gpu, target, output);
        float value = output[((HEIGHT / 2) * WIDTH + 50) * 4];
        assert(isfinite(value));
        assert(fabsf(value - expected[test]) < 0.001f);
        printf("synthesis policy case=%d midpoint=%.3f\n", test, value);
        for (int input = 0; input < 5; input++)
            pl_tex_destroy(gpu, &inputs[input]);
    }
    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &target);
    free(pixels);
    free(output);
}

static void scene_transitions(pl_gpu gpu, struct mp_motion *motion, bool fade)
{
    float *first = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *second = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(first && second && output);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            for (int c = 0; c < 4; c++) {
                int index = (y * WIDTH + x) * 4 + c;
                first[index] = c == 3 ? 1 : fade
                             ? 0.05f + 0.8f * noise(x, y, 0) : noise(x, y, c);
                second[index] = c == 3 ? 1 : fade ? first[index] + 0.12f
                              : noise((37 * x + 53) % WIDTH,
                                      (17 * y + 31) % HEIGHT, c);
            }
        }
    }
    pl_tex a = upload(gpu, first);
    pl_tex b = upload(gpu, second);
    for (int step = 1; step <= 3; step++) {
        float alpha = step * 0.25f;
        pl_tex result;
        assert(mp_motion_render(motion, a, b, 50, 51, alpha, &result));
        readback(gpu, result, output);
        double error = 0;
        for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
            float expected = fade ? first[i] * (1 - alpha) + second[i] * alpha
                           : alpha < 0.5f ? first[i] : second[i];
            assert(isfinite(output[i]));
            error += fabs(output[i] - expected);
        }
        error /= WIDTH * HEIGHT * 4;
        printf("%s alpha=%.2f error=%.6f\n",
               fade ? "brightness fade" : "equal-histogram scene cut", alpha, error);
        fflush(stdout);
        assert(error < 0.003);
    }
    mp_motion_reset(motion);
    pl_tex_destroy(gpu, &a);
    pl_tex_destroy(gpu, &b);
    free(first);
    free(second);
    free(output);
}

static void periodic_scene_translation(pl_gpu gpu)
{
    enum { SIZE = 16, SHIFT = 4 };
    float pixels[2][SIZE * SIZE * 4];
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int offset = (y * SIZE + x) * 4;
            float color = (x / SHIFT) % 2;
            for (int c = 0; c < 3; c++) {
                pixels[0][offset + c] = color;
                pixels[1][offset + c] = 1.0f - color;
            }
            pixels[0][offset + 3] = pixels[1][offset + 3] = 1.0f;
        }
    }

    // Translation of a periodic pattern changes every tile without a cut.
    // The synthetic motion fields model a match with zero residual.
    const float forward[] = {SHIFT, 0, 0, 1};
    const float backward[] = {-SHIFT, 0, 0, 1};
    pl_tex first = upload_size(gpu, pixels[0], SIZE, SIZE);
    pl_tex second = upload_size(gpu, pixels[1], SIZE, SIZE);
    pl_tex fwd = upload_size(gpu, forward, 1, 1);
    pl_tex bwd = upload_size(gpu, backward, 1, 1);
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = 1, .h = 1, .format = format,
        .sampleable = true, .renderable = true, .storable = true,
    ));
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(target && dispatch);
    struct pl_shader_desc descriptors[] = {
        policy_sampled("first", first),
        policy_sampled("second", second),
        policy_sampled("forward", fwd),
        policy_sampled("backward", bwd),
    };
    pl_shader shader = pl_dispatch_begin(dispatch);
    assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
        .body = test_scene_body, .compute = true, .compute_group_size = {1, 1},
        .output = PL_SHADER_SIG_COLOR,
        .descriptors = descriptors,
        .num_descriptors = sizeof(descriptors) / sizeof(descriptors[0]),
    }));
    assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
        .shader = &shader, .target = target,
    )));
    float output[4];
    readback(gpu, target, output);
    printf("periodic scene translation: cut=%.3f residual=%.3f\n",
           output[0], output[1]);
    fflush(stdout);
    assert(output[0] == 0 && output[1] == 0 && output[3] == 1);

    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &target);
    pl_tex_destroy(gpu, &bwd);
    pl_tex_destroy(gpu, &fwd);
    pl_tex_destroy(gpu, &second);
    pl_tex_destroy(gpu, &first);
}

static void foreground_pair(pl_gpu gpu, struct mp_motion *motion,
                            int width, int height, int scale)
{
    float *pixels[2] = {calloc(width * height * 4, sizeof(float)),
                        calloc(width * height * 4, sizeof(float))};
    float *output = calloc(width * height * 4, sizeof(float));
    assert(pixels[0] && pixels[1] && output);
    for (int frame = 0; frame < 2; frame++) {
        int shift = frame * 16;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                bool foreground = x >= (24 + shift) * scale &&
                                  x < (88 + shift) * scale &&
                                  y >= 16 * scale && y < 80 * scale;
                for (int c = 0; c < 4; c++) {
                    float sx = (x + 0.5f) / scale - 0.5f;
                    float sy = (y + 0.5f) / scale - 0.5f;
                    pixels[frame][(y * width + x) * 4 + c] = c == 3 ? 1 :
                        foreground ? noise(x / scale - shift, y / scale, c)
                                   : signal(sx, sy, c);
                }
            }
        }
    }
    pl_tex a = upload_size(gpu, pixels[0], width, height);
    pl_tex b = upload_size(gpu, pixels[1], width, height);
    pl_tex result;
    assert(mp_motion_render(motion, a, b, 40, 41, 0.5f, &result));
    assert(result->params.w == width && result->params.h == height);
    readback(gpu, result, output);
    double foreground_error = 0, background_error = 0;
    for (int y = 36 * scale; y < 60 * scale; y++) {
        for (int x = 56 * scale; x < 72 * scale; x++) {
            for (int c = 0; c < 3; c++)
                foreground_error += fabs(output[(y * width + x) * 4 + c] -
                                          noise(x / scale - 8, y / scale, c));
        }
    }
    for (int y = 2 * scale; y < 10 * scale; y++) {
        for (int x = 2 * scale; x < 10 * scale; x++) {
            float sx = (x + 0.5f) / scale - 0.5f;
            float sy = (y + 0.5f) / scale - 0.5f;
            for (int c = 0; c < 3; c++)
                background_error += fabs(output[(y * width + x) * 4 + c] -
                                          signal(sx, sy, c));
        }
    }
    foreground_error /= 24 * 16 * 3 * scale * scale;
    background_error /= 8 * 8 * 3 * scale * scale;
    printf("independent foreground %dx%d: foreground_error=%.6f "
           "background_error=%.6f\n",
           width, height, foreground_error, background_error);
    fflush(stdout);
    assert(foreground_error < 0.04);
    assert(background_error < 0.003);
    mp_motion_reset(motion);
    pl_tex_destroy(gpu, &a);
    pl_tex_destroy(gpu, &b);
    free(pixels[0]);
    free(pixels[1]);
    free(output);
}

struct captured_source {
    pl_tex texture;
    int acquired;
    int released;
};

static float ramp(float x, float y, int channel)
{
    if (channel == 0)
        return 0.1f + 0.6f * x / WIDTH;
    if (channel == 1)
        return 0.1f + 0.6f * y / HEIGHT;
    return 0.1f + 0.2f * x / WIDTH + 0.2f * y / HEIGHT;
}

static bool acquire(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu;
    struct captured_source *source = frame->user_data;
    source->acquired++;
    frame->planes[0].texture = source->texture;
    return true;
}

static void release(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu;
    struct captured_source *source = frame->user_data;
    source->released++;
}

static void capture_ownership(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output);
    pattern(pixels, 0);
    struct captured_source source = {.texture = upload(gpu, pixels)};
    struct pl_frame frame = {
        .num_planes = 1,
        .planes[0] = {.components = 4, .component_mapping = {0, 1, 2, 3}},
        .repr = pl_color_repr_rgb, .color = pl_color_space_bt709,
        .rotation = PL_ROTATION_90,
        .acquire = acquire, .release = release, .user_data = &source,
    };
    pl_tex captured = NULL;
    assert(mp_motion_capture(motion, &frame, WIDTH, HEIGHT, &captured));
    assert(source.acquired > 0 && source.acquired == source.released);
    assert(frame.rotation == PL_ROTATION_90);
    pl_tex_destroy(gpu, &source.texture);
    readback(gpu, captured, output);
    for (int i = 0; i < WIDTH * HEIGHT * 4; i++)
        assert(fabs(output[i] - pixels[i]) < 0.002);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            for (int c = 0; c < 3; c++)
                pixels[(y * WIDTH + x) * 4 + c] = ramp(x, y, c);
        }
    }
    source.texture = upload(gpu, pixels);
    frame.rotation = PL_ROTATION_0;
    frame.color.transfer = PL_COLOR_TRC_LINEAR;
    frame.crop = (struct pl_rect2df) {8, 4, 120, 92};
    assert(mp_motion_capture(motion, &frame, WIDTH, HEIGHT, &captured));
    assert(source.acquired == source.released);
    readback(gpu, captured, output);
    float maximum_error = 0;
    for (int y = 4; y < HEIGHT - 4; y++) {
        for (int x = 4; x < WIDTH - 4; x++) {
            for (int c = 0; c < 3; c++) {
                float sx = 8 + (x + 0.5f) * 112 / WIDTH - 0.5f;
                float sy = 4 + (y + 0.5f) * 88 / HEIGHT - 0.5f;
                float error = fabs(output[(y * WIDTH + x) * 4 + c] -
                                   ramp(sx, sy, c));
                if (error > maximum_error)
                    maximum_error = error;
            }
        }
    }
    printf("capture source crop: maximum error %.6f\n", maximum_error);
    fflush(stdout);
    assert(maximum_error < 0.003);
    pl_tex_destroy(gpu, &source.texture);
    pl_tex_destroy(gpu, &captured);
    free(pixels);
    free(output);
    puts("capture: balanced acquire/release, independent RGB storage, unrotated pixels passed");
}

static void color_roundtrip(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *expected = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && expected && output);
    pattern(pixels, 0);
    pl_tex source = upload(gpu, pixels);
    struct pl_frame frame = {
        .num_planes = 1,
        .planes[0] = {
            .texture = source, .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_bt709,
    };
    frame.color.primaries = PL_COLOR_PRIM_BT_601_525;
    frame.color.hdr.max_luma = 160;
    pl_tex captured = NULL;
    assert(mp_motion_capture(motion, &frame, WIDTH, HEIGHT, &captured));
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = WIDTH, .h = HEIGHT, .format = format,
        .renderable = true, .host_readable = true,
    ));
    assert(target);
    struct pl_frame destination = frame;
    destination.planes[0].texture = target;
    destination.color = pl_color_space_bt709;
    pl_renderer renderer = pl_renderer_create(gpu->log, gpu);
    struct pl_render_params params = pl_render_default_params;
    params.dither_params = NULL;
    params.peak_detect_params = NULL;
    assert(pl_render_image(renderer, &frame, &destination, &params));
    assert(pl_tex_download(gpu, pl_tex_transfer_params(.tex = target, .ptr = expected)));
    frame.planes[0].texture = captured;
    assert(frame.color.hdr.max_luma == 160);
    assert(pl_render_image(renderer, &frame, &destination, &params));
    assert(pl_tex_download(gpu, pl_tex_transfer_params(.tex = target, .ptr = output)));
    float maximum_error = 0;
    for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
        float error = fabs(output[i] - expected[i]);
        if (error > maximum_error)
            maximum_error = error;
    }
    printf("BT.601 RGB capture and SDR reference white: maximum error %.6f\n", maximum_error);
    fflush(stdout);
    assert(maximum_error < 0.003);
    pl_renderer_destroy(&renderer);
    pl_tex_destroy(gpu, &target);
    pl_tex_destroy(gpu, &source);
    pl_tex_destroy(gpu, &captured);
    free(pixels);
    free(expected);
    free(output);
}

static pl_tex cache_input(pl_gpu gpu, int width, int height, int offset)
{
    float *pixels = calloc(width * height * 4, sizeof(float));
    assert(pixels);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            for (int c = 0; c < 4; c++)
                pixels[(y * width + x) * 4 + c] = c == 3
                    ? 1 : noise(x + offset, y - offset, c);
        }
    }
    pl_tex result = upload_size(gpu, pixels, width, height);
    free(pixels);
    return result;
}

static void cache_compare(pl_gpu gpu, struct mp_motion *motion,
                          struct mp_motion *fresh, pl_tex first, pl_tex second,
                          uint64_t first_id, uint64_t second_id, float alpha,
                          bool expected_analysis)
{
    struct mp_motion_stats stats = mp_motion_get_stats(motion);
    pl_tex result = NULL;
    assert(mp_motion_render(motion, first, second, first_id, second_id,
                            alpha, &result));
    assert(mp_motion_get_stats(motion).analyzed_pairs - stats.analyzed_pairs ==
           expected_analysis);
    assert(result->params.w == first->params.w);
    assert(result->params.h == first->params.h);
    size_t count = (size_t)first->params.w * first->params.h * 4;
    float *actual = calloc(count, sizeof(float));
    float *expected = calloc(count, sizeof(float));
    assert(actual && expected);
    readback(gpu, result, actual);
    mp_motion_reset(fresh);
    assert(mp_motion_render(fresh, first, second, first_id, second_id,
                            alpha, &result));
    readback(gpu, result, expected);
    for (size_t i = 0; i < count; i++) {
        assert(isfinite(actual[i]) && isfinite(expected[i]));
        assert(actual[i] == expected[i]);
    }
    free(actual);
    free(expected);
}

static void pyramid_reuse(pl_gpu gpu)
{
    pl_tex inputs[6];
    for (int i = 0; i < 6; i++)
        inputs[i] = cache_input(gpu, i < 4 ? 128 : 256,
                               i < 4 ? 96 : 192, i * 3);
    const struct {
        const char *name;
        int first, second;
        uint64_t first_id, second_id;
        bool reset, analyze;
    } cases[] = {
        {"forward-adjacent", 1, 2, 11, 12, false, true},
        {"backward-adjacent", 2, 0, 12, 10, false, true},
        {"shared-first", 0, 2, 10, 12, false, true},
        {"shared-second", 2, 1, 12, 11, false, true},
        {"reversed", 1, 0, 11, 10, false, true},
        {"disjoint", 2, 3, 12, 13, false, true},
        {"same-pair-new-alpha", 0, 1, 10, 11, false, false},
        {"reset-same-pair", 0, 1, 10, 11, true, true},
        {"same-ids-new-source-size", 4, 5, 10, 11, false, true},
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        struct mp_motion *motion = mp_motion_create(gpu, 128);
        struct mp_motion *fresh = mp_motion_create(gpu, 128);
        assert(motion && fresh);
        pl_tex result;
        assert(mp_motion_render(motion, inputs[0], inputs[1], 10, 11, 0.2f,
                                &result));
        if (cases[i].reset)
            mp_motion_reset(motion);
        cache_compare(gpu, motion, fresh, inputs[cases[i].first],
                      inputs[cases[i].second], cases[i].first_id,
                      cases[i].second_id, 0.5f, cases[i].analyze);
        printf("pyramid reuse %s: exact output\n", cases[i].name);
        mp_motion_destroy(&motion);
        mp_motion_destroy(&fresh);
    }
    for (int i = 0; i < 6; i++)
        pl_tex_destroy(gpu, &inputs[i]);
    puts("Pyramid reuse regression: passed (9 cases, exact half-output equality)");
}

static void padding_sizes(pl_gpu gpu)
{
    static const int sizes[][2] = {
        {8, 8}, {8, 17}, {17, 8}, {9, 9}, {65, 73}, {129, 97},
    };
    for (int i = 0; i < 6; i++) {
        int width = sizes[i][0], height = sizes[i][1];
        pl_tex first = cache_input(gpu, width, height, 0);
        pl_tex second = cache_input(gpu, width, height, 1);
        struct mp_motion *motion = mp_motion_create(gpu, 480);
        assert(motion);
        for (int step = 1; step <= 3; step++) {
            pl_tex result;
            assert(mp_motion_render(motion, first, second, 1, 2,
                                    step * 0.25f, &result));
            size_t count = (size_t) width * height * 4;
            float *pixels = malloc(count * sizeof(float));
            assert(pixels);
            readback(gpu, result, pixels);
            for (size_t j = 0; j < count; j++)
                assert(isfinite(pixels[j]));
            free(pixels);
            printf("padding size=%dx%d step=%d finite output\n", width, height, step);
        }
        assert(mp_motion_get_stats(motion).analyzed_pairs == 1);
        mp_motion_destroy(&motion);
        pl_tex_destroy(gpu, &first);
        pl_tex_destroy(gpu, &second);
    }
}

static void capabilities(pl_gpu gpu)
{
    assert(!mp_motion_supported(NULL));
    assert(mp_motion_supported(gpu));
    struct pl_gpu_t limited = *gpu;
    limited.glsl.compute = false;
    assert(!mp_motion_supported(&limited));
    limited = *gpu;
    limited.glsl.max_group_threads = 63;
    assert(!mp_motion_supported(&limited));
    limited = *gpu;
    limited.glsl.max_group_size[0] = 63;
    assert(!mp_motion_supported(&limited));
    limited = *gpu;
    limited.glsl.max_group_size[1] = 7;
    assert(!mp_motion_supported(&limited));
    limited = *gpu;
    limited.glsl.max_shmem_size = (49 + 361) * 16 + (169 + 2 * 64) * 4 - 1;
    assert(!mp_motion_supported(&limited));
    limited.glsl.max_shmem_size++;
    limited.glsl.max_group_threads = 64;
    limited.glsl.max_group_size[0] = 64;
    limited.glsl.max_group_size[1] = 8;
    assert(mp_motion_supported(&limited));
}

static float reference_sample(const float *pixels, int width, int height,
                              float x, float y, int channel)
{
    x = fminf(width - 1, fmaxf(0, x));
    y = fminf(height - 1, fmaxf(0, y));
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = x0 + 1 < width ? x0 + 1 : x0;
    int y1 = y0 + 1 < height ? y0 + 1 : y0;
    float fx = x - x0, fy = y - y0;
    float a = pixels[(y0 * width + x0) * 4 + channel];
    float b = pixels[(y0 * width + x1) * 4 + channel];
    float c = pixels[(y1 * width + x0) * 4 + channel];
    float d = pixels[(y1 * width + x1) * 4 + channel];
    return (a + fx * (b - a)) * (1 - fy) + (c + fx * (d - c)) * fy;
}

static float reference_smooth(float lo, float hi, float value)
{
    float t = fminf(1, fmaxf(0, (value - lo) / (hi - lo)));
    return t * t * (3 - 2 * t);
}

static void synthesis_sampling(pl_gpu gpu)
{
    int width = 1920, height = 1080;
    size_t bytes = (size_t)width * height * 4 * sizeof(float);
    float *pixels = malloc(bytes), *first = malloc(bytes);
    float *second = malloc(bytes), *output = malloc(bytes);
    assert(pixels && first && second && output);
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    bool failed = false;
    for (int test = 0; test < 3; test++) {
        width = test == 2 ? 127 : 1920;
        height = test == 2 ? 95 : 1080;
        float shift = test ? .30029296875f : 6;
        diagonal_pixels(pixels, width, height, 4, 0, true, 0);
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
            for (int c = 0; c < 3; c++) {
                int index = (y * width + x) * 4 + c;
                pixels[index] = fminf(1, fmaxf(0, pixels[index] +
                    (noise(x, y, c) - .5f) / 255));
            }
        if (test == 2) for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
            pixels[(y * width + x) * 4 + 3] = (float)x / (width - 1);
        pl_tex a = upload_size(gpu, pixels, width, height);
        diagonal_pixels(pixels, width, height, 4, shift, true, 0);
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
            for (int c = 0; c < 3; c++) {
                int index = (y * width + x) * 4 + c;
                pixels[index] = fminf(1, fmaxf(0, pixels[index] +
                    (noise(x, y, c + (int)(shift * 4)) - .5f) / 255));
            }
        if (test == 2) for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
            pixels[(y * width + x) * 4 + 3] = (float)y / (height - 1);
        pl_tex b = upload_size(gpu, pixels, width, height);
        readback(gpu, a, first);
        readback(gpu, b, second);
        pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
            PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE);
        assert(format);
        pl_tex target = pl_tex_create(gpu, pl_tex_params(
            .w = width, .h = height, .format = format,
            .sampleable = true, .renderable = true, .storable = true));
        assert(target);
        for (int step = 1; step <= 3; step++) {
            float alpha = step * .25f;
            pl_tex maps[3];
            float displacement[2];
            for (int direction = 0; direction < 3; direction++) {
                for (int i = 0; i < 480 * 270; i++) {
                    pixels[i * 4] = 0;
                    pixels[i * 4 + 1] = direction == 0 ? -alpha * shift :
                        direction == 1 ? (1 - alpha) * shift : 0;
                    pixels[i * 4 + 2] = direction < 2 ? .0625f : 0;
                    pixels[i * 4 + 3] = 1;
                }
                maps[direction] = upload_size(gpu, pixels, 480, 270);
                if (direction < 2) {
                    readback(gpu, maps[direction], pixels);
                    displacement[direction] = pixels[1] * ((float)height / 270);
                }
            }
            struct pl_shader_desc descriptors[] = {
                policy_sampled("first", a), policy_sampled("second", b),
                policy_sampled("forward", maps[0]), policy_sampled("backward", maps[1]),
                policy_sampled("scene", maps[2]),
            };
            float size[] = {width, height}, motion_size[] = {480, 270};
            struct pl_shader_var variables[] = {
                {.var = pl_var_vec2("motion_size"), .data = motion_size},
                {.var = pl_var_vec2("output_size"), .data = size},
                {.var = pl_var_float("alpha"), .data = &alpha},
                {.var = pl_var_int("linear_hdr"), .data = &(int){0}},
                {.var = pl_var_vec2("projection_ratio"),
                 .data = (float[]){(1.0f - alpha) / alpha, alpha / (1.0f - alpha)}},
            };
            pl_shader shader = pl_dispatch_begin(dispatch);
            assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
                .header = test_sample_header,
            }));
            assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
                .header = test_synthesis_pixel_header, .body = test_synthesis_body,
                .compute = true, .compute_group_size = {8, 8},
                .output = PL_SHADER_SIG_COLOR,
                .descriptors = descriptors, .num_descriptors = 5,
                .variables = variables, .num_variables = 5,
            }));
            assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
                .shader = &shader, .target = target,
            )));
            readback(gpu, target, output);
            float maximum = 0;
            int worst_x = 0, worst_y = 0;
            int border = test == 2 ? 0 : 64;
            for (int y = border; y < height - border; y++) {
                for (int x = border; x < width - border; x++) {
                    float wa[4], wb[4], conflict = 0;
                    for (int c = 0; c < 4; c++) {
                        wa[c] = reference_sample(first, width, height, x, y + displacement[0], c);
                        wb[c] = reference_sample(second, width, height, x, y + displacement[1], c);
                        if (c < 3) conflict = fmaxf(conflict, fabsf(wa[c] - wb[c]));
                    }
                    // The two offsets describe the same trajectory. At these
                    // fixtures' sampling precision the cross projections have
                    // the same color residual as the two warped endpoints.
                    int pixel = (y * width + x) * 4;
                    float stationary = 0;
                    for (int c = 0; c < 3; c++)
                        stationary = fmaxf(stationary,
                            fabsf(first[pixel + c] - second[pixel + c]));
                    float gain = stationary > conflict ? (stationary - conflict) / stationary : 0;
                    float consistency = 1 - reference_smooth(.03f, .15f, conflict);
                    float trust = consistency *
                        (fmaxf(.0625f, 1 - reference_smooth(.002f, .02f, conflict)) +
                         (1 - consistency) * .0625f * gain);
                    for (int c = 0; c < 4; c++) {
                        int index = (y * width + x) * 4 + c;
                        float original = first[index] * (1 - alpha) + second[index] * alpha;
                        float warped = wa[c] * (1 - alpha) + wb[c] * alpha;
                        float expected = original * (1 - trust) + warped * trust;
                        assert(isfinite(output[index]) && isfinite(expected));
                        float difference = fabsf(output[index] - expected);
                        if (difference > maximum) {
                            maximum = difference; worst_x = x; worst_y = y;
                        }
                    }
                }
            }
            printf("synthesis sampling case=%d alpha=%.2f max=%.9g at=%d,%d\n",
                   test, alpha, maximum, worst_x, worst_y);
            fflush(stdout);
            failed |= maximum >= .002f;
            for (int i = 0; i < 3; i++) pl_tex_destroy(gpu, &maps[i]);
        }
        pl_tex_destroy(gpu, &a); pl_tex_destroy(gpu, &b);
        pl_tex_destroy(gpu, &target);
    }
    free(pixels); free(first); free(second); free(output);
    pl_dispatch_destroy(&dispatch);
    assert(!failed);
}

// Preserve the pre-cache shader as an independent control, including its
// short-circuit cycle check and final selected-cost recomputation.
#define RECONSTRUCTION_GLSL(...) #__VA_ARGS__
static const char original_reconstruction_body[] = RECONSTRUCTION_GLSL(
    ivec2 grid = textureSize(forward, 0);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(pixel, grid))) {
        vec2 size = vec2(textureSize(first, 0));
        vec2 p = (vec2(pixel) + 0.5) * grid_scale - 0.5;
        for (int y = -3; y <= 3; y += 2) {
            for (int x = -3; x <= 3; x += 2)
                reference_colors[((y + 3) / 2) * 4 + (x + 3) / 2] =
                    sample_color(first, p + vec2(x, y));
        }
        // Select an observed vector; averaging aliases can invent invalid motion.
        vec4 selected = texelFetch(forward, pixel, 0);
        float limit = patch_cost(second, p, selected.xy) + 0.000001;
        float best = -1.0;
        float nearest = 1e20;
        for (int y = -2; y <= 2; y++) {
            for (int x = -2; x <= 2; x++) {
                ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), grid - 1);
                float evidence = unpack_evidence(texelFetch(evidence_map, q, 0));
                float distance = float(x * x + y * y);
                if (!(evidence > best || (evidence == best && distance < nearest)))
                    continue;
                vec2 position = (vec2(q) + 0.5) / vec2(grid);
                vec4 value = texelFetch(forward, q, 0);
                vec4 reverse = textureLod(backward,
                                          position + value.xy / size, 0.0);
                if (any(isnan(value)) || any(isinf(value)) ||
                    any(isnan(reverse)) || any(isinf(reverse)))
                    continue;
                float cycle = length(value.xy + reverse.xy);
                if (cycle >= 1.5 || patch_cost(second, p, value.xy) > limit)
                    continue;
                selected = value;
                best = evidence;
                nearest = distance;
            }
        }
        float cost = patch_cost(second, p, selected.xy);
        // Use the same local score tolerance as the observed-neighbor selection.
        vec4 hypothesis = texelFetch(translation, ivec2(0), 0);
        if (hypothesis.w > 0.0) {
            vec2 vector = direction != 0 ? -hypothesis.xy : hypothesis.xy;
            vector = clamp(vector, -p, size - 1.0 - p);
            float score = patch_cost(second, p, vector);
            if (score <= cost + 0.000001) {
                selected = vec4(vector, score, hypothesis.w);
                cost = score;
            }
        }
        color = vec4(selected.xy, cost, selected.w);
    }
);
#undef RECONSTRUCTION_GLSL

static void reconstruction_equivalence(pl_gpu gpu)
{
    enum { W = 9, H = 7 };
    float pixels[WIDTH * HEIGHT * 4];
    float vectors[W * H * 4], reverse[W * H * 4], evidence[W * H * 4];
    uint32_t output[2][W * H * 4];
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = W, .h = H, .format = format,
        .renderable = true, .storable = true, .host_readable = true,
    ));
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(target && dispatch);
    for (int test = 0; test < 8; test++) {
        pattern(pixels, 0);
        if (test < 4) {
            for (int i = 0; i < WIDTH * HEIGHT * 4; i++)
                pixels[i] = 0.5f;
        }
        pl_tex first = upload(gpu, pixels);
        if (test >= 4)
            pattern(pixels, test & 1 ? 3 : 0);
        pl_tex second = upload(gpu, pixels);
        for (int i = 0; i < W * H; i++) {
            vectors[4 * i] = (i % 3 - 1) * 1.25f;
            vectors[4 * i + 1] = 0;
            vectors[4 * i + 2] = 0.75f; // Not the cost at the receiving pixel.
            vectors[4 * i + 3] = (i % 5 + 1) / 8.0f;
            reverse[4 * i] = test == 0 ? 4.0f : test == 1 ? 1.5f : 0;
            reverse[4 * i + 1] = reverse[4 * i + 2] = 0;
            reverse[4 * i + 3] = 1;
            float score = test == 2 ? 0.5f : (i % 7 + 1) / 8.0f;
            uint32_t bits;
            memcpy(&bits, &score, sizeof(bits));
            for (int c = 0; c < 4; c++)
                evidence[4 * i + c] = (bits >> (8 * c)) & 255;
        }
        float hypothesis[] = {1.25f, 0, 0, test >= 6 ? 0.75f : 0};
        pl_tex forward = upload_size(gpu, vectors, W, H);
        pl_tex backward = upload_size(gpu, reverse, W, H);
        pl_tex observed = upload_size(gpu, evidence, W, H);
        pl_tex translation = upload_size(gpu, hypothesis, 1, 1);
        struct pl_shader_desc descriptors[] = {
            policy_sampled("first", first), policy_sampled("second", second),
            policy_sampled("forward", forward), policy_sampled("backward", backward),
            policy_sampled("evidence_map", observed),
            policy_sampled("translation", translation),
        };
        float scale[] = {(float) WIDTH / W, (float) HEIGHT / H};
        int direction = test & 1;
        struct pl_shader_var variables[] = {
            {.var = pl_var_vec2("grid_scale"), .data = scale},
            {.var = pl_var_int("direction"), .data = &direction},
        };
        for (int variant = 0; variant < 2; variant++) {
            pl_shader shader = pl_dispatch_begin(dispatch);
            assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
                .header = test_sample_header,
            }));
            assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
                .header = test_refine_header,
                .body = variant ? test_reconstruction_body : original_reconstruction_body,
                .compute = true, .compute_group_size = {8, 8},
                .output = PL_SHADER_SIG_COLOR,
                .descriptors = descriptors, .num_descriptors = 6,
                .variables = variables, .num_variables = 2,
            }));
            assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
                .shader = &shader, .target = target,
            )));
            assert(pl_tex_download(gpu, pl_tex_transfer_params(
                .tex = target, .ptr = output[variant],
            )));
        }
        assert(memcmp(output[0], output[1], sizeof(output[0])) == 0);
        for (int i = 0; i < W * H * 4; i++) {
            float value;
            memcpy(&value, &output[0][i], sizeof(value));
            assert(isfinite(value));
        }
        printf("reconstruction case=%d exact_components=%d\n", test, W * H * 4);
        pl_tex_destroy(gpu, &first);
        pl_tex_destroy(gpu, &second);
        pl_tex_destroy(gpu, &forward);
        pl_tex_destroy(gpu, &backward);
        pl_tex_destroy(gpu, &observed);
        pl_tex_destroy(gpu, &translation);
    }
    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &target);
}

static void dense_pair_equivalence(pl_gpu gpu, bool bidirectional)
{
    static const char pair_body[] =
        "vec2 p = vec2(gl_GlobalInvocationID.xy) * vec2(2.75, 2.5) + 0.375;"
        "vec2 cost = dense_pair_cost(first, second, p, vector);"
        "color = vec4(cost, dense_cost(first, second, p, vec2(0), 1e20),"
        "dense_cost(first, second, p, vector, 1e20));";
    static const char bidirectional_body[] =
        "vec2 p = vec2(gl_GlobalInvocationID.xy) * vec2(2.75, 2.5) + 0.375;"
        "vec2 v = vector; vec2 reverse = -v * vec2(0.5, 1.25);"
        "if ((int(gl_GlobalInvocationID.x) & 3) == 1) reverse = vec2(0);"
        "if ((int(gl_GlobalInvocationID.x) & 3) == 2) v = vec2(0);"
        "vec3 shared_cost = bidirectional_cost(first, second, p, v, reverse);"
        "vec2 a = dense_pair_cost(first, second, p, v);"
        "vec2 b = dense_pair_cost(second, first, p, reverse);"
        "vec2 enabled = vec2(any(notEqual(v, vec2(0))), any(notEqual(reverse, vec2(0))));"
        "vec2 gain = max(vec2(0), shared_cost.xx - shared_cost.yz) * enabled;"
        "color = vec4(gain, max(vec2(0), vec2(a.x - a.y, b.x - b.y)) * enabled);";
    static const float vectors[][2] = {
        {0, 0}, {3, 0}, {1.25f, -0.5f}, {-3.75f, 2.125f},
    };
    float *pixels = malloc(WIDTH * HEIGHT * 4 * sizeof(float));
    assert(pixels);
    pattern(pixels, 0);
    pl_tex first = upload(gpu, pixels);
    pattern(pixels, 3);
    pl_tex second = upload(gpu, pixels);
    free(pixels);
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = 32, .h = 24, .format = format,
        .renderable = true, .storable = true, .host_readable = true,
    ));
    assert(target);
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(dispatch);
    struct pl_shader_desc descriptors[] = {
        policy_sampled("first", first), policy_sampled("second", second),
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        struct pl_shader_var variable = {
            .var = pl_var_vec2("vector"), .data = vectors[i], .dynamic = true,
        };
        pl_shader shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_sample_header,
        }));
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_refine_header,
            .body = bidirectional ? bidirectional_body : pair_body,
            .compute = true, .compute_group_size = {8, 8},
            .output = PL_SHADER_SIG_COLOR,
            .descriptors = descriptors, .num_descriptors = 2,
            .variables = &variable, .num_variables = 1,
        }));
        assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
            .shader = &shader, .target = target,
        )));
        uint32_t actual[32 * 24 * 4];
        assert(pl_tex_download(gpu, pl_tex_transfer_params(
            .tex = target, .ptr = actual,
        )));
        for (int pixel = 0; pixel < 32 * 24; pixel++) {
            assert(actual[pixel * 4] == actual[pixel * 4 + 2]);
            assert(actual[pixel * 4 + 1] == actual[pixel * 4 + 3]);
        }
        printf("dense %s vector=%.3f,%.3f exact_costs=1536\n",
               bidirectional ? "bidirectional" : "pair",
               vectors[i][0], vectors[i][1]);
    }
    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &first);
    pl_tex_destroy(gpu, &second);
    pl_tex_destroy(gpu, &target);
}

static void refine_zero_motion(pl_gpu gpu)
{
    float pixels[WIDTH * HEIGHT * 4];
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            float *pixel = &pixels[(y * WIDTH + x) * 4];
            pixel[0] = pixel[1] = pixel[2] = 0.25f + x / 256.0f;
            pixel[3] = 1;
        }
    }
    pl_tex first = upload(gpu, pixels);
    pl_tex stationary = upload(gpu, pixels);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        for (int c = 0; c < 3; c++)
            pixels[i * 4 + c] -= 5.0f / 256.0f;
    }
    pl_tex moving = upload(gpu, pixels);
    const float parent_vectors[] = {
        5, 0, 0, 0.75f, 5, 0, 0, 0.75f,
        5, 0, 0, 0.75f, 5, 0, 0, 0.75f,
    };
    pl_tex parent = upload_size(gpu, parent_vectors, 2, 2);
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    enum { W = 16, H = 12 };
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = W, .h = H, .format = format,
        .renderable = true, .storable = true, .host_readable = true,
    ));
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(target && dispatch);

    for (int shift = 0; shift <= 5; shift += 5) {
        struct pl_shader_desc descriptors[] = {
            policy_sampled("first", first),
            policy_sampled("second", shift ? moving : stationary),
            policy_sampled("parent", parent),
        };
        int settings[] = {W, H, 2};
        float motion_scale[] = {1, 1};
        struct pl_shader_var variables[] = {
            {.var = pl_var_ivec3("settings"), .data = settings},
            {.var = pl_var_vec2("motion_scale"), .data = motion_scale},
        };
        pl_shader shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_sample_header,
        }));
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_refine_header, .body = test_refine_body,
            .compute = true, .compute_group_size = {8, 8},
            .output = PL_SHADER_SIG_COLOR,
            .descriptors = descriptors, .num_descriptors = 3,
            .variables = variables, .num_variables = 2,
        }));
        assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
            .shader = &shader, .target = target,
        )));
        float actual[W * H * 4];
        assert(pl_tex_download(gpu, pl_tex_transfer_params(
            .tex = target, .ptr = actual,
        )));
        // A limited refinement can retain a stale predictor on a stationary
        // ramp. An actually translated ramp must still keep its predictor.
        for (int y = 2; y < H - 2; y++) {
            for (int x = 2; x < W - 2; x++) {
                const float *v = &actual[(y * W + x) * 4];
                assert(v[0] == shift && v[1] == 0 && v[2] == 0);
                assert(v[3] == (shift ? 0.75f : 0.0f));
            }
        }
    }

    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &target);
    pl_tex_destroy(gpu, &parent);
    pl_tex_destroy(gpu, &moving);
    pl_tex_destroy(gpu, &stationary);
    pl_tex_destroy(gpu, &first);
}

static void evidence_bits(pl_gpu gpu)
{
    static const uint32_t values[] = {
        0x00000000, 0x80000000, 0x00800000, 0x80800000,
        0x358637bc, 0x358637bd, 0x358637be,
        0x3dcccccc, 0x3dcccccd, 0x3dccccce,
        0x3f7fffff, 0x3f800000, 0x3f800001,
        0x7f7fffff, 0xff7fffff,
    };
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR |
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE);
    assert(format);
    pl_tex encoded = pl_tex_create(gpu, pl_tex_params(
        .w = 1, .h = 1, .format = format,
        .sampleable = true, .renderable = true, .storable = true,
    ));
    format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex decoded = pl_tex_create(gpu, pl_tex_params(
        .w = 1, .h = 1, .format = format,
        .renderable = true, .storable = true, .host_readable = true,
    ));
    assert(encoded && decoded);
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    assert(dispatch);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        union { uint32_t bits; float value; } input = {.bits = values[i]};
        struct pl_shader_var variable = {
            .var = pl_var_float("value"), .data = &input.value, .dynamic = true,
        };
        pl_shader shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_sample_header,
        }));
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_refine_header,
            .body = "color = pack_evidence(value);",
            .compute = true, .compute_group_size = {1, 1},
            .output = PL_SHADER_SIG_COLOR,
            .variables = &variable, .num_variables = 1,
        }));
        assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
            .shader = &shader, .target = encoded,
        )));
        struct pl_shader_desc descriptor = policy_sampled("encoded", encoded);
        shader = pl_dispatch_begin(dispatch);
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_sample_header,
        }));
        assert(pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = test_refine_header,
            .body = "color = vec4(unpack_evidence("
                    "texelFetch(encoded, ivec2(0), 0)));",
            .compute = true, .compute_group_size = {1, 1},
            .output = PL_SHADER_SIG_COLOR,
            .descriptors = &descriptor, .num_descriptors = 1,
        }));
        assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
            .shader = &shader, .target = decoded,
        )));
        uint32_t actual[4];
        assert(pl_tex_download(gpu, pl_tex_transfer_params(
            .tex = decoded, .ptr = actual,
        )));
        for (int c = 0; c < 4; c++)
            assert(actual[c] == values[i]);
        printf("evidence bits input=%08x output=%08x\n", values[i], actual[0]);
    }
    pl_dispatch_destroy(&dispatch);
    pl_tex_destroy(gpu, &encoded);
    pl_tex_destroy(gpu, &decoded);
}

static double pq_nits(double code)
{
    double p = pow(fmax(0, fmin(1, code)), 32.0 / 2523.0);
    return 10000 * pow(fmax(0, p - 3424.0 / 4096.0) /
                           (2413.0 / 128.0 - 2392.0 / 128.0 * p),
                       16384.0 / 2610.0);
}

static double pq_code(double nits)
{
    double p = pow(fmax(0, nits) / 10000, 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
                   (1 + 2392.0 / 128.0 * p),
               2523.0 / 32.0);
}

static struct pl_frame pq_frame(pl_tex texture)
{
    struct pl_frame frame = {
        .num_planes = 1,
        .planes[0] = {.texture = texture,
                      .components = 4,
                      .component_mapping = {0, 1, 2, 3}},
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_hdr10,
    };
    frame.color.hdr.min_luma = 0.005f;
    frame.color.hdr.max_luma = 1000;
    return frame;
}

static void fill_nits(float *pixels, int width, int height, double nits)
{
    for (int i = 0; i < width * height; i++) {
        for (int c = 0; c < 3; c++)
            pixels[i * 4 + c] = (float)pq_code(nits);
        pixels[i * 4 + 3] = 1;
    }
}

static void capture_contract(pl_gpu gpu, struct mp_motion *motion)
{
    const double values[] = {0, 0.001, 0.1, 1, 100, 203, 1000, 4000, 10000};
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *codes = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *previous = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && codes && output && previous);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            for (int c = 0; c < 3; c++)
                pixels[(y * WIDTH + x) * 4 + c] =
                    (float)pq_code(values[x * 9 / WIDTH]);
            pixels[(y * WIDTH + x) * 4 + 3] = 1;
        }
    }
    pl_tex texture = upload(gpu, pixels);
    readback(gpu, texture, codes);
    struct pl_frame source = pq_frame(texture);
    struct mp_motion_frame frame = {0};
    uint64_t identity = 0;
    for (int metadata = 0; metadata < 3; metadata++) {
        source.color.hdr.max_luma = metadata == 0   ? 1000
                                    : metadata == 1 ? 4000
                                                    : 10000;
        source.color.hdr.max_cll = source.color.hdr.max_luma;
        source.color.hdr.max_fall = 300;
        struct pl_hdr_metadata original = source.color.hdr;
        pl_tex allocation = frame.texture;
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frame) == MP_MOTION_OK);
        assert(frame.owner == motion && frame.id > identity);
        assert(!allocation || frame.texture == allocation);
        identity = frame.id;
        assert(frame.domain == MP_MOTION_HDR_LINEAR);
        assert(frame.color.primaries == PL_COLOR_PRIM_BT_2020);
        assert(frame.color.transfer == PL_COLOR_TRC_LINEAR_HDR);
        assert(frame.color.hdr.max_luma == source.color.hdr.max_luma);
        assert(frame.repr.alpha == PL_ALPHA_PREMULTIPLIED);
        assert(frame.color.hdr.max_cll == source.color.hdr.max_cll &&
               frame.color.hdr.max_fall == 300);
        assert(!memcmp(&original, &source.color.hdr, sizeof(original)));
        readback(gpu, frame.texture, output);
        if (metadata)
            assert(
                !memcmp(output, previous, WIDTH * HEIGHT * 4 * sizeof(float)));
        for (int patch = 0; patch < 9; patch++) {
            int offset =
                (HEIGHT / 2 * WIDTH + (2 * patch + 1) * WIDTH / 18) * 4;
            double expected = pq_nits(codes[offset]);
            double observed = output[offset] * PL_COLOR_SDR_WHITE;
            double tolerance = fmax(0.0005, expected * 0.0015);
            printf("PQ capture metadata=%d patch=%d expected=%.9f actual=%.9f "
                   "tolerance=%.9f\n",
                   metadata, patch, expected, observed, tolerance);
            fflush(stdout);
            assert(fabs(observed - expected) < tolerance);
        }
        memcpy(previous, output, WIDTH * HEIGHT * 4 * sizeof(float));
    }
    struct pl_hdr_metadata static_metadata = source.color.hdr;
    for (int dynamic = 0; dynamic < 8; dynamic++) {
        source.color.hdr = static_metadata;
        switch (dynamic) {
        case 0:
            source.color.hdr.scene_avg = 100;
            break;
        case 1:
            source.color.hdr.scene_max[0] = 1000;
            break;
        case 2:
            source.color.hdr.scene_max[1] = 1000;
            break;
        case 3:
            source.color.hdr.scene_max[2] = 1000;
            break;
        case 4:
            source.color.hdr.max_pq_y = 0.8f;
            break;
        case 5:
            source.color.hdr.avg_pq_y = 0.4f;
            break;
        case 6:
            source.color.hdr.ootf.target_luma = 400;
            break;
        case 7:
            source.color.hdr.ootf.num_anchors = 1;
            break;
        }
        struct pl_hdr_metadata original = source.color.hdr;
        assert(!mp_motion_capture_supported(motion, &source));
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frame) == MP_MOTION_UNSUPPORTED);
        assert(!frame.id && !frame.owner && frame.texture);
        assert(pl_hdr_metadata_equal(&source.color.hdr, &original));
    }
    source.color.hdr = static_metadata;
    source.color.transfer = PL_COLOR_TRC_HLG;
    assert(!mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame));
    assert(!frame.id && !frame.owner && frame.texture);
    struct pl_frame result = {.num_planes = 99};
    assert(mp_motion_render_frames(motion, &frame, &frame, 0.5f, &result) ==
           MP_MOTION_UNSUPPORTED);
    assert(!result.num_planes);
    source.color.transfer = PL_COLOR_TRC_PQ;
    source.field = PL_FIELD_TOP;
    assert(!mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame));
    assert(!frame.id && !frame.owner);
    source.field = PL_FIELD_NONE;
    assert(!mp_motion_capture_frame(motion, &source, 7, HEIGHT, &frame));
    assert(!frame.id && !frame.owner);
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_OK);
    assert(frame.id == identity + 1);
    struct mp_motion *foreign = mp_motion_create(gpu, 128);
    assert(foreign);
    assert(mp_motion_render_frames(foreign, &frame, &frame, 0.5f, &result) ==
           MP_MOTION_UNSUPPORTED);
    assert(!result.num_planes);
    mp_motion_destroy(&foreign);
    mp_motion_frame_destroy(&frame);
    assert(!frame.texture && !frame.id && !frame.owner);
    pl_tex_destroy(gpu, &texture);
    free(pixels);
    free(codes);
    free(output);
    free(previous);
    puts("PQ capture identity, metadata independence, failure invalidation and "
         "ownership: passed");
}

static void synthetic_contract(const struct pl_frame *frame)
{
    assert(frame->num_planes == 1 && frame->planes[0].texture);
    assert(frame->planes[0].texture->params.format->component_depth[0] == 32);
    assert(frame->color.transfer == PL_COLOR_TRC_LINEAR_HDR);
    struct pl_hdr_metadata dynamic = frame->color.hdr;
    dynamic.prim = (struct pl_raw_primaries){0};
    dynamic.min_luma = dynamic.max_luma = dynamic.max_cll = 0;
    assert(pl_hdr_metadata_equal(&dynamic, &pl_hdr_metadata_empty));
    assert(frame->repr.sys == PL_COLOR_SYSTEM_RGB && !frame->repr.dovi);
    assert(frame->repr.alpha == PL_ALPHA_PREMULTIPLIED);
    assert(!frame->enhancement_layer && frame->field == PL_FIELD_NONE);
    assert(!frame->prev && !frame->next && !frame->acquire && !frame->release);
    assert(!frame->icc && !frame->profile.len && !frame->lut &&
           !frame->num_overlays);
}

static void linear_blend(pl_gpu gpu, struct mp_motion *motion, double first,
                         double second)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output);
    struct mp_motion_frame frames[2] = {0};
    pl_tex sources[2];
    double captured[2];
    for (int i = 0; i < 2; i++) {
        fill_nits(pixels, WIDTH, HEIGHT, i ? second : first);
        sources[i] = upload(gpu, pixels);
        struct pl_frame source = pq_frame(sources[i]);
        source.color.hdr.max_luma = 1000;
        source.color.hdr.max_fall = i ? 200 : 100;
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[i]) == MP_MOTION_OK);
        readback(gpu, frames[i].texture, output);
        captured[i] = output[0] * PL_COLOR_SDR_WHITE;
    }
    for (int phase = 1; phase < 4; phase++) {
        float alpha = phase / 4.0f;
        double expected = (1 - alpha) * captured[0] + alpha * captured[1];
        struct pl_frame result;
        assert(mp_motion_render_frames(motion, &frames[0], &frames[1], alpha,
                                       &result) == MP_MOTION_OK);
        synthetic_contract(&result);
        readback(gpu, result.planes[0].texture, output);
        double observed =
            (output[(HEIGHT / 2 * WIDTH + WIDTH / 2) * 4]) * PL_COLOR_SDR_WHITE;
        double tolerance = fmax(0.0005, expected * 0.0015);
        printf("PQ blend first=%.3f second=%.3f alpha=%.2f expected=%.9f "
               "actual=%.9f tolerance=%.9f\n",
               first, second, alpha, expected, observed, tolerance);
        fflush(stdout);
        assert(fabs(observed - expected) < tolerance);
    }
    struct pl_frame result;
    uint64_t analyzed = mp_motion_get_stats(motion).analyzed_pairs;
    pl_tex legacy;
    assert(mp_motion_render(motion, sources[0], sources[1], frames[0].id,
                            frames[1].id, 0.5f, &legacy));
    assert(mp_motion_get_stats(motion).analyzed_pairs == analyzed + 1);
    readback(gpu, legacy, output);
    double code_blend = pq_nits(output[0]);
    printf("Legacy encoded blend first=%.3f second=%.3f actual_nits=%.9f\n",
           first, second, code_blend);
    analyzed++;
    assert(mp_motion_render_frames(motion, &frames[0], &frames[1], 0.5f,
                                   &result) == MP_MOTION_OK);
    assert(mp_motion_get_stats(motion).analyzed_pairs == analyzed + 1);
    pl_tex allocation = frames[0].texture;
    uint64_t old_id = frames[0].id;
    fill_nits(pixels, WIDTH, HEIGHT, (first + second) / 2);
    pl_tex replacement = upload(gpu, pixels);
    struct pl_frame changed = pq_frame(replacement);
    assert(mp_motion_capture_frame(motion, &changed, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_OK);
    assert(frames[0].texture == allocation && frames[0].id != old_id);
    readback(gpu, frames[0].texture, output);
    double expected = (output[0] * PL_COLOR_SDR_WHITE + captured[1]) / 2;
    analyzed = mp_motion_get_stats(motion).analyzed_pairs;
    assert(mp_motion_render_frames(motion, &frames[0], &frames[1], 0.5f,
                                   &result) == MP_MOTION_OK);
    assert(mp_motion_get_stats(motion).analyzed_pairs == analyzed + 1);
    synthetic_contract(&result);
    readback(gpu, result.planes[0].texture, output);
    assert(fabs(output[0] * PL_COLOR_SDR_WHITE - expected) <
           fmax(0.0005, expected * 0.0015));
    pl_tex_destroy(gpu, &replacement);
    for (int i = 0; i < 2; i++) {
        mp_motion_frame_destroy(&frames[i]);
        pl_tex_destroy(gpu, &sources[i]);
    }
    free(pixels);
    free(output);
}

static void translated_hdr(pl_gpu gpu, struct mp_motion *motion, int shift)
{
    const int width = 256, height = 192;
    float *pixels = calloc(width * height * 4, sizeof(float));
    float *reference = calloc(width * height * 4, sizeof(float));
    float *output = calloc(width * height * 4, sizeof(float));
    assert(pixels && reference && output);
    struct mp_motion_frame frames[2] = {0};
    for (int direction = 0; direction < 2; direction++) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                for (int c = 0; c < 3; c++)
                    pixels[(y * width + x) * 4 + c] =
                        0.15f + 0.7f * noise(x - direction * shift, y, c);
                pixels[(y * width + x) * 4 + 3] = 1;
            }
        }
        pl_tex source = upload_size(gpu, pixels, width, height);
        struct pl_frame input = pq_frame(source);
        assert(mp_motion_capture_frame(motion, &input, width, height,
                                       &frames[direction]) == MP_MOTION_OK);
        pl_tex_destroy(gpu, &source);
    }
    readback(gpu, frames[0].texture, reference);
    for (int step = 1; step <= 7; step++) {
        float alpha = step / 8.0f;
        struct pl_frame result;
        assert(mp_motion_render_frames(motion, &frames[0], &frames[1], alpha,
                                       &result) == MP_MOTION_OK);
        synthetic_contract(&result);
        readback(gpu, result.planes[0].texture, output);
        double sum = 0, maximum = 0;
        int count = 0;
        for (int y = 24; y < height - 24; y++) {
            for (int x = 24; x < width - 24; x++) {
                for (int c = 0; c < 3; c++) {
                    double position = x - alpha * shift;
                    int lo = (int)floor(position);
                    double fraction = position - lo;
                    double expected =
                        ((1 - fraction) * reference[(y * width + lo) * 4 + c] +
                         fraction * reference[(y * width + lo + 1) * 4 + c]) *
                        PL_COLOR_SDR_WHITE;
                    double actual =
                        output[(y * width + x) * 4 + c] * PL_COLOR_SDR_WHITE;
                    double error = fabs(pq_code(expected) - pq_code(actual));
                    sum += error;
                    maximum = fmax(maximum, error);
                    count++;
                }
            }
        }
        printf("PQ translation width=%d shift=%d alpha=%.3f mean=%.9f "
               "maximum=%.9f\n",
               width, shift, alpha, sum / count, maximum);
        fflush(stdout);
        assert(sum / count < 0.02 && maximum < 0.1);
    }
    for (int i = 0; i < 2; i++)
        mp_motion_frame_destroy(&frames[i]);
    free(pixels);
    free(reference);
    free(output);
}

static void p010_capture(pl_gpu gpu, struct mp_motion *motion)
{
    const int codes[] = {64, 65, 128, 256, 512, 768, 940};
    uint16_t *luma = calloc(WIDTH * HEIGHT, sizeof(uint16_t));
    uint16_t *chroma = calloc(WIDTH * HEIGHT / 2, sizeof(uint16_t));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(luma && chroma && output);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++)
            luma[y * WIDTH + x] = codes[x * 7 / WIDTH] << 6;
    }
    for (int i = 0; i < WIDTH * HEIGHT / 2; i++)
        chroma[i] = 512 << 6;
    pl_fmt formats[] = {
        pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, PL_FMT_CAP_SAMPLEABLE),
        pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 16, PL_FMT_CAP_SAMPLEABLE),
    };
    assert(formats[0] && formats[1]);
    pl_tex planes[] = {
        pl_tex_create(
            gpu, pl_tex_params(.w = WIDTH, .h = HEIGHT, .format = formats[0],
                               .sampleable = true, .host_writable = true,
                               .initial_data = luma)),
        pl_tex_create(
            gpu, pl_tex_params(.w = WIDTH / 2, .h = HEIGHT / 2,
                               .format = formats[1], .sampleable = true,
                               .host_writable = true, .initial_data = chroma)),
    };
    assert(planes[0] && planes[1]);
    struct pl_frame source = {
        .num_planes = 2,
        .planes =
            {
                {.texture = planes[0],
                 .components = 1,
                 .component_mapping = {0}},
                {.texture = planes[1],
                 .components = 2,
                 .component_mapping = {1, 2}},
            },
        .repr =
            {
                .sys = PL_COLOR_SYSTEM_BT_2020_NC,
                .levels = PL_COLOR_LEVELS_LIMITED,
                .bits = {.sample_depth = 16, .color_depth = 10, .bit_shift = 6},
            },
        .color = pl_color_space_hdr10,
    };
    struct mp_motion_frame frame = {0};
    assert(mp_motion_capture_supported(motion, &source));
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_OK);
    readback(gpu, frame.texture, output);
    for (int patch = 0; patch < 7; patch++) {
        int offset = (HEIGHT / 2 * WIDTH + (2 * patch + 1) * WIDTH / 14) * 4;
        double expected = pq_nits((codes[patch] - 64) / 876.0);
        double tolerance = fmax(0.0005, expected * 0.0015);
        for (int c = 0; c < 3; c++) {
            double actual = output[offset + c] * PL_COLOR_SDR_WHITE;
            printf("P010 code=%d channel=%d expected=%.9f actual=%.9f "
                   "tolerance=%.9f\n",
                   codes[patch], c, expected, actual, tolerance);
            fflush(stdout);
            assert(fabs(actual - expected) < tolerance);
        }
    }
    mp_motion_frame_destroy(&frame);
    for (int i = 0; i < 2; i++)
        pl_tex_destroy(gpu, &planes[i]);
    free(luma);
    free(chroma);
    free(output);
}

static void source_roundtrip(pl_gpu gpu, struct mp_motion *motion, bool p3)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *reference = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && reference && output);
    pattern(pixels, 0);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        for (int c = 0; c < 3; c++)
            pixels[i * 4 + c] = 0.05f + 0.9f * pixels[i * 4 + c];
    }
    pl_tex texture = upload(gpu, pixels);
    pl_fmt format =
        pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                    PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(
        gpu, pl_tex_params(.w = WIDTH, .h = HEIGHT, .format = format,
                           .renderable = true, .host_readable = true));
    assert(target);
    struct pl_frame source = pq_frame(texture);
    if (p3)
        source.color.primaries = PL_COLOR_PRIM_DISPLAY_P3;
    for (int metadata = 0; metadata < 3; metadata++) {
        source.color.hdr.max_luma = metadata == 0   ? 1000
                                    : metadata == 1 ? 4000
                                                    : 10000;
        source.color.hdr.max_cll = source.color.hdr.max_luma;
        struct mp_motion_frame frame = {0};
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frame) == MP_MOTION_OK);
        struct pl_frame captured = source;
        captured.planes[0].texture = frame.texture;
        captured.color = frame.color;
        captured.repr = frame.repr;
        for (int mode = 0; mode < 3; mode++) {
            struct pl_frame destination = pq_frame(target);
            destination.color =
                mode == 0 ? pl_color_space_bt709 : pl_color_space_hdr10;
            destination.color.hdr.max_luma = mode == 0   ? 203
                                             : mode == 1 ? 1000
                                                         : 4000;
            destination.color.hdr.min_luma = mode == 0 ? 0.203f : 0.005f;
            struct pl_render_params params = pl_render_default_params;
            params.peak_detect_params = NULL;
            params.dither_params = NULL;
            pl_renderer first = pl_renderer_create(gpu->log, gpu);
            pl_renderer second = pl_renderer_create(gpu->log, gpu);
            assert(pl_render_image(first, &source, &destination, &params));
            assert(pl_tex_download(
                gpu, pl_tex_transfer_params(.tex = target, .ptr = reference)));
            assert(pl_render_image(second, &captured, &destination, &params));
            assert(pl_tex_download(
                gpu, pl_tex_transfer_params(.tex = target, .ptr = output)));
            float maximum = 0;
            for (int i = 0; i < WIDTH * HEIGHT * 4; i++)
                maximum = fmaxf(maximum, fabsf(reference[i] - output[i]));
            printf("PQ source roundtrip p3=%d metadata=%d target=%d "
                   "maximum=%.9f\n",
                   p3, metadata, mode, maximum);
            fflush(stdout);
            assert(maximum < 0.003);
            pl_renderer_destroy(&first);
            pl_renderer_destroy(&second);
        }
        mp_motion_frame_destroy(&frame);
    }
    pl_tex_destroy(gpu, &texture);
    pl_tex_destroy(gpu, &target);
    free(pixels);
    free(reference);
    free(output);
}

struct acquired_record_source {
    pl_tex texture;
    struct pl_color_space color;
    enum pl_color_system system;
    enum pl_field field;
    bool ready;
    int acquired, released;
};

static bool acquire_record(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu;
    struct acquired_record_source *source = frame->user_data;
    source->acquired++;
    frame->planes[0].texture = source->texture;
    frame->color = source->color;
    frame->repr.sys = source->system;
    frame->field = source->field;
    return source->ready;
}

static void release_record(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu;
    struct acquired_record_source *source = frame->user_data;
    assert(frame->planes[0].texture == source->texture);
    assert(pl_color_space_equal(&frame->color, &source->color));
    assert(frame->repr.sys == source->system && frame->field == source->field);
    source->released++;
}

static void acquired_contract(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output);
    fill_nits(pixels, WIDTH, HEIGHT, 1000);
    struct acquired_record_source owner = {
        .texture = upload(gpu, pixels),
        .ready = true,
        .color = pl_color_space_bt2020_hlg,
        .system = PL_COLOR_SYSTEM_RGB,
    };
    struct pl_frame source = pq_frame(NULL);
    source.acquire = acquire_record;
    source.release = release_record;
    source.user_data = &owner;
    struct mp_motion_frame frame = {0};
    assert(mp_motion_capture_supported(motion, &source));
    assert(!owner.acquired && !owner.released);
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_UNSUPPORTED);
    assert(owner.acquired == 1 && owner.released == 1 && !frame.id);
    source.color.transfer = PL_COLOR_TRC_HLG;
    assert(!mp_motion_capture_supported(motion, &source));
    owner.color = pl_color_space_hdr10;
    owner.color.hdr.max_luma = 1000;
    owner.color.hdr.max_cll = 5000;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_OK);
    assert(owner.acquired == 2 && owner.released == 2);
    assert(frame.domain == MP_MOTION_HDR_LINEAR &&
           frame.color.hdr.max_cll == 5000);
    readback(gpu, owner.texture, pixels);
    readback(gpu, frame.texture, output);
    assert(fabs(output[0] * PL_COLOR_SDR_WHITE - pq_nits(pixels[0])) < 1.5);
    owner.ready = false;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_ERROR);
    assert(owner.acquired == 3 && owner.released == 3 && !frame.id &&
           !frame.owner);
    assert(frame.texture && frame.gpu == gpu);
    owner.ready = true;
    owner.field = PL_FIELD_TOP;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_UNSUPPORTED);
    assert(owner.acquired == 4 && owner.released == 4 && !frame.id);
    owner.field = PL_FIELD_NONE;
    owner.system = PL_COLOR_SYSTEM_DOLBYVISION;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_UNSUPPORTED);
    assert(owner.acquired == 5 && owner.released == 5 && !frame.id);
    owner.system = PL_COLOR_SYSTEM_RGB;
    owner.color.hdr.scene_avg = 100;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_UNSUPPORTED);
    assert(owner.acquired == 6 && owner.released == 6 && !frame.id);
    mp_motion_frame_destroy(&frame);
    pl_tex_destroy(gpu, &owner.texture);
    free(pixels);
    free(output);
    puts("Working capture: classification and metadata follow acquire; release "
         "runs once including failure");
}

static void alpha_blend(pl_gpu gpu, struct mp_motion *motion, bool transparent)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output);
    struct mp_motion_frame frames[2] = {0};
    double premultiplied[2], coverage[2];
    for (int i = 0; i < 2; i++) {
        fill_nits(pixels, WIDTH, HEIGHT, i ? 400 : 1000);
        for (int n = 0; n < WIDTH * HEIGHT; n++)
            pixels[n * 4 + 3] = transparent ? 0 : i ? 0.75f : 0.25f;
        pl_tex texture = upload(gpu, pixels);
        struct pl_frame source = pq_frame(texture);
        source.repr.alpha = PL_ALPHA_INDEPENDENT;
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[i]) == MP_MOTION_OK);
        readback(gpu, frames[i].texture, output);
        premultiplied[i] = output[0] * PL_COLOR_SDR_WHITE;
        coverage[i] = output[3];
        assert(fabs(coverage[i] - (transparent ? 0
                                   : i         ? 0.75
                                               : 0.25)) < 0.000001);
        pl_tex_destroy(gpu, &texture);
    }
    for (int step = 1; step < 4; step++) {
        float alpha = step / 4.0f;
        double opacity = (1 - alpha) * coverage[0] + alpha * coverage[1];
        double nits = (1 - alpha) * premultiplied[0] + alpha * premultiplied[1];
        double expected = opacity > 0 ? nits / opacity : 0;
        struct pl_frame result;
        assert(mp_motion_render_frames(motion, &frames[0], &frames[1], alpha,
                                       &result) == MP_MOTION_OK);
        synthetic_contract(&result);
        readback(gpu, result.planes[0].texture, output);
        double actual =
            opacity > 0 ? output[0] * PL_COLOR_SDR_WHITE / opacity : 0;
        printf("HDR alpha transparent=%d phase=%.2f opacity=%.6f actual=%.9f "
               "expected=%.9f\n",
               transparent, alpha, output[3], actual, expected);
        fflush(stdout);
        assert(isfinite(output[0]) && fabs(output[3] - opacity) < 0.000001);
        assert(fabs(actual - expected) < fmax(0.0005, expected * 0.0015));
    }
    for (int i = 0; i < 2; i++)
        mp_motion_frame_destroy(&frames[i]);
    free(pixels);
    free(output);
}

static void low_peak_info(void *opaque, const struct pl_render_info *info)
{
    int *features = opaque;
    assert(info->pass && info->pass->shader);
    const char *description = info->pass->shader->description;
    if (description && strstr(description, "feature extraction"))
        (*features)++;
}

struct hdr_reference_info {
    int passes, peaks, features;
};

static void hdr_reference_info(void *priv, const struct pl_render_info *info)
{
    struct hdr_reference_info *counts = priv;
    assert(info->pass && info->pass->shader);
    const char *desc = info->pass->shader->description;
    assert(desc);
    counts->passes++;
    counts->peaks += !!strstr(desc, "peak detection");
    counts->features += !!strstr(desc, "feature extraction");
}

static void hdr_reference(pl_gpu gpu, const struct pl_frame *source,
                          const struct pl_frame *target,
                          const struct pl_render_params *params,
                          const struct pl_hdr_metadata *peak,
                          const float *original, const float *synthetic)
{
    struct pl_frame input = *source;
    input.color.hdr.max_pq_y = peak->max_pq_y;
    input.color.hdr.avg_pq_y = peak->avg_pq_y;
    input.color.hdr.scene_avg = peak->scene_avg;
    memcpy(input.color.hdr.scene_max, peak->scene_max,
           sizeof(input.color.hdr.scene_max));
    struct pl_render_params opts = *params;
    opts.peak_detect_params = NULL;
    struct hdr_reference_info counts = {0};
    opts.info_callback = hdr_reference_info;
    opts.info_priv = &counts;
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    pl_renderer rr = pl_renderer_create(gpu->log, gpu);
    assert(pixels && rr);
    assert(pl_render_image(rr, &input, target, &opts));
    assert(pl_renderer_get_errors(rr).errors == 0);
    assert(pl_tex_download(gpu, pl_tex_transfer_params(
        .tex = target->planes[0].texture, .ptr = pixels)));
    float maxima[2] = {0};
    for (int n = 0; n < WIDTH * HEIGHT * 4; n++) {
        assert(isfinite(pixels[n]) && isfinite(original[n]) &&
               isfinite(synthetic[n]));
        if (n % 4 == 3) {
            assert(pixels[n] == 1 && original[n] == 1 && synthetic[n] == 1);
        } else {
            maxima[0] = fmaxf(maxima[0], fabsf(original[n] - pixels[n]));
            maxima[1] = fmaxf(maxima[1], fabsf(synthetic[n] - pixels[n]));
        }
    }
    printf("HDR independent reference maxima=%a,%a passes=%d peaks=%d "
           "features=%d gate=%a\n",
           maxima[0], maxima[1], counts.passes, counts.peaks, counts.features,
           0.003);
    fflush(stdout);
    assert(counts.passes == 1 && counts.peaks == 0 && counts.features == 0);
    assert(maxima[0] < 0.003 && maxima[1] < 0.003);
    pl_renderer_destroy(&rr);
    free(pixels);
}

static void low_peak_contrast(pl_gpu gpu, struct mp_motion *motion, bool contrast,
                              bool uniform)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *reference = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && reference && output);
    pattern(pixels, 0);
    for (int n = 0; n < WIDTH * HEIGHT; n++) {
        for (int c = 0; c < 3; c++)
            pixels[n * 4 + c] =
                (float)pq_code(uniform ? 75 :
                              100 * (0.05 + 0.9 * pixels[n * 4 + c]));
        pixels[n * 4 + 3] = 0.5f;
    }
    pl_tex texture = upload(gpu, pixels);
    struct pl_frame source = pq_frame(texture);
    source.repr.alpha = PL_ALPHA_INDEPENDENT;
    source.color.hdr.max_luma = 100;
    source.color.hdr.max_cll = 100;
    source.color.hdr.max_fall = 30;
    struct mp_motion_frame frame = {0};
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_OK);
    struct pl_frame result;
    assert(mp_motion_render_frames(motion, &frame, &frame, 0.25, &result) ==
           MP_MOTION_OK);
    synthetic_contract(&result);
    assert(result.planes[0].texture == frame.texture);
    assert(result.color.transfer == PL_COLOR_TRC_LINEAR_HDR);
    assert(result.repr.alpha == PL_ALPHA_PREMULTIPLIED);
    assert(result.color.hdr.max_luma == 100);

    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                               PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(
        gpu, pl_tex_params(.w = WIDTH, .h = HEIGHT, .format = format,
                           .renderable = true, .host_readable = true));
    assert(target);
    struct pl_frame destination = pq_frame(target);
    destination.color = pl_color_space_bt709;
    destination.color.hdr.max_luma = 50;
    destination.color.hdr.min_luma = 0.05f;
    struct pl_color_map_params mapping = pl_color_map_default_params;
    mapping.contrast_recovery = contrast ? 1 : 0;
    mapping.contrast_smoothness = 3;
    mapping.metadata = PL_HDR_METADATA_ANY;
    struct pl_render_params params = pl_render_default_params;
    params.color_map_params = &mapping;
    params.peak_detect_params = &pl_peak_detect_default_params;
    params.dither_params = NULL;
    params.info_callback = low_peak_info;
    int features[2] = {0};
    pl_renderer original = pl_renderer_create(gpu->log, gpu);
    pl_renderer synthesized = pl_renderer_create(gpu->log, gpu);
    assert(original && synthesized);
    params.info_priv = &features[0];
    assert(pl_render_image(original, &source, &destination, &params));
    assert(pl_renderer_get_errors(original).errors == 0);
    assert(pl_tex_download(
        gpu, pl_tex_transfer_params(.tex = target, .ptr = reference)));
    params.info_priv = &features[1];
    assert(pl_render_image(synthesized, &result, &destination, &params));
    assert(pl_renderer_get_errors(synthesized).errors == 0);
    assert(pl_tex_download(
        gpu, pl_tex_transfer_params(.tex = target, .ptr = output)));
    float maximum = 0;
    for (int n = 0; n < WIDTH * HEIGHT * 4; n++) {
        assert(isfinite(reference[n]) && isfinite(output[n]));
        if (n % 4 == 3)
            assert(reference[n] == 1 && output[n] == 1);
        maximum = fmaxf(maximum, fabsf(reference[n] - output[n]));
    }
    printf("HDR low peak contrast=%d features=%d,%d maximum=%.9f\n",
           contrast, features[0], features[1], maximum);
    fflush(stdout);
    assert(features[0] == contrast && features[1] == contrast);
    assert(maximum < 0.003);
    if (uniform) {
        struct pl_hdr_metadata peak = {0};
        assert(pl_renderer_get_hdr_metadata(original, &peak));
        mapping.contrast_recovery = 0;
        hdr_reference(gpu, &source, &destination, &params, &peak,
                      reference, output);
    }
    pl_renderer_destroy(&original);
    pl_renderer_destroy(&synthesized);
    mp_motion_frame_destroy(&frame);
    pl_tex_destroy(gpu, &texture);
    pl_tex_destroy(gpu, &target);
    free(pixels);
    free(reference);
    free(output);
}

static void synthetic_roundtrip(pl_gpu gpu, struct mp_motion *motion, bool p3,
                                int mastering, bool peak_detect)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *reference = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && reference && output);
    pattern(pixels, 0);
    for (int n = 0; n < WIDTH * HEIGHT; n++) {
        for (int c = 0; c < 3; c++)
            pixels[n * 4 + c] =
                mastering > 0 && mastering <= 204
                    ? (float)pq_code(mastering *
                                     (0.05 + 0.9 * pixels[n * 4 + c]))
                    : 0.05f + 0.9f * pixels[n * 4 + c];
        pixels[n * 4 + 3] = 0.5f;
    }
    pl_tex texture = upload(gpu, pixels);
    pl_fmt format =
        pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                    PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(
        gpu, pl_tex_params(.w = WIDTH, .h = HEIGHT, .format = format,
                           .renderable = true, .host_readable = true));
    assert(target);
    struct pl_frame source = pq_frame(texture);
    source.repr.alpha = PL_ALPHA_INDEPENDENT;
    source.color.hdr.max_luma = (float)mastering;
    source.color.hdr.max_cll = mastering ? (float)mastering : 1000;
    source.color.hdr.max_fall = 300;
    if (p3)
        source.color.primaries = PL_COLOR_PRIM_DISPLAY_P3;
    struct mp_motion_frame frame = {0};
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_OK);
    readback(gpu, frame.texture, pixels);
    for (int step = 1; step < 4; step++) {
        struct pl_frame result;
        assert(mp_motion_render_frames(motion, &frame, &frame, step / 4.0f,
                                       &result) == MP_MOTION_OK);
        synthetic_contract(&result);
        assert(result.planes[0].texture == frame.texture);
        assert(source.color.primaries == result.color.primaries &&
               result.color.transfer == PL_COLOR_TRC_LINEAR_HDR);
        assert(result.color.hdr.max_luma == frame.color.hdr.max_luma);
        assert(result.color.hdr.max_cll == source.color.hdr.max_cll);
        assert(pl_color_repr_equal(&frame.repr, &result.repr));
        readback(gpu, result.planes[0].texture, output);
        float raw_error = 0;
        for (int n = 0; n < WIDTH * HEIGHT * 4; n++)
            raw_error = fmaxf(raw_error, fabsf(output[n] - pixels[n]));
        printf("HDR linear samples raw_error=%.9f\n", raw_error);
        struct pl_frame destination = pq_frame(target);
        destination.color = pl_color_space_bt709;
        destination.color.hdr.max_luma = 203;
        destination.color.hdr.min_luma = 0.203f;
        struct pl_render_params params = pl_render_default_params;
        params.peak_detect_params =
            peak_detect ? &pl_peak_detect_default_params : NULL;
        params.dither_params = NULL;
        pl_renderer original = pl_renderer_create(gpu->log, gpu);
        pl_renderer synthesized = pl_renderer_create(gpu->log, gpu);
        assert(pl_render_image(original, &source, &destination, &params));
        assert(pl_tex_download(
            gpu, pl_tex_transfer_params(.tex = target, .ptr = reference)));
        assert(pl_render_image(synthesized, &result, &destination, &params));
        assert(pl_tex_download(
            gpu, pl_tex_transfer_params(.tex = target, .ptr = output)));
        struct pl_hdr_metadata original_peak = {0};
        if (peak_detect) {
            struct pl_hdr_metadata synthetic_peak = {0};
            bool first_peak =
                pl_renderer_get_hdr_metadata(original, &original_peak);
            bool second_peak =
                pl_renderer_get_hdr_metadata(synthesized, &synthetic_peak);
            printf("HDR peak detection mastering=%d source=%d synthetic=%d\n",
                   mastering, first_peak, second_peak);
            assert(first_peak);
            assert(second_peak);
        }
        float maximum = 0;
        int worst = 0;
        for (int n = 0; n < WIDTH * HEIGHT * 4; n++) {
            float error = fabsf(output[n] - reference[n]);
            if (error > maximum) {
                maximum = error;
                worst = n;
            }
        }
        printf("HDR synthetic display p3=%d mastering=%d peak_detect=%d "
               "alpha=%.2f maximum=%.9f\n",
               p3, mastering, peak_detect, step / 4.0f, maximum);
        printf("HDR worst pixel=%d,%d channel=%d expected=%.9f actual=%.9f "
               "source=%g,%g,%g alpha=%g\n",
               (worst / 4) % WIDTH, worst / 4 / WIDTH, worst % 4,
               reference[worst], output[worst], pixels[worst / 4 * 4],
               pixels[worst / 4 * 4 + 1], pixels[worst / 4 * 4 + 2],
               pixels[worst / 4 * 4 + 3]);
        fflush(stdout);
        assert(maximum < 0.003);
        if (!p3 && mastering == 0 && peak_detect && step == 1)
            hdr_reference(gpu, &source, &destination, &params, &original_peak,
                          reference, output);
        pl_renderer_destroy(&original);
        pl_renderer_destroy(&synthesized);
    }
    mp_motion_frame_destroy(&frame);
    pl_tex_destroy(gpu, &texture);
    pl_tex_destroy(gpu, &target);
    free(pixels);
    free(reference);
    free(output);
}

static void inferred_color(pl_gpu gpu, struct mp_motion *motion, bool hd_source,
                           bool xyz)
{
    const int width = hd_source ? 1920 : 720, height = hd_source ? 1080 : 480;
    const int out_width = hd_source ? 720 : 1280,
              out_height = hd_source ? 480 : 720;
    float *pixels = calloc(width * height * 4, sizeof(float));
    float *reference = calloc(out_width * out_height * 4, sizeof(float));
    float *output = calloc(out_width * out_height * 4, sizeof(float));
    assert(pixels && reference && output);
    for (int n = 0; n < width * height; n++) {
        pixels[n * 4] = xyz ? 0.4f : 0.9f;
        pixels[n * 4 + 1] = xyz ? 0.4f : 0.3f;
        pixels[n * 4 + 2] = xyz ? 0.4f : 0.1f;
        pixels[n * 4 + 3] = 1;
    }
    pl_tex texture = upload_size(gpu, pixels, width, height);
    struct pl_frame source = pq_frame(texture);
    source.color = pl_color_space_bt709;
    source.color.primaries = PL_COLOR_PRIM_UNKNOWN;
    if (xyz) {
        source.repr.sys = PL_COLOR_SYSTEM_XYZ;
        source.color = (struct pl_color_space){
            .primaries = PL_COLOR_PRIM_BT_2020,
            .transfer = PL_COLOR_TRC_PQ,
        };
    }
    struct mp_motion_frame frame = {0};
    assert(mp_motion_capture_frame(motion, &source, out_width, out_height,
                                   &frame) == MP_MOTION_OK);
    assert(frame.domain == MP_MOTION_SDR);
    assert(frame.color.primaries == (xyz         ? PL_COLOR_PRIM_DCI_P3
                                     : hd_source ? PL_COLOR_PRIM_BT_709
                                                 : PL_COLOR_PRIM_BT_601_525));
    if (xyz)
        assert(frame.color.transfer == PL_COLOR_TRC_ST428);
    pl_fmt format =
        pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                    PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(
        gpu, pl_tex_params(.w = out_width, .h = out_height, .format = format,
                           .renderable = true, .host_readable = true));
    assert(target);
    struct pl_frame destination = pq_frame(target);
    destination.color = pl_color_space_bt709;
    struct pl_frame captured = source;
    captured.planes[0].texture = frame.texture;
    captured.color = frame.color;
    captured.repr = frame.repr;
    struct pl_render_params params = pl_render_default_params;
    params.peak_detect_params = NULL;
    params.dither_params = NULL;
    pl_renderer renderer = pl_renderer_create(gpu->log, gpu);
    assert(pl_render_image(renderer, &source, &destination, &params));
    assert(pl_tex_download(
        gpu, pl_tex_transfer_params(.tex = target, .ptr = reference)));
    assert(pl_render_image(renderer, &captured, &destination, &params));
    assert(pl_tex_download(
        gpu, pl_tex_transfer_params(.tex = target, .ptr = output)));
    float maximum = 0;
    for (int n = 0; n < out_width * out_height * 4; n++)
        maximum = fmaxf(maximum, fabsf(reference[n] - output[n]));
    printf("Inferred display xyz=%d ref=%g,%g,%g actual=%g,%g,%g maxluma=%g "
           "minluma=%g trc=%d\n",
           xyz, reference[0], reference[1], reference[2], output[0], output[1],
           output[2], frame.color.hdr.max_luma, frame.color.hdr.min_luma,
           frame.color.transfer);
    printf("Inferred capture source=%dx%d target=%dx%d primaries=%d "
           "maximum=%.9f\n",
           width, height, out_width, out_height, frame.color.primaries,
           maximum);
    fflush(stdout);
    assert(maximum < 0.003);
    pl_renderer_destroy(&renderer);
    mp_motion_frame_destroy(&frame);
    pl_tex_destroy(gpu, &texture);
    pl_tex_destroy(gpu, &target);
    free(pixels);
    free(reference);
    free(output);
}

static void hdr_cut(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output);
    struct mp_motion_frame frames[2] = {0};
    double endpoints[2];
    for (int i = 0; i < 2; i++) {
        fill_nits(pixels, WIDTH, HEIGHT, i ? 4000 : 0.1);
        pl_tex texture = upload(gpu, pixels);
        struct pl_frame source = pq_frame(texture);
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[i]) == MP_MOTION_OK);
        readback(gpu, frames[i].texture, output);
        endpoints[i] = output[0] * PL_COLOR_SDR_WHITE;
        pl_tex_destroy(gpu, &texture);
    }
    for (int phase = 1; phase < 4; phase++) {
        float alpha = phase / 4.0f;
        struct pl_frame result;
        assert(mp_motion_render_frames(motion, &frames[0], &frames[1], alpha,
                                       &result) == MP_MOTION_OK);
        synthetic_contract(&result);
        readback(gpu, result.planes[0].texture, output);
        double expected = endpoints[alpha < 0.5f ? 0 : 1];
        for (int n = 0; n < WIDTH * HEIGHT; n++) {
            for (int c = 0; c < 3; c++)
                assert(fabs(output[n * 4 + c] * PL_COLOR_SDR_WHITE - expected) <
                       fmax(0.0005, expected * 0.0015));
        }
    }
    for (int i = 0; i < 2; i++)
        mp_motion_frame_destroy(&frames[i]);
    free(pixels);
    free(output);
    puts("PQ scene cut selects and encodes the nearest endpoint: passed");
}

static void display_freshness(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *reference = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && output && reference);
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                                PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE |
                                    PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex expected = pl_tex_create(
        gpu, pl_tex_params(.w = WIDTH, .h = HEIGHT, .format = format,
                           .sampleable = true, .host_writable = true));
    pl_tex target = pl_tex_create(
        gpu, pl_tex_params(.w = WIDTH, .h = HEIGHT, .format = format,
                           .renderable = true, .host_readable = true));
    assert(expected && target);
    struct pl_frame destination = pq_frame(target);
    destination.color = pl_color_space_bt709;
    destination.color.hdr.max_luma = 203;
    struct pl_render_params params = pl_render_default_params;
    params.peak_detect_params = NULL;
    params.dither_params = NULL;
    pl_renderer persistent = pl_renderer_create(gpu->log, gpu);
    assert(persistent);
    struct mp_motion_frame frames[2] = {0};
    pl_tex allocation = NULL;
    float previous = -1;
    float previous_reference = -1;
    double previous_raw = -1;
    const double values[][2] = {
        {10, 20}, {100, 200}, {1000, 1500}, {4000, 5000}};
    for (int pair = 0; pair < 4; pair++) {
        double endpoints[2];
        for (int i = 0; i < 2; i++) {
            double nits = values[pair][i];
            fill_nits(pixels, WIDTH, HEIGHT, nits);
            pl_tex texture = upload(gpu, pixels);
            struct pl_frame source = pq_frame(texture);
            assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                           &frames[i]) == MP_MOTION_OK);
            readback(gpu, frames[i].texture, output);
            endpoints[i] = output[0] * PL_COLOR_SDR_WHITE;
            pl_tex_destroy(gpu, &texture);
        }
        for (int phase = 1; phase < 4; phase++) {
            float alpha = phase / 4.0f;
            struct pl_frame result;
            assert(mp_motion_render_frames(motion, &frames[0], &frames[1],
                                           alpha, &result) == MP_MOTION_OK);
            synthetic_contract(&result);
            assert(!allocation || result.planes[0].texture == allocation);
            allocation = result.planes[0].texture;
            double nits = (1 - alpha) * endpoints[0] + alpha * endpoints[1];
            readback(gpu, result.planes[0].texture, output);
            double raw = output[0] * PL_COLOR_SDR_WHITE;
            assert(fabs(raw - nits) < fmax(0.0005, nits * 0.0015));
            assert(fabs(raw - previous_raw) > 0.0001);
            previous_raw = raw;
            for (int n = 0; n < WIDTH * HEIGHT; n++) {
                for (int c = 0; c < 3; c++)
                    pixels[n * 4 + c] = (float)(nits / PL_COLOR_SDR_WHITE);
                pixels[n * 4 + 3] = 1;
            }
            assert(pl_tex_upload(
                gpu, pl_tex_transfer_params(.tex = expected, .ptr = pixels)));
            struct pl_frame analytical = result;
            analytical.planes[0].texture = expected;
            pl_renderer fresh = pl_renderer_create(gpu->log, gpu);
            assert(fresh);
            assert(pl_render_image(fresh, &analytical, &destination, &params));
            assert(pl_tex_download(
                gpu, pl_tex_transfer_params(.tex = target, .ptr = reference)));
            assert(pl_render_image(persistent, &result, &destination, &params));
            assert(pl_tex_download(
                gpu, pl_tex_transfer_params(.tex = target, .ptr = output)));
            float maximum = 0;
            for (int n = 0; n < WIDTH * HEIGHT * 4; n++)
                maximum = fmaxf(maximum, fabsf(reference[n] - output[n]));
            printf("PQ display freshness pair=%d phase=%.2f error=%.9f "
                   "value=%.9f\n",
                   pair, alpha, maximum, output[0]);
            fflush(stdout);
            assert(maximum < 0.003);
            if (pair < 2) {
                // Both references must be observably different; a clipping
                // plateau cannot detect a stale displayed output.
                assert(fabsf(reference[0] - previous_reference) > 0.0001);
                assert(fabsf(output[0] - previous) > 0.0001);
            } else {
                assert(reference[0] > 0.99f && output[0] > 0.99f);
            }
            previous = output[0];
            previous_reference = reference[0];
            pl_renderer_destroy(&fresh);
        }
    }
    for (int i = 0; i < 2; i++)
        mp_motion_frame_destroy(&frames[i]);
    pl_renderer_destroy(&persistent);
    pl_tex_destroy(gpu, &expected);
    pl_tex_destroy(gpu, &target);
    free(pixels);
    free(output);
    free(reference);
    puts("PQ reused output storage presents each new pair and phase: passed");
}

static void sdr_namespace(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels);
    for (int n = 0; n < WIDTH * HEIGHT; n++) {
        for (int c = 0; c < 3; c++)
            pixels[n * 4 + c] = 0.3f;
        pixels[n * 4 + 3] = 1;
    }
    struct mp_motion_frame frames[2] = {0};
    pl_tex textures[2];
    for (int i = 0; i < 2; i++) {
        textures[i] = upload(gpu, pixels);
        struct pl_frame source = pq_frame(textures[i]);
        source.color = pl_color_space_bt709;
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[i]) == MP_MOTION_OK);
    }
    struct pl_frame result;
    assert(mp_motion_render_frames(motion, &frames[0], &frames[1], 0.5f,
                                   &result) == MP_MOTION_OK);
    uint64_t analyzed = mp_motion_get_stats(motion).analyzed_pairs;
    pl_tex legacy;
    assert(mp_motion_render(motion, textures[0], textures[1], frames[0].id,
                            frames[1].id, 0.5f, &legacy));
    assert(mp_motion_get_stats(motion).analyzed_pairs == analyzed + 1);
    assert(mp_motion_render_frames(motion, &frames[0], &frames[1], 0.5f,
                                   &result) == MP_MOTION_OK);
    assert(mp_motion_get_stats(motion).analyzed_pairs == analyzed + 2);
    for (int i = 0; i < 2; i++) {
        mp_motion_frame_destroy(&frames[i]);
        pl_tex_destroy(gpu, &textures[i]);
    }
    free(pixels);
    puts("SDR frame/legacy identity namespaces: passed");
}

static void hdr_compatibility(pl_gpu gpu, struct mp_motion *motion)
{
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels);
    fill_nits(pixels, WIDTH, HEIGHT, 100);
    pl_tex texture = upload(gpu, pixels);
    struct pl_frame source = pq_frame(texture);
    struct mp_motion_frame frames[2] = {0};
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_OK);
    for (int change = 0; change < 5; change++) {
        source = pq_frame(texture);
        switch (change) {
        case 0:
            source.color.hdr.min_luma = 0.001f;
            break;
        case 1:
            source.color.hdr.max_luma = 4000;
            break;
        case 2:
            source.color.hdr.max_cll = 1000;
            break;
        case 3:
            source.color.hdr.prim =
                *pl_raw_primaries_get(PL_COLOR_PRIM_DISPLAY_P3);
            break;
        case 4:
            source.color.hdr.max_fall = 300;
            break;
        }
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[1]) == MP_MOTION_OK);
        uint64_t generated = mp_motion_get_stats(motion).generated_frames;
        struct pl_frame result;
        enum mp_motion_result status = mp_motion_render_frames(
            motion, &frames[0], &frames[1], 0.5f, &result);
        if (change < 4) {
            assert(status == MP_MOTION_UNSUPPORTED && !result.num_planes);
            assert(mp_motion_render_frames(motion, &frames[0], &frames[1], NAN,
                                           &result) == MP_MOTION_ERROR);
            assert(mp_motion_get_stats(motion).generated_frames == generated);
        } else {
            assert(status == MP_MOTION_OK);
            synthetic_contract(&result);
        }
    }
    struct pl_frame result;
    assert(mp_motion_render_frames(motion, &frames[0], &frames[1], NAN,
                                   &result) == MP_MOTION_ERROR);
    assert(!result.num_planes);
    for (int i = 0; i < 2; i++)
        mp_motion_frame_destroy(&frames[i]);
    pl_tex_destroy(gpu, &texture);
    free(pixels);
    puts("HDR static presentation compatibility is distinct from GPU failure");
}


#ifdef PL_HAVE_DOVI
// CPU reference for the polynomial/NLQ test vectors below. It does not call
// libplacebo's color conversion helpers or read back the expected GPU result.
static void dovi_reference(const struct pl_dovi_metadata *rpu,
                           const float bl[3], const float el[3], double nits[3])
{
    const double lms_to_rgb[3][3] = {
        {3.06441879, -2.16597676, 0.10155818},
        {-0.65612108, 1.78554118, -0.12943749},
        {0.01736321, -0.04725154, 1.03004253},
    };
    double signal[3], lms[3], transformed[3] = {0};
    for (int c = 0; c < 3; c++) {
        double x = bl[c] * 1023.0 / 1024.0;
        const float *poly = rpu->comp[c].poly_coeffs[0];
        signal[c] = poly[0] + x * (poly[1] + x * poly[2]);
        if (rpu->nlq_active) {
            double code = round(el[c] * 1023.0 - rpu->nlq[c].offset);
            double residual = code == 0 ? 0 : copysign(
                (fabs(code) - 0.5) * rpu->nlq[c].deadzone_slope +
                    rpu->nlq[c].deadzone_threshold, code);
            signal[c] += fmax(-rpu->nlq[c].hdr_in_max,
                              fmin(rpu->nlq[c].hdr_in_max, residual));
            signal[c] = fmax(0, fmin(1, signal[c]));
        }
        signal[c] -= rpu->nonlinear_offset[c];
    }
    for (int c = 0; c < 3; c++) {
        double code = 0;
        for (int j = 0; j < 3; j++)
            code += rpu->nonlinear.m[c][j] * signal[j];
        lms[c] = pq_nits(code);
    }
    for (int c = 0; c < 3; c++) {
        for (int j = 0; j < 3; j++)
            transformed[c] += rpu->linear.m[c][j] * lms[j];
    }
    for (int c = 0; c < 3; c++) {
        nits[c] = 0;
        for (int j = 0; j < 3; j++)
            nits[c] += lms_to_rgb[c][j] * transformed[j];
        nits[c] = fmax(0, nits[c]);
    }
}

// Exercise the whole PQ range independently of the lower-luminance FEL cases.
static void dovi_capture_range(pl_gpu gpu, struct mp_motion *motion)
{
    const float values[][3] = {
        {0, 0, 0}, {0.001f, 0.001f, 0.001f}, {0.125f, 0.125f, 0.125f},
        {0.5f, 0.5f, 0.5f}, {0.75f, 0.75f, 0.75f}, {1, 1, 1},
        {0.125f, 0.5f, 0.75f}, {1, 0.75f, 0.5f},
    };
    struct pl_dovi_metadata rpu = {
        // Fixed inverse of the LMS-to-RGB matrix in the CPU reference.
        .linear = {{
            {0.4408196242f, 0.5353728475f, 0.0238130739f},
            {0.1619850896f, 0.7586528815f, 0.0793629501f},
            {0.0000000012f, 0.0257773102f, 0.9740729348f},
        }},
    };
    for (int c = 0; c < 3; c++) {
        rpu.comp[c].num_pivots = 2;
        rpu.comp[c].pivots[1] = 1;
        rpu.comp[c].poly_coeffs[0][1] = 1;
        rpu.nonlinear.m[c][c] = 1;
    }
    const int patches = sizeof(values) / sizeof(values[0]);
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *codes = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *actual = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && codes && actual);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int i = (y * WIDTH + x) * 4;
            for (int c = 0; c < 3; c++)
                pixels[i + c] = values[x * patches / WIDTH][c];
            pixels[i + 3] = 1;
        }
    }
    pl_tex texture = upload(gpu, pixels);
    readback(gpu, texture, codes);
    struct pl_frame source = pq_frame(texture);
    source.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
    source.repr.dovi = &rpu;
    source.repr.bits = (struct pl_bit_encoding){10, 10, 0};
    source.color.hdr.max_luma = 10000;
    struct mp_motion_frame frame = {0};
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT, &frame) ==
           MP_MOTION_OK);
    assert(frame.domain == MP_MOTION_HDR_LINEAR &&
           frame.color.primaries == PL_COLOR_PRIM_BT_2020 &&
           frame.color.transfer == PL_COLOR_TRC_LINEAR_HDR);
    assert(frame.repr.sys == PL_COLOR_SYSTEM_RGB && !frame.repr.dovi);
    readback(gpu, frame.texture, actual);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int i = (y * WIDTH + x) * 4;
            double expected[3];
            dovi_reference(&rpu, &codes[i], NULL, expected);
            for (int c = 0; c < 3; c++) {
                double observed = actual[i + c] * PL_COLOR_SDR_WHITE;
                double tolerance = fmax(0.0005, expected[c] * 0.0015);
                if (!y && x % (WIDTH / patches) == 0) {
                    printf("DV range patch=%d channel=%d code=%.9f "
                           "expected=%.9f actual=%.9f tolerance=%.9f\n",
                           x * patches / WIDTH, c, codes[i + c], expected[c],
                           observed, tolerance);
                    fflush(stdout);
                }
                assert(isfinite(observed) && isfinite(expected[c]));
                assert(fabs(observed - expected[c]) < tolerance);
            }
        }
    }
    mp_motion_frame_destroy(&frame);
    pl_tex_destroy(gpu, &texture);
    free(pixels);
    free(codes);
    free(actual);
}

static void dovi_distinct_pair(pl_gpu gpu, struct mp_motion *motion)
{
    const float opacity = 0.75f;
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *actual = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *reference = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels && actual && reference);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        pixels[i * 4] = 0.5f;
        pixels[i * 4 + 1] = 0.4375f;
        pixels[i * 4 + 2] = 0.5625f;
        pixels[i * 4 + 3] = opacity;
    }
    pl_tex bl = upload(gpu, pixels);
    readback(gpu, bl, pixels);
    struct pl_dovi_metadata rpu[2] = {0};
    struct mp_motion_frame frames[2] = {0};
    double endpoint[2][3];
    float captured[2][3];
    for (int n = 0; n < 2; n++) {
        for (int c = 0; c < 3; c++) {
            rpu[n].comp[c].num_pivots = 2;
            rpu[n].comp[c].pivots[1] = 1;
            rpu[n].comp[c].poly_coeffs[0][0] = 0.125f + n * 0.002f;
            rpu[n].comp[c].poly_coeffs[0][1] = 0.5f;
            rpu[n].nonlinear.m[c][c] = 0.875f;
            rpu[n].linear.m[c][c] = 0.9375f;
        }
        struct pl_frame source = pq_frame(bl);
        source.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
        source.repr.dovi = &rpu[n];
        source.repr.bits = (struct pl_bit_encoding){10, 10, 0};
        source.repr.alpha = PL_ALPHA_INDEPENDENT;
        dovi_reference(&rpu[n], pixels, NULL, endpoint[n]);
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[n]) == MP_MOTION_OK);
        readback(gpu, frames[n].texture, actual);
        for (int c = 0; c < 3; c++) {
            captured[n][c] = actual[c];
            double expected = opacity * endpoint[n][c];
            assert(isfinite(actual[c]));
            assert(fabs(actual[c] * PL_COLOR_SDR_WHITE - expected) <
                   fmax(0.0005, expected * 0.0015));
        }
    }
    assert(frames[0].id != frames[1].id);
    assert(pl_color_space_equal(&frames[0].color, &frames[1].color));
    assert(fabs(captured[1][0] - captured[0][0]) > 0.000001);
    assert(fabs(endpoint[1][0] - endpoint[0][0]) >
           8 * fmax(0.0005, endpoint[1][0] * 0.0015));
    pl_fmt sample_format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                                      PL_FMT_CAP_SAMPLEABLE);
    pl_fmt target_format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(sample_format && target_format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = WIDTH, .h = HEIGHT, .format = target_format,
        .renderable = true, .host_readable = true));
    assert(target);
    struct pl_frame destination = pq_frame(target);
    destination.color = pl_color_space_bt709;
    destination.color.hdr.max_luma = 203;
    pl_renderer renderer = pl_renderer_create(gpu->log, gpu);
    assert(renderer);
    struct pl_render_params params = pl_render_default_params;
    params.dither_params = NULL;
    params.peak_detect_params = NULL;
    float previous_phase = 0;
    const float phases[] = {0.25f, 0.75f};
    for (int phase = 0; phase < 2; phase++) {
        float alpha = phases[phase];
        double expected[3];
        for (int c = 0; c < 3; c++)
            expected[c] = opacity * ((1 - alpha) * endpoint[0][c] +
                                     alpha * endpoint[1][c]);
        struct pl_frame output;
        assert(mp_motion_render_frames(motion, &frames[0], &frames[1], alpha,
                                       &output) == MP_MOTION_OK);
        synthetic_contract(&output);
        readback(gpu, output.planes[0].texture, actual);
        assert(fabs(actual[0] - captured[0][0]) > 0.000001);
        assert(fabs(actual[0] - captured[1][0]) > 0.000001);
        if (phase)
            assert(fabs(actual[0] - previous_phase) > 0.000001);
        previous_phase = actual[0];
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            for (int c = 0; c < 3; c++) {
                double observed = actual[i * 4 + c] * PL_COLOR_SDR_WHITE;
                assert(isfinite(observed));
                assert(fabs(observed - expected[c]) <
                       fmax(0.0005, expected[c] * 0.0015));
                pixels[i * 4 + c] = (float)(expected[c] / PL_COLOR_SDR_WHITE);
            }
            assert(fabs(actual[i * 4 + 3] - opacity) < 0.000001);
            pixels[i * 4 + 3] = opacity;
        }
        // Build the display reference from CPU values, not captured GPU RGB.
        pl_tex oracle = pl_tex_create(gpu, pl_tex_params(
            .w = WIDTH, .h = HEIGHT, .format = sample_format,
            .sampleable = true, .host_writable = true, .initial_data = pixels));
        assert(oracle);
        struct pl_frame cpu_frame = pq_frame(oracle);
        cpu_frame.color.transfer = PL_COLOR_TRC_LINEAR_HDR;
        cpu_frame.repr.alpha = PL_ALPHA_PREMULTIPLIED;
        assert(pl_render_image(renderer, &cpu_frame, &destination, &params));
        assert(pl_tex_download(gpu, pl_tex_transfer_params(
            .tex = target, .ptr = reference)));
        assert(pl_render_image(renderer, &output, &destination, &params));
        assert(pl_tex_download(gpu, pl_tex_transfer_params(
            .tex = target, .ptr = actual)));
        double maximum = 0;
        for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
            assert(isfinite(actual[i]) && isfinite(reference[i]));
            maximum = fmax(maximum, fabs(actual[i] - reference[i]));
        }
        printf("DV distinct pair alpha=%.2f expected_premul_nits=%.9f "
               "display_error=%.9f\n", alpha, expected[0], maximum);
        fflush(stdout);
        assert(maximum < 0.003);
        pl_tex_destroy(gpu, &oracle);
    }
    pl_renderer_destroy(&renderer);
    pl_tex_destroy(gpu, &target);
    mp_motion_frame_destroy(&frames[0]);
    mp_motion_frame_destroy(&frames[1]);
    pl_tex_destroy(gpu, &bl);
    free(pixels);
    free(actual);
    free(reference);
}

struct dovi_source_owner {
    int acquired, released;
    bool ready;
    enum pl_field field;
};

static bool dovi_acquire(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu;
    struct dovi_source_owner *owner = frame->user_data;
    owner->acquired++;
    frame->field = owner->field;
    return owner->ready;
}

static void dovi_release(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu;
    struct dovi_source_owner *owner = frame->user_data;
    assert(frame->field == owner->field);
    owner->released++;
}
#endif

static void dovi_frames(pl_gpu gpu, struct mp_motion *motion)
{
    const float bl_code[] = {0.5f, 0.4375f, 0.5625f};
    float *pixels = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(pixels);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        for (int c = 0; c < 3; c++)
            pixels[i * 4 + c] = bl_code[c];
        pixels[i * 4 + 3] = 1;
    }
    pl_tex bl = upload(gpu, pixels);
    struct pl_frame source = pq_frame(bl);
    source.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
    source.repr.bits = (struct pl_bit_encoding){10, 10, 0};
    struct mp_motion_frame frames[2] = {0};
    assert(!mp_motion_capture_supported(motion, &source));
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_UNSUPPORTED);
#ifdef PL_HAVE_DOVI
    dovi_capture_range(gpu, motion);
    dovi_distinct_pair(gpu, motion);
    struct pl_dovi_metadata rpu = {0};
    for (int c = 0; c < 3; c++) {
        rpu.comp[c].num_pivots = 2;
        rpu.comp[c].pivots[1] = 1;
        rpu.comp[c].poly_coeffs[0][0] = 0.125f;
        rpu.comp[c].poly_coeffs[0][1] = 0.5f;
        rpu.nonlinear.m[c][c] = 0.875f;
        rpu.linear.m[c][c] = 0.9375f;
        rpu.nlq[c].offset = 512;
        rpu.nlq[c].deadzone_slope = 0.001f;
        rpu.nlq[c].deadzone_threshold = 0.0005f;
        rpu.nlq[c].hdr_in_max = 0.25f;
    }
    source.repr.dovi = &rpu;
    float *actual = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    float *reference = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(actual && reference);
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = WIDTH, .h = HEIGHT, .format = format,
        .renderable = true, .host_readable = true));
    assert(target);
    pl_renderer renderer = pl_renderer_create(gpu->log, gpu);
    assert(renderer);
    struct pl_frame destination = pq_frame(target);
    destination.color = pl_color_space_bt709;
    destination.color.hdr.max_luma = 203;
    struct pl_render_params params = pl_render_default_params;
    params.peak_detect_params = NULL;
    params.dither_params = NULL;
    double previous = 0;
    pl_tex el = NULL;
    struct pl_frame enhancement = {0};
    // BL-only and FEL negative/zero/positive residuals; only the EL changes.
    for (int test = 0; test < 4; test++) {
        rpu.nlq_active = test != 0;
        float el_code = (float)(test == 1 ? 480 : test == 2 ? 512 : 544) / 1023;
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            for (int c = 0; c < 3; c++)
                pixels[i * 4 + c] = el_code;
        }
        pl_tex_destroy(gpu, &el);
        el = upload(gpu, pixels);
        readback(gpu, el, pixels);
        enhancement = pq_frame(el);
        enhancement.repr.bits = (struct pl_bit_encoding){10, 10, 0};
        source.enhancement_layer = test ? &enhancement : NULL;
        // The RPU defines BT.2020/PQ even if the transport tags say otherwise.
        source.color.primaries = PL_COLOR_PRIM_BT_709;
        source.color.transfer = PL_COLOR_TRC_SRGB;
        assert(mp_motion_capture_supported(motion, &source));
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[0]) == MP_MOTION_OK);
        assert(frames[0].color.primaries == PL_COLOR_PRIM_BT_2020 &&
               frames[0].color.transfer == PL_COLOR_TRC_LINEAR_HDR &&
               frames[0].domain == MP_MOTION_HDR_LINEAR);
        assert(frames[0].repr.sys == PL_COLOR_SYSTEM_RGB &&
               !frames[0].repr.dovi);
        readback(gpu, frames[0].texture, actual);
        double expected[3];
        dovi_reference(&rpu, bl_code, pixels, expected);
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            for (int c = 0; c < 3; c++) {
                double value = actual[i * 4 + c] * PL_COLOR_SDR_WHITE;
                assert(isfinite(value));
                assert(fabs(value - expected[c]) <
                       fmax(0.0005, expected[c] * 0.0015));
            }
        }
        if (test > 1)
            assert(actual[0] > previous);
        previous = actual[0];
        struct pl_frame output;
        assert(mp_motion_render_frames(motion, &frames[0], &frames[0],
                                       0.5f, &output) == MP_MOTION_OK);
        synthetic_contract(&output);
        struct pl_frame canonical = source;
        canonical.color.primaries = PL_COLOR_PRIM_BT_2020;
        canonical.color.transfer = PL_COLOR_TRC_PQ;
        assert(pl_render_image(renderer, &canonical, &destination, &params));
        assert(pl_tex_download(gpu, pl_tex_transfer_params(
            .tex = target, .ptr = reference)));
        assert(pl_render_image(renderer, &output, &destination, &params));
        assert(pl_tex_download(gpu, pl_tex_transfer_params(
            .tex = target, .ptr = actual)));
        double maximum = 0;
        for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
            assert(isfinite(actual[i]) && isfinite(reference[i]));
            maximum = fmax(maximum, fabs(actual[i] - reference[i]));
        }
        printf("DV capture case=%d CPU_nits=%.9f display_error=%.9f\n",
               test, expected[0], maximum);
        fflush(stdout);
        assert(maximum < 0.003);
    }
    // Different real polynomial coefficients decode this BL to the same RGB.
    rpu.nlq_active = false;
    source.enhancement_layer = NULL;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_OK);
    for (int c = 0; c < 3; c++) {
        rpu.comp[c].poly_coeffs[0][0] -= 0.25f * (bl_code[c] * 1023 / 1024);
        rpu.comp[c].poly_coeffs[0][1] += 0.25f;
    }
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[1]) == MP_MOTION_OK);
    readback(gpu, frames[0].texture, reference);
    readback(gpu, frames[1].texture, actual);
    for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
        assert(isfinite(actual[i]) && isfinite(reference[i]));
        assert(fabs(actual[i] - reference[i]) < 0.000001);
    }
    struct pl_frame output;
    assert(mp_motion_render_frames(motion, &frames[0], &frames[1], 0.5f,
                                   &output) == MP_MOTION_OK);
    synthetic_contract(&output);
    readback(gpu, output.planes[0].texture, actual);
    for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
        assert(isfinite(actual[i]) && isfinite(reference[i]));
        assert(fabs(actual[i] - reference[i]) < 0.000001);
    }
    source.color.hdr.avg_pq_y = 0.5f;
    assert(!mp_motion_capture_supported(motion, &source));
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_UNSUPPORTED);
    source.color.hdr.avg_pq_y = 0;
    source.color.hdr.ootf.target_luma = 400;
    assert(!mp_motion_capture_supported(motion, &source));
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_UNSUPPORTED);
    source.color.hdr.ootf.target_luma = 0;
    rpu.nlq_active = true;
    assert(!mp_motion_capture_supported(motion, &source));
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_UNSUPPORTED);
    struct dovi_source_owner bl_owner = {.ready = true};
    struct dovi_source_owner el_owner = {.ready = true};
    source.acquire = dovi_acquire;
    source.release = dovi_release;
    source.user_data = &bl_owner;
    enhancement.acquire = dovi_acquire;
    enhancement.release = dovi_release;
    enhancement.user_data = &el_owner;
    source.enhancement_layer = &enhancement;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_OK);
    assert(bl_owner.acquired == 1 && bl_owner.released == 1);
    assert(el_owner.acquired == 1 && el_owner.released == 1);

    // Classification must use the acquired EL, not just its initial descriptor.
    el_owner.field = PL_FIELD_TOP;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_UNSUPPORTED);
    assert(bl_owner.acquired == 2 && bl_owner.released == 2);
    assert(el_owner.acquired == 2 && el_owner.released == 2);
    assert(!frames[0].id && !frames[0].owner);
    el_owner.field = PL_FIELD_NONE;
    el_owner.ready = false;
    int errors = gpu_errors;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_ERROR);
    assert(bl_owner.acquired == 3 && bl_owner.released == 3);
    assert(el_owner.acquired == 3 && el_owner.released == 3);
    assert(!frames[0].id && !frames[0].owner && gpu_errors == errors);

    el_owner.ready = true;
    enhancement.planes[0].texture = NULL;
    expected_gpu_errors = 0;
    expected_gpu_error = "Failed sampling enhancement layer; "
                         "falling back to base-layer-only rendering";
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) != MP_MOTION_OK);
    expected_gpu_error = NULL;
    assert(expected_gpu_errors == 1 && gpu_errors == errors);
    assert(bl_owner.acquired == 4 && bl_owner.released == 4);
    assert(el_owner.acquired == 4 && el_owner.released == 4);
    assert(!frames[0].id && !frames[0].owner);

    // Neither failed sampling nor its sticky renderer error may poison reuse.
    enhancement.planes[0].texture = el;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_OK);
    assert(frames[0].id && frames[0].owner == motion);
    assert(bl_owner.acquired == 5 && bl_owner.released == 5);
    assert(el_owner.acquired == 5 && el_owner.released == 5);
    bl_owner.ready = false;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_ERROR);
    assert(bl_owner.acquired == 6 && bl_owner.released == 6);
    assert(el_owner.acquired == 5 && el_owner.released == 5);
    assert(!frames[0].id && !frames[0].owner && gpu_errors == errors);
    bl_owner.ready = true;
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_OK);
    assert(bl_owner.acquired == 7 && bl_owner.released == 7);
    assert(el_owner.acquired == 6 && el_owner.released == 6);
    assert(frames[0].id && frames[0].owner == motion && gpu_errors == errors);
    pl_renderer_destroy(&renderer);
    pl_tex_destroy(gpu, &target);
    pl_tex_destroy(gpu, &el);
    free(actual);
    free(reference);
#else
    struct pl_dovi_metadata rpu = {0};
    source.repr.dovi = &rpu;
    assert(!mp_motion_capture_supported(motion, &source));
    assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                   &frames[0]) == MP_MOTION_UNSUPPORTED);
    puts("DV capture rejected: libplacebo built without DOVI support");
#endif
    mp_motion_frame_destroy(&frames[0]);
    mp_motion_frame_destroy(&frames[1]);
    pl_tex_destroy(gpu, &bl);
    free(pixels);
}

static void hdr_owned_output(pl_gpu gpu)
{
    const size_t count = WIDTH * HEIGHT * 4;
    float *reference = malloc(count * sizeof(*reference));
    float *pixels = malloc(count * sizeof(*pixels));
    struct mp_motion *motion = mp_motion_create(gpu, WIDTH);
    assert(reference && pixels && motion);

    struct mp_motion_frame frames[2] = {0};
    for (int i = 0; i < 2; i++) {
        pattern(pixels, 4 * i);
        pl_tex texture = upload(gpu, pixels);
        struct pl_frame source = pq_frame(texture);
        assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                       &frames[i]) == MP_MOTION_OK);
        pl_tex_destroy(gpu, &texture);
    }
    struct pl_frame borrowed, retained, next;
    assert(mp_motion_render_frames(motion, &frames[0], &frames[1], 0.5f,
                                   &borrowed) == MP_MOTION_OK);
    readback(gpu, borrowed.planes[0].texture, reference);
    assert(mp_motion_render_frames_owned(motion, &frames[0], &frames[1], 0.5f,
                                         &retained) == MP_MOTION_OK);
    assert(retained.planes[0].texture == borrowed.planes[0].texture);
    assert(pl_color_space_equal(&retained.color, &borrowed.color));
    assert(pl_color_repr_equal(&retained.repr, &borrowed.repr));
    synthetic_contract(&retained);
    assert(mp_motion_render_frames_owned(motion, &frames[0], &frames[1], 0.75f,
                                         &next) == MP_MOTION_OK);
    assert(retained.planes[0].texture != next.planes[0].texture);
    pl_tex_destroy(gpu, &next.planes[0].texture);
    assert(mp_motion_render_frames_owned(motion, &frames[0], &frames[0], 0.5f,
                                         &next) == MP_MOTION_UNSUPPORTED);
    assert(!next.planes[0].texture);

    mp_motion_reset(motion);
    for (int i = 0; i < 2; i++)
        mp_motion_frame_destroy(&frames[i]);
    mp_motion_destroy(&motion);
    readback(gpu, retained.planes[0].texture, pixels);
    assert(!memcmp(pixels, reference, count * sizeof(*pixels)));
    pl_tex_destroy(gpu, &retained.planes[0].texture);
    free(pixels);
    free(reference);
}

static void hdr_frames(pl_gpu gpu, struct mp_motion *motion)
{
    if (!mp_motion_hdr_supported(gpu)) {
        puts("PQ frame tests skipped: FP32 working format unavailable");
        return;
    }
    low_peak_contrast(gpu, motion, true, false);
    hdr_owned_output(gpu);
    low_peak_contrast(gpu, motion, false, false);
    low_peak_contrast(gpu, motion, true, true);
    dovi_frames(gpu, motion);
    capture_contract(gpu, motion);
    acquired_contract(gpu, motion);
    p010_capture(gpu, motion);
    source_roundtrip(gpu, motion, false);
    source_roundtrip(gpu, motion, true);
    alpha_blend(gpu, motion, false);
    alpha_blend(gpu, motion, true);
    const int mastering[] = {0, 20, 203, 204, 1000, 4000, 10000};
    for (size_t i = 0; i < sizeof(mastering) / sizeof(mastering[0]); i++) {
        for (int peak = 0; peak < 2; peak++) {
            synthetic_roundtrip(gpu, motion, false, mastering[i], peak);
            synthetic_roundtrip(gpu, motion, true, mastering[i], peak);
        }
    }
    hdr_compatibility(gpu, motion);
    inferred_color(gpu, motion, true, false);
    inferred_color(gpu, motion, false, false);
    inferred_color(gpu, motion, true, true);
    sdr_namespace(gpu, motion);
    hdr_cut(gpu, motion);
    display_freshness(gpu, motion);
    linear_blend(gpu, motion, 0.1, 0.2);
    linear_blend(gpu, motion, 100, 200);
    linear_blend(gpu, motion, 1000, 1500);
    linear_blend(gpu, motion, 4000, 5000);
    translated_hdr(gpu, motion, 8);
    translated_hdr(gpu, motion, 4);
}

static void prepared_translation(pl_gpu gpu, pl_fmt format, bool hdr)
{
    const int shift = 4;
    float opacity = hdr ? .75f : .25f;
    float scale = hdr ? 4 : .5f;
    float *pixels[2], *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(output);
    struct mp_motion *motion = mp_motion_create(gpu, 128);
    assert(motion);
    struct pl_rendered_frame views[2] = {0};
    for (int i = 0; i < 2; i++) {
        pixels[i] = calloc(WIDTH * HEIGHT * 4, sizeof(float));
        assert(pixels[i]);
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                for (int c = 0; c < 3; c++)
                    pixels[i][(y * WIDTH + x) * 4 + c] =
                        scale * opacity * (.15f + .7f * noise(x - i * shift, y, c));
                pixels[i][(y * WIDTH + x) * 4 + 3] = opacity;
            }
        }
        views[i].texture = pl_tex_create(gpu, pl_tex_params(
            .w = WIDTH, .h = HEIGHT, .format = format,
            .sampleable = true, .initial_data = pixels[i]));
        views[i].signature = i + 1;
        assert(views[i].texture);
    }
    for (int phase = 1; phase < 4; phase++) {
        float alpha = phase / 4.0f;
        pl_tex result = NULL;
        assert(mp_motion_render_views(motion, &views[0], &views[1], alpha,
                                      &result) == MP_MOTION_OK);
        readback(gpu, result, output);
        double sum = 0, maximum = 0;
        int count = 0;
        for (int y = 24; y < HEIGHT - 24; y++) {
            for (int x = 24; x < WIDTH - 24; x++) {
                assert(fabsf(output[(y * WIDTH + x) * 4 + 3] - opacity) < .0005f);
                for (int c = 0; c < 3; c++) {
                    // All phases land on integer samples; this is a translated
                    // common pattern, not a whole-frame luminance scene change.
                    int source_x = x - (int)(alpha * shift);
                    double expected = pixels[0][(y * WIDTH + source_x) * 4 + c];
                    double actual = output[(y * WIDTH + x) * 4 + c];
                    assert(isfinite(actual));
                    double error = fabs(pq_code(expected * PL_COLOR_SDR_WHITE) -
                                        pq_code(actual * PL_COLOR_SDR_WHITE));
                    sum += error;
                    maximum = fmax(maximum, error);
                    count++;
                }
            }
        }
        printf("Prepared translation hdr=%d phase=%.2f mean=%.9f max=%.9f\n",
               hdr, alpha, sum / count, maximum);
        fflush(stdout);
        assert(sum / count < .02 && maximum < .1);
        assert(mp_motion_get_stats(motion).analyzed_pairs == 1);
    }
    mp_motion_destroy(&motion);
    for (int i = 0; i < 2; i++) {
        readback(gpu, views[i].texture, output);
        assert(!memcmp(output, pixels[i], WIDTH * HEIGHT * 4 * sizeof(float)));
        pl_tex_destroy(gpu, &views[i].texture);
        free(pixels[i]);
    }
    free(output);
}

static void prepared_views(pl_gpu gpu)
{
    if (!mp_motion_hdr_supported(gpu)) {
        puts("Prepared-view FP32 capability unavailable: skipped");
        return;
    }
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                                PL_FMT_CAP_SAMPLEABLE);
    assert(format);
    float *pixels[2], *output = calloc(WIDTH * HEIGHT * 4, sizeof(float));
    assert(output);
    for (int i = 0; i < 2; i++) {
        pixels[i] = calloc(WIDTH * HEIGHT * 4, sizeof(float));
        assert(pixels[i]);
    }
    pl_fmt nearest_only = NULL;
    for (int i = 0; i < gpu->num_formats; i++) {
        pl_fmt candidate = gpu->formats[i];
        if (candidate->type != PL_FMT_FLOAT || candidate->num_components != 4 ||
            !(candidate->caps & PL_FMT_CAP_SAMPLEABLE) ||
            (candidate->caps & PL_FMT_CAP_LINEAR))
            continue;
        bool fp32 = true;
        for (int c = 0; c < 4; c++)
            fp32 &= candidate->component_depth[c] == 32;
        if (fp32) {
            nearest_only = candidate;
            break;
        }
    }
    if (nearest_only)
        format = nearest_only;
    printf("Prepared-view FP32 input format: %s, hardware linear: %s\n",
           format->name, format->caps & PL_FMT_CAP_LINEAR ? "yes" : "no");
    const float opacity[] = {0, .25f, .75f, 1};
    for (int range = 0; range < 2; range++) {
        for (int a = 0; a < 4; a++) {
            struct mp_motion *motion = mp_motion_create(gpu, 128);
            assert(motion);
            struct pl_rendered_frame views[2] = {0};
            for (int i = 0; i < 2; i++) {
                float alpha = opacity[a];
                for (int n = 0; n < WIDTH * HEIGHT; n++) {
                    for (int c = 0; c < 3; c++) {
                        float linear = (range ? 4.0f : .2f) * (1 + c * .1f);
                        pixels[i][n * 4 + c] = linear * alpha;
                    }
                    pixels[i][n * 4 + 3] = alpha;
                }
                views[i].texture = pl_tex_create(gpu, pl_tex_params(
                    .w = WIDTH, .h = HEIGHT, .format = format,
                    .sampleable = true, .initial_data = pixels[i]));
                views[i].signature = 100 + i;
                views[i].timestamp = i ? .75 : -.25;
                assert(views[i].texture);
            }
            struct mp_motion_stats before_prepare = mp_motion_get_stats(motion);
            assert(mp_motion_prepare_view(motion, &views[0]) == MP_MOTION_OK);
            struct mp_motion_stats after_prepare = mp_motion_get_stats(motion);
            assert(!memcmp(&before_prepare, &after_prepare, sizeof(before_prepare)));
            assert(mp_motion_prepare_view(motion, &views[0]) == MP_MOTION_OK);
            after_prepare = mp_motion_get_stats(motion);
            assert(!memcmp(&before_prepare, &after_prepare, sizeof(before_prepare)));
            if (!range && !a) {
                uint64_t signature = views[0].signature;
                views[0].signature = 0;
                pl_tex invalid = views[0].texture;
                assert(mp_motion_render_views(motion, &views[0], &views[1],
                                              .5f, &invalid) == MP_MOTION_UNSUPPORTED);
                assert(!invalid);
                views[0].signature = signature;
            }
            for (int phase = 1; phase < 4; phase++) {
                float alpha = phase / 4.0f;
                pl_tex result = NULL;
                assert(mp_motion_render_views(motion, &views[0], &views[1],
                                              alpha, &result) == MP_MOTION_OK);
                assert(result && result->params.format->type == PL_FMT_FLOAT);
                for (int c = 0; c < 4; c++)
                    assert(result->params.format->component_depth[c] == 32);
                readback(gpu, result, output);
                double maximum = 0;
                for (int n = 0; n < WIDTH * HEIGHT * 4; n++) {
                    double expected = (1.0 - alpha) * pixels[0][n] + alpha * pixels[1][n];
                    assert(isfinite(output[n]));
                    maximum = fmax(maximum, fabs(output[n] - expected));
                }
                printf("Prepared linear view range=%d opacity=%.2f phase=%.2f max=%.9f\n",
                       range, opacity[a], alpha, maximum);
                fflush(stdout);
                assert(maximum < .0005);
                assert(mp_motion_get_stats(motion).analyzed_pairs == 1);
            }
            pl_tex result = NULL;
            uint64_t generated = mp_motion_get_stats(motion).generated_frames;
            assert(mp_motion_render_views(motion, &views[0], &views[1], .75f,
                                          &result) == MP_MOTION_OK);
            assert(mp_motion_get_stats(motion).generated_frames == generated);
            views[1].signature++;
            assert(mp_motion_render_views(motion, &views[0], &views[1], .5f,
                                          &result) == MP_MOTION_OK);
            assert(mp_motion_get_stats(motion).analyzed_pairs == 2);
            // Numeric IDs alone cannot alias legacy or captured-frame caches.
            assert(mp_motion_render(motion, views[0].texture, views[1].texture,
                                    views[0].signature, views[1].signature, .5f,
                                    &result));
            assert(mp_motion_get_stats(motion).analyzed_pairs == 3);
            assert(mp_motion_render_views(motion, &views[0], &views[1], .5f,
                                          &result) == MP_MOTION_OK);
            assert(mp_motion_get_stats(motion).analyzed_pairs == 4);
            struct mp_motion_frame captured[2] = {0};
            for (int i = 0; i < 2; i++) {
                struct pl_frame source = pq_frame(views[i].texture);
                // The low-range inputs are valid PQ codes even after restoring
                // independent alpha. Preserve HDR here so captured and views
                // both use linear_hdr=true: only their namespace distinguishes
                // equal numeric IDs. Values above one are not PQ source codes.
                if (range)
                    source.color = pl_color_space_bt709;
                source.repr.alpha = PL_ALPHA_PREMULTIPLIED;
                assert(mp_motion_capture_frame(motion, &source, WIDTH, HEIGHT,
                                               &captured[i]) == MP_MOTION_OK);
                views[i].signature = captured[i].id;
            }
            struct pl_frame described;
            assert(mp_motion_render_frames(motion, &captured[0], &captured[1],
                                           .5f, &described) == MP_MOTION_OK);
            uint64_t analyzed = mp_motion_get_stats(motion).analyzed_pairs;
            assert(mp_motion_render_views(motion, &views[0], &views[1], .5f,
                                          &result) == MP_MOTION_OK);
            assert(mp_motion_get_stats(motion).analyzed_pairs == analyzed + 1);
            assert(mp_motion_render_frames(motion, &captured[0], &captured[1],
                                           .5f, &described) == MP_MOTION_OK);
            assert(mp_motion_get_stats(motion).analyzed_pairs == analyzed + 2);
            const float invalid[] = {0, 1, -.1f, 1.1f, NAN, INFINITY};
            for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
                result = views[0].texture;
                assert(mp_motion_render_views(motion, &views[0], &views[1],
                                              invalid[i], &result) == MP_MOTION_ERROR);
                assert(!result);
            }
            for (int i = 0; i < 2; i++)
                mp_motion_frame_destroy(&captured[i]);
            mp_motion_destroy(&motion);
            assert(!motion);
            for (int i = 0; i < 2; i++) {
                // Borrowed textures remain owned by LP's caller and unchanged.
                readback(gpu, views[i].texture, output);
                assert(!memcmp(output, pixels[i], WIDTH * HEIGHT * 4 * sizeof(float)));
                pl_tex_destroy(gpu, &views[i].texture);
            }
        }
    }
    free(pixels[0]);
    free(pixels[1]);
    free(output);
    prepared_translation(gpu, format, false);
    prepared_translation(gpu, format, true);
    puts("Prepared-view linear pixels, identities and borrowed lifetime: passed");
}

static void pipeline_pattern(float *pixels, int shift, bool hdr)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            for (int c = 0; c < 3; c++) {
                float value = noise((x - shift + WIDTH) % WIDTH, y, c);
                pixels[(y * WIDTH + x) * 4 + c] = hdr
                    ? (float)pq_code(20 + 600 * value) : .1f + .7f * value;
            }
            pixels[(y * WIDTH + x) * 4 + 3] = 1;
        }
    }
}

static bool reject_motion_shader(void *priv, const struct pl_rendered_mix *images,
                                 pl_shader shader)
{
    assert(mix_motion_shader(priv, images, shader));
    return false;
}

static void single_presentation(pl_gpu gpu)
{
    if (!mp_motion_hdr_supported(gpu))
        return;
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    size_t bytes = WIDTH * HEIGHT * 4 * sizeof(float);
    float *pixels = malloc(bytes), *expected = malloc(bytes), *actual = malloc(bytes);
    assert(pixels && expected && actual);
    const char hook_text[] =
        "//!HOOK OUTPUT\n//!BIND HOOKED\n"
        "vec4 hook() { return HOOKED_texOff(0) * vec4(0.9, 0.8, 0.7, 1.0); }\n";
    const struct pl_hook *hook = pl_mpv_user_shader_parse(
        gpu, hook_text, sizeof(hook_text) - 1);
    assert(hook);
    for (int mode = 0; mode < 3; mode++) {
        for (int geometry = 0; geometry < 2; geometry++) {
            struct mp_motion *motion = mp_motion_create(gpu, WIDTH);
            if (!mp_motion_shader_supported(motion)) {
                mp_motion_destroy(&motion);
                continue;
            }
            pl_renderer rr = pl_renderer_create(gpu->log, gpu);
            pl_source_peak_state state = pl_source_peak_state_create(gpu->log, gpu);
            pipeline_pattern(pixels, 0, mode == 1);
            for (int i = 0; i < WIDTH * HEIGHT; i++)
                pixels[i * 4 + 3] = geometry ? .25f + .5f * noise(i, 0, 0) : 1;
            struct pl_frame frame = pq_frame(upload(gpu, pixels));
            frame.repr.alpha = PL_ALPHA_INDEPENDENT;
            if (!mode)
                frame.color = pl_color_space_bt709;
            if (mode == 2) {
                frame.color.transfer = PL_COLOR_TRC_HLG;
                frame.color.hdr.max_luma = 0;
            }
            if (geometry) {
                frame.crop = (struct pl_rect2df){4, 6, WIDTH - 8, HEIGHT - 10};
                frame.rotation = PL_ROTATION_90;
            }
            pl_tex output = pl_tex_create(gpu, pl_tex_params(
                .w = WIDTH, .h = HEIGHT, .format = format,
                .renderable = true, .sampleable = true, .host_readable = true,
                .blit_dst = true,
            ));
            assert(output);
            struct pl_frame target = pq_frame(output);
            target.repr.alpha = PL_ALPHA_INDEPENDENT;
            if (!mode)
                target.color = pl_color_space_bt709;
            struct pl_render_params params = pl_render_fast_params;
            params.hooks = &hook;
            params.num_hooks = 1;
            uint64_t id = 1;
            assert(pl_source_peak_submit(rr, state, 1, id, &frame, NULL) !=
                   PL_SOURCE_PEAK_ERROR);
            struct pl_source_peak_context context = {
                .state = state, .epoch = 1, .ids = &id, .current_id = id,
            };
            const struct pl_frame *frames[] = {&frame};
            double timestamp = 0;
            struct pl_frame_mix mix = {
                .frames = frames, .num_frames = 1, .signatures = &id,
                .timestamps = &timestamp, .vsync_duration = .25,
            };
            struct motion_mix initialization = {.motion = motion, .prepare_only = true};
            const struct pl_custom_frame_mixer texture_mixer = {
                .priv = &initialization, .mix = mix_motion_views,
            };
            const struct pl_frame_shader_mixer shader_mixer = {
                .priv = &initialization, .mix = mix_motion_shader,
            };
            assert(pl_render_image_mix_source(rr, &mix, &target, &params,
                                              &context, &texture_mixer));
            readback(gpu, output, expected);
            for (int reset = 0; reset < 2; reset++) {
                if (reset)
                    mp_motion_reset(motion);
                assert(pl_render_image_mix_source_shader(rr, &mix, &target, &params,
                                                         &context, &shader_mixer));
                readback(gpu, output, actual);
                assert(!memcmp(actual, expected, bytes));
                assert(mp_motion_get_stats(motion).analyzed_pairs == 0);
                assert(mp_motion_get_stats(motion).generated_frames == 0);
            }
            printf("Single presentation mode=%d geometry=%d hooks/reset bitexact\n",
                   mode, geometry);
            pl_renderer_destroy(&rr);
            pl_source_peak_state_destroy(&state);
            mp_motion_destroy(&motion);
            pl_tex_destroy(gpu, &frame.planes[0].texture);
            pl_tex_destroy(gpu, &output);
        }
    }
    pl_mpv_user_shader_destroy(&hook);
    free(pixels);
    free(expected);
    free(actual);
}

static void prepared_pipeline(pl_gpu gpu)
{
    if (!mp_motion_hdr_supported(gpu)) {
        puts("Prepared-view pipeline FP32 capability unavailable: skipped");
        return;
    }
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    size_t bytes = WIDTH * HEIGHT * 4 * sizeof(float);
    float *pixels = malloc(bytes), *actual = malloc(bytes), *expected = malloc(bytes);
    assert(pixels && actual && expected);
    for (int mode = 0; mode < 4; mode++) {
        for (int delayed = 0; delayed < 2; delayed++) {
            struct mp_motion *motion = mp_motion_create(gpu, WIDTH);
            pl_renderer rr = pl_renderer_create(gpu->log, gpu);
            pl_renderer oracle = pl_renderer_create(gpu->log, gpu);
            pl_source_peak_state state = pl_source_peak_state_create(gpu->log, gpu);
            assert(motion && rr && oracle && state);
            struct pl_peak_detect_params peak = pl_peak_detect_default_params;
            peak.allow_delayed = delayed;
            struct pl_render_params params = pl_render_fast_params;
            params.peak_detect_params = &peak;
            struct pl_frame frames[2];
            const struct pl_frame *sources[] = {&frames[0], &frames[1]};
            uint64_t ids[] = {2, 3};
            for (int i = 0; i < 2; i++) {
                pipeline_pattern(pixels, 8 * i, mode & 1);
                frames[i] = pq_frame(upload(gpu, pixels));
                if (!(mode & 1))
                    frames[i].color = pl_color_space_bt709;
                // Warm both delayed snapshots with the same histogram. This
                // oracle compares translation, not blending different maps.
                if (!i)
                    assert(pl_source_peak_submit(rr, state, 1, 1, &frames[i],
                                                  &peak) != PL_SOURCE_PEAK_ERROR);
                assert(pl_source_peak_submit(rr, state, 1, ids[i], &frames[i],
                                              &peak) != PL_SOURCE_PEAK_ERROR);
            }
            pl_tex destination = pl_tex_create(gpu, pl_tex_params(
                .w = WIDTH, .h = HEIGHT, .format = format,
                .renderable = true, .sampleable = true, .host_readable = true,
            ));
            assert(destination);
            struct pl_frame target = pq_frame(destination);
            if (!(mode & 2))
                target.color = pl_color_space_bt709;
            struct pl_source_peak_context context = {
                .state = state, .epoch = 1, .ids = ids, .current_id = 2,
            };
            double timestamps[] = {0, 1};
            struct pl_frame_mix mix = {
                .frames = sources, .num_frames = 2, .signatures = ids,
                .timestamps = timestamps, .vsync_duration = .25,
            };
            pl_fmt half = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
                PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR);
            if (!half)
                half = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
                    PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE);
            if (half) {
                for (int c = 0; c < 4; c++) {
                    if (half->component_depth[c] != 16) {
                        half = NULL;
                        break;
                    }
                }
            }
            char allocation[128];
            snprintf(allocation, sizeof(allocation),
                     "(Re)creating %dx%dx0 texture with format %s:",
                     WIDTH, HEIGHT, half ? half->name : "unavailable");
            expected_allocation = allocation;
            saw_expected_allocation = false;
            enum pl_log_level previous_level = pl_log_level_update(gpu->log, PL_LOG_DEBUG);
            assert(pl_render_image_mix_source(rr, &mix, &target, &params,
                                               &context, NULL));
            if (half)
                assert(saw_expected_allocation);
            pl_log_level_update(gpu->log, previous_level);
            expected_allocation = NULL;
            assert(mp_motion_get_stats(motion).analyzed_pairs == 0);
            struct pl_frame_mix single = mix;
            single.num_frames = 1;
            assert(pl_render_image_mix_source(rr, &single, &target, &params,
                                               &context, NULL));
            readback(gpu, destination, expected);
            struct motion_mix initialization = {.motion = motion, .prepare_only = true};
            const struct pl_custom_frame_mixer prepare_mixer = {
                .priv = &initialization, .mix = mix_motion_views,
            };
            snprintf(allocation, sizeof(allocation),
                     "(Re)creating %dx%dx0 texture with format %s:",
                     WIDTH, HEIGHT, format->name);
            expected_allocation = allocation;
            saw_expected_allocation = false;
            pl_log_level_update(gpu->log, PL_LOG_DEBUG);
            assert(pl_render_image_mix_source(rr, &single, &target, &params,
                                               &context, &prepare_mixer));
            assert(saw_expected_allocation);
            pl_log_level_update(gpu->log, previous_level);
            expected_allocation = NULL;
            assert(initialization.num_frames == 1 && !initialization.synthesized);
            assert(initialization.result == MP_MOTION_OK);
            assert(mp_motion_get_stats(motion).analyzed_pairs == 0);
            assert(mp_motion_get_stats(motion).generated_frames == 0);
            readback(gpu, destination, actual);
            if (mp_motion_shader_supported(motion)) {
                memcpy(pixels, actual, bytes);
                const struct pl_frame_shader_mixer single_shader = {
                    .priv = &initialization, .mix = mix_motion_shader,
                };
                assert(pl_render_image_mix_source_shader(rr, &single, &target,
                                                         &params, &context,
                                                         &single_shader));
                readback(gpu, destination, actual);
                assert(!memcmp(pixels, actual, bytes));
                assert(initialization.num_frames == 1 && !initialization.synthesized);
                assert(mp_motion_get_stats(motion).analyzed_pairs == 0);
                assert(mp_motion_get_stats(motion).generated_frames == 0);
            }
            float single_maximum = 0;
            for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
                assert(isfinite(actual[i]) && isfinite(expected[i]));
                single_maximum = fmaxf(single_maximum, fabsf(actual[i] - expected[i]));
            }
            printf("Prepared single-source mode=%d delayed=%d max=%.9f\n",
                   mode, delayed, single_maximum);
            if (half) {
                // Ordinary mixing materializes FP16; prepared views retain FP32.
                // Permit only its two adjacent binary16 values, without epsilon
                // or an assumption about the backend conversion rounding mode.
                for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
                    float value = actual[i];
                    assert(fabsf(value) <= 65504);
                    float step = fabsf(value) < ldexpf(1, -14) ? ldexpf(1, -24)
                        : ldexpf(1, ilogbf(fabsf(value)) - 10);
                    float lower = floorf(value / step) * step;
                    float upper = ceilf(value / step) * step;
                    assert(expected[i] >= lower && expected[i] <= upper);
                }
            } else {
                puts("Single-source FP16 precision comparison unavailable");
            }

            for (int phase = 1; phase < 4; phase++) {
                struct motion_mix synthesis = {.motion = motion, .alpha = phase * .25f};
                struct pl_custom_frame_mixer mixer = {
                    .priv = &synthesis, .mix = mix_motion_views,
                };
                timestamps[0] = -phase * .25;
                timestamps[1] = 1 - phase * .25;
                assert(pl_render_image_mix_source(rr, &mix, &target, &params,
                                                   &context, &mixer));
                assert(synthesis.synthesized);
                readback(gpu, destination, actual);
                if (mp_motion_shader_supported(motion)) {
                    struct pl_frame_shader_mixer shader_mixer = {
                        .priv = &synthesis, .mix = mix_motion_shader,
                    };
                    synthesis.synthesized = false;
                    uint64_t before = mp_motion_get_stats(motion).generated_frames;
                    if (!mode && !delayed && phase == 1) {
                        shader_mixer.mix = reject_motion_shader;
                        expected_gpu_error =
                            "Custom frame mix failed: shader callback/output (errors=0x0)";
                        expected_gpu_errors = 0;
                        assert(!pl_render_image_mix_source_shader(rr, &mix, &target,
                                                                  &params, &context,
                                                                  &shader_mixer));
                        assert(expected_gpu_errors == 1);
                        expected_gpu_error = NULL;
                        assert(mp_motion_get_stats(motion).generated_frames == before);
                        readback(gpu, destination, expected);
                        assert(!memcmp(actual, expected, bytes));
                        shader_mixer.mix = mix_motion_shader;
                        synthesis.synthesized = false;
                    }
                    assert(pl_render_image_mix_source_shader(rr, &mix, &target,
                                                             &params, &context,
                                                             &shader_mixer));
                    assert(synthesis.synthesized);
                    assert(mp_motion_get_stats(motion).generated_frames == before);
                    mp_motion_record_shader_output(motion);
                    assert(mp_motion_get_stats(motion).generated_frames == before + 1);
                    readback(gpu, destination, expected);
                    assert(!memcmp(actual, expected, bytes));
                }
                pipeline_pattern(pixels, 2 * phase, mode & 1);
                struct pl_frame source = pq_frame(upload(gpu, pixels));
                if (!(mode & 1))
                    source.color = pl_color_space_bt709;
                uint64_t reference = ids[phase <= 2 ? 0 : 1];
                context.ids = &reference;
                assert(pl_render_image_source(oracle, &source, &target, &params, &context));
                readback(gpu, destination, expected);
                context.ids = ids;
                float maximum = 0;
                for (int y = 16; y < HEIGHT - 16; y++) {
                    for (int x = 16; x < WIDTH - 16; x++) {
                        for (int c = 0; c < 4; c++) {
                            int offset = (y * WIDTH + x) * 4 + c;
                            assert(isfinite(actual[offset]) && isfinite(expected[offset]));
                            maximum = fmaxf(maximum, fabsf(actual[offset] - expected[offset]));
                        }
                    }
                }
                printf("Prepared LP-motion pipeline mode=%d delayed=%d phase=%d max=%.9f\n",
                       mode, delayed, phase, maximum);
                fflush(stdout);
                assert(maximum < .003f);
                pl_tex_destroy(gpu, &source.planes[0].texture);
            }
            assert(mp_motion_get_stats(motion).analyzed_pairs == 1);
            assert(mp_motion_get_stats(motion).generated_frames ==
                   (mp_motion_shader_supported(motion) ? 6 : 3));
            pl_renderer_destroy(&oracle);
            pl_renderer_destroy(&rr);
            pl_source_peak_state_destroy(&state);
            mp_motion_destroy(&motion);
            for (int i = 0; i < 2; i++)
                pl_tex_destroy(gpu, &frames[i].planes[0].texture);
            pl_tex_destroy(gpu, &destination);
        }
    }
    free(pixels);
    free(actual);
    free(expected);
}

// Fixed SDR fixture replay: full-size decoded YUV420P, never peer PNG inputs.
static struct pl_frame replay_source(pl_gpu gpu, const char *path)
{
    const int w = 1280, h = 720;
    FILE *file = fopen(path, "rb");
    assert(file);
    struct pl_frame frame = {
        .num_planes = 3,
        .repr = {
            .sys = PL_COLOR_SYSTEM_BT_709,
            .levels = PL_COLOR_LEVELS_LIMITED,
            .bits = {.sample_depth = 8, .color_depth = 8},
        },
        .color = pl_color_space_bt709,
        .crop = {0, 0, w, h},
    };
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 8, 8,
                               PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
    assert(format);
    for (int i = 0; i < 3; i++) {
        int width = i ? w / 2 : w, height = i ? h / 2 : h;
        size_t bytes = (size_t) width * height;
        uint8_t *pixels = malloc(bytes);
        assert(pixels && fread(pixels, 1, bytes, file) == bytes);
        frame.planes[i] = (struct pl_plane) {
            .texture = pl_tex_create(gpu, pl_tex_params(
                .w = width, .h = height, .format = format,
                .sampleable = true, .initial_data = pixels,
            )),
            .components = 1,
            .component_mapping = {i},
        };
        free(pixels);
        assert(frame.planes[i].texture);
    }
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0);
    pl_frame_set_chroma_location(&frame, PL_CHROMA_LEFT);
    return frame;
}

static void replay_write(const char *prefix, const char *suffix,
                         const void *pixels, size_t bytes)
{
    char path[4096];
    int len = snprintf(path, sizeof(path), "%s%s", prefix, suffix);
    assert(len > 0 && len < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(pixels, 1, bytes, file) == bytes);
    assert(fclose(file) == 0);
}

static void replay_downsample(pl_gpu gpu, pl_dispatch dispatch, pl_tex native,
                              pl_tex small, uint8_t *pixels)
{
    pl_shader sh = pl_dispatch_begin(dispatch);
    assert(pl_shader_sample_bilinear(sh, pl_sample_src(
        .tex = native, .new_w = 640, .new_h = 360,
    )));
    assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
        .shader = &sh, .target = small,
    )));
    assert(pl_tex_download(gpu, pl_tex_transfer_params(.tex = small, .ptr = pixels)));
}

#if PL_API_VER >= 391
static void record_motion_stage(void *priv, const struct pl_render_info *info)
{
    if (info->stage == PL_RENDER_STAGE_MOTION)
        *(bool *)priv = true;
}
#endif

static void replay_phases(pl_gpu gpu, const char *input, bool compare_libplacebo)
{
#if PL_API_VER < 391
    (void) compare_libplacebo;
#endif
    FILE *file = fopen(input, "r");
    assert(file);
    pl_renderer rr = pl_renderer_create(gpu->log, gpu);
#if PL_API_VER >= 392
    pl_renderer flow_renderer = compare_libplacebo
        ? pl_renderer_create(gpu->log, gpu) : NULL;
    assert(!compare_libplacebo || flow_renderer);
#endif
    pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
    struct mp_motion *motion = mp_motion_create(gpu, 480);
    pl_source_peak_state state = pl_source_peak_state_create(gpu->log, gpu);
    assert(rr && dispatch && motion && state && mp_motion_shader_supported(motion));
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR |
        PL_FMT_CAP_HOST_READABLE);
    pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format && rgba8);
    pl_tex native = pl_tex_create(gpu, pl_tex_params(
        .w = 1280, .h = 720, .format = format, .renderable = true,
        .sampleable = true, .host_readable = true,
    ));
    pl_tex small = pl_tex_create(gpu, pl_tex_params(
        .w = 640, .h = 360, .format = rgba8, .renderable = true,
        .host_readable = true,
    ));
    assert(native && small);
    const size_t bytes = 1280 * 720 * 4 * sizeof(float);
    float *pixels = malloc(bytes), *reference = malloc(bytes);
    uint8_t *quantized = malloc(640 * 360 * 4);
    assert(pixels && reference && quantized);
    struct pl_frame target = pq_frame(native);
    target.color = pl_color_space_bt709;
    struct pl_render_params params = pl_render_fast_params;
    char line[16384], first[4096], second[4096], prefix[4096], extra;
    int count = 0, endpoint_sample = -1;
    double endpoint_pts0 = NAN, endpoint_pts1 = NAN;
    while (fgets(line, sizeof(line), file)) {
        double pts0, pts1, pts;
        float alpha;
        assert(sscanf(line, "%4095s %4095s %lf %lf %lf %f %4095s %c",
                      first, second, &pts0, &pts1, &pts, &alpha, prefix, &extra) == 7);
        assert(count < 10 && isfinite(pts0) && isfinite(pts1) && isfinite(pts));
        assert(pts0 < pts && pts < pts1 && isfinite(alpha) && alpha > 0 && alpha < 1);
        assert(fabs(pts - (pts0 + (pts1 - pts0) * alpha)) < 1e-7);
        printf("Replay %d: %.17g %.17g %.17g alpha=%.9g\n",
               count, pts0, pts1, pts, alpha);
        fflush(stdout);
        struct pl_frame frames[] = {
            replay_source(gpu, first), replay_source(gpu, second),
        };
        const struct pl_frame *sources[] = {&frames[0], &frames[1]};
        uint64_t ids[] = {2 * count + 1, 2 * count + 2};
        // pl_frame_mix timestamps and vsync duration use source-duration units.
        double duration = pts1 - pts0;
        double times[] = {(pts0 - pts) / duration, (pts1 - pts) / duration};
        struct pl_frame_mix mix = {
            .frames = sources, .num_frames = 2, .signatures = ids,
            .timestamps = times, .vsync_duration = 1.0 / (120 * duration),
        };
        for (int i = 0; i < 2; i++)
            assert(pl_source_peak_submit(rr, state, 1, ids[i], &frames[i], NULL) !=
                   PL_SOURCE_PEAK_ERROR);
        // Register SDR sources without requesting dynamic peak measurement.
        struct pl_source_peak_context context = {
            .state = state, .epoch = 1, .ids = ids, .current_id = ids[0],
        };
        if (pts0 != endpoint_pts0 || pts1 != endpoint_pts1) {
            // Decode/color baseline uses ordinary rendering, not endpoint synthesis.
            for (int i = 0; i < 2; i++) {
                assert(pl_render_image(rr, &frames[i], &target, &params));
                assert(!pl_renderer_get_errors(rr).errors);
                replay_downsample(gpu, dispatch, native, small, quantized);
                replay_write(prefix, i ? "-source1.rgba" : "-source0.rgba",
                             quantized, 640 * 360 * 4);
            }
            endpoint_pts0 = pts0;
            endpoint_pts1 = pts1;
            endpoint_sample = count;
        }
        float maximum = 0;
        for (int shader = 0; shader < 2; shader++) {
            struct motion_mix synthesis = {.motion = motion, .alpha = alpha};
            struct pl_custom_frame_mixer texture_mixer = {
                .priv = &synthesis, .mix = mix_motion_views,
            };
            struct pl_frame_shader_mixer shader_mixer = {
                .priv = &synthesis, .mix = mix_motion_shader,
            };
            bool ok = shader
                ? pl_render_image_mix_source_shader(rr, &mix, &target, &params,
                                                    &context, &shader_mixer)
                : pl_render_image_mix_source(rr, &mix, &target, &params,
                                             &context, &texture_mixer);
            assert(ok && synthesis.synthesized && synthesis.num_frames == 2);
            assert(synthesis.result == MP_MOTION_OK);
            assert(!pl_renderer_get_errors(rr).errors);
            assert(pl_tex_download(gpu, pl_tex_transfer_params(
                .tex = native, .ptr = pixels,
            )));
            for (size_t i = 0; i < bytes / sizeof(float); i++) {
                assert(isfinite(pixels[i]));
                if (shader)
                    maximum = fmaxf(maximum, fabsf(pixels[i] - reference[i]));
            }
            if (!shader)
                memcpy(reference, pixels, bytes);
            replay_write(prefix, shader ? "-shader.f32" : "-texture.f32", pixels, bytes);
            replay_downsample(gpu, dispatch, native, small, quantized);
            replay_write(prefix, shader ? "-shader.rgba" : "-texture.rgba",
                         quantized, 640 * 360 * 4);
        }
        assert(maximum <= 1e-5f);
#if PL_API_VER >= 391
        if (compare_libplacebo) {
            bool estimated = false;
            struct pl_render_params official = params;
            official.frame_interp_params = &pl_frame_interp_default_params;
            official.info_callback = record_motion_stage;
            official.info_priv = &estimated;
            assert(pl_render_image_mix(rr, &mix, &target, &official));
            assert(estimated && !pl_renderer_get_errors(rr).errors);
            assert(pl_tex_download(gpu, pl_tex_transfer_params(
                .tex = native, .ptr = pixels,
            )));
            for (size_t i = 0; i < bytes / sizeof(float); i++)
                assert(isfinite(pixels[i]));
            replay_write(prefix, "-libplacebo.f32", pixels, bytes);
            replay_downsample(gpu, dispatch, native, small, quantized);
            replay_write(prefix, "-libplacebo.rgba", quantized, 640 * 360 * 4);
#if PL_API_VER >= 392
            official.frame_interp_params = pl_frame_interp_params(
                .algo = PL_FRAME_INTERP_VISUALIZE,
            );
            estimated = false;
            assert(pl_render_image_mix(flow_renderer, &mix, &target, &official));
            assert(estimated && !pl_renderer_get_errors(flow_renderer).errors);
            replay_downsample(gpu, dispatch, native, small, quantized);
            replay_write(prefix, "-libplacebo-motion.rgba", quantized,
                         640 * 360 * 4);
#endif
        }
#endif
        char receipt[1024];
        int len = snprintf(receipt, sizeof(receipt),
            "{\"sample\":%d,\"pts0\":%.17g,\"pts1\":%.17g,\"output_pts\":%.17g,"
            "\"alpha\":%.9g,\"native_size\":[1280,720],\"rgba8_size\":[640,360],"
            "\"color\":\"BT709/BT1886 full RGB\",\"source\":\"YUV420P8 limited BT709 left\","
            "\"analysis_cap\":480,\"endpoint_sample\":%d,"
            "\"shader_texture_max_error\":%.9g}\n",
            count, pts0, pts1, pts, alpha, endpoint_sample, maximum);
        assert(len > 0 && len < sizeof(receipt));
        replay_write(prefix, ".json", receipt, len);
        for (int i = 0; i < 2; i++)
            for (int p = 0; p < 3; p++)
                pl_tex_destroy(gpu, &frames[i].planes[p].texture);
        count++;
    }
    assert(!ferror(file) && count == 10 && gpu_errors == 0);
    assert(fclose(file) == 0);
    free(pixels);
    free(reference);
    free(quantized);
    pl_tex_destroy(gpu, &native);
    pl_tex_destroy(gpu, &small);
    mp_motion_destroy(&motion);
    pl_dispatch_destroy(&dispatch);
    pl_source_peak_state_destroy(&state);
#if PL_API_VER >= 392
    pl_renderer_destroy(&flow_renderer);
#endif
    pl_renderer_destroy(&rr);
    puts("All 10 prepared-view replay samples passed");
}

int main(int argc, char **argv)
{
    bool compare_libplacebo = argc == 4 &&
                             !strcmp(argv[3], "--compare-libplacebo");
    bool replay = (argc == 3 || compare_libplacebo) &&
                  !strcmp(argv[1], "--replay-phases");
    if (argc != 1 && !replay) {
        fputs("Usage: gpu-motion [--replay-phases cases.txt "
              "[--compare-libplacebo]]\n", stderr);
        return 2;
    }
#if PL_API_VER < 391
    if (compare_libplacebo) {
        fputs("--compare-libplacebo requires libplacebo API 391 or newer\n",
              stderr);
        return 2;
    }
#endif
    pl_log log = pl_log_create(PL_API_VER, pl_log_params(
        .log_cb = log_message, .log_level = PL_LOG_WARN,
    ));
    pl_vulkan vulkan = pl_vulkan_create(log, NULL);
    if (!vulkan) {
        fputs("Vulkan device unavailable\n", stderr);
        return 77;
    }
    VkPhysicalDeviceProperties device;
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties) vulkan->get_proc_addr(
            vulkan->instance, "vkGetPhysicalDeviceProperties");
    assert(get_properties);
    get_properties(vulkan->phys_device, &device);
    printf("Vulkan device: %s (API %u.%u.%u)\n", device.deviceName,
           VK_VERSION_MAJOR(device.apiVersion), VK_VERSION_MINOR(device.apiVersion),
           VK_VERSION_PATCH(device.apiVersion));
    fflush(stdout);
    if (replay) {
        replay_phases(vulkan->gpu, argv[2], compare_libplacebo);
        pl_vulkan_destroy(&vulkan);
        pl_log_destroy(&log);
        return 0;
    }
    struct mp_motion *motion = mp_motion_create(vulkan->gpu, 128);
    capabilities(vulkan->gpu);
    motion_queue_cadence(vulkan->gpu);
    evidence_bits(vulkan->gpu);
    dense_pair_equivalence(vulkan->gpu, false);
    dense_pair_equivalence(vulkan->gpu, true);
    reconstruction_equivalence(vulkan->gpu);
    refine_zero_motion(vulkan->gpu);
    assert(motion);
    subpixel_matching(vulkan->gpu);
    translation_selection(vulkan->gpu);
    pyramid_reuse(vulkan->gpu);
    padding_sizes(vulkan->gpu);
    translated_pair(vulkan->gpu, motion);
    affine_pair(vulkan->gpu, motion, 129, 97, 1);
    foreground_pair(vulkan->gpu, motion, WIDTH, HEIGHT, 1);
    static_and_cut(vulkan->gpu, motion);
    straight_edge(vulkan->gpu, motion, 1, false, 8);
    straight_edge(vulkan->gpu, motion, 1, true, 8);
    straight_edge(vulkan->gpu, motion, 4, false, 8);
    straight_edge(vulkan->gpu, motion, 4, true, 8);
    straight_edge(vulkan->gpu, motion, 1, false, 2);
    straight_edge(vulkan->gpu, motion, 1, true, 2);
    straight_edge(vulkan->gpu, motion, 4, false, 2);
    straight_edge(vulkan->gpu, motion, 4, true, 2);
    warp_policy(vulkan->gpu);
    synthesis_policy(vulkan->gpu);
    scene_transitions(vulkan->gpu, motion, true);
    scene_transitions(vulkan->gpu, motion, false);
    periodic_scene_translation(vulkan->gpu);
    capture_ownership(vulkan->gpu, motion);
    color_roundtrip(vulkan->gpu, motion);
    hdr_frames(vulkan->gpu, motion);
    prepared_views(vulkan->gpu);
    single_presentation(vulkan->gpu);
    prepared_pipeline(vulkan->gpu);
    mp_motion_destroy(&motion);
    assert(!motion);
    motion = mp_motion_create(vulkan->gpu, 480);
    assert(motion);
    affine_pair(vulkan->gpu, motion, 1920, 1080, 8);
    foreground_pair(vulkan->gpu, motion, 1920, 1080, 4);
    for (int variant = 0; variant < 32; variant++) {
        diagonal_edge(vulkan->gpu, motion, false, variant);
        diagonal_edge(vulkan->gpu, motion, true, variant);
    }
    synthesis_sampling(vulkan->gpu);
    mp_motion_destroy(&motion);
    pl_vulkan_destroy(&vulkan);
    assert(gpu_errors == 0);
    pl_log_destroy(&log);
    puts("GPU motion interpolation: passed");
    return 0;
}
