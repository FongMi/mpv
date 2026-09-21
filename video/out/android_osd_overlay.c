/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window_jni.h>
#include <libavcodec/jni.h>
#include <string.h>

#include "android_osd_overlay.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/jni.h"
#include "sub/osd.h"
#include "video/mp_image.h"
#include "vo.h"

struct overlay_texture {
    GLuint texture;
    int width;
    int height;
    int change_id;
};

struct android_osd_overlay {
    struct mp_log *log;
    struct vo *vo;
    int64_t wid;
    ANativeWindow *window;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config;
    GLuint program;
    GLint position;
    GLint texcoord;
    GLint sampler;
    struct overlay_texture textures[MAX_OSD_PARTS];
    uint8_t *upload;
    size_t upload_size;
    int width;
    int height;
    int rendered_width;
    int rendered_height;
    int64_t rendered_change_id;
    struct mp_osd_res osd_res;
    bool geometry_dirty;
    bool pending;
};

static const char vertex_shader_source[] =
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 texture_coord;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    texture_coord = texcoord;\n"
    "}\n";

static const char fragment_shader_source[] =
    "precision mediump float;\n"
    "uniform sampler2D texture_sampler;\n"
    "varying vec2 texture_coord;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(texture_sampler, texture_coord).bgra;\n"
    "}\n";

static void destroy_egl(struct android_osd_overlay *ctx);

static bool check_shader(struct android_osd_overlay *ctx, GLuint shader,
                         const char *name)
{
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return true;

    char log[1024] = {0};
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    MP_ERR(ctx, "Failed to compile OSD %s shader: %s\n", name, log);
    return false;
}

static GLuint compile_shader(struct android_osd_overlay *ctx, GLenum type,
                             const char *source, const char *name)
{
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    if (!check_shader(ctx, shader, name)) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool create_program(struct android_osd_overlay *ctx)
{
    GLuint vertex = compile_shader(ctx, GL_VERTEX_SHADER,
                                   vertex_shader_source, "vertex");
    GLuint fragment = compile_shader(ctx, GL_FRAGMENT_SHADER,
                                     fragment_shader_source, "fragment");
    if (!vertex || !fragment) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }

    ctx->program = glCreateProgram();
    if (!ctx->program) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }
    glAttachShader(ctx->program, vertex);
    glAttachShader(ctx->program, fragment);
    glLinkProgram(ctx->program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(ctx->program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {0};
        glGetProgramInfoLog(ctx->program, sizeof(log), NULL, log);
        MP_ERR(ctx, "Failed to link OSD shader: %s\n", log);
        return false;
    }

    ctx->position = glGetAttribLocation(ctx->program, "position");
    ctx->texcoord = glGetAttribLocation(ctx->program, "texcoord");
    ctx->sampler = glGetUniformLocation(ctx->program, "texture_sampler");
    return ctx->position >= 0 && ctx->texcoord >= 0 && ctx->sampler >= 0;
}

static bool update_size(struct android_osd_overlay *ctx)
{
    int width = ctx->vo->opts->android_osd_surface_size.w;
    int height = ctx->vo->opts->android_osd_surface_size.h;
    if (width <= 0 || height <= 0) {
        EGLint egl_width = 0;
        EGLint egl_height = 0;
        if (!eglQuerySurface(ctx->display, ctx->surface, EGL_WIDTH,
                             &egl_width) ||
            !eglQuerySurface(ctx->display, ctx->surface, EGL_HEIGHT,
                             &egl_height) ||
            egl_width <= 0 || egl_height <= 0)
        {
            MP_ERR(ctx, "Failed to query OSD surface size.\n");
            return false;
        }
        width = egl_width;
        height = egl_height;
    }

    if (ctx->width != width || ctx->height != height) {
        ctx->width = width;
        ctx->height = height;
        ctx->geometry_dirty = true;
    }
    return true;
}

static bool create_egl(struct android_osd_overlay *ctx)
{
    JNIEnv *env = MP_JNI_GET_ENV(ctx);
    if (!env) {
        MP_ERR(ctx, "Could not attach Java VM for OSD surface.\n");
        return false;
    }

    jobject surface = (jobject)(intptr_t)ctx->wid;
    ctx->window = ANativeWindow_fromSurface(env, surface);
    if (!ctx->window) {
        MP_ERR(ctx, "Failed to create OSD ANativeWindow.\n");
        return false;
    }

    ctx->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->display == EGL_NO_DISPLAY ||
        !eglInitialize(ctx->display, NULL, NULL))
    {
        MP_ERR(ctx, "Failed to initialize OSD EGL display: 0x%x\n",
               eglGetError());
        goto fail;
    }

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(ctx->display, config_attrs, &ctx->config, 1,
                         &num_configs) ||
        num_configs < 1)
    {
        MP_ERR(ctx, "No RGBA OSD EGL config is available: 0x%x\n",
               eglGetError());
        goto fail;
    }

    EGLint visual_id = 0;
    eglGetConfigAttrib(ctx->display, ctx->config, EGL_NATIVE_VISUAL_ID,
                       &visual_id);
    ANativeWindow_setBuffersGeometry(ctx->window, 0, 0, visual_id);

    const EGLint context_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    ctx->context = eglCreateContext(ctx->display, ctx->config, EGL_NO_CONTEXT,
                                    context_attrs);
    if (ctx->context == EGL_NO_CONTEXT) {
        MP_ERR(ctx, "Failed to create OSD EGL context: 0x%x\n",
               eglGetError());
        goto fail;
    }

    ctx->surface =
        eglCreateWindowSurface(ctx->display, ctx->config,
                               (EGLNativeWindowType)ctx->window, NULL);
    if (ctx->surface == EGL_NO_SURFACE) {
        MP_ERR(ctx, "Failed to create OSD EGL surface: 0x%x\n",
               eglGetError());
        goto fail;
    }
    if (!eglMakeCurrent(ctx->display, ctx->surface, ctx->surface,
                        ctx->context))
    {
        MP_ERR(ctx, "Failed to activate OSD EGL context: 0x%x\n",
               eglGetError());
        goto fail;
    }

    eglSwapInterval(ctx->display, 0);
    if (!create_program(ctx) || !update_size(ctx))
        goto fail;

    glViewport(0, 0, ctx->width, ctx->height);
    glUseProgram(ctx->program);
    glUniform1i(ctx->sampler, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(ctx->display, ctx->surface);
    MP_VERBOSE(ctx, "Initialized %dx%d transparent OSD surface.\n",
               ctx->width, ctx->height);
    return true;

fail:
    destroy_egl(ctx);
    return false;
}

static void clear_surface(struct android_osd_overlay *ctx)
{
    if (!ctx->window || ctx->display == EGL_NO_DISPLAY ||
        ctx->surface == EGL_NO_SURFACE || ctx->context == EGL_NO_CONTEXT)
        return;
    if (!eglMakeCurrent(ctx->display, ctx->surface, ctx->surface,
                        ctx->context))
        return;
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(ctx->display, ctx->surface);
}

static void destroy_egl(struct android_osd_overlay *ctx)
{
    clear_surface(ctx);

    if (ctx->display != EGL_NO_DISPLAY &&
        ctx->context != EGL_NO_CONTEXT &&
        eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context))
    {
        for (int n = 0; n < MAX_OSD_PARTS; n++) {
            if (ctx->textures[n].texture)
                glDeleteTextures(1, &ctx->textures[n].texture);
        }
        if (ctx->program)
            glDeleteProgram(ctx->program);
    }

    memset(ctx->textures, 0, sizeof(ctx->textures));
    ctx->program = 0;
    if (ctx->display != EGL_NO_DISPLAY)
        eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
    if (ctx->display != EGL_NO_DISPLAY && ctx->surface != EGL_NO_SURFACE)
        eglDestroySurface(ctx->display, ctx->surface);
    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT)
        eglDestroyContext(ctx->display, ctx->context);
    if (ctx->window)
        ANativeWindow_release(ctx->window);

    ctx->window = NULL;
    ctx->display = EGL_NO_DISPLAY;
    ctx->context = EGL_NO_CONTEXT;
    ctx->surface = EGL_NO_SURFACE;
    ctx->config = NULL;
    ctx->width = 0;
    ctx->height = 0;
    ctx->rendered_width = 0;
    ctx->rendered_height = 0;
    ctx->rendered_change_id = 0;
    ctx->pending = false;
}

static bool ensure_surface(struct android_osd_overlay *ctx)
{
    if (ctx->window)
        return true;
    return ctx->wid && ctx->wid != -1 && create_egl(ctx);
}

static bool upload_texture(struct android_osd_overlay *ctx,
                           struct overlay_texture *texture,
                           const struct sub_bitmaps *item)
{
    if (!texture->texture)
        glGenTextures(1, &texture->texture);
    if (!texture->texture)
        return false;

    glBindTexture(GL_TEXTURE_2D, texture->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    int row_size = item->packed_w * 4;
    const uint8_t *pixels = item->packed->planes[0];
    if (item->packed->stride[0] != row_size) {
        size_t required = (size_t)row_size * item->packed_h;
        if (required > ctx->upload_size) {
            uint8_t *upload =
                talloc_realloc(ctx, ctx->upload, uint8_t, required);
            if (!upload) {
                MP_ERR(ctx, "Could not allocate OSD upload buffer.\n");
                return false;
            }
            ctx->upload = upload;
            ctx->upload_size = required;
        }
        for (int y = 0; y < item->packed_h; y++) {
            memcpy(ctx->upload + y * row_size,
                   item->packed->planes[0] + y * item->packed->stride[0],
                   row_size);
        }
        pixels = ctx->upload;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, item->packed_w, item->packed_h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR) {
        MP_ERR(ctx, "Failed to upload OSD texture %dx%d.\n",
               item->packed_w, item->packed_h);
        return false;
    }

    texture->width = item->packed_w;
    texture->height = item->packed_h;
    texture->change_id = item->change_id;
    return true;
}

static void draw_part(struct android_osd_overlay *ctx,
                      const struct overlay_texture *texture,
                      const struct sub_bitmap *part)
{
    if (part->w <= 0 || part->h <= 0 || part->dw == 0 || part->dh == 0)
        return;

    float left = 2.0f * part->x / ctx->width - 1.0f;
    float right = 2.0f * (part->x + part->dw) / ctx->width - 1.0f;
    float top = 1.0f - 2.0f * part->y / ctx->height;
    float bottom = 1.0f - 2.0f * (part->y + part->dh) / ctx->height;
    float u0 = (float)part->src_x / texture->width;
    float u1 = (float)(part->src_x + part->w) / texture->width;
    float v0 = (float)part->src_y / texture->height;
    float v1 = (float)(part->src_y + part->h) / texture->height;

    const GLfloat positions[] = {
        left, top,
        left, bottom,
        right, top,
        right, bottom,
    };
    const GLfloat texcoords[] = {
        u0, v0,
        u0, v1,
        u1, v0,
        u1, v1,
    };
    glVertexAttribPointer(ctx->position, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glVertexAttribPointer(ctx->texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

struct android_osd_overlay *android_osd_overlay_create(struct vo *vo)
{
    struct android_osd_overlay *ctx =
        talloc_zero(vo, struct android_osd_overlay);
    if (!ctx)
        return NULL;

    ctx->log = mp_log_new(ctx, vo->log, "osd-overlay");
    ctx->vo = vo;
    ctx->display = EGL_NO_DISPLAY;
    ctx->context = EGL_NO_CONTEXT;
    ctx->surface = EGL_NO_SURFACE;
    ctx->geometry_dirty = true;
    return ctx;
}

bool android_osd_overlay_set_surface(struct android_osd_overlay *ctx,
                                     int64_t wid)
{
    if (ctx->wid == wid && ctx->window)
        return true;
    destroy_egl(ctx);
    ctx->wid = wid;
    ctx->geometry_dirty = true;
    return !wid || wid == -1 || create_egl(ctx);
}

void android_osd_overlay_invalidate_geometry(struct android_osd_overlay *ctx)
{
    ctx->geometry_dirty = true;
}

bool android_osd_overlay_render(struct android_osd_overlay *ctx, double pts)
{
    if (!ctx->wid || ctx->wid == -1)
        return true;
    if (!ensure_surface(ctx))
        return false;
    if (!update_size(ctx)) {
        destroy_egl(ctx);
        return false;
    }

    if (ctx->geometry_dirty) {
        ctx->vo->dwidth = ctx->width;
        ctx->vo->dheight = ctx->height;
        struct mp_rect src, dst;
        vo_get_src_dst_rects(ctx->vo, &src, &dst, &ctx->osd_res);
        ctx->geometry_dirty = false;
    }

    const bool formats[SUBBITMAP_COUNT] = {
        [SUBBITMAP_BGRA] = true,
    };
    struct sub_bitmap_list *overlays =
        osd_render(ctx->vo->osd, ctx->osd_res, pts, 0, formats);
    if (!overlays) {
        MP_ERR(ctx, "Failed to render OSD bitmaps.\n");
        return false;
    }

    int64_t change_id = overlays->change_id;
    bool changed =
        change_id != ctx->rendered_change_id ||
        ctx->width != ctx->rendered_width ||
        ctx->height != ctx->rendered_height;
    if (!changed) {
        talloc_free(overlays);
        return true;
    }
    if (!eglMakeCurrent(ctx->display, ctx->surface, ctx->surface,
                        ctx->context))
    {
        talloc_free(overlays);
        destroy_egl(ctx);
        return false;
    }

    glViewport(0, 0, ctx->width, ctx->height);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(ctx->program);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(ctx->position);
    glEnableVertexAttribArray(ctx->texcoord);

    bool ok = true;
    for (int n = 0; n < overlays->num_items; n++) {
        const struct sub_bitmaps *item = overlays->items[n];
        if (item->format != SUBBITMAP_BGRA || !item->packed ||
            item->render_index < 0 || item->render_index >= MAX_OSD_PARTS ||
            item->packed_w <= 0 || item->packed_h <= 0)
        {
            MP_ERR(ctx, "Invalid packed OSD bitmap.\n");
            ok = false;
            continue;
        }

        struct overlay_texture *texture =
            &ctx->textures[item->render_index];
        if (texture->change_id != item->change_id ||
            texture->width != item->packed_w ||
            texture->height != item->packed_h)
        {
            if (!upload_texture(ctx, texture, item)) {
                ok = false;
                continue;
            }
        } else {
            glBindTexture(GL_TEXTURE_2D, texture->texture);
        }

        for (int i = 0; i < item->num_parts; i++)
            draw_part(ctx, texture, &item->parts[i]);
    }

    glDisableVertexAttribArray(ctx->position);
    glDisableVertexAttribArray(ctx->texcoord);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        MP_ERR(ctx, "Failed to draw OSD overlay: 0x%x\n", error);
        ok = false;
    }
    talloc_free(overlays);
    if (ok) {
        ctx->rendered_width = ctx->width;
        ctx->rendered_height = ctx->height;
        ctx->rendered_change_id = change_id;
    }
    ctx->pending = ok;
    return ok;
}

bool android_osd_overlay_present(struct android_osd_overlay *ctx)
{
    if (!ctx->pending)
        return true;
    if (!eglSwapBuffers(ctx->display, ctx->surface)) {
        MP_ERR(ctx, "Failed to present OSD surface: 0x%x\n",
               eglGetError());
        destroy_egl(ctx);
        return false;
    }
    ctx->pending = false;
    return true;
}

bool android_osd_overlay_get_size(struct android_osd_overlay *ctx,
                                  int *w, int *h)
{
    if (!ensure_surface(ctx) || !update_size(ctx))
        return false;
    *w = ctx->width;
    *h = ctx->height;
    return true;
}

void android_osd_overlay_destroy(struct android_osd_overlay *ctx)
{
    if (!ctx)
        return;
    destroy_egl(ctx);
    talloc_free(ctx);
}
