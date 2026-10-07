#!/usr/bin/env python3
"""Compile Android OSD context/lifecycle contracts from the production helpers.

The resulting executable is standalone; use an Android cross compiler and run
it on a device, or use a host C compiler. No real GPU is required by this test.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

from vo_android_frame import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflag", action="append", default=[])
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--output", type=Path)
    output.add_argument("--generate", type=Path)
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] /
              "video/out/android_osd_overlay.c").read_text(encoding="utf-8")
    state_start = source.index("struct egl_state {")
    state_end = source.index("};", state_start) + 2
    helpers = source[state_start:state_end] + "\n"
    for name in ("save_egl", "restore_egl", "android_osd_geometry_from_node",
                 "android_osd_overlay_transforms_video",
                 "android_osd_overlay_active",
                 "android_osd_overlay_get_video_rects",
                 "update_geometry", "draw_part",
                 "android_osd_overlay_render"):
        helpers += function(source, name)
    command = (Path(__file__).resolve().parents[1] /
               "player/command.c").read_text(encoding="utf-8")
    helpers += function(command, "cmd_android_video_geometry")
    helpers += function(command, "cmd_android_video_geometry_clear")
    assert '{ "android-video-geometry-clear", cmd_android_video_geometry_clear }' in command
    assert '{"subtitle-aspect", OPT_DOUBLE(v.d), OPTDEF_DOUBLE(0)}' in command
    osd = (Path(__file__).resolve().parents[1] / "sub/osd.c").read_text(encoding="utf-8")
    helpers += function(osd, "osd_get_video_par")
    helpers += function(osd, "osd_res_equals")
    ass = (Path(__file__).resolve().parents[1] / "sub/sd_ass.c").read_text(encoding="utf-8")
    start = ass.index("    double scale = dim.display_par;")
    ass_scale = ass[start:ass.index("    long long ts =", start)]
    lavc = (Path(__file__).resolve().parents[1] / "sub/sd_lavc.c").read_text(encoding="utf-8")
    start = lavc.index("    double video_par = 0;")
    bitmap_par = lavc[start:lavc.index("    if (opts->stretch_image_subs)", start)]
    aspect = (Path(__file__).resolve().parents[1] /
              "video/out/aspect.c").read_text(encoding="utf-8")
    for name in ("aspect_calc_panscan", "clamp_size", "src_dst_split_scaling"):
        helpers += function(aspect, name)
    fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#define MP_ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MPV_FORMAT_NONE 0
#define MPV_FORMAT_DOUBLE 5
#define MPV_FORMAT_INT64 4
#define MPV_FORMAT_NODE_ARRAY 7
#define bstr0(x) (x)
struct mpv_node_list;
struct mpv_node {
    union { int64_t int64; double double_; struct mpv_node_list *list; } u;
    int format;
};
struct mpv_node_list { int num; struct mpv_node *values; };
struct mp_osd_res { int w, h, ml, mt, mr, mb; double display_par, video_aspect; };
struct mp_image_params { int w, h, p_w, p_h, rotate; };
static struct mpv_node committed_values[7];
static struct mpv_node_list committed_list = {7, committed_values};
static struct mpv_node committed;
static int commits;
static bool reject_commit;
struct MPContext { void *mconfig; };
struct mp_cmd_ctx {
    struct MPContext *mpctx;
    struct { union { int i; double d; } v; } args[7];
    bool success;
};
static int m_config_set_option_node(void *config, const char *name,
                                    struct mpv_node *request, int flags) {
    (void)config; (void)flags;
    assert(!strcmp(name, "android-video-geometry"));
    if (reject_commit)
        return -1;
    if (request->format == MPV_FORMAT_NONE) {
        committed = *request;
        commits++;
        return 0;
    }
    committed_list.num = request->u.list->num;
    memcpy(committed_values, request->u.list->values, sizeof(committed_values));
    committed = (struct mpv_node){.format = MPV_FORMAT_NODE_ARRAY,
                                 .u.list = &committed_list};
    commits++;
    return 0;
}
typedef int EGLDisplay, EGLContext, EGLSurface, EGLenum;
#define EGL_NO_DISPLAY 0
#define EGL_DRAW 1
#define EGL_READ 2
#define MP_ERR(ctx, ...) ((void)(ctx))
static int display, context, draw, read_surface, api;
static bool fail_restore, render_ok = true;
static int eglGetCurrentDisplay(void) { return display; }
static int eglGetCurrentContext(void) { return context; }
static int eglGetCurrentSurface(int kind) {
    return kind == EGL_DRAW ? draw : read_surface;
}
static int eglQueryAPI(void) { return api; }
static bool eglBindAPI(int value) { api = value; return true; }
static bool eglMakeCurrent(int d, int w, int r, int c) {
    if (c == 20) {
        assert(api == 7); // Bind the saved API before restoring its context.
        if (fail_restore)
            return false;
    }
    display = c ? d : 0;
    context = c;
    draw = w;
    read_surface = r;
    return true;
}
struct mp_vo_opts {
    float scale_x, scale_y, pan_x, pan_y;
    bool android_video_surface_transform;
    float panscan;
    bool keepaspect;
    struct mpv_node android_video_geometry;
};
struct mp_rect { int unused; };
struct driver { int caps; };
struct vo {
    struct mp_vo_opts *opts;
    struct driver *driver;
    void *params, *log;
    int dwidth, dheight;
    double monitor_par;
};
struct android_osd_overlay {
    struct vo *vo;
    bool window, geometry_dirty;
    int width, height, position, texcoord;
    struct mp_osd_res osd_res;
};
struct overlay_texture { int width, height; };
struct sub_bitmap { int x, y, w, h, dw, dh, src_x, src_y; };
typedef float GLfloat;
#define GL_FLOAT 0
#define GL_FALSE 0
#define GL_TRIANGLE_STRIP 0
static float drawn_positions[8];
static void glVertexAttribPointer(int index, int size, int type, int normalized,
                                  int stride, const float *values) {
    (void)size; (void)type; (void)normalized; (void)stride;
    if (!index)
        memcpy(drawn_positions, values, sizeof(drawn_positions));
}
static void glDrawArrays(int mode, int first, int count) {
    (void)mode; (void)first; (void)count;
}
static struct mp_vo_opts observed;
static int observed_w, observed_h, fallback_calls;
static void vo_get_src_dst_rects(struct vo *vo, struct mp_rect *src,
                               struct mp_rect *dst, struct mp_osd_res *osd) {
    (void)src; (void)dst; (void)osd;
    observed = *vo->opts;
    fallback_calls++;
}
static void mp_get_src_dst_rects(void *log, struct mp_vo_opts *opts,
    int caps, void *params, int w, int h, double par,
    struct mp_rect *src, struct mp_rect *dst, struct mp_osd_res *osd) {
    (void)log; (void)caps; (void)params; (void)par;
    (void)src; (void)dst; (void)osd;
    observed = *opts;
    observed_w = w;
    observed_h = h;
}
static bool render(struct android_osd_overlay *ctx, double pts) {
    (void)ctx; (void)pts;
    display = 1; context = 2; draw = 3; read_surface = 4; api = 5;
    return render_ok;
}
static void destroy_egl(struct android_osd_overlay *ctx) {
    ctx->window = false;
    display = context = draw = read_surface = 0;
}
'''
    fixture += helpers
    fixture += r'''
static double ass_video_scale(struct mp_osd_res dim, struct mp_image_params params,
    bool converted, int override, int video_data, double explicit_aspect) {
    struct { struct mp_image_params video_params; } context = {params}, *ctx = &context;
    struct { double ass_video_aspect; int ass_use_video_data; }
        options = {explicit_aspect, video_data}, *opts = &options;
    struct { int ass_style_override[1]; } shared = {{override}}, *shared_opts = &shared;
    struct { int order; } subtitle = {0}, *sd = &subtitle;
'''
    fixture += ass_scale
    fixture += "    return scale;\n}\n"
    fixture += r'''
#define AV_CODEC_ID_DVD_SUBTITLE 1
#define AV_CODEC_ID_HDMV_PGS_SUBTITLE 2
static double bitmap_video_par(struct mp_osd_res d, struct mp_image_params params,
                               int codec, bool stretch) {
    struct decoder { int codec_id; } decoder = {codec};
    struct { struct mp_image_params video_params; struct decoder *avctx; }
        context = {params, &decoder}, *priv = &context;
    struct { bool stretch_dvd_subs; } options = {stretch}, *opts = &options;
'''
    fixture += bitmap_par
    fixture += "    return video_par;\n}\n"
    fixture += r'''
int main(void) {
    struct MPContext mpctx = {0};
    struct mp_cmd_ctx cmd = {.mpctx = &mpctx};
    // Full 16:9 image in a 4:3 ZOOM viewport, before clipping at the View parent.
    int coordinates[] = {800, 600, -133, 0, 933, 600};
    for (int n = 0; n < 6; n++)
        cmd.args[n].v.i = coordinates[n];
    cmd_android_video_geometry(&cmd);
    assert(cmd.success && commits == 1);
    struct mp_osd_res geometry;
    assert(android_osd_geometry_from_node(&committed, &geometry));
    assert(geometry.w == 800 && geometry.h == 600);
    assert(geometry.ml == -133 && geometry.mr == -133);
    assert(geometry.mt == 0 && geometry.mb == 0 && geometry.display_par == 1);
    // Shrinking and panning can reveal the source outside the preceding crop.
    cmd.args[2].v.i = 200;
    cmd.args[3].v.i = 150;
    cmd.args[4].v.i = 733;
    cmd.args[5].v.i = 450;
    cmd_android_video_geometry(&cmd);
    assert(cmd.success && commits == 2);
    assert(android_osd_geometry_from_node(&committed, &geometry));
    assert(geometry.ml == 200 && geometry.mr == 67);
    assert(geometry.mt == 150 && geometry.mb == 150);
    // Invalid rectangles and unrepresentable margins never publish partial state.
    const int invalid[][6] = {
        {0, 600, 0, 0, 800, 600}, {800, -1, 0, 0, 800, 600},
        {800, 600, 20, 0, 20, 600}, {800, 600, 0, 20, 800, 10},
        {800, 600, INT_MIN, 0, 800, 600},
        {800, 600, -100, 0, INT_MAX, 600},
    };
    for (int n = 0; n < MP_ARRAY_SIZE(invalid); n++) {
        for (int i = 0; i < 6; i++)
            cmd.args[i].v.i = invalid[n][i];
        cmd_android_video_geometry(&cmd);
        assert(!cmd.success && commits == 2);
    }
    for (int n = 0; n < 6; n++)
        cmd.args[n].v.i = coordinates[n];
    reject_commit = true;
    cmd_android_video_geometry(&cmd);
    assert(!cmd.success && commits == 2);
    reject_commit = false;
    struct mpv_node empty = {0};
    assert(!android_osd_geometry_from_node(&empty, &geometry));
    committed_list.num = 5;
    assert(!android_osd_geometry_from_node(&committed, &geometry));
    committed_list.num = 6;
    assert(android_osd_geometry_from_node(&committed, &geometry));
    assert(geometry.video_aspect == 0);
    committed_list.num = 7;
    committed_values[0].format = 0;
    assert(!android_osd_geometry_from_node(&committed, &geometry));
    committed_values[0].format = MPV_FORMAT_INT64;
    struct mp_vo_opts opts = {
        .scale_x = 2, .scale_y = 3, .pan_x = .25f, .pan_y = -.5f,
        .android_video_surface_transform = true, .keepaspect = true,
        .android_video_geometry = committed,
    };
    struct mp_vo_opts original = opts;
    struct driver driver = {0};
    struct vo vo = {&opts, &driver, &opts, NULL, 1920, 1080, 1};
    struct android_osd_overlay overlay = {
        .vo = &vo, .window = true, .width = 400, .height = 300, .texcoord = 1,
    };
    // OSD buffer recreation may temporarily have a different size. Projection
    // scales the complete logical viewport, including fixed-position stats.
    update_geometry(&overlay);
    assert(overlay.osd_res.w == 800 && overlay.osd_res.h == 600);
    assert(overlay.osd_res.ml == 200 && overlay.osd_res.mr == 67);
    struct overlay_texture texture = {16, 16};
    struct sub_bitmap part = {.x = 200, .y = 150, .w = 16, .h = 16,
                              .dw = 400, .dh = 300};
    draw_part(&overlay, &texture, &part);
    assert(drawn_positions[0] == -.5f && drawn_positions[1] == .5f);
    assert(drawn_positions[4] == .5f && drawn_positions[5] == .5f);
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    android_osd_overlay_get_video_rects(&overlay, &src, &dst, &osd);
    assert(observed.scale_x == 1 && observed.scale_y == 1);
    assert(observed.pan_x == 0 && observed.pan_y == 0);
    assert(!observed.keepaspect);
    assert(observed_w == 1920 && observed_h == 1080);
    assert(opts.scale_x == original.scale_x && opts.scale_y == original.scale_y);
    assert(opts.pan_x == original.pan_x && opts.pan_y == original.pan_y);
    assert(opts.android_video_surface_transform);
    assert(vo.dwidth == 1920 && vo.dheight == 1080);
    // Recreating the OSD window changes its target, not video ownership.
    const bool windows[] = {false, true, false, true};
    for (int enabled = 0; enabled <= 1; enabled++) {
        opts.android_video_surface_transform = enabled;
        for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
            overlay.window = windows[i];
            assert(android_osd_overlay_active(&overlay) ==
                   (enabled || windows[i]));
            assert(android_osd_overlay_transforms_video(&overlay) == enabled);
            int previous_fallback_calls = fallback_calls;
            android_osd_overlay_get_video_rects(&overlay, &src, &dst, &osd);
            if (enabled) {
                assert(fallback_calls == previous_fallback_calls);
                assert(observed.scale_x == 1 && observed.scale_y == 1);
                assert(observed.pan_x == 0 && observed.pan_y == 0);
                assert(!observed.keepaspect);
            } else {
                assert(fallback_calls == previous_fallback_calls + 1);
                assert(observed.scale_x == 2 && observed.scale_y == 3);
                assert(observed.pan_x == .25f && observed.pan_y == -.5f);
                assert(observed.keepaspect);
            }
            assert(opts.scale_x == original.scale_x);
            assert(opts.scale_y == original.scale_y);
            assert(opts.pan_x == original.pan_x && opts.pan_y == original.pan_y);
        }
    }
    overlay.window = true;
    vo.params = NULL;
    int previous_fallback_calls = fallback_calls;
    android_osd_overlay_get_video_rects(&overlay, &src, &dst, &osd);
    assert(fallback_calls == previous_fallback_calls + 1);
    assert(!android_osd_overlay_active(NULL));
    assert(!android_osd_overlay_transforms_video(NULL));
    // Clearing a binding must also release a configured-yes host transform.
    opts.android_video_surface_transform = true;
    vo.params = &opts;
    int previous_commits = commits;
    reject_commit = true;
    cmd_android_video_geometry_clear(&cmd);
    assert(!cmd.success && commits == previous_commits);
    assert(android_osd_geometry_from_node(&committed, &geometry));
    reject_commit = false;
    cmd_android_video_geometry_clear(&cmd);
    assert(cmd.success && commits == previous_commits + 1);
    opts.android_video_geometry = committed;
    assert(!android_osd_overlay_transforms_video(&overlay));
    overlay.window = false;
    assert(!android_osd_overlay_active(&overlay));
    previous_fallback_calls = fallback_calls;
    android_osd_overlay_get_video_rects(&overlay, &src, &dst, &osd);
    assert(fallback_calls == previous_fallback_calls + 1);
    assert(observed.keepaspect && observed.scale_x == opts.scale_x);
    update_geometry(&overlay);
    assert(observed_w == overlay.width && observed_h == overlay.height);
    assert(observed.keepaspect && observed.pan_x == opts.pan_x);
    cmd_android_video_geometry(&cmd);
    assert(cmd.success && commits == previous_commits + 2);
    opts.android_video_geometry = committed;
    assert(android_osd_overlay_transforms_video(&overlay));
    assert(android_osd_overlay_active(&overlay));
    update_geometry(&overlay);
    assert(overlay.osd_res.w == 800 && overlay.osd_res.h == 600);
    assert(overlay.osd_res.ml == -133 && overlay.osd_res.mr == -133);
    opts.android_video_geometry = empty;
    assert(!android_osd_overlay_transforms_video(&overlay));
    assert(!android_osd_overlay_active(&overlay));
    overlay.window = true;
    assert(android_osd_overlay_active(&overlay));
    opts.android_video_geometry = committed;

    // Subtitle baseline aspect is atomic, independent of gesture margins and OSD PAR.
    previous_commits = commits;
    const double invalid_aspects[] = {-1, NAN, INFINITY};
    for (unsigned n = 0; n < sizeof(invalid_aspects) / sizeof(invalid_aspects[0]); n++) {
        cmd.args[6].v.d = invalid_aspects[n];
        cmd_android_video_geometry(&cmd);
        assert(!cmd.success && commits == previous_commits);
    }
    cmd.args[6].v.d = 16.0 / 9;
    cmd_android_video_geometry(&cmd);
    assert(cmd.success && commits == previous_commits + 1);
    assert(android_osd_geometry_from_node(&committed, &geometry));
    assert(geometry.video_aspect == 16.0 / 9 && geometry.display_par == 1);
    struct mp_osd_res source_geometry = geometry;
    source_geometry.video_aspect = 0;
    assert(!osd_res_equals(source_geometry, geometry));
    assert(osd_res_equals(geometry, geometry));
    struct mp_image_params params = {320, 240, 1, 1, 0};
    assert(fabs(osd_get_video_par(geometry, &params) - 4.0 / 3) < 1e-12);
    geometry.ml = 200; geometry.mr = 300; geometry.mt = -100; geometry.mb = 200;
    assert(fabs(osd_get_video_par(geometry, &params) - 4.0 / 3) < 1e-12);
    params.p_w = 10; params.p_h = 11;
    assert(fabs(osd_get_video_par(source_geometry, &params) - 10.0 / 11) < 1e-12);
    for (int rotation = 0; rotation < 360; rotation += 90) {
        params.rotate = rotation;
        geometry.video_aspect = rotation % 180 ? 9.0 / 16 : 16.0 / 9;
        assert(fabs(osd_get_video_par(geometry, &params) - 4.0 / 3) < 1e-12);
    }
    params.rotate = 0;
    geometry.video_aspect = 16.0 / 9;
    assert(ass_video_scale(geometry, params, true, 1, 2, 0) == 1);
    assert(ass_video_scale(geometry, params, false, 1, 0, 0) == 1);
    assert(ass_video_scale(geometry, params, false, 1, 2, 2) == 2);
    assert(fabs(ass_video_scale(geometry, params, false, 0, 0, 0) - 4.0 / 3) < 1e-12);
    assert(fabs(ass_video_scale(geometry, params, false, 1, 2, 0) - 4.0 / 3) < 1e-12);
    assert(fabs(bitmap_video_par(geometry, params, AV_CODEC_ID_DVD_SUBTITLE, true) - 4.0 / 3) < 1e-12);
    assert(bitmap_video_par(geometry, params, AV_CODEC_ID_DVD_SUBTITLE, false) == 0);
    params = (struct mp_image_params){720, 480, 10, 11, 0};
    assert(fabs(bitmap_video_par(geometry, params, AV_CODEC_ID_HDMV_PGS_SUBTITLE, false) + 32.0 / 27) < 1e-12);
    assert(fabs(bitmap_video_par(source_geometry, params, AV_CODEC_ID_HDMV_PGS_SUBTITLE, false) + 10.0 / 11) < 1e-12);
    opts.android_video_geometry = committed;

    // FIT, ZOOM and fixed-axis layouts size the video Surface to its display
    // aspect. Panscan must not discard pixels before the View can shrink/pan.
    const int surfaces[][2] = {{1440, 1080}, {1920, 1440}, {1280, 960}};
    for (unsigned i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); i++) {
        for (int panscan = 0; panscan <= 1; panscan++) {
            int w, h;
            opts.panscan = panscan;
            aspect_calc_panscan(&opts, 1440, 1080, 1440, 1080, 0,
                                surfaces[i][0], surfaces[i][1], 1, &w, &h);
            assert(w == surfaces[i][0] && h == surfaces[i][1]);
            int sy0 = 0, sy1 = 1080, dy0, dy1, mt, mb;
            src_dst_split_scaling(1080, surfaces[i][1], h, 0, 0, 0, 1, false,
                                  &sy0, &sy1, &dy0, &dy1, &mt, &mb);
            assert(sy0 == 0 && sy1 == 1080 && mt == 0 && mb == 0);
        }
    }
    // The separate OSD uses the player viewport, including ZOOM's margins.
    int w, h, sy0 = 0, sy1 = 1080, dy0, dy1, mt, mb;
    opts.panscan = 1;
    aspect_calc_panscan(&opts, 1440, 1080, 1440, 1080, 0,
                        1920, 1080, 1, &w, &h);
    assert(w == 1920 && h == 1440);
    src_dst_split_scaling(1080, 1080, h, 0, 0, 0, 1, false,
                          &sy0, &sy1, &dy0, &dy1, &mt, &mb);
    assert(mt == -180 && mb == -180);
    // FILL changes display aspect while retaining the original source pixels.
    opts.panscan = 0;
    aspect_calc_panscan(&opts, 1440, 1080, 1920, 1080, 0,
                        1920, 1080, 1, &w, &h);
    assert(w == 1920 && h == 1080);

    display = 10; context = 20; draw = 30; read_surface = 40; api = 7;
    assert(android_osd_overlay_render(&overlay, 0));
    assert(display == 10 && context == 20 && draw == 30);
    assert(read_surface == 40 && api == 7);
    render_ok = false;
    assert(!android_osd_overlay_render(&overlay, 0));
    assert(!overlay.window);
    assert(android_osd_overlay_transforms_video(&overlay));
    assert(android_osd_overlay_active(&overlay));
    assert(display == 10 && context == 20 && draw == 30);
    assert(read_surface == 40 && api == 7);
    render_ok = true;
    fail_restore = true;
    assert(!android_osd_overlay_render(&overlay, 0));
    fail_restore = false;
    display = context = draw = read_surface = 0; api = 7;
    assert(android_osd_overlay_render(&overlay, 0));
    assert(display == 0 && context == 0 && api == 7);
    puts("Android OSD context, failure, geometry and opt-in contracts passed.");
    return 0;
}
'''
    if args.generate:
        args.generate.write_text(fixture, encoding="utf-8")
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mpv-osd-test-") as directory:
        path = Path(directory) / "test.c"
        path.write_text(fixture, encoding="utf-8")
        # Match mpv's treatment of intentionally unused production parameters.
        subprocess.run([args.cc, *args.cflag, "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-parameter", str(path), "-lm",
                        "-o", str(args.output)], check=True)


if __name__ == "__main__":
    main()
