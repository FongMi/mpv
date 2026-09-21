// SPDX-License-Identifier: LGPL-2.1-or-later
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define HAVE_VULKAN 1
#define IMGFMT_MEDIACODEC 1
#define MAPPER_BACKEND_GL 0
#define MAPPER_BACKEND_VULKAN 1
#define RA_HWDEC_MAP_RETRY (-2)
#define AMEDIA_OK 0
#define AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE 1
#define ADATASPACE_UNKNOWN 0
#define EGL_NO_DISPLAY 0
#define EGL_NO_CONTEXT 0
#define EGL_NATIVE_BUFFER_ANDROID 1
#define EGL_NONE 0
#define POLLIN 1
#define POLLERR 2
#define POLLNVAL 4
#define MP_ERR(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)
#define mp_assert assert

typedef int AImage;
typedef int AHardwareBuffer;
typedef int AImageCropRect;
typedef int64_t AVMediaCodecBuffer;
typedef int media_status_t;
typedef int EGLClientBuffer;
typedef int EGLDisplay;
typedef int EGLImageKHR;
typedef struct {
    unsigned width, height;
} AHardwareBuffer_Desc;
struct pollfd {
    int fd, events, revents;
};
struct mp_image {
    int imgfmt;
    void *planes[4];
};
struct texture {
    struct {
        int w, h;
    } params;
};
struct ra_hwdec_mapper;
struct priv_owner {
    int (*AImage_getHardwareBuffer)(AImage *, AHardwareBuffer **);
    int (*AImage_getDataSpace)(AImage *, int32_t *);
    int (*AImage_getCropRect)(AImage *, AImageCropRect *);
    void (*AImage_delete)(AImage *);
    void (*AHardwareBuffer_describe)(AHardwareBuffer *, AHardwareBuffer_Desc *);
};
struct owner {
    struct priv_owner *priv;
};
struct priv {
    int backend;
    void *vk;
    int egl_image;
    struct mp_image *gl_source_frame, *pending_frame;
    int64_t pending_timestamp_ns;
    struct {
        int pending_replacements, unavailable_outputs, render_requests;
        int acquire_errors, acquired_images, mapped_images;
    } stats;
    int (*GetNativeClientBufferANDROID)(AHardwareBuffer *);
    int (*CreateImageKHR)(int, int, int, int, const int *);
    int (*DestroyImageKHR)(int, int);
};
struct ra_hwdec_mapper {
    struct priv *priv;
    struct owner *owner;
    struct mp_image *src;
    struct texture *tex[1];
};
static int release_status, release_calls, acquire_status, acquire_calls;
static int timestamp_status;
static int map_calls, delete_calls;
static struct mp_image *vk_current;
static AImage image;
static AHardwareBuffer buffer;
static struct mp_image *mp_image_new_ref(struct mp_image *p)
{
    return p;
}

static void mp_image_unrefp(struct mp_image **p)
{
    *p = NULL;
}

static void mp_image_setrefp(struct mp_image **p, struct mp_image *v)
{
    *p = v;
}

static int av_mediacodec_get_buffer_timestamp(AVMediaCodecBuffer *p, int64_t *v)
{
    *v = *p;
    return timestamp_status;
}

static int av_mediacodec_release_buffer_status(AVMediaCodecBuffer *p, int render)
{
    assert(p && render);
    release_calls++;
    return release_status;
}

static bool aimagereader_vk_reuse(void *p, struct mp_image *frame)
{
    (void)p;
    return vk_current && vk_current->planes[3] == frame->planes[3];
}

static int acquire_latest_image(struct ra_hwdec_mapper *m, AImage **out, int *fd,
                                int64_t pts)
{
    assert(m->priv->pending_frame == m->src);
    assert(pts == *(AVMediaCodecBuffer *)m->src->planes[3] * INT64_C(1000));
    acquire_calls++;
    *out = acquire_status == AMEDIA_OK ? &image : NULL;
    *fd = -1;
    return acquire_status;
}

static void delete_image(struct priv_owner *o, AImage *p, int fd)
{
    (void)o;
    assert(p == &image && fd == -1);
    delete_calls++;
}

static int poll(struct pollfd *p, int n, int ms)
{
    (void)p;
    (void)n;
    (void)ms;
    assert(false);
    return -1;
}

static int close(int fd)
{
    (void)fd;
    assert(false);
    return 0;
}

static int get_buffer(AImage *p, AHardwareBuffer **b)
{
    assert(p == &image);
    *b = &buffer;
    return 0;
}

static int get_crop(AImage *p, AImageCropRect *c)
{
    assert(p == &image);
    *c = 0;
    return 0;
}

static void delete_gl(AImage *p)
{
    assert(p == &image);
    delete_calls++;
}

static void describe(AHardwareBuffer *b, AHardwareBuffer_Desc *d)
{
    assert(b == &buffer);
    *d = (AHardwareBuffer_Desc){1280, 720};
}

static void log_data_space_mismatch(struct ra_hwdec_mapper *m, int d)
{
    (void)m;
    (void)d;
}

static int aimagereader_vk_map(void *p, AImage *i, AHardwareBuffer *b,
                               const AImageCropRect *c, int d, struct mp_image *f,
                               int *fd)
{
    (void)p;
    (void)c;
    (void)d;
    assert(i == &image && b == &buffer && *fd == -1);
    vk_current = f;
    map_calls++;
    return 0;
}

static int native_buffer(AHardwareBuffer *b)
{
    assert(b == &buffer);
    return 1;
}

static int eglGetCurrentDisplay(void)
{
    return 1;
}

static int create_image(int d, int c, int t, int b, const int *a)
{
    (void)c;
    (void)t;
    (void)a;
    assert(d == 1 && b == 1);
    return 1;
}

static int destroy_image(int d, int i)
{
    assert(d == 1 && i == 1);
    return 1;
}

static bool replace_gl_mapping(struct ra_hwdec_mapper *m, int d, int e, AImage *i,
                               struct mp_image *f)
{
    assert(d == 1 && e == 1 && i == &image);
    m->priv->gl_source_frame = f;
    m->priv->egl_image = e;
    map_calls++;
    return true;
}

#include "aimage_mapper_functions.h"

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "vulkan")));
    for (int backend = argc == 2 ? MAPPER_BACKEND_VULKAN : MAPPER_BACKEND_GL;
         backend <= MAPPER_BACKEND_VULKAN; backend++) {
        struct priv p = {.backend = backend,
                         .GetNativeClientBufferANDROID = native_buffer,
                         .CreateImageKHR = create_image,
                         .DestroyImageKHR = destroy_image};
        struct priv_owner owner = {.AImage_getHardwareBuffer = get_buffer,
                                   .AImage_getCropRect = get_crop,
                                   .AImage_delete = delete_gl,
                                   .AHardwareBuffer_describe = describe};
        struct owner hwdec = {.priv = &owner};
        struct texture tex = {.params = {1280, 720}};
        struct ra_hwdec_mapper mapper = {.priv = &p, .owner = &hwdec, .tex = {&tex}};
        AVMediaCodecBuffer ids[] = {10, 20, 30};
        struct mp_image frames[3] = {{.imgfmt = IMGFMT_MEDIACODEC},
                                     {.imgfmt = IMGFMT_MEDIACODEC},
                                     {.imgfmt = IMGFMT_MEDIACODEC}};
        for (int i = 0; i < 3; i++)
            frames[i].planes[3] = &ids[i];
        vk_current = NULL;
        release_calls = acquire_calls = map_calls = delete_calls = 0;
        release_status = 1;
        acquire_status = AMEDIA_OK;
        mapper.src = &frames[0];
        assert(mapper_map(&mapper) == 0 && map_calls == 1 && !p.pending_frame);
        release_status = 0;
        timestamp_status = -1; // Exact cache reuse needs no new codec query.
        assert(mapper_map(&mapper) == 0 && release_calls == 1 && map_calls == 1);
        timestamp_status = 0;
        mapper.src = &frames[1];
        // A different, unavailable buffer must never acquire A's pixels or retry
        // forever.
        assert(mapper_map(&mapper) == -1);
        assert(map_calls == 1 && acquire_calls == 1 && !p.pending_frame);
        release_status = -1;
        assert(mapper_map(&mapper) == -1 && !p.pending_frame);
        release_status = 1;
        acquire_status = AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
        assert(mapper_map(&mapper) == RA_HWDEC_MAP_RETRY);
        assert(p.pending_frame == &frames[1]);
        int releases = release_calls;
        acquire_status = AMEDIA_OK;
        assert(mapper_map(&mapper) == 0 && release_calls == releases);
        assert(map_calls == 2 && !p.pending_frame);
        mapper.src = &frames[2];
        acquire_status = -3;
        assert(mapper_map(&mapper) == -1 && !p.pending_frame && map_calls == 2);
        // A fresh mapper cannot wait for an unsubmitted buffer.
        struct priv fresh = p;
        fresh.gl_source_frame = NULL;
        fresh.egl_image = 0;
        mapper.priv = &fresh;
        vk_current = NULL;
        release_status = 0;
        releases = release_calls;
        assert(mapper_map(&mapper) == -1);
        assert(release_calls == releases + 1 && !fresh.pending_frame);

        const int64_t times[] = {INT64_MIN / 1000, -1, 0, 1, INT64_MAX / 1000,
                                 INT64_MIN / 1000 - 1, INT64_MAX / 1000 + 1, 10};
        for (int i = 0; i < 8; i++) {
            fresh.gl_source_frame = NULL;
            fresh.egl_image = 0;
            vk_current = NULL;
            ids[2] = times[i];
            timestamp_status = i == 7 ? -1 : 0;
            release_status = 1;
            acquire_status = AMEDIA_OK;
            releases = release_calls;
            int acquired = acquire_calls;
            int result = mapper_map(&mapper);
            assert(result == (i < 5 ? 0 : -1));
            assert(release_calls == releases + (i < 5));
            assert(acquire_calls == acquired + (i < 5));
            assert(!fresh.pending_frame);
        }
        timestamp_status = 0;
        printf("AImageReader backend=%d: identity, fatal unavailable, pending and "
               "errors passed\n",
               backend);
    }
}
