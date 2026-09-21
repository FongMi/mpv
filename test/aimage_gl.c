// SPDX-License-Identifier: LGPL-2.1-or-later
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef int EGLDisplay;
typedef int EGLImageKHR;
typedef int EGLSyncKHR;
typedef int EGLint;
typedef int EGLenum;
typedef int EGLBoolean;
typedef int AImage;
#define EGL_NO_DISPLAY 0
#define EGL_NO_SYNC_KHR 0
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#define EGL_SYNC_NATIVE_FENCE_ANDROID 1
#define EGL_NONE 0
#define EGL_EXTENSIONS 2
#define GL_TEXTURE_EXTERNAL_OES 3
#define MP_ERR(...) ((void)0)

struct mp_image { int refs; };
static char events[128];
static int count, fail_create, fail_dup, fail_destroy, fail_image;
static int missing_symbol, extension_mode, last_fd;
static void event(char c) { assert(count < 127); events[count++] = c; }
static EGLSyncKHR create_sync(EGLDisplay d, EGLenum type, const EGLint *attrs)
{
    assert(d == 1 && type == EGL_SYNC_NATIVE_FENCE_ANDROID && attrs[0] == EGL_NONE);
    event('C');
    return fail_create ? 0 : 7;
}
static EGLBoolean destroy_sync(EGLDisplay d, EGLSyncKHR s)
{
    assert(d == 1 && s == 7);
    event('S');
    return !fail_destroy;
}
static EGLint dup_fd(EGLDisplay d, EGLSyncKHR s)
{
    assert(d == 1 && s == 7);
    event('D');
    return fail_dup ? -1 : 42;
}
static EGLBoolean destroy_image(EGLDisplay d, EGLImageKHR image)
{
    assert(d == 1 && (image == 11 || image == 12));
    event(image == 11 ? 'I' : 'i');
    return !fail_image;
}
static void delete_image(AImage *image)
{
    assert(*image > 0);
    event(*image == 1 ? 'X' : 'x');
    *image = 0;
}
static void delete_async(AImage *image, int fd)
{
    assert((*image == 1 || *image == 2) && fd == 42);
    last_fd = fd;
    event(*image == 1 ? 'A' : 'a');
    *image = 0;
}
static void flush(void) { event('F'); }
static void finish(void) { event('W'); }
static void bind(unsigned int target, unsigned int texture)
{
    assert(target == GL_TEXTURE_EXTERNAL_OES);
    event(texture ? 'B' : 'b');
}
static void image_target(unsigned int target, EGLImageKHR image)
{
    assert(target == GL_TEXTURE_EXTERNAL_OES && image == 12);
    event('T');
}
typedef struct GL {
    void (*Flush)(void);
    void (*Finish)(void);
    void (*BindTexture)(unsigned int, unsigned int);
} GL;
struct priv {
    unsigned int gl_texture;
    AImage *image;
    EGLImageKHR egl_image;
    EGLDisplay egl_image_display;
    struct mp_image *gl_source_frame;
    EGLSyncKHR (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean (*DestroySyncKHR)(EGLDisplay, EGLSyncKHR);
    EGLint (*DupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
    EGLBoolean (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void (*EGLImageTargetTexture2DOES)(unsigned int, EGLImageKHR);
};
struct priv_owner {
    void (*AImage_delete)(AImage *);
    void (*AImage_deleteAsync)(AImage *, int);
};
struct ra_hwdec { struct priv_owner *priv; };
struct ra_hwdec_mapper { struct priv *priv; struct ra_hwdec *owner; GL *ra; };
static GL *ra_gl_get(GL *gl) { return gl; }
static void mp_image_unrefp(struct mp_image **image)
{
    if (*image) {
        assert((*image)->refs == 1);
        (*image)->refs--;
        *image = NULL;
        event('U');
    }
}
static EGLDisplay eglGetCurrentDisplay(void) { return 1; }
static const char *eglQueryString(EGLDisplay display, int name)
{
    assert(display == 1 && name == EGL_EXTENSIONS);
    return extension_mode == 2 ? NULL : extension_mode ? "" :
        "EGL_ANDROID_native_fence_sync";
}
static bool gl_check_extension(const char *extensions, const char *name)
{
    return !strcmp(extensions, name);
}
static void *eglGetProcAddress(const char *name)
{
    if (!strcmp(name, "eglCreateSyncKHR"))
        return missing_symbol == 1 ? NULL : (void *)create_sync;
    if (!strcmp(name, "eglDestroySyncKHR"))
        return missing_symbol == 2 ? NULL : (void *)destroy_sync;
    assert(!strcmp(name, "eglDupNativeFenceFDANDROID"));
    return missing_symbol == 3 ? NULL : (void *)dup_fd;
}
// Production uses EGL's conventional function/data pointer conversion.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4152)
#endif
#include "aimage_gl_functions.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static void expect(const char *expected)
{
    assert(!strcmp(events, expected));
}
int main(void)
{
    GL gl = {flush, finish, bind};
    struct priv_owner owner = {delete_image, delete_async};
    struct ra_hwdec hwdec = {&owner};
    struct priv p = {0};
    struct ra_hwdec_mapper mapper = {&p, &hwdec, &gl};
    for (extension_mode = 0; extension_mode < 3; extension_mode++) {
        for (missing_symbol = 0; missing_symbol < 4; missing_symbol++)
            assert(init_gl_release_fence(&mapper) ==
                   (!extension_mode && !missing_symbol));
    }
    extension_mode = missing_symbol = 0;
    assert(init_gl_release_fence(&mapper));
    assert(release_gl_mapping(&mapper));
    expect("");
    for (int scenario = 0; scenario < 5; scenario++) {
        for (int replace = 0; replace < 2; replace++) {
            AImage old_image = 1, new_image = 2;
            struct mp_image old_frame = {1}, new_frame = {1};
            p.image = &old_image;
            p.egl_image = 11;
            p.egl_image_display = 1;
            p.gl_texture = 5;
            p.gl_source_frame = &old_frame;
            p.DestroyImageKHR = destroy_image;
            p.EGLImageTargetTexture2DOES = image_target;
            count = 0;
            memset(events, 0, sizeof(events));
            last_fd = -1;
            fail_create = scenario == 1;
            fail_dup = scenario == 2;
            fail_destroy = scenario == 3;
            fail_image = scenario == 4;
            bool ok = replace ? replace_gl_mapping(&mapper, 1, 12, &new_image,
                                                   &new_frame) :
                                release_gl_mapping(&mapper);
            assert(ok == (scenario == 0));
            const char *prefix[] = {"CFDSIAU", "CWIXU", "CFDSWIXU",
                                    "CFDSIAU", "CFDSIAU"};
            char expected[128];
            snprintf(expected, sizeof(expected), "%s%s", prefix[scenario],
                     replace ? (ok ? "BTb" : "ixU") : "");
            expect(expected);
            assert(!old_image && !old_frame.refs);
            assert(last_fd == ((fail_create || fail_dup) ? -1 : 42));
            if (replace && ok) {
                assert(p.image == &new_image && p.egl_image == 12);
                assert(p.gl_source_frame == &new_frame && new_frame.refs == 1);
                count = 0;
                memset(events, 0, sizeof(events));
                assert(release_gl_mapping(&mapper));
                expect("CFDSiaU");
                assert(!new_image && !new_frame.refs);
                assert(!p.image && !p.egl_image && !p.gl_source_frame);
            } else {
                assert(!p.image && !p.egl_image && !p.gl_source_frame);
                assert(!p.egl_image_display);
                assert(new_frame.refs == (replace ? 0 : 1));
                assert(new_image == (replace ? 0 : 2));
                int saved_count = count;
                assert(release_gl_mapping(&mapper));
                assert(count == saved_count);
            }
        }
    }
    fail_create = fail_dup = fail_destroy = fail_image = 0;
    count = 0;
    memset(events, 0, sizeof(events));
    AImage first_image = 2;
    struct mp_image first_frame = {1};
    assert(replace_gl_mapping(&mapper, 1, 12, &first_image, &first_frame));
    expect("BTb");
    assert(p.image == &first_image && p.gl_source_frame == &first_frame);
    assert(release_gl_mapping(&mapper));
    expect("BTbCFDSiaU");
    assert(!first_image && !first_frame.refs);
    puts("AImage GL: capability, release order and replacement ownership passed");
    return 0;
}
