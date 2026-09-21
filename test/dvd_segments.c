#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define DVD_AUDIO_STREAM 1
#define DVD_TIMEBASE 90000
#define DVD_TIME_TO_S(t) ((t) / (double)DVD_TIMEBASE)
#define DVDNAV_STATUS_OK 1
#define DVDNAV_NAV_PACKET 2
#define DVDNAV_NOP 0
#define STREAM_CTRL_GET_SEGMENTS 3
#define STREAM_OK 1
#define SRI_END_OF_CELL 0x3fffffffU
#define MP_VERBOSE(ctx, ...) ((void)(ctx))
#define MP_DBG(ctx, ...) ((void)(ctx))
#define TA_FREEP(p) do { free(*(p)); *(p) = NULL; } while (0)
#define talloc_free free
#define talloc_steal(ctx, p) (p)
#define MP_TARRAY_APPEND(ctx, a, n, ...) do { \
    (a) = realloc((a), ((n) + 1) * sizeof(*(a))); (a)[(n)++] = (__VA_ARGS__); \
} while (0)

typedef struct { int logical; int physical[8]; } dvdnav_t;
typedef struct {
    struct { uint32_t vobu_ea, vobu_1stref_ea; } dsi_gi;
    struct { uint32_t prev_video; } vobu_sri;
} dsi_t;
typedef struct { struct { uint32_t vobu_s_ptm, vobu_e_ptm; } pci_gi; } pci_t;
struct stream_stream_id { int type, id; };
struct stream_decl_stream { int type, id; const char *codec; };
struct stream_segment {
    int64_t pos;
    double raw_base, time;
    struct stream_stream_id *present;
    int num_present;
};
struct stream_segments_req { struct stream_segment *segs; int num; };
struct priv {
    dvdnav_t *dvdnav;
    struct stream_segment *segs;
    int num_segs;
    int audio_physical, audio_logical, sub_physical;
    uint32_t nav_change_id, discontinuity_id;
    bool sub_visible, still_active, nav_pts_valid, tl_anchor, cell_start;
    bool at_boundary, drain_enabled, pending_drain, drain_immediate;
    bool drain_user_activation, pci_valid;
    double cell_raw_base, cell_time, cell_raw_end;
};
typedef struct { struct priv *priv; int64_t pos; } stream_t;
static bool vm_owned = true, publication_locked, menu;
static dsi_t fixture_dsi;
static pci_t fixture_pci;
static int64_t vm_time, sector_offset;
static int sector_calls;
static int dvdnav_is_domain_vts(dvdnav_t *nav) { assert(vm_owned && !publication_locked); return !menu; }
static int dvdnav_get_audio_logical_stream(dvdnav_t *nav, int logical)
{
    assert(vm_owned && !publication_locked);
    return nav->physical[logical];
}
static int dvdnav_get_active_logical_stream(dvdnav_t *nav, int type)
{
    assert(vm_owned && !publication_locked);
    return nav->logical;
}
static int dvdnav_get_active_audio_stream(dvdnav_t *nav) { return nav->physical[nav->logical]; }
static int dvdnav_get_active_spu_stream(dvdnav_t *nav) { return 0x80 | 3; }
static const dsi_t *dvdnav_get_current_nav_dsi(dvdnav_t *nav) { return &fixture_dsi; }
static int64_t dvdnav_get_current_time(dvdnav_t *nav)
{
    assert(vm_owned && !publication_locked);
    return vm_time;
}
static void clear_still(struct priv *p) { p->still_active = false; }
static void dvd_list_streams(stream_t *s, const dsi_t *nav, void *ta,
                             struct stream_decl_stream **list, int *num)
{
    assert(vm_owned && !publication_locked);
    *list = malloc(sizeof(**list));
    **list = (struct stream_decl_stream){1, 0x80, "ac3"};
    *num = 1;
}
static int dvdnav_get_next_block(dvdnav_t *nav, void *buf, int *event, int *len)
{
    assert(vm_owned && !publication_locked);
    *event = DVDNAV_NAV_PACKET;
    *len = 2048;
    return DVDNAV_STATUS_OK;
}
static int dvdnav_sector_search(dvdnav_t *nav, int64_t offset, int origin)
{
    assert(vm_owned && !publication_locked && origin == SEEK_CUR);
    sector_offset = offset;
    sector_calls++;
    return DVDNAV_STATUS_OK;
}
static const char *dvdnav_err_to_string(dvdnav_t *nav) { return "test"; }

#include "dvd_segments_functions.h"

static int process_event(stream_t *s, int event, void *buf, int len)
{
    assert(event == DVDNAV_NAV_PACKET);
    record_vobu(s, &fixture_pci);
    return -1;
}

int main(void)
{
    dvdnav_t nav = {.logical = 6};
    for (int n = 0; n < 8; n++)
        nav.physical[n] = -1;
    nav.physical[0] = nav.physical[6] = 0;
    nav.physical[3] = 1;
    struct priv p = {.dvdnav = &nav, .audio_logical = 0, .drain_enabled = true};
    stream_t stream = {.priv = &p};
    bump_discontinuity(&p, true);
    assert(p.audio_logical == 6 && p.audio_physical == 0);
    assert(p.sub_physical == 3 && !p.sub_visible);
    assert(p.pending_drain && p.drain_immediate && p.tl_anchor);
    assert(p.discontinuity_id == 1);
    nav.logical = 3;
    bump_discontinuity(&p, false);
    assert(p.audio_logical == 3 && p.audio_physical == 1);
    assert(p.discontinuity_id == 1);
    fixture_dsi.dsi_gi.vobu_ea = 3;
    vm_time = 10 * DVD_TIMEBASE;
    fixture_pci.pci_gi.vobu_s_ptm = DVD_TIMEBASE;
    fixture_pci.pci_gi.vobu_e_ptm = 2 * DVD_TIMEBASE;
    record_vobu(&stream, &fixture_pci);
    assert(p.num_segs == 1 && p.segs[0].time == 10 && p.segs[0].raw_base == 1);
    assert(p.segs[0].present[0].id == 0x80);
    p.cell_start = true;
    stream.pos = 2048;
    fixture_pci.pci_gi.vobu_s_ptm = 0;
    fixture_pci.pci_gi.vobu_e_ptm = DVD_TIMEBASE;
    record_vobu(&stream, &fixture_pci);
    assert(p.num_segs == 2 && p.segs[1].time == 11 && p.segs[1].raw_base == 0);
    struct stream_segments_req req = {0};
    struct stream_segment *owned = p.segs;
    assert(take_segments(&stream, &req) == STREAM_OK);
    assert(req.segs == owned && req.num == 2 && !p.segs && !p.num_segs);
    for (int n = 0; n < req.num; n++)
        free(req.segs[n].present);
    free(req.segs);
    p.at_boundary = false;
    vm_time = 300 * DVD_TIMEBASE;
    bump_discontinuity(&p, true);
    record_vobu(&stream, &fixture_pci);
    assert(p.segs[0].time == 300 && p.discontinuity_id == 2);
    free(p.segs[0].present);
    drop_segments(&p);
    fixture_dsi.vobu_sri.prev_video = 11;
    p.pci_valid = true;
    assert(land_on_picture(&stream));
    assert(sector_calls == 1 && sector_offset == -11);
    assert(!p.num_segs && !p.segs && p.tl_anchor && !p.pci_valid);
    fixture_dsi.dsi_gi.vobu_1stref_ea = 1;
    assert(!land_on_picture(&stream));
    assert(sector_calls == 1 && p.num_segs == 1);
    free(p.segs[0].present);
    drop_segments(&p);
    puts("DVD VM selection, cell segments, ownership transfer and picture landing passed.");
    return 0;
}
