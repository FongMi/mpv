// SPDX-License-Identifier: LGPL-2.1-or-later
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define HAVE_VULKAN 1
#define MAPPER_BACKEND_GL 0
#define MAPPER_BACKEND_VULKAN 1
#define AMEDIA_OK 0
#define AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE 1
#define AMEDIA_ERROR_UNKNOWN (-1)
#define MP_TIME_MS_TO_NS(t) ((t) * INT64_C(1000000))
#define MP_ERR(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)

typedef int media_status_t;
typedef struct {
    int64_t timestamp;
    int error, fence;
} AImage;
struct priv {
    int backend;
    struct {
        int acquire_timeouts, stale_images, newer_images;
    } stats;
};
struct priv_owner {
    int reader, image_lock, image_cond;
    bool image_available;
    int (*AImageReader_acquireLatestImageAsync)(int, AImage **, int *);
    int (*AImageReader_acquireLatestImage)(int, AImage **);
    int (*AImage_getTimestamp)(AImage *, int64_t *);
    void (*AImage_deleteAsync)(AImage *, int);
    void (*AImage_delete)(AImage *);
};
struct owner {
    struct priv_owner *priv;
};
struct ra_hwdec_mapper {
    struct priv *priv;
    struct owner *owner;
};
static AImage images[2];
static int count, cursor, deletes, last_fence;
static int64_t clock_ns;
static int64_t mp_time_ns(void)
{
    clock_ns += MP_TIME_MS_TO_NS(200);
    return clock_ns;
}

static void mp_mutex_lock(int *p)
{
    (void)p;
}

static void mp_mutex_unlock(int *p)
{
    (void)p;
}

static void mp_cond_timedwait(int *c, int *m, int64_t t)
{
    (void)c;
    (void)m;
    (void)t;
    assert(false); // The no-image case reaches its existing deadline.
}

static int acquire_async(int reader, AImage **image, int *fence)
{
    (void)reader;
    if (cursor == count)
        return AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
    *image = &images[cursor++];
    *fence = (*image)->fence;
    return AMEDIA_OK;
}

static int acquire(int reader, AImage **image)
{
    int fence;
    return acquire_async(reader, image, &fence);
}

static int timestamp(AImage *image, int64_t *value)
{
    *value = image->timestamp;
    return image->error;
}

static void delete_async(AImage *image, int fence)
{
    assert(image >= images && image < images + count);
    assert(fence == image->fence);
    deletes++;
    last_fence = fence;
}

static void delete_sync(AImage *image)
{
    assert(image >= images && image < images + count);
    deletes++;
    last_fence = -1;
}

#include "aimage_timestamp_functions.h"

int main(void)
{
    const struct {
        int64_t expected, actual;
    } cases[] = {
        {0, 0},
        {-1000, -1000},
        {1000, 1000},
        {1000000, 999999},
        {1000000, 1000001},
        {1000000, 900000},
        {1000000, 1100000},
        {INT64_MAX, INT64_MAX},
        {INT64_MAX, INT64_MAX - 1},
        {INT64_MIN, INT64_MIN},
        {INT64_MIN, INT64_MIN + 1},
    };
    for (int backend = MAPPER_BACKEND_GL; backend <= MAPPER_BACKEND_VULKAN; backend++) {
        struct priv p = {.backend = backend};
        struct priv_owner o = {
            .AImageReader_acquireLatestImageAsync = acquire_async,
            .AImageReader_acquireLatestImage = acquire,
            .AImage_getTimestamp = timestamp,
            .AImage_deleteAsync = delete_async,
            .AImage_delete = delete_sync,
        };
        struct owner owner = {.priv = &o};
        struct ra_hwdec_mapper mapper = {.priv = &p, .owner = &owner};
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]) + 2; i++) {
            bool getter_error = i == sizeof(cases) / sizeof(cases[0]);
            bool empty = i > sizeof(cases) / sizeof(cases[0]);
            int64_t expected = getter_error || empty ? 1000 : cases[i].expected;
            int64_t actual = getter_error || empty ? 1000 : cases[i].actual;
            int fence = backend == MAPPER_BACKEND_VULKAN ? 7 : -1;
            images[0] = (AImage){actual, getter_error ? -2 : 0, fence};
            images[1] = (AImage){expected, 0, fence};
            count = empty ? 0 : actual < expected ? 2 : 1;
            cursor = deletes = 0;
            last_fence = -99;
            clock_ns = 0;
            memset(&p.stats, 0, sizeof(p.stats));
            AImage *result = NULL;
            int result_fence = -1;
            int status =
                acquire_latest_image(&mapper, &result, &result_fence, expected);
            if (empty) {
                assert(status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE);
                assert(!result && result_fence == -1 && !deletes);
                assert(p.stats.acquire_timeouts == 1);
            } else if (getter_error || actual > expected) {
                assert(status == (getter_error ? -2 : AMEDIA_ERROR_UNKNOWN));
                assert(!result && result_fence == -1 && deletes == 1 && cursor == 1);
                assert(last_fence == fence && p.stats.newer_images == !getter_error);
            } else {
                assert(status == AMEDIA_OK && result == &images[count - 1]);
                assert(result_fence == fence && cursor == count);
                assert(deletes == (actual < expected));
                assert(p.stats.stale_images == (actual < expected));
                if (actual < expected)
                    assert(last_fence == fence);
            }
        }
    }
    puts("AImage timestamps: exact int64 identity, stale discard, future failure and "
         "fences passed");
}
