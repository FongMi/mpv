/* Host fixture for X11 window geometry publication from production code. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#define VO_EVENT_RESIZE 1
#define VO_WIN_FORCE_POS 1
#define RC_W(r) ((r).x1 - (r).x0)
#define RC_H(r) ((r).y1 - (r).y0)
#define mp_assert assert

struct mp_rect { int x0, y0, x1, y1; };
struct vo_win_geometry { struct mp_rect win; double monitor_par; int flags; };
struct mp_vo_opts {
    bool auto_window_resize, ontop, show_in_taskbar;
    struct { bool wh_valid, xy_valid; } geometry;
    int focus_on;
};
struct vo_x11_state {
    struct mp_vo_opts *opts;
    struct mp_rect screenrc, nofsrc, winrc;
    double dpi_scale;
    bool window, parent, pseudo_mapped, window_hidden;
    int old_dw, old_dh, pending_vo_events;
};
struct vo {
    struct vo_x11_state *x11;
    int dwidth, dheight;
    double monitor_par;
};

static struct vo_win_geometry calculated;
static int events, resizes;

static void vo_calc_window_geometry(struct vo *vo, struct mp_vo_opts *opts,
    const struct mp_rect *screen, const struct mp_rect *monitor,
    double dpi_scale, bool force, struct vo_win_geometry *geo, void *constraint)
{
    *geo = calculated;
}

static void vo_event(struct vo *vo, int event)
{
    // Publication must survive the config path clearing its pending resize.
    assert(!(vo->x11->pending_vo_events & VO_EVENT_RESIZE));
    assert(event == VO_EVENT_RESIZE);
    events++;
}

static void vo_x11_update_screeninfo(struct vo *vo) {}
static void vo_x11_update_geometry(struct vo *vo) {}
static void wait_until_mapped(struct vo *vo) {}
static void vo_x11_fullscreen(struct vo *vo) {}
static void vo_x11_setlayer(struct vo *vo, bool value) {}
static void vo_x11_set_in_taskbar(struct vo *vo, bool value) {}
static void vo_x11_map_window(struct vo *vo, struct mp_rect rc) { resizes++; }
static void vo_x11_highlevel_resize(struct vo *vo, struct mp_rect rc, bool force)
{
    resizes++;
}
static void x11_send_ewmh_msg(struct vo_x11_state *x11, const char *name,
                              long *data) {}

#include "x11_window_geometry_functions.h"

int main(void)
{
    struct mp_vo_opts opts = {.show_in_taskbar = true};
    struct mp_rect rect = {.x1 = 1920, .y1 = 1080};
    struct vo_x11_state x11 = {
        .opts = &opts, .window = true, .pseudo_mapped = true,
        .nofsrc = rect, .winrc = rect, .old_dw = 1920, .old_dh = 1080,
    };
    struct vo vo = {.x11 = &x11, .dwidth = 1920, .dheight = 1080,
                    .monitor_par = 1};
    calculated = (struct vo_win_geometry){.win = rect, .monitor_par = 1};
    vo_x11_config_vo_window(&vo);
    assert(!events && !resizes);

    // A mapped window can retain its size while applying a new monitor PAR.
    calculated.win.x1 = 3840;
    calculated.monitor_par = 0.5;
    vo_x11_config_vo_window(&vo);
    assert(vo.monitor_par == 0.5 && vo.dwidth == 1920 && vo.dheight == 1080);
    assert(events == 1 && !resizes && !x11.pending_vo_events);

    vo_x11_config_vo_window(&vo);
    assert(events == 1 && !resizes);

    // Restoring PAR publishes another change, still without resizing the window.
    calculated.monitor_par = 1;
    vo_x11_config_vo_window(&vo);
    assert(events == 2 && vo.monitor_par == 1 && !resizes);
    puts("X11 monitor PAR publication contracts passed.");
}
