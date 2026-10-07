/* Host fixture for the production Android Surface request/submit functions. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
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
#define MP_NOPTS_VALUE -1
#define MP_ERR(vo, ...) ((void)(vo))
#define MAX_OSD_FAILURES 3
#define EGL_NO_SURFACE 0
#define EGL_WIDTH 1
#define EGL_HEIGHT 2

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
};
struct vo_android_state;
struct vo_internal { int lock; bool hasframe_rendered; };
struct vo {
    struct mp_vo_opts *opts;
    struct vo_android_state *android;
    struct vo_internal *in;
    void *priv;
    int dwidth, dheight;
    bool want_redraw;
};
struct mp_image_params { int unused; };
struct ra_ctx_fns { const char *type; };
struct ra_swapchain;
struct ra_swapchain_fns { void (*swap_buffers)(struct ra_swapchain *); };
struct ra_swapchain { struct vo *vo; const struct ra_swapchain_fns *fns; };
struct ra_ctx {
    struct ra_swapchain *swapchain;
    const struct ra_ctx_fns *fns;
    struct vo *vo;
    void *priv;
};
typedef void *pl_swapchain;
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
};
struct android_priv { int egl_display, egl_surface, buffer_width, buffer_height; };
struct MPOpts { struct mp_vo_opts *vo; };
struct MPContext { struct MPOpts *opts; struct mp_vo_opts *mconfig; };
struct mp_cmd_arg { union { int64_t i64; int i; } v; };
struct mp_cmd_ctx { struct MPContext *mpctx; struct mp_cmd_arg args[3]; bool success; };

static int events;
static bool submit_ok = true;
static int swaps;
static int codec_releases, backend_errors;
static bool codec_release_ok = true, egl_swap_ok = true, egl_size_known = true;
static int drawable_w, drawable_h, next_drawable_w, next_drawable_h;
void vo_android_surface_frame_presented(struct vo *vo, int w, int h);
static void mp_mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mp_mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static pl_swapchain get_active_swapchain(struct priv *p) { return p; }
static bool pl_swapchain_submit_frame(pl_swapchain sw) { (void)sw; return submit_ok; }
static bool android_osd_overlay_present(void *overlay) { (void)overlay; return true; }
static void android_osd_overlay_invalidate_geometry(void *overlay) { (void)overlay; }
static bool android_osd_overlay_set_surface(void *overlay, int64_t wid)
{
    (void)overlay; (void)wid; return true;
}
static bool android_osd_overlay_get_size(void *overlay, int *w, int *h)
{
    (void)overlay; (void)w; (void)h; return false;
}
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
static void vo_event(struct vo *vo, int event) { (void)vo; assert(event == 1); events++; }
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
    vo_android_get_surface_frame(vo, &frame);
    return frame.token;
}

int main(void)
{
    struct mpv_node values[4];
    struct mpv_node_list list = {.num = 4, .values = values};
    struct mp_vo_opts opts = {
        .WinID = 10,
        .android_surface_frame = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &list},
    };
    ANativeWindow window = {.refs = 1}, replacement = {.refs = 1};
    struct vo_android_state android = {0};
    struct vo_internal core = {0};
    struct vo vo = {.opts = &opts, .android = &android, .in = &core};
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
    assert(events == 7);

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
    vo_android_set_native_window(&vo, NULL);
    puts("Android Surface frame synchronization contracts passed.");
    return 0;
}
