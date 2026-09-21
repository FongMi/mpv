// Device harness: compile the actual mpv Android EGL implementation. Only VO
// ownership, log delivery and framebuffer metadata are replaced by test adapters.
#include <jni.h>
#include <stdbool.h>
#include <android/log.h>
#include <EGL/egl.h>

static int surface_creations;
static int requested_swap_interval = -1;
static int swap_calls;
static EGLBoolean last_swap_result;
static EGLBoolean counted_swap_buffers(EGLDisplay display, EGLSurface surface)
{
    swap_calls++;
    last_swap_result = eglSwapBuffers(display, surface);
    return last_swap_result;
}
static EGLSurface counted_create_surface(EGLDisplay display, EGLConfig config,
                                         EGLNativeWindowType window,
                                         const EGLint *attributes)
{
    surface_creations++;
    return eglCreateWindowSurface(display, config, window, attributes);
}

static EGLBoolean counted_swap_interval(EGLDisplay display, EGLint interval)
{
    requested_swap_interval = interval;
    return eglSwapInterval(display, interval);
}

#define eglCreateWindowSurface counted_create_surface
#define eglSwapInterval counted_swap_interval
#define eglSwapBuffers counted_swap_buffers
#include MPV_EGL_CONTEXT_SOURCE
#undef eglCreateWindowSurface
#undef eglSwapInterval
#undef eglSwapBuffers

struct vo_android_state {
    ANativeWindow *window;
    JNIEnv *env;
};

struct probe {
    struct priv egl;
    struct vo vo;
    struct mp_vo_opts opts;
    struct vo_android_state android;
    struct ra_ctx ctx;
    struct ra_swapchain swapchain;
};

void mp_msg(struct mp_log *log, int level, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "MpvEglSourceProbe", format, args);
    va_end(args);
}

ANativeWindow *vo_android_native_window(struct vo *vo)
{
    return vo->android->window;
}

bool vo_android_has_native_window(struct vo *vo)
{
    return vo_android_native_window(vo) != NULL;
}

ANativeWindow *vo_android_create_native_window(struct vo *vo)
{
    return ANativeWindow_fromSurface(vo->android->env,
                                    (jobject)(intptr_t)vo->opts->WinID);
}

void vo_android_set_native_window(struct vo *vo, ANativeWindow *window)
{
    if (vo->android->window)
        ANativeWindow_release(vo->android->window);
    vo->android->window = window;
}

void ra_swapchain_invalidate_color(struct ra_swapchain *swapchain)
{
    swapchain->color_hint_valid = false;
}

bool vo_android_surface_size(struct vo *vo, int *width, int *height)
{
    *width = vo->opts->android_surface_size.w;
    *height = vo->opts->android_surface_size.h;
    return *width > 0 && *height > 0;
}

void ra_gl_ctx_resize(struct ra_swapchain *swapchain, int width, int height, int fbo)
{
    // The real implementation wraps FBO 0 with these dimensions. The Java
    // harness renders its red/blue fixture using the same VO dimensions.
    assert(width == swapchain->ctx->vo->dwidth);
    assert(height == swapchain->ctx->vo->dheight);
    assert(fbo == 0);
}

void ra_gl_ctx_uninit(struct ra_ctx *ctx)
{
    // RA resources are owned by the Java fixture. EGL teardown remains real.
}

void vo_android_uninit(struct vo *vo)
{
    vo_android_set_native_window(vo, NULL);
}

int ra_gl_ctx_get_swap_interval(struct ra_swapchain *swapchain)
{
    return 0; // A real window replacement must preserve this non-default option.
}

static jintArray probe_state(JNIEnv *env, struct probe *probe, int result, int events)
{
    EGLint width = 0, height = 0;
    assert(eglQuerySurface(probe->egl.egl_display, probe->egl.egl_surface, EGL_WIDTH, &width));
    assert(eglQuerySurface(probe->egl.egl_display, probe->egl.egl_surface, EGL_HEIGHT, &height));
    jint values[] = {result, width, height, probe->vo.dwidth, probe->vo.dheight,
                     surface_creations, !!(events & VO_EVENT_RESIZE),
                     ANativeWindow_getBuffersDataSpace(probe->android.window),
                     requested_swap_interval};
    jintArray array = (*env)->NewIntArray(env, 9);
    if (array)
        (*env)->SetIntArrayRegion(env, array, 0, 9, values);
    return array;
}

JNIEXPORT jlong JNICALL
Java_org_mpv_egl_NativeProbe_createProbe(
    JNIEnv *env, jclass cls, jobject surface)
{
    struct probe *probe = calloc(1, sizeof(*probe));
    assert(probe);
    struct priv *p = &probe->egl;
    p->egl_display = eglGetCurrentDisplay();
    p->egl_context = eglGetCurrentContext();
    p->egl_surface = eglGetCurrentSurface(EGL_DRAW);
    p->egl_colorspace = EGL_NONE;
    p->rejected_egl_colorspace = -1;
    p->rejected_dataspace = -1;
    p->set_buffers_dataspace = (set_buffers_dataspace_fn)
        dlsym(RTLD_DEFAULT, "ANativeWindow_setBuffersDataSpace");
    EGLint config_id, count;
    assert(eglQueryContext(p->egl_display, p->egl_context, EGL_CONFIG_ID, &config_id));
    EGLint attributes[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
    assert(eglChooseConfig(p->egl_display, attributes, &p->egl_config, 1, &count));
    assert(count == 1);
    assert(eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_NATIVE_VISUAL_ID,
                             &p->egl_native_visual_id));
    probe->android.window = ANativeWindow_fromSurface(env, surface);
    assert(probe->android.window);
    probe->android.env = env;
    probe->opts.WinID = (intptr_t)(*env)->NewGlobalRef(env, surface);
    assert(probe->opts.WinID);
    probe->vo.android = &probe->android;
    probe->vo.opts = &probe->opts;
    probe->ctx.vo = &probe->vo;
    probe->ctx.priv = p;
    probe->ctx.swapchain = &probe->swapchain;
    probe->swapchain.ctx = &probe->ctx;
    surface_creations = 0;
    requested_swap_interval = -1;
    swap_calls = 0;
    last_swap_result = EGL_FALSE;
    return (jlong)(intptr_t)probe;
}

JNIEXPORT jintArray JNICALL
Java_org_mpv_egl_NativeProbe_checkProbeState(
    JNIEnv *env, jclass cls, jlong handle, jboolean redraw)
{
    struct probe *probe = (void *)(intptr_t)handle;
    int events = 0;
    int request = redraw ? VOCTRL_REDRAW : VOCTRL_CHECK_EVENTS;
    int result = android_control(&probe->ctx, &events, request, NULL);
    return probe_state(env, probe, result, events);
}

JNIEXPORT jboolean JNICALL
Java_org_mpv_egl_NativeProbe_checkPendingColorResize(
    JNIEnv *env, jclass cls, jlong handle)
{
    struct probe *probe = (void *)(intptr_t)handle;
    android_set_egl_colorspace(&probe->ctx, EGL_GL_COLORSPACE_SRGB_KHR);
    EGLint width = 0, height = 0;
    assert(eglQuerySurface(probe->egl.egl_display, probe->egl.egl_surface, EGL_WIDTH, &width));
    assert(eglQuerySurface(probe->egl.egl_display, probe->egl.egl_surface, EGL_HEIGHT, &height));
    return width == probe->vo.dwidth && height == probe->vo.dheight;
}

JNIEXPORT jboolean JNICALL
Java_org_mpv_egl_NativeProbe_swapProbe(
    JNIEnv *env, jclass cls, jlong handle)
{
    struct probe *probe = (void *)(intptr_t)handle;
    int before = swap_calls;
    android_swap_buffers(&probe->ctx);
    return swap_calls == before + 1 && last_swap_result == EGL_TRUE;
}

JNIEXPORT jintArray JNICALL
Java_org_mpv_egl_NativeProbe_reconfigureProbe(
    JNIEnv *env, jclass cls, jlong handle, jint width, jint height)
{
    struct probe *probe = (void *)(intptr_t)handle;
    probe->opts.android_surface_size.w = width;
    probe->opts.android_surface_size.h = height;
    requested_swap_interval = -1;
    int success = android_reconfig(&probe->ctx);
    return probe_state(env, probe, success, 0);
}

JNIEXPORT jint JNICALL
Java_org_mpv_egl_NativeProbe_setProbeDataspace(
    JNIEnv *env, jclass cls, jlong handle, jint dataspace)
{
    struct probe *probe = (void *)(intptr_t)handle;
    struct android_color color = {EGL_NONE, dataspace};
    return android_apply_color(&probe->ctx, probe->android.window, &color);
}

JNIEXPORT void JNICALL
Java_org_mpv_egl_NativeProbe_cycleProbeWindow(
    JNIEnv *env, jclass cls, jlong handle)
{
    struct probe *probe = (void *)(intptr_t)handle;
    EGLContext context = probe->egl.egl_context;
    int64_t window_id = probe->opts.WinID;
    probe->opts.WinID = 0;
    assert(android_update_window(&probe->ctx));
    assert(!android_check_visible(&probe->ctx));
    assert(android_reconfig(&probe->ctx));
    probe->opts.WinID = window_id;
    assert(android_update_window(&probe->ctx));
    assert(android_check_visible(&probe->ctx));
    assert(eglGetCurrentContext() == context);
}

JNIEXPORT void JNICALL
Java_org_mpv_egl_NativeProbe_detachProbeWindow(
    JNIEnv *env, jclass cls, jlong handle)
{
    struct probe *probe = (void *)(intptr_t)handle;
    assert(android_detach_window(&probe->ctx));
}

JNIEXPORT void JNICALL
Java_org_mpv_egl_NativeProbe_uninitProbe(
    JNIEnv *env, jclass cls, jlong handle)
{
    struct probe *probe = (void *)(intptr_t)handle;
    android_uninit(&probe->ctx);
}

JNIEXPORT void JNICALL
Java_org_mpv_egl_NativeProbe_replaceProbeWindow(
    JNIEnv *env, jclass cls, jlong handle, jobject surface)
{
    struct probe *probe = (void *)(intptr_t)handle;
    jobject previous = (jobject)(intptr_t)probe->opts.WinID;
    probe->opts.WinID = (intptr_t)(*env)->NewGlobalRef(env, surface);
    assert(probe->opts.WinID);
    assert(android_update_window(&probe->ctx));
    (*env)->DeleteGlobalRef(env, previous);
}

JNIEXPORT void JNICALL
Java_org_mpv_egl_NativeProbe_destroyProbe(
    JNIEnv *env, jclass cls, jlong handle)
{
    struct probe *probe = (void *)(intptr_t)handle;
    if (probe->android.window)
        ANativeWindow_release(probe->android.window);
    (*env)->DeleteGlobalRef(env, (jobject)(intptr_t)probe->opts.WinID);
    free(probe);
}

JNIEXPORT jint JNICALL
Java_org_mpv_egl_NativeProbe_setGeometry(
    JNIEnv *env, jclass cls, jobject surface, jint width, jint height)
{
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window)
        return -1;
    int format = ANativeWindow_getFormat(window);
    int result = ANativeWindow_setBuffersGeometry(window, width, height, format);
    ANativeWindow_release(window);
    return result;
}
