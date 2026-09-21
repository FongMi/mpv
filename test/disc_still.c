/* Execute the complete disc read owner with a latched slave EOF. Native file
 * operations and stream event publication are the scripted I/O boundary. */
#include <assert.h>
#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define STREAM_CTRL_GET_NAV_STATE 1
#define STREAM_CTRL_GET_DVD_STREAMS 2
#define STREAM_VIDEO 0
#define STREAM_AUDIO 1
#define STREAM_SUB 2
#define MAX_DVD_SPU_STREAMS 1
#define MP_NOPTS_VALUE (-DBL_MAX)
#define MP_VERBOSE(...) ((void)0)
#define MP_DBG(...) ((void)0)
#define MP_TRACE(...) ((void)0)
#define MP_TARRAY_REMOVE_AT(array, count, index) \
    do { (void)(array); assert((index) == 0); (count)--; } while (0)

struct bluray_priv_s {
    bool still_active, failed;
    int still_duration;
    uint32_t still_id, discontinuity_id;
};
static void hold_still(struct bluray_priv_s *b, int duration);
static void clear_still(struct bluray_priv_s *b);
struct stream_nav_state {
    bool nav_active, menu_active, still_active, drain_pending, failed;
    uint32_t discontinuity_id;
};
enum read_result { READ_OPEN_ERROR, READ_SEEK_ERROR, READ_STILL, READ_LEFT_STILL, READ_EOF };
typedef struct {
    struct bluray_priv_s native;
    bool nav_active;
    enum read_result result;
    int peeks;
} stream_t;
struct sh_stream { bool selected; int index, type; void *codec; };
struct demux_packet { int stream; double pts, dts; bool segmented; void *codec; };
struct stream_dvd_streams { uint64_t generation; };
struct out_state { void *seg; };
typedef struct demuxer {
    void *priv, *packet_pool;
    stream_t *stream;
    bool failed;
    struct demux_packet *packet;
    int reads;
} demuxer_t;
struct priv {
    struct demux_packet *pending_pkt;
    demuxer_t *slave;
    bool nav_active, is_bd, is_dvd, is_cdda, reopen_pending, seek_reinit;
    uint32_t last_discontinuity_id, eof_log_id;
    bool eof_log_still;
    struct dvd_sub_hold {
        bool pending;
        struct demux_packet *pkt;
        struct sh_stream *sh;
    } dvd_sub_hold[MAX_DVD_SPU_STREAMS];
    struct pending_sub {
        struct demux_packet *pkt;
        struct sh_stream *sh;
    } pending_subs[1];
    int num_pending_subs, slave_to_outer_count;
    struct sh_stream **slave_to_outer;
    struct stream_dvd_streams dvd_streams;
    double skip_until;
    struct out_state output;
    int discontinuities;
};

static int stream_control(stream_t *s, int command, void *arg)
{
    if (command != STREAM_CTRL_GET_NAV_STATE)
        return -1;
    *(struct stream_nav_state *)arg = (struct stream_nav_state){
        .nav_active = s->nav_active, .still_active = s->native.still_active,
        .failed = s->native.failed, .discontinuity_id = s->native.discontinuity_id,
    };
    return 1;
}
static int stream_read_peek(stream_t *s, void *buffer, int length)
{
    (void)buffer;
    assert(length == 1);
    s->peeks++;
    switch (s->result) {
    case READ_OPEN_ERROR:
    case READ_SEEK_ERROR:
        // A failed next-clip open leaves libbluray without an input file.
        // Its next read is negative; the stream publishes failure and clears
        // the hold before returning it to this demux owner.
        s->native.failed = true;
        clear_still(&s->native);
        return -1;
    case READ_STILL:
        hold_still(&s->native, 3);
        return 0;
    case READ_LEFT_STILL:
        clear_still(&s->native);
        s->native.discontinuity_id++;
        return 0;
    case READ_EOF:
        return 0;
    }
    assert(false);
    return -1;
}
static void demux_set_failed(demuxer_t *d) { d->failed = true; }
static void demux_set_nav_active(demuxer_t *d, bool active) { (void)d; (void)active; }
static void process_discontinuity(demuxer_t *d, uint32_t id)
{
    struct priv *p = d->priv;
    p->last_discontinuity_id = id;
    p->discontinuities++;
}
static bool reopen_slave(demuxer_t *d) { (void)d; return true; }
static bool demux_stream_is_selected(struct sh_stream *sh) { return sh->selected; }
static struct demux_packet *demux_copy_packet(void *pool, struct demux_packet *pkt)
{ (void)pool; return pkt; }
static bool deliver_dvd_sub(demuxer_t *d, struct sh_stream *sh,
                            struct demux_packet *pkt, struct demux_packet **out)
{ (void)d; (void)sh; *out = pkt; return true; }
static struct demux_packet *demux_read_any_packet(demuxer_t *d, bool *new_streams)
{
    if (new_streams)
        *new_streams = false;
    d->reads++;
    struct demux_packet *pkt = d->packet;
    d->packet = NULL;
    return pkt;
}
static void pull_segments(demuxer_t *d) { (void)d; }
static void sync_streams(demuxer_t *d) { (void)d; }
static void demux_update(demuxer_t *d, double pts) { (void)d; (void)pts; }
static void refresh_disc_metadata(demuxer_t *d) { (void)d; }
static int demux_get_num_stream(demuxer_t *d) { (void)d; return 1; }
static void talloc_free(void *p) { (void)p; }
static void map_packet(demuxer_t *d, struct sh_stream *sh, struct demux_packet *pkt)
{ (void)d; (void)sh; (void)pkt; }
static struct sh_stream *demux_get_stream(demuxer_t *d, int index)
{ (void)d; (void)index; assert(false); return NULL; }
static struct out_state *get_ostate(struct priv *p, struct sh_stream *sh)
{ (void)sh; return &p->output; }
static void queue_dvd_sub_aliases(demuxer_t *d, struct sh_stream *sh,
                                 struct demux_packet *pkt, void *seg)
{ (void)d; (void)sh; (void)pkt; (void)seg; assert(false); }
static struct sh_stream *selected_dvd_audio(struct priv *p, struct sh_stream *src)
{ (void)p; return src; }
static void inject_still(demuxer_t *d, double pts) { (void)d; (void)pts; }

#include "disc_still_functions.h"

struct fixture {
    stream_t stream;
    struct priv priv;
    demuxer_t demux, slave;
    struct sh_stream output;
    struct sh_stream *mapping[1];
};
static void reset(struct fixture *f, bool interactive, bool still, enum read_result result)
{
    *f = (struct fixture){0};
    f->stream = (stream_t){.nav_active = interactive, .result = result};
    if (still)
        hold_still(&f->stream.native, 3);
    f->output = (struct sh_stream){.selected = true, .index = 0, .type = STREAM_VIDEO};
    f->mapping[0] = &f->output;
    f->priv = (struct priv){.is_bd = true, .slave = &f->slave,
        .slave_to_outer = f->mapping, .slave_to_outer_count = 1,
        .skip_until = MP_NOPTS_VALUE};
    f->demux = (demuxer_t){.priv = &f->priv, .stream = &f->stream};
}
static bool read_packet(struct fixture *f, struct demux_packet **pkt)
{
    *pkt = NULL;
    return d_read_packet(&f->demux, pkt);
}

int main(void)
{
    struct fixture f;
    struct demux_packet *pkt;
    for (int interactive = 0; interactive < 2; interactive++) {
        for (int error = READ_OPEN_ERROR; error <= READ_SEEK_ERROR; error++) {
            reset(&f, interactive != 0, true, error);
            assert(!read_packet(&f, &pkt));
            assert(f.stream.peeks == 1 && f.demux.failed && f.stream.native.failed);
            assert(!f.stream.native.still_active && pkt == NULL);
            assert(!read_packet(&f, &pkt) && f.stream.peeks == 1);
        }
    }
    reset(&f, false, true, READ_STILL);
    uint32_t still_id = f.stream.native.still_id;
    for (int i = 0; i < 3; i++) {
        assert(!read_packet(&f, &pkt));
        assert(f.stream.peeks == i + 1 && !f.demux.failed);
        assert(f.stream.native.still_active && f.stream.native.still_duration == 3);
        assert(f.stream.native.still_id == still_id); // The player's timer keeps this identity.
    }
    for (int interactive = 0; interactive < 2; interactive++) {
        reset(&f, interactive != 0, true, READ_LEFT_STILL);
        assert(!read_packet(&f, &pkt) && f.stream.peeks == 1 && !f.demux.failed);
        assert(!f.stream.native.still_active);
        struct demux_packet payload = {.stream = 0};
        f.slave.packet = &payload;
        assert(read_packet(&f, &pkt) && pkt == &payload && f.priv.discontinuities == 1);
        assert(!f.demux.failed && f.stream.peeks == 1);
    }
    reset(&f, false, false, READ_EOF);
    assert(!read_packet(&f, &pkt) && f.stream.peeks == 0 && !f.demux.failed);
    reset(&f, true, false, READ_EOF);
    assert(!read_packet(&f, &pkt) && f.stream.peeks == 1 && !f.demux.failed);
    reset(&f, false, true, READ_STILL);
    f.priv.is_bd = false;
    assert(!read_packet(&f, &pkt) && f.stream.peeks == 0 && !f.demux.failed);
    puts("Disc still: latched EOF pumping, failures, release, identity and terminal EOF passed");
    return 0;
}
