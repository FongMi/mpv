/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef MP_GPU_NEXT_MOTION_H
#define MP_GPU_NEXT_MOTION_H

#include <stdbool.h>
#include <stdint.h>
#include <libplacebo/renderer.h>

struct mp_motion;

enum mp_motion_result {
    MP_MOTION_ERROR = -1,
    MP_MOTION_UNSUPPORTED = 0,
    MP_MOTION_OK = 1,
};

enum mp_motion_domain {
    MP_MOTION_SDR,
    MP_MOTION_HDR_LINEAR,
};

// Caller-owned storage; do not shallow-copy ownership. A zero identity is
// invalid. Every successful capture publishes a new content identity.
struct mp_motion_frame {
    pl_tex texture;
    pl_gpu gpu;
    struct pl_color_space color;
    struct pl_color_repr repr;
    enum mp_motion_domain domain;
    uint64_t id;
    const struct mp_motion *owner;
};

struct mp_motion_stats {
    uint64_t analyzed_pairs;
    uint64_t generated_frames;
    uint64_t repeated_frames;
    // Rolling GPU means; zero means no completed timer sample is available.
    uint64_t capture_ns;
    uint64_t translation_cost_ns;
    uint64_t translation_select_ns;
    uint64_t coarse_ns;
    uint64_t refinement_ns;
    uint64_t evidence_ns;
    uint64_t reconstruction_ns;
    uint64_t warp_ns;
    uint64_t synthesis_ns;
    int analysis_width, analysis_height;
    int synthesis_width, synthesis_height;
};

#ifdef __cplusplus
extern "C" {
#endif

bool mp_motion_supported(pl_gpu gpu);
bool mp_motion_hdr_supported(pl_gpu gpu);
// Query finalized source metadata without acquiring or modifying GPU state.
// An acquire callback may still change the description; capture rechecks it.
bool mp_motion_capture_supported(const struct mp_motion *motion,
                                  const struct pl_frame *source);
struct mp_motion *mp_motion_create(pl_gpu gpu, int max_dimension);
void mp_motion_destroy(struct mp_motion **motion);
void mp_motion_reset(struct mp_motion *motion);
struct mp_motion_stats mp_motion_get_stats(const struct mp_motion *motion);

// Capture into a caller-owned RGB texture. Source acquire/release callbacks
// remain responsible for decoder ownership; no source pixels are read back.
bool mp_motion_capture(struct mp_motion *motion, const struct pl_frame *source,
                       int width, int height, pl_tex *texture);

// Capture a described working frame. PQ uses FP32 linear premultiplied RGB
// in the effective source primaries; 1.0 is PL_COLOR_SDR_WHITE cd/m2. The
// HDR-linear transfer preserves HDR identity even below SDR white. Color
// metadata describes the source presentation, not new capture measurements.
// DV is reshaped and, when required, composed with the FEL first. Its decoded
// working space is BT.2020; the RGB record carries no RPU or enhancement layer.
// Dynamic presentation metadata (including DV L1), other HDR transfers,
// HDR ICC/LUT processing and field views are unsupported.
// SDR retains the encoded-RGB domain. Classification follows source acquire;
// BL/EL acquire/release remain balanced, including acquisition failure.
// Failure invalidates the identity but retains storage. Zero-initialize new
// records; their GPU must outlive storage, and their owner must outlive use.
enum mp_motion_result mp_motion_capture_frame(
    struct mp_motion *motion, const struct pl_frame *source, int width, int height,
    struct mp_motion_frame *frame);
void mp_motion_frame_destroy(struct mp_motion_frame *frame);

// Match working-frame identities, domains and effective color representation.
// Both inputs must remain alive. Output is borrowed until render/reset/destroy.
// HDR requires identical effective static mastering and MaxCLL. Output remains
// FP32 linear premultiplied RGB at the same 203 nit scale with common mastering
// metadata. MaxFALL is unknown because warping can change the mean.
// Incompatible inputs return UNSUPPORTED; invalid phases/GPU failures return
// ERROR. Both invalidate output. The final renderer owns display adaptation.
enum mp_motion_result mp_motion_render_frames(
    struct mp_motion *motion, const struct mp_motion_frame *first,
    const struct mp_motion_frame *second, float alpha, struct pl_frame *output);

#define MP_MOTION_HAVE_OWNED_FRAMES 1
// Like mp_motion_render_frames, but requires distinct input textures and
// transfers the output texture to the caller. The texture survives later
// renders, reset and motion destruction. Release it with pl_tex_destroy on
// the same GPU; release any previous output before reusing its descriptor.
enum mp_motion_result mp_motion_render_frames_owned(
    struct mp_motion *motion, const struct mp_motion_frame *first,
    const struct mp_motion_frame *second, float alpha, struct pl_frame *output);

// Initialize the analysis and warp resources for a real prepared view. This
// does not sample pixels, analyze a pair or publish output/statistics. The view
// has the same format/GPU/lifetime contract as render_views; it is not retained.
enum mp_motion_result mp_motion_prepare_view(
    struct mp_motion *motion, const struct pl_rendered_frame *view);

// Borrow prepared views from one libplacebo custom-mixer callback. The caller
// guarantees the same GPU and common display-linear RGB, premultiplied alpha,
// with 1.0 equal to PL_COLOR_SDR_WHITE cd/m2. Textures must be sampleable 2D
// RGBA FP32 with normal samplers and matching dimensions
// of at least 8x8.
// Signatures must be nonzero and identify rendered pixels. Reset motion when
// changing the renderer that produces these signatures. Their namespace is
// independent of raw source IDs and owned capture IDs.
// No input ownership is acquired. Output is borrowed until render/reset/destroy,
// or may alias an input; consume it within the custom-mixer output lifetime.
// Incompatible inputs return UNSUPPORTED; invalid phases/GPU failures return
// ERROR. Both clear output. GPU ownership and color cannot be checked here.
enum mp_motion_result mp_motion_render_views(
    struct mp_motion *motion, const struct pl_rendered_frame *first,
    const struct pl_rendered_frame *second, float alpha, pl_tex *output);

bool mp_motion_shader_supported(const struct mp_motion *motion);
// Append actual single-source presentation without analyzing or recording a pair.
// The prepared view must remain alive until the renderer finishes the draw.
enum mp_motion_result mp_motion_append_single_view_shader(
    struct mp_motion *motion, const struct pl_rendered_frame *view, pl_shader shader);

// Append the same synthesis directly to a renderer-owned output shader. The
// prepared views must remain alive until the renderer finishes the draw.
enum mp_motion_result mp_motion_append_view_shader(
    struct mp_motion *motion, const struct pl_rendered_frame *first,
    const struct pl_rendered_frame *second, float alpha, pl_shader shader);
// Call once after a successful pair render using the appended shader.
void mp_motion_record_shader_output(struct mp_motion *motion);

// Cache analysis by source identity. The returned texture is borrowed until
// the next render/reset/destroy; both sources must remain alive during render.
bool mp_motion_render(struct mp_motion *motion, pl_tex first, pl_tex second,
                      uint64_t first_id, uint64_t second_id, float alpha,
                      pl_tex *output);

// Transfer the synthesis texture to the caller. Sources must be distinct.
// The caller destroys the result on the GPU owner thread; later operations on
// motion do not invalidate it. On failure, output is cleared.
bool mp_motion_render_owned(struct mp_motion *motion, pl_tex first, pl_tex second,
                            uint64_t first_id, uint64_t second_id, float alpha,
                            pl_tex *output);

#ifdef __cplusplus
}
#endif

#endif
