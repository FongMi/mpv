/* SPDX-License-Identifier: LGPL-2.1-or-later */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/vulkan.h>

#include "gpu_hwdec_copy.h"

static int gpu_errors;

static void log_message(void *priv, enum pl_log_level level, const char *message)
{
    (void)priv;
    if (level <= PL_LOG_ERR)
        gpu_errors++;
    fprintf(stderr, "%s\n", message);
}

static void readback(pl_gpu gpu, pl_dispatch dispatch, pl_tex source,
                     float *pixels)
{
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
                                PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    assert(format);
    pl_tex target = pl_tex_create(gpu, pl_tex_params(
        .w = source->params.w, .h = source->params.h, .format = format,
        .renderable = true, .host_readable = true));
    assert(target);
    pl_shader shader = pl_dispatch_begin(dispatch);
    assert(pl_shader_sample_nearest(shader, pl_sample_src(.tex = source)));
    assert(pl_dispatch_finish(dispatch, pl_dispatch_params(
        .shader = &shader, .target = target)));
    assert(pl_tex_download(gpu, pl_tex_transfer_params(.tex = target, .ptr = pixels)));
    pl_tex_destroy(gpu, &target);
}

static void rejected_uint32(pl_gpu gpu, pl_dispatch dispatch)
{
    pl_fmt format = pl_find_fmt(gpu, PL_FMT_UINT, 1, 32, 32,
                                PL_FMT_CAP_SAMPLEABLE);
    assert(format);
    const uint32_t pixel = UINT32_MAX;
    pl_tex source = pl_tex_create(gpu, pl_tex_params(
        .w = 1, .h = 1, .format = format,
        .sampleable = true, .initial_data = &pixel));
    assert(source);
    pl_tex output = source;
    assert(!copy_hwdec_plane(gpu, dispatch, source, 1, &output));
    assert(output == source);
    assert(!copy_hwdec_plane(gpu, dispatch, source, 0, &output));
    assert(output == source);
    assert(!copy_hwdec_plane(gpu, dispatch, source, 5, &output));
    assert(output == source);
    assert(!copy_hwdec_plane(gpu, dispatch, NULL, 1, &output));
    assert(output == source);
    pl_tex_destroy(gpu, &source);
    format = pl_find_fmt(gpu, PL_FMT_SINT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
    assert(format);
    const int32_t signed_pixel = -1;
    source = pl_tex_create(gpu, pl_tex_params(
        .w = 1, .h = 1, .format = format,
        .sampleable = true, .initial_data = &signed_pixel));
    assert(source);
    output = source;
    assert(!copy_hwdec_plane(gpu, dispatch, source, 1, &output));
    assert(output == source);
    pl_tex_destroy(gpu, &source);
    puts("UINT32/SINT/invalid source reject without changing output");
}

static double sample_value(int index, enum pl_fmt_type type, int bits)
{
    if (type == PL_FMT_FLOAT) {
        const float values[] = {-2.0f, -0.25f, 0, 0.125f, 1, 1.5f, 32};
        return values[index % 7];
    }
    if (type == PL_FMT_SNORM) {
        const int values[] = {-32768, -32767, -1, 0, 1, 32767};
        return values[index % 6];
    }
    unsigned maximum = (1u << bits) - 1;
    const unsigned values[] = {0, 1, maximum / 2, maximum - 1, maximum};
    return values[index % 5];
}

static bool copy_case(pl_gpu gpu, pl_dispatch dispatch, enum pl_fmt_type type,
                      int bits, int components, int width, int height)
{
    pl_fmt format = pl_find_fmt(gpu, type, components, bits, bits,
                                PL_FMT_CAP_SAMPLEABLE);
    // Three-component hardware planes may be exposed through RGBA storage.
    if (!format && components == 3)
        format = pl_find_fmt(gpu, type, 4, bits, bits, PL_FMT_CAP_SAMPLEABLE);
    if (!format || format->num_components < components ||
        format->texel_size != (size_t)format->num_components * bits / 8)
    {
        printf("SKIP type=%d bits=%d components=%d: packed format unavailable\n",
               type, bits, components);
        return false;
    }
    for (int c = 0; c < components; c++) {
        if (format->component_depth[c] != bits || format->host_bits[c] != bits)
            return false;
    }
    int source_components = format->num_components;
    size_t count = (size_t)width * height * source_components;
    void *pixels = calloc(count, bits / 8);
    assert(pixels);
    for (size_t i = 0; i < count; i++) {
        double value = sample_value((int)i, type, bits);
        if (type == PL_FMT_FLOAT)
            ((float *)pixels)[i] = (float)value;
        else if (type == PL_FMT_SNORM)
            ((int16_t *)pixels)[i] = (int16_t)value;
        else if (bits == 8)
            ((uint8_t *)pixels)[i] = (uint8_t)value;
        else
            ((uint16_t *)pixels)[i] = (uint16_t)value;
    }
    pl_tex source = pl_tex_create(gpu, pl_tex_params(
        .w = width, .h = height, .format = format,
        .sampleable = true, .host_writable = true, .initial_data = pixels));
    assert(source);
    pl_tex snapshot = NULL;
    assert(copy_hwdec_plane(gpu, dispatch, source, components, &snapshot));
    assert(snapshot && snapshot != source);
    assert(snapshot->params.w == width && snapshot->params.h == height);
    assert(snapshot->params.format->type == PL_FMT_FLOAT);
    for (int c = 0; c < components; c++)
        assert(snapshot->params.format->component_depth[c] == 32);

    // The snapshot must survive mutation and destruction of the source storage.
    memset(pixels, 0, count * bits / 8);
    assert(pl_tex_upload(gpu, pl_tex_transfer_params(.tex = source, .ptr = pixels)));
    pl_tex_destroy(gpu, &source);
    size_t output_components = 4;
    float *output = calloc((size_t)width * height * output_components, sizeof(float));
    assert(output);
    readback(gpu, dispatch, snapshot, output);
    double maximum = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            for (int c = 0; c < components; c++) {
                int index = (y * width + x) * source_components + c;
                double expected = sample_value(index, type, bits);
                if (type == PL_FMT_SNORM)
                    expected = fmax(-1, expected / 32767);
                else if (type != PL_FMT_FLOAT)
                    expected /= (1u << bits) - 1;
                float actual = output[((size_t)y * width + x) * output_components + c];
                assert(isfinite(actual));
                double error = fabs(actual - expected);
                maximum = fmax(maximum, error);
                assert(error < 0.000001);
            }
        }
    }
    printf("copy type=%d bits=%d components=%d stored=%d size=%dx%d max=%.9g\n",
           type, bits, components, source_components, width, height, maximum);
    fflush(stdout);
    pl_tex_destroy(gpu, &snapshot);
    free(pixels);
    free(output);
    return true;
}

int main(void)
{
    pl_log log = pl_log_create(PL_API_VER, pl_log_params(
        .log_cb = log_message, .log_level = PL_LOG_WARN));
    pl_vulkan vulkan = pl_vulkan_create(log, NULL);
    if (!vulkan) {
        fputs("Vulkan device unavailable\n", stderr);
        pl_log_destroy(&log);
        return 77;
    }
    pl_dispatch dispatch = pl_dispatch_create(log, vulkan->gpu);
    assert(dispatch);
    const struct { enum pl_fmt_type type; int bits; } formats[] = {
        {PL_FMT_UNORM, 8}, {PL_FMT_UNORM, 16}, {PL_FMT_SNORM, 16},
        {PL_FMT_FLOAT, 32},
        {PL_FMT_UINT, 8}, {PL_FMT_UINT, 16},
    };
    const int sizes[][2] = {{1, 1}, {1, 7}, {13, 9}, {32, 16}};
    int total = 0;
    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        int cases = 0;
        for (int components = 1; components <= 4; components++) {
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                cases += copy_case(vulkan->gpu, dispatch, formats[f].type,
                                   formats[f].bits, components,
                                   sizes[s][0], sizes[s][1]);
            }
        }
        assert(cases >= 4); // Every required format family must execute on GPU.
        total += cases;
    }
    rejected_uint32(vulkan->gpu, dispatch);
    pl_dispatch_destroy(&dispatch);
    pl_vulkan_destroy(&vulkan);
    assert(!gpu_errors);
    pl_log_destroy(&log);
    printf("GPU hardware plane snapshots: %d cases passed\n", total);
    return 0;
}
