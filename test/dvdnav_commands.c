/* Exercise the production command owner with native DVD API replies scripted. */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STREAM_OK 1
#define STREAM_ERROR -1
#define STREAM_UNSUPPORTED -2
#define DVDNAV_STATUS_OK 1
#define DVDNAV_STATUS_ERR 0
#define MP_DBG(...) ((void)0)
enum { DVD_MENU_Root, DVD_MENU_Title, DVD_MENU_Part, DVD_MENU_Audio,
       DVD_MENU_Subpicture, DVD_MENU_Angle, DVD_MENU_Escape };
typedef int dvdnav_status_t;
typedef struct { int unused; } dvdnav_t;
typedef struct { int up, down, left, right; } btni_t;
typedef struct {
    struct { struct { int hli_ss, btn_ns; } hl_gi; btni_t btnit[2]; } hli;
} pci_t;
struct priv {
    dvdnav_t *dvdnav;
    pci_t pci;
    bool pci_valid, still_active, activation_pending_jump;
    bool terminal_stop, wait_pending, pending_drain, at_boundary;
    bool drain_enabled, drain_immediate, drain_user_activation;
    bool tl_anchor, nav_pts_valid;
    int still_duration;
    uint32_t discontinuity_id;
};
typedef struct { struct priv *priv; } stream_t;
struct stream_nav_cmd;

static int menu_result, title_menu_result, up_result, activate_result, select_result, mouse_result;
static int menu_calls, up_calls, activate_calls, select_calls, mouse_calls;
static int highlight_calls, release_calls, dropped_segments;
static int current_button;
static bool native_positioned, menu_domain, move_on_menu, move_on_activate;

static dvdnav_status_t dvdnav_menu_call(dvdnav_t *dvd, int menu)
{
    (void)dvd;
    menu_calls++;
    int result = menu == DVD_MENU_Title ? title_menu_result : menu_result;
    if (result == DVDNAV_STATUS_OK && move_on_menu)
        native_positioned = false;
    return result;
}
static dvdnav_status_t dvdnav_go_up(dvdnav_t *dvd)
{
    (void)dvd;
    up_calls++;
    if (up_result == DVDNAV_STATUS_OK && move_on_menu)
        native_positioned = false;
    return up_result;
}
static dvdnav_status_t dvdnav_button_activate(dvdnav_t *dvd, pci_t *pci)
{
    (void)dvd;
    (void)pci;
    activate_calls++;
    if (activate_result == DVDNAV_STATUS_OK && move_on_activate)
        native_positioned = false;
    return activate_result;
}
static dvdnav_status_t dvdnav_mouse_activate(dvdnav_t *dvd, pci_t *pci, int x, int y)
{
    (void)x;
    (void)y;
    return dvdnav_button_activate(dvd, pci);
}
static dvdnav_status_t dvdnav_button_activate_cmd(dvdnav_t *dvd, int button, void *cmd)
{
    (void)dvd;
    (void)button;
    (void)cmd;
    release_calls++;
    return DVDNAV_STATUS_OK;
}
static dvdnav_status_t dvdnav_get_current_highlight(dvdnav_t *dvd, int32_t *button)
{
    (void)dvd;
    *button = current_button;
    return DVDNAV_STATUS_OK;
}
static dvdnav_status_t dvdnav_button_select(dvdnav_t *dvd, pci_t *pci, int button)
{
    (void)dvd;
    (void)pci;
    select_calls++;
    if (select_result == DVDNAV_STATUS_OK)
        current_button = button;
    return select_result;
}
static dvdnav_status_t dvdnav_mouse_select(dvdnav_t *dvd, pci_t *pci, int x, int y)
{
    (void)dvd;
    (void)pci;
    (void)x;
    (void)y;
    mouse_calls++;
    return mouse_result;
}
static dvdnav_status_t dvdnav_get_position(dvdnav_t *dvd, uint32_t *pos, uint32_t *len)
{
    (void)dvd;
    *pos = 0;
    *len = 1;
    return native_positioned ? DVDNAV_STATUS_OK : DVDNAV_STATUS_ERR;
}
static bool in_menu_domain(dvdnav_t *dvd) { (void)dvd; return menu_domain; }
static void update_highlight(struct priv *p) { (void)p; highlight_calls++; }
static bool nav_action_prohibited(dvdnav_t *dvd, int action)
{ (void)dvd; (void)action; return false; }
static int dvdnav_get_active_audio_stream(dvdnav_t *dvd) { (void)dvd; return 0; }
static int dvdnav_get_active_spu_stream(dvdnav_t *dvd) { (void)dvd; return 0; }
static void set_active_audio(struct priv *p, int stream) { (void)p; (void)stream; }
static void set_active_sub(struct priv *p, int stream) { (void)p; (void)stream; }
static void drop_segments(struct priv *p) { (void)p; dropped_segments++; }

#include "dvdnav_commands_functions.h"

static struct priv reset(dvdnav_t *dvd)
{
    menu_result = up_result = activate_result = select_result = mouse_result = DVDNAV_STATUS_OK;
    title_menu_result = DVDNAV_STATUS_OK;
    menu_calls = up_calls = activate_calls = select_calls = mouse_calls = 0;
    highlight_calls = release_calls = dropped_segments = 0;
    current_button = 1;
    native_positioned = menu_domain = true;
    move_on_menu = move_on_activate = false;
    return (struct priv){
        .dvdnav = dvd, .pci_valid = true, .still_active = true,
        .still_duration = 255, .drain_enabled = true, .discontinuity_id = 7,
        .pci = {.hli = {.hl_gi = {.hli_ss = 1, .btn_ns = 2},
                        .btnit = {{.up = 2, .down = 2, .left = 2, .right = 2}}}},
    };
}

static int run(struct priv *p, enum stream_nav_action action)
{
    stream_t stream = {.priv = p};
    struct stream_nav_cmd cmd = {.action = action};
    return handle_nav_cmd(&stream, &cmd);
}

static void assert_no_jump(const struct priv *p)
{
    assert(p->pci_valid && p->still_active && p->still_duration == 255);
    assert(p->discontinuity_id == 7 && !p->pending_drain && !p->drain_immediate);
    assert(!p->at_boundary && dropped_segments == 0 && release_calls == 0);
}

int main(void)
{
    dvdnav_t dvd = {0};
    struct priv p = reset(&dvd);
    menu_result = DVDNAV_STATUS_ERR;
    move_on_menu = true;
    assert(run(&p, STREAM_NAV_MENU_ROOT) == STREAM_OK && menu_calls == 2);
    assert(!p.pci_valid && p.pending_drain && p.discontinuity_id == 8);
    const enum stream_nav_action menus[] = {
        STREAM_NAV_MENU_ROOT, STREAM_NAV_MENU_TITLE, STREAM_NAV_MENU_POPUP,
        STREAM_NAV_MENU_AUDIO, STREAM_NAV_MENU_SUBTITLE, STREAM_NAV_MENU_ANGLE,
        STREAM_NAV_PREV_MENU,
    };
    for (unsigned i = 0; i < sizeof(menus) / sizeof(menus[0]); i++) {
        p = reset(&dvd);
        menu_result = title_menu_result = DVDNAV_STATUS_ERR;
        assert(run(&p, menus[i]) == STREAM_ERROR);
        assert(menu_calls == (menus[i] == STREAM_NAV_MENU_ROOT ? 2 : 1));
        assert(!p.activation_pending_jump && highlight_calls == 0);
        assert_no_jump(&p);
    }
    p = reset(&dvd);
    up_result = DVDNAV_STATUS_ERR;
    assert(run(&p, STREAM_NAV_GO_UP) == STREAM_ERROR && up_calls == 1);
    assert(highlight_calls == 0 && !p.activation_pending_jump);
    assert_no_jump(&p);
    const enum stream_nav_action activations[] = {STREAM_NAV_SELECT, STREAM_NAV_MOUSE_CLICK};
    for (unsigned i = 0; i < sizeof(activations) / sizeof(activations[0]); i++) {
        p = reset(&dvd);
        activate_result = DVDNAV_STATUS_ERR;
        assert(run(&p, activations[i]) == STREAM_ERROR);
        assert(activate_calls == 1 && !p.activation_pending_jump && highlight_calls == 0);
        assert_no_jump(&p);
    }
    const enum stream_nav_action directions[] = {
        STREAM_NAV_UP, STREAM_NAV_DOWN, STREAM_NAV_LEFT, STREAM_NAV_RIGHT,
    };
    for (unsigned i = 0; i < sizeof(directions) / sizeof(directions[0]); i++) {
        p = reset(&dvd);
        assert(run(&p, directions[i]) == STREAM_OK);
        assert(current_button == 2 && select_calls == 1);
        assert_no_jump(&p);
    }
    p = reset(&dvd);
    select_result = DVDNAV_STATUS_ERR;
    assert(run(&p, STREAM_NAV_RIGHT) == STREAM_ERROR && current_button == 1);
    assert(highlight_calls == 0);
    assert_no_jump(&p);
    p = reset(&dvd);
    p.pci.hli.btnit[0].right = 0;
    assert(run(&p, STREAM_NAV_RIGHT) == STREAM_OK && current_button == 1 && select_calls == 0);
    assert_no_jump(&p);
    p = reset(&dvd);
    mouse_result = DVDNAV_STATUS_ERR;
    assert(run(&p, STREAM_NAV_MOUSE_MOVE) == STREAM_OK && mouse_calls == 1);
    assert_no_jump(&p);
    p = reset(&dvd);
    assert(run(&p, STREAM_NAV_SELECT) == STREAM_OK && p.activation_pending_jump);
    assert_no_jump(&p); // Register-only activation must keep the current still/subpicture.
    p = reset(&dvd);
    move_on_activate = true;
    assert(run(&p, STREAM_NAV_SELECT) == STREAM_OK && activate_calls == 1);
    assert(!p.still_active && p.pending_drain && p.drain_immediate);
    assert(p.discontinuity_id == 8 && !p.activation_pending_jump);
    p = reset(&dvd);
    move_on_menu = true;
    p.terminal_stop = true;
    assert(run(&p, STREAM_NAV_MENU_ROOT) == STREAM_OK && menu_calls == 1);
    assert(!p.pci_valid && !p.still_active && !p.terminal_stop && p.pending_drain);
    assert(p.discontinuity_id == 8);
    p = reset(&dvd);
    move_on_menu = true;
    assert(run(&p, STREAM_NAV_GO_UP) == STREAM_OK && up_calls == 1);
    assert(release_calls == 1 && !p.still_active && p.pending_drain);
    assert(p.discontinuity_id == 8);
    p = reset(&dvd);
    assert(run(&p, STREAM_NAV_KEY_RED) == STREAM_UNSUPPORTED);
    assert_no_jump(&p);
    puts("DVD commands: rejected actions, accepted no-ops, activation and jump ownership passed");
    return 0;
}
