#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define STREAM_VIDEO 0
#define STREAM_AUDIO 1
#define STREAM_SUB 2
#define STREAM_OK 1
#define STREAM_CTRL_GET_STREAMS 1
#define STREAM_CTRL_GET_NAV_STATE 2
#define STREAM_CTRL_GET_DVD_STREAMS 3
#define STREAM_CTRL_GET_CURRENT_TITLE 4
#define MP_NOPTS_VALUE -1e20
#define MAX_DVD_SPU_STREAMS 32
#define MP_VERBOSE(ctx, ...) ((void)(ctx))
#define MP_TRACE(ctx, ...) ((void)(ctx))
#define MP_DBG(ctx, ...) ((void)(ctx))
#define atomic_load(p) (*(p))
#define atomic_store(p, v) (*(p) = (v))
#define talloc_zero(ctx, type) ((type *)calloc(1, sizeof(type)))
#define talloc_free free
#define TA_FREEP(p) do { free(*(p)); *(p) = NULL; } while (0)
#define MP_TARRAY_GROW(ctx, a, i) ((a) = realloc((a), ((i) + 1) * sizeof(*(a))))
#define MP_TARRAY_APPEND(ctx, a, n, ...) do { \
    MP_TARRAY_GROW(ctx, a, n); (a)[(n)++] = (__VA_ARGS__); \
} while (0)

enum track_flags {
    TRACK_FORCED = 1, TRACK_VISUAL_IMPAIRED = 2,
    TRACK_HEARING_IMPAIRED = 4, TRACK_COMMENTARY = 8,
};
struct mp_codec_params { const char *codec; };
struct sh_stream_group { struct sh_stream **members; int num_members; };
struct sh_stream {
    int type, index, demuxer_id, dvd_nav_logical, dvd_nav_id;
    bool dvd_nav_stream, forced_track, visual_impaired_track;
    bool hearing_impaired_track, commentary_track, default_track;
    bool dependent_track, selected, absent;
    uint64_t dvd_nav_generation;
    char *lang;
    struct mp_codec_params *codec;
    struct sh_stream_group *group;
};
struct demux_packet { int stream; int64_t pos; double pts, dts; };
struct demuxer {
    void *priv, *packet_pool, *stream;
    int edition, num_editions;
};
struct stream_nav_state {
    int active_audio_id;
    bool nav_active, menu_active;
};

#include "disc_segment_types.h"

static struct stream_dvd_streams catalog;
static struct sh_stream *inputs[4];
static int num_inputs, registered;
static struct stream_decl_stream declared[4];
static int num_declared;
static int copies;
static int current_title;
static struct stream_nav_state nav = {.active_audio_id = 0x80};

static char *talloc_strdup(void *ctx, const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static struct sh_stream *demux_alloc_sh_stream(int type)
{
    struct sh_stream *sh = calloc(1, sizeof(*sh));
    sh->type = type;
    sh->codec = calloc(1, sizeof(*sh->codec));
    return sh;
}

static void adopt_codec_params(struct sh_stream *dst, struct sh_stream *src)
{
    *dst->codec = *src->codec;
}

static void demux_add_sh_stream(struct demuxer *d, struct sh_stream *sh)
{
    sh->index = registered++;
}

static int demux_get_num_stream(struct demuxer *d) { return num_inputs; }
static struct sh_stream *demux_get_stream(struct demuxer *d, int n) { return inputs[n]; }
static bool demux_stream_is_selected(struct sh_stream *sh) { return sh->selected; }
static void demux_set_stream_absent(struct demuxer *d, struct sh_stream *sh, bool absent)
{
    sh->absent = absent;
}
static bool bd_stream_pid_valid(struct sh_stream *sh) { return true; }
static void set_disc_aspect(struct demuxer *d, struct sh_stream *sh) {}
static void get_disc_track_info(void *stream, struct sh_stream *sh, bool dvd) {}
static void set_dvd_sub_palette(struct demuxer *d, struct sh_stream *sh) {}
static void demux_set_stream_group(struct demuxer *d, struct sh_stream_group *group) {}
static const char *stream_type_name(int type) { return "test"; }
static void demuxer_select_track(struct demuxer *d, struct sh_stream *sh,
                                double pts, bool selected)
{
    sh->selected = selected;
}

static struct demux_packet *demux_copy_packet(void *pool, struct demux_packet *pkt)
{
    struct demux_packet *copy = malloc(sizeof(*copy));
    *copy = *pkt;
    copies++;
    return copy;
}

static int stream_control(void *stream, int cmd, void *arg)
{
    if (cmd == STREAM_CTRL_GET_DVD_STREAMS) {
        *(struct stream_dvd_streams *)arg = catalog;
    } else if (cmd == STREAM_CTRL_GET_NAV_STATE) {
        *(struct stream_nav_state *)arg = nav;
    } else if (cmd == STREAM_CTRL_GET_CURRENT_TITLE) {
        if (current_title < 0)
            return -1;
        *(unsigned *)arg = current_title;
    } else {
        assert(cmd == STREAM_CTRL_GET_STREAMS);
        struct stream_streams_req *req = arg;
        req->num = num_declared;
        req->streams = malloc(sizeof(declared));
        memcpy(req->streams, declared, sizeof(declared));
    }
    return STREAM_OK;
}

#include "disc_segments_functions.h"

static void test_alias_segments(void)
{
    struct priv p = {.is_dvd = true, .applied_seg = -1};
    struct demuxer demuxer = {.priv = &p};
    struct mp_codec_params audio_codec = {.codec = "ac3"};
    struct mp_codec_params sub_codec = {.codec = "dvd_subtitle"};
    struct sh_stream audio = {.type = STREAM_AUDIO, .demuxer_id = 0x80, .codec = &audio_codec};
    struct sh_stream sub = {.type = STREAM_SUB, .demuxer_id = 0x20, .codec = &sub_codec, .index = 1};
    inputs[0] = &audio;
    inputs[1] = &sub;
    num_inputs = 2;
    catalog.generation = 1;
    for (int n = 0; n < 8; n++)
        catalog.audio[n].id = -1;
    for (int n = 0; n < 32; n++)
        catalog.sub[n].id = -1;
    catalog.audio[0].id = catalog.audio[6].id = 0x80;
    catalog.audio[6].flags = TRACK_COMMENTARY;
    catalog.sub[2].id = catalog.sub[7].id = 0x20;
    catalog.sub[7].flags = TRACK_FORCED;
    declared[0] = (struct stream_decl_stream){STREAM_AUDIO, 0x80, "ac3"};
    declared[1] = (struct stream_decl_stream){STREAM_SUB, 0x20, "dvd_subtitle"};
    num_declared = 2;
    sync_streams(&demuxer);
    assert(p.num_outer_streams == 4 && p.num_decl == 4);
    assert(p.dvd_audio[0] != p.dvd_audio[6]);
    assert(p.dvd_audio[6]->commentary_track && p.dvd_subs[7]->forced_track);
    p.dvd_audio[6]->selected = p.dvd_subs[7]->selected = true;
    assert(selected_dvd_audio(&p, &audio) == p.dvd_audio[6]);
    assert(audio.selected && sub.selected);

    struct stream_stream_id present[] = {{STREAM_AUDIO, 0x80}, {STREAM_SUB, 0x20}};
    p.segs = calloc(2, sizeof(*p.segs));
    p.num_segs = 2;
    p.segs[0] = (struct stream_segment){.pos = 100, .raw_base = 100, .time = 2,
                                      .present = present, .num_present = 2};
    p.segs[1] = (struct stream_segment){.pos = 200, .raw_base = 2000, .time = 10,
                                      .present = present, .num_present = 2};
    apply_presence(&demuxer, 0);
    assert(!p.dvd_audio[0]->absent && !p.dvd_audio[6]->absent);
    assert(!p.dvd_subs[2]->absent && !p.dvd_subs[7]->absent);
    present[0].id = 0x81;
    apply_presence(&demuxer, 0);
    assert(p.dvd_audio[0]->absent && p.dvd_audio[6]->absent);
    present[0].id = 0x80;

    struct demux_packet packet = {.pos = 150, .pts = 101, .dts = 100};
    map_packet(&demuxer, p.dvd_audio[6], &packet);
    assert(packet.pts == 3 && packet.dts == 2);
    assert(get_ostate(&p, p.dvd_audio[6])->seg == 0);
    packet = (struct demux_packet){.pos = 210, .pts = 2002, .dts = 2001};
    map_packet(&demuxer, p.dvd_audio[6], &packet);
    assert(packet.pts == 12 && packet.dts == 11);
    packet = (struct demux_packet){.pos = -1, .pts = 2003, .dts = 2002};
    map_packet(&demuxer, p.dvd_audio[6], &packet);
    assert(packet.pts == 13 && packet.dts == 12);

    struct demux_packet *sp = malloc(sizeof(*sp));
    *sp = (struct demux_packet){.pos = 150, .pts = 101, .dts = MP_NOPTS_VALUE};
    map_packet(&demuxer, p.dvd_subs[2], sp);
    assert(sp->pts == 3);
    queue_dvd_sub_aliases(&demuxer, &sub, sp, get_ostate(&p, p.dvd_subs[2])->seg);
    assert(p.num_pending_subs == 2);
    struct demux_packet *out = NULL;
    for (int n = 0; n < p.num_pending_subs; n++) {
        struct pending_sub ps = p.pending_subs[n];
        assert(ps.pkt->pts == 3);
        assert(deliver_dvd_sub(&demuxer, ps.sh, ps.pkt, &out));
    }
    p.num_pending_subs = 0;
    assert(out && out->stream == p.dvd_subs[7]->index && out->pts == 3);
    assert(p.dvd_sub_hold[2].pkt->pts == 3 && p.dvd_sub_hold[7].pkt->pts == 3);
    free(out);
    arm_dvd_sub_holds(&p);
    assert(!p.dvd_sub_hold[2].pending && p.dvd_sub_hold[7].pending);

    struct sh_stream *old = p.dvd_audio[0];
    struct sh_stream *selected = p.dvd_audio[6];
    catalog.generation++;
    catalog.audio[0].id = -1;
    sync_streams(&demuxer);
    assert(!p.dvd_audio[0] && p.dvd_audio[6] == selected);
    assert(!old->dvd_nav_generation && old->absent && !selected->absent);
    assert(p.num_decl == 3);
    clear_dvd_sub_holds(&p);
    reset_segments(&p);
    assert(p.applied_seg == -1 && !p.num_segs && !p.segs);
    for (int n = 0; n < p.num_ostate; n++)
        assert(p.ostate[n].seg == -1);
    for (int n = 0; n < p.num_outer_streams; n++) {
        free(p.outer_streams[n]->lang);
        free(p.outer_streams[n]->codec);
        free(p.outer_streams[n]);
    }
    free(p.pending_subs);
    free(p.outer_streams);
    free(p.slave_to_outer);
    free(p.ostate);
    free(p.decl);
}

static void test_initial_edition(void)
{
    struct priv p = {.num_title_editions = 2, .has_menu_edition = true};
    struct demuxer demuxer = {.priv = &p, .edition = -1, .num_editions = 3};
    nav = (struct stream_nav_state){.nav_active = true};
    current_title = 1;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == 1);

    nav.menu_active = true;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == 2);

    demuxer.num_editions = 2;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == 1);

    current_title = 2;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == 1);

    demuxer.edition = 2;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == 0);

    current_title = -1;
    demuxer.num_editions = 3;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == 2);

    p.has_menu_edition = false;
    demuxer.num_editions = 2;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == 0);

    demuxer.num_editions = 0;
    sync_initial_edition(&demuxer);
    assert(demuxer.edition == -1);
}

int main(void)
{
    test_alias_segments();
    printf("Disc logical aliases, segment presence and mapped subtitle replay passed (%d copies).\n", copies);
    test_initial_edition();
    puts("Disc title and menu editions passed.");
    return 0;
}
