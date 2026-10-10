/* Behavioral contracts for production image subtitle palette and refresh code. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAX_QUEUE 4
#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MP_NOPTS_VALUE (-1e20)
#define CONTROL_OK 1
#define CONTROL_UNKNOWN 0
#define UPDATE_OSD UINT64_C(1)
#define UPDATE_SUB_FILT (UINT64_C(1) << 1)
#define UPDATE_SUB_HARD (UINT64_C(1) << 13)

struct m_color { uint8_t r, g, b, a; };
struct mp_subtitle_opts {
    bool sub_gray;
    double image_subs_brightness;
    bool image_subs_override;
    struct m_color image_subs_color;
};
struct mp_image_params { int w, h; };
struct mp_dvdnav_highlight { int rect; };
struct stream_nav_state {
    bool menu_active;
    struct mp_dvdnav_highlight hl;
    uint32_t change_id;
};
struct sub {
    bool valid;
    int count, src_w, src_h;
    int64_t id;
    uint32_t original[4], rendered[4];
};
struct sd_lavc_priv {
    struct sub subs[MAX_QUEUE];
    int64_t new_id;
    struct mp_image_params video_params;
    bool menu_active;
    struct mp_dvdnav_highlight hl;
    uint32_t hli_change_id;
};
struct sd {
    struct mp_subtitle_opts *opts;
    struct sd_lavc_priv *priv;
};
enum sd_ctrl {
    SD_CTRL_SUB_STEP, SD_CTRL_SET_VIDEO_PARAMS, SD_CTRL_UPDATE_OPTS,
    SD_CTRL_APPLY_DVDNAV,
};

static int renders;
static void read_sub_bitmaps(struct sd *sd, struct sub *sub);
static double step_sub(struct sd *sd, double now, double movement)
{
    (void)sd;
    return now + movement;
}
static void clear_sub(struct sub *sub)
{
    sub->count = 0;
    sub->valid = false;
}

#include "image_subs_functions.h"

static void read_sub_bitmaps(struct sd *sd, struct sub *sub)
{
    assert(!sub->count && !sub->src_w && !sub->src_h);
    memcpy(sub->rendered, sub->original, sizeof(sub->original));
    convert_pal(sub->rendered, 4, sd->opts, sd->priv->menu_active);
    sub->count = 1;
    sub->src_w = sub->src_h = 10;
    renders++;
}

static int update_options(struct sd *sd)
{
    uint64_t flags = UPDATE_OSD;
    return control(sd, SD_CTRL_UPDATE_OPTS, &flags);
}

static struct mp_subtitle_opts defaults(void)
{
    return (struct mp_subtitle_opts) {
        .image_subs_brightness = 1.0,
        .image_subs_color = {255, 255, 255, 255},
    };
}

static void test_original_palette(void)
{
    struct mp_subtitle_opts opts = defaults();
    opts.image_subs_color = (struct m_color){0, 255, 0, 0};
    uint32_t colors[] = {0xff001122, 0x80112233, 0x00223344, 0x01ffffff};
    const uint32_t expected[] = {0xff001122, 0x80081119, 0, 0x01010101};
    convert_pal(colors, 4, &opts, false);
    assert(!memcmp(colors, expected, sizeof(colors)));
}

static void test_brightness(void)
{
    struct mp_subtitle_opts opts = defaults();
    opts.image_subs_brightness = 2.0;
    uint32_t colors[] = {0xff808080, 0x80405060, 0xff000000, 0x00ffffff};
    const uint32_t expected[] = {0xffffffff, 0x80405060, 0xff000000, 0};
    convert_pal(colors, 4, &opts, false);
    assert(!memcmp(colors, expected, sizeof(colors)));

    opts.image_subs_brightness = 0.5;
    uint32_t gray = 0xff808080;
    convert_pal(&gray, 1, &opts, false);
    assert(gray == 0xff404040);
}

static void test_tint_and_alpha(void)
{
    struct mp_subtitle_opts opts = defaults();
    opts.image_subs_override = true;
    opts.image_subs_color = (struct m_color){255, 255, 0, 128};
    uint32_t colors[] = {0xff808080, 0x80808080, 0xff000000, 0x00ffffff};
    const uint32_t expected[] = {0x80404000, 0x40202000, 0x80000000, 0};
    convert_pal(colors, 4, &opts, false);
    assert(!memcmp(colors, expected, sizeof(colors)));

    opts.image_subs_color.a = 255;
    uint32_t blue = 0xff0000ff;
    convert_pal(&blue, 1, &opts, false);
    assert(blue == 0xff121200);

    opts.image_subs_brightness = 2.0;
    uint32_t gray = 0xff808080;
    convert_pal(&gray, 1, &opts, false);
    assert(gray == 0xffffff00);
}

static void test_gray_order_and_menu_palette(void)
{
    struct mp_subtitle_opts opts = defaults();
    opts.sub_gray = true;
    uint32_t red = 0xffff0000;
    convert_pal(&red, 1, &opts, false);
    assert(red == 0xff555555);

    opts.image_subs_override = true;
    opts.image_subs_color = (struct m_color){255, 255, 0, 0};
    opts.image_subs_brightness = 2.0;
    red = 0xffff0000;
    convert_pal(&red, 1, &opts, false);
    assert(red == 0);

    opts.image_subs_color.a = 255;
    red = 0xffff0000;
    convert_pal(&red, 1, &opts, false);
    assert(red == 0xffaaaa00);

    uint32_t menu = 0xffff0000;
    convert_pal(&menu, 1, &opts, true);
    assert(menu == 0xff555555);
    opts.sub_gray = false;
    menu = 0x80112233;
    convert_pal(&menu, 1, &opts, true);
    assert(menu == 0x80081119);
}

static void test_refresh_and_restore(void)
{
    struct mp_subtitle_opts opts = defaults();
    struct sd_lavc_priv priv = { .new_id = 10 };
    struct sd sd = { .opts = &opts, .priv = &priv };
    priv.subs[0] = (struct sub) {
        .valid = true, .count = 1, .src_w = 10, .src_h = 10, .id = 1,
        .original = {0xff808080, 0x80808080, 0xff000000, 0},
    };
    priv.subs[2] = priv.subs[0];
    renders = 0;
    opts.image_subs_brightness = 2.0;
    assert(update_options(&sd) == CONTROL_OK);
    assert(renders == 2 && priv.subs[0].id == 10 && priv.subs[2].id == 11);
    assert(priv.subs[0].rendered[0] == 0xffffffff);
    assert(priv.subs[0].rendered[1] == 0x80808080);
    assert(!priv.subs[1].valid && !priv.subs[3].valid);

    opts.image_subs_override = true;
    opts.image_subs_color = (struct m_color){255, 255, 0, 255};
    assert(update_options(&sd) == CONTROL_OK);
    assert(priv.subs[0].id == 12 && priv.subs[0].rendered[0] == 0xffffff00);
    assert(priv.subs[0].original[0] == 0xff808080);

    opts = defaults();
    assert(update_options(&sd) == CONTROL_OK);
    assert(priv.subs[0].id == 14 && priv.subs[0].rendered[0] == 0xff808080);
    assert(priv.subs[0].rendered[1] == 0x80404040);
}

static void test_menu_transition(void)
{
    struct mp_subtitle_opts opts = defaults();
    opts.image_subs_brightness = 2.0;
    opts.image_subs_override = true;
    opts.image_subs_color = (struct m_color){255, 255, 0, 255};
    struct sd_lavc_priv priv = { .new_id = 10, .hli_change_id = 4 };
    struct sd sd = { .opts = &opts, .priv = &priv };
    priv.subs[0] = (struct sub) {
        .valid = true, .count = 1, .src_w = 10, .src_h = 10,
        .original = {0xff808080, 0, 0, 0},
    };
    assert(update_options(&sd) == CONTROL_OK);
    assert(priv.subs[0].rendered[0] == 0xffffff00);
    struct stream_nav_state nav = { .menu_active = true, .change_id = 4 };
    assert(control(&sd, SD_CTRL_APPLY_DVDNAV, &nav) == CONTROL_OK);
    assert(priv.subs[0].id == 11 && priv.subs[0].rendered[0] == 0xff808080);
    assert(update_options(&sd) == CONTROL_OK);
    assert(priv.subs[0].rendered[0] == 0xff808080);
    nav.menu_active = false;
    assert(control(&sd, SD_CTRL_APPLY_DVDNAV, &nav) == CONTROL_OK);
    assert(!priv.subs[0].valid && !priv.subs[0].count);
}

static void test_independent_decoders(void)
{
    struct mp_subtitle_opts opts = defaults();
    opts.image_subs_brightness = 1.5;
    struct sd_lavc_priv primary = { .new_id = 10 };
    struct sd_lavc_priv secondary = { .new_id = 20 };
    primary.subs[0] = (struct sub) {
        .valid = true, .count = 1, .src_w = 10, .src_h = 10,
        .original = {0xff646464, 0, 0, 0},
    };
    secondary.subs[0] = primary.subs[0];
    struct sd sd1 = { .opts = &opts, .priv = &primary };
    struct sd sd2 = { .opts = &opts, .priv = &secondary };
    assert(update_options(&sd1) == CONTROL_OK);
    assert(primary.subs[0].id == 10 && secondary.subs[0].id == 0);
    assert(update_options(&sd2) == CONTROL_OK);
    assert(secondary.subs[0].id == 20);
    assert(primary.subs[0].rendered[0] == 0xff969696);
    assert(secondary.subs[0].rendered[0] == primary.subs[0].rendered[0]);
}

static void test_hard_options_do_not_request_packet_replay(void)
{
    struct mp_subtitle_opts opts = defaults();
    struct sd_lavc_priv priv = { .new_id = 10 };
    struct sd sd = { .opts = &opts, .priv = &priv };
    priv.subs[0] = (struct sub) {
        .valid = true, .count = 1, .src_w = 10, .src_h = 10,
        .original = {0xff808080, 0, 0, 0},
    };
    uint64_t flags = UPDATE_OSD | UPDATE_SUB_HARD;
    assert(control(&sd, SD_CTRL_UPDATE_OPTS, &flags) == CONTROL_UNKNOWN);
    assert(priv.subs[0].id == 10 && priv.subs[0].rendered[0] == 0xff808080);
    flags = UPDATE_OSD | UPDATE_SUB_FILT;
    assert(control(&sd, SD_CTRL_UPDATE_OPTS, &flags) == CONTROL_UNKNOWN);
    assert(priv.subs[0].id == 11);
}

int main(void)
{
    test_original_palette();
    test_brightness();
    test_tint_and_alpha();
    test_gray_order_and_menu_palette();
    test_refresh_and_restore();
    test_menu_transition();
    test_independent_decoders();
    test_hard_options_do_not_request_packet_replay();
    puts("Image subtitle palette and refresh contracts passed.");
    return 0;
}
