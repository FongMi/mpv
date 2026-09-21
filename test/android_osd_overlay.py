#!/usr/bin/env python3
"""Compile Android OSD context/lifecycle contracts from the production helpers.

The resulting executable is standalone; use an Android cross compiler and run
it on a device, or use a host C compiler. No real GPU is required by this test.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, name):
    marker = source.index(name + "(")
    start = source.rfind("\n", 0, marker) + 1
    body = source.index("{", marker)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


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
    for name in ("save_egl", "restore_egl", "android_osd_overlay_active",
                 "android_osd_overlay_transforms_video",
                 "android_osd_overlay_get_video_rects",
                 "android_osd_overlay_render"):
        helpers += function(source, name)
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
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
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
};
struct mp_rect { int unused; };
struct mp_osd_res { int unused; };
struct driver { int caps; };
struct vo {
    struct mp_vo_opts *opts;
    struct driver *driver;
    void *params, *log;
    int dwidth, dheight;
    double monitor_par;
};
struct android_osd_overlay { struct vo *vo; bool window; };
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
int main(void) {
    struct mp_vo_opts opts = {2, 3, .25f, -.5f, true, 0};
    struct mp_vo_opts original = opts;
    struct driver driver = {0};
    struct vo vo = {&opts, &driver, &opts, NULL, 1920, 1080, 1};
    struct android_osd_overlay overlay = {&vo, true};
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    android_osd_overlay_get_video_rects(&overlay, &src, &dst, &osd);
    assert(observed.scale_x == 1 && observed.scale_y == 1);
    assert(observed.pan_x == 0 && observed.pan_y == 0);
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
            assert(android_osd_overlay_active(&overlay) == windows[i]);
            assert(android_osd_overlay_transforms_video(&overlay) == enabled);
            int previous_fallback_calls = fallback_calls;
            android_osd_overlay_get_video_rects(&overlay, &src, &dst, &osd);
            if (enabled) {
                assert(fallback_calls == previous_fallback_calls);
                assert(observed.scale_x == 1 && observed.scale_y == 1);
                assert(observed.pan_x == 0 && observed.pan_y == 0);
            } else {
                assert(fallback_calls == previous_fallback_calls + 1);
                assert(observed.scale_x == 2 && observed.scale_y == 3);
                assert(observed.pan_x == .25f && observed.pan_y == -.5f);
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
