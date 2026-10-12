/* Standalone fixture for the production DRM color and atomic commit state.
 * The DRM and color helpers are mocked; no display device is required.
 */
#include <stdbool.h>
#include <stdio.h>

#define DRM_MODE_ATOMIC_NONBLOCK 1
#define DRM_MODE_PAGE_FLIP_EVENT 2
#define DRM_MODE_ATOMIC_ALLOW_MODESET 4
#define MP_WARN(...) ((void)0)
#define CHECK(value) do {                                                \
    if (!(value)) {                                                     \
        fprintf(stderr, "DRM modeset check failed at line %d: %s\n",     \
                __LINE__, #value);                                      \
        return 1;                                                       \
    }                                                                   \
    checks++;                                                           \
} while (0)

struct pl_color_space { int id; };
static const struct pl_color_space pl_color_space_srgb = {.id = 1};
struct mp_image_params { struct pl_color_space color; };
struct framebuffer { int id; };
struct drm_object { int id; };
struct drm_atomic_context {
    void *request;
    struct drm_object *draw_plane, *crtc;
};
struct vo_drm_state {
    int fd;
    bool waiting_for_flip;
    struct framebuffer *fb;
    struct drm_atomic_context *atomic_context;
};
struct vo { struct vo_drm_state *drm; int color_id; };
struct priv { bool allow_modeset; };
struct ra_ctx { struct vo *vo; struct priv *priv; };
struct gbm_frame { void *bo; };

static int submitted_flags, commit_result, frees, allocs, checks;

static void update_framebuffer_from_bo(struct ra_ctx *ctx, void *bo)
{
    (void)ctx;
    (void)bo;
}

static int drm_object_set_property(void *request, struct drm_object *object,
                                   const char *name, int value)
{
    (void)request;
    (void)object;
    (void)name;
    (void)value;
    return 0;
}

static int drmModeAtomicCommit(int fd, void *request, int flags, void *data)
{
    (void)fd;
    (void)request;
    (void)data;
    submitted_flags = flags;
    return commit_result;
}

static void drmModeAtomicFree(void *request)
{
    (void)request;
    frees++;
}

static void *drmModeAtomicAlloc(void)
{
    allocs++;
    return &allocs;
}

static bool vo_drm_set_color(struct vo *vo, struct pl_color_space *color)
{
    bool changed = vo->color_id != color->id;
    vo->color_id = color->id;
    return changed;
}

#include "drm_modeset_functions.h"

int main(void)
{
    struct drm_object plane = {.id = 1}, crtc = {.id = 2};
    struct framebuffer fb = {.id = 3};
    struct drm_atomic_context atomic = {
        .request = &allocs, .draw_plane = &plane, .crtc = &crtc,
    };
    struct vo_drm_state drm = {.fb = &fb, .atomic_context = &atomic};
    struct vo vo = {.drm = &drm, .color_id = 2};
    struct priv p = {0};
    struct ra_ctx ctx = {.vo = &vo, .priv = &p};
    struct gbm_frame frame = {0};
    struct mp_image_params hdr = {.color = {.id = 2}};
    const int results[] = {0, -22, 13};

    CHECK(drm_egl_set_color(&ctx, NULL));
    CHECK(vo.color_id == 1 && p.allow_modeset);
    CHECK(drm_egl_set_color(&ctx, NULL));
    CHECK(p.allow_modeset);
    CHECK(drm_egl_set_color(&ctx, &hdr));
    CHECK(vo.color_id == 2 && p.allow_modeset);
    CHECK(drm_egl_set_color(&ctx, &hdr));
    CHECK(p.allow_modeset);

    commit_result = -22;
    queue_flip(&ctx, &frame);
    CHECK(submitted_flags & DRM_MODE_ATOMIC_ALLOW_MODESET);
    CHECK(!drm.waiting_for_flip && p.allow_modeset);
    CHECK(drm_egl_set_color(&ctx, &hdr));
    CHECK(p.allow_modeset);
    commit_result = 0;
    queue_flip(&ctx, &frame);
    CHECK(submitted_flags & DRM_MODE_ATOMIC_ALLOW_MODESET);
    CHECK(drm.waiting_for_flip && !p.allow_modeset);
    CHECK(drm_egl_set_color(&ctx, &hdr));
    CHECK(!p.allow_modeset);

    for (int pending = 0; pending < 2; pending++) {
        for (int result = 0; result < 3; result++) {
            p.allow_modeset = pending != 0;
            commit_result = results[result];
            queue_flip(&ctx, &frame);
            CHECK(!!(submitted_flags & DRM_MODE_ATOMIC_ALLOW_MODESET) ==
                  (pending != 0));
            CHECK(drm.waiting_for_flip == (commit_result == 0));
            CHECK(p.allow_modeset == (pending && commit_result != 0));
            CHECK(atomic.request == &allocs);
        }
    }
    CHECK(frees == 8 && allocs == 8);
    printf("DRM modeset: %d checks passed\n", checks);
    return 0;
}
