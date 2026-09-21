/*
 * Android AImageReader / Vulkan backend selection.
 *
 * Prefer direct libplacebo sampling and fall back to conversion when the
 * Android hardware buffer cannot be sampled directly.
 */

#include "config.h"

#include <string.h>

#include <android/data_space.h>

#include "common/common.h"
#include "common/msg.h"
#include "osdep/threads.h"
#include "video/out/gpu/hwdec.h"

#include "hwdec_aimagereader_vk_private.h"

int aimagereader_vk_format_depth(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_G8B8G8R8_422_UNORM:
    case VK_FORMAT_B8G8R8G8_422_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:
        return 8;
    case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
    case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:
        return 10;
    case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
    case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16:
        return 12;
    case VK_FORMAT_G16B16G16R16_422_UNORM:
    case VK_FORMAT_B16G16R16G16_422_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
        return 16;
    default:
        return 0;
    }
}

void aimagereader_vk_apply_data_space(
    VkAndroidHardwareBufferFormatPropertiesANDROID *props,
    int32_t data_space)
{
    bool is_ycbcr =
        aimagereader_vk_format_depth(props->format) ||
        props->suggestedYcbcrModel !=
            VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    if (data_space == ADATASPACE_UNKNOWN || !is_ycbcr)
        return;

    // Override only recognized metadata; preserve the driver's other defaults.
    switch (data_space & ADATASPACE_STANDARD_MASK) {
    case ADATASPACE_STANDARD_BT709:
        props->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        break;
    case ADATASPACE_STANDARD_BT601_625:
    case ADATASPACE_STANDARD_BT601_525:
        props->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
        break;
    case ADATASPACE_STANDARD_BT2020:
        props->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
        break;
    default:
        break;
    }

    switch (data_space & ADATASPACE_RANGE_MASK) {
    case ADATASPACE_RANGE_FULL:
        props->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
        break;
    case ADATASPACE_RANGE_LIMITED:
        props->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        break;
    default:
        break;
    }
}

struct aimagereader_vk {
    struct mp_log *log;
    struct ra_hwdec_mapper *mapper;
    struct aimagereader_vk_api api;
    mp_mutex lock;
    struct aimagereader_vk_direct *direct;
    struct aimagereader_vk_convert *convert;
    enum android_vulkan_aimagereader_backend backend;
    bool direct_mapped;
};

static const char *backend_name(enum android_vulkan_aimagereader_backend backend)
{
    switch (backend) {
    case ANDROID_VULKAN_AIMAGEREADER_BACKEND_DIRECT:
        return "direct";
    case ANDROID_VULKAN_AIMAGEREADER_BACKEND_COMPUTE:
        return "compute";
    case ANDROID_VULKAN_AIMAGEREADER_BACKEND_FRAGMENT:
        return "fragment";
    default:
        return "auto";
    }
}

static void reset_mapper_params(struct aimagereader_vk *p)
{
    p->mapper->dst_params = p->mapper->src_params;
    p->mapper->dst_params.imgfmt = IMGFMT_RGB0;
    p->mapper->dst_params.hw_subfmt = 0;
    p->mapper->dst_params_ready = false;
    p->mapper->dst_params_preserve_repr = false;
    p->mapper->dst_params_map_coordinates = false;
    p->mapper->dst_num_components = 0;
    memset(p->mapper->dst_component_mapping, 0,
           sizeof(p->mapper->dst_component_mapping));
}

static bool switch_to_conversion(struct aimagereader_vk *p)
{
    mp_assert(p->direct && !p->convert && !p->direct_mapped);
    aimagereader_vk_direct_destroy(&p->direct);
    reset_mapper_params(p);
    p->convert = aimagereader_vk_convert_create(p->mapper, &p->api,
                                                p->backend);
    if (!p->convert) {
        mp_err(p->log, "No Vulkan conversion backend for this Android "
                       "hardware buffer\n");
        return false;
    }
    return true;
}

bool aimagereader_vk_available(struct ra_ctx *ra_ctx, struct mp_log *log,
                              enum android_vulkan_aimagereader_backend backend)
{
    if (backend == ANDROID_VULKAN_AIMAGEREADER_BACKEND_DIRECT)
        return aimagereader_vk_direct_available(ra_ctx, log);
    if (backend == ANDROID_VULKAN_AIMAGEREADER_BACKEND_COMPUTE ||
        backend == ANDROID_VULKAN_AIMAGEREADER_BACKEND_FRAGMENT)
        return aimagereader_vk_convert_available(ra_ctx, log);
    return aimagereader_vk_direct_available(ra_ctx, log) ||
           aimagereader_vk_convert_available(ra_ctx, log);
}

struct aimagereader_vk *aimagereader_vk_create(
    struct ra_hwdec_mapper *mapper, const struct aimagereader_vk_api *api,
    enum android_vulkan_aimagereader_backend backend)
{
    struct aimagereader_vk *p = talloc_zero(NULL, struct aimagereader_vk);
    p->log = mapper->log;
    p->mapper = mapper;
    p->api = *api;
    p->backend = backend;
    mp_mutex_init(&p->lock);
    mp_info(p->log, "Vulkan AImageReader backend: %s\n",
            backend_name(backend));

    if (backend == ANDROID_VULKAN_AIMAGEREADER_BACKEND_AUTO ||
        backend == ANDROID_VULKAN_AIMAGEREADER_BACKEND_DIRECT) {
        p->direct = aimagereader_vk_direct_create(mapper, api);
        if (p->direct)
            return p;
        if (backend == ANDROID_VULKAN_AIMAGEREADER_BACKEND_DIRECT)
            goto error;
    }

    reset_mapper_params(p);
    p->convert = aimagereader_vk_convert_create(mapper, api, backend);
    if (p->convert)
        return p;

error:
    mp_mutex_destroy(&p->lock);
    talloc_free(p);
    return NULL;
}

void aimagereader_vk_destroy(struct aimagereader_vk **state)
{
    struct aimagereader_vk *p = *state;
    if (!p)
        return;

    mp_mutex_lock(&p->lock);
    aimagereader_vk_direct_destroy(&p->direct);
    aimagereader_vk_convert_destroy(&p->convert);
    mp_mutex_unlock(&p->lock);
    mp_mutex_destroy(&p->lock);
    talloc_free(p);
    *state = NULL;
}

void aimagereader_vk_buffer_removed(struct aimagereader_vk *p,
                                    AHardwareBuffer *buffer)
{
    mp_mutex_lock(&p->lock);
    if (p->direct)
        aimagereader_vk_direct_buffer_removed(p->direct, buffer);
    if (p->convert)
        aimagereader_vk_convert_buffer_removed(p->convert, buffer);
    mp_mutex_unlock(&p->lock);
}

bool aimagereader_vk_reuse(struct aimagereader_vk *p,
                           struct mp_image *frame)
{
    mp_mutex_lock(&p->lock);
    bool reused = p->convert
        ? aimagereader_vk_convert_reuse(p->convert, frame)
        : p->direct && aimagereader_vk_direct_reuse(p->direct, frame);
    mp_mutex_unlock(&p->lock);
    return reused;
}

bool aimagereader_vk_retain_last(struct aimagereader_vk *p,
                                struct mp_image *frame)
{
    mp_mutex_lock(&p->lock);
    bool retained = p->convert
        ? aimagereader_vk_convert_retain_last(p->convert, frame)
        : p->direct &&
          aimagereader_vk_direct_retain_last(p->direct, frame);
    mp_mutex_unlock(&p->lock);
    return retained;
}

int aimagereader_vk_map(struct aimagereader_vk *p, AImage *image,
                        AHardwareBuffer *buffer,
                        const AImageCropRect *crop, int32_t data_space,
                        struct mp_image *frame, int *acquire_fence)
{
    mp_mutex_lock(&p->lock);
    int result;
    if (p->convert) {
        result = aimagereader_vk_convert_map(
            p->convert, image, buffer, crop, data_space, frame, acquire_fence);
        goto done;
    }
    if (!p->direct) {
        result = -1;
        goto done;
    }

    result = aimagereader_vk_direct_map(
        p->direct, image, buffer, crop, data_space, frame, acquire_fence);
    if (result == 0) {
        p->direct_mapped = true;
        goto done;
    }
    if (result != AIMAGEREADER_VK_MAP_UNSUPPORTED)
        goto done;
    if (p->backend == ANDROID_VULKAN_AIMAGEREADER_BACKEND_DIRECT) {
        mp_err(p->log, "Forced direct Vulkan AImageReader backend does not "
                       "support this Android hardware buffer\n");
        result = -1;
        goto done;
    }
    if (p->direct_mapped) {
        mp_err(p->log, "Android hardware-buffer format changed after the "
                       "direct Vulkan mapper was configured\n");
        result = -1;
        goto done;
    }

    if (!switch_to_conversion(p)) {
        result = -1;
        goto done;
    }
    result = aimagereader_vk_convert_map(
        p->convert, image, buffer, crop, data_space, frame, acquire_fence);

done:
    mp_mutex_unlock(&p->lock);
    return result;
}
