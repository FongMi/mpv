/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gamut_mapping.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/tone_mapping.h>

#include "motion.h"

#ifndef PL_HAVE_HDR_LINEAR
#error "Motion HDR requires a matching libplacebo HDR-linear SDK"
#endif
#ifndef PL_HAVE_FRAME_SHADER_MIXER
#error "Motion views require a matching libplacebo shader-mixer SDK"
#endif

#ifndef PL_HAVE_DISPATCH_PREPARED
#error "Motion requires a matching prepared-dispatch SDK"
#endif

#define LEVELS 3
#define MATCH_RADIUS 6
#define MATCH_GROUP_SIZE 64
#define MATCH_SHARED_BYTES ((49 + 361) * 16 + (169 + 2 * MATCH_GROUP_SIZE) * 4)
#define GLSL(...) #__VA_ARGS__

enum motion_input_kind {
    MOTION_INPUT_RAW,
    MOTION_INPUT_CAPTURED,
    MOTION_INPUT_VIEW,
};

struct mp_motion {
    pl_gpu gpu;
    pl_fmt format, hdr_format, position_format;
    pl_dispatch dispatch;
    pl_renderer renderer;
    pl_tex images[LEVELS][2];
    pl_tex vectors[LEVELS][2];
    pl_tex evidence[2];
    pl_tex reconstructed[2];
    pl_tex warps[2];
    pl_tex scene;
    pl_tex translation;
    pl_tex translation_costs;
    pl_tex result;
    pl_dispatch_prepared pyramid_pass[LEVELS][2];
    pl_dispatch_prepared matching_pass[LEVELS][2];
    pl_dispatch_prepared evidence_pass, reconstruction_pass[2], warp_pass[2];
    pl_dispatch_prepared translation_pass, selection_pass, scene_pass;
    pl_fmt prepared_format[2];
    uint64_t prepared_sampler[2];
    int prepared_width, prepared_height;
    int max_dimension;
    int source_width, source_height;
    uint64_t ids[2];
    enum motion_input_kind input_kind;
    bool linear_hdr;
    uint64_t capture_id;
    bool pair_valid;
    bool result_valid, warp_valid;
    float last_alpha, warp_alpha;
    struct mp_motion_stats stats;
    int timing_stage;
};

static const char sample_header[] = GLSL(
    vec4 sample_rgba(sampler2D image, vec2 p, ivec2 size)
    {
        p = clamp(p, vec2(0), vec2(size - 1));
        ivec2 lo = ivec2(floor(p));
        ivec2 hi = min(lo + ivec2(1), size - 1);
        vec2 fraction = p - vec2(lo);
        vec4 a = texelFetch(image, lo, 0);
        vec4 b = texelFetch(image, ivec2(hi.x, lo.y), 0);
        vec4 c = texelFetch(image, ivec2(lo.x, hi.y), 0);
        vec4 d = texelFetch(image, hi, 0);
        return mix(mix(a, b, fraction.x), mix(c, d, fraction.x), fraction.y);
    }

    vec4 sample_rgba(sampler2D image, vec2 p)
    {
        return sample_rgba(image, p, textureSize(image, 0));
    }

    vec3 sample_color(sampler2D image, vec2 p)
    {
        return sample_rgba(image, p).rgb;
    }

    vec3 hdr_encode(vec3 linear_rgb)
    {
        vec3 p = pow(max(linear_rgb * 0.0203, 0.0),
                     vec3(2610.0 / 16384.0));
        // log1p(-q) avoids rounding a near-one quotient before the PQ power.
        // For 0..10000 nits q is in [0, 21/128]. Eight terms bound the
        // real-arithmetic PQ relative truncation error below 9.1e-7.
        vec3 q = (1.0 - 3424.0 / 4096.0) * (vec3(1.0) - p) /
                 (vec3(1.0) + (2392.0 / 128.0) * p);
        vec3 log_ratio = -q * (vec3(1.0) + q * (vec3(1.0 / 2.0) +
            q * (vec3(1.0 / 3.0) + q * (vec3(1.0 / 4.0) +
            q * (vec3(1.0 / 5.0) + q * (vec3(1.0 / 6.0) +
            q * (vec3(1.0 / 7.0) + q * vec3(1.0 / 8.0))))))));
        return exp((2523.0 / 32.0) * log_ratio);
    }

    // Only the metric is bounded; synthesis remains linear.
    vec3 hdr_proxy(vec3 linear_rgb)
    {
        return hdr_encode(clamp(linear_rgb, 0.0, 10000.0 / 203.0));
    }

    // Keep FP32 ordering scores exact in the supported RGBA16F format.
    vec4 pack_evidence(float evidence)
    {
        uint bits = floatBitsToUint(evidence);
        return vec4(uvec4(bits, bits >> 8, bits >> 16, bits >> 24) & uvec4(255));
    }

    float unpack_evidence(vec4 evidence)
    {
        uvec4 bytes = uvec4(evidence);
        uint bits = bytes.x | (bytes.y << 8) | (bytes.z << 16) |
                    (bytes.w << 24);
        return uintBitsToFloat(bits);
    }
);

static const char match_header[] = GLSL(
    shared vec3 reference_colors[49];
    shared vec3 search_colors[361];
    shared float candidate_cost[169];
    shared int best_index[64];
    shared float second_cost[64];

    int motion_distance(int index, int side, int radius)
    {
        return abs(index % side - radius) + abs(index / side - radius);
    }

    void select_candidate(int index, int side, int radius,
                          inout int best, inout float runner_up)
    {
        float cost = candidate_cost[index];
        float best_cost = candidate_cost[best];
        if (cost < best_cost || (cost == best_cost &&
            (motion_distance(index, side, radius) < motion_distance(best, side, radius) ||
             (motion_distance(index, side, radius) == motion_distance(best, side, radius) &&
              index < best)))) {
            runner_up = best_cost;
            best = index;
        } else {
            runner_up = min(runner_up, cost);
        }
    }

    float difference(sampler2D second, vec2 p, vec2 delta,
                     float limit)
    {
        float sum = 0.0;
        for (int y = -3; y <= 3; y++) {
            for (int x = -3; x <= 3; x++) {
                vec2 q = p + vec2(x, y);
                vec3 a = reference_colors[(y + 3) * 7 + x + 3];
                vec3 b = sample_color(second, q + delta);
                vec3 d = abs(a - b);
                sum += dot(d, vec3(0.25)) +
                       0.25 * abs(dot(a - b, vec3(0.2126, 0.7152, 0.0722)));
                if (sum > limit * 49.0)
                    return sum / 49.0;
            }
        }
        return sum / 49.0;
    }

    // Adding an integer offset is exact when the fractional part is
    // representable at the largest magnitude in the entire search interval.
    bool exact_offsets(float p, int extent)
    {
        int exponent;
        frexp(abs(p) + float(extent), exponent);
        float step = ldexp(1.0, exponent - 24);
        return p == round(p / step) * step;
    }

    float cached_difference(ivec2 start, int width)
    {
        float sum = 0.0;
        for (int y = 0; y < 7; y++) {
            for (int x = 0; x < 7; x++) {
                vec3 a = reference_colors[y * 7 + x];
                vec3 b = search_colors[(start.y + y) * width + start.x + x];
                vec3 d = abs(a - b);
                sum += dot(d, vec3(0.25)) +
                       0.25 * abs(dot(a - b, vec3(0.2126, 0.7152, 0.0722)));
            }
        }
        return sum / 49.0;
    }

    float axis_cost(sampler2D second, vec2 p, ivec2 delta, ivec2 size, int radius)
    {
        vec2 q = p + vec2(delta);
        if (all(lessThanEqual(abs(delta), ivec2(radius))) &&
            all(greaterThanEqual(q, vec2(0))) &&
            all(lessThanEqual(q, vec2(size - 1)))) {
            int side = 2 * radius + 1;
            ivec2 candidate = delta + radius;
            return candidate_cost[candidate.y * side + candidate.x];
        }
        return difference(second, p, vec2(delta), 1e20);
    }

    float refine(float left, float center, float right)
    {
        float curvature = left + right - 2.0 * center;
        return curvature > 0.000001
             ? clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0.0;
    }
);

static const char match_body[] = GLSL(
    vec2 uv = (vec2(gl_WorkGroupID.xy) + 0.5) / vec2(settings.xy);
    ivec2 size = textureSize(first, 0);
    vec2 p = uv * vec2(size) - 0.5;
    int lane = int(gl_LocalInvocationIndex);
    if (lane < 49)
        reference_colors[lane] = sample_color(first,
            p + vec2(lane % 7 - 3, lane / 7 - 3));
    int side = settings.z * 2 + 1;
    int width = side + 6;
    int extent = settings.z + 3;
    bool cache = exact_offsets(p.x, extent) && exact_offsets(p.y, extent);
    if (cache) {
        for (int index = lane; index < width * width; index += 64) {
            ivec2 offset = ivec2(index % width, index / width) - extent;
            search_colors[index] = sample_color(second, p + vec2(offset));
        }
    }
    barrier();
    for (int index = lane; index < side * side; index += 64) {
        ivec2 delta = ivec2(index % side, index / side) - settings.zz;
        vec2 q = p + vec2(delta);
        candidate_cost[index] = any(lessThan(q, vec2(0))) ||
                               any(greaterThan(q, vec2(size - 1)))
                             ? 1e20
                             : (cache ? cached_difference(delta + settings.zz, width)
                                      : difference(second, p, vec2(delta), 1e20));
    }
    barrier();
    // Sort by cost, distance, then scan order while retaining the runner-up.
    int index = lane;
    float runner_up = 1e20;
    if (lane < side * side) {
        for (int next = lane + 64; next < side * side; next += 64)
            select_candidate(next, side, settings.z, index, runner_up);
    } else {
        index = 169;
    }
    best_index[lane] = index;
    second_cost[lane] = runner_up;
    barrier();
    for (int stride = 32; stride > 0; stride /= 2) {
        if (lane < stride && best_index[lane + stride] < 169) {
            int left = best_index[lane];
            float other = second_cost[lane];
            if (left < 169) {
                select_candidate(best_index[lane + stride], side, settings.z,
                                 left, other);
            } else {
                left = best_index[lane + stride];
            }
            best_index[lane] = left;
            second_cost[lane] = min(other, second_cost[lane + stride]);
        }
        barrier();
    }
    if (lane == 0) {
        int index = best_index[0];
        ivec2 best_vector = ivec2(index % side, index / side) - settings.zz;
        float best = candidate_cost[index];
        float runner_up = second_cost[0];
        vec2 vector = vec2(best_vector);
        if (best > 0.000001) {
            vec2 subpixel;
            subpixel.x = refine(
                axis_cost(second, p, best_vector - ivec2(1, 0), size, settings.z), best,
                axis_cost(second, p, best_vector + ivec2(1, 0), size, settings.z));
            subpixel.y = refine(
                axis_cost(second, p, best_vector - ivec2(0, 1), size, settings.z), best,
                axis_cost(second, p, best_vector + ivec2(0, 1), size, settings.z));
            vec2 candidate = clamp(vector + subpixel, -p, vec2(size - 1) - p);
            float cost = difference(second, p, candidate, 1e20);
            if (cost < best) {
                vector = candidate;
                best = cost;
            }
        }
        float uniqueness = clamp((runner_up - best) / max(runner_up, 0.000001),
                                 0.0, 1.0);
        imageStore(out_image, ivec2(gl_WorkGroupID.xy),
                   vec4(vector * motion_scale, best, uniqueness));
    }
);

// All frame-wide hypotheses use the same in-bounds support. Local reconstruction
// can then resolve repeated patches without assuming every surface moves alike.
static const char translation_cost_body[] = GLSL(
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(pixel, 2 * radius + 1))) {
        ivec2 size = textureSize(first, 0);
        vec2 vector = vec2(pixel - radius);
        float cost = 0.0;
        for (int index = 0; index < 64; index++) {
            vec2 p = vec2(radius) + (vec2(index % 8, index / 8) + 0.5) / 8.0 *
                     vec2(size - 1 - 2 * radius);
            vec3 error = sample_color(first, p) - sample_color(second, p + vector);
            cost += dot(abs(error), vec3(0.25)) +
                    0.25 * abs(dot(error, vec3(0.2126, 0.7152, 0.0722)));
        }
        color = pack_evidence(cost / 64.0);
    }
);

static const char translation_select_header[] = GLSL(
    shared vec4 minima[64];

    bool preferred(float cost, int index, float best, int best_index, ivec2 size)
    {
        ivec2 radius = (size - 1) / 2;
        vec2 vector = vec2(ivec2(index % size.x, index / size.x) - radius);
        vec2 best_vector = vec2(ivec2(best_index % size.x,
                                    best_index / size.x) - radius);
        float norm = dot(vector, vector);
        float best_norm = dot(best_vector, best_vector);
        return cost < best || (cost == best &&
               (norm < best_norm || (norm == best_norm && index < best_index)));
    }
);

static const char translation_select_body[] = GLSL(
    ivec2 size = textureSize(costs, 0);
    ivec2 radius = (size - 1) / 2;
    int lane = int(gl_LocalInvocationIndex);
    int best_index = radius.y * size.x + radius.x;
    float best = 1e20;
    float runner_up = 1e20;
    for (int index = lane; index < size.x * size.y; index += 64) {
        float cost = unpack_evidence(texelFetch(costs,
                            ivec2(index % size.x, index / size.x), 0));
        if (preferred(cost, index, best, best_index, size)) {
            runner_up = best;
            best = cost;
            best_index = index;
        } else {
            runner_up = min(runner_up, cost);
        }
    }
    // The runner-up can share a lane with the winner; keep both until reduction.
    minima[lane] = vec4(best, float(best_index), runner_up, 0);
    barrier();
    if (lane == 0) {
        best = 1e20;
        runner_up = 1e20;
        for (int index = 0; index < 64; index++) {
            vec4 value = minima[index];
            if (preferred(value.x, int(value.y), best, best_index, size)) {
                runner_up = min(runner_up, best);
                best = value.x;
                best_index = int(value.y);
            } else {
                runner_up = min(runner_up, value.x);
            }
            runner_up = min(runner_up, value.z);
        }
        float zero = unpack_evidence(texelFetch(costs, radius, 0));
        float uniqueness = clamp((runner_up - best - 0.000001) /
                                 max(runner_up, 0.000001), 0.0, 1.0);
        ivec2 vector = ivec2(best_index % size.x, best_index / size.x) - radius;
        imageStore(out_image, ivec2(0),
                   vec4(vector, best, best < zero ? uniqueness : 0.0));
    }
);

static const char refine_header[] = GLSL(
    // The first patch is identical for every parent, LK and axis candidate.
    vec3 reference_colors[16];

    // Central differences stay inside the matching patch extent.
    vec3 patch_tensor(sampler2D first, vec2 p)
    {
        float xx = 0.0, xy = 0.0, yy = 0.0;
        for (int y = -2; y <= 2; y++) {
            for (int x = -2; x <= 2; x++) {
                vec2 q = p + vec2(x, y);
                vec3 dx = sample_color(first, q + vec2(1, 0)) -
                          sample_color(first, q - vec2(1, 0));
                vec3 dy = sample_color(first, q + vec2(0, 1)) -
                          sample_color(first, q - vec2(0, 1));
                xx += dot(dx, dx);
                xy += dot(dx, dy);
                yy += dot(dy, dy);
            }
        }
        return vec3(xx, xy, yy);
    }

    float predictor_strength(sampler2D image, vec2 p)
    {
        vec3 center = sample_color(image, p);
        vec3 contrast = max(abs(center - sample_color(image, p + vec2(6, 0))),
                            abs(center - sample_color(image, p - vec2(6, 0))));
        contrast = max(contrast, abs(center - sample_color(image, p + vec2(0, 6))));
        contrast = max(contrast, abs(center - sample_color(image, p - vec2(0, 6))));
        return max(contrast.r, max(contrast.g, contrast.b));
    }

    vec2 dense_pair_cost(sampler2D first, sampler2D second, vec2 p, vec2 vector)
    {
        vec2 cost = vec2(0);
        for (int y = -3; y <= 3; y++) {
            for (int x = -3; x <= 3; x++) {
                vec2 q = p + vec2(x, y);
                vec3 source = sample_color(first, q);
                vec3 zero_error = source - sample_color(second, q);
                vec3 motion_error = source - sample_color(second, q + vector);
                cost.x += dot(abs(zero_error), vec3(0.25)) +
                    0.25 * abs(dot(zero_error, vec3(0.2126, 0.7152, 0.0722)));
                cost.y += dot(abs(motion_error), vec3(0.25)) +
                    0.25 * abs(dot(motion_error, vec3(0.2126, 0.7152, 0.0722)));
            }
        }
        return cost / 49.0;
    }

    vec3 bidirectional_cost(sampler2D first, sampler2D second, vec2 p,
                            vec2 forward, vec2 backward)
    {
        vec3 cost = vec3(0.0);
        bool use_forward = any(notEqual(forward, vec2(0)));
        bool use_backward = any(notEqual(backward, vec2(0)));
        for (int y = -3; y <= 3; y++) {
            for (int x = -3; x <= 3; x++) {
                vec2 q = p + vec2(x, y);
                vec3 a = sample_color(first, q);
                vec3 b = sample_color(second, q);
                vec3 error = a - b;
                cost.x += dot(abs(error), vec3(0.25)) +
                    0.25 * abs(dot(error, vec3(0.2126, 0.7152, 0.0722)));
                if (use_forward) {
                    error = a - sample_color(second, q + forward);
                    cost.y += dot(abs(error), vec3(0.25)) +
                        0.25 * abs(dot(error, vec3(0.2126, 0.7152, 0.0722)));
                }
                if (use_backward) {
                    error = b - sample_color(first, q + backward);
                    cost.z += dot(abs(error), vec3(0.25)) +
                        0.25 * abs(dot(error, vec3(0.2126, 0.7152, 0.0722)));
                }
            }
        }
        return cost / 49.0;
    }

    float dense_cost(sampler2D first, sampler2D second, vec2 p, vec2 vector,
                     float limit)
    {
        float cost = 0.0;
        for (int y = -3; y <= 3; y++) {
            for (int x = -3; x <= 3; x++) {
                vec2 q = p + vec2(x, y);
                vec3 error = sample_color(first, q) -
                             sample_color(second, q + vector);
                cost += dot(abs(error), vec3(0.25)) +
                        0.25 * abs(dot(error, vec3(0.2126, 0.7152, 0.0722)));
                if (cost / 49.0 > limit)
                    return cost / 49.0;
            }
        }
        return cost / 49.0;
    }

    float patch_cost(sampler2D second, vec2 p, vec2 vector, float limit)
    {
        float cost = 0.0;
        for (int y = -3; y <= 3; y += 2) {
            for (int x = -3; x <= 3; x += 2) {
                vec2 q = p + vec2(x, y);
                vec3 error = reference_colors[((y + 3) / 2) * 4 + (x + 3) / 2] -
                             sample_color(second, q + vector);
                cost += dot(abs(error), vec3(0.25)) +
                        0.25 * abs(dot(error, vec3(0.2126, 0.7152, 0.0722)));
            }
            // Every remaining tap contributes a non-negative residual.
            if (cost / 16.0 > limit)
                return cost / 16.0;
        }
        return cost / 16.0;
    }

    float patch_cost(sampler2D second, vec2 p, vec2 vector)
    {
        return patch_cost(second, p, vector, 1e20);
    }
);

// Select observed parent vectors before refinement. A flat child patch can
// use a neighbor supported by visible structure, without averaging motions
// across independent surfaces into a predictor belonging to neither.
static const char refine_body[] = GLSL(
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(pixel, settings.xy))) {
        vec2 uv = (vec2(gl_GlobalInvocationID.xy) + 0.5) / vec2(settings.xy);
        vec2 size = vec2(textureSize(first, 0));
        vec2 p = uv * size - 0.5;
        for (int y = -3; y <= 3; y += 2) {
            for (int x = -3; x <= 3; x += 2)
                reference_colors[((y + 3) / 2) * 4 + (x + 3) / 2] =
                    sample_color(first, p + vec2(x, y));
        }
        vec3 tensor = vec3(0);
        bool tensor_valid = false;
        ivec2 parent_size = textureSize(parent, 0);
        ivec2 nearest = clamp(ivec2(uv * vec2(parent_size)), ivec2(0), parent_size - 1);
        vec4 inherited = texelFetch(parent, nearest, 0);
        vec2 seed = clamp(inherited.xy / motion_scale, -p, size - 1.0 - p);
        float best = patch_cost(second, p, seed);
        float strength = predictor_strength(first,
            (vec2(nearest) + 0.5) / vec2(parent_size) * size - 0.5);
        ivec2 base = ivec2(floor(uv * vec2(parent_size) - 0.5));
        // Include one parent ring to carry observed motion across flat support.
        for (int y = -1; y <= 2; y++) {
            for (int x = -1; x <= 2; x++) {
                ivec2 q = clamp(base + ivec2(x, y), ivec2(0), parent_size - 1);
                vec4 candidate = texelFetch(parent, q, 0);
                vec2 vector = clamp(candidate.xy / motion_scale, -p, size - 1.0 - p);
                if (all(equal(vector, seed)))
                    continue;
                float cost = patch_cost(second, p, vector, best);
                if (cost > best)
                    continue;
                float support = predictor_strength(first,
                    (vec2(q) + 0.5) / vec2(parent_size) * size - 0.5);
                bool preferred = support > strength;
                // Flat support inherits structure. Observed ties retain the shorter
                // normal displacement instead of jumping to a repeated edge.
                if (cost == best) {
                    if (!tensor_valid) {
                        tensor = patch_tensor(first, p);
                        tensor_valid = true;
                    }
                    if (tensor.x + tensor.z > 0.000001) {
                        vec2 normal = tensor.x > tensor.z
                            ? vec2(1, tensor.y / tensor.x)
                            : vec2(tensor.y / tensor.z, 1);
                        float candidate_motion = abs(dot(normal, vector));
                        float seed_motion = abs(dot(normal, seed));
                        preferred = candidate_motion < seed_motion ||
                            (candidate_motion == seed_motion && support > strength);
                    }
                }
                if (cost < best || (cost == best && preferred)) {
                    best = cost;
                    strength = support;
                    seed = vector;
                    inherited = candidate;
                }
            }
        }
        vec2 vector = seed;
        float residual = 0.0;
        for (int iteration = 0; iteration < 3 && best > 0.000001; iteration++) {
            residual = 0.0;
            float xx = 0.000001;
            float xy = 0.0;
            float yy = 0.000001;
            vec2 rhs = vec2(0.0);
            for (int y = -3; y <= 3; y += 2) {
                for (int x = -3; x <= 3; x += 2) {
                    vec2 q = p + vec2(x, y);
                    vec2 position = q + vector;
                    vec3 error = sample_color(second, position) -
                                 reference_colors[((y + 3) / 2) * 4 + (x + 3) / 2];
                    residual += dot(abs(error), vec3(1.0 / 3.0));
                    vec3 dx = 0.5 * (sample_color(second, position + vec2(1, 0)) -
                                      sample_color(second, position - vec2(1, 0)));
                    vec3 dy = 0.5 * (sample_color(second, position + vec2(0, 1)) -
                                      sample_color(second, position - vec2(0, 1)));
                    xx += dot(dx, dx);
                    xy += dot(dx, dy);
                    yy += dot(dy, dy);
                    rhs += vec2(dot(dx, error), dot(dy, error));
                }
            }
            if (residual < 0.000001)
                break;
            float determinant = xx * yy - xy * xy;
            if (determinant <= 0.000000000001)
                break;
            vec2 step = vec2(yy * rhs.x - xy * rhs.y,
                              xx * rhs.y - xy * rhs.x) / determinant;
            vec2 next = clamp(vector - step, seed - vec2(settings.z),
                               seed + vec2(settings.z));
            next = clamp(next, -p, size - 1.0 - p);
            if (any(isnan(next)) || any(isinf(next)))
                break;
            float change = length(next - vector);
            vector = next;
            if (change < 0.001)
                break;
        }
        residual = patch_cost(second, p, vector);
        // Exact zero motion is already the unique closest zero-cost candidate.
        if (any(notEqual(vector, vec2(0))) || residual != 0.0) {
            // Choose an equivalent lattice representative before interpolation mixes
            // tangentially ambiguous vectors. Sampled edges need not produce an
            // exactly rank-one structure tensor.
            if (!tensor_valid)
                tensor = patch_tensor(first, p);
            float xx = tensor.x, xy = tensor.y, yy = tensor.z;
            float trace = xx + yy;
            if (trace > 0.000001) {
                vec2 axis = xx > yy ? vec2(1, 0) : vec2(0, 1);
                float projected = xx > yy ? vector.x + xy / xx * vector.y
                                          : vector.y + xy / yy * vector.x;
                // Bound acceptance by the original minimum, then rank candidates.
                float limit = residual + 0.000001;
                float lowest = 1e20;
                float distance = 1e20;
                float magnitude = 1e20;
                vec2 selected = vector;
                bool unique = false;
                for (int offset = -1; offset <= 1; offset++) {
                    vec2 candidate = clamp(axis * (round(projected) + float(offset)),
                                           -p, size - 1.0 - p);
                    float cost = patch_cost(second, p, candidate);
                    float delta = abs(dot(candidate, axis) - projected);
                    float norm = dot(candidate, candidate);
                    if (cost > limit)
                        continue;
                    if (cost < lowest || (cost == lowest &&
                        (delta < distance ||
                         (delta == distance && norm < magnitude)))) {
                        lowest = cost;
                        distance = delta;
                        magnitude = norm;
                        selected = candidate;
                        unique = true;
                    } else if (cost == lowest && delta == distance &&
                               norm == magnitude &&
                               any(notEqual(candidate, selected))) {
                        unique = false;
                    }
                }
                if (unique) {
                    vector = selected;
                    residual = lowest;
                }
            }
        }
        // Dense support can observe motion missed by central-difference tensors.
        // Resolve equivalent axis vectors without increasing the original cost.
        if (all(equal(motion_scale, vec2(1))) && any(notEqual(vector, vec2(0)))) {
            vec2 cost = dense_pair_cost(first, second, p, vector);
            if (cost.x < cost.y) {
                // The inherited match confidence does not describe zero motion.
                vector = vec2(0.0);
                residual = patch_cost(second, p, vector);
                inherited.w = 0.0;
            } else if (cost.x > cost.y) {
                float limit = cost.y + 0.000001;
                vec2 original = vector;
                float magnitude = dot(vector, vector);
                float distance = 0.0;
                for (int index = 0; index < 2 * (2 * settings.z + 1); index++) {
                    int offset = index % (2 * settings.z + 1) - settings.z;
                    vec2 axis = index < 2 * settings.z + 1 ? vec2(1, 0) : vec2(0, 1);
                    vec2 candidate = clamp(axis * float(offset), -p, size - 1.0 - p);
                    float norm = dot(candidate, candidate);
                    float delta = dot(candidate - original, candidate - original);
                    if ((norm < magnitude || (norm == magnitude && delta < distance)) &&
                        dense_cost(first, second, p, candidate, limit) <= limit) {
                        vector = candidate;
                        magnitude = norm;
                        distance = delta;
                    }
                }
                if (any(notEqual(vector, original)))
                    residual = patch_cost(second, p, vector);
            }
        }
        color = vec4(vector * motion_scale, residual, inherited.w);
    }
);

static const char evidence_body[] = GLSL(
    ivec2 grid = textureSize(forward, 0);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(pixel, grid))) {
        vec2 position = (vec2(pixel) + 0.5) / vec2(grid);
        vec2 size = vec2(textureSize(first, 0));
        vec2 center = position * size - 0.5;
        vec2 f = texelFetch(forward, pixel, 0).xy;
        vec2 r = texelFetch(backward, pixel, 0).xy;
        bool use_forward = any(notEqual(f, vec2(0)));
        bool use_backward = any(notEqual(r, vec2(0)));
        vec2 evidence = vec2(0.0);
        if (use_forward || use_backward) {
            vec3 cost = bidirectional_cost(first, second, center, f, r);
            if (use_forward)
                evidence.x = max(0.0, cost.x - cost.y);
            if (use_backward)
                evidence.y = max(0.0, cost.x - cost.z);
        }
        color = pack_evidence(evidence.x);
        imageStore(backward_evidence, pixel, pack_evidence(evidence.y));
    }
);

static const char reconstruction_body[] = GLSL(
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
        float cost = patch_cost(second, p, selected.xy);
        float limit = cost + 0.000001;
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
                if (cycle >= 1.5)
                    continue;
                float score = patch_cost(second, p, value.xy, limit);
                if (score > limit)
                    continue;
                selected = value;
                cost = score;
                best = evidence;
                nearest = distance;
            }
        }
        // Use the same local score tolerance as the observed-neighbor selection.
        vec4 hypothesis = texelFetch(translation, ivec2(0), 0);
        if (hypothesis.w > 0.0) {
            vec2 vector = direction != 0 ? -hypothesis.xy : hypothesis.xy;
            vector = clamp(vector, -p, size - 1.0 - p);
            float limit = cost + 0.000001;
            float score = patch_cost(second, p, vector, limit);
            if (score <= limit) {
                selected = vec4(vector, score, hypothesis.w);
                cost = score;
            }
        }
        color = vec4(selected.xy, cost, selected.w);
    }
);

static const char scene_body[] = GLSL(
    float hist0[8];
    float hist1[8];
    for (int i = 0; i < 8; i++) {
        hist0[i] = 0.0;
        hist1[i] = 0.0;
    }
    vec3 mean0 = vec3(0.0);
    vec3 mean1 = vec3(0.0);
    float residual = 0.0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            vec2 uv = (vec2(x, y) + 0.5) / 16.0;
            vec3 a = textureLod(first, uv, 0.0).rgb;
            vec3 b = textureLod(second, uv, 0.0).rgb;
            int ia = clamp(int(dot(a, vec3(0.2126, 0.7152, 0.0722)) * 8.0), 0, 7);
            int ib = clamp(int(dot(b, vec3(0.2126, 0.7152, 0.0722)) * 8.0), 0, 7);
            hist0[ia] += 1.0;
            hist1[ib] += 1.0;
            mean0 += a;
            mean1 += b;
            residual += 0.5 * (textureLod(forward, uv, 0.0).z +
                               textureLod(backward, uv, 0.0).z);
        }
    }
    float histogram_distance = 0.0;
    for (int i = 0; i < 8; i++)
        histogram_distance += abs(hist0[i] - hist1[i]);
    vec3 mean_distance = abs(mean0 - mean1) / 256.0;
    float cut = max(histogram_distance / 512.0,
                    max(mean_distance.r, max(mean_distance.g, mean_distance.b)));
    // A global brightness change contributes matching error without a cut.
    float brightness_error = dot(mean_distance, vec3(0.25)) +
        0.25 * abs(dot((mean0 - mean1) / 256.0, vec3(0.2126, 0.7152, 0.0722)));
    color = vec4(cut, max(0.0, residual / 256.0 - brightness_error), 0.0, 1.0);
);

static const char synthesis_header[] = GLSL(
    float support(vec2 p, vec2 size)
    {
        vec2 distance = min(p, 1.0 - p) * size;
        return clamp(min(distance.x, distance.y), 0.0, 1.0);
    }

    float quality(vec4 vector)
    {
        return (1.0 - smoothstep(0.01, 0.08, vector.z)) *
               smoothstep(0.01, 0.15, vector.w);
    }
);

static const char warp_body[] = GLSL(
    vec2 uv = (vec2(gl_GlobalInvocationID.xy) + 0.5) / map_size;
    vec2 source = uv;
    vec2 previous = source;
    for (int i = 0; i < 4; i++) {
        previous = source;
        source = uv - weight * textureLod(forward, source, 0.0).xy / map_size;
    }
    vec4 f = textureLod(forward, source, 0.0);
    vec2 next = uv - weight * f.xy / map_size;
    vec2 error = (next - source) * output_size;
    float residual = length(error);
    if (residual > 0.25) {
        vec2 difference = error - (source - previous) * output_size;
        float denominator = dot(difference, difference);
        if (denominator > 0.0) {
            // Solve the linearized fixed point using consecutive residuals.
            vec2 candidate = next - (next - source) * dot(error, difference) / denominator;
            if (support(candidate, map_size) > 0.0) {
                vec4 match = textureLod(forward, candidate, 0.0);
                float candidate_residual =
                    length((candidate + weight * match.xy / map_size - uv) * output_size);
                if (candidate_residual < residual) {
                    source = candidate;
                    f = match;
                    residual = candidate_residual;
                }
            }
        }
    }
    vec2 displacement = -weight * f.xy;
    f.xy /= map_size;
    float trust = quality(f);
    // Qualify each inverse independently. The samples at the common phase
    // determine agreement, including the aperture case.
    float geometry = 1.0 - smoothstep(0.25, 1.0, residual);
    vec4 reverse = textureLod(backward, source + f.xy, 0.0);
    float reliable = 1.0 - smoothstep(0.001, 0.01, max(f.z, reverse.z));
    float cycle = length(f.xy * map_size + reverse.xy);
    float certified = max(trust * (1.0 - smoothstep(0.5, 1.5, cycle)),
                          reliable * (1.0 - smoothstep(0.1, 0.5, cycle)));
    if (any(isnan(reverse)) || any(isinf(reverse)))
        certified = 0.0;
    geometry *= min(support(source, map_size), support(source + f.xy, map_size));
    if (any(isnan(f)) || any(isinf(f)) ||
        isnan(geometry) || isinf(geometry))
        geometry = 0.0;
    // Use the solved vector directly. Subtracting normalized coordinates
    // loses precision before half-float storage, even for integer translations.
    color = vec4(displacement,
                 trust * geometry, certified * geometry);
);

static const char synthesis_pixel_header[] = GLSL(
    vec3 synthesis_pq(vec3 linear_rgb)
    {
        vec3 p = pow(clamp(linear_rgb, 0.0, 10000.0 / 203.0) * 0.0203,
                     vec3(2610.0 / 16384.0));
        vec3 t = 1.0 / fma(vec3(2392.0 / 128.0), p, vec3(1.0));
        // Express the weighted cubic exponent directly in the denominator reciprocal.
        vec3 exponent = fma(t, fma(t, fma(t, vec3(-0.2091452777), vec3(-1.6654376984)),
                                     vec3(-19.4895648956)), vec3(0.9942703843));
        return exp2(exponent);
    }

    float relative_fit(float stationary_error, float warped_error)
    {
        return stationary_error > warped_error
            ? (stationary_error - warped_error) / stationary_error : 0.0;
    }

    vec3 sample_witness(sampler2D image, vec2 p, vec2 extent, vec2 texel_size)
    {
        p = clamp(p, vec2(0.0), extent - 1.0);
        vec2 lo = floor(p);
        vec2 center = (lo + 1.0) * texel_size;
        vec2 fraction = p - lo;
        vec4 red = textureGather(image, center, 0);
        vec4 green = textureGather(image, center, 1);
        vec4 blue = textureGather(image, center, 2);
        // Gather returns bottom-left, bottom-right, top-right, top-left.
        vec3 a = vec3(red.w, green.w, blue.w);
        vec3 b = vec3(red.z, green.z, blue.z);
        vec3 c = vec3(red.x, green.x, blue.x);
        vec3 d = vec3(red.y, green.y, blue.y);
        return mix(mix(a, b, fraction.x), mix(c, d, fraction.x), fraction.y);
    }

    vec4 synthesize_pixel(sampler2D source0, sampler2D source1, sampler2D warp0,
                          sampler2D warp1, sampler2D scene_map, ivec2 pixel,
                          vec2 output_extent, vec2 motion_extent, vec2 projection,
                          float blend, int hdr)
    {
        vec2 uv = (vec2(pixel) + 0.5) / output_extent;
        ivec2 source_extent = ivec2(output_extent);
        ivec2 source_pixel = clamp(pixel, ivec2(0), source_extent - 1);
        vec4 original0 = texelFetch(source0, source_pixel, 0);
        vec4 original1 = texelFetch(source1, source_pixel, 0);
        vec2 scene_change = texelFetch(scene_map, ivec2(0), 0).rg;
        if (scene_change.r > 0.45 || scene_change.g > 0.08) {
            return blend < 0.5 ? original0 : original1;
        } else {
            vec4 f = textureLod(warp0, uv, 0.0);
            vec4 r = textureLod(warp1, uv, 0.0);
            const vec4 finite_limit = vec4(uintBitsToFloat(0x7f7fffffu));
            if (!all(lessThanEqual(abs(f), finite_limit)) ||
                !all(lessThanEqual(abs(r), finite_limit))) {
                f = vec4(0.0);
                r = vec4(0.0);
            }
            vec4 original = mix(original0, original1, blend);
            // Without displacement, trajectory and stationary errors agree.
            if (all(equal(vec4(f.xy, r.xy), vec4(0.0))))
                return original;

            // Aperture recovery needs support from both inverse maps.
            if (max(f.z, r.z) == 0.0 && min(f.w, r.w) == 0.0)
                return original;

            // Match the photometric data term in source pixel coordinates.
            vec2 scale = output_extent / motion_extent;
            vec2 displacement0 = f.xy * scale;
            vec2 displacement1 = r.xy * scale;
            vec2 p0 = vec2(pixel) + displacement0;
            vec2 p1 = vec2(pixel) + displacement1;
            vec4 warped0 = sample_rgba(source0, p0, source_extent);
            vec4 warped1 = sample_rgba(source1, p1, source_extent);
            float certified = min(f.w, r.w);
            float unique = min(min(f.z, r.z), certified);
            float strongest = max(f.z, r.z);
            vec2 phase = vec2(1.0 - blend, blend);
            bool same_original = all(equal(original0.rgb, original1.rgb));
            bool fixed_weights = false;
            if (hdr != 0 && (strongest <= certified || same_original) &&
                !any(isnan(warped0.rgb)) && !any(isnan(warped1.rgb))) {
                vec3 lo = max(min(warped0.rgb, warped1.rgb), 0.0);
                vec3 hi = min(max(warped0.rgb, warped1.rgb), 10000.0 / 203.0);
                // The PQ slope in log luminance is below 0.12. These ratios
                // bound the error below 0.03 or 0.002, respectively. The
                // resulting shared confidence is certified in either case.
                float ratio = unique == certified ? 1.27 : 1.016;
                fixed_weights = all(lessThanEqual(hi, ratio * lo));
            }
            vec2 visibility = phase * certified;
            if (!fixed_weights) {
                vec3 metric0 = vec3(0.0), metric1 = vec3(0.0);
                if (hdr == 0 || !all(equal(warped0.rgb, warped1.rgb))) {
                    metric0 = hdr != 0 ? synthesis_pq(warped0.rgb) : warped0.rgb;
                    metric1 = hdr != 0 ? synthesis_pq(warped1.rgb) : warped1.rgb;
                }
                vec3 conflict = abs(metric0 - metric1);
                float color_error = max(conflict.r, max(conflict.g, conflict.b));
                // An edge can constrain motion without a unique tangential match.
                // Recover it only when both directions and full-size colors agree.
                float aperture = certified * (1.0 - smoothstep(0.002, 0.02, color_error));
                float reliable = max(unique, aperture);
                float conflict_weight = smoothstep(0.03, 0.15, color_error);
                float stationary_error = 0.0;
                if ((strongest > reliable || (strongest > 0.0 && conflict_weight > 0.0)) &&
                    !same_original) {
                    vec3 stationary0 = hdr != 0 ? synthesis_pq(original0.rgb) : original0.rgb;
                    vec3 stationary1 = hdr != 0 ? synthesis_pq(original1.rgb) : original1.rgb;
                    vec3 stationary = abs(stationary0 - stationary1);
                    stationary_error = max(stationary.r, max(stationary.g, stationary.b));
                }
                vec2 confidence = max(vec2(f.z, r.z) * relative_fit(stationary_error, color_error),
                                      reliable);
                vec2 weights = phase * confidence;
                if (!(color_error >= 0.0))
                    return original;
                visibility = weights * (1.0 - conflict_weight);
                if (conflict_weight > 0.0 && stationary_error > 0.0) {
                    // Each inverse predicts both endpoints of one trajectory.
                    // Conflicting inverse hypotheses cannot validate each other.
                    vec2 texel_size = 1.0 / output_extent;
                    vec3 paired1 = sample_witness(source1, vec2(pixel) -
                        displacement0 * projection.x, output_extent, texel_size);
                    vec3 paired0 = sample_witness(source0, vec2(pixel) -
                        displacement1 * projection.y, output_extent, texel_size);
                    vec3 predicted1 = hdr != 0 ? synthesis_pq(paired1) : paired1;
                    vec3 predicted0 = hdr != 0 ? synthesis_pq(paired0) : paired0;
                    vec3 error0 = abs(metric0 - predicted1);
                    vec3 error1 = abs(predicted0 - metric1);
                    vec2 paired_error = vec2(max(error0.r, max(error0.g, error0.b)),
                                             max(error1.r, max(error1.g, error1.b)));
                    vec2 support = vec2(1.0) - smoothstep(vec2(0.03), vec2(0.15), paired_error);
                    vec2 evidence = support * vec2(relative_fit(stationary_error, paired_error.x),
                                                   relative_fit(stationary_error, paired_error.y));
                    evidence = mix(vec2(0.0), evidence, greaterThan(vec2(f.z, r.z), vec2(0.0)));
                    visibility += conflict_weight * phase * vec2(f.z, r.z) * evidence;
                }
            }
            return original * (1.0 - visibility.x - visibility.y) +
                   warped0 * visibility.x + warped1 * visibility.y;
        }
    }
);

static const char synthesis_body[] = GLSL(
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    color = synthesize_pixel(first, second, forward, backward, scene, pixel,
                             output_size, motion_size, projection_ratio, alpha, linear_hdr);
);

static const char synthesis_shader_body[] = GLSL(
    ivec2 pixel = clamp(ivec2(floor(motion_position)), ivec2(0), ivec2(output_size) - 1);
    if (source_only != 0) {
        color = texelFetch(first, pixel, 0);
    } else {
        color = synthesize_pixel(first, second, forward, backward, scene, pixel,
                                 output_size, motion_size, projection_ratio, alpha, linear_hdr);
    }
);

static bool recreate_color(struct mp_motion *m, pl_tex *texture, int w, int h,
                           bool linear_hdr)
{
    pl_fmt format = linear_hdr ? m->hdr_format : m->format;
    return format && pl_tex_recreate(m->gpu, texture, pl_tex_params(
        .w = w, .h = h,
        .format = format,
        .sampleable = true,
        .renderable = true,
        .storable = true,
    ));
}

static bool recreate(struct mp_motion *m, pl_tex *texture, int w, int h)
{
    return recreate_color(m, texture, w, h, false);
}

static struct pl_shader_desc sampled(const char *name, pl_tex texture)
{
    return (struct pl_shader_desc) {
        .desc = { .name = name, .type = PL_DESC_SAMPLED_TEX },
        .binding = {
            .object = texture,
            .sample_mode = PL_TEX_SAMPLE_LINEAR,
            .address_mode = PL_TEX_ADDRESS_CLAMP,
        },
    };
}

static struct pl_shader_desc sampled_texels(const char *name, pl_tex texture)
{
    struct pl_shader_desc desc = sampled(name, texture);
    // sample_rgba performs bilinear filtering on explicit texels.
    desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    return desc;
}

static pl_shader begin_dispatch(struct mp_motion *m,
                                struct pl_custom_shader *custom)
{
    pl_shader shader = pl_dispatch_begin(m->dispatch);
    if (!pl_shader_custom(shader, &(struct pl_custom_shader) {
            .header = sample_header,
        }) || !pl_shader_custom(shader, custom)) {
        pl_dispatch_abort(m->dispatch, &shader);
        return NULL;
    }
    return shader;
}

static bool dispatch(struct mp_motion *m, pl_tex target,
                     struct pl_custom_shader *custom)
{
    custom->output = PL_SHADER_SIG_COLOR;
    custom->compute = true;
    custom->compute_group_size[0] = target->params.w == 1 ? 1 : 8;
    custom->compute_group_size[1] = target->params.h == 1 ? 1 : 8;
    pl_shader shader = begin_dispatch(m, custom);
    return shader && pl_dispatch_finish(m->dispatch, pl_dispatch_params(
        .shader = &shader, .target = target,
    ));
}

enum motion_pass_mode {
    MOTION_PASS_PREPARE,
    MOTION_PASS_RUN,
    MOTION_PASS_DEFER,
};

static bool dispatch_prepared(struct mp_motion *m, pl_dispatch_prepared *pass,
                              pl_tex target, struct pl_custom_shader *custom,
                              bool matching, enum motion_pass_mode mode)
{
    if (mode == MOTION_PASS_PREPARE) {
        custom->output = matching ? PL_SHADER_SIG_NONE : PL_SHADER_SIG_COLOR;
        custom->compute = true;
        custom->compute_group_size[0] = matching ? MATCH_GROUP_SIZE
                                                : target->params.w == 1 ? 1 : 8;
        custom->compute_group_size[1] = matching ? 1
                                                : target->params.h == 1 ? 1 : 8;
        custom->compute_shmem = matching ? MATCH_SHARED_BYTES : 0;
        pl_shader shader = begin_dispatch(m, custom);
        if (!shader)
            return false;
        *pass = matching
            ? pl_dispatch_prepare_compute(m->dispatch, pl_dispatch_compute_params(
                .shader = &shader,
                .dispatch_size = {target->params.w, target->params.h, 1},
            ))
            : pl_dispatch_prepare(m->dispatch, pl_dispatch_params(
                .shader = &shader, .target = target,
            ));
        return *pass != NULL;
    }

    struct pl_desc_binding bindings[6];
    assert(custom->num_descriptors <= sizeof(bindings) / sizeof(bindings[0]));
    for (int i = 0; i < custom->num_descriptors; i++)
        bindings[i] = custom->descriptors[i].binding;
    return pl_dispatch_run_prepared_ex(*pass, &(struct pl_dispatch_prepared_params) {
        .descriptors = bindings, .num_descriptors = custom->num_descriptors,
        .variables = custom->variables, .num_variables = custom->num_variables,
        .target = matching ? NULL : target,
        .dispatch_size = {matching ? target->params.w : 0,
                          matching ? target->params.h : 0, matching ? 1 : 0},
    }, mode == MOTION_PASS_DEFER);
}

static void destroy_passes(struct mp_motion *m)
{
    for (int level = 0; level < LEVELS; level++) {
        for (int direction = 0; direction < 2; direction++) {
            pl_dispatch_prepared_destroy(&m->pyramid_pass[level][direction]);
            pl_dispatch_prepared_destroy(&m->matching_pass[level][direction]);
        }
    }
    for (int direction = 0; direction < 2; direction++) {
        pl_dispatch_prepared_destroy(&m->reconstruction_pass[direction]);
        pl_dispatch_prepared_destroy(&m->warp_pass[direction]);
    }
    pl_dispatch_prepared_destroy(&m->evidence_pass);
    pl_dispatch_prepared_destroy(&m->translation_pass);
    pl_dispatch_prepared_destroy(&m->selection_pass);
    pl_dispatch_prepared_destroy(&m->scene_pass);
    m->prepared_format[0] = m->prepared_format[1] = NULL;
}

static bool downsample(struct mp_motion *m, pl_tex source, pl_tex target,
                       bool linear_hdr, pl_dispatch_prepared *pass,
                       enum motion_pass_mode mode)
{
    m->timing_stage = -1;
    struct pl_shader_desc desc = sampled_texels("source", source);
    // Preserve exact integer reductions before matching. Dividing large pixel
    // coordinates in the shader can shift the sampling phase on some backends.
    float scale[] = {
        (float) source->params.w / target->params.w,
        (float) source->params.h / target->params.h,
    };
    int hdr = linear_hdr;
    struct pl_shader_var vars[] = {
        {.var = pl_var_vec2("source_scale"), .data = scale},
        {.var = pl_var_int("linear_hdr"), .data = &hdr},
    };
    return dispatch_prepared(m, pass, target, &(struct pl_custom_shader) {
        .description = "motion pyramid sampling",
        .body = GLSL(
            vec2 p = (vec2(gl_GlobalInvocationID.xy) + 0.5) * source_scale - 0.5;
            vec3 sampled = sample_color(source, p);
            color = vec4(linear_hdr != 0 ? hdr_proxy(sampled) : sampled, 1.0);
        ),
        .descriptors = &desc, .num_descriptors = 1,
        .variables = vars, .num_variables = 2,
    }, false, mode);
}

static bool analyze_translation(struct mp_motion *m, enum motion_pass_mode mode)
{
    int radius[2] = {
        fmin(MATCH_RADIUS << (LEVELS - 1), (m->images[0][0]->params.w - 1) / 2),
        fmin(MATCH_RADIUS << (LEVELS - 1), (m->images[0][0]->params.h - 1) / 2),
    };
    if (!recreate(m, &m->translation_costs, 2 * radius[0] + 1, 2 * radius[1] + 1) ||
        !recreate(m, &m->translation, 1, 1))
        return false;
    struct pl_shader_desc desc[] = {
        sampled("first", m->images[0][0]), sampled("second", m->images[0][1]),
    };
    m->timing_stage = LEVELS + 4;
    if (!dispatch_prepared(m, &m->translation_pass, m->translation_costs,
                           &(struct pl_custom_shader) {
            .description = "motion translation costs",
            .body = translation_cost_body,
            .descriptors = desc, .num_descriptors = 2,
            .variables = &(struct pl_shader_var) {
                .var = pl_var_ivec2("radius"), .data = radius,
            }, .num_variables = 1,
        }, false, mode))
        return false;
    struct pl_shader_desc selection[] = {
        sampled("costs", m->translation_costs),
        {
            .desc = {.name = "out_image", .type = PL_DESC_STORAGE_IMG,
                     .access = PL_DESC_ACCESS_WRITEONLY},
            .binding.object = m->translation,
        },
    };
    m->timing_stage = LEVELS + 5;
    return dispatch_prepared(m, &m->selection_pass, m->translation, &(struct pl_custom_shader) {
        .description = "motion translation selection",
        .header = translation_select_header, .body = translation_select_body,
        .descriptors = selection, .num_descriptors = 2,
    }, true, mode);
}

static bool analysis_passes(struct mp_motion *m, pl_tex first, pl_tex second,
                            const bool reuse[2], enum motion_pass_mode finish)
{
    bool prepare = finish == MOTION_PASS_PREPARE;
    enum motion_pass_mode mode = prepare ? MOTION_PASS_PREPARE : MOTION_PASS_DEFER;
    int width = first->params.w;
    int height = first->params.h;
    int largest = width > height ? width : height;
    double scale = largest > m->max_dimension
                 ? (double) m->max_dimension / largest : 1.0;
    width = fmax(8, lround(width * scale));
    height = fmax(8, lround(height * scale));
    pl_tex inputs[2] = {first, second};
    for (int level = 0; level < LEVELS; level++) {
        int spacing = level == LEVELS - 1 ? 4 : 8;
        for (int direction = 0; direction < 2; direction++) {
            if (!recreate(m, &m->images[level][direction], width, height) ||
                !recreate(m, &m->vectors[level][direction],
                          (width + spacing - 1) / spacing,
                          (height + spacing - 1) / spacing) ||
                ((prepare || !reuse[direction]) &&
                 !downsample(m, inputs[direction], m->images[level][direction],
                             level == 0 && m->linear_hdr,
                             &m->pyramid_pass[level][direction], mode)))
                return false;
            inputs[direction] = m->images[level][direction];
        }
        width = (width + 1) / 2;
        height = (height + 1) / 2;
    }

    if (!analyze_translation(m, mode))
        return false;

    for (int level = LEVELS - 1; level >= 0; level--) {
        for (int direction = 0; direction < 2; direction++) {
            pl_tex target = m->vectors[level][direction];
            bool parent = level + 1 < LEVELS;
            struct pl_shader_desc desc[3] = {
                sampled("first", m->images[level][direction]),
                sampled("second", m->images[level][!direction]),
            };
            if (parent) {
                desc[2] = sampled("parent", m->vectors[level + 1][direction]);
            } else {
                desc[2] = (struct pl_shader_desc) {
                    .desc = {
                        .name = "out_image", .type = PL_DESC_STORAGE_IMG,
                        .access = PL_DESC_ACCESS_WRITEONLY,
                    },
                    .binding.object = target,
                };
            }
            int settings[] = {target->params.w, target->params.h,
                              parent ? 2 : MATCH_RADIUS};
            // Finest-analysis pixel units retain exact integer motion in
            // RGBA16F, including aspect ratios with nonbinary reciprocals.
            float motion_scale[] = {
                (float) m->images[0][direction]->params.w /
                    m->images[level][direction]->params.w,
                (float) m->images[0][direction]->params.h /
                    m->images[level][direction]->params.h,
            };
            struct pl_shader_var variables[] = {
                {.var = pl_var_ivec3("settings"), .data = settings},
                {.var = pl_var_vec2("motion_scale"), .data = motion_scale},
            };
            m->timing_stage = level;
            struct pl_custom_shader custom = {
                .description = parent ? "motion optical refinement"
                                      : "motion block matching",
                .header = parent ? refine_header : match_header,
                .body = parent ? refine_body : match_body,
                .descriptors = desc, .num_descriptors = 3,
                .variables = variables, .num_variables = 2,
            };
            if (!dispatch_prepared(m, &m->matching_pass[level][direction],
                                   target, &custom, !parent, mode))
                return false;
        }
    }
    for (int direction = 0; direction < 2; direction++) {
        pl_tex vectors = m->vectors[0][direction];
        if (!recreate(m, &m->evidence[direction], vectors->params.w, vectors->params.h))
            return false;
    }
    struct pl_shader_desc evidence_desc[] = {
        sampled("first", m->images[0][0]), sampled("second", m->images[0][1]),
        sampled("forward", m->vectors[0][0]), sampled("backward", m->vectors[0][1]),
        {
            .desc = {.name = "backward_evidence", .type = PL_DESC_STORAGE_IMG,
                     .access = PL_DESC_ACCESS_WRITEONLY},
            .binding.object = m->evidence[1],
        },
    };
    m->timing_stage = LEVELS + 2;
    if (!dispatch_prepared(m, &m->evidence_pass, m->evidence[0], &(struct pl_custom_shader) {
        .description = "motion observed evidence pair",
        .header = refine_header, .body = evidence_body,
        .descriptors = evidence_desc, .num_descriptors = 5,
    }, false, mode))
        return false;

    for (int direction = 0; direction < 2; direction++) {
        pl_tex vectors = m->vectors[0][direction];
        if (!recreate(m, &m->reconstructed[direction],
                      vectors->params.w, vectors->params.h))
            return false;
        struct pl_shader_desc desc[] = {
            sampled("first", m->images[0][direction]),
            sampled("second", m->images[0][!direction]),
            sampled("forward", m->vectors[0][direction]),
            sampled("backward", m->vectors[0][!direction]),
            sampled("evidence_map", m->evidence[direction]),
            sampled("translation", m->translation),
        };
        float grid_scale[] = {
            (float) m->images[0][direction]->params.w / vectors->params.w,
            (float) m->images[0][direction]->params.h / vectors->params.h,
        };
        struct pl_shader_var variables[] = {
            {.var = pl_var_int("direction"), .data = &direction},
            {.var = pl_var_vec2("grid_scale"), .data = grid_scale},
        };
        m->timing_stage = LEVELS + 3;
        if (!dispatch_prepared(m, &m->reconstruction_pass[direction],
                               m->reconstructed[direction], &(struct pl_custom_shader) {
            .description = "motion field reconstruction",
            .header = refine_header, .body = reconstruction_body,
            .descriptors = desc, .num_descriptors = 6,
            .variables = variables, .num_variables = 2,
        }, false, mode))
            return false;
    }
    struct pl_shader_desc scene_desc[] = {
        sampled("first", m->images[0][0]), sampled("second", m->images[0][1]),
        sampled("forward", m->vectors[0][0]), sampled("backward", m->vectors[0][1]),
    };
    m->timing_stage = -1;
    if (!recreate(m, &m->scene, 1, 1) ||
        !dispatch_prepared(m, &m->scene_pass, m->scene, &(struct pl_custom_shader) {
            .description = "motion scene detection",
            .body = scene_body,
            .descriptors = scene_desc, .num_descriptors = 4,
        }, false, finish))
        return false;
    return true;
}

static bool analyze(struct mp_motion *m, pl_tex first, pl_tex second,
                    uint64_t first_id, uint64_t second_id,
                    enum motion_pass_mode finish)
{
    bool valid = m->pair_valid && m->source_width == first->params.w &&
                 m->source_height == first->params.h;
    if (valid && m->ids[0] == first_id && m->ids[1] == second_id)
        return true;
    bool reuse[2] = {
        valid && m->ids[0] == first_id,
        valid && m->ids[1] == second_id,
    };
    if (valid && (m->ids[1] == first_id || m->ids[0] == second_id)) {
        for (int level = 0; level < LEVELS; level++) {
            pl_tex image = m->images[level][0];
            m->images[level][0] = m->images[level][1];
            m->images[level][1] = image;
        }
        reuse[0] = m->ids[1] == first_id;
        reuse[1] = m->ids[0] == second_id;
    }
    m->pair_valid = false;
    m->result_valid = false;
    m->warp_valid = false;
    if (!analysis_passes(m, first, second, reuse, finish) ||
        pl_gpu_is_failed(m->gpu))
    {
        pl_gpu_flush(m->gpu);
        return false;
    }
    m->source_width = first->params.w;
    m->source_height = first->params.h;
    m->ids[0] = first_id;
    m->ids[1] = second_id;
    m->pair_valid = true;
    m->stats.analyzed_pairs++;
    m->stats.analysis_width = m->images[0][0]->params.w;
    m->stats.analysis_height = m->images[0][0]->params.h;
    return true;
}

static void gpu_timing(void *priv, const struct pl_dispatch_info *info)
{
    struct mp_motion *m = priv;
    if (!info->num_samples)
        return;
    if (m->timing_stage == LEVELS + 4)
        m->stats.translation_cost_ns = info->average;
    else if (m->timing_stage == LEVELS + 5)
        m->stats.translation_select_ns = info->average;
    else if (m->timing_stage == LEVELS - 1)
        m->stats.coarse_ns = info->average;
    else if (m->timing_stage >= 0 && m->timing_stage < LEVELS - 1)
        m->stats.refinement_ns = info->average;
    else if (m->timing_stage == LEVELS)
        m->stats.warp_ns = info->average;
    else if (m->timing_stage == LEVELS + 1)
        m->stats.synthesis_ns = info->average;
    else if (m->timing_stage == LEVELS + 2)
        m->stats.evidence_ns = info->average;
    else if (m->timing_stage == LEVELS + 3)
        m->stats.reconstruction_ns = info->average;
}

static void capture_timing(void *priv, const struct pl_render_info *info)
{
    struct mp_motion *m = priv;
    m->stats.capture_ns += info->pass->average;
}

static pl_fmt motion_format(pl_gpu gpu)
{
    if (!gpu || !gpu->glsl.compute ||
        gpu->glsl.max_group_threads < MATCH_GROUP_SIZE ||
        gpu->glsl.max_group_size[0] < MATCH_GROUP_SIZE ||
        gpu->glsl.max_group_size[1] < 8 ||
        gpu->glsl.max_shmem_size < MATCH_SHARED_BYTES)
        return NULL;
    return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0,
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR |
        PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE);
}

static pl_fmt motion_hdr_format(pl_gpu gpu)
{
    if (!gpu || !pl_color_transfer_is_normalized_linear(PL_COLOR_TRC_LINEAR_HDR))
        return NULL;
    return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE);
}

bool mp_motion_hdr_supported(pl_gpu gpu)
{
    return motion_format(gpu) && motion_hdr_format(gpu);
}

bool mp_motion_supported(pl_gpu gpu)
{
    return motion_format(gpu) != NULL;
}

struct mp_motion *mp_motion_create(pl_gpu gpu, int max_dimension)
{
    if (max_dimension < 16 || max_dimension > 1024)
        return NULL;
    pl_fmt format = motion_format(gpu);
    if (!format)
        return NULL;
    struct mp_motion *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->gpu = gpu;
    m->format = format;
    m->hdr_format = motion_hdr_format(gpu);
    m->position_format = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 2);
    m->max_dimension = max_dimension;
    m->dispatch = pl_dispatch_create(gpu->log, gpu);
    m->timing_stage = -1;
    m->renderer = pl_renderer_create(gpu->log, gpu);
    if (!m->dispatch || !m->renderer) {
        mp_motion_destroy(&m);
        return NULL;
    }
    pl_dispatch_callback(m->dispatch, m, gpu_timing);
    return m;
}

void mp_motion_reset(struct mp_motion *m)
{
    if (m) {
        m->pair_valid = false;
        m->result_valid = false;
        m->warp_valid = false;
    }
}

struct mp_motion_stats mp_motion_get_stats(const struct mp_motion *m)
{
    return m ? m->stats : (struct mp_motion_stats) {0};
}

void mp_motion_destroy(struct mp_motion **motion)
{
    struct mp_motion *m = *motion;
    if (!m)
        return;
    for (int level = 0; level < LEVELS; level++) {
        for (int direction = 0; direction < 2; direction++) {
            pl_tex_destroy(m->gpu, &m->images[level][direction]);
            pl_tex_destroy(m->gpu, &m->vectors[level][direction]);
        }
    }
    for (int i = 0; i < 2; i++) {
        pl_tex_destroy(m->gpu, &m->evidence[i]);
        pl_tex_destroy(m->gpu, &m->reconstructed[i]);
        pl_tex_destroy(m->gpu, &m->warps[i]);
    }
    pl_tex_destroy(m->gpu, &m->scene);
    pl_tex_destroy(m->gpu, &m->translation);
    pl_tex_destroy(m->gpu, &m->translation_costs);
    pl_tex_destroy(m->gpu, &m->result);
    destroy_passes(m);
    pl_dispatch_destroy(&m->dispatch);
    pl_renderer_destroy(&m->renderer);
    free(m);
    *motion = NULL;
}

static struct pl_frame captured_frame(pl_tex texture,
                                      const struct pl_color_space *color)
{
    return (struct pl_frame) {
        .num_planes = 1,
        .planes[0] = {
            .texture = texture, .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
        .repr = pl_color_repr_rgb,
        .color = *color,
    };
}

static bool render_capture(struct mp_motion *m, const struct pl_frame *source,
                           const struct pl_frame *target, bool linear_hdr,
                           bool preserve_alpha)
{
    struct pl_frame input = *source;
    input.rotation = PL_ROTATION_0;
    input.overlays = NULL;
    input.num_overlays = 0;
    struct pl_render_params params = pl_render_default_params;
    params.skip_caching_single_frame = true;
    params.dither_params = NULL;
    params.peak_detect_params = NULL;
    if (preserve_alpha)
        params.background = PL_CLEAR_SKIP;
    struct pl_color_map_params mapping = pl_color_map_default_params;
    if (linear_hdr) {
        mapping.gamut_mapping = &pl_gamut_map_clip;
        mapping.tone_mapping_function = &pl_tone_map_clip;
        mapping.mastering_clip = 0;
        mapping.film_strength = 0;
        mapping.metadata = PL_HDR_METADATA_HDR10;
        params.color_map_params = &mapping;
    }
    params.info_callback = capture_timing;
    params.info_priv = m;
    m->stats.capture_ns = 0;
    bool dovi = input.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION;
    if (dovi)
        pl_renderer_reset_errors(m->renderer, NULL);
    bool ok = pl_render_image(m->renderer, &input, target, &params);
    // Display rendering may fall back to BL-only if FEL sampling fails.
    // Such an incomplete reconstruction cannot become a motion source.
    return ok && (!dovi || !pl_renderer_get_errors(m->renderer).errors);
}

bool mp_motion_capture(struct mp_motion *m, const struct pl_frame *source,
                       int width, int height, pl_tex *texture)
{
    if (!m || !source || !texture || width < 8 || height < 8 ||
        !recreate(m, texture, width, height))
        return false;
    struct pl_frame target = captured_frame(*texture, &source->color);
    return render_capture(m, source, &target, false, false);
}

static struct pl_hdr_metadata motion_hdr_metadata(
    const struct pl_hdr_metadata *source)
{
    // Warping preserves the per-pixel bound, but can change frame averages.
    return (struct pl_hdr_metadata) {
        .prim = source->prim,
        .min_luma = source->min_luma,
        .max_luma = source->max_luma,
        .max_cll = source->max_cll,
    };
}

static bool hdr_source(const struct pl_frame *input)
{
    return input->color.transfer == PL_COLOR_TRC_PQ ||
           input->repr.sys == PL_COLOR_SYSTEM_DOLBYVISION;
}

static bool capture_format_supported(const struct pl_frame *input)
{
    bool dovi = input->repr.sys == PL_COLOR_SYSTEM_DOLBYVISION;
    if (dovi) {
#ifdef PL_HAVE_DOVI
        if (!input->repr.dovi ||
            (input->repr.dovi->nlq_active &&
             (!input->enhancement_layer ||
              input->enhancement_layer->field != PL_FIELD_NONE)))
            return false;
#else
        return false;
#endif
    }
    if (hdr_source(input)) {
        struct pl_hdr_metadata metadata = motion_hdr_metadata(&input->color.hdr);
        metadata.max_fall = input->color.hdr.max_fall;
        if (!pl_hdr_metadata_equal(&metadata, &input->color.hdr))
            return false;
    }
    return input->field == PL_FIELD_NONE &&
           (dovi || !pl_color_transfer_is_hdr(input->color.transfer) ||
            input->color.transfer == PL_COLOR_TRC_PQ) &&
           (dovi || input->color.transfer != PL_COLOR_TRC_LINEAR ||
            !pl_color_space_is_hdr(&input->color));
}

bool mp_motion_capture_supported(const struct mp_motion *m,
                                  const struct pl_frame *source)
{
    return m && source && capture_format_supported(source) &&
           (!hdr_source(source) ||
            (m->hdr_format && !source->icc && !source->profile.len &&
             !source->lut));
}

static enum mp_motion_result capture_acquired_frame(
    struct mp_motion *m, const struct pl_frame *acquired, int width, int height,
    struct mp_motion_frame *frame)
{
    struct pl_frame input = *acquired;
    input.acquire = NULL;
    input.release = NULL;
    input.rotation = PL_ROTATION_0;
    input.overlays = NULL;
    input.num_overlays = 0;
    if (!mp_motion_capture_supported(m, &input))
        return MP_MOTION_UNSUPPORTED;
    bool hdr = hdr_source(&input);
    frame->gpu = m->gpu;
    if (!recreate_color(m, &frame->texture, width, height, hdr))
        return MP_MOTION_ERROR;
    // Resolve the source before choosing a capture color space. An initial
    // tag may be replaced by representation-specific inference (e.g. XYZ).
    struct pl_frame target = captured_frame(frame->texture,
                                            &pl_color_space_unknown);
    target.repr.alpha = PL_ALPHA_PREMULTIPLIED;
    pl_frames_infer(m->renderer, &input, &target);
    if (!mp_motion_capture_supported(m, &input))
        return MP_MOTION_UNSUPPORTED;
    hdr = hdr_source(&input);
    struct pl_color_space presentation = input.color;
    if (hdr) {
        if (!recreate_color(m, &frame->texture, width, height, true))
            return MP_MOTION_ERROR;
        // Absolute PQ decoding must not consume display/authoring metadata.
        input.color.hdr = (struct pl_hdr_metadata) {
            .min_luma = PL_COLOR_HDR_BLACK, .max_luma = 10000,
        };
        struct pl_color_space working = input.color;
        working.transfer = PL_COLOR_TRC_LINEAR_HDR;
        target = captured_frame(frame->texture, &working);
        target.repr.alpha = PL_ALPHA_PREMULTIPLIED;
    } else {
        target.color = input.color;
    }
    if (!render_capture(m, &input, &target, hdr, true))
        return MP_MOTION_ERROR;
    frame->color = target.color;
    if (hdr)
        frame->color.hdr = presentation.hdr;
    frame->repr = target.repr;
    frame->domain = hdr ? MP_MOTION_HDR_LINEAR : MP_MOTION_SDR;
    frame->id = ++m->capture_id;
    frame->owner = m;
    return MP_MOTION_OK;
}

enum mp_motion_result mp_motion_capture_frame(
    struct mp_motion *m, const struct pl_frame *source, int width, int height,
    struct mp_motion_frame *frame)
{
    if (!frame)
        return MP_MOTION_ERROR;
    frame->id = 0;
    frame->owner = NULL;
    frame->color = (struct pl_color_space) {0};
    frame->repr = (struct pl_color_repr) {0};
    frame->domain = MP_MOTION_SDR;
    if (!m || !source || m->capture_id == UINT64_MAX ||
        (frame->texture && frame->gpu != m->gpu))
        return MP_MOTION_ERROR;
    if (width < 8 || height < 8)
        return MP_MOTION_UNSUPPORTED;

    struct pl_frame acquired = *source;
    bool ready = !acquired.acquire || acquired.acquire(m->gpu, &acquired);
    struct pl_frame enhancement = {0};
    bool acquired_el = false;
    if (ready && mp_motion_capture_supported(m, &acquired) &&
        acquired.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION &&
        acquired.repr.dovi->nlq_active)
    {
        enhancement = *acquired.enhancement_layer;
        ready = !enhancement.acquire ||
                enhancement.acquire(m->gpu, &enhancement);
        acquired_el = true;
        acquired.enhancement_layer = &enhancement;
    }
    struct pl_frame input = acquired;
    struct pl_frame render_el = enhancement;
    if (acquired_el) {
        // The core owns both acquisitions so it can classify metadata after
        // the callbacks. The renderer must not acquire the EL a second time.
        render_el.acquire = NULL;
        render_el.release = NULL;
        input.enhancement_layer = &render_el;
    }
    enum mp_motion_result result = ready
        ? capture_acquired_frame(m, &input, width, height, frame)
        : MP_MOTION_ERROR;
    if (acquired_el && enhancement.release)
        enhancement.release(m->gpu, &enhancement);
    if (acquired.release)
        acquired.release(m->gpu, &acquired);
    return result;
}

void mp_motion_frame_destroy(struct mp_motion_frame *frame)
{
    pl_tex_destroy(frame->gpu, &frame->texture);
    *frame = (struct mp_motion_frame) {0};
}

static bool warp_passes(struct mp_motion *m, float alpha,
                        enum motion_pass_mode mode)
{
    bool prepare = mode == MOTION_PASS_PREPARE;
    if (!prepare && m->warp_valid && m->warp_alpha == alpha)
        return true;
    m->warp_valid = false;
    float size[] = {m->source_width, m->source_height};
    float map_size[] = {m->images[0][0]->params.w, m->images[0][0]->params.h};
    for (int direction = 0; direction < 2; direction++) {
        if (!recreate(m, &m->warps[direction], map_size[0], map_size[1]))
            return false;
        struct pl_shader_desc warp_desc[] = {
            sampled("forward", m->reconstructed[direction]),
            sampled("backward", m->reconstructed[!direction]),
        };
        float weight = direction ? 1.0f - alpha : alpha;
        struct pl_shader_var warp_vars[] = {
            {.var = pl_var_vec2("output_size"), .data = size},
            {.var = pl_var_vec2("map_size"), .data = map_size},
            {.var = pl_var_float("weight"), .data = &weight, .dynamic = true},
        };
        m->timing_stage = LEVELS;
        if (!dispatch_prepared(m, &m->warp_pass[direction],
                               m->warps[direction], &(struct pl_custom_shader) {
            .description = "motion inverse warp",
            .header = synthesis_header, .body = warp_body,
            .descriptors = warp_desc, .num_descriptors = 2,
            .variables = warp_vars, .num_variables = 3,
        }, false, mode))
            return false;
    }
    if (!prepare) {
        m->warp_alpha = alpha;
        m->warp_valid = true;
    }
    return true;
}

static bool prepare_resources(struct mp_motion *m, pl_tex source, pl_tex second)
{
    if (m->prepared_format[0] == source->params.format &&
        m->prepared_format[1] == second->params.format &&
        m->prepared_sampler[0] == source->sampler_signature &&
        m->prepared_sampler[1] == second->sampler_signature &&
        m->prepared_width == source->params.w &&
        m->prepared_height == source->params.h)
        return true;

    destroy_passes(m);
    mp_motion_reset(m);
    m->source_width = source->params.w;
    m->source_height = source->params.h;
    // Compile the actual input configurations without reading either source,
    // executing analysis, or establishing a pair identity.
    const bool reuse[2] = {false, false};
    if (!analysis_passes(m, source, second, reuse, MOTION_PASS_PREPARE) ||
        !warp_passes(m, 0, MOTION_PASS_PREPARE)) {
        destroy_passes(m);
        return false;
    }
    m->prepared_format[0] = source->params.format;
    m->prepared_format[1] = second->params.format;
    m->prepared_sampler[0] = source->sampler_signature;
    m->prepared_sampler[1] = second->sampler_signature;
    m->prepared_width = source->params.w;
    m->prepared_height = source->params.h;
    return true;
}

static bool synthesize(struct mp_motion *m, pl_tex first, pl_tex second,
                       float alpha, pl_shader shader, int source_only)
{
    float size[] = {m->source_width, m->source_height};
    float map_size[] = {m->images[0][0]->params.w, m->images[0][0]->params.h};
    struct pl_shader_desc desc[] = {
        sampled_texels("first", first), sampled_texels("second", second),
        sampled("forward", m->warps[0]), sampled("backward", m->warps[1]),
        sampled("scene", m->scene),
    };
    int hdr = m->linear_hdr;
    float projection[2] = {0};
    if (!source_only) {
        float remaining = 1.0f - alpha;
        projection[0] = remaining / alpha;
        projection[1] = alpha / remaining;
    }
    struct pl_shader_var vars[] = {
        {.var = pl_var_vec2("output_size"), .data = size},
        {.var = pl_var_vec2("motion_size"), .data = map_size},
        {.var = pl_var_float("alpha"), .data = &alpha, .dynamic = true},
        {.var = pl_var_int("linear_hdr"), .data = &hdr},
        {.var = pl_var_vec2("projection_ratio"), .data = projection, .dynamic = true},
        {.var = pl_var_int("source_only"), .data = &source_only, .dynamic = true},
    };
    struct pl_custom_shader custom = {
        .description = "motion compensated interpolation",
        .header = synthesis_pixel_header,
        .body = synthesis_body,
        .descriptors = desc,
        .num_descriptors = sizeof(desc) / sizeof(desc[0]),
        .variables = vars,
        // The compute path does not bind source_only.
        .num_variables = sizeof(vars) / sizeof(vars[0]) - 1,
    };
    if (!shader) {
        m->timing_stage = LEVELS + 1;
        return dispatch(m, m->result, &custom);
    }

    int w = m->source_width, h = m->source_height;
    float corners[4][2] = {{0, 0}, {w, 0}, {0, h}, {w, h}};
    struct pl_shader_va position = {
        .attr = {.name = "motion_position", .fmt = m->position_format},
        .data = {corners[0], corners[1], corners[2], corners[3]},
    };
    custom.body = synthesis_shader_body;
    custom.num_variables = sizeof(vars) / sizeof(vars[0]);
    custom.vertex_attribs = &position;
    custom.num_vertex_attribs = 1;
    custom.output = PL_SHADER_SIG_COLOR;
    custom.output_w = w;
    custom.output_h = h;
    return pl_shader_custom(shader, &(struct pl_custom_shader) {
               .header = sample_header,
           }) && pl_shader_custom(shader, &custom);
}

static void record_synthesis(struct mp_motion *m)
{
    m->stats.generated_frames++;
    m->stats.synthesis_width = m->source_width;
    m->stats.synthesis_height = m->source_height;
}

static bool render(struct mp_motion *m, pl_tex first, pl_tex second,
                    uint64_t first_id, uint64_t second_id, float alpha,
                    enum motion_input_kind input_kind, bool linear_hdr,
                    pl_tex *output)
{
    if (output)
        *output = NULL;
    if (!m || !first || !second || !output || !isfinite(alpha) ||
        alpha <= 0 || alpha >= 1 || first->params.w != second->params.w ||
        first->params.h != second->params.h || first->params.w < 8 ||
        first->params.h < 8 || first->sampler_type != PL_SAMPLER_NORMAL ||
        second->sampler_type != PL_SAMPLER_NORMAL)
        return false;
    if (m->input_kind != input_kind || m->linear_hdr != linear_hdr) {
        mp_motion_reset(m);
        m->input_kind = input_kind;
        m->linear_hdr = linear_hdr;
    }
    if (first == second) {
        *output = first;
        return true;
    }
    pl_dispatch_reset_frame(m->dispatch);
    if (!prepare_resources(m, first, second) ||
        !analyze(m, first, second, first_id, second_id, MOTION_PASS_DEFER))
        return false;
    if (!recreate_color(m, &m->result, first->params.w, first->params.h,
                        linear_hdr))
        goto flush;
    if (m->result_valid && m->last_alpha == alpha) {
        m->stats.repeated_frames++;
        *output = m->result;
        return true;
    }
    m->result_valid = false;
    // Standalone synthesis submits the analysis and warp work it consumes.
    if (!warp_passes(m, alpha, MOTION_PASS_DEFER) ||
        !synthesize(m, first, second, alpha, NULL, 0) || pl_gpu_is_failed(m->gpu))
        goto flush;
    m->last_alpha = alpha;
    m->result_valid = true;
    record_synthesis(m);
    *output = m->result;
    return true;

flush:
    pl_gpu_flush(m->gpu);
    return false;
}

bool mp_motion_render(struct mp_motion *m, pl_tex first, pl_tex second,
                      uint64_t first_id, uint64_t second_id, float alpha,
                      pl_tex *output)
{
    return render(m, first, second, first_id, second_id, alpha, MOTION_INPUT_RAW,
                  false, output);
}

bool mp_motion_render_owned(struct mp_motion *m, pl_tex first, pl_tex second,
                            uint64_t first_id, uint64_t second_id, float alpha,
                            pl_tex *output)
{
    if (first == second) {
        if (output)
            *output = NULL;
        return false;
    }
    if (!mp_motion_render(m, first, second, first_id, second_id, alpha, output))
        return false;

    m->result = NULL;
    m->result_valid = false;
    return true;
}

static enum mp_motion_result render_frames(
    struct mp_motion *m, const struct mp_motion_frame *first,
    const struct mp_motion_frame *second, float alpha, bool owned,
    struct pl_frame *output)
{
    if (output)
        *output = (struct pl_frame) {0};
    if (!isfinite(alpha) || alpha <= 0 || alpha >= 1)
        return MP_MOTION_ERROR;
    if (!m || !first || !second || !output || !first->id || !second->id ||
        first->owner != m || second->owner != m ||
        first->gpu != m->gpu || second->gpu != m->gpu ||
        (owned && first->texture == second->texture) ||
        first->domain != second->domain ||
        (first->domain != MP_MOTION_SDR &&
         first->domain != MP_MOTION_HDR_LINEAR) ||
        !pl_color_repr_equal(&first->repr, &second->repr))
        return MP_MOTION_UNSUPPORTED;
    bool hdr = first->domain == MP_MOTION_HDR_LINEAR;
    struct pl_color_space color = first->color;
    struct pl_color_space other = second->color;
    if (hdr) {
        color.hdr = motion_hdr_metadata(&first->color.hdr);
        other.hdr = motion_hdr_metadata(&second->color.hdr);
        if (color.transfer != PL_COLOR_TRC_LINEAR_HDR ||
            color.primaries == PL_COLOR_PRIM_UNKNOWN ||
            first->repr.alpha != PL_ALPHA_PREMULTIPLIED)
            return MP_MOTION_UNSUPPORTED;
    }
    if (!pl_color_space_equal(&color, &other))
        return MP_MOTION_UNSUPPORTED;
    pl_tex texture;
    if (!render(m, first->texture, second->texture, first->id, second->id,
                alpha, MOTION_INPUT_CAPTURED, hdr, &texture))
        return MP_MOTION_ERROR;
    *output = captured_frame(texture, &color);
    output->repr = first->repr;
    if (owned) {
        m->result = NULL;
        m->result_valid = false;
    }
    return MP_MOTION_OK;
}

enum mp_motion_result mp_motion_render_frames(
    struct mp_motion *m, const struct mp_motion_frame *first,
    const struct mp_motion_frame *second, float alpha, struct pl_frame *output)
{
    return render_frames(m, first, second, alpha, false, output);
}

enum mp_motion_result mp_motion_render_frames_owned(
    struct mp_motion *m, const struct mp_motion_frame *first,
    const struct mp_motion_frame *second, float alpha, struct pl_frame *output)
{
    return render_frames(m, first, second, alpha, true, output);
}

static enum mp_motion_result validate_views(
    struct mp_motion *m, const struct pl_rendered_frame *first,
    const struct pl_rendered_frame *second, float alpha)
{
    if (!isfinite(alpha) || alpha <= 0 || alpha >= 1)
        return MP_MOTION_ERROR;
    if (!m || !first || !second || !m->hdr_format ||
        !first->signature || !second->signature)
        return MP_MOTION_UNSUPPORTED;
    pl_tex textures[] = {first->texture, second->texture};
    for (int i = 0; i < 2; i++) {
        pl_tex tex = textures[i];
        if (!tex || tex->params.w < 8 || tex->params.h < 8 || tex->params.d ||
            !tex->params.sampleable || tex->sampler_type != PL_SAMPLER_NORMAL)
            return MP_MOTION_UNSUPPORTED;
        pl_fmt fmt = tex->params.format;
        if (!fmt || fmt->type != PL_FMT_FLOAT || fmt->num_components != 4)
            return MP_MOTION_UNSUPPORTED;
        for (int c = 0; c < 4; c++) {
            if (fmt->component_depth[c] != 32)
                return MP_MOTION_UNSUPPORTED;
        }
    }
    if (textures[0]->params.w != textures[1]->params.w ||
        textures[0]->params.h != textures[1]->params.h)
        return MP_MOTION_UNSUPPORTED;
    return MP_MOTION_OK;
}

enum mp_motion_result mp_motion_prepare_view(
    struct mp_motion *m, const struct pl_rendered_frame *view)
{
    enum mp_motion_result result = validate_views(m, view, view, 0.5f);
    if (result != MP_MOTION_OK)
        return result;
    if (m->input_kind != MOTION_INPUT_VIEW || !m->linear_hdr) {
        mp_motion_reset(m);
        m->input_kind = MOTION_INPUT_VIEW;
        m->linear_hdr = true;
    }
    return prepare_resources(m, view->texture, view->texture)
        ? MP_MOTION_OK : MP_MOTION_ERROR;
}

enum mp_motion_result mp_motion_render_views(
    struct mp_motion *m, const struct pl_rendered_frame *first,
    const struct pl_rendered_frame *second, float alpha, pl_tex *output)
{
    if (output)
        *output = NULL;
    enum mp_motion_result result = validate_views(m, first, second, alpha);
    if (result != MP_MOTION_OK)
        return result;
    if (!output)
        return MP_MOTION_UNSUPPORTED;
    if (!render(m, first->texture, second->texture, first->signature, second->signature,
                alpha, MOTION_INPUT_VIEW, true, output))
        return MP_MOTION_ERROR;
    return MP_MOTION_OK;
}

bool mp_motion_shader_supported(const struct mp_motion *m)
{
    return m && m->position_format;
}

enum mp_motion_result mp_motion_append_single_view_shader(
    struct mp_motion *m, const struct pl_rendered_frame *view, pl_shader shader)
{
    if (!shader || !mp_motion_shader_supported(m))
        return MP_MOTION_UNSUPPORTED;
    enum mp_motion_result result = mp_motion_prepare_view(m, view);
    if (result != MP_MOTION_OK)
        return result;
    // Descriptor bindings must be distinct even when a branch does not read them.
    return synthesize(m, view->texture, m->images[0][0], 0, shader, 1)
        ? MP_MOTION_OK : MP_MOTION_ERROR;
}

enum mp_motion_result mp_motion_append_view_shader(
    struct mp_motion *m, const struct pl_rendered_frame *first,
    const struct pl_rendered_frame *second, float alpha, pl_shader shader)
{
    enum mp_motion_result result = validate_views(m, first, second, alpha);
    if (result != MP_MOTION_OK)
        return result;
    if (!shader || !mp_motion_shader_supported(m))
        return MP_MOTION_UNSUPPORTED;
    if (m->input_kind != MOTION_INPUT_VIEW || !m->linear_hdr) {
        mp_motion_reset(m);
        m->input_kind = MOTION_INPUT_VIEW;
        m->linear_hdr = true;
    }
    pl_dispatch_reset_frame(m->dispatch);
    if (!prepare_resources(m, first->texture, second->texture) ||
        !analyze(m, first->texture, second->texture, first->signature,
                 second->signature, MOTION_PASS_RUN) ||
        !warp_passes(m, alpha, MOTION_PASS_RUN) ||
        !synthesize(m, first->texture, second->texture, alpha, shader, 0))
        return MP_MOTION_ERROR;
    return MP_MOTION_OK;
}

void mp_motion_record_shader_output(struct mp_motion *m)
{
    record_synthesis(m);
    // The fused output no longer has a standalone synthesis timer.
    m->stats.synthesis_ns = 0;
}
