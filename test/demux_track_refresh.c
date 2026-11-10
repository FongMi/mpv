/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "test_utils.h"

#include "demux/demux.h"
#include "demux/packet.h"

struct demux_queue {
    bool correct_pos, correct_dts;
    int64_t last_pos;
    double last_dts;
    uint64_t tail_cum_pos;
};

struct demux_stream {
    struct demux_internal *in;
    enum stream_type type;
    int index;
    bool selected, refreshing;
    double base_ts, last_ret_dts;
    int64_t last_ret_pos;
    struct demux_queue *queue;
    struct demux_packet *reader_head;
};

struct demux_cached_range {
    bool updated;
};

struct demux_internal {
    struct mp_log *log;
    struct demuxer *d_thread;
    struct sh_stream **streams;
    struct demux_cached_range *current_range;
    int num_streams, seek_flags;
    uint64_t max_bytes;
    bool back_demuxing, recorder, seeking;
    double seek_pts;
};

static void ds_clear_reader_queue_state(struct demux_stream *ds)
{
    ds->reader_head = NULL;
}

static void clear_queue(struct demux_queue *queue)
{
    *queue = (struct demux_queue){.correct_pos = true, .correct_dts = true};
}

static void update_seek_ranges(struct demux_cached_range *range)
{
    range->updated = true;
}

static void seek(struct demuxer *demuxer, double pts, int flags)
{
    // The production refresh must queue a seek without performing I/O here.
    abort();
}

#include "demux_track_refresh_functions.h"

static void check_refresh(enum stream_type type, uint64_t max_bytes,
                          bool backward, bool recording, bool keep_audio)
{
    struct demux_packet packet = {.cum_pos = 10};
    struct demux_queue audio_queue = {
        .correct_dts = true, .last_dts = 15, .tail_cum_pos = 60,
    };
    struct demux_queue new_queue = {.correct_dts = true};
    struct demux_stream audio = {
        .type = STREAM_AUDIO, .selected = true, .base_ts = 10,
        .last_ret_pos = -1, .last_ret_dts = 9,
        .queue = &audio_queue, .reader_head = &packet,
    };
    struct demux_stream next = {
        .type = type, .selected = true, .base_ts = MP_NOPTS_VALUE,
        .last_ret_pos = -1, .last_ret_dts = MP_NOPTS_VALUE,
        .queue = &new_queue,
    };
    struct sh_stream existing = {.ds = &audio};
    struct sh_stream selected = {.ds = &next};
    struct sh_stream *streams[] = {&existing, &selected};
    const struct demuxer_desc desc = {.seek = seek};
    struct demuxer demuxer = {.desc = &desc, .seekable = true};
    struct demux_cached_range range = {0};
    struct demux_internal in = {
        .d_thread = &demuxer, .streams = streams, .num_streams = 2,
        .max_bytes = max_bytes, .current_range = &range,
        .back_demuxing = backward, .recorder = recording,
    };

    initiate_refresh_seek(&in, &next, 11);

    assert_true(in.seeking && range.updated);
    assert_int_equal(in.seek_flags, SEEK_HR);
    assert_float_equal(in.seek_pts, 9, 0);
    assert_true(audio.refreshing);
    assert_false(next.refreshing);
    assert_true(audio.reader_head == (keep_audio ? &packet : NULL));
    assert_float_equal(audio_queue.last_dts, keep_audio ? 15 : 9, 0);
    assert_int_equal(get_forward_buffered_bytes(&audio), keep_audio ? 50 : 0);
}

static void check_refresh_packets(bool use_dts)
{
    struct demux_queue queue = {
        .correct_dts = use_dts, .correct_pos = !use_dts,
        .last_dts = 15, .last_pos = 100,
    };
    struct demux_stream audio = {.queue = &queue, .refreshing = true};
    struct demux_packet older = {.dts = 14, .pos = 99};
    struct demux_packet overlap = {.dts = 15, .pos = 100};
    struct demux_packet newer = {.dts = 16, .pos = 101};

    assert_true(drop_refresh_packet(&audio, &older));
    assert_true(audio.refreshing);
    assert_true(drop_refresh_packet(&audio, &overlap));
    assert_false(audio.refreshing);

    // The seek can resume after the tail without rereading an exact match.
    audio.refreshing = true;
    assert_false(drop_refresh_packet(&audio, &newer));
    assert_false(audio.refreshing);
}

int main(void)
{
    check_refresh(STREAM_VIDEO, 100, false, false, true);
    check_refresh(STREAM_VIDEO, 99, false, false, false);
    check_refresh(STREAM_VIDEO, 100, true, false, false);
    check_refresh(STREAM_VIDEO, 100, false, true, false);
    check_refresh(STREAM_AUDIO, 100, false, false, false);
    check_refresh_packets(true);
    check_refresh_packets(false);
    return 0;
}
