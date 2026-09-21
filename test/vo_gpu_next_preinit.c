// Inject initialization failures into the production VO with tracked GPU owners.
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#define HAVE_ANDROID 1
enum stage { SUCCESS, RA, GPU, SWAPCHAIN, OVERLAY, SURFACE };
static enum stage fail_at;
static int ra, swapchain, overlay, renderer;
static bool owns_ra, owns_gpu, owns_overlay;
static struct ra_ctx_opts { bool want_alpha; } opts;
static struct gpu_ctx { void *swapchain; } gpu = {&swapchain};
struct priv {
    void *ra_ctx, *log, *sw, *osd_overlay, *renderer;
    struct gpu_ctx *context;
};
struct vo {
    struct priv *priv;
    void *log, *hwdec_devs;
};

#define mp_get_config_group(...) (&opts)
#define mppl_video_opts_want_alpha(...) false
#define talloc_free(ptr) ((void)(ptr))
#define ra_ctx_create(...) ((owns_ra = fail_at != RA) ? &ra : NULL)
#define gpu_ctx_create(...) ((owns_gpu = fail_at != GPU) ? &gpu : NULL)
#define gpu_ctx_create_swapchain(...) (fail_at != SWAPCHAIN)
#define android_osd_overlay_create(...) ((owns_overlay = fail_at != OVERLAY) ? &overlay : NULL)
#define android_osd_overlay_set_surface(...) (fail_at != SURFACE)
#define mppl_video_init(...) (&renderer)
#define mppl_video_set_osd_source(...) ((void)0)
#define mppl_video_configure_queue(...) ((void)0)
#define hwdec_devices_create() (&renderer)
#define hwdec_devices_set_loader(...) ((void)0)
#define mppl_video_init_hwdecs(...) ((void)0)

static void android_osd_overlay_destroy(void *ptr)
{
    if (ptr) {
        assert(ptr == &overlay && owns_overlay && owns_gpu && owns_ra);
        owns_overlay = false;
    }
}

static void gpu_ctx_destroy(struct gpu_ctx **ptr)
{
    if (*ptr) {
        assert(*ptr == &gpu && !owns_overlay && owns_gpu && owns_ra);
        owns_gpu = false;
        *ptr = NULL;
    }
}

static void ra_ctx_destroy(void **ptr)
{
    if (*ptr) {
        assert(*ptr == &ra && !owns_overlay && !owns_gpu && owns_ra);
        owns_ra = false;
        *ptr = NULL;
    }
}

#include "vo_gpu_next_preinit.h"

int main(void)
{
    for (fail_at = RA; fail_at <= SURFACE; fail_at++) {
        struct priv p = {0};
        struct vo vo = {.priv = &p};
        assert(preinit(&vo) < 0);
        assert(!p.ra_ctx && !p.context && !p.sw && !p.osd_overlay);
        assert(!owns_ra && !owns_gpu && !owns_overlay && !vo.hwdec_devs);
    }
    fail_at = SUCCESS;
    struct priv p = {0};
    struct vo vo = {.priv = &p};
    assert(preinit(&vo) == 0);
    assert(owns_ra && owns_gpu && owns_overlay);
    assert(p.sw == &swapchain && p.renderer == &renderer && vo.hwdec_devs);
    return 0;
}
