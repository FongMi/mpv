// GPU/codec stubs for the extracted gpu-next slot lifecycle. The parameter
// structures and comparison functions are extracted from mpv as well.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK mp_mutex;
#define mp_mutex_lock AcquireSRWLockExclusive
#define mp_mutex_unlock ReleaseSRWLockExclusive
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t mp_mutex;
#define mp_mutex_lock pthread_mutex_lock
#define mp_mutex_unlock pthread_mutex_unlock
#endif

#include "../video/out/gpu_next/motion_clock.h"

#define mp_assert assert
#define mp_require assert
#define MPMIN(a,b) ((a) < (b) ? (a) : (b))
#define MP_TARRAY_GROW(ctx, array, index) do { (void)(ctx); assert((index) < 16); } while (0)
#define MP_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static char error_log[512];
#define MP_ERR(p, ...) ((void)(p), snprintf(error_log, sizeof(error_log), __VA_ARGS__))
#define MP_INFO(p, ...) ((void)(p))
struct pl_hook_par;
static void grow_source_parameters(void *address, int count)
{
    const struct pl_hook_par ***array = address;
    const struct pl_hook_par **next = malloc(((size_t)count + 1) * sizeof(*next));
    assert(next && next != *array);
    if (count)
        memcpy(next, *array, (size_t)count * sizeof(*next));
    free(*array);
    *array = next;
}
#define MP_TARRAY_APPEND(ctx, array, count, value) \
    do { (void)(ctx); \
        if (_Generic((array), const struct pl_hook_par **: 1, default: 0)) \
            grow_source_parameters(&(array), (count)); \
        (array)[(count)++] = (value); } while (0)

#define MP_TARRAY_REMOVE_AT(array, count, index) \
    do { memmove(&(array)[index], &(array)[(index) + 1], \
                 (--(count) - (index)) * sizeof((array)[0])); } while (0)

enum mp_imgfmt { IMGFMT_MEDIACODEC, IMGFMT_RGB0, IMGFMT_YUV420P };
#define IMGFMT_IS_HWACCEL(fmt) ((fmt) == IMGFMT_MEDIACODEC)
enum pl_color_primaries { PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_BT_2020 };
enum pl_color_transfer {
    PL_COLOR_TRC_BT_1886, PL_COLOR_TRC_PQ, PL_COLOR_TRC_LINEAR,
    PL_COLOR_TRC_LINEAR_HDR, PL_COLOR_TRC_SRGB, PL_COLOR_TRC_HLG,
};
enum pl_color_system {
    PL_COLOR_SYSTEM_BT_709, PL_COLOR_SYSTEM_DOLBYVISION, PL_COLOR_SYSTEM_RGB, PL_COLOR_SYSTEM_XYZ,
};
enum pl_alpha_mode { PL_ALPHA_PREMULTIPLIED, PL_ALPHA_INDEPENDENT };
enum mp_csp_light { MP_CSP_LIGHT_AUTO };
enum pl_chroma_location { PL_CHROMA_LEFT, PL_CHROMA_CENTER };
enum mp_stereo3d_mode { MP_STEREO3D_MONO };

struct pl_hdr_metadata { float max_luma; };
struct pl_color_space {
    enum pl_color_primaries primaries;
    enum pl_color_transfer transfer;
    struct pl_hdr_metadata hdr;
};
struct pl_bit_encoding { int sample_depth, color_depth, bit_shift; };
struct pl_color_repr {
    struct pl_bit_encoding bits;
    enum pl_color_system sys;
    const void *dovi;
    enum pl_alpha_mode alpha;
};
struct mp_rect { int x0, y0, x1, y1; };
struct pl_rect2df { float x0, y0, x1, y1; };
#define MP_IMGFIELD_INTERLACED 1
#define MP_IMGFIELD_TOP_FIRST 2
enum pl_field { PL_FIELD_NONE, PL_FIELD_TOP, PL_FIELD_BOTTOM };
static enum pl_field pl_field_other(enum pl_field field)
{
    return field == PL_FIELD_TOP ? PL_FIELD_BOTTOM : PL_FIELD_TOP;
}
enum pl_source_peak_status { PL_SOURCE_PEAK_UNSUPPORTED = -2,
    PL_SOURCE_PEAK_ERROR = -1, PL_SOURCE_PEAK_PENDING, PL_SOURCE_PEAK_READY };
struct mp_pass_perf { int count; };
typedef void *pl_tex;
typedef void *pl_shader;
typedef void *pl_gpu;
struct mp_motion;

struct pl_plane { pl_tex texture; int components, component_mapping[4]; };
struct pl_frame {
    enum pl_chroma_location chroma_location;
    int rotation, num_planes;
    struct pl_rect2df crop;
    void *user_data;
    struct pl_plane planes[4];
    struct pl_color_space color;
    struct pl_color_repr repr;
    float pixel_aspect_ratio;
    const void *overlays, *enhancement_layer;
    const void *icc, *lut;
    struct { const void *data; size_t len; } profile;
    int num_overlays, field, first_field;
    bool (*acquire)(pl_gpu, struct pl_frame *);
    void (*release)(pl_gpu, struct pl_frame *);
};

#include "vo_gpu_next_contracts.h"

struct decoder_buffer { bool consumed; int retries; };

struct mp_image {
    struct mp_image_params params;
    void *priv;
    int imgfmt, pixel_id, retries, num_planes, fields;
    double pts, pkt_duration;
    struct mp_image *enhancement_layer;
    bool consumed;
    bool owns_el, owned_ref;
    struct decoder_buffer *buffer; // Shared by all references to one decoder output.
};
struct frame_priv {
    struct { struct { pl_tex tex; } entries[1]; } subs;
    struct vo *vo;
    struct ra_hwdec *hwdec, *el_hwdec;
    struct source_frame *source;
    pl_tex el_tex[4];
    bool reset_preload, motion_capture, mapped_coordinates;
    struct mp_image_params mapped_params;
    pl_tex motion_texture;
    uint64_t motion_id;
    struct mp_motion_frame motion_frame;
    pl_tex native_tex[4];
    struct pl_frame native_frame, view_frame;
    bool native_ready;
    enum pl_source_peak_status peak_status;
};
struct pl_frame_mix {
    int num_frames;
    const struct pl_frame *const *frames;
    const double *timestamps;
    const uint64_t *signatures;
    double vsync_duration;
};
struct ra { bool uses_pl; };
struct ra_ctx { struct ra *ra; };
struct ra_hwdec_mapper_driver { bool map_on_reset, consumes_source; };
struct ra_hwdec_driver { const struct ra_hwdec_mapper_driver *mapper; };
struct ra_hwdec { struct ra *ra; const struct ra_hwdec_driver *driver; };
struct timer_pool { int unused; };
struct gl_next_opts { int motion_fps; };
struct fixture_options { struct { int num_hooks; const void *peak_detect_params; } params; };
struct priv {
    void *gpu, *stats;
    void *source_peak, *rr;
    uint64_t source_epoch;
    uint64_t source_ids[16];
    char *source_lut_path;
    int source_lut_type, source_srgb_power22;
    float source_ref_luma;
    bool motion_views, hwdec_map_retry;
    struct fixture_options *pars;
    const struct pl_hook *hooks[2];
    const struct pl_hook_par **source_parameters;
    void *hwdec_dispatch, *pllog, *queue;
    pl_tex sub_tex[4];
    int num_sub_tex;
    struct ra_hwdec *hwdec_ctx;
    struct hwdec_slot hwdec, el_hwdec;
    struct mp_image *pending_frame;
    struct source_frame *source_frames[32];
    int num_source_frames;
    bool hwdec_map_failed, flush_cache;
    bool queue_fields;
    uint64_t last_id, last_source_id, reset_map_failed_id;
    bool want_seek_reset, want_reset;
    double last_pts;
    struct ra_ctx *ra_ctx;
    void *motion;
    struct mp_motion_clock motion_clock;
    int motion_fps;
    enum motion_state motion_state;
    bool paused;
    uint64_t motion_captured, motion_submitted;
    int64_t motion_epoch;
    struct gl_next_opts *next_opts;
    bool motion_bypass_reported;
    uint64_t motion_last_captured, motion_last_submitted;
    uint64_t motion_last_generated;
    int64_t motion_stats_time;
};
struct vo_frame {
    uint64_t frame_id;
    int num_frames;
    struct mp_image *current;
    struct mp_image **frames;
    double ideal_frame_duration;
    double ideal_frame_vsync_duration;
    double vsync_interval;
    double approx_duration;
    bool repeat;
    bool redraw;
};

enum { CONF_TYPE_INT, CONF_TYPE_INT64, CONF_TYPE_STRING, CONF_TYPE_BOOL,
       CONF_TYPE_NODE };
enum { M_PROPERTY_GET, M_PROPERTY_GET_TYPE, M_PROPERTY_OK = 1,
       M_PROPERTY_UNAVAILABLE = -1 };
struct m_option { int type; };
struct m_property { int unused; };
struct m_sub_property {
    const char *name;
    struct m_option type;
    union { int int_; int64_t int64; char *string; bool bool_; } value;
    bool unavailable;
};
struct vo_internal {
    mp_mutex lock;
    struct voctrl_motion_info motion_info;
    bool motion_info_valid;
};
struct vo { struct priv *priv; struct vo_internal *in; };
typedef struct { struct vo *video_out; } MPContext;

static struct mp_motion_stats motion_stats;
static struct m_sub_property motion_properties[24];
static bool gpu_supported = true;
static bool motion_create_fail;
static int motion_create_calls, motion_destroy_calls, motion_queue_resets;
static int motion_render_calls;
static bool motion_render_fail;
static float motion_render_alpha;
static int described_render_calls;
static bool described_unsupported;

static bool mp_motion_render(void *motion, pl_tex first, pl_tex second,
                             uint64_t first_id, uint64_t second_id, float alpha,
                             pl_tex *output)
{
    assert(motion && first == (void *)1 && second == (void *)2);
    assert(first_id == 10 && second_id == 11);
    motion_render_calls++;
    motion_render_alpha = alpha;
    if (!isfinite(alpha) || alpha <= 0 || alpha >= 1 || motion_render_fail)
        return false;
    *output = (void *)3;
    return true;
}

static enum mp_motion_result mp_motion_render_frames(
    void *motion, const struct mp_motion_frame *first,
    const struct mp_motion_frame *second, float alpha, struct pl_frame *output)
{
    assert(motion && first->texture == (void *)1 &&
           second->texture == (void *)2);
    assert(first->id == 10 && second->id == 11);
    described_render_calls++;
    motion_render_alpha = alpha;
    *output = (struct pl_frame) {0};
    if (described_unsupported)
        return MP_MOTION_UNSUPPORTED;
    if (!isfinite(alpha) || alpha <= 0 || alpha >= 1 || motion_render_fail)
        return MP_MOTION_ERROR;
    *output = (struct pl_frame) {
        .planes[0].texture = (void *)3,
        .color = {
            .primaries = PL_COLOR_PRIM_BT_2020, .transfer = PL_COLOR_TRC_LINEAR_HDR,
            .hdr.max_luma = 1000,
        },
        .repr = {
            .sys = PL_COLOR_SYSTEM_RGB, .alpha = PL_ALPHA_PREMULTIPLIED,
        },
    };
    return MP_MOTION_OK;
}

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

static void discard_queued_frames(void);
static void pl_queue_reset(void *queue)
{
    (void)queue;
    motion_queue_resets++;
    discard_queued_frames();
}
static void mp_motion_reset(void *motion) { (void)motion; }

static int64_t mp_time_ns(void) { return 123456789; }
static void get_motion_info(struct priv *p, struct voctrl_motion_info *info);

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
static int live_copies, destroyed_copies, copy_calls, fail_copy_at;
static int accesses, releases, freed_frames;
static bool stale_success, deferred_params = true;
static struct mp_image *latest_image;
static struct decoder_buffer decoder_buffers[128];
static int num_decoder_buffers;
static struct mp_image retained[3];
static int retained_count, retained_capacity, release_requests, stale_reuses;
static pl_tex owned_copies[128];
static void pl_tex_destroy(void *gpu, pl_tex *tex)
{
    (void)gpu;
    for (int n = 0; n < 128; n++) {
        if (*tex && owned_copies[n] == *tex) {
            free(*tex);
            owned_copies[n] = NULL;
            live_copies--;
            destroyed_copies++;
            break;
        }
    }
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
    mapper->driver = hwdec->driver ? hwdec->driver->mapper : NULL;
    mapper->ra = hwdec->ra;
    mapper->src_params = mapper->dst_params = *params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    return mapper;
}

static int ra_hwdec_mapper_map(struct ra_hwdec_mapper *mapper,
                               struct mp_image *image)
{
    calls.maps++;
    int *retries = image->buffer ? &image->buffer->retries : &image->retries;
    bool *consumed = image->buffer ? &image->buffer->consumed : &image->consumed;
    if (*retries > 0) {
        (*retries)--;
        return -2;
    }
    if (retained_capacity) {
        assert(image->buffer);
        for (int n = 0; n < retained_count; n++) {
            if (retained[n].buffer == image->buffer) {
                mapper->src = &retained[n];
                return 0;
            }
        }
        release_requests++;
        if (*consumed) {
            assert(retained_count);
            stale_reuses++;
            mapper->src = &retained[retained_count - 1];
        } else {
            *consumed = true;
            if (retained_count == retained_capacity) {
                memmove(retained, retained + 1,
                        (retained_capacity - 1) * sizeof(retained[0]));
                retained_count--;
            }
            retained[retained_count++] = *image;
            mapper->src = &retained[retained_count - 1];
        }
    } else {
        if (*consumed) {
            if (!stale_success)
                return -1;
            mapper->src = latest_image;
            return 0;
        }
        *consumed = true;
        latest_image = image;
        mapper->src = image;
    }
    mapper->dst_params.imgfmt = image->num_planes == 2
        ? IMGFMT_YUV420P : IMGFMT_RGB0;
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

#define PL_API_VER 367
#define PL_HAVE_PREPARED_FIELD_MIXER 1
#define RA_HWDEC_MAP_RETRY (-2)
struct pl_source_frame {
    struct mp_image *frame_data;
    double pts, duration;
    enum pl_field first_field;
    bool (*map)(pl_gpu, pl_tex *, const struct pl_source_frame *, struct pl_frame *);
    void (*unmap)(pl_gpu, struct pl_frame *, const struct pl_source_frame *);
    void (*discard)(const struct pl_source_frame *);
};
static struct pl_source_frame queued_frames[8];
static int num_queued_frames;
#define queued_frame queued_frames[0]
static void discard_queued_frames(void)
{
    for (int n = 0; n < num_queued_frames; n++)
        queued_frames[n].discard(&queued_frames[n]);
    memset(queued_frames, 0, sizeof(queued_frames));
    num_queued_frames = 0;
}
static int queue_pushes, image_refs, live_image_refs;
static void pl_queue_push(void *queue, const struct pl_source_frame *frame)
{
    (void)queue;
    assert(num_queued_frames < MP_ARRAY_SIZE(queued_frames));
    queued_frames[num_queued_frames++] = *frame;
    queue_pushes++;
}
static bool map_frame(pl_gpu gpu, pl_tex *tex, const struct pl_source_frame *src,
                       struct pl_frame *frame)
{
    (void)gpu; (void)tex; (void)src; (void)frame;
    abort(); // Only callback identity is tested; no fake production mapping.
}
static struct mp_image *mp_image_new_ref(struct mp_image *src)
{
    struct mp_image *image = malloc(sizeof(*image));
    assert(image);
    if (!src->buffer) {
        assert(num_decoder_buffers < MP_ARRAY_SIZE(decoder_buffers));
        src->buffer = &decoder_buffers[num_decoder_buffers++];
        *src->buffer = (struct decoder_buffer) {src->consumed, src->retries};
    }
    *image = *src;
    image->priv = NULL;
    image->owned_ref = true;
    live_image_refs++;
    image->owns_el = !!src->enhancement_layer;
    if (image->owns_el)
        image->enhancement_layer = mp_image_new_ref(src->enhancement_layer);
    image_refs++;
    return image;
}
#define talloc_zero(ctx, type) ((void)(ctx), (type *)calloc(1, sizeof(type)))
static void note_hwdec_map_error(struct priv *p, int result, const char *op)
{
    (void)op;
    p->hwdec_map_failed |= result != RA_HWDEC_MAP_RETRY;
}
static int ra_hwdec_mapper_begin_access(struct ra_hwdec_mapper *mapper)
{
    assert(mapper->src);
    accesses++;
    return 0;
}
static void ra_hwdec_mapper_end_access(struct ra_hwdec_mapper *mapper)
{
    (void)mapper;
    releases++;
}
static pl_tex hwdec_get_tex(struct priv *p, struct ra_hwdec_mapper *mapper, int n)
{
    (void)p;
    (void)n;
    return &mapper->src->pixel_id;
}
struct mp_imgfmt_desc {
    int num_planes, num_components;
    struct { int plane; uint8_t offset; } comps[4];
};
static struct mp_imgfmt_desc mp_imgfmt_get_desc(int format)
{
    if (format == IMGFMT_YUV420P)
        return (struct mp_imgfmt_desc) {
            .num_planes = 2, .num_components = 3,
            .comps = {{0, 0}, {1, 0}, {1, 8}},
        };
    return (struct mp_imgfmt_desc) {
        .num_planes = 1, .num_components = 4,
        .comps = {{0, 16}, {0, 8}, {0, 0}, {0, 24}},
    };
}
static int mp_imgfmt_desc_get_num_comps(const struct mp_imgfmt_desc *desc)
{
    return desc->num_components;
}
static void mp_image_params_guess_csp(struct mp_image_params *params)
{
    // Make preservation observable: raw mapper representation must survive this.
    params->repr.sys = PL_COLOR_SYSTEM_RGB;
    params->repr.dovi = NULL;
    params->repr.bits = (struct pl_bit_encoding){8, 8, 0};
}
static void pl_frame_set_chroma_location(struct pl_frame *frame,
                                          enum pl_chroma_location location)
{
    frame->chroma_location = location;
}
static void pl_dispatch_reset_frame(void *dispatch) { assert(dispatch); }
static void *pl_dispatch_create(void *log, pl_gpu gpu)
{
    (void)log;
    (void)gpu;
    return (void *)1;
}
static bool copy_hwdec_plane(pl_gpu gpu, void *dispatch, pl_tex src,
                              int components, pl_tex *out)
{
    (void)gpu;
    assert(dispatch && src && components >= 1 && components <= 4);
    if (++copy_calls == fail_copy_at)
        return false;
    int *pixel = malloc(sizeof(*pixel));
    assert(pixel);
    *pixel = *(int *)src;
    for (int n = 0; n < 128; n++) {
        if (!owned_copies[n]) {
            owned_copies[n] = pixel;
            live_copies++;
            *out = pixel;
            return true;
        }
    }
    abort();
}
static struct ra_hwdec *ra_hwdec_get(struct ra_hwdec **ctx, int format)
{
    return format == -12345 ? NULL : *ctx;
}
static bool hwdec_driver_uses_deferred_dst_params(
    struct priv *p, const struct ra_hwdec_mapper_driver *driver)
{
    (void)p;
    return driver && deferred_params;
}
static void mp_motion_frame_destroy(struct mp_motion_frame *frame)
{
    pl_tex_destroy(frame->gpu, &frame->texture);
    *frame = (struct mp_motion_frame){0};
}

static const struct pl_color_repr pl_color_repr_rgb = {
    .sys = PL_COLOR_SYSTEM_RGB,
};
static bool capture_supported = true, capture_legacy_ok = true;
static int capture_typed_calls, capture_legacy_calls;
static enum mp_motion_result capture_result = MP_MOTION_OK;
static struct pl_frame capture_input;
static int capture_width, capture_height;

static bool mp_image_crop_valid(const struct mp_image_params *params)
{
    return params->crop.x0 >= 0 && params->crop.y0 >= 0 &&
           params->crop.x1 > params->crop.x0 &&
           params->crop.y1 > params->crop.y0 &&
           params->crop.x1 <= params->w && params->crop.y1 <= params->h;
}

static bool mp_motion_capture_supported(void *motion,
                                        const struct pl_frame *source)
{
    assert(motion && source);
    return capture_supported;
}

static enum mp_motion_result mp_motion_capture_frame(
    void *motion, const struct pl_frame *source, int width, int height,
    struct mp_motion_frame *frame)
{
    capture_typed_calls++;
    capture_input = *source;
    capture_width = width;
    capture_height = height;
    if (capture_result == MP_MOTION_OK) {
        *frame = (struct mp_motion_frame) {
            .texture = (void *)9, .owner = motion, .id = 101,
            .domain = MP_MOTION_HDR_LINEAR,
            .color = {
                .primaries = PL_COLOR_PRIM_BT_2020,
                .transfer = PL_COLOR_TRC_LINEAR_HDR, .hdr.max_luma = 1000,
            },
            .repr = {.sys = PL_COLOR_SYSTEM_RGB,
                     .alpha = PL_ALPHA_PREMULTIPLIED},
        };
    }
    return capture_result;
}

static bool mp_motion_capture(void *motion, const struct pl_frame *source,
                              int width, int height, pl_tex *texture)
{
    assert(motion);
    capture_legacy_calls++;
    capture_input = *source;
    capture_width = width;
    capture_height = height;
    if (capture_legacy_ok)
        *texture = (void *)8;
    return capture_legacy_ok;
}
static void free_image(struct mp_image *image)
{
    freed_frames++;
    if (image->owned_ref) {
        assert(live_image_refs > 0);
        live_image_refs--;
    }
    if (image->owns_el)
        free_image(image->enhancement_layer);
    free(image->priv);
    free(image);
}

#define talloc_free(p) _Generic((p), struct mp_image *: free_image, default: free)(p)

enum { PL_LUT_UNKNOWN, PL_LUT_NATIVE, PL_LUT_NORMALIZED, PL_LUT_CONVERSION };
enum { PL_VAR_FLOAT, PL_VAR_SINT, PL_VAR_UINT };
typedef union { float f; int i; unsigned u; } pl_var_data;
struct pl_hook_par { const char *name; int type; pl_var_data *data; };
struct pl_hook { const struct pl_hook_par *parameters; int num_parameters; };
enum { PL_DEINTERLACE_WEAVE, PL_DEINTERLACE_BOB, PL_DEINTERLACE_ALGORITHM_COUNT };
struct pl_deinterlace_params { int algo; };
struct pl_render_params {
    bool disable_fbos, force_low_bit_depth_fbos;
    const struct pl_deinterlace_params *deinterlace_params;
    const void *lut;
    int lut_type;
    const struct pl_hook_par *const *source_parameters;
    int num_source_parameters;
};
struct pl_rendered_frame { pl_tex texture; uint64_t signature; double timestamp; };
struct pl_rendered_mix { const struct pl_rendered_frame *frames; int num_frames; };
struct pl_custom_frame_mixer { void *priv; pl_tex (*mix)(void *, const struct pl_rendered_mix *); };
struct pl_frame_shader_mixer {
    void *priv;
    bool (*mix)(void *, const struct pl_rendered_mix *, pl_shader);
};
struct pl_source_peak_context {
    void *state; uint64_t epoch; const uint64_t *ids;
    uint64_t current_id, retire_before_id;
};
static bool render_boundary_ok = true;
struct pl_render_errors { unsigned errors; };
static unsigned renderer_error_mask;
static int renderer_error_reads;
static struct pl_render_errors pl_renderer_get_errors(void *rr)
{
    (void)rr;
    renderer_error_reads++;
    return (struct pl_render_errors) {.errors = renderer_error_mask};
}
static bool skip_motion_callback;
static int prepared_count = 2;
static int timed_calls, untimed_calls, direct_calls, view_flow_calls, infer_calls;
static enum mp_motion_result view_flow_result = MP_MOTION_OK;
static bool shader_supported;
static int shader_calls, shader_outputs;
static struct pl_source_peak_context routed_context;
static uint64_t routed_ids[16], routed_signatures[16];
static int routed_count;
static bool routed_custom;
static float routed_alpha;
static struct pl_rendered_frame routed_views[2];
static int pl_rotation_normalize(int rotation) { return (rotation % 4 + 4) % 4; }
static void pl_frames_infer(void *rr, struct pl_frame *source, struct pl_frame *target)
{
    (void)rr; (void)source; (void)target;
    infer_calls++;
}
static int view_prepare_calls;
static enum mp_motion_result mp_motion_prepare_view(
    struct mp_motion *motion, const struct pl_rendered_frame *view)
{
    assert(motion && view->texture);
    view_prepare_calls++;
    return view_flow_result;
}
static enum mp_motion_result mp_motion_render_views(
    struct mp_motion *motion, const struct pl_rendered_frame *first,
    const struct pl_rendered_frame *second, float alpha, pl_tex *output)
{
    (void)motion;
    view_flow_calls++;
    routed_views[0] = *first; routed_views[1] = *second;
    routed_alpha = alpha;
    *output = view_flow_result == MP_MOTION_OK ? (void *)99 : NULL;
    return view_flow_result;
}
static bool pl_render_image_mix_source(void *rr, const struct pl_frame_mix *mix,
    const struct pl_frame *target, const struct pl_render_params *params,
    const struct pl_source_peak_context *source, const struct pl_custom_frame_mixer *custom)
{
    (void)rr; (void)target; (void)params;
    timed_calls++;
    routed_context = *source;
    routed_count = mix->num_frames;
    routed_custom = !!custom;
    for (int i = 0; i < mix->num_frames; i++) {
        routed_ids[i] = source->ids[i];
        routed_signatures[i] = mix->signatures[i];
    }
    if (custom) {
        if (skip_motion_callback)
            return false;
        struct pl_rendered_frame prepared[2] = {
            {.texture = (void *)81, .signature = 801},
            {.texture = (void *)82, .signature = 802},
        };
        struct pl_rendered_mix views = {.frames = prepared, .num_frames = prepared_count};
        if (!custom->mix(custom->priv, &views))
            return false;
    }
    return render_boundary_ok;
}
static bool mp_motion_shader_supported(struct mp_motion *motion)
{
    return motion && shader_supported;
}
static enum mp_motion_result mp_motion_append_single_view_shader(
    struct mp_motion *motion, const struct pl_rendered_frame *view, pl_shader shader)
{
    assert(shader == (void *)83);
    return mp_motion_prepare_view(motion, view);
}
static enum mp_motion_result mp_motion_append_view_shader(
    struct mp_motion *motion, const struct pl_rendered_frame *first,
    const struct pl_rendered_frame *second, float alpha, pl_shader shader)
{
    assert(shader == (void *)83);
    pl_tex output;
    return mp_motion_render_views(motion, first, second, alpha, &output);
}
static void mp_motion_record_shader_output(struct mp_motion *motion)
{
    assert(motion);
    shader_outputs++;
}
static bool pl_render_image_mix_source_shader(void *rr, const struct pl_frame_mix *mix,
    const struct pl_frame *target, const struct pl_render_params *params,
    const struct pl_source_peak_context *source, const struct pl_frame_shader_mixer *custom)
{
    shader_calls++;
    if (skip_motion_callback)
        return false;
    struct pl_rendered_frame prepared[2] = {
        {.texture = (void *)81, .signature = 801},
        {.texture = (void *)82, .signature = 802},
    };
    struct pl_rendered_mix views = {.frames = prepared, .num_frames = prepared_count};
    bool mixed = custom->mix(custom->priv, &views, (void *)83);
    bool rendered = pl_render_image_mix_source(rr, mix, target, params, source, NULL);
    return mixed && rendered;
}
static bool pl_render_image_mix(void *rr, const struct pl_frame_mix *mix,
    const struct pl_frame *target, const struct pl_render_params *params)
{
    (void)rr; (void)mix; (void)target; (void)params;
    untimed_calls++;
    return render_boundary_ok;
}
static bool pl_render_image_source(void *rr, const struct pl_frame *source,
    const struct pl_frame *target, const struct pl_render_params *params,
    const struct pl_source_peak_context *context)
{
    (void)rr; (void)source; (void)target; (void)params;
    direct_calls++;
    routed_context = *context;
    routed_ids[0] = context->ids[0];
    return render_boundary_ok;
}

// Boundary-only spies: no peak history, commit, or native-copy model lives here.
static int source_resets, source_submits, native_prepares, hook_updates;
static uint64_t reset_epoch, submitted_ids[8];
static struct pl_frame submitted_frames[8];
static bool submitted_has_frame[8];
static const void *submitted_params;
static enum pl_source_peak_status submit_status[8];
static struct mp_image *prepare_failure, *hook_images[64];
static int protocol_acquires, protocol_releases;
static bool protocol_acquire_ok = true;
static bool pl_source_peak_state_reset(void *state, uint64_t epoch)
{
    (void)state;
    source_resets++;
    reset_epoch = epoch;
    return true;
}
static void replace_path(char **out, const char *path)
{
    free(*out);
    *out = NULL;
    if (path) {
        size_t size = strlen(path) + 1;
        *out = malloc(size);
        assert(*out);
        memcpy(*out, path, size);
    }
}
#define talloc_replace(ctx, dst, src) ((void)(ctx), replace_path(&(dst), (src)))
static bool prepare_native_frame(struct mp_image *mpi)
{
    native_prepares++;
    return mpi != prepare_failure;
}
static bool pl_queue_peek(void *queue, int index, struct pl_source_frame *out)
{
    (void)queue;
    if (index >= num_queued_frames)
        return false;
    *out = queued_frames[index];
    return true;
}
static enum pl_source_peak_status pl_source_peak_submit(
    void *rr, void *state, uint64_t epoch, uint64_t id,
    const struct pl_frame *frame, const void *params)
{
    (void)rr; (void)state;
    assert(epoch == reset_epoch && source_submits < 8);
    submitted_ids[source_submits] = id;
    submitted_has_frame[source_submits] = !!frame;
    if (frame)
        submitted_frames[source_submits] = *frame;
    submitted_params = params;
    return submit_status[source_submits++];
}
#define bstr0(name) (name)
static bool gpu_get_auto_param(const struct mp_image *mpi, const char *name, double *value)
{
    assert(hook_updates < 64);
    hook_images[hook_updates++] = (struct mp_image *)mpi;
    if (!strcmp(name, "PTS"))
        *value = mpi->pts;
    else if (!strcmp(name, "max_luma"))
        *value = mpi->params.color.hdr.max_luma;
    else if (!strcmp(name, "chroma_offset_x"))
        *value = mpi->params.chroma_location == PL_CHROMA_LEFT ? -.5 : 0;
    else
        return false;
    return true;
}
static bool protocol_acquire(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu;
    protocol_acquires++;
    struct mp_image *mpi = frame->user_data;
    frame->color = mpi->params.color;
    return protocol_acquire_ok;
}
static void protocol_release(pl_gpu gpu, struct pl_frame *frame)
{
    (void)gpu; (void)frame;
    protocol_releases++;
}

static bool pl_color_space_is_hdr(const struct pl_color_space *color)
{
    return color->transfer == PL_COLOR_TRC_PQ ||
           color->transfer == PL_COLOR_TRC_HLG ||
           color->transfer == PL_COLOR_TRC_LINEAR_HDR;
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

// The worker represents a VO stuck inside a driver callback. It cannot service
// control requests until released. The watchdog bounds failures without making
// successful property reads depend on the worker running again.
struct blocked_vo {
    mp_mutex lock;
#ifdef _WIN32
    CONDITION_VARIABLE wake;
#else
    pthread_cond_t wake;
#endif
    bool entered, release, timed_out;
};

static struct blocked_vo blocked;

static void signal_blocked(void)
{
#ifdef _WIN32
    WakeAllConditionVariable(&blocked.wake);
#else
    pthread_cond_broadcast(&blocked.wake);
#endif
}

static bool wait_blocked(bool watchdog)
{
#ifdef _WIN32
    return SleepConditionVariableSRW(&blocked.wake, &blocked.lock,
                                     watchdog ? 2000 : INFINITE, 0);
#else
    if (!watchdog)
        return pthread_cond_wait(&blocked.wake, &blocked.lock) == 0;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    return pthread_cond_timedwait(&blocked.wake, &blocked.lock, &until) == 0;
#endif
}

#ifdef _WIN32
static DWORD WINAPI blocked_vo_thread(void *arg)
#else
static void *blocked_vo_thread(void *arg)
#endif
{
    (void)arg;
    mp_mutex_lock(&blocked.lock);
    blocked.entered = true;
    signal_blocked();
    while (!blocked.release) {
        if (!wait_blocked(true)) {
            blocked.timed_out = true;
            blocked.release = true;
            signal_blocked();
        }
    }
    mp_mutex_unlock(&blocked.lock);
    return 0;
}

static void test_motion_snapshot_blocked(void)
{
    struct vo_internal in = {0};
#ifndef _WIN32
    pthread_mutex_init(&in.lock, NULL);
    pthread_mutex_init(&blocked.lock, NULL);
    pthread_cond_init(&blocked.wake, NULL);
#endif
    struct vo vo = {.in = &in};
    MPContext context = {.video_out = &vo};
    struct voctrl_motion_info info = {
        .state = "active", .supported = true, .target_fps = 120,
        .epoch_ns = 100, .timestamp_ns = 200, .submitted = 7,
    };
    vo_set_motion_info(&vo, &info);
#ifdef _WIN32
    HANDLE worker = CreateThread(NULL, 0, blocked_vo_thread, NULL, 0, NULL);
    assert(worker);
#else
    pthread_t worker;
    assert(pthread_create(&worker, NULL, blocked_vo_thread, NULL) == 0);
#endif
    mp_mutex_lock(&blocked.lock);
    while (!blocked.entered)
        assert(wait_blocked(false));
    mp_mutex_unlock(&blocked.lock);

    // Exercise the production property, not only the snapshot getter. Repeated
    // reads must keep the original publication time while the VO is blocked.
    for (int i = 0; i < 3; i++) {
        assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
        assert(motion_property("timestamp-ns")->value.int64 == 200);
        assert(motion_property("epoch-ns")->value.int64 == 100);
        assert(motion_property("swapchain-submissions")->value.int64 == 7);
    }
    mp_mutex_lock(&blocked.lock);
    bool returned_while_blocked = !blocked.release && !blocked.timed_out;
    blocked.release = true;
    signal_blocked();
    mp_mutex_unlock(&blocked.lock);
#ifdef _WIN32
    assert(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
    CloseHandle(worker);
#else
    assert(pthread_join(worker, NULL) == 0);
    pthread_cond_destroy(&blocked.wake);
    pthread_mutex_destroy(&blocked.lock);
    pthread_mutex_destroy(&in.lock);
#endif
    assert(returned_while_blocked);
    puts("motion snapshot: property returns while VO worker remains blocked; publication time stable");
}

static void test_motion_info(void)
{
    check_motion_event(event_MPV_EVENT_TICK);
    check_motion_event(event_MPV_EVENT_VIDEO_RECONFIG);
    check_motion_event(event_MP_EVENT_CORE_IDLE);
    MPContext context = {0};
    struct m_option type;
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET_TYPE, &type) == M_PROPERTY_OK);
    assert(type.type == CONF_TYPE_NODE);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_UNAVAILABLE);
    struct priv p = {
        .gpu = (void *)1, .motion = (void *)2, .motion_fps = 120,
        .motion_state = MOTION_ACTIVE, .motion_captured = UINT64_C(1) << 40,
        .motion_submitted = (UINT64_C(1) << 40) + 99,
        .motion_epoch = 1000,
    };
    struct vo_internal in = {0};
#ifndef _WIN32
    pthread_mutex_init(&in.lock, NULL);
#endif
    struct vo vo = {.priv = &p, .in = &in};
    context.video_out = &vo;
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_UNAVAILABLE);
    motion_stats = (struct mp_motion_stats) {
        .analyzed_pairs = 17, .generated_frames = 31, .repeated_frames = 7,
        .capture_ns = 100, .coarse_ns = 200, .refinement_ns = 300,
        .warp_ns = 400, .synthesis_ns = 500,
        .evidence_ns = 600, .reconstruction_ns = 700,
        .translation_cost_ns = 800, .translation_select_ns = 900,
        .analysis_width = 480, .analysis_height = 270,
        .synthesis_width = 1920, .synthesis_height = 1080,
    };
    publish_motion_info(&vo);
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
    assert(motion_property("gpu-translation-cost-pass-ns")->value.int64 == 800);
    assert(motion_property("gpu-translation-select-pass-ns")->value.int64 == 900);
    assert(motion_property("gpu-coarse-pass-ns")->value.int64 == 200);
    assert(motion_property("gpu-refinement-pass-ns")->value.int64 == 300);
    assert(motion_property("gpu-evidence-pass-ns")->value.int64 == 600);
    assert(motion_property("gpu-reconstruction-pass-ns")->value.int64 == 700);
    assert(motion_property("gpu-warp-pass-ns")->value.int64 == 400);
    assert(motion_property("gpu-synthesis-ns")->value.int64 == 500);
    assert(motion_property("last-analysis-width")->value.int_ == 480);
    assert(motion_property("last-analysis-height")->value.int_ == 270);
    assert(motion_property("last-synthesis-width")->value.int_ == 1920);
    assert(motion_property("last-synthesis-height")->value.int_ == 1080);
    p.paused = true;
    publish_motion_info(&vo);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(strcmp(motion_property("state")->value.string, "paused") == 0);
    assert(motion_property("epoch-ns")->value.int64 == 1000);
    p = (struct priv){.gpu = (void *)1};
    gpu_supported = false;
    publish_motion_info(&vo);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(!motion_property("supported")->value.bool_);
    assert(strcmp(motion_property("state")->value.string, "disabled") == 0);
    assert(motion_property("synthesis-submissions")->value.int64 == 0);
    assert(motion_property("epoch-ns")->value.int64 == 0);
    assert(motion_property("gpu-synthesis-ns")->unavailable);
    assert(motion_property("gpu-translation-cost-pass-ns")->unavailable);
    assert(motion_property("gpu-translation-select-pass-ns")->unavailable);
    assert(motion_property("gpu-evidence-pass-ns")->unavailable);
    assert(motion_property("gpu-reconstruction-pass-ns")->unavailable);
    assert(motion_property("last-synthesis-width")->unavailable);
    p.motion_fps = 60;
    p.motion_state = MOTION_UNSUPPORTED;
    assert(!motion_backend_failed(&p));
    report_motion_stats(&vo, true);
    assert(p.motion_submitted == 1);
    report_motion_stats(&vo, false);
    assert(p.motion_submitted == 1);
    publish_motion_info(&vo);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(strcmp(motion_property("state")->value.string, "unsupported") == 0);
    p.motion_state = MOTION_ERROR;
    assert(motion_backend_failed(&p));
    publish_motion_info(&vo);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_OK);
    assert(strcmp(motion_property("state")->value.string, "error") == 0);
    vo_set_motion_info(&vo, NULL);
    assert(mp_property_motion_info(&context, NULL, M_PROPERTY_GET, NULL) == M_PROPERTY_UNAVAILABLE);
#ifndef _WIN32
    pthread_mutex_destroy(&in.lock);
#endif
    gpu_supported = true;
    puts("motion info: backend capability, state, 64-bit counters, timing and unavailable data passed");
}

static void test_motion_capability_transition(void)
{
    struct gl_next_opts options = {.motion_fps = 60};
    struct priv p = {
        .gpu = (void *)1, .next_opts = &options,
        .motion_captured = 20, .motion_submitted = 30,
        .motion_last_generated = 17,
    };
    struct vo vo = {.priv = &p};
    gpu_supported = false;
    update_motion_options(&vo);
    assert(p.motion_state == MOTION_UNSUPPORTED && !p.motion);
    assert(p.motion_fps == 60 && !motion_backend_failed(&p));
    assert(p.motion_captured == 0 && p.motion_submitted == 0);
    assert(p.motion_last_generated == 0);
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
    struct mp_motion_stats saved_stats = motion_stats;
    motion_stats.generated_frames = (UINT64_C(1) << 40) + 7;
    p.motion_stats_time = mp_time_ns() - INT64_C(5000000000);
    report_motion_stats(&vo, true);
    assert(p.motion_last_generated == motion_stats.generated_frames);
    assert(p.motion_stats_time == mp_time_ns());
    motion_stats.generated_frames++;
    report_motion_stats(&vo, false);
    assert(p.motion_last_generated + 1 == motion_stats.generated_frames);
    motion_stats = saved_stats;
    motion_create_fail = true;
    options.motion_fps = 60;
    update_motion_options(&vo);
    assert(p.motion_state == MOTION_ERROR && !p.motion);
    assert(p.motion_last_generated == 0);
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

static void test_motion_frame_alpha(void)
{
    struct priv p = {.motion = (void *)1};
    struct vo vo = {.priv = &p};
    struct frame_priv fp[] = {
        {.motion_texture = (void *)1, .motion_id = 10},
        {.motion_texture = (void *)2, .motion_id = 11},
    };
    struct mp_image images[] = {
        {.params = original, .priv = &fp[0]},
        {.params = original, .priv = &fp[1]},
    };
    struct pl_frame frames[] = {
        {.user_data = &images[0]}, {.user_data = &images[1]},
    };
    const struct pl_frame *sources[] = {&frames[0], &frames[1]};
    double timestamps[2];
    struct pl_frame_mix mix = {
        .num_frames = 2, .frames = sources, .timestamps = timestamps,
    };
    const struct { double before, after; int result; } cases[] = {
        {-0.5f, 0.5f, 1}, {-1, 3, 1}, {-3, 1, 1},
        {-1, 0, 0}, {0, 1, 0}, {-1, -0.5f, 0}, {0.5f, 1, 0},
        {-1, 1e-8f, 0}, {-1.0f / 29.97f, 1e-10f, 0},
        {-1e-30f, 1e30f, 0},
        {-1e-50, 1, 0}, {-1, 1e-50, 0},
        {-FLT_MAX, FLT_MAX, -1},
        {-INFINITY, 1, -1}, {-1, INFINITY, -1},
        {-INFINITY, INFINITY, -1},
    };
    for (int n = 0; n < MP_ARRAY_SIZE(cases); n++) {
        timestamps[0] = cases[n].before;
        timestamps[1] = cases[n].after;
        motion_render_calls = 0;
        struct pl_frame output = {0};
        assert(prepare_motion_frame(&vo, &mix, &output) == cases[n].result);
        assert(motion_render_calls == (cases[n].result != 0));
        if (cases[n].result > 0) {
            assert(motion_render_alpha > 0 && motion_render_alpha < 1);
            assert(output.planes[0].texture == (void *)3);
            assert(output.user_data ==
                   sources[motion_render_alpha < 0.5f ? 0 : 1]->user_data);
        } else {
            assert(!output.planes[0].texture);
        }
    }
    timestamps[0] = -0.5f;
    timestamps[1] = 0.5f;
    motion_render_calls = 0;
    motion_render_fail = true;
    struct pl_frame output = {0};
    assert(prepare_motion_frame(&vo, &mix, &output) == -1);
    assert(motion_render_calls == 1 && !output.planes[0].texture);
    motion_render_fail = false;
    motion_render_calls = 0;
    frames[1].rotation = 1;
    assert(prepare_motion_frame(&vo, &mix, &output) == 0);
    frames[1].rotation = 0;
    frames[1].crop.x0 = 1;
    assert(prepare_motion_frame(&vo, &mix, &output) == 0);
    frames[1].crop.x0 = 0;
    images[1].params.w++;
    assert(prepare_motion_frame(&vo, &mix, &output) == 0);
    images[1].params.w--;
    fp[1].motion_texture = NULL;
    assert(prepare_motion_frame(&vo, &mix, &output) == 0);
    assert(!motion_render_calls);
    puts("motion alpha: rounded endpoints use source; invalid input and core "
         "failures remain fatal; frame compatibility preserved");

    for (int i = 0; i < 2; i++) {
        fp[i].motion_frame = (struct mp_motion_frame) {
            .texture = (void *)(uintptr_t)(i + 1), .id = 10 + i,
        };
        images[i].params.color = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_BT_2020, .transfer = PL_COLOR_TRC_PQ,
            .hdr.max_luma = i ? 4000.0f : 1000.0f,
        };
        frames[i].color = images[i].params.color;
        frames[i].color.transfer = PL_COLOR_TRC_LINEAR;
        frames[i].repr = (struct pl_color_repr) {
            .sys = PL_COLOR_SYSTEM_RGB, .alpha = PL_ALPHA_PREMULTIPLIED,
        };
        frames[i].crop.x1 = (float)original.w;
        frames[i].crop.y1 = (float)original.h;
        frames[i].pixel_aspect_ratio = 1.25f;
        frames[i].overlays = (void *)4;
        frames[i].num_overlays = 2;
    }
    described_render_calls = 0;
    for (int phase = 1; phase < 4; phase++) {
        timestamps[0] = -phase;
        timestamps[1] = 4 - phase;
        assert(prepare_motion_frame(&vo, &mix, &output) == 1);
        assert(output.planes[0].texture == (void *)3);
        assert(output.color.transfer == PL_COLOR_TRC_LINEAR_HDR);
        assert(output.color.hdr.max_luma == 1000);
        assert(output.repr.sys == PL_COLOR_SYSTEM_RGB);
        assert(output.repr.alpha == PL_ALPHA_PREMULTIPLIED);
        assert(output.crop.x1 == original.w && output.crop.y1 == original.h);
        assert(output.pixel_aspect_ratio == 1.25f);
        assert(output.overlays == (void *)4 && output.num_overlays == 2);
        assert(output.user_data == sources[phase < 2 ? 0 : 1]->user_data);
    }
    assert(described_render_calls == 3 && !motion_render_calls);
    motion_render_fail = true;
    assert(prepare_motion_frame(&vo, &mix, &output) == -1);
    assert(!output.planes[0].texture);
    motion_render_fail = false;
    images[1].params.color.primaries = PL_COLOR_PRIM_BT_709;
    assert(prepare_motion_frame(&vo, &mix, &output) == 0);
    images[1].params.color.primaries = PL_COLOR_PRIM_BT_2020;
    fp[0].motion_frame.id = 0;
    fp[0].motion_texture = NULL;
    assert(prepare_motion_frame(&vo, &mix, &output) == 0);
    assert(described_render_calls == 4 && !motion_render_calls);
    fp[0].motion_frame.id = 10;
    described_unsupported = true;
    assert(prepare_motion_frame(&vo, &mix, &output) == 0);
    assert(!output.planes[0].texture);
    described_unsupported = false;
    puts("PQ typed output preserves synthetic color and captured geometry; "
         "core classifies static metadata compatibility and unsupported pairs");
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

static void test_motion_source_slot(void)
{
    struct priv p = {.motion = (void *)1, .motion_fps = 60, .last_pts = 9.98};
    struct mp_image source = {.pts = 10};
    struct vo_frame frame = {
        .current = &source,
        .ideal_frame_duration = 1.0 / 29.97,
        .approx_duration = 1.0 / 29.97,
        .ideal_frame_vsync_duration = 1.0 / 60,
        .vsync_interval = 1.0 / 60,
    };
    assert(motion_output_pts(&p, &frame, 9.993) == source.pts);
    assert(motion_output_pts(&p, &frame, 10.007) == source.pts);
    assert(motion_output_pts(&p, &frame, 10.010) == 10.010);
    frame.repeat = true;
    assert(motion_output_pts(&p, &frame, 10.007) == 10.007);
    frame.repeat = false;
    frame.redraw = true;
    assert(motion_output_pts(&p, &frame, 10.007) == 10.007);
    frame.redraw = false;
    source.fields = MP_IMGFIELD_INTERLACED;
    assert(motion_output_pts(&p, &frame, 10.007) == 10.007);
    source.fields = 0;
    p.last_pts = 10.003;
    assert(motion_output_pts(&p, &frame, 10.007) == 10.007);
    frame.vsync_interval = 1.0 / 120;
    frame.ideal_frame_vsync_duration = 1.0 / 120;
    p.last_pts = 9.98;
    assert(motion_output_pts(&p, &frame, 10.003) == 10.003);
}

static struct mp_image *new_snapshot_frame(struct vo *vo, int pixel)
{
    struct mp_image *image = calloc(1, sizeof(*image));
    struct frame_priv *fp = calloc(1, sizeof(*fp));
    assert(image && fp);
    *image = (struct mp_image) {
        .params = original, .priv = fp, .imgfmt = IMGFMT_MEDIACODEC,
        .pixel_id = pixel, .num_planes = 1,
    };
    fp->vo = vo;
    fp->motion_id = (uint64_t)pixel;
    return image;
}

static void test_snapshot_lifecycle(void)
{
    calls.fail_create = false;
    struct ra ra = {.uses_pl = true};
    struct ra_ctx ctx = {.ra = &ra};
    const struct ra_hwdec_mapper_driver mapper_driver = {.map_on_reset = true};
    const struct ra_hwdec_driver driver = {.mapper = &mapper_driver};
    struct ra_hwdec hwdec = {.ra = &ra, .driver = &driver};
    struct priv p = {.ra_ctx = &ctx, .hwdec_ctx = &hwdec};
    struct vo vo = {.priv = &p};
    struct mp_image *images[5];
    int before = calls.maps;
    for (int n = 0; n < 5; n++) {
        images[n] = new_snapshot_frame(&vo, 100 + n);
        struct frame_priv *fp = images[n]->priv;
        assert(preload_deferred_hwdec_frame(&p, fp, images[n]) == 1);
        assert(fp->source->snapshot.ready && !p.hwdec.owner);
    }
    assert(calls.maps == before + 5);
    // More queued sources than the native mapper's three retained slots.
    for (int n = 0; n < 5; n++) {
        struct frame_priv *fp = images[n]->priv;
        assert(*(int *)fp->source->snapshot.frame.planes[0].texture == 100 + n);
        assert(preload_deferred_hwdec_frame(&p, fp, images[n]) == 1);
    }
    assert(calls.maps == before + 5);
    // The legacy mapper may return success with the latest pixels for consumed A.
    stale_success = true;
    assert(ra_hwdec_mapper_map(p.hwdec.mapper, images[0]) == 0);
    assert(p.hwdec.mapper->src->pixel_id == 104);
    assert(*(int *)((struct frame_priv *)images[0]->priv)
                       ->source->snapshot.frame.planes[0].texture == 100);
    stale_success = false;
    ra_hwdec_mapper_unmap(p.hwdec.mapper);
    for (int n = 0; n < 5; n++)
        discard_frame(&(struct pl_source_frame){.frame_data = images[n]});
    clear_source_frames(&p);
    assert(live_copies == 0);

    struct mp_image *bl = new_snapshot_frame(&vo, 201);
    struct mp_image *el = new_snapshot_frame(&vo, 202);
    bl->enhancement_layer = el;
    el->retries = 1;
    struct frame_priv *fp = bl->priv;
    p.pending_frame = bl;
    assert(preload_deferred_hwdec_frame(&p, fp, bl) == RA_HWDEC_MAP_RETRY);
    assert(fp->source->snapshot.ready && !fp->source->el_snapshot.ready);
    int copies = copy_calls;
    assert(preload_deferred_hwdec_frame(&p, fp, bl) == 1);
    assert(copy_calls == copies + 1); // No second BL copy or consume.
    assert(*(int *)fp->source->snapshot.frame.planes[0].texture == 201);
    assert(*(int *)fp->source->el_snapshot.frame.planes[0].texture == 202);
    clear_pending_frame(&p);
    assert(!p.pending_frame && live_copies == 2);
    clear_source_frames(&p);
    assert(!live_copies);
    talloc_free(el); // Fixture models the enhancement-layer reference separately.

    bl = new_snapshot_frame(&vo, 301);
    bl->num_planes = 2;
    fp = bl->priv;
    fail_copy_at = copy_calls + 2;
    assert(preload_deferred_hwdec_frame(&p, fp, bl) == -1);
    assert(!fp->source->snapshot.ready && live_copies == 0 && p.hwdec_map_failed);
    assert(!p.hwdec.owner && !p.hwdec.acquired);
    fail_copy_at = 0;
    assert(fp->source->failed);
    struct mp_image *retry = mp_image_new_ref(bl);
    free_frame(bl); // The cache alone must remember that this source was consumed.
    struct vo_frame retry_frame = {.frame_id = 301, .num_frames = 1,
                                   .frames = &retry};
    before = calls.maps;
    int failed_copies = copy_calls;
    stale_success = true;
    assert(queue_frame(&vo, &retry_frame, 0, false, false, false) == -1);
    assert(calls.maps == before && copy_calls == failed_copies && !live_copies);
    assert(!num_queued_frames && p.pending_frame);
    stale_success = false;
    clear_pending_frame(&p);
    clear_source_frames(&p);
    talloc_free(retry);

    bl = new_snapshot_frame(&vo, 401);
    el = new_snapshot_frame(&vo, 402);
    bl->enhancement_layer = el;
    el->retries = 1;
    fp = bl->priv;
    p.pending_frame = bl;
    assert(preload_deferred_hwdec_frame(&p, fp, bl) == RA_HWDEC_MAP_RETRY);
    before = freed_frames;
    reset_frame_queue(&p);
    assert(freed_frames == before + 1 && !p.pending_frame && live_copies == 1);
    clear_pending_frame(&p);
    assert(freed_frames == before + 1);
    clear_source_frames(&p);
    assert(!live_copies);
    talloc_free(el);
    slot_uninit(&p, &p.hwdec);
    slot_uninit(&p, &p.el_hwdec);
    const struct ra_hwdec_mapper_driver consuming = {.consumes_source = true};
    const struct ra_hwdec_driver gl_driver = {.mapper = &consuming};
    hwdec.driver = &gl_driver;
    deferred_params = false;
    bl = new_snapshot_frame(&vo, 501);
    fp = bl->priv;
    assert(preload_deferred_hwdec_frame(&p, fp, bl) == 1);
    assert(fp->source->snapshot.ready &&
           *(int *)fp->source->snapshot.frame.planes[0].texture == 501);
    before = calls.maps;
    assert(preload_deferred_hwdec_frame(&p, fp, bl) == 1);
    assert(calls.maps == before);
    unmap_frame(p.gpu, &fp->source->snapshot.frame,
                &(struct pl_source_frame){.frame_data = bl});
    assert(live_copies == 1);
    clear_source_frames(&p);
    assert(!live_copies);
    slot_uninit(&p, &p.hwdec);
    deferred_params = true;
    assert(accesses == releases && destroyed_copies > 0);
    puts("gpu-next snapshot identity, partial copy and pending lifecycle passed");
}

static void test_snapshot_metadata(void)
{
    struct ra ra = {.uses_pl = true};
    struct ra_ctx ctx = {.ra = &ra};
    struct ra_hwdec hwdec = {.ra = &ra};
    struct priv p = {.ra_ctx = &ctx};
    struct vo vo = {.priv = &p};
    struct mp_image *bl = new_snapshot_frame(&vo, 701);
    struct mp_image *el = new_snapshot_frame(&vo, 702);
    struct frame_priv *fp = bl->priv;
    struct hwdec_slot *slots[] = {&p.hwdec, &p.el_hwdec};
    struct mp_image *images[] = {bl, el};
    fp->source = get_source_frame(&p, bl, fp->motion_id);
    struct hwdec_snapshot *snapshots[] = {&fp->source->snapshot, &fp->source->el_snapshot};
    const int raw_map[] = {2, 0, 1, -1}; // CR, Y, CB, NONE; not RGB.
    for (int n = 0; n < 2; n++) {
        images[n]->params.repr.dovi = images[n];
        images[n]->params.color.hdr.max_luma = (float)(1000 + n);
        assert(slot_preload(&p, slots[n], &hwdec, bl, images[n], true) == 1);
        struct ra_hwdec_mapper *mapper = slots[n]->mapper;
        mapper->dst_params.w = 2048 + n;
        mapper->dst_params.h = 1088 + n;
        mapper->dst_params.crop = (struct mp_rect){4 + n, 6, 1924, 1086};
        mapper->dst_params.color.primaries = PL_COLOR_PRIM_BT_2020;
        mapper->dst_params.color.transfer = PL_COLOR_TRC_PQ;
        mapper->dst_params.color.hdr.max_luma = (float)(1000 + n);
        mapper->dst_params.chroma_location = n ? PL_CHROMA_CENTER : PL_CHROMA_LEFT;
        mapper->dst_params.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
        mapper->dst_params.repr.dovi = images[n];
        mapper->dst_params.repr.bits = (struct pl_bit_encoding){10 + n, 10, n};
        mapper->dst_params_preserve_repr = true;
        mapper->dst_params_map_coordinates = true;
        mapper->dst_num_components = 3;
        memcpy(mapper->dst_component_mapping, raw_map, sizeof(raw_map));
        assert(snapshot_hwdec_frame(&p, slots[n], &hwdec, bl, images[n],
                                    true, snapshots[n]) == 1);
        const struct hwdec_snapshot *snapshot = snapshots[n];
        assert(snapshot->ready && snapshot->mapped_coordinates);
        assert(snapshot->params.w == 2048 + n && snapshot->params.h == 1088 + n);
        assert(snapshot->params.crop.x0 == 4 + n &&
               snapshot->params.crop.y1 == 1086);
        assert(snapshot->frame.num_planes == 1 &&
               snapshot->frame.planes[0].components == 3);
        assert(!memcmp(snapshot->frame.planes[0].component_mapping,
                       raw_map, sizeof(raw_map)));
        assert(snapshot->frame.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION);
        assert(snapshot->frame.repr.dovi == images[n]);
        assert(snapshot->frame.repr.bits.sample_depth == 10 + n);
        assert(snapshot->frame.repr.bits.color_depth == 10 &&
               snapshot->frame.repr.bits.bit_shift == n);
        assert(snapshot->frame.color.transfer == PL_COLOR_TRC_PQ);
        assert(snapshot->frame.color.hdr.max_luma == (float)(1000 + n));
        assert(snapshot->frame.chroma_location ==
               (n ? PL_CHROMA_CENTER : PL_CHROMA_LEFT));
        assert(*(int *)snapshot->frame.planes[0].texture == 701 + n);
    }
    // Non-override path sorts actual component offsets and permits guessed repr.
    struct pl_frame described;
    p.hwdec.mapper->dst_num_components = 0;
    p.hwdec.mapper->dst_params_preserve_repr = false;
    describe_hwdec_frame(p.hwdec.mapper, &described);
    const int sorted[] = {2, 1, 0, 3};
    assert(described.planes[0].components == 4);
    assert(!memcmp(described.planes[0].component_mapping,
                   sorted, sizeof(sorted)));
    assert(described.repr.sys == PL_COLOR_SYSTEM_RGB && described.repr.dovi == NULL);
    free_frame(bl);
    clear_source_frames(&p);
    talloc_free(el);
    slot_uninit(&p, &p.hwdec);
    slot_uninit(&p, &p.el_hwdec);
    assert(live_copies == 0);
    puts("gpu-next snapshot preserves distinct BL/EL raw mappings and metadata");
}

static void test_pending_queue_entry(void)
{
    struct ra ra = {.uses_pl = true};
    struct ra_ctx ctx = {.ra = &ra};
    const struct ra_hwdec_mapper_driver mapper = {.consumes_source = true};
    const struct ra_hwdec_driver driver = {.mapper = &mapper};
    struct ra_hwdec hwdec = {.ra = &ra, .driver = &driver};
    const uint64_t high = (UINT64_C(1) << 40) + 123;
    for (int mode = 0; mode < 4; mode++) {
        struct priv p = {.ra_ctx = &ctx, .hwdec_ctx = &hwdec};
        struct vo vo = {.priv = &p};
        struct mp_image bl = {.params = original, .pixel_id = 601,
                              .num_planes = 1};
        struct mp_image el = {.params = original, .pixel_id = 602,
                              .num_planes = 1, .retries = 1};
        bl.enhancement_layer = &el;
        struct mp_image *images[] = {&bl, &bl};
        struct vo_frame frame = {.frame_id = high, .num_frames = 2,
                                 .frames = images, .approx_duration = 0.04};
        p.last_id = high; // draw_frame skips the already pushed index zero.
        int pushes = queue_pushes;
        assert(queue_frame(&vo, &frame, 1, true, true, true) == RA_HWDEC_MAP_RETRY);
        struct mp_image *pending = p.pending_frame;
        struct frame_priv *fp = pending->priv;
        struct source_frame *source = fp->source;
        assert(fp->motion_id == high + 1 && source->snapshot.ready);
        assert(!source->el_snapshot.ready && p.last_id == high);
        assert(source->references == 2 && live_copies == 1);
        assert(bl.buffer->consumed && !el.buffer->consumed);
        int copies = copy_calls;
        if (mode == 0) {
            prune_pending_frame(&p, &frame); // Pending is not at index zero.
            assert(p.pending_frame == pending);
        } else if (mode == 1) {
            frame.num_frames = 0;
            prune_pending_frame(&p, &frame);
            prune_source_frames(&p, &frame);
            assert(!p.pending_frame && p.num_source_frames == 1 &&
                   source->references == 1);
        } else if (mode == 2) {
            frame.num_frames = 1; // Temporary lookahead shrink is not eviction.
            prune_pending_frame(&p, &frame);
            prune_source_frames(&p, &frame);
            assert(!p.pending_frame && p.num_source_frames == 1 &&
                   source->references == 1);
        } else {
            reset_frame_queue(&p);
            assert(!p.pending_frame && source->references == 1 && !p.last_id);
        }
        frame.num_frames = 2;
        assert(queue_frame(&vo, &frame, 1, false, false, false) == 1);
        fp = queued_frame.frame_data->priv;
        assert(fp->source == source && copy_calls == copies + 1);
        assert(*(int *)source->snapshot.frame.planes[0].texture == 601);
        assert(*(int *)source->el_snapshot.frame.planes[0].texture == 602);
        assert(bl.buffer->consumed && el.buffer->consumed);
        assert(!p.pending_frame && p.last_id == high + 1);
        assert(queue_pushes == pushes + 1 && queued_frame.duration == 0);
        assert(queued_frame.map == map_frame && queued_frame.unmap == unmap_frame &&
               queued_frame.discard == discard_frame && !fp->motion_capture);
        clear_pending_frame(&p); // Queue still owns its reference.
        assert(source->references == 2);
        frame.frame_id += 2;
        prune_source_frames(&p, &frame);
        assert(!p.num_source_frames && source->references == 1);
        assert(live_copies == 2); // LP may retain past frames beyond the watermark.
        discard_queued_frames();
        assert(!live_copies);
        clear_source_frames(&p);
        slot_uninit(&p, &p.hwdec);
        slot_uninit(&p, &p.el_hwdec);
    }
    puts("gpu-next pending BL/EL retry, future watermark and queue ownership passed");
}

static void test_source_cache_reset(void)
{
    for (int capacity = 1; capacity <= 3; capacity += 2) {
        retained_count = release_requests = stale_reuses = 0;
        retained_capacity = capacity;
        struct ra ra = {.uses_pl = true};
        struct ra_ctx ctx = {.ra = &ra};
        const struct ra_hwdec_mapper_driver mapper = {.consumes_source = true};
        const struct ra_hwdec_driver driver = {.mapper = &mapper};
        struct ra_hwdec hwdec = {.ra = &ra, .driver = &driver};
        struct gl_next_opts opts = {.motion_fps = 120};
        struct priv p = {.ra_ctx = &ctx, .hwdec_ctx = &hwdec,
                         .next_opts = &opts, .motion_fps = 60};
        struct vo vo = {.priv = &p};
        struct mp_image *images[5];
        for (int n = 0; n < 5; n++)
            images[n] = new_snapshot_frame(&vo, 800 + n);
        struct vo_frame frame = {.frame_id = 100, .num_frames = 5,
                                 .frames = images};
        for (int n = 0; n < 5; n++)
            assert(queue_frame(&vo, &frame, n, false, false, false) == 1);
        struct frame_priv *first = queued_frame.frame_data->priv;
        struct source_frame *source = first->source;
        assert(*(int *)source->snapshot.frame.planes[0].texture == 800);
        assert(release_requests == 5 && !stale_reuses && live_copies == 5);
        update_motion_options(&vo); // Actual production reset drops every queue entry.
        assert(!num_queued_frames && !p.last_id && live_copies == 5);
        assert(source->references == 1 && p.num_source_frames == 5);
        assert(queue_frame(&vo, &frame, 0, false, false, false) == 1);
        first = queued_frame.frame_data->priv;
        assert(first->source == source && source->references == 2);
        assert(*(int *)source->snapshot.frame.planes[0].texture == 800);
        assert(release_requests == 5 && !stale_reuses);

        // The model must expose stale-success if the owner really consumes A twice.
        assert(ra_hwdec_mapper_map(p.hwdec.mapper, images[0]) == 0);
        assert(p.hwdec.mapper->src->pixel_id == 804);
        assert(stale_reuses == 1 && release_requests == 6);
        ra_hwdec_mapper_unmap(p.hwdec.mapper);

        frame.num_frames = 1;
        prune_source_frames(&p, &frame);
        assert(p.num_source_frames == 5); // Consumed future sources remain available.
        reset_frame_queue(&p);
        frame.frame_id = 105;
        prune_source_frames(&p, &frame);
        assert(!p.num_source_frames && !live_copies);
        clear_source_frames(&p);
        slot_uninit(&p, &p.hwdec);
        for (int n = 0; n < 5; n++)
            talloc_free(images[n]);
    }
    retained_capacity = retained_count = 0;
    puts("gpu-next source cache prevents same-ID reconsume after motion reset");
}

static void test_capture_selector(void)
{
    struct priv p = {.motion = (void *)1};
    struct vo vo = {.priv = &p};
    struct frame_priv fp = {.vo = &vo, .motion_capture = true};
    struct mp_image mpi = {.priv = &fp, .params = {.w = 128, .h = 96}};
    struct pl_frame source = {
        .num_planes = 1, .planes[0].texture = (void *)7,
        .repr = {.sys = PL_COLOR_SYSTEM_DOLBYVISION, .dovi = &mpi},
        .color = {.primaries = PL_COLOR_PRIM_BT_709,
                  .transfer = PL_COLOR_TRC_SRGB},
        .rotation = 3, .enhancement_layer = &mpi,
    };
    capture_typed_calls = capture_legacy_calls = 0;
    capture_supported = capture_legacy_ok = true;
    capture_result = MP_MOTION_OK;
    for (int typed = 0; typed < 2; typed++) {
        struct pl_frame frame = source;
        if (typed) {
            frame.repr = pl_color_repr_rgb;
            frame.color.transfer = PL_COLOR_TRC_PQ;
        }
        fp.mapped_coordinates = !typed;
        fp.mapped_params = (struct mp_image_params) {
            .w = 192, .h = 108, .crop = {4, 6, 132, 102},
        };
        assert(capture_motion_frame(&vo, &mpi, &frame));
        assert(capture_typed_calls == typed + 1 && !capture_legacy_calls);
        assert(capture_input.repr.sys == (typed ? PL_COLOR_SYSTEM_RGB
                                             : PL_COLOR_SYSTEM_DOLBYVISION));
        assert(capture_input.color.transfer == (typed ? PL_COLOR_TRC_PQ
                                                     : PL_COLOR_TRC_SRGB));
        assert(capture_width == 128 && capture_height == 96);
        assert(capture_input.crop.x0 == (typed ? 0 : 4));
        assert(capture_input.crop.y0 == (typed ? 0 : 6));
        assert(capture_input.crop.x1 == (typed ? 128 : 132));
        assert(capture_input.crop.y1 == (typed ? 96 : 102));
        assert(frame.planes[0].texture == fp.motion_frame.texture);
        assert(frame.num_planes == 1 && frame.planes[0].components == 4);
        assert(frame.color.primaries == PL_COLOR_PRIM_BT_2020 &&
               frame.color.transfer == PL_COLOR_TRC_LINEAR_HDR);
        assert(frame.repr.sys == PL_COLOR_SYSTEM_RGB && !frame.repr.dovi);
        assert(frame.repr.alpha == PL_ALPHA_PREMULTIPLIED);
        assert(frame.rotation == 3 && frame.user_data == &mpi);
        assert(!frame.enhancement_layer && !fp.mapped_coordinates);
        assert(p.motion_captured == (uint64_t)typed + 1);
    }
    struct pl_frame frame = source;
    frame.repr = pl_color_repr_rgb;
    assert(capture_motion_frame(&vo, &mpi, &frame));
    assert(capture_legacy_calls == 1 && capture_typed_calls == 2);
    assert(frame.planes[0].texture == fp.motion_texture);
    assert(frame.color.transfer == PL_COLOR_TRC_SRGB);
    assert(p.motion_captured == 3);

    capture_supported = false;
    frame = source;
    fp.mapped_coordinates = true;
    assert(capture_motion_frame(&vo, &mpi, &frame));
    assert(!memcmp(&frame, &source, sizeof(frame)) && fp.mapped_coordinates);
    assert(capture_legacy_calls == 1 && capture_typed_calls == 2);
    assert(p.motion_captured == 3 && p.motion_bypass_reported);
    capture_supported = true;
    capture_result = MP_MOTION_UNSUPPORTED;
    assert(capture_motion_frame(&vo, &mpi, &frame));
    assert(!memcmp(&frame, &source, sizeof(frame)) && p.motion_captured == 3);
    assert(capture_typed_calls == 3 && capture_legacy_calls == 1);

    // Real owned allocations make missing either cleanup observable.
    for (int legacy = 0; legacy < 2; legacy++) {
        int pixel = 77;
        assert(copy_hwdec_plane(p.gpu, (void *)1, &pixel, 4,
                                &fp.motion_texture));
        assert(copy_hwdec_plane(p.gpu, (void *)1, &pixel, 4,
                                &fp.motion_frame.texture));
        assert(live_copies == 2);
        capture_result = MP_MOTION_ERROR;
        capture_legacy_ok = false;
        frame = source;
        if (legacy)
            frame.repr = pl_color_repr_rgb;
        assert(!capture_motion_frame(&vo, &mpi, &frame));
        assert(p.motion_state == MOTION_ERROR && p.motion_captured == 3);
        assert(!fp.motion_texture && !fp.motion_frame.texture);
        assert(!fp.motion_frame.id && !fp.motion_frame.owner && !live_copies);
    }
    capture_result = MP_MOTION_OK;
    capture_legacy_ok = true;
    puts("gpu-next capture selector: DV/SRGB and PQ typed, SDR legacy, "
         "unsupported passthrough and owned failure cleanup passed");
}

static void test_seek_reset(void)
{
    struct priv p = {0};
    struct vo_frame frame = {.frame_id = 100, .num_frames = 3};
    update_seek_reset(&p, &frame);
    assert(p.last_source_id == 102 && !p.want_reset);
    p.want_seek_reset = true;
    p.last_id = 102;
    reset_frame_queue(&p); // FPS/view queue reset while a seek is pending.
    assert(!p.last_id && p.last_source_id == 102);
    update_seek_reset(&p, &frame);
    assert(p.want_seek_reset && !p.want_reset);
    frame.frame_id = 101;
    frame.num_frames = 1; // A smaller lookahead must not lower the barrier.
    update_seek_reset(&p, &frame);
    assert(p.last_source_id == 102 && p.want_seek_reset && !p.want_reset);
    frame.num_frames = 0;
    update_seek_reset(&p, &frame);
    assert(p.last_source_id == 102 && p.want_seek_reset && !p.want_reset);
    frame = (struct vo_frame) {.frame_id = 200, .num_frames = 2};
    update_seek_reset(&p, &frame);
    assert(!p.want_seek_reset && p.want_reset && p.last_source_id == 201);
    p.want_reset = false;
    reset_frame_queue(&p);
    // A later seek must also retain the source barrier after an FPS reset.
    p.want_seek_reset = true;
    update_seek_reset(&p, &frame);
    assert(p.want_seek_reset && !p.want_reset && p.last_source_id == 201);
    frame.frame_id = 300;
    frame.num_frames = 1;
    update_seek_reset(&p, &frame);
    assert(!p.want_seek_reset && p.want_reset && p.last_source_id == 300);
    puts("gpu-next seek waits for a new source across FPS resets and redraws");
}

static void test_source_protocol(void)
{
    struct fixture_options options = {.params = {.num_hooks = 1,
        .peak_detect_params = (void *)7}};
    struct gl_next_opts next = {0};
    struct priv p = {.gpu = (void *)1, .source_peak = (void *)1,
                     .pars = &options, .next_opts = &next};
    struct vo vo = {.priv = &p};
    pl_var_data pts_value = {0};
    struct pl_hook_par pts_parameter = {.name = "PTS", .type = PL_VAR_FLOAT, .data = &pts_value};
    struct pl_hook pts_hook = {.parameters = &pts_parameter, .num_parameters = 1};
    p.hooks[0] = &pts_hook;
    source_resets = 0;
    update_source_policy(&p, 203, NULL, 0, 0);
    assert(source_resets == 1 && p.source_epoch == 1 && p.want_reset);
    p.want_reset = false;
    update_source_policy(&p, 203, "", 99, 6);
    assert(source_resets == 1 && !p.want_reset);
    update_source_policy(&p, 203, "input.cube", 1, 6);
    assert(source_resets == 2);
    update_source_policy(&p, 203, "input.cube", 1, 0);
    assert(source_resets == 2);
    update_source_policy(&p, 203, "input.cube", 2, 0);
    assert(source_resets == 3);
    update_source_policy(&p, 203, NULL, 2, 0);
    assert(source_resets == 4 && !p.source_lut_path);
    update_source_policy(&p, 203, "", 44, 0);
    assert(source_resets == 4);
    update_source_policy(&p, 100, NULL, 44, 0);
    assert(source_resets == 5);
    update_source_policy(&p, 100, NULL, 44, 1);
    assert(source_resets == 6);
    uint64_t epoch = p.source_epoch;
    gpu_supported = true;
    motion_create_fail = false;
    const int fps[] = {60, 120, 0};
    for (int i = 0; i < 3; i++) {
        next.motion_fps = fps[i];
        update_motion_options(&vo);
        assert(p.source_epoch == epoch && source_resets == 6);
    }
    struct frame_priv fp[2] = {{.vo = &vo, .motion_id = 10},
                               {.vo = &vo, .motion_id = 40}};
    struct mp_image images[2] = {0};
    for (int i = 0; i < 2; i++) {
        images[i].priv = &fp[i];
        images[i].pts = 1.25 + i;
        images[i].params.w = 64;
        images[i].params.h = 32;
        images[i].params.color.hdr.max_luma = (float)(100 + 100 * i);
        fp[i].native_frame = (struct pl_frame) {
            .user_data = &images[i], .acquire = protocol_acquire,
            .release = protocol_release, .crop = {4, 3, 20, 21},
        };
        fp[i].view_frame = fp[i].native_frame;
    }
    images[0].params.crop = (struct mp_rect){1, 2, 63, 31};
    struct pl_frame crop_probe = fp[0].view_frame;
    source_crop(&images[0], &crop_probe);
    assert(crop_probe.crop.x0 == 1 && crop_probe.crop.y0 == 2 && crop_probe.crop.x1 == 63);
    assert(fp[0].view_frame.crop.x0 == 4);
    images[0].params.crop = (struct mp_rect){0};
    fp[1].mapped_coordinates = true;
    fp[1].mapped_params.w = 80;
    fp[1].mapped_params.h = 48;
    crop_probe = fp[1].view_frame;
    source_crop(&images[1], &crop_probe);
    assert(crop_probe.crop.x0 == 0 && crop_probe.crop.x1 == 80 && crop_probe.crop.y1 == 48);
    assert(fp[1].view_frame.crop.x0 == 4);
    fp[1].mapped_params.w = 64;
    fp[1].mapped_params.h = 32;
    fp[1].mapped_params.crop = (struct mp_rect){2, 1, 60, 30};
    images[1].fields = MP_IMGFIELD_INTERLACED | MP_IMGFIELD_TOP_FIRST;
    protocol_acquires = protocol_releases = hook_updates = 0;
    for (int i = 0; i < 2; i++) {
        struct pl_frame view = fp[i].view_frame;
        assert(view_acquire(NULL, &view));
        assert(view.color.hdr.max_luma == 100 + 100 * i);
        assert(view.crop.x0 == 4 && view.crop.y0 == 3);
        view_release(NULL, &view);
        struct pl_frame native = fp[i].native_frame;
        assert(source_peak_acquire(NULL, &native));
        assert(native.crop.x0 == (i ? 2 : 0));
        assert(native.crop.x1 == (i ? 60 : 64));
        assert(native.field == (i ? PL_FIELD_TOP : PL_FIELD_NONE));
        protocol_release(NULL, &native);
        assert(fp[i].view_frame.crop.x0 == 4);
    }
    assert(hook_updates == 2 && hook_images[0] == &images[0] && hook_images[1] == &images[1]);
    assert(hook_images[0]->pts == 1.25 && hook_images[1]->pts == 2.25);
    assert(protocol_acquires == 4 && protocol_releases == 4);
    protocol_acquire_ok = false;
    struct pl_frame failed = fp[0].view_frame;
    assert(!view_acquire(NULL, &failed) && hook_updates == 2);
    view_release(NULL, &failed);
    failed = fp[0].native_frame;
    assert(!source_peak_acquire(NULL, &failed));
    assert(failed.crop.x0 == 4); // Failed delegate must not rewrite its crop.
    protocol_release(NULL, &failed);
    protocol_acquire_ok = true;
    images[1].fields = MP_IMGFIELD_INTERLACED;
    failed = fp[1].native_frame;
    source_crop(&images[1], &failed);
    assert(failed.field == PL_FIELD_BOTTOM && failed.first_field == PL_FIELD_BOTTOM);
    num_queued_frames = 2;
    for (int i = 0; i < 2; i++)
        queued_frames[i] = (struct pl_source_frame){.frame_data = &images[i]};
    options.params.peak_detect_params = NULL;
    source_submits = native_prepares = 0;
    submit_status[0] = submit_status[1] = PL_SOURCE_PEAK_READY;
    prepare_failure = &images[0]; // Disabled registration must not prepare it.
    bool disabled_ok = submit_source_frames(&p);
    printf("disabled source registration: native_prepares=%d submits=%d\n",
           native_prepares, source_submits);
    fflush(stdout);
    assert(native_prepares == 0);
    assert(disabled_ok && source_submits == 2);
    assert(submitted_ids[0] == 10 && submitted_ids[1] == 40);
    assert(!submitted_has_frame[0] && !submitted_has_frame[1] && !submitted_params);
    assert(!fp[0].native_ready && !fp[1].native_ready);
    source_submits = native_prepares = 0;
    submit_status[1] = PL_SOURCE_PEAK_ERROR;
    p.hwdec_map_retry = true;
    assert(!submit_source_frames(&p));
    assert(!native_prepares && source_submits == 2);
    assert(p.hwdec_map_failed && !p.hwdec_map_retry);
    assert(!submitted_has_frame[0] && !submitted_has_frame[1]);
    assert(!fp[0].native_ready && !fp[1].native_ready);
    prepare_failure = NULL;
    p.hwdec_map_failed = false;
    options.params.peak_detect_params = (void *)7;
    source_submits = native_prepares = 0;
    submit_status[0] = PL_SOURCE_PEAK_PENDING;
    submit_status[1] = PL_SOURCE_PEAK_UNSUPPORTED;
    assert(submit_source_frames(&p));
    assert(submitted_has_frame[0] && submitted_has_frame[1]);
    assert(source_submits == 2 && native_prepares == 2);
    assert(submitted_ids[0] == 10 && submitted_ids[1] == 40);
    assert(submitted_params == options.params.peak_detect_params);
    assert(fp[1].peak_status == PL_SOURCE_PEAK_UNSUPPORTED && !p.hwdec_map_failed);
    assert(submitted_frames[0].crop.x0 == 0 && submitted_frames[1].crop.x0 == 2);
    assert(submitted_frames[0].acquire == source_peak_acquire);
    assert(fp[0].native_frame.acquire == protocol_acquire && fp[0].view_frame.crop.x0 == 4);
    source_submits = native_prepares = 0;
    submit_status[1] = PL_SOURCE_PEAK_ERROR;
    p.hwdec_map_retry = true;
    assert(!submit_source_frames(&p));
    assert(source_submits == 2 && p.hwdec_map_failed && !p.hwdec_map_retry);
    source_submits = native_prepares = 0;
    prepare_failure = &images[0];
    assert(!submit_source_frames(&p));
    assert(native_prepares == 1 && !source_submits);
    prepare_failure = NULL;
    memset(queued_frames, 0, sizeof(queued_frames));
    num_queued_frames = 0;
    free(p.source_lut_path);
    puts("gpu-next source policy/wrapper/submit protocol: LP history and GPU preparation not modeled");
}

static void test_source_parameters(void)
{
    struct fixture_options options = {.params.num_hooks = 2};
    struct priv p = {.pars = &options};
    struct vo vo = {.priv = &p};
    pl_var_data values[5] = {{0}, {0}, {0}, {.f = 1.5f}, {0}};
    struct pl_hook_par first[] = {
        {.name = "PTS", .type = PL_VAR_FLOAT, .data = &values[0]},
        {.name = "max_luma", .type = PL_VAR_SINT, .data = &values[1]},
        {.name = "gain", .type = PL_VAR_FLOAT, .data = &values[3]},
    };
    struct pl_hook_par second[] = {
        {.name = "max_luma", .type = PL_VAR_UINT, .data = &values[2]},
        {.name = "chroma_offset_x", .type = PL_VAR_FLOAT, .data = &values[4]},
    };
    struct pl_hook hooks[] = {{.parameters = first, .num_parameters = 3},
                              {.parameters = second, .num_parameters = 2}};
    p.hooks[0] = &hooks[0]; p.hooks[1] = &hooks[1];
    struct frame_priv fp[2] = {{.vo = &vo}, {.vo = &vo}};
    struct mp_image mpi[2] = {{.priv = &fp[0], .pts = 1.25},
                              {.priv = &fp[1], .pts = 2.75}};
    mpi[0].params.color.hdr.max_luma = 101.4f;
    mpi[1].params.color.hdr.max_luma = 202.6f;
    mpi[0].params.chroma_location = PL_CHROMA_LEFT;
    mpi[1].params.chroma_location = PL_CHROMA_CENTER;
    struct pl_render_params params = {0};
    hook_updates = 0;
    update_hook_opts_dynamic(&p, &hooks[0], &mpi[0], &params);
    assert(params.num_source_parameters == 2);
    assert(params.source_parameters[0] == &first[0] && params.source_parameters[1] == &first[1]);
    update_hook_opts_dynamic(&p, &hooks[1], &mpi[0], &params);
    assert(params.num_source_parameters == 4 && params.source_parameters == p.source_parameters);
    assert(params.source_parameters[2] == &second[0] && params.source_parameters[3] == &second[1]);
    update_hook_opts_dynamic(&p, &hooks[0], &mpi[0], &params);
    assert(params.num_source_parameters == 6 && params.source_parameters[4] == &first[0]);
    assert(params.source_parameters[5] == &first[1]);
    assert(values[0].f == 1.25f && values[1].i == 101 && values[2].u == 101);
    assert(values[3].f == 1.5f && values[4].f == -.5f);
    const struct pl_hook_par *bindings[6];
    for (int i = 0; i < 6; i++)
        bindings[i] = params.source_parameters[i];
    const void *allocation = params.source_parameters;
    for (int i = 0; i < 2; i++) {
        struct pl_frame view = {.user_data = &mpi[i]};
        assert(view_acquire(NULL, &view));
        assert(values[0].f == (float)mpi[i].pts);
        assert(values[1].i == (i ? 203 : 101) && values[2].u == (i ? 203u : 101u));
        assert(values[4].f == (i ? 0 : -.5f) && values[3].f == 1.5f);
        assert(params.source_parameters == allocation && params.num_source_parameters == 6);
        for (int j = 0; j < 6; j++)
            assert(bindings[j] == params.source_parameters[j]);
    }
    // Model caller initialization only: reset statements in draw_frame are
    // checked by the generator, not replaced with a fake rendering controller.
    params.source_parameters = NULL;
    params.num_source_parameters = 0;
    struct pl_hook unknown = {.parameters = &first[2], .num_parameters = 1};
    update_hook_opts_dynamic(&p, &unknown, &mpi[1], &params);
    assert(!params.source_parameters && !params.num_source_parameters);
    update_hook_opts_dynamic(&p, &hooks[1], &mpi[1], &params);
    assert(params.num_source_parameters == 2 && params.source_parameters[0] == &second[0]);
    assert(params.source_parameters[1] == &second[1]);
    free(p.source_parameters);
    puts("gpu-next actual auto-parameter updates and source binding collection: passed");
}

static void test_field_queue_registration(void)
{
    struct pl_deinterlace_params deint = {.algo = PL_DEINTERLACE_BOB};
    struct pl_render_params params = {.deinterlace_params = &deint};
    assert(queue_fields_enabled(&params));
    deint.algo = PL_DEINTERLACE_WEAVE;
    assert(!queue_fields_enabled(&params));
    deint.algo = PL_DEINTERLACE_BOB;
    params.disable_fbos = true;
    assert(!queue_fields_enabled(&params));

    struct priv p = {.queue_fields = true, .motion = (void *)1, .motion_fps = 60};
    struct vo vo = {.priv = &p};
    struct mp_image first = {.imgfmt = IMGFMT_YUV420P, .pts = 1, .pkt_duration = .125,
        .fields = MP_IMGFIELD_INTERLACED | MP_IMGFIELD_TOP_FIRST};
    struct mp_image second = {.imgfmt = IMGFMT_YUV420P, .pts = 1.25, .pkt_duration = .5,
        .fields = MP_IMGFIELD_INTERLACED};
    struct mp_image *images[] = {&first, &second};
    struct vo_frame frame = {.frame_id = 100, .num_frames = 2, .frames = images,
        .approx_duration = .75};
    assert(queue_frame(&vo, &frame, 0, true, true, false) == 1);
    assert(queue_frame(&vo, &frame, 1, true, true, false) == 1);
    assert(queued_frames[0].first_field == PL_FIELD_TOP && queued_frames[0].duration == .25);
    assert(queued_frames[1].first_field == PL_FIELD_BOTTOM && queued_frames[1].duration == .5);
    discard_queued_frames();

    frame.current = &first;
    frame.vsync_interval = 1.0 / 120;
    frame.ideal_frame_duration = 1.0 / 30;
    assert(!motion_interpolation_needed(&p, &frame));
    p.motion_fps = 120;
    assert(motion_interpolation_needed(&p, &frame));
    frame.vsync_interval = 1.0 / 60;
    assert(!motion_interpolation_needed(&p, &frame));
    frame.ideal_frame_duration = 1.0 / 25;
    assert(motion_interpolation_needed(&p, &frame));

    second.pts = NAN;
    assert(field_source_duration(&frame, 0) == .125);
    first.pkt_duration = NAN;
    assert(field_source_duration(&frame, 0) == 0);
    first.imgfmt = IMGFMT_MEDIACODEC;
    assert(source_first_field(&first) == PL_FIELD_NONE);
    first.imgfmt = IMGFMT_YUV420P;
    first.enhancement_layer = &second;
    assert(source_first_field(&first) == PL_FIELD_NONE);
    second.fields = 0;
    assert(source_first_field(&second) == PL_FIELD_NONE);
    p.queue_fields = false;
    first.enhancement_layer = NULL;
    assert(queue_frame(&vo, &frame, 0, true, true, false) == 1);
    assert(queued_frame.first_field == PL_FIELD_NONE && queued_frame.duration == .75);
    discard_queued_frames();
    puts("gpu-next registers software field order and source-specific durations");
}

static void test_prepared_routing(void)
{
    struct priv p = {.gpu = (void *)1, .motion = (void *)2, .motion_views = true,
                     .source_peak = (void *)3, .source_epoch = 7};
    struct vo vo = {.priv = &p};
    struct frame_priv fp[3] = {{.motion_id = 10, .motion_texture = (void *)1},
                              {.motion_id = 40, .motion_texture = (void *)2},
                              {.motion_id = 90, .motion_texture = (void *)3}};
    struct mp_image mpi[3] = {{.priv = &fp[0]}, {.priv = &fp[1]}, {.priv = &fp[2]}};
    struct pl_frame images[3] = {{.user_data = &mpi[0]}, {.user_data = &mpi[1]},
                                 {.user_data = &mpi[2]}};
    const struct pl_frame *frames[] = {&images[0], &images[1], &images[2]};
    uint64_t signatures[] = {100010, 100040, 100090};
    uint64_t view_signatures[] = {210, 240, 290};
    double times[] = {-1.25, -.25, .75};
    struct pl_frame_mix mix = {.num_frames = 3, .frames = frames,
        .signatures = signatures, .timestamps = times};
    struct vo_frame event = {.frame_id = 40};
    struct pl_frame target = {.crop = {0, 0, 32, 32}};
    struct pl_render_params params = {0};
    int pair[2], result;
    float alpha;
    assert(select_motion_pair(&mix, pair, &alpha) == 1);
    assert(pair[0] == 1 && pair[1] == 2 && alpha == .25f);
    times[1] = 0;
    assert(select_motion_pair(&mix, pair, &alpha) == 0);
    times[1] = -DBL_MIN;
    assert(select_motion_pair(&mix, pair, &alpha) == 0);
    times[1] = NAN;
    assert(select_motion_pair(&mix, pair, &alpha) == -1);
    times[1] = -.25;
    num_queued_frames = 1;
    queued_frames[0] = (struct pl_source_frame){.frame_data = &mpi[0]};
    timed_calls = untimed_calls = direct_calls = view_flow_calls = infer_calls = 0;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(result == 1 && timed_calls == 1 && view_flow_calls == 1 && routed_custom);
    assert(routed_count == 2 && routed_ids[0] == 40 && routed_ids[1] == 90);
    assert(routed_signatures[0] == 240 && routed_signatures[1] == 290);
    assert(routed_context.current_id == 40 && routed_context.retire_before_id == 10);
    assert(routed_views[0].signature == 801 && routed_views[1].signature == 802);
    assert(routed_views[0].texture == (void *)81 && routed_alpha == .25f);
    fp[2].peak_status = PL_SOURCE_PEAK_UNSUPPORTED;
    images[2].color.transfer = PL_COLOR_TRC_HLG;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(!result && !routed_custom && view_flow_calls == 1);
    target.color.transfer = PL_COLOR_TRC_PQ;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(result == 1 && routed_custom && view_flow_calls == 2);
    images[2].color.transfer = PL_COLOR_TRC_PQ;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(!result && !routed_custom && view_flow_calls == 2);
    target.color.transfer = PL_COLOR_TRC_BT_1886;
    fp[2].peak_status = PL_SOURCE_PEAK_READY;
    view_flow_result = MP_MOTION_ERROR;
    assert(!render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(result == -1 && !untimed_calls && !direct_calls);
    view_flow_result = MP_MOTION_OK;
    render_boundary_ok = false;
    assert(!render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(result == -1);
    render_boundary_ok = true;
    int flow = view_flow_calls;
    p.motion = NULL;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, false, &result));
    assert(!result && !routed_custom && view_flow_calls == flow);
    p.motion = (void *)2;
    p.paused = true; // draw passes can_interpolate=false when paused.
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, false, &result));
    assert(!result && !routed_custom && view_flow_calls == flow);
    p.paused = false;
    params.lut = (void *)5;
    params.lut_type = PL_LUT_CONVERSION;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(!result && !routed_custom && view_flow_calls == flow);
    params.lut = NULL;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(result == 1 && routed_custom && view_flow_calls == ++flow);
    params.lut = (void *)5;
    const int compatible_luts[] = {PL_LUT_NATIVE, PL_LUT_NORMALIZED};
    for (int i = 0; i < 2; i++) {
        params.lut_type = compatible_luts[i];
        assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
        assert(result == 1 && routed_custom && view_flow_calls == ++flow);
    }
    params.lut = NULL;
    params.lut_type = PL_LUT_UNKNOWN;

    const struct pl_deinterlace_params deint = {.algo = PL_DEINTERLACE_BOB};
    mpi[1].imgfmt = IMGFMT_YUV420P;
    mpi[1].fields = MP_IMGFIELD_INTERLACED | MP_IMGFIELD_TOP_FIRST;
    images[2].user_data = &mpi[1];
    images[1].first_field = images[2].first_field = PL_FIELD_TOP;
    images[1].field = PL_FIELD_TOP;
    images[2].field = PL_FIELD_BOTTOM;
    params.deinterlace_params = &deint;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(result == 1 && routed_custom && view_flow_calls == ++flow);
    assert(routed_ids[0] == 40 && routed_ids[1] == 40);
    assert(routed_signatures[0] == 240 && routed_signatures[1] == 290);
    params.deinterlace_params = NULL;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(!result && !routed_custom && view_flow_calls == flow);
    assert(routed_signatures[1] == signatures[1] && routed_signatures[2] == signatures[2]);
    images[2].user_data = &mpi[2];
    mpi[1].fields = 0;
    images[1].field = images[2].field = PL_FIELD_NONE;
    images[1].first_field = images[2].first_field = PL_FIELD_NONE;

    p.want_seek_reset = true;
    int timed = timed_calls, submitted = source_submits;
    assert(render_source_mix(&vo, &event, &mix, view_signatures, &target, &params, true, &result));
    assert(untimed_calls == 1 && timed_calls == timed && view_flow_calls == flow);
    assert(source_submits == submitted && !result);
    p.want_seek_reset = false;
    p.motion_views = false;
    described_unsupported = false;
    motion_render_fail = false;
    fp[1].motion_id = 10; fp[1].motion_texture = (void *)1;
    fp[2].motion_id = 11; fp[2].motion_texture = (void *)2;
    struct pl_frame_mix legacy = mix;
    legacy.num_frames = 2;
    legacy.frames = frames + 1;
    legacy.timestamps = times + 1;
    legacy.signatures = signatures + 1;
    event.frame_id = 10;
    assert(render_source_mix(&vo, &event, &legacy, view_signatures, &target, &params, true, &result));
    assert(result == 1 && direct_calls == 1 && timed_calls == timed);
    assert(routed_context.current_id == 10 && routed_ids[0] == 10);
    p.motion_views = true;
    shader_supported = true;
    shader_calls = shader_outputs = 0;
    event.vsync_interval = 1.0 / 120;
    p.motion_fps = 120;
    assert(render_source_mix(&vo, &event, &legacy, view_signatures, &target, &params, true, &result));
    assert(result == 1 && shader_calls == 1 && shader_outputs == 1);
    render_boundary_ok = false;
    assert(!render_source_mix(&vo, &event, &legacy, view_signatures, &target, &params, true, &result));
    assert(result == -1 && shader_calls == 2 && shader_outputs == 1);
    render_boundary_ok = true;
    view_flow_result = MP_MOTION_ERROR;
    assert(!render_source_mix(&vo, &event, &legacy, view_signatures, &target, &params, true, &result));
    assert(result == -1 && shader_calls == 3 && shader_outputs == 1);
    view_flow_result = MP_MOTION_OK;
    p.motion_fps = 60;
    assert(render_source_mix(&vo, &event, &legacy, view_signatures, &target, &params, true, &result));
    assert(result == 1 && routed_custom && shader_calls == 3 && shader_outputs == 1);
    p.motion_fps = 120;
    event.redraw = true;
    assert(render_source_mix(&vo, &event, &legacy, view_signatures, &target, &params, true, &result));
    assert(result == 1 && routed_custom && shader_calls == 3 && shader_outputs == 1);
    shader_supported = false;
    event.redraw = false;
    assert(render_source_mix(&vo, &event, &legacy, view_signatures, &target, &params, true, &result));
    assert(result == 1 && routed_custom && shader_calls == 3 && shader_outputs == 1);
    for (int shader = 0; shader < 2; shader++) {
        shader_supported = shader;
        renderer_error_mask = shader ? 0x105 : 0;
        for (int failure = 0; failure < 5; failure++) {
            skip_motion_callback = failure == 0;
            prepared_count = failure == 1 ? 1 : 2;
            view_flow_result = failure == 2 ? MP_MOTION_UNSUPPORTED
                : failure == 3 ? MP_MOTION_ERROR : MP_MOTION_OK;
            render_boundary_ok = failure != 4;
            const char *stages[] = {"callback not invoked", "invalid callback frame count",
                "motion unsupported", "motion error", "libplacebo output"};
            error_log[0] = 0;
            int reads = renderer_error_reads;
            assert(!render_source_mix(&vo, &event, &legacy, view_signatures,
                                     &target, &params, true, &result));
            assert(result == -1 && strstr(error_log, stages[failure]));
            assert(strstr(error_log, shader ? "shader failed" : "texture failed"));
            assert(strstr(error_log, "t0=") && strstr(error_log, "alpha="));
            assert(strstr(error_log, shader ? "errors=0x105," : "errors=0x0,"));
            assert(renderer_error_reads == reads + 1);
            assert(renderer_error_mask == (shader ? 0x105 : 0));
        }
    }
    skip_motion_callback = false;
    prepared_count = 2;
    render_boundary_ok = true;
    view_flow_result = MP_MOTION_OK;
    error_log[0] = 0;
    int reads = renderer_error_reads;
    assert(render_source_mix(&vo, &event, &legacy, view_signatures,
                             &target, &params, true, &result));
    assert(result == 1 && !error_log[0]);
    assert(renderer_error_reads == reads && renderer_error_mask == 0x105);
    double invalid_times[] = {NAN, .75};
    legacy.timestamps = invalid_times;
    assert(!render_source_mix(&vo, &event, &legacy, view_signatures,
                              &target, &params, true, &result));
    assert(result == -1 && strstr(error_log, "Invalid motion pair timestamps"));
    shader_supported = false;
    struct pl_frame_mix single = mix;
    single.num_frames = 1;
    prepared_count = 1;
    // Existing renderer degradation keeps the ordinary single-source route.
    assert(renderer_error_mask == 0x105);
    assert(render_source_mix(&vo, &event, &single, view_signatures,
                             &target, &params, false, &result));
    assert(result == 0 && !routed_custom && !view_prepare_calls);
    assert(renderer_error_mask == 0x105);
    renderer_error_mask = 0;
    int previous_flow = view_flow_calls;
    int previous_outputs = shader_outputs;
    assert(render_source_mix(&vo, &event, &single, view_signatures,
                             &target, &params, false, &result));
    assert(result == 0 && routed_custom && routed_count == 1);
    assert(view_prepare_calls == 1 && view_flow_calls == previous_flow);
    assert(shader_outputs == previous_outputs);
    assert(routed_ids[0] == fp[0].motion_id && routed_signatures[0] == view_signatures[0]);
    view_flow_result = MP_MOTION_ERROR;
    assert(!render_source_mix(&vo, &event, &single, view_signatures,
                              &target, &params, false, &result));
    assert(result == -1 && view_prepare_calls == 2);
    view_flow_result = MP_MOTION_OK;
    render_boundary_ok = false;
    int single_calls = timed_calls;
    int ordinary_calls = untimed_calls;
    assert(!render_source_mix(&vo, &event, &single, view_signatures,
                              &target, &params, false, &result));
    assert(result == -1 && view_prepare_calls == 3);
    assert(timed_calls == single_calls + 1 && untimed_calls == ordinary_calls);
    render_boundary_ok = true;
    params.disable_fbos = true;
    assert(render_source_mix(&vo, &event, &single, view_signatures,
                             &target, &params, false, &result));
    assert(!routed_custom && result == 0 && view_prepare_calls == 3);
    params.disable_fbos = false;
    p.want_seek_reset = true;
    assert(render_source_mix(&vo, &event, &single, view_signatures,
                             &target, &params, false, &result));
    assert(view_prepare_calls == 3);
    p.want_seek_reset = false;
    shader_supported = true;
    int previous_shaders = shader_calls;
    int previous_ordinary = untimed_calls;
    assert(render_source_mix(&vo, &event, &single, view_signatures,
                             &target, &params, false, &result));
    assert(result == 0 && shader_calls == previous_shaders + 1);
    assert(view_prepare_calls == 4 && shader_outputs == previous_outputs);
    view_flow_result = MP_MOTION_ERROR;
    assert(!render_source_mix(&vo, &event, &single, view_signatures,
                              &target, &params, false, &result));
    assert(result == -1 && shader_calls == previous_shaders + 2);
    view_flow_result = MP_MOTION_OK;
    render_boundary_ok = false;
    assert(!render_source_mix(&vo, &event, &single, view_signatures,
                              &target, &params, false, &result));
    assert(result == -1 && shader_calls == previous_shaders + 3);
    assert(untimed_calls == previous_ordinary);
    render_boundary_ok = true;
    shader_supported = false;
    prepared_count = 2;
    memset(queued_frames, 0, sizeof(queued_frames));
    num_queued_frames = 0;
    puts("gpu-next production prepared routing: boundary protocol only");
}

static void test_decoder_flush(void)
{
    struct ra ra = {.uses_pl = true};
    struct ra_ctx ctx = {.ra = &ra};
    const struct ra_hwdec_mapper_driver consuming = {.consumes_source = true};
    const struct ra_hwdec_mapper_driver ordinary = {0};
    struct ra_hwdec_driver driver = {.mapper = &consuming};
    struct ra_hwdec hwdec = {.ra = &ra, .driver = &driver};
    struct priv p = {.ra_ctx = &ctx, .hwdec_ctx = &hwdec};
    struct vo vo = {.priv = &p};
    struct mp_image sw = {.imgfmt = -12345};
    struct mp_image hw = {.imgfmt = IMGFMT_RGB0};
    struct mp_image *images[] = {&sw, &hw};
    struct vo_frame frame = {.num_frames = 2, .frames = images};

    // Lookahead and enhancement layers are classified before mapper creation.
    assert(!p.hwdec.mapper && prepare_decoder_flush(&p, &frame));
    frame.num_frames = 1;
    assert(!prepare_decoder_flush(&p, &frame));
    sw.enhancement_layer = &hw;
    assert(prepare_decoder_flush(&p, &frame));
    sw.enhancement_layer = NULL;
    driver.mapper = &ordinary;
    frame.num_frames = 2;
    assert(!prepare_decoder_flush(&p, &frame));
    driver.mapper = &consuming;

    // Isolate each owner: neither path has a mapper or current/lookahead.
    struct mp_image *pending = new_snapshot_frame(&vo, 1099);
    p.pending_frame = pending;
    assert(prepare_decoder_flush(&p, NULL) && !p.pending_frame);
    struct mp_image *cached = new_snapshot_frame(&vo, 1100);
    struct source_frame *source = get_source_frame(&p, cached, 1100);
    unref_source_frame(&p, source); // Only the cache retains this source.
    talloc_free(cached);
    assert(p.num_source_frames == 1 && !p.hwdec.mapper && !p.el_hwdec.mapper);
    assert(prepare_decoder_flush(&p, NULL));
    assert(!p.num_source_frames && !live_image_refs);

    // A pending enhancement mapping and independent source-cache references
    // must both retire, even when the core no longer has a current frame.
    bool saved_deferred = deferred_params;
    deferred_params = false;
    struct mp_image *bl = new_snapshot_frame(&vo, 1101);
    struct mp_image *el = new_snapshot_frame(&vo, 1102);
    bl->enhancement_layer = el;
    el->retries = 1;
    p.pending_frame = bl;
    assert(preload_deferred_hwdec_frame(&p, bl->priv, bl) == RA_HWDEC_MAP_RETRY);
    assert(p.num_source_frames == 1 && p.hwdec.mapper && p.el_hwdec.mapper);
    int frees = calls.frees;
    uint64_t epoch = p.source_epoch;
    assert(prepare_decoder_flush(&p, NULL));
    assert(!p.pending_frame && !p.num_source_frames && !num_queued_frames);
    assert(!p.hwdec.mapper && !p.el_hwdec.mapper && calls.frees == frees + 2);
    assert(!live_copies && !live_image_refs && p.source_epoch == epoch);
    talloc_free(el); // The fixture models the EL reference separately.

    // A queued source can outlive the core's current frame. New-generation
    // mapping must still work after the queue/cache/mapper teardown.
    bl = new_snapshot_frame(&vo, 1103);
    assert(preload_deferred_hwdec_frame(&p, bl->priv, bl) == 1);
    pl_queue_push(NULL, &(struct pl_source_frame){
        .frame_data = bl, .discard = discard_frame,
    });
    assert(prepare_decoder_flush(&p, NULL));
    assert(!num_queued_frames && !p.num_source_frames && !live_copies);
    assert(!live_image_refs && !p.hwdec.mapper);

    // Mapper-only ownership also needs retirement, including the EL slot.
    assert(slot_reconfig(&p, &p.el_hwdec, &hwdec, &original));
    assert(prepare_decoder_flush(&p, NULL));
    assert(!p.el_hwdec.mapper && !prepare_decoder_flush(&p, NULL));
    deferred_params = saved_deferred;
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
    test_motion_snapshot_blocked();
    test_motion_capability_transition();
    test_motion_frame_alpha();
    test_motion_source_slot();
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
    test_snapshot_lifecycle();
    test_decoder_flush();
    test_pending_queue_entry();
    test_source_cache_reset();
    test_snapshot_metadata();
    test_capture_selector();
    test_seek_reset();
    test_source_protocol();
    test_source_parameters();
    test_field_queue_registration();
    test_prepared_routing();
    assert(!live_image_refs && !num_queued_frames && !live_copies);
    puts("gpu-next motion target preserves source cadence and playback speed");
    puts("gpu-next mapper aspect, format, metadata and failure tests passed");
    return 0;
}
