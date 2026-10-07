/* Host fixture for the production Android Surface request/submit functions. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define VO_EVENT_WIN_STATE 1
#define MPV_FORMAT_INT64 4
#define MPV_FORMAT_NODE_ARRAY 7
#define bstr0(x) (x)
#define HAVE_ANDROID 1
#define VO_TRUE 1
#define VO_FALSE 0
#define VO_NOTIMPL -1
#define VOCTRL_RESET 1
#define VOCTRL_EXTERNAL_RESIZE 2
#define VOCTRL_UPDATE_RENDER_OPTS 3
#define VOCTRL_UPDATE_OSD_SIZE 4
#define VOCTRL_UPDATE_WINDOW 5
#define VOCTRL_UPDATE_OSD_SURFACE 6
#define VOCTRL_SET_PANSCAN 9
#define MP_NOPTS_VALUE -1
#define MP_ERR(vo, ...) ((void)(vo))
#define MP_VERBOSE(vo, ...) ((void)(vo))
#define MAX_OSD_FAILURES 3
#define EGL_NO_SURFACE 0
#define EGL_WIDTH 1
#define EGL_HEIGHT 2
#define VOCTRL_GET_ANDROID_VIDEO_SURFACE_TRANSFORM 7
#define VOCTRL_GET_ANDROID_SURFACE_FRAME 8
#define talloc_free(ctx) ((void)(ctx))
#define M_PROPERTY_GET 1
#define M_PROPERTY_GET_TYPE 2
#define M_PROPERTY_OK 1
#define M_PROPERTY_NOT_IMPLEMENTED -1
#define CONF_TYPE_STRING 1
#define CONF_TYPE_BOOL 2

#include "android_surface_frame_types.h"

typedef struct ANativeWindow { int refs, width, height; } ANativeWindow;
typedef int EGLint;
typedef struct AVMediaCodecBuffer { int unused; } AVMediaCodecBuffer;
struct mp_image { void *planes[4]; };
struct mp_log { int unused; };
struct mpv_node_list;
struct mpv_node {
    union { int64_t int64; struct mpv_node_list *list; } u;
    int format;
};
struct mpv_node_list { int num; struct mpv_node *values; };
struct mp_vo_opts {
    int64_t WinID, android_osd_wid;
    struct mpv_node android_surface_frame;
    struct { int w, h; } android_surface_size;
    bool android_video_surface_transform;
};
struct vo_android_state;
struct vo_internal {
    int lock;
    bool hasframe_rendered, android_video_surface_transform;
    struct vo_android_surface_frame android_surface_frame;
};
struct vo;
struct vo_driver { int (*control)(struct vo *, int, void *); };
struct vo {
    struct mp_vo_opts *opts;
    struct vo_android_state *android;
    struct vo_internal *in;
    struct vo_driver *driver;
    void *priv;
    int dwidth, dheight;
    bool want_redraw;
};
struct mp_image_params { int unused; };
struct ra_ctx_fns { const char *type; };
struct ra_swapchain;
struct ra_swapchain_fns {
    void (*swap_buffers)(struct ra_swapchain *);
    int (*color_depth)(struct ra_swapchain *);
};
struct ra_swapchain { struct vo *vo; const struct ra_swapchain_fns *fns; };
struct ra_ctx {
    struct ra_swapchain *swapchain;
    const struct ra_ctx_fns *fns;
    struct vo *vo;
    void *priv;
};
typedef void *pl_swapchain;
struct mp_rect { int x0, y0, x1, y1; };
struct mp_osd_res { int w, h, ml, mr, mt, mb; double display_par, video_aspect; };
struct osd_object { struct mp_osd_res vo_res; bool osd_changed; };
struct mpv_global { void *client_api; };
struct osd_state { struct mpv_global *global; };
struct priv {
    struct ra_ctx *ra_ctx;
    bool frame_pending;
    void *osd_overlay;
    bool submitted_video;
    int64_t video_wid;
    struct mp_image *next_image;
    double last_pts;
    bool osd_render_ok;
    int osd_failures;
    void *context;
    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
    int osd_sync;
};
struct gpu_priv {
    struct ra_ctx *ctx;
    void *renderer, *osd_overlay;
};
struct android_priv { int egl_display, egl_surface, buffer_width, buffer_height; };
struct MPOpts { struct mp_vo_opts *vo; };
struct MPContext { struct MPOpts *opts; struct mp_vo_opts *mconfig; struct vo *video_out; };
typedef struct MPContext MPContext;
struct mp_cmd_arg { union { int64_t i64; int i; } v; };
struct mp_cmd_ctx { struct MPContext *mpctx; struct mp_cmd_arg args[3]; bool success; };
struct m_property { int unused; };
struct m_option { int type; };

static int m_property_bool_ro(int action, void *arg, bool value)
{
    if (action == M_PROPERTY_GET_TYPE)
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_BOOL};
    else if (action == M_PROPERTY_GET)
        *(bool *)arg = value;
    else
        return M_PROPERTY_NOT_IMPLEMENTED;
    return M_PROPERTY_OK;
}

static char *talloc_asprintf(void *ctx, const char *format, ...)
{
    (void)ctx;
    char *out = malloc(128);
    assert(out);
    va_list args;
    va_start(args, format);
    vsnprintf(out, 128, format, args);
    va_end(args);
    return out;
}

static int events;
static bool submit_ok = true;
static int swaps;
static int codec_releases, backend_errors, osd_surface_updates;
static bool codec_release_ok = true, egl_swap_ok = true, egl_size_known = true;
static int drawable_w, drawable_h, next_drawable_w, next_drawable_h;
static int transform_queries;
static int synchronous_queries;
static bool transform_query_supported = true;
void vo_android_surface_frame_presented(struct vo *vo, int w, int h);
static void mp_mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mp_mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static int transform_control(struct vo *vo, int request, void *out)
{
    assert(!vo->in->lock);
    assert(request == VOCTRL_GET_ANDROID_VIDEO_SURFACE_TRANSFORM);
    transform_queries++;
    *(bool *)out = vo->opts->android_video_surface_transform;
    return transform_query_supported ? VO_TRUE : VO_NOTIMPL;
}
int vo_control(struct vo *vo, int request, void *out)
{
    (void)vo; (void)request; (void)out;
    synchronous_queries++;
    return VO_NOTIMPL;
}
static pl_swapchain get_active_swapchain(struct priv *p) { return p; }
static bool pl_swapchain_submit_frame(pl_swapchain sw) { (void)sw; return submit_ok; }
static bool android_osd_overlay_present(void *overlay) { (void)overlay; return true; }
static int osd_geometry_invalidations;
static void android_osd_overlay_invalidate_geometry(void *overlay)
{
    (void)overlay;
    osd_geometry_invalidations++;
}
static struct mp_rect video_rect;
static int geometry_updates, renderer_resizes, framebuffer_depth;
static void android_osd_overlay_get_video_rects(void *overlay, struct mp_rect *src,
    struct mp_rect *dst, struct mp_osd_res *osd)
{
    (void)overlay;
    *src = (struct mp_rect){0, 0, 1920, 1080};
    *dst = video_rect;
    *osd = (struct mp_osd_res){2400, 1080};
    geometry_updates++;
}
static void gl_video_resize(void *renderer, struct mp_rect *src,
    struct mp_rect *dst, struct mp_osd_res *osd)
{
    (void)renderer;
    assert(src->x1 == 1920 && dst->x1 == video_rect.x1 && osd->w == 2400);
    renderer_resizes++;
}
static void gl_video_set_fb_depth(void *renderer, int depth)
{
    (void)renderer;
    framebuffer_depth = depth;
}
static void gpu_ctx_resize(void *context, int w, int h)
{
    (void)context;
    assert(w == 2400 && h == 1080);
}
static bool mp_rect_equals(struct mp_rect *a, struct mp_rect *b)
{
    return a->x0 == b->x0 && a->y0 == b->y0 && a->x1 == b->x1 && a->y1 == b->y1;
}
static int osd_events, last_osd_event;
static void mp_client_broadcast_event_external(void *api, int event, void *data)
{
    (void)api;
    assert(!data);
    osd_events++;
    last_osd_event = event;
}
static bool android_osd_overlay_set_surface(void *overlay, int64_t wid)
{
    (void)overlay; (void)wid;
    osd_surface_updates++;
    return true;
}
static bool android_osd_overlay_get_size(void *overlay, int *w, int *h)
{
    (void)overlay; (void)w; (void)h; return false;
}
static void vo_report_backend_error(struct vo *vo) { (void)vo; assert(false); }
static void mp_image_unrefp(struct mp_image **image) { *image = NULL; }
static int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    assert(buffer && render == 1);
    codec_releases++;
    return codec_release_ok ? 0 : -1;
}
static void report_backend_error(struct vo *vo, const char *component)
{
    (void)vo; (void)component;
    backend_errors++;
}
static bool eglQuerySurface(int display, int surface, int dimension, int *size)
{
    (void)display; (void)surface;
    *size = dimension == EGL_WIDTH ? drawable_w : drawable_h;
    return egl_size_known;
}
static bool eglSwapBuffers(int display, int surface)
{
    (void)display; (void)surface;
    if (!egl_swap_ok)
        return false;
    drawable_w = next_drawable_w;
    drawable_h = next_drawable_h;
    return true;
}
static void swap_buffers(struct ra_swapchain *sw)
{
    swaps++;
    // Model a successful EGL swap; GPU-next submit failure must prevent its ack.
    vo_android_surface_frame_presented(sw->vo, sw->vo->dwidth, sw->vo->dheight);
}
static void vo_event(struct vo *vo, int event) { assert(!vo->in->lock); assert(event == 1); events++; }
static void ANativeWindow_release(ANativeWindow *window) { window->refs--; }
static int ANativeWindow_getWidth(ANativeWindow *window) { return window->width; }
static int ANativeWindow_getHeight(ANativeWindow *window) { return window->height; }
static int32_t ANativeWindow_getFormat(ANativeWindow *window) { (void)window; return 0; }
static int ANativeWindow_setBuffersGeometry(ANativeWindow *window, int w, int h, int format)
{
    (void)window; (void)format;
    next_drawable_w = w;
    next_drawable_h = h;
    return 0;
}
static int m_config_set_option_node(struct mp_vo_opts *opts, const char *name,
                                    struct mpv_node *request, int flags)
{
    assert(!strcmp(name, "android-surface-frame") && flags == 0);
    assert(request->format == MPV_FORMAT_NODE_ARRAY && request->u.list->num == 4);
    // Model the cache's deep copy; the command's stack nodes must not escape.
    struct mpv_node_list *list = opts->android_surface_frame.u.list;
    memcpy(list->values, request->u.list->values, 4 * sizeof(struct mpv_node));
    return 0;
}

#include "android_surface_frame_functions.h"

static void request_frame(struct vo *vo, int64_t token, int w, int h)
{
    struct MPOpts opts = {.vo = vo->opts};
    struct MPContext ctx = {.opts = &opts, .mconfig = vo->opts};
    struct mp_cmd_ctx cmd = {
        .mpctx = &ctx,
        .args = {{.v.i64 = token}, {.v.i = w}, {.v.i = h}},
    };
    cmd_android_surface_frame(&cmd);
    assert(cmd.success);
}

static int64_t completed(struct vo *vo)
{
    struct vo_android_surface_frame frame;
    vo_get_android_surface_frame(vo, vo->opts, &frame);
    return frame.token;
}

static void test_geometry_resize(struct vo *vo)
{
    int state_events = events;
    void *previous_priv = vo->priv;
    struct ra_swapchain_fns fns = {0};
    struct ra_swapchain swapchain = {.fns = &fns};
    struct ra_ctx ctx = {.swapchain = &swapchain};
    struct gpu_priv gpu = {.ctx = &ctx};
    struct priv gpu_next = {0};
    vo->dwidth = 2400;
    vo->dheight = 1080;
    // Scaling or panning inside a fixed Surface must not invalidate display metadata.
    for (int n = 0; n < 3; n++) {
        video_rect = (struct mp_rect){-n * 100, 0, 2400 + n * 100, 1080};
        vo->priv = &gpu;
        vo->want_redraw = false;
        gpu_resize(vo);
        assert(vo->want_redraw && framebuffer_depth == 0);
        vo->priv = &gpu_next;
        vo->want_redraw = false;
        gpu_next_resize(vo);
        assert(vo->want_redraw && gpu_next.osd_sync == n + 1);
        assert(gpu_next.dst.x0 == video_rect.x0 && gpu_next.osd_res.w == 2400);
        assert(events == state_events);
    }
    gpu_next_resize(vo);
    assert(gpu_next.osd_sync == 3 && events == state_events);
    assert(renderer_resizes == 3 && geometry_updates == 7);
    vo->priv = previous_priv;
}

static bool event_has_property(const char *const *properties, const char *name)
{
    for (int n = 0; properties[n]; n++) {
        if (!strcmp(properties[n], name))
            return true;
    }
    return false;
}

static void test_osd_resize_events(void)
{
    struct mpv_global global = {0};
    struct osd_state osd = {.global = &global};
    struct mp_osd_res res = {.w = 2400, .h = 1080, .display_par = 1};
    struct osd_object object = {.vo_res = res};
    // Video margins change the OSD geometry, not the window geometry.
    res.ml = res.mr = 240;
    check_obj_resize(&osd, res, &object);
    assert(object.osd_changed && object.vo_res.ml == 240);
    assert(osd_events == 1 && last_osd_event == MP_EVENT_OSD_RESIZE);
    const char *names[] = {"osd-width", "osd-height", "osd-par", "osd-dimensions"};
    for (int n = 0; n < 4; n++) {
        assert(event_has_property(event_MP_EVENT_OSD_RESIZE, names[n]));
        assert(event_has_property(event_MP_EVENT_WIN_RESIZE, names[n]));
    }
    assert(!event_has_property(event_MP_EVENT_OSD_RESIZE, "current-window-scale"));
    assert(!event_has_property(event_MP_EVENT_OSD_RESIZE, "display-names"));
    assert(event_has_property(event_MP_EVENT_WIN_RESIZE, "current-window-scale"));
    assert(event_has_property(event_MPV_EVENT_VIDEO_RECONFIG,
                              "current-window-scale"));
    assert(event_has_property(event_MP_EVENT_WIN_STATE, "display-names"));
    object.osd_changed = false;
    check_obj_resize(&osd, res, &object);
    assert(!object.osd_changed && osd_events == 1);
    // OSD buffer size changes still invalidate all OSD dimension properties.
    res.w = 1920;
    check_obj_resize(&osd, res, &object);
    assert(object.osd_changed && osd_events == 2);
    assert(last_osd_event == MP_EVENT_OSD_RESIZE);
    // Pixel aspect changes are also confined to OSD observers.
    object.osd_changed = false;
    res.display_par = 1.5;
    check_obj_resize(&osd, res, &object);
    assert(object.osd_changed && osd_events == 3);
    assert(last_osd_event == MP_EVENT_OSD_RESIZE);
}

int main(void)
{
    test_osd_resize_events();
    struct mpv_node values[4];
    struct mpv_node_list list = {.num = 4, .values = values};
    struct mp_vo_opts opts = {
        .WinID = 10,
        .android_surface_frame = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &list},
    };
    ANativeWindow window = {.refs = 1}, replacement = {.refs = 1};
    struct vo_android_state android = {0};
    struct vo_internal core = {0};
    struct vo_driver backend = {.control = transform_control};
    struct vo vo = {.opts = &opts, .android = &android, .in = &core, .driver = &backend};
    vo_android_set_native_window(&vo, &window);
    request_frame(&vo, 1, 1920, 1080);

    // Layout, resize acceptance and the old output's frame are insufficient.
    assert(completed(&vo) == 0);
    vo_android_surface_frame_drawn(&vo, 2400, 1080);
    vo_android_surface_frame_presented(&vo, 2400, 1080);
    assert(completed(&vo) == 0);
    vo_android_surface_frame_drawn(&vo, 1920, 1080);
    assert(completed(&vo) == 0);
    vo_android_surface_frame_presented(&vo, 1920, 1080);
    assert(completed(&vo) == 1);

    // Core reads only published state, even while the VO has not applied new options.
    struct mp_vo_opts requested = opts;
    struct MPOpts player_opts = {.vo = &requested};
    struct MPContext player = {.opts = &player_opts, .video_out = &vo};
    struct m_option type;
    bool active;
    char *frame;
    assert(mp_property_android_video_surface_transform(&player, NULL,
        M_PROPERTY_GET_TYPE, &type) == M_PROPERTY_OK);
    assert(type.type == CONF_TYPE_BOOL && transform_queries == 0);
    assert(synchronous_queries == 0);
    opts.android_video_surface_transform = true;
    mp_property_android_video_surface_transform(&player, NULL, M_PROPERTY_GET, &active);
    assert(!active && transform_queries == 0);
    update_android_video_surface_transform(&vo);
    mp_property_android_video_surface_transform(&player, NULL, M_PROPERTY_GET, &active);
    assert(active && transform_queries == 1);
    mp_property_android_surface_frame(&player, NULL, M_PROPERTY_GET, &frame);
    assert(strcmp(frame, "1:1920:1080") == 0);
    free(frame);
    assert(mp_property_android_surface_frame(&player, NULL,
        M_PROPERTY_GET_TYPE, &type) == M_PROPERTY_OK);
    assert(type.type == CONF_TYPE_STRING);
    assert(synchronous_queries == 0);

    test_geometry_resize(&vo);
    // Geometry notifications are redundant; real ownership transitions still notify.
    int state_events = events;
    update_android_video_surface_transform(&vo);
    assert(events == state_events);
    opts.android_video_surface_transform = false;
    update_android_video_surface_transform(&vo);
    assert(events == state_events + 1);
    opts.android_video_surface_transform = true;
    update_android_video_surface_transform(&vo);
    assert(events == state_events + 2);

    // A core-side window change cannot acknowledge the old VO cache's frame.
    requested.WinID = 11;
    mp_property_android_surface_frame(&player, NULL, M_PROPERTY_GET, &frame);
    assert(strcmp(frame, "0:0:0") == 0);
    free(frame);
    requested.WinID = opts.WinID;
    struct mpv_node core_values[4];
    memcpy(core_values, values, sizeof(core_values));
    struct mpv_node_list core_list = {.num = 4, .values = core_values};
    requested.android_surface_frame.u.list = &core_list;
    for (int n = 0; n < 4; n++) {
        core_values[n].u.int64++;
        mp_property_android_surface_frame(&player, NULL, M_PROPERTY_GET, &frame);
        assert(strcmp(frame, "0:0:0") == 0);
        free(frame);
        core_values[n].u.int64--;
    }
    core_list.num = 3;
    mp_property_android_surface_frame(&player, NULL, M_PROPERTY_GET, &frame);
    assert(strcmp(frame, "0:0:0") == 0);
    free(frame);
    assert(synchronous_queries == 0);
    transform_query_supported = false;
    update_android_video_surface_transform(&vo);
    assert(!vo_get_android_video_surface_transform(&vo));
    transform_query_supported = true;
    set_android_video_surface_transform(&vo, false);

    // A new same-size token requires another real submit (including paused redraw).
    request_frame(&vo, 2, 1920, 1080);
    assert(completed(&vo) == 0);
    vo_android_surface_frame_drawn(&vo, 1920, 1080);
    vo_android_surface_frame_presented(&vo, 1920, 1080);
    assert(completed(&vo) == 2);

    // A draw for A cannot acknowledge B, even when their dimensions agree.
    request_frame(&vo, 3, 1440, 1080);
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    request_frame(&vo, 4, 1440, 1080);
    vo_android_surface_frame_presented(&vo, 1440, 1080);
    assert(completed(&vo) == 0);

    // Empty/failed draws clear the candidate, and a failed swap reports no completion.
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    vo_android_surface_frame_drawn(&vo, 0, 0);
    vo_android_surface_frame_presented(&vo, 1440, 1080);
    assert(completed(&vo) == 0);
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    assert(completed(&vo) == 0);
    vo_android_surface_frame_presented(&vo, 2400, 1080);
    assert(completed(&vo) == 0);

    // Replacement and detach reject an in-flight draw and clear completed state.
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    opts.WinID = 11;
    vo_android_set_native_window(&vo, &replacement);
    vo_android_surface_frame_presented(&vo, 1440, 1080);
    assert(completed(&vo) == 0 && window.refs == 0);
    request_frame(&vo, 5, 1440, 1080);
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    vo_android_surface_frame_presented(&vo, 1440, 1080);
    assert(completed(&vo) == 5);
    vo_android_set_native_window(&vo, NULL);
    assert(completed(&vo) == 0 && replacement.refs == 0);

    // Reattaching the same native identity cannot reuse an old completion.
    replacement.refs = 1;
    vo_android_set_native_window(&vo, &replacement);
    request_frame(&vo, 6, 1920, 1080);
    assert(completed(&vo) == 0);
    values[2].u.int64 = (int64_t)INT_MAX + 1;
    vo_android_surface_frame_drawn(&vo, 1920, 1080);
    assert(completed(&vo) == 0);
    values[2].u.int64 = 1920;
    values[3].format = 0;
    vo_android_surface_frame_drawn(&vo, 1920, 1080);
    assert(completed(&vo) == 0);
    assert(events == 11); // Includes four published transform mode changes.

    // Production GPU-next flip: failed submit cannot complete through a later EGL swap.
    values[3].format = MPV_FORMAT_INT64;
    request_frame(&vo, 7, 1920, 1080);
    vo.dwidth = 1920;
    vo.dheight = 1080;
    struct ra_swapchain_fns swap_fns = {.swap_buffers = swap_buffers};
    struct ra_swapchain sw = {.vo = &vo, .fns = &swap_fns};
    struct ra_ctx_fns ctx_fns = {.type = "opengl"};
    struct ra_ctx ctx = {.swapchain = &sw, .fns = &ctx_fns};
    struct priv driver = {.ra_ctx = &ctx, .frame_pending = true, .video_wid = 11};
    vo.priv = &driver;
    vo_android_surface_frame_drawn(&vo, 1920, 1080);
    submit_ok = false;
    gpu_next_flip_page(&vo);
    assert(completed(&vo) == 0 && swaps == 1);
    driver.frame_pending = true;
    vo_android_surface_frame_drawn(&vo, 1920, 1080);
    submit_ok = true;
    gpu_next_flip_page(&vo);
    assert(completed(&vo) == 7 && swaps == 2);

    // Direct paused resize reuses a usable codec submission, without a second release.
    driver.submitted_video = true;
    core.hasframe_rendered = true;
    request_frame(&vo, 8, 1440, 1080);
    direct_control(&vo, VOCTRL_EXTERNAL_RESIZE, NULL);
    assert(completed(&vo) == 8);

    // Core reset invalidates reuse before the driver's deferred RESET is delivered.
    request_frame(&vo, 9, 1920, 1080);
    core.hasframe_rendered = false;
    direct_control(&vo, VOCTRL_EXTERNAL_RESIZE, NULL);
    assert(completed(&vo) == 0);
    assert(driver.submitted_video);
    direct_control(&vo, VOCTRL_RESET, NULL);
    assert(!driver.submitted_video);
    core.hasframe_rendered = true;
    direct_control(&vo, VOCTRL_EXTERNAL_RESIZE, NULL);
    assert(completed(&vo) == 0);
    driver.submitted_video = true; // A successful new buffer release.
    direct_control(&vo, VOCTRL_EXTERNAL_RESIZE, NULL);
    assert(completed(&vo) == 9);

    request_frame(&vo, 10, 1440, 1080);
    direct_reconfig(&vo, NULL);
    direct_control(&vo, VOCTRL_EXTERNAL_RESIZE, NULL);
    assert(completed(&vo) == 0);
    driver.submitted_video = true;
    driver.video_wid = 12;
    direct_control(&vo, VOCTRL_EXTERNAL_RESIZE, NULL);
    assert(completed(&vo) == 0);

    // OSD replacement is independent of a video Surface awaiting replacement.
    assert(direct_control(&vo, VOCTRL_UPDATE_OSD_SURFACE, NULL) == VO_TRUE);
    assert(osd_surface_updates == 1);
    assert(direct_control(&vo, VOCTRL_UPDATE_WINDOW, NULL) == VO_NOTIMPL);
    assert(osd_surface_updates == 1);

    // A geometry snapshot changes only OSD layout, without rebuilding the VO.
    int invalidations = osd_geometry_invalidations;
    vo.want_redraw = false;
    assert(direct_control(&vo, VOCTRL_SET_PANSCAN, NULL) == VO_TRUE);
    assert(osd_geometry_invalidations == invalidations + 1 && vo.want_redraw);
    assert(osd_surface_updates == 1);

    // The production codec release hook, including failure, resumes pending resize.
    driver.video_wid = 11;
    AVMediaCodecBuffer buffer = {0};
    struct mp_image image = {.planes = {[3] = &buffer}};
    driver.next_image = &image;
    driver.osd_render_ok = true;
    driver.submitted_video = false;
    codec_release_ok = false;
    direct_flip_page(&vo);
    assert(completed(&vo) == 0 && !driver.submitted_video);
    assert(codec_releases == 1 && backend_errors == 1);
    driver.next_image = &image;
    codec_release_ok = true;
    direct_flip_page(&vo);
    assert(completed(&vo) == 10 && driver.submitted_video);
    assert(codec_releases == 2);
    request_frame(&vo, 11, 1920, 1080);
    direct_control(&vo, VOCTRL_EXTERNAL_RESIZE, NULL);
    assert(completed(&vo) == 11 && codec_releases == 2);

    // The first EGL swap still presents the old drawable. The next draw uses
    // its resized extent; a failed swap must not complete that draw either.
    request_frame(&vo, 12, 1440, 1080);
    struct android_priv egl = {.egl_surface = 1, .buffer_width = 1920, .buffer_height = 1080};
    ctx.vo = &vo;
    ctx.priv = &egl;
    drawable_w = next_drawable_w = 1920;
    drawable_h = next_drawable_h = 1080;
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    android_swap_buffers(&ctx);
    assert(completed(&vo) == 0 && drawable_w == 1440);
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    egl_swap_ok = false;
    android_swap_buffers(&ctx);
    assert(completed(&vo) == 0);
    egl_swap_ok = true;
    vo_android_surface_frame_drawn(&vo, 1440, 1080);
    android_swap_buffers(&ctx);
    assert(completed(&vo) == 12);
    vo_android_uninit(&vo);
    assert(!vo.android && completed(&vo) == 0);
    puts("Android Surface frame synchronization contracts passed.");
    return 0;
}
