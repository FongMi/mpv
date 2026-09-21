/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#if !HAVE_GPL
#error GPL only
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#ifdef __linux__
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#endif

#include <dvdnav/dvdnav.h>
#include <dvdnav/title_layout.h>
#include <libavutil/common.h>
#include <libavutil/intreadwrite.h>

#include "osdep/io.h"

#include "options/options.h"
#include "common/msg.h"
#include "input/input.h"
#include "misc/thread_tools.h"
#include "options/m_config.h"
#include "options/path.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "stream.h"
#include "demux/demux.h"
#include "video/csputils.h"
#include "video/out/vo.h"

#define TITLE_MENU -1
#define TITLE_LONGEST -2

#define DVD_TIMEBASE 90000
#define DVD_TIME_TO_S(x) ((x) / (double)(DVD_TIMEBASE))
#define DVD_TIME_FROM_S(x) ((int64_t)((x) * DVD_TIMEBASE))

#define DVD_SRC_W_DEFAULT 720
#define DVD_SRC_H_DEFAULT 576

struct priv {
    dvdnav_t *dvdnav;                   // handle to libdvdnav stuff
    stream_t *iso_stream;               // backing stream for ISO images
    struct mp_log *log;                 // borrowed from the stream
    struct mp_log *lib_log;             // used by libdvdnav's log callback
    bool probing;                       // open is an .iso auto-detection probe
    char *filename;                     // path
    int64_t duration;                   // in 90 kHz PTS ticks
    uint32_t last_nav_end_pts;          // last VOBU end PTS in 90 kHz ticks
    bool nav_pts_valid;
    bool had_initial_vts;

    int dvd_speed;

    int track;
    char *device;

    mp_mutex vm_lock;                   // serializes libdvdnav reads and controls
    mp_mutex lock;                      // protects only the published state below
    struct stream_nav_state nav_state;
    struct stream_dvd_streams dvd_streams;
    bool dvd_catalog_dirty, dvd_selection_dirty;
    int num_titles;                     // immutable after opening; -1 on error
    int current_title;                  // libdvdnav title number; -1 on error
    int64_t current_time;               // in 90 kHz PTS ticks; -1 on error

    // The remaining navigation state is owned by vm_lock.
    bool still_active;                  // fill_buffer() is holding a still
    bool terminal_stop;                 // EOF until an explicit navigation command
    bool failed;                        // fatal VM error; cancellation is not failure
    bool menu_support_known;            // reachability cached for the current VTS
    bool menu_supported;
    int still_duration;                 // finite length in seconds; 0 if infinite
    uint32_t still_id;                  // incremented for each new held still
    uint32_t spu_clut[16];              // the PGC CLUT, as RGB
    bool spu_clut_valid;
    bool in_menu;
    int current_button;                 // mirror of libdvdnav HL_BTNN_REG
    struct mp_dvdnav_highlight hl;      // focused button rect + palette
    uint32_t nav_change_id;
    uint32_t discontinuity_id;          // bumped on actions that may jump
    bool drain_enabled;                 // a demuxer is attached and acks drains
    bool pending_drain;                 // hold EOF until the demuxer acks
    bool wait_pending;                  // DVDNAV_WAIT awaits that drain
    bool drain_immediate;               // user command should flush queued data now
    bool drain_user_activation;         // pending jump came from a button
    bool activation_pending_jump;       // activation may jump on the next read
    bool at_boundary;                   // no payload delivered since last jump
    int src_w, src_h;                   // video resolution in pixels
    int auto_actioned_button;           // last auto-activated button; 0 if none
    pci_t pci;                          // Copy of the last NAV packet's PCI.
    bool pci_valid;

    // Disc-driven audio/sub/angle state.
    int audio_physical;                 // 0..7 from DVDNAV_AUDIO_STREAM_CHANGE
    int audio_logical;                  // VTS/domain attribute index
    int sub_physical;                   // 0..31 from DVDNAV_SPU_STREAM_CHANGE
    bool sub_visible;                   // SPU "on" flag from same event

    struct dvd_opts *opts;

    // What demux_disc maps the data by, see STREAM_CTRL_GET_SEGMENTS.
    struct stream_segment *segs;        // VOBUs read, not collected yet
    int num_segs;
    bool tl_anchor;                     // the next VOBU takes its playback
                                        // time from the VM (after a jump)
    bool cell_start;                    // the next VOBU opens a cell
    double cell_raw_base;               // start PTM of the cell's first VOBU
    double cell_time;                   // playback time of cell_raw_base
    double cell_raw_end;                // end PTM of its last VOBU read
};

struct dvd_opts {
    int angle;
    int speed;
    int region;
    char *device;
};

#define OPT_BASE_STRUCT struct dvd_opts

const struct m_sub_options dvd_conf = {
    .opts = (const struct m_option[]){
        {"device", OPT_STRING(device), .flags = M_OPT_FILE},
        {"speed", OPT_INT(speed)},
        {"angle", OPT_INT(angle), M_RANGE(1, 99)},
        {"region", OPT_CHOICE(region, {"auto", 0}), M_RANGE(1, 8)},
        {0}
    },
    .size = sizeof(struct dvd_opts),
    .defaults = &(const struct dvd_opts){
        .angle = 1,
    },
};

#define DNE(e) [e] = # e
static const char *const mp_dvdnav_events[] = {
    DNE(DVDNAV_BLOCK_OK),
    DNE(DVDNAV_NOP),
    DNE(DVDNAV_STILL_FRAME),
    DNE(DVDNAV_SPU_STREAM_CHANGE),
    DNE(DVDNAV_AUDIO_STREAM_CHANGE),
    DNE(DVDNAV_VTS_CHANGE),
    DNE(DVDNAV_CELL_CHANGE),
    DNE(DVDNAV_NAV_PACKET),
    DNE(DVDNAV_STOP),
    DNE(DVDNAV_HIGHLIGHT),
    DNE(DVDNAV_SPU_CLUT_CHANGE),
    DNE(DVDNAV_HOP_CHANNEL),
    DNE(DVDNAV_WAIT),
};

#define LOOKUP_NAME(array, i) \
    (((i) >= 0 && (i) < MP_ARRAY_SIZE(array)) ? array[(i)] : "?")

static void dvd_set_speed(stream_t *stream, char *device, unsigned speed)
{
#if defined(__linux__) && defined(SG_IO) && defined(GPCMD_SET_STREAMING)
    int fd;
    unsigned char buffer[28];
    unsigned char cmd[12];
    struct sg_io_hdr sghdr;
    struct stat st;

    memset(&st, 0, sizeof(st));

    if (stat(device, &st) == -1) return;

    if (!S_ISBLK(st.st_mode)) return; /* not a block device */

    switch (speed) {
    case 0: /* don't touch speed setting */
        return;
    case -1: /* restore default value */
        MP_INFO(stream, "Restoring DVD speed... ");
        break;
    default: /* limit to <speed> KB/s */
        // speed < 100 is multiple of DVD single speed (1350KB/s)
        if (speed < 100)
            speed *= 1350;
        MP_INFO(stream, "Limiting DVD speed to %dKB/s... ", speed);
        break;
    }

    memset(&sghdr, 0, sizeof(sghdr));
    sghdr.interface_id = 'S';
    sghdr.timeout = 5000;
    sghdr.dxfer_direction = SG_DXFER_TO_DEV;
    sghdr.dxfer_len = sizeof(buffer);
    sghdr.dxferp = buffer;
    sghdr.cmd_len = sizeof(cmd);
    sghdr.cmdp = cmd;

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = GPCMD_SET_STREAMING;
    cmd[10] = sizeof(buffer);

    memset(buffer, 0, sizeof(buffer));
    /* first sector 0, last sector 0xffffffff */
    AV_WB32(buffer + 8, 0xffffffff);
    if (speed == -1)
        buffer[0] = 4; /* restore default */
    else {
        /* <speed> kilobyte */
        AV_WB32(buffer + 12, speed);
        AV_WB32(buffer + 20, speed);
    }
    /* 1 second */
    AV_WB16(buffer + 18, 1000);
    AV_WB16(buffer + 26, 1000);

    fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        MP_INFO(stream, "Couldn't open DVD device for writing, changing DVD speed needs write access.\n");
        return;
    }

    if (ioctl(fd, SG_IO, &sghdr) < 0)
        MP_INFO(stream, "failed\n");
    else
        MP_INFO(stream, "successful\n");

    close(fd);
#endif
}

// Check if this is likely to be an .ifo or similar file.
static int dvd_probe(const char *path, const char *ext, const char *sig)
{
    if (!bstr_case_endswith(bstr0(path), bstr0(ext)))
        return false;

    FILE *temp = fopen(path, "rb");
    if (!temp)
        return false;

    bool r = false;

    char data[50];

    mp_assert(strlen(sig) <= sizeof(data));

    if (fread(data, 50, 1, temp) == 1) {
        if (memcmp(data, sig, strlen(sig)) == 0)
            r = true;
    }

    fclose(temp);
    return r;
}

static bool in_menu_domain(dvdnav_t *dvdnav)
{
    return dvdnav_is_domain_vmgm(dvdnav) == 1 ||
           dvdnav_is_domain_vtsm(dvdnav) == 1 ||
           dvdnav_is_domain_fp(dvdnav) == 1;
}


static void compute_button_rect(struct priv *priv, pci_t *pci, int btn)
{
    priv->hl.rect = (struct mp_rect){0};
    if (btn <= 0 || btn > pci->hli.hl_gi.btn_ns)
        return;
    // btni_t is packed and full of bitfields, memcpy to ensure correct alignment.
    btni_t b;
    memcpy(&b, &pci->hli.btnit[btn - 1], sizeof(b));
    int xs = b.x_start, xe = b.x_end;
    int ys = b.y_start, ye = b.y_end;
    priv->hl.rect = (struct mp_rect){
        .x0 = xs, .y0 = ys,
        .x1 = xe > xs ? xe : xs,
        .y1 = ye > ys ? ye : ys,
    };
}

// Resolve the 4-entry "select-state" highlight palette for the focused button.
// btn_coli holds two packed words per color set ([select, action]); the select
// word encodes [Ci3 Ci2 Ci1 Ci0 A3 A2 A1 A0]: four CLUT indices and four 4-bit
// alphas.
static void compute_highlight_palette(struct priv *priv, pci_t *pci, int btn)
{
    memset(priv->hl.palette, 0, sizeof(priv->hl.palette));
    if (!priv->spu_clut_valid || btn <= 0 || btn > pci->hli.hl_gi.btn_ns)
        return;
    btni_t b;
    memcpy(&b, &pci->hli.btnit[btn - 1], sizeof(b));
    if (b.btn_coln == 0)
        return;
    int coln = b.btn_coln - 1;
    if (coln > 2)
        coln = 2;
    uint32_t coli;
    memcpy(&coli, &pci->hli.btn_colit.btn_coli[coln][0], sizeof(coli)); // [0] = select state

    for (int i = 0; i < 4; i++) {
        uint8_t ci = (coli >> (16 + i * 4)) & 0xF;
        uint8_t a  = (coli >> (i * 4)) & 0xF;
        uint32_t alpha = (a << 4) | a;
        priv->hl.palette[i] = (alpha << 24) | priv->spu_clut[ci];
    }
}

static const char *dvd_domain_name(dvdnav_t *dvdnav)
{
    if (dvdnav_is_domain_fp(dvdnav) == 1)
        return "FP";
    if (dvdnav_is_domain_vmgm(dvdnav) == 1)
        return "VMGM";
    if (dvdnav_is_domain_vtsm(dvdnav) == 1)
        return "VTSM";
    if (dvdnav_is_domain_vts(dvdnav) == 1)
        return "VTS";
    return "?";
}

// Map the logical stream codec and physical ordinal to demux_lavf AVStream->id.
static int dvd_audio_logical_stream(struct priv *priv, int physical)
{
    if (physical < 0)
        return -1;
    for (int n = 0; n < 8; n++) {
        if (dvdnav_get_audio_logical_stream(priv->dvdnav, n) == physical)
            return n;
    }
    return -1;
}

static int dvd_spu_logical_stream(struct priv *priv, int physical)
{
    if (physical < 0)
        return -1;
    for (int n = 0; n < 32; n++) {
        if (dvdnav_get_spu_logical_stream(priv->dvdnav, n) == physical)
            return n;
    }
    return -1;
}

static int dvd_audio_substream_id(struct priv *priv, int logical, int physical)
{
    if (logical < 0 || physical < 0 || physical > 7)
        return -1;
    uint16_t fmt = dvdnav_audio_stream_format(priv->dvdnav, logical);
    switch (fmt) {
    case DVD_AUDIO_FORMAT_AC3:
        return 0x80 + physical;
    case DVD_AUDIO_FORMAT_DTS:
        return 0x88 + physical;
    case DVD_AUDIO_FORMAT_LPCM:
        return 0xa0 + physical;
    case DVD_AUDIO_FORMAT_MPEG:
    case DVD_AUDIO_FORMAT_MPEG2_EXT:
        return 0x1c0 + physical;
    default:
        return -1;
    }
}

static int dvd_audio_logical_for_id(struct priv *priv, int id)
{
    if (id < 0)
        return -1;
    for (int n = 0; n < 8; n++) {
        int physical = dvdnav_get_audio_logical_stream(priv->dvdnav, n);
        if (dvd_audio_substream_id(priv, n, physical) == id)
            return n;
    }
    return -1;
}

// Resolve AST_REG using the same active-stream rules as libdvdnav. vm_lock held.
static int dvd_audio_logical(struct priv *priv, int logical, int physical)
{
    if (physical < 0 || physical > 7)
        return -1;
    dvdnav_t *dvdnav = priv->dvdnav;
    if (dvdnav_is_domain_vts(dvdnav) != 1)
        logical = 0;
    int mapped = logical >= 0 && logical < 8
        ? dvdnav_get_audio_logical_stream(dvdnav, logical) : -1;
    if (mapped < 0) {
        for (logical = 0; logical < 8; logical++) {
            mapped = dvdnav_get_audio_logical_stream(dvdnav, logical);
            if (mapped >= 0)
                break;
        }
    }
    return mapped == physical ? logical : -1;
}

static void refresh_video_resolution(struct priv *priv)
{
    uint32_t w = 0, h = 0;
    if (dvdnav_get_video_resolution(priv->dvdnav, &w, &h) == DVDNAV_STATUS_OK &&
        w > 0 && h > 0)
    {
        priv->src_w = (int)w;
        priv->src_h = (int)h;
    } else {
        priv->src_w = DVD_SRC_W_DEFAULT;
        priv->src_h = DVD_SRC_H_DEFAULT;
    }
}

static bool note_activation(struct priv *priv, dvdnav_status_t result)
{
    if (result != DVDNAV_STATUS_OK)
        return false;
    priv->activation_pending_jump = true;
    return true;
}

// Pull the current selection back from libdvdnav and refresh our overlay
// state. Called on NAV_PACKET/HIGHLIGHT events and after every nav command.
// Runs with priv->vm_lock held.
static void update_highlight(struct priv *priv)
{
    int prev_btn = priv->current_button;
    bool prev_menu = priv->in_menu;
    struct mp_dvdnav_highlight prev_hl = priv->hl;

    priv->in_menu = in_menu_domain(priv->dvdnav);
    pci_t *pci = priv->in_menu && priv->pci_valid ? &priv->pci : NULL;
    bool has_buttons = pci && pci->hli.hl_gi.hli_ss != 0 && pci->hli.hl_gi.btn_ns > 0;

    // Suppress the visible highlight while we're inside the menu's intro.
    // The PCI gives us both the current VOBU's start PTS and the highlight
    // valid window; once vobu_s_ptm catches up to hli_s_ptm (and we're still
    // inside hli_e_ptm), it's live.
    bool highlight_live = false;
    if (has_buttons) {
        uint32_t now = pci->pci_gi.vobu_s_ptm;
        uint32_t hs  = pci->hli.hl_gi.hli_s_ptm;
        uint32_t he  = pci->hli.hl_gi.hli_e_ptm;
        highlight_live = now >= hs && (he == 0 || now < he);
    }

    int32_t btn = 0;
    if (highlight_live)
        dvdnav_get_current_highlight(priv->dvdnav, &btn);

    if (!highlight_live || btn <= 0 || btn > pci->hli.hl_gi.btn_ns) {
        priv->current_button = 0;
        priv->hl = (struct mp_dvdnav_highlight){0};
    } else {
        priv->current_button = btn;
        compute_button_rect(priv, pci, btn);
        compute_highlight_palette(priv, pci, btn);
    }

    if (priv->in_menu != prev_menu || priv->current_button != prev_btn ||
        memcmp(&prev_hl, &priv->hl, sizeof(prev_hl)) != 0)
    {
        priv->nav_change_id++;
    }

    // When we leave the menu, clear the auto-action latch so the next entry
    // can fire again on the same button number.
    if (!priv->in_menu)
        priv->auto_actioned_button = 0;

    // A btnit entry can request immediate activation when its button gets focus.
    if (highlight_live && priv->current_button > 0 &&
        priv->current_button != priv->auto_actioned_button)
    {
        btni_t b;
        memcpy(&b, &pci->hli.btnit[priv->current_button - 1], sizeof(b));
        if (b.auto_action_mode == 1) {
            priv->auto_actioned_button = priv->current_button;
            note_activation(priv, dvdnav_button_activate(priv->dvdnav, pci));
        }
    }
}

// Move the highlight to the spec-defined neighbour of the current button. We
// resolve the neighbour ourselves and use dvdnav_button_select() (rather than
// dvdnav_{upper,lower,left,right}_button_select()) so that auto-action buttons
// are activated only through update_highlight(), where we can observe it.
static bool select_neighbour_button(struct priv *priv, pci_t *pci,
                                    enum stream_nav_action action)
{
    int32_t cur = 0;
    if (dvdnav_get_current_highlight(priv->dvdnav, &cur) != DVDNAV_STATUS_OK ||
        cur <= 0 || cur > pci->hli.hl_gi.btn_ns)
        return false;
    btni_t b;
    memcpy(&b, &pci->hli.btnit[cur - 1], sizeof(b));
    int target = action == STREAM_NAV_UP    ? b.up    :
                 action == STREAM_NAV_DOWN  ? b.down  :
                 action == STREAM_NAV_LEFT  ? b.left  :
                 action == STREAM_NAV_RIGHT ? b.right : 0;
    return target <= 0 ||
           dvdnav_button_select(priv->dvdnav, pci, target) == DVDNAV_STATUS_OK;
}

static void clear_still(struct priv *priv)
{
    priv->still_active = false;
    priv->still_duration = 0;
}

static bool vm_moved(struct priv *priv, dvdnav_status_t st)
{
    if (st != DVDNAV_STATUS_OK)
        return false;
    priv->pci_valid = false;
    update_highlight(priv);
    return true;
}

static void release_still(struct priv *priv)
{
    if (priv->still_active)
        dvdnav_button_activate_cmd(priv->dvdnav, 0, NULL);
    clear_still(priv);
}

// Accepted highlight moves need not activate the VM or discard presented data.
static bool do_nav_cmd(stream_t *stream, struct stream_nav_cmd *cmd, bool *acted)
{
    struct priv *priv = stream->priv;
    dvdnav_status_t st;
    *acted = false;

    switch (cmd->action) {
    case STREAM_NAV_MENU_ROOT:
        st = dvdnav_menu_call(priv->dvdnav, DVD_MENU_Root);
        if (st != DVDNAV_STATUS_OK)
            st = dvdnav_menu_call(priv->dvdnav, DVD_MENU_Title);
        *acted = vm_moved(priv, st);
        return *acted;
    case STREAM_NAV_MENU_TITLE:
        *acted = vm_moved(priv, dvdnav_menu_call(priv->dvdnav, DVD_MENU_Title));
        return *acted;
    case STREAM_NAV_MENU_POPUP:
        *acted = vm_moved(priv, dvdnav_menu_call(priv->dvdnav, DVD_MENU_Part));
        return *acted;
    case STREAM_NAV_MENU_AUDIO:
        *acted = vm_moved(priv, dvdnav_menu_call(priv->dvdnav, DVD_MENU_Audio));
        return *acted;
    case STREAM_NAV_MENU_SUBTITLE:
        *acted = vm_moved(priv, dvdnav_menu_call(priv->dvdnav, DVD_MENU_Subpicture));
        return *acted;
    case STREAM_NAV_MENU_ANGLE:
        *acted = vm_moved(priv, dvdnav_menu_call(priv->dvdnav, DVD_MENU_Angle));
        return *acted;
    case STREAM_NAV_GO_UP:
        st = dvdnav_go_up(priv->dvdnav);
        if (st == DVDNAV_STATUS_OK)
            release_still(priv);
        *acted = vm_moved(priv, st);
        return *acted;
    case STREAM_NAV_PREV_MENU:
        *acted = vm_moved(priv, dvdnav_menu_call(priv->dvdnav, DVD_MENU_Escape));
        return *acted;
    default:
        break;
    }

    if (!in_menu_domain(priv->dvdnav) || !priv->pci_valid)
        return false;

    pci_t *pci = &priv->pci;
    if (pci->hli.hl_gi.hli_ss == 0 || pci->hli.hl_gi.btn_ns == 0)
        return false;

    switch (cmd->action) {
    case STREAM_NAV_UP:
    case STREAM_NAV_DOWN:
    case STREAM_NAV_LEFT:
    case STREAM_NAV_RIGHT:
        if (!select_neighbour_button(priv, pci, cmd->action))
            return false;
        break;
    case STREAM_NAV_MOUSE_MOVE:
        // Moving outside a button is an accepted no-op.
        dvdnav_mouse_select(priv->dvdnav, pci, cmd->x, cmd->y);
        break;
    case STREAM_NAV_MOUSE_CLICK:
        *acted = note_activation(priv,
                                 dvdnav_mouse_activate(priv->dvdnav, pci,
                                                       cmd->x, cmd->y));
        if (!*acted)
            return false;
        break;
    case STREAM_NAV_SELECT:
        *acted = note_activation(priv,
                                 dvdnav_button_activate(priv->dvdnav, pci));
        if (!*acted)
            return false;
        break;
    default:
        return false;
    }

    // A button command jumps only when the disc says so, and libdvdnav clears
    // the still it was parked on only then. Take the library's answer instead
    // of keeping our own, which cannot tell the two apart. Without this a
    // resync re-delivers the old page's subpicture over the page the jump
    // landed on.
    if (*acted && priv->still_active) {
        uint32_t pos, len;
        if (dvdnav_get_position(priv->dvdnav, &pos, &len) != DVDNAV_STATUS_OK)
            priv->still_active = false;
    }

    update_highlight(priv);
    return true;
}

static void drop_segments(struct priv *priv);

static void set_active_audio(struct priv *priv, int physical)
{
    int logical = dvdnav_get_active_logical_stream(priv->dvdnav, DVD_AUDIO_STREAM);
    logical = dvd_audio_logical(priv, logical, physical);
    if (priv->audio_physical != physical || priv->audio_logical != logical) {
        priv->audio_physical = physical;
        priv->audio_logical = logical;
        priv->nav_change_id++;
    }
}

static void set_active_sub(struct priv *priv, int raw)
{
    bool visible = raw >= 0 && !(raw & 0x80);
    int phys = raw >= 0 ? (raw & 0x1F) : -1;
    if (priv->sub_physical != phys || priv->sub_visible != visible) {
        priv->sub_physical = phys;
        priv->sub_visible = visible;
        priv->nav_change_id++;
    }
}

// A failed VM continuation cannot be resumed as a still or drain. vm_lock held.
static void mark_failed(stream_t *s)
{
    struct priv *priv = s->priv;
    priv->failed |= !mp_cancel_test(s->cancel);
    clear_still(priv);
    priv->pending_drain = false;
    priv->wait_pending = false;
    priv->drain_immediate = false;
    priv->drain_user_activation = false;
    priv->activation_pending_jump = false;
}

// Mark a source-position jump. Runs with priv->vm_lock held.
static void bump_discontinuity(struct priv *priv, bool immediate)
{
    set_active_audio(priv, dvdnav_get_active_audio_stream(priv->dvdnav));
    set_active_sub(priv, dvdnav_get_active_spu_stream(priv->dvdnav));
    drop_segments(priv);
    priv->tl_anchor = true;
    clear_still(priv);
    priv->nav_pts_valid = false;
    // Back-to-back jump events with no payload between them are one jump.
    if (priv->at_boundary) {
        if (immediate && priv->pending_drain)
            priv->drain_immediate = true;
        return;
    }
    priv->at_boundary = true;
    priv->discontinuity_id++;
    // Hold an EOF at the boundary until the demuxer flushed and acked.
    // Off while no demuxer is attached (open/probing would starve).
    if (priv->drain_enabled) {
        priv->pending_drain = true;
        priv->drain_immediate = immediate;
        priv->drain_user_activation = false;
    }
    MP_DBG(priv, "discontinuity -> %"PRIu32" (drain=%d)\n",
           priv->discontinuity_id, priv->pending_drain);
}

static bool nav_action_prohibited(dvdnav_t *dvdnav, enum stream_nav_action a)
{
    user_ops_t ops = dvdnav_get_restrictions(dvdnav);
    switch (a) {
    case STREAM_NAV_MENU_ROOT:     return ops.root_menu_call;
    case STREAM_NAV_MENU_TITLE:    return ops.title_menu_call;
    case STREAM_NAV_MENU_POPUP:    return ops.chapter_menu_call;
    case STREAM_NAV_MENU_AUDIO:    return ops.audio_menu_call;
    case STREAM_NAV_MENU_SUBTITLE: return ops.subpic_menu_call;
    case STREAM_NAV_MENU_ANGLE:    return ops.angle_menu_call;
    case STREAM_NAV_GO_UP:         return ops.go_up;
    case STREAM_NAV_PREV_MENU:     return ops.resume;
    case STREAM_NAV_SELECT:
    case STREAM_NAV_MOUSE_CLICK:   return ops.button_select_or_activate;
    default:                       return false;
    }
}

static int handle_nav_cmd(stream_t *stream, struct stream_nav_cmd *cmd)
{
    struct priv *priv = stream->priv;

    uint32_t pos, len;
    bool positioned = dvdnav_get_position(priv->dvdnav, &pos, &len) ==
                      DVDNAV_STATUS_OK;
    if (cmd->action >= STREAM_NAV_KEY_RED && cmd->action <= STREAM_NAV_KEY_BLUE)
        return STREAM_UNSUPPORTED;
    if (nav_action_prohibited(priv->dvdnav, cmd->action))
        MP_DBG(stream, "disc prohibits this user operation at the current position, performing it anyway\n");
    bool acted;
    if (!do_nav_cmd(stream, cmd, &acted))
        return STREAM_ERROR;
    // A VM jump invalidates the position until the next block; register-only
    // button commands leave it valid and must not discard the menu subpicture.
    bool position_lost = positioned &&
        dvdnav_get_position(priv->dvdnav, &pos, &len) != DVDNAV_STATUS_OK;
    if (position_lost && !priv->pending_drain)
        priv->at_boundary = false;
    if (position_lost || (acted && !priv->pci_valid)) {
        priv->terminal_stop = false;
        priv->wait_pending = false;
        bump_discontinuity(priv, true);
        priv->activation_pending_jump = false;
    }
    return STREAM_OK;
}



static bool dvd_logical_track_info(struct priv *priv,
                                    struct stream_track_req *req, int n)
{
    uint16_t lang = 0xffff;

    if (req->type == STREAM_AUDIO) {
        if (n < 0)
            return false;
        lang = dvdnav_audio_stream_to_lang(priv->dvdnav, n);
        audio_attr_t attr;
        if (dvdnav_get_audio_attr(priv->dvdnav, n, &attr) == DVDNAV_STATUS_OK) {
            MP_VERBOSE(priv, "audio 0x%x: logical %d, code extension %d\n",
                       req->id, n, attr.code_extension);
            switch (attr.code_extension) {
            case 2: // visually impaired
                req->flags |= TRACK_VISUAL_IMPAIRED;
                break;
            case 3: // director's comments
            case 4: // alternate director's comments
                req->flags |= TRACK_COMMENTARY;
                break;
            }
        }
    } else if (req->type == STREAM_SUB) {
        if (n < 0)
            return false;
        lang = dvdnav_spu_stream_to_lang(priv->dvdnav, n);
        subp_attr_t attr;
        if (dvdnav_get_spu_attr(priv->dvdnav, n, &attr) == DVDNAV_STATUS_OK) {
            MP_VERBOSE(priv, "subtitle 0x%x: logical %d, code extension %d\n",
                       req->id, n, attr.code_extension);
            switch (attr.code_extension) {
            case 9: // mandatory caption, forced display
                req->flags |= TRACK_FORCED;
                break;
            case 5: // closed captions, normal
            case 6: // closed captions, large
            case 7: // closed captions, for children
                req->flags |= TRACK_HEARING_IMPAIRED;
                break;
            case 13: // director's comments, normal
            case 14: // director's comments, large
            case 15: // director's comments, for children
                req->flags |= TRACK_COMMENTARY;
                break;
            }
        }
    } else {
        return false;
    }

    if (lang && lang != 0xffff)
        snprintf(req->name, sizeof(req->name), "%c%c", lang >> 8, lang);
    return true;
}

static bool dvd_track_info(stream_t *stream, struct stream_track_req *req)
{
    struct priv *priv = stream->priv;
    int logical = req->type == STREAM_AUDIO
        ? dvd_audio_logical_for_id(priv, req->id)
        : req->type == STREAM_SUB ? dvd_spu_logical_stream(priv, req->id) : -1;
    return dvd_logical_track_info(priv, req, logical);
}

/**
 * \brief mp_dvdnav_number_of_subs() returns the count of available subtitles
 * \param stream: - stream pointer
 * \return 0 on error, something meaningful otherwise
 */
static int mp_dvdnav_number_of_subs(stream_t *stream)
{
    struct priv *priv = stream->priv;
    uint8_t lg, k, n = 0;

    for (k = 0; k < 32; k++) {
        lg = dvdnav_get_spu_logical_stream(priv->dvdnav, k);
        if (lg == 0xff)
            continue;
        if (lg >= n)
            n = lg + 1;
    }
    return n;
}

static const char *dvd_audio_codec(uint16_t fmt)
{
    switch (fmt) {
    case DVD_AUDIO_FORMAT_AC3:       return "ac3";
    case DVD_AUDIO_FORMAT_DTS:       return "dts";
    case DVD_AUDIO_FORMAT_LPCM:      return "pcm_dvd";
    case DVD_AUDIO_FORMAT_MPEG:
    case DVD_AUDIO_FORMAT_MPEG2_EXT: return "mp2";
    }
    return NULL;
}

// Appends the streams the current program chain declares to the list: its
// video, the audio streams it makes available, its subpictures. With a
// VOBU's dsi, only the video and audio with packs in that VOBU (a
// subpicture stays declared, it shows on past the VOBU holding its packs).
static void dvd_list_streams(stream_t *stream, const dsi_t *dsi, void *ta,
                             struct stream_decl_stream **list, int *num)
{
    struct priv *priv = stream->priv;
    dvdnav_t *dvdnav = priv->dvdnav;
    if (!dsi || dsi->dsi_gi.vobu_1stref_ea) {
        MP_TARRAY_APPEND(ta, *list, *num, ((struct stream_decl_stream){
            STREAM_VIDEO, 0x1e0, "mpeg2video"}));
    }
    for (int l = 0; l < 8; l++) {
        int physical = dvdnav_get_audio_logical_stream(dvdnav, l);
        int id = dvd_audio_substream_id(priv, l, physical);
        if (id < 0 || (dsi && !dsi->synci.a_synca[physical]))
            continue;
        MP_TARRAY_APPEND(ta, *list, *num, ((struct stream_decl_stream){
            STREAM_AUDIO, id,
            dvd_audio_codec(dvdnav_audio_stream_format(dvdnav, l))}));
    }
    int subs = mp_dvdnav_number_of_subs(stream);
    for (int i = 0; i < MPMIN(subs, 32); i++) {
        MP_TARRAY_APPEND(ta, *list, *num, ((struct stream_decl_stream){
            STREAM_SUB, 0x20 + i, "dvd_subtitle"}));
    }
}

// Forget the VOBUs read but not collected. Runs with priv->vm_lock held.
static void drop_segments(struct priv *priv)
{
    TA_FREEP(&priv->segs);
    priv->num_segs = 0;
}

// Note the VOBU this NAV packet opens: where its data lands in the stream,
// the cell's timestamp base, which declared streams have packs in it. A
// VOBU without payload (the hold of a still, which is skipped) delivers
// nothing and takes no playback time. Runs with priv->vm_lock held.
static void record_vobu(stream_t *s, const pci_t *pci)
{
    struct priv *priv = s->priv;
    dvdnav_t *dvdnav = priv->dvdnav;
    const dsi_t *dsi = dvdnav_get_current_nav_dsi(dvdnav);
    double s_ptm = DVD_TIME_TO_S(pci->pci_gi.vobu_s_ptm);
    double e_ptm = DVD_TIME_TO_S(pci->pci_gi.vobu_e_ptm);

    if (!dsi || !dsi->dsi_gi.vobu_ea)
        return;

    if (priv->tl_anchor) {
        // The VM moved, its clock says where playback continues.
        priv->cell_raw_base = s_ptm;
        priv->cell_time = DVD_TIME_TO_S(dvdnav_get_current_time(dvdnav));
        priv->tl_anchor = false;
        priv->cell_start = false;
    } else if (priv->cell_start) {
        // A cell's timestamps continue where the previous cell's ended.
        priv->cell_time += priv->cell_raw_end - priv->cell_raw_base;
        priv->cell_raw_base = s_ptm;
        priv->cell_start = false;
    }
    priv->cell_raw_end = e_ptm;

    MP_TARRAY_APPEND(priv, priv->segs, priv->num_segs, ((struct stream_segment){
        .pos = s->pos,
        .raw_base = priv->cell_raw_base,
        .time = priv->cell_time,
    }));
    struct stream_segment *seg = &priv->segs[priv->num_segs - 1];
    struct stream_decl_stream *has = NULL;
    int num_has = 0;
    dvd_list_streams(s, dsi, NULL, &has, &num_has);
    for (int n = 0; n < num_has; n++) {
        MP_TARRAY_APPEND(priv->segs, seg->present, seg->num_present,
                         ((struct stream_stream_id){has[n].type, has[n].id}));
    }
    talloc_free(has);
}

// Handle one event from dvdnav_get_next_block(). Returns the value that
// fill_buffer() should return, or -1 to continue reading..
static struct stream_dvd_streams read_dvd_streams(struct priv *priv);
static bool same_dvd_streams(const struct stream_dvd_streams *a,
                             const struct stream_dvd_streams *b);

static int process_event(stream_t *s, int event, void *buf, int len)
{
    struct priv *priv = s->priv;
    dvdnav_t *dvdnav = priv->dvdnav;

    // BLOCK_OK is the hot path. Catalog metadata changes at navigation
    // events, while an AST/SPST-only command can select an alias without a
    // physical stream-change event.
    if (event != DVDNAV_BLOCK_OK)
        priv->dvd_selection_dirty = true;
    if (event == DVDNAV_CELL_CHANGE || event == DVDNAV_VTS_CHANGE ||
        event == DVDNAV_AUDIO_STREAM_CHANGE || event == DVDNAV_SPU_STREAM_CHANGE)
        priv->dvd_catalog_dirty = true;

    switch (event) {
    case DVDNAV_BLOCK_OK:
        // Real data is flowing again: we are no longer holding a still, and
        // the last jump boundary (if any) has been crossed.
        priv->activation_pending_jump = false;
        clear_still(priv);
        priv->at_boundary = false;
        return len;
    case DVDNAV_STOP:
        // End of disc: the next libdvdnav read would restart from first play.
        priv->activation_pending_jump = false;
        clear_still(priv);
        priv->terminal_stop = true;
        return 0;
    case DVDNAV_NAV_PACKET: {
        pci_t *pnavpci = dvdnav_get_current_nav_pci(dvdnav);
        if (!pnavpci)
            break;
        uint32_t start_pts = pnavpci->pci_gi.vobu_s_ptm;
        uint32_t backward = priv->last_nav_end_pts - start_pts;
        if (!priv->at_boundary && priv->nav_pts_valid &&
            priv->last_nav_end_pts > start_pts &&
            backward > DVD_TIMEBASE && backward < (1U << 31))
        {
            // This VOBU starts a new MPEG timeline without a HOP or WAIT.
            bump_discontinuity(priv, false);
        }
        priv->last_nav_end_pts = pnavpci->pci_gi.vobu_e_ptm;
        priv->nav_pts_valid = true;
        priv->pci = *pnavpci;
        priv->pci.hli.hl_gi.btn_ns =
            MPMIN(priv->pci.hli.hl_gi.btn_ns, MP_ARRAY_SIZE(priv->pci.hli.btnit));
        priv->pci_valid = true;
        MP_TRACE(s, "start pts = %"PRIu32"\n", pnavpci->pci_gi.vobu_s_ptm);
        record_vobu(s, &priv->pci);
        // Reset selection to first button if the current selection is out of
        // range. libdvdnav holds stale button selections when the menu changes,
        // which makes the new page unusable.
        if (in_menu_domain(dvdnav) && pnavpci->hli.hl_gi.hli_ss &&
            pnavpci->hli.hl_gi.btn_ns > 0)
        {
            int32_t btn = 0;
            dvdnav_get_current_highlight(dvdnav, &btn);
            if (btn <= 0 || btn > priv->pci.hli.hl_gi.btn_ns) {
                MP_VERBOSE(s, "stale button %d for a %d-button menu, "
                           "selecting button 1\n", btn,
                           priv->pci.hli.hl_gi.btn_ns);
                dvdnav_button_select(dvdnav, &priv->pci, 1);
            }
        }
        // Each NAV packet can change the highlighted button or the
        // available button set; keep our mirrored state in sync.
        update_highlight(priv);
        break;
    }
    case DVDNAV_STILL_FRAME: {
        dvdnav_still_event_t *still = buf;
        priv->activation_pending_jump = false;
        // A zero-length still has no presentation interval to hold.
        if (still->length == 0) {
            MP_VERBOSE(s, "zero-length still, skipping\n");
            if (dvdnav_still_skip(dvdnav) != DVDNAV_STATUS_OK) {
                MP_ERR(s, "failed to skip zero-length still\n");
                mark_failed(s);
                return 0;
            }
            break;
        }
        // An indefinite still outside a menu has no authored button action to
        // resume the VM. End the title after draining its last frame.
        if (still->length == 0xFF && !in_menu_domain(dvdnav)) {
            MP_VERBOSE(s, "title still -> EOF\n");
            clear_still(priv);
            return 0;
        }
        // Report EOF so the decoder drains and the last frame is presented.
        // The player starts finite timing only after playback reaches EOF;
        // indefinite menu stills remain held until user interaction moves the VM.
        if (!priv->still_active) {
            priv->still_active = true;
            priv->still_duration = still->length == 0xFF ? 0 : still->length;
            priv->still_id++;
            if (priv->still_duration) {
                MP_VERBOSE(s, "finite still -> EOF, hold last frame for %d s\n",
                           priv->still_duration);
            } else {
                MP_VERBOSE(s, "indefinite still -> EOF, hold last frame\n");
            }
        }
        return 0;
    }
    case DVDNAV_WAIT:
        // libdvdnav can be ahead of the decoder here. Keep its VM at this
        // boundary until the player has presented the queued packets.
        if (!priv->drain_enabled || (priv->at_boundary && !priv->pending_drain)) {
            if (dvdnav_wait_skip(dvdnav) != DVDNAV_STATUS_OK) {
                MP_ERR(s, "failed to resume DVD wait\n");
                mark_failed(s);
                return 0;
            }
            break;
        }
        MP_VERBOSE(s, "DVDNAV_WAIT: draining queued packets\n");
        priv->wait_pending = true;
        bump_discontinuity(priv, false);
        return 0;
    case DVDNAV_HOP_CHANNEL:
        MP_VERBOSE(s, "hop channel (domain=%s)\n", dvd_domain_name(dvdnav));
        bump_discontinuity(priv, false);
        if (priv->pending_drain && priv->activation_pending_jump)
            priv->drain_user_activation = true;
        priv->activation_pending_jump = false;
        break;
    case DVDNAV_HIGHLIGHT:
        update_highlight(priv);
        break;
    case DVDNAV_VTS_CHANGE: {
        int tit = 0, part = 0;
        dvdnav_vts_change_event_t *vts_event =
            (dvdnav_vts_change_event_t *)buf;
        MP_VERBOSE(s, "switched to VTS: %d (old=%d) domain=%s\n",
                   vts_event->new_vtsN, vts_event->old_vtsN,
                   dvd_domain_name(dvdnav));
        priv->menu_support_known = false;
        // Opening in menu mode can precede VM startup. Refresh even the
        // initial VTS, replacing any provisional resolution from open.
        refresh_video_resolution(priv);
        if (!priv->had_initial_vts) {
            // dvdnav sends an initial VTS change before any data; don't
            // cause a blocking wait for the player, because the player in
            // turn can't initialize the demuxer without data.
            priv->had_initial_vts = true;
            break;
        }
        if (dvdnav_current_title_info(dvdnav, &tit, &part) == DVDNAV_STATUS_OK)
        {
            MP_VERBOSE(s, "new title %d\n", tit);
        }
        // VTS change is a title-set boundary, flush.
        bump_discontinuity(priv, false);
        if (priv->pending_drain && priv->activation_pending_jump)
            priv->drain_user_activation = true;
        priv->activation_pending_jump = false;
        break;
    }
    case DVDNAV_CELL_CHANGE: {
        dvdnav_cell_change_event_t *ev =  (dvdnav_cell_change_event_t *)buf;

        // A new PGC can remap logical slots without a HOP or timestamp reset.
        // Stop before its first payload so old queued PES keeps the old catalog.
        struct stream_dvd_streams streams = read_dvd_streams(priv);
        if (priv->dvd_streams.generation && !priv->at_boundary &&
            !same_dvd_streams(&streams, &priv->dvd_streams))
            bump_discontinuity(priv, false);

        if (ev->pgc_length)
            priv->duration = ev->pgc_length;
        priv->cell_start = true;
        int64_t current_time = dvdnav_get_current_time(dvdnav);
        MP_VERBOSE(s, "cell change: cell=%d pg=%d pgc_len=%.3f "
                   "cur_time=%.3f domain=%s\n",
                   ev->cellN, ev->pgN, DVD_TIME_TO_S(ev->pgc_length),
                   DVD_TIME_TO_S(current_time),
                   dvd_domain_name(dvdnav));
        break;
    }
    case DVDNAV_SPU_CLUT_CHANGE: {
        // A CLUT entry is 0x00YYCrCb, and the matrix takes its input as
        // Y, Cb, Cr and answers as R, G, B.
        struct mp_csp_params csp = MP_CSP_PARAMS_DEFAULTS;
        struct pl_transform3x3 cmatrix;
        mp_get_csp_matrix(&csp, &cmatrix);
        const uint32_t *clut = buf;
        for (int i = 0; i < 16; i++) {
            uint32_t e = clut[i];
            int y[3] = {(e >> 16) & 0xff, e & 0xff, (e >> 8) & 0xff};
            int c[3];
            mp_map_fixp_color(&cmatrix, 8, y, 8, c);
            priv->spu_clut[i] = (c[0] << 16) | (c[1] << 8) | c[2];
        }
        priv->spu_clut_valid = true;
        update_highlight(priv);
        break;
    }
    case DVDNAV_AUDIO_STREAM_CHANGE: {
        dvdnav_audio_stream_change_event_t *ev = buf;
        // The event's logical value is AST_REG, not necessarily the active slot.
        int logical = dvd_audio_logical(priv, ev->logical, ev->physical);
        MP_VERBOSE(s, "audio change phys=%d log=%d domain=%s\n",
                   ev->physical, ev->logical, dvd_domain_name(dvdnav));
        if (priv->audio_physical != ev->physical || priv->audio_logical != logical) {
            priv->audio_physical = ev->physical;
            priv->audio_logical = logical;
            priv->nav_change_id++;
        }
        break;
    }
    case DVDNAV_SPU_STREAM_CHANGE: {
        dvdnav_spu_stream_change_event_t *ev = buf;
        MP_VERBOSE(s, "sub change phys_wide=0x%x lb=0x%x ps=0x%x log=%d domain=%s\n",
                   ev->physical_wide, ev->physical_letterbox,
                   ev->physical_pan_scan, ev->logical, dvd_domain_name(dvdnav));
        set_active_sub(priv, ev->physical_wide);
        break;
    }
    }
    return -1;
}

// Publish one coherent view after a VM read/event or control, with vm_lock
// held (or during open). libdvdnav getters may take its internal VM lock or do
// I/O, so none may run while holding the lock used by player-thread queries.

static void read_dvd_selection(struct priv *priv, struct stream_dvd_streams *streams)
{
    dvdnav_t *dvd = priv->dvdnav;
    streams->active_audio = dvdnav_get_active_logical_stream(dvd, DVD_AUDIO_STREAM);
    streams->active_sub = dvdnav_get_active_logical_stream(dvd, DVD_SUBTITLE_STREAM);
    int sub = dvdnav_get_active_spu_stream(dvd);
    streams->sub_visible = sub != -1 && !(sub & 0x80);
}

static struct stream_dvd_streams read_dvd_streams(struct priv *priv)
{
    dvdnav_t *dvd = priv->dvdnav;
    struct stream_dvd_streams streams = {
        .discontinuity_id = priv->discontinuity_id,
        .domain = dvdnav_is_domain_vts(dvd) ? 4 :
                  dvdnav_is_domain_vtsm(dvd) ? 3 :
                  dvdnav_is_domain_vmgm(dvd) ? 2 : 1,
    };
    int32_t title, pgc, program;
    if (dvdnav_current_title_program(dvd, &title, &pgc, &program) == DVDNAV_STATUS_OK) {
        streams.title = title;
        streams.pgc = pgc;
    }
    int audio_count = dvdnav_get_number_of_stream_attributes(dvd, DVD_AUDIO_STREAM);
    int sub_count = dvdnav_get_number_of_stream_attributes(dvd, DVD_SUBTITLE_STREAM);
    for (int i = 0; i < 8; i++) {
        streams.audio[i].id = -1;
        if (audio_count > i) {
            int physical = dvdnav_get_audio_logical_stream(dvd, i);
            streams.audio[i].id = dvd_audio_substream_id(priv, i, physical);
            struct stream_track_req req = {.type = STREAM_AUDIO, .id = streams.audio[i].id};
            if (dvd_logical_track_info(priv, &req, i)) {
                memcpy(streams.audio[i].lang, req.name, 2);
                streams.audio[i].flags = req.flags;
            }
        }
    }
    for (int i = 0; i < 32; i++) {
        streams.sub[i].id = -1;
        if (sub_count > i) {
            int physical = dvdnav_get_spu_logical_stream(dvd, i);
            if (physical >= 0 && physical < 32)
                streams.sub[i].id = 0x20 + physical;
            struct stream_track_req req = {.type = STREAM_SUB, .id = physical};
            if (dvd_logical_track_info(priv, &req, i)) {
                memcpy(streams.sub[i].lang, req.name, 2);
                streams.sub[i].flags = req.flags;
            }
        }
    }
    read_dvd_selection(priv, &streams);
    return streams;
}

static bool same_dvd_streams(const struct stream_dvd_streams *a,
                             const struct stream_dvd_streams *b)
{
    if (a->domain != b->domain || a->title != b->title || a->pgc != b->pgc)
        return false;
    // A timestamp reset within a title PGC does not replace its logical tracks.
    // Menus have no title identity, so retain their navigation epoch boundary.
    if ((a->domain != 4 || a->title <= 0 || a->pgc <= 0) &&
        a->discontinuity_id != b->discontinuity_id)
        return false;
    for (int i = 0; i < 8; i++) {
        if (a->audio[i].id != b->audio[i].id || a->audio[i].flags != b->audio[i].flags || strcmp(a->audio[i].lang, b->audio[i].lang))
            return false;
    }
    for (int i = 0; i < 32; i++) {
        if (a->sub[i].id != b->sub[i].id || a->sub[i].flags != b->sub[i].flags || strcmp(a->sub[i].lang, b->sub[i].lang))
            return false;
    }
    return true;
}

// Called under vm_lock, before touching AST/SPST. Read-ahead can hold a new
// MPEG timeline in the same PGC; only an unchanged logical catalog is valid.
static int select_dvd_stream(struct priv *priv, struct stream_dvd_select *select)
{
    struct stream_dvd_streams streams = read_dvd_streams(priv);
    if (priv->failed || priv->terminal_stop ||
        (priv->pending_drain && !priv->wait_pending &&
         (priv->drain_immediate || streams.domain != 4 ||
          streams.title <= 0 || streams.pgc <= 0)) ||
        !select->generation || select->generation != priv->dvd_streams.generation ||
        !same_dvd_streams(&streams, &priv->dvd_streams))
        return STREAM_ERROR;
    if (select->type == STREAM_SUB && select->logical == -1)
        return dvdnav_toggle_spu_stream(priv->dvdnav, 0) == DVDNAV_STATUS_OK
                   ? STREAM_OK : STREAM_ERROR;
    int limit = select->type == STREAM_AUDIO ? 8 : select->type == STREAM_SUB ? 32 : 0;
    if (select->logical < 0 || select->logical >= limit)
        return STREAM_ERROR;
    struct stream_dvd_stream *entry = select->type == STREAM_AUDIO
        ? &streams.audio[select->logical] : &streams.sub[select->logical];
    if (entry->id < 0)
        return STREAM_ERROR;
    int type = select->type == STREAM_AUDIO ? DVD_AUDIO_STREAM : DVD_SUBTITLE_STREAM;
    if (dvdnav_set_active_stream(priv->dvdnav, select->logical, type) != DVDNAV_STATUS_OK)
        return STREAM_ERROR;
    // The VM owner cannot stop between these calls. The validated setter
    // requires the same started VM as toggle_spu_stream.
    if (select->type == STREAM_SUB)
        dvdnav_toggle_spu_stream(priv->dvdnav, 1);
    return STREAM_OK;
}

// Runs under vm_lock only. Never acquire a native VM lock while holding the
// publication lock: GET_NAV_STATE must remain responsive during DVD reads.
static struct stream_dvd_streams update_dvd_streams(struct priv *priv,
                                                    struct stream_nav_state *state)
{
    struct stream_dvd_streams streams = priv->dvd_streams;
    // WAIT precedes title commands: only the drain id changed. The catalog
    // and selection still belong to the VM whose packets are being presented.
    if (priv->wait_pending && streams.generation) {
        streams.discontinuity_id = priv->discontinuity_id;
        if (priv->dvd_selection_dirty)
            read_dvd_selection(priv, &streams);
        priv->dvd_selection_dirty = false;
    // A boundary after a VM jump still belongs to the old presentation epoch.
    } else if ((!priv->pending_drain && !priv->terminal_stop) || !streams.generation) {
        if (priv->dvd_catalog_dirty || !streams.generation) {
            streams = read_dvd_streams(priv);
            streams.generation = priv->dvd_streams.generation;
            if (!streams.generation || !same_dvd_streams(&streams, &priv->dvd_streams))
                streams.generation++;
            priv->dvd_catalog_dirty = false;
        } else if (priv->dvd_selection_dirty) {
            read_dvd_selection(priv, &streams);
        }
        priv->dvd_selection_dirty = false;
    } else if (priv->pending_drain && !priv->drain_immediate &&
               !priv->terminal_stop && priv->dvd_selection_dirty) {
        // A selection accepted at a same-PGC timeline boundary is visible
        // immediately, without publishing a future VM's catalog or selection.
        struct stream_dvd_streams current = read_dvd_streams(priv);
        if (same_dvd_streams(&current, &streams)) {
            streams.active_audio = current.active_audio;
            streams.active_sub = current.active_sub;
            streams.sub_visible = current.sub_visible;
        }
        priv->dvd_selection_dirty = false;
    }
    state->no_audio = true;
    for (int i = 0; i < 8; i++)
        state->no_audio &= streams.audio[i].id < 0;
    state->dvd_generation = streams.generation;
    state->active_audio_logical = streams.active_audio;
    state->active_sub_logical = streams.active_sub;
    state->sub_visible = streams.sub_visible;
    return streams;
}

static void publish_state(struct priv *priv)
{
    dvdnav_t *dvdnav = priv->dvdnav;
    int32_t title = -1, part = 0;
    if (dvdnav_current_title_info(dvdnav, &title, &part) != DVDNAV_STATUS_OK)
        title = -1;
    int64_t time = dvdnav_get_current_time(dvdnav);
    int32_t cur_angle = 0, num_angles = 0;
    dvdnav_get_angle_info(dvdnav, &cur_angle, &num_angles);
    // Menu routes may default to physical 0 even when their IFO has no audio.
    bool no_audio = dvdnav_get_number_of_streams(dvdnav, DVD_AUDIO_STREAM) == 0;
    if (!priv->menu_support_known) {
        priv->menu_supported =
            dvdnav_menu_available(dvdnav, DVD_MENU_Root) == DVDNAV_STATUS_OK ||
            dvdnav_menu_available(dvdnav, DVD_MENU_Title) == DVDNAV_STATUS_OK;
        priv->menu_support_known = true;
    }
    if (priv->src_w <= 0 || priv->src_h <= 0)
        refresh_video_resolution(priv);
    struct stream_nav_state state = {
        .nav_active = true,
        .failed = priv->failed,
        .menu_supported = priv->menu_supported,
        .no_audio = no_audio,
        .menu_active = in_menu_domain(dvdnav),
        .still_active = priv->still_active,
        .still_duration = priv->still_duration,
        .still_id = priv->still_id,
        .src_w = priv->src_w,
        .src_h = priv->src_h,
        .hl = priv->hl,
        .change_id = priv->nav_change_id,
        .discontinuity_id = priv->discontinuity_id,
        .drain_pending = priv->pending_drain,
        .drain_immediate = priv->drain_immediate,
        .drain_user_activation = priv->drain_user_activation,
        .active_audio_id = no_audio ? -1 :
            dvd_audio_substream_id(priv, priv->audio_logical, priv->audio_physical),
        .active_sub_id = priv->sub_physical >= 0 ? 0x20 + priv->sub_physical : -1,
        .sub_visible = priv->sub_visible,
        .angle = cur_angle,
        .num_angles = num_angles,
    };
    struct stream_dvd_streams streams = update_dvd_streams(priv, &state);
    mp_mutex_lock(&priv->lock);
    priv->dvd_streams = streams;
    priv->nav_state = state;
    priv->current_title = title;
    priv->current_time = time;
    mp_mutex_unlock(&priv->lock);
}

static int fill_buffer(stream_t *s, void *buf, int max_len)
{
    struct priv *priv = s->priv;
    dvdnav_t *dvdnav = priv->dvdnav;

    if (max_len < 2048) {
        MP_FATAL(s, "Short read size. Data corruption will follow. Please "
                    "provide a patch.\n");
        return -1;
    }

    while (1) {
        // Keep the fetched event and its state update in one VM operation.
        mp_mutex_lock(&priv->vm_lock);
        bool drain = priv->pending_drain;
        if (priv->terminal_stop || priv->failed) {
            mp_mutex_unlock(&priv->vm_lock);
            return 0;
        }
        if (drain) {
            MP_DBG(s, "holding drain EOF (jump boundary)\n");
            mp_mutex_unlock(&priv->vm_lock);
            return 0;
        }

        int len = -1;
        int event = DVDNAV_NOP;
        dvdnav_status_t status = priv->drain_enabled
            ? dvdnav_get_next_block_with_wait(dvdnav, buf, &event, &len)
            : dvdnav_get_next_block(dvdnav, buf, &event, &len);
        if (status != DVDNAV_STATUS_OK)
        {
            MP_ERR(s, "Error getting next block from DVD %d (%s)\n",
                   event, dvdnav_err_to_string(dvdnav));
            mark_failed(s);
            publish_state(priv);
            mp_mutex_unlock(&priv->vm_lock);
            return 0;
        }
        if (event != DVDNAV_BLOCK_OK) {
            const char *name = LOOKUP_NAME(mp_dvdnav_events, event);
            MP_TRACE(s, "event %s (%d)\n", name, event);
        }

        int r = process_event(s, event, buf, len);
        publish_state(priv);
        mp_mutex_unlock(&priv->vm_lock);
        if (r >= 0)
            return r;
    }
    return 0;
}

static bool land_on_picture(stream_t *stream)
{
    struct priv *priv = stream->priv;
    dvdnav_t *dvdnav = priv->dvdnav;
    uint8_t buf[2048];

    // Read up to the landing VOBU's NAV packet, handling the events libdvdnav
    // queues for the seek on the way, as fill_buffer() does.
    for (int i = 0; i < 32; i++) {
        int event = DVDNAV_NOP, len = 0;
        if (dvdnav_get_next_block(dvdnav, buf, &event, &len) != DVDNAV_STATUS_OK)
            return false;
        int r = process_event(stream, event, buf, len);
        if (event == DVDNAV_NAV_PACKET)
            break;
        if (r >= 0)
            return false;
    }

    const dsi_t *dsi = dvdnav_get_current_nav_dsi(dvdnav);
    if (!dsi || dsi->dsi_gi.vobu_1stref_ea)
        return false;
    uint32_t back = dsi->vobu_sri.prev_video & SRI_END_OF_CELL;
    if (back == SRI_END_OF_CELL || back == 0)
        return false;
    if (dvdnav_sector_search(dvdnav, -(int64_t)back, SEEK_CUR) != DVDNAV_STATUS_OK) {
        MP_VERBOSE(stream, "landed in a VOBU without video, cannot move %"PRIu32
                   " blocks back to its picture: %s\n",
                   back, dvdnav_err_to_string(dvdnav));
        return false;
    }
    MP_VERBOSE(stream, "landed in a VOBU without video, moved %"PRIu32
               " blocks back to its picture\n", back);
    drop_segments(priv);
    priv->tl_anchor = true;
    priv->pci_valid = false;
    return true;
}

static int64_t seek_time_ticks(struct priv *priv, double d)
{
    int64_t tm = DVD_TIME_FROM_S(d);
    if (tm < 0)
        tm = 0;
    if (priv->duration > 0 && tm >= priv->duration)
        tm = priv->duration - 1;
    return tm;
}

// Called with vm_lock held so controls cannot split a read from its event.
static int control_locked(stream_t *stream, int cmd, void *arg)
{
    struct priv *priv = stream->priv;
    dvdnav_t *dvdnav = priv->dvdnav;
    int tit, part;

    switch (cmd) {
    case STREAM_CTRL_SET_DVD_STREAM:
        return select_dvd_stream(priv, arg);

    case STREAM_CTRL_GET_NUM_CHAPTERS: {
        if (dvdnav_current_title_info(dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        if (dvdnav_get_number_of_parts(dvdnav, tit, &part) != DVDNAV_STATUS_OK)
            break;
        if (!part)
            break;
        *(unsigned int *)arg = part;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CHAPTER_TIME: {
        double *ch = arg;
        int chapter = *ch;
        if (dvdnav_current_title_info(dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        uint64_t *parts = NULL, duration = 0;
        int n = dvdnav_describe_title_chapters(dvdnav, tit, &parts, &duration);
        if (!parts)
            break;
        if (chapter < 0 || chapter + 1 > n) {
            free(parts);
            break;
        }
        *ch = chapter > 0 ? DVD_TIME_TO_S(parts[chapter - 1]) : 0;
        free(parts);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TIME_LENGTH: {
        if (priv->duration > 0) {
            *(double *)arg = DVD_TIME_TO_S(priv->duration);
            return STREAM_OK;
        }
        break;
    }
    case STREAM_CTRL_GET_ASPECT_RATIO: {
        uint8_t ar = dvdnav_get_video_aspect(dvdnav);
        *(double *)arg = !ar ? 4.0 / 3.0 : 16.0 / 9.0;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TITLE_LENGTH: {
        int t = *(double *)arg;
        int32_t num_titles = 0;
        if (dvdnav_get_number_of_titles(dvdnav, &num_titles) != DVDNAV_STATUS_OK)
            break;
        if (t < 0 || t >= num_titles)
            break;
        uint64_t duration = 0;
        uint64_t *parts = NULL;
        dvdnav_describe_title_chapters(dvdnav, t + 1, &parts, &duration);
        if (!parts)
            break;
        free(parts);
        *(double *)arg = DVD_TIME_TO_S(duration);
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_CURRENT_TITLE: {
        int title = *((unsigned int *) arg);
        int32_t num_titles = 0;
        if (dvdnav_get_number_of_titles(priv->dvdnav, &num_titles) != DVDNAV_STATUS_OK)
            break;
        // demux_disc appends a synthetic "Disc Menu" edition at the end.
        dvdnav_status_t status;
        if (title == num_titles) {
            status = dvdnav_menu_call(priv->dvdnav, DVD_MENU_Root);
            if (status != DVDNAV_STATUS_OK)
                status = dvdnav_menu_call(priv->dvdnav, DVD_MENU_Title);
        } else {
            status = dvdnav_title_play(priv->dvdnav, title + 1);
        }
        if (status != DVDNAV_STATUS_OK)
            break;
        // This may run on the player thread; buffered data is dropped on the
        // demuxer thread when the discontinuity is processed.
        release_still(priv);
        priv->terminal_stop = false;
        priv->menu_support_known = false;
        priv->pci_valid = false;
        update_highlight(priv);
        bump_discontinuity(priv, true);
        return STREAM_OK;
    }
    case STREAM_CTRL_SEEK_TO_TIME: {
        struct stream_seek_req *req = arg;
        double d = req->target;
        int flags = req->flags;
        req->skip_until = MP_NOPTS_VALUE;
        if (in_menu_domain(dvdnav)) {
            // The menu subpicture (buttons, highlight) is a one-shot packet
            // at its cell start, and dvdnav's time search lands mid-cell,
            // past it, so the menu would show without buttons until its next
            // loop. Map seeks to program skips, every landing is a program
            // start where the display state is established.
            double cur = DVD_TIME_TO_S(dvdnav_get_current_time(dvdnav));
            MP_VERBOSE(stream, "menu seek to %f (cur %f) -> %s program\n",
                       d, cur, d >= cur ? "next" : "prev");
            dvdnav_status_t r = d >= cur ? dvdnav_next_pg_search(dvdnav)
                                         : dvdnav_prev_pg_search(dvdnav);
            if (r != DVDNAV_STATUS_OK)
                break;
        } else {
            uint32_t pos, len;
            if (dvdnav_get_position(dvdnav, &pos, &len) != DVDNAV_STATUS_OK)
                break;
            int64_t tm = seek_time_ticks(priv, d);
            MP_VERBOSE(stream, "seek to PTS %f (%"PRId64")\n", d, tm);
            if (flags & SEEK_HR) {
                // Decode forward from the verified selected-angle NAV. A
                // refused floor seek must not fall back to interpolation.
                if (dvdnav_time_search_floor(dvdnav, tm) != DVDNAV_STATUS_OK)
                    break;
            } else {
                bool jumped = false;
#if DVDNAV_VERSION >= DVDNAV_VERSION_CODE(7, 0, 0)
                jumped = dvdnav_jump_to_sector_by_time(dvdnav, tm, 0) == DVDNAV_STATUS_OK;
#endif
                if (!jumped && dvdnav_time_search(dvdnav, tm) != DVDNAV_STATUS_OK)
                    break;
            }
        }
        // The seek reinitializes everything itself. Clear any held boundary
        // and coalesce the HOP_CHANNEL libdvdnav emits for the seek.
        priv->terminal_stop = false;
        clear_still(priv);
        priv->pending_drain = false;
        priv->wait_pending = false;
        priv->drain_immediate = false;
        priv->drain_user_activation = false;
        priv->at_boundary = true;
        drop_segments(priv);
        priv->tl_anchor = true;
        stream_drop_buffers(stream);
        if (!in_menu_domain(dvdnav) && land_on_picture(stream)) {
            req->skip_until = d;
            return STREAM_OK;
        }
        d = DVD_TIME_TO_S(dvdnav_get_current_time(dvdnav));
        MP_VERBOSE(stream, "landed at: %f\n", d);
        uint32_t pos, len;
        if (dvdnav_get_position(dvdnav, &pos, &len) == DVDNAV_STATUS_OK)
            MP_VERBOSE(stream, "block: %" PRIu32 "\n", pos);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NUM_ANGLES: {
        uint32_t curr, angles;
        if (dvdnav_get_angle_info(dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        *(int *)arg = angles;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_ANGLE: {
        uint32_t curr, angles;
        if (dvdnav_get_angle_info(dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        *(int *)arg = curr;
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_ANGLE: {
        uint32_t curr, angles;
        int new_angle = *(int *)arg;
        if (dvdnav_get_angle_info(dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        if (new_angle > angles || new_angle < 1)
            break;
        if (dvdnav_angle_change(dvdnav, new_angle) != DVDNAV_STATUS_OK)
            break;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TRACK_INFO: {
        if (!dvd_track_info(stream, arg))
            break;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_DVD_INFO: {
        struct stream_dvd_info_req *req = arg;
        memset(req, 0, sizeof(*req));
        static_assert(sizeof(uint32_t) == sizeof(unsigned int), "");
        memcpy(req->palette, priv->spu_clut, sizeof(req->palette));
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_STREAMS: {
        struct stream_streams_req *req = arg;
        req->streams = NULL;
        req->num = 0;
        dvd_list_streams(stream, NULL, NULL, &req->streams, &req->num);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_SEGMENTS: {
        struct stream_segments_req *req = arg;
        req->segs = talloc_steal(NULL, priv->segs);
        req->num = priv->num_segs;
        priv->segs = NULL;
        priv->num_segs = 0;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_DISC_NAME: {
#if DVDNAV_VERSION >= DVDNAV_VERSION_CODE(7, 0, 0)
        char *volid = dvdnav_get_volid_string(dvdnav);
        if (volid && volid[0]) {
            bstr name = bstr_sanitize_utf8_latin1(NULL, bstr0(volid));
            // Sanitize adds a terminating NUL, so we can use it directly, but
            // only if conversion has happened.
            *(char **)arg = name.start == (unsigned char *)volid
                                ? talloc_strdup(NULL, volid) : (char *)name.start;
            free(volid);
            return STREAM_OK;
        }
        free(volid);
#endif
        const char *volume = NULL;
        if (dvdnav_get_title_string(dvdnav, &volume) != DVDNAV_STATUS_OK)
            break;
        if (!volume || !volume[0])
            break;
        *(char**)arg = talloc_strdup(NULL, volume);
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_ACTIVE_TRACK: {
#if DVDNAV_VERSION >= DVDNAV_VERSION_CODE(7, 0, 0)
        // The disc's own commands compare against this register, so a track
        // the user picked here has to reach it or the disc keeps acting on
        // its previous choice. Despite the header calling it a physical index,
        // dvdnav_set_active_stream() validates against pgc->audio_control,
        // which is keyed by the logical stream.
        struct stream_track_req *req = arg;
        if (req->type != STREAM_AUDIO)
            break;
        int n = dvd_audio_logical_for_id(priv, req->id);
        if (n < 0)
            break;
        if (dvdnav_set_active_stream(dvdnav, n, DVD_AUDIO_STREAM) != DVDNAV_STATUS_OK)
            break;
        MP_VERBOSE(stream, "audio register set to logical %d\n", n);
        return STREAM_OK;
#else
        break;
#endif
    }
    case STREAM_CTRL_NAV_CMD: {
        return handle_nav_cmd(stream, arg);
    }
    case STREAM_CTRL_NAV_DRAIN_ENABLE: {
        priv->drain_enabled = true;
        priv->pending_drain = false;
        priv->drain_immediate = false;
        priv->drain_user_activation = false;
        priv->at_boundary = false;
        MP_DBG(stream, "jump boundary drain enabled\n");
        return STREAM_OK;
    }
    case STREAM_CTRL_NAV_DRAIN_ACK: {
        MP_DBG(stream, "jump boundary drain acked (was %d)\n",
               priv->pending_drain);
        if (priv->wait_pending) {
            MP_VERBOSE(stream, "DVDNAV_WAIT: queued packets drained\n");
            if (dvdnav_wait_skip(dvdnav) != DVDNAV_STATUS_OK) {
                MP_ERR(stream, "failed to resume DVD wait\n");
                mark_failed(stream);
                return STREAM_ERROR;
            }
            priv->wait_pending = false;
        }
        priv->pending_drain = false;
        priv->drain_immediate = false;
        priv->drain_user_activation = false;
        return STREAM_OK;
    }
    case STREAM_CTRL_NAV_STILL_SKIP: {
        struct stream_nav_still_skip *req = arg;
        int result = STREAM_ERROR;
        if (priv->still_active && priv->still_duration > 0 &&
            priv->still_id == req->id)
        {
            if (dvdnav_still_skip(dvdnav) == DVDNAV_STATUS_OK) {
                MP_VERBOSE(stream, "finite still %u elapsed\n", req->id);
                // The slave demuxer has latched EOF. Treat the resumed VM as
                // a jump so the regular drain/resync path clears that EOF.
                priv->at_boundary = false;
                bump_discontinuity(priv, true);
                result = STREAM_OK;
            } else {
                MP_ERR(stream, "failed to skip finite still %u\n", req->id);
                mark_failed(stream);
            }
        }
        return result;
    }
    }

    return STREAM_UNSUPPORTED;
}

// The player polls these controls while the demuxer can be blocked in a DVD
// read. Return the last published state without waiting for the VM or its I/O.
static int control(stream_t *stream, int cmd, void *arg)
{
    struct priv *priv = stream->priv;
    int result = STREAM_UNSUPPORTED;
    mp_mutex_lock(&priv->lock);
    switch (cmd) {
    case STREAM_CTRL_GET_DVD_STREAMS:
        *(struct stream_dvd_streams *)arg = priv->dvd_streams;
        result = priv->dvd_streams.generation ? STREAM_OK : STREAM_UNSUPPORTED;
        break;
    case STREAM_CTRL_GET_NAV_STATE:
        *(struct stream_nav_state *)arg = priv->nav_state;
        result = STREAM_OK;
        break;
    case STREAM_CTRL_GET_NUM_TITLES:
        if (priv->num_titles >= 0) {
            *(unsigned int *)arg = priv->num_titles;
            result = STREAM_OK;
        }
        break;
    case STREAM_CTRL_GET_CURRENT_TITLE:
        if (priv->current_title >= 0) {
            *(unsigned int *)arg = priv->current_title - 1;
            result = STREAM_OK;
        }
        break;
    case STREAM_CTRL_GET_CURRENT_TIME:
        if (priv->current_time != -1) {
            *(double *)arg = DVD_TIME_TO_S(priv->current_time);
            result = STREAM_OK;
        }
        break;
    default:
        mp_mutex_unlock(&priv->lock);
        mp_mutex_lock(&priv->vm_lock);
        result = control_locked(stream, cmd, arg);
        if (result == STREAM_OK) {
            priv->dvd_selection_dirty = true;
            if (cmd == STREAM_CTRL_NAV_CMD || cmd == STREAM_CTRL_NAV_DRAIN_ACK ||
                cmd == STREAM_CTRL_NAV_STILL_SKIP || cmd == STREAM_CTRL_SET_CURRENT_TITLE ||
                cmd == STREAM_CTRL_SEEK_TO_TIME || cmd == STREAM_CTRL_SET_ANGLE)
                priv->dvd_catalog_dirty = true;
        }
        publish_state(priv);
        mp_mutex_unlock(&priv->vm_lock);
        return result;
    }
    mp_mutex_unlock(&priv->lock);
    return result;
}

static void stream_dvdnav_close(stream_t *s)
{
    struct priv *priv = s->priv;
    if (priv->dvdnav)
        dvdnav_close(priv->dvdnav);
    priv->dvdnav = NULL;
    if (priv->iso_stream)
        free_stream(priv->iso_stream);
    priv->iso_stream = NULL;
    if (priv->dvd_speed)
        dvd_set_speed(s, priv->filename, -1);
    mp_mutex_destroy(&priv->vm_lock);
    mp_mutex_destroy(&priv->lock);
}

static void dvdnav_log(void *p, dvdnav_logger_level_t level,
                       const char *fmt, va_list va)
{
    struct priv *priv = p;
    int lvl;
    switch (level) {
    case DVDNAV_LOGGER_LEVEL_ERROR: lvl = MSGL_ERR;   break;
    case DVDNAV_LOGGER_LEVEL_WARN:  lvl = MSGL_WARN;  break;
    case DVDNAV_LOGGER_LEVEL_DEBUG: lvl = MSGL_DEBUG; break;
    case DVDNAV_LOGGER_LEVEL_INFO:
    default:                        lvl = MSGL_V;     break;
    }
    if (priv->probing)
        lvl = MPMAX(lvl, MSGL_V);
    if (!mp_msg_test(priv->lib_log, lvl))
        return;
    mp_msg_va(priv->lib_log, lvl, fmt, va);
    mp_msg(priv->lib_log, lvl, "\n");
}

static dvdnav_status_t nav_open(stream_t *stream, dvdnav_t **dest, const char *path)
{
    struct priv *priv = stream->priv;
    priv->lib_log = mp_log_new(stream, stream->log, "/libdvdnav");
    const dvdnav_logger_cb logger_cb = { .pf_log = dvdnav_log };
    return dvdnav_open2(dest, priv, &logger_cb, path);
}

static void set_dvd_languages(stream_t *stream)
{
    struct priv *priv = stream->priv;
    struct MPOpts *opts = mp_get_config_group(NULL, stream->global, &mp_opt_root);

    static const struct {
        int type;
        dvdnav_status_t (*select)(dvdnav_t *, char *);
        const char *name;
    } regs[] = {
        {STREAM_AUDIO, dvdnav_audio_language_select, "audio"},
        {STREAM_SUB,   dvdnav_spu_language_select,   "subtitle"},
        // SPRM0 picks the menu language unit. Unlike the other two libdvdnav
        // reads it itself, so leaving it alone means every multi language disc
        // shows English menus. Follow the subtitle preference, menus are text.
        {STREAM_SUB,   dvdnav_menu_language_select,  "menu"},
    };

    for (int i = 0; i < MP_ARRAY_SIZE(regs); i++) {
        char **langs = opts->stream_lang[regs[i].type];
        for (int n = 0; langs && langs[n]; n++) {
            // dvdnav_*_language_select() take the first two bytes as they are,
            // so a longer code would be truncated into a bogus one.
            if (strlen(langs[n]) != 2)
                continue;
            if (regs[i].select(priv->dvdnav, langs[n]) == DVDNAV_STATUS_OK) {
                MP_VERBOSE(stream, "%s language register set to '%s'\n",
                           regs[i].name, langs[n]);
            }
            break;
        }
    }

    talloc_free(opts);
}

static struct priv *new_dvdnav_stream(stream_t *stream, char *filename)
{
    struct priv *priv = stream->priv;
    const char *title_str;

    if (!filename)
        return NULL;

    if (!(priv->filename = mp_get_user_path(priv, stream->global, filename)))
        return NULL;

    priv->dvd_speed = priv->opts->speed;
    dvd_set_speed(stream, priv->filename, priv->dvd_speed);

    if (nav_open(stream, &priv->dvdnav, priv->filename) != DVDNAV_STATUS_OK)
        return NULL;

    if (!priv->dvdnav)
        return NULL;

    dvdnav_set_readahead_flag(priv->dvdnav, 1);
    if (dvdnav_set_PGC_positioning_flag(priv->dvdnav, 1) != DVDNAV_STATUS_OK)
        MP_ERR(stream, "stream_dvdnav, failed to set PGC positioning\n");
    /* report the title?! */
    dvdnav_get_title_string(priv->dvdnav, &title_str);

    return priv;
}

static int dvdnav_stream_seek(void *handle, uint64_t pos)
{
    stream_t *source = handle;
    if (!source || pos > INT64_MAX)
        return -1;
    return stream_seek(source, (int64_t)pos) ? 0 : -1;
}

static int dvdnav_stream_read(void *handle, void *buffer, int size)
{
    stream_t *source = handle;
    if (!source || size < 0)
        return -1;
    return stream_read(source, buffer, size);
}

static dvdnav_stream_cb dvdnav_stream_callbacks = {
    .pf_seek = dvdnav_stream_seek,
    .pf_read = dvdnav_stream_read,
    .pf_readv = NULL,
};

extern const stream_info_t stream_info_ffmpeg;
extern const stream_info_t stream_info_cb;

static int open_dvdnav_iso_stream(stream_t *stream, const char *url)
{
    struct priv *priv = stream->priv;
    bool media3_source = strncmp(url, "media3iso://", 12) == 0;
    struct stream_open_args args = {
        .global = stream->global,
        .cancel = stream->cancel,
        .url = url,
        .flags = STREAM_READ | (stream->stream_origin & STREAM_ORIGIN_MASK),
        .sinfo = media3_source ? &stream_info_cb : &stream_info_ffmpeg,
    };
    if (stream_create_with_args(&args, &priv->iso_stream) != STREAM_OK ||
        !priv->iso_stream)
        return STREAM_ERROR;
    if (media3_source) {
        priv->iso_stream->is_network = true;
        priv->iso_stream->streaming = true;
    }

    if (!priv->iso_stream->seekable) {
        MP_ERR(stream, "DVD ISO stream must be seekable.\n");
        return STREAM_UNSUPPORTED;
    }

    if (dvdnav_open_stream(&priv->dvdnav, priv->iso_stream,
                           &dvdnav_stream_callbacks) != DVDNAV_STATUS_OK)
        return STREAM_UNSUPPORTED;

    if (!priv->dvdnav)
        return STREAM_UNSUPPORTED;

    stream->is_network = priv->iso_stream->is_network;
    stream->streaming = priv->iso_stream->streaming;
    dvdnav_set_readahead_flag(priv->dvdnav, 1);
    if (dvdnav_set_PGC_positioning_flag(priv->dvdnav, 1) != DVDNAV_STATUS_OK)
        MP_ERR(stream, "stream_dvdnav, failed to set PGC positioning\n");

    return STREAM_OK;
}

static int open_s_internal(stream_t *stream)
{
    struct priv *priv, *p;
    priv = p = stream->priv;
    char *filename;
    int ret = 0;

    mp_mutex_init(&priv->vm_lock);
    mp_mutex_init(&priv->lock);

    priv->log = stream->log;
    priv->audio_physical = -1;
    priv->audio_logical = -1;
    priv->sub_physical = -1;
    priv->sub_visible = false;
    priv->tl_anchor = true;

    p->opts = mp_get_config_group(stream, stream->global, &dvd_conf);

    if (!priv->dvdnav) {
        if (p->device && p->device[0])
            filename = p->device;
        else if (p->opts->device && p->opts->device[0])
            filename = p->opts->device;
        else
            filename = DEFAULT_OPTICAL_DEVICE;
        if (!new_dvdnav_stream(stream, filename)) {
            if (!priv->probing)
                MP_ERR(stream, "Couldn't open DVD device: %s\n", filename);
            ret = priv->probing ? STREAM_UNSUPPORTED : STREAM_ERROR;
            goto err;
        }
    }
    priv->probing = false;

    int32_t region_mask = 0;
    if (p->opts->region) {
        region_mask = 1 << (p->opts->region - 1);
    } else {
#if DVDNAV_VERSION >= DVDNAV_VERSION_CODE(7, 0, 0)
        if (dvdnav_get_disk_region_mask(priv->dvdnav, &region_mask) != DVDNAV_STATUS_OK)
            region_mask = 0;
        // The disc reports every region it allows, while a player holds one.
        // Claim the lowest of them, so the disc's own region checks pass.
        region_mask &= -region_mask;
#endif
        if (!region_mask)
            region_mask = 0xff; // Nothing to go on, claim region free.
    }
    if (dvdnav_set_region_mask(priv->dvdnav, region_mask) == DVDNAV_STATUS_OK) {
        MP_VERBOSE(stream, "player region mask set to 0x%02x\n", region_mask);
    } else {
        MP_WARN(stream, "Couldn't set the player region mask.\n");
    }

    set_dvd_languages(stream);

    int32_t num_titles = 0;
    priv->num_titles = -1;
    if (dvdnav_get_number_of_titles(priv->dvdnav, &num_titles) == DVDNAV_STATUS_OK)
        priv->num_titles = num_titles;

    if (p->track == TITLE_LONGEST) { // longest
        dvdnav_t *dvdnav = priv->dvdnav;
        uint64_t best_length = 0;
        int best_title = -1;
        MP_VERBOSE(stream, "List of available titles:\n");
        for (int n = 1; n <= num_titles; n++) {
            uint64_t *parts = NULL, duration = 0;
            dvdnav_describe_title_chapters(dvdnav, n, &parts, &duration);
            if (parts) {
                if (duration > best_length) {
                    best_length = duration;
                    best_title = n;
                }
                if (duration > 90000) { // arbitrarily ignore <1s titles
                    char *time = mp_format_time(duration / 90000, false);
                    MP_VERBOSE(stream, "title: %3d duration: %s\n",
                               n - 1, time);
                    talloc_free(time);
                }
                free(parts);
            }
        }
        p->track = best_title - 1;
        MP_INFO(stream, "Selecting title %d.\n", p->track);
    }

    // demux_disc.c appends a synthetic "Disc Menu" edition at index num_titles.
    if (p->track >= num_titles)
        p->track = TITLE_MENU;

    if (p->track >= 0) {
        if (dvdnav_title_play(priv->dvdnav, p->track + 1) != DVDNAV_STATUS_OK) {
            MP_FATAL(stream, "couldn't select title %d, error '%s'\n",
                   p->track, dvdnav_err_to_string(priv->dvdnav));
            ret = STREAM_UNSUPPORTED;
            goto err;
        }
    } else {
        if (dvdnav_menu_call(priv->dvdnav, DVD_MENU_Root) != DVDNAV_STATUS_OK)
            dvdnav_menu_call(priv->dvdnav, DVD_MENU_Title);
    }
    if (p->opts->angle > 1)
        dvdnav_angle_change(priv->dvdnav, p->opts->angle);

    publish_state(priv);

    stream->fill_buffer = fill_buffer;
    stream->control = control;
    stream->close = stream_dvdnav_close;
    stream->demuxer = "+disc";
    stream->lavf_type = "mpeg";

    return STREAM_OK;

err:
    stream_dvdnav_close(stream);
    return ret;
}

static int open_s(stream_t *stream)
{
    struct priv *priv = talloc_zero(stream, struct priv);
    stream->priv = priv;

    bstr title, bdevice;
    bstr_split_tok(bstr0(stream->path), "/", &title, &bdevice);

    struct MPOpts *opts = mp_get_config_group(stream, stream->global, &mp_opt_root);
    int edition_id = opts->edition_id;
    bool disc_menu = opts->disc_menu;
    talloc_free(opts);

    priv->track = disc_menu ? TITLE_MENU : TITLE_LONGEST;

    if (edition_id >= 0) {
        priv->track = edition_id;
    } else if (bstr_equals0(title, "longest") || bstr_equals0(title, "first")) {
        priv->track = TITLE_LONGEST;
    } else if (bstr_equals0(title, "menu")) {
        priv->track = TITLE_MENU;
    } else if (title.len) {
        bstr rest;
        priv->track = bstrtoll(title, &rest, 10);
        if (rest.len) {
            MP_ERR(stream, "number expected: '%.*s'\n", BSTR_P(rest));
            return STREAM_ERROR;
        }
    }

    priv->device = bstrto0(priv, bdevice);

    return open_s_internal(stream);
}

const stream_info_t stream_info_dvdnav = {
    .name = "dvdnav",
    .open = open_s,
    .protocols = (const char*const[]){ "dvd", "dvdnav", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};

int stream_open_dvd_iso(stream_t *stream, const char *url, bool local_path)
{
    struct priv *priv = talloc_zero(stream, struct priv);
    stream->priv = priv;
    priv->probing = true;

    struct MPOpts *opts = mp_get_config_group(stream, stream->global, &mp_opt_root);
    priv->track = opts->edition_id >= 0 ? opts->edition_id :
                  (opts->disc_menu ? TITLE_MENU : TITLE_LONGEST);
    talloc_free(opts);

    if (local_path) {
        priv->device = talloc_strdup(priv, url);
    } else {
        int r = open_dvdnav_iso_stream(stream, url);
        if (r != STREAM_OK) {
            if (priv->dvdnav)
                dvdnav_close(priv->dvdnav);
            if (priv->iso_stream)
                free_stream(priv->iso_stream);
            talloc_free(priv);
            stream->priv = NULL;
            return r;
        }
    }

    int r = open_s_internal(stream);
    if (r != STREAM_OK) {
        talloc_free(priv);
        stream->priv = NULL;
    } else {
        stream->info = &stream_info_dvdnav;
    }
    return r;
}

static bool check_ifo(const char *path)
{
    if (strcasecmp(mp_basename(path), "video_ts.ifo"))
        return false;

    return dvd_probe(path, ".ifo", "DVDVIDEO-VMG");
}

static int ifo_dvdnav_stream_open(stream_t *stream)
{
    struct priv *priv = talloc_zero(stream, struct priv);
    stream->priv = priv;

    if (!stream->access_references)
        goto unsupported;

    struct MPOpts *opts = mp_get_config_group(NULL, stream->global, &mp_opt_root);
    priv->track = opts->edition_id >= 0 ? opts->edition_id :
                  (opts->disc_menu ? TITLE_MENU : TITLE_LONGEST);
    talloc_free(opts);

    char *path = mp_file_get_path(priv, bstr0(stream->url));
    if (!path)
        goto unsupported;

    // Hand the .iso to libdvdnav as the device. Opening validates the VMG
    // IFO, so it doubles as the probe (see priv->probing).
    if (bstr_case_endswith(bstr0(path), bstr0(".iso"))) {
        priv->device = talloc_strdup(priv, path);
        priv->probing = stream->autoprobed;
        int r = open_s_internal(stream);
        if (r != STREAM_OK) {
            if (priv->probing)
                goto unsupported;
            return r;
        }
        MP_INFO(stream, "DVD ISO image detected. Redirecting to dvd://\n");
        return r;
    }

    // We allow the path to point to a directory containing VIDEO_TS/, a
    // directory containing VIDEO_TS.IFO, or that file itself.
    if (!check_ifo(path)) {
        // On UNIX, just assume the filename is always uppercase.
        char *npath = mp_path_join(priv, path, "VIDEO_TS.IFO");
        if (!check_ifo(npath)) {
            npath = mp_path_join(priv, path, "VIDEO_TS/VIDEO_TS.IFO");
            if (!check_ifo(npath))
                goto unsupported;
        }
        path = npath;
    }

    priv->device = bstrto0(priv, mp_dirname(path));

    MP_INFO(stream, ".IFO detected. Redirecting to dvd://\n");
    return open_s_internal(stream);

unsupported:
    talloc_free(priv);
    stream->priv = NULL;
    return STREAM_UNSUPPORTED;
}

const stream_info_t stream_info_ifo_dvdnav = {
    .name = "ifo_dvdnav",
    .open = ifo_dvdnav_stream_open,
    .protocols = (const char*const[]){ "file", "", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};
