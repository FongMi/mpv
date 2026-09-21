#include "test_utils.h"

// Exercise the production negotiation and packet-ownership helpers without
// creating a demux thread or an Android audio device.
#include "filters/f_decoder_wrapper.c"

static int wakeups;
static bool decoder_failed;

bool mp_filter_has_failed(struct mp_filter *f)
{
    bool failed = decoder_failed;
    decoder_failed = false;
    return failed;
}

void mp_filter_wakeup(struct mp_filter *f)
{
    wakeups++;
}

void mp_frame_unref(struct mp_frame *frame)
{
    talloc_free(frame->data);
    *frame = MP_NO_FRAME;
}

static struct mp_aframe *make_format(int format, double pts, bool with_data)
{
    struct mp_aframe *frame = mp_aframe_create();
    struct mp_chmap channels;
    mp_chmap_from_channels(&channels, 6);
    assert_true(mp_aframe_set_format(frame, format));
    assert_true(mp_aframe_set_chmap(frame, &channels));
    assert_true(mp_aframe_set_rate(frame, 48000));
    mp_aframe_set_pts(frame, pts);
    if (with_data) {
        const uint8_t bytes[] = {1, 2, 3, 4};
        assert_true(mp_aframe_set_encoded_data(frame, bytes, sizeof(bytes), 512));
    }
    return frame;
}

static void add_packet(struct priv *p, int id)
{
    struct demux_packet *packet = talloc_zero(NULL, struct demux_packet);
    packet->pos = id;
    MP_TARRAY_APPEND(p, p->passthrough_packets, p->num_passthrough_packets,
                     MAKE_FRAME(MP_FRAME_PACKET, packet));
}

static void add_output(struct priv *p, int format, double pts, int packets)
{
    struct passthrough_output output = {
        .format = talloc_steal(p, make_format(format, pts, true)),
        .packets = packets,
    };
    MP_TARRAY_APPEND(p, p->passthrough_outputs, p->num_passthrough_outputs, output);
    p->queued_passthrough_packets += packets;
}

static struct priv *new_priv(struct mp_filter *filter)
{
    struct priv *p = talloc_zero(NULL, struct priv);
    filter->priv = p;
    p->public.f = filter;
    mp_mutex_init(&p->cache_lock);
    return p;
}

static void free_priv(struct priv *p)
{
    reset_decoder_state(p);
    mp_mutex_destroy(&p->cache_lock);
    talloc_free(p);
}

static void test_format_acknowledgement(void)
{
    struct mp_filter filter = {0};
    struct priv *p = new_priv(&filter);
    add_packet(p, 1);
    add_packet(p, 2);
    MP_TARRAY_APPEND(p, p->passthrough_packets, p->num_passthrough_packets, MP_EOF_FRAME);
    add_output(p, AF_FORMAT_RAW_DTS, 1, 1);
    add_output(p, AF_FORMAT_RAW_DTSHD, 2, 1);
    p->accepted_passthrough_format = talloc_steal(p,
        make_format(AF_FORMAT_RAW_DTS, 0, false));
    p->pending_passthrough_format = talloc_steal(p,
        make_format(AF_FORMAT_RAW_DTSHD, 0, false));

    // An equal format and timestamp do not identify the same access unit.
    struct mp_aframe *frame = make_format(AF_FORMAT_RAW_DTS, 1, true);
    mp_decoder_wrapper_accept_passthrough(&p->public, frame);
    assert_int_equal(p->num_passthrough_packets, 3);
    assert_true(p->pending_passthrough_format);
    talloc_free(frame);
    frame = mp_aframe_new_ref(p->passthrough_outputs[0].format);
    mp_decoder_wrapper_accept_passthrough(&p->public, frame);
    assert_int_equal(p->num_passthrough_packets, 2);
    assert_true(p->pending_passthrough_format);
    talloc_free(frame);

    frame = make_format(AF_FORMAT_RAW_DTSHD, 2, false);
    mp_decoder_wrapper_accept_passthrough(&p->public, frame);
    assert_false(p->pending_passthrough_format);
    assert_int_equal(p->num_passthrough_packets, 2);
    assert_int_equal(p->num_passthrough_outputs, 1);
    talloc_free(frame);

    frame = mp_aframe_new_ref(p->passthrough_outputs[0].format);
    mp_decoder_wrapper_accept_passthrough(&p->public, frame);
    assert_int_equal(p->num_passthrough_packets, 0);
    assert_int_equal(p->num_passthrough_outputs, 0);
    assert_int_equal(p->queued_passthrough_packets, 0);
    talloc_free(frame);
    free_priv(p);
}

static void test_replay_and_discard_order(void)
{
    struct mp_filter filter = {0};
    struct priv *p = new_priv(&filter);
    add_packet(p, 1);
    add_packet(p, 2);
    add_packet(p, 3);
    add_output(p, AF_FORMAT_RAW_DTSHD, 1, 1);
    discard_passthrough_packets(p, p->queued_passthrough_packets, 1);
    assert_int_equal(((struct demux_packet *)p->passthrough_packets[0].data)->pos, 1);
    assert_int_equal(((struct demux_packet *)p->passthrough_packets[1].data)->pos, 3);
    struct demux_packet *packet = talloc_zero(NULL, struct demux_packet);
    packet->pos = 4;
    MP_TARRAY_APPEND(p, p->replay_packets, p->num_replay_packets,
                     MAKE_FRAME(MP_FRAME_PACKET, packet));
    p->packet = MP_EOF_FRAME;
    int count;
    struct mp_frame *saved = take_passthrough_packets(p, &count);
    assert_int_equal(count, 4);
    reset_decoder_state(p);
    assert_int_equal(((struct demux_packet *)saved[0].data)->pos, 1);
    assert_int_equal(((struct demux_packet *)saved[1].data)->pos, 3);
    assert_int_equal(((struct demux_packet *)saved[2].data)->pos, 4);
    assert_int_equal(saved[3].type, MP_FRAME_EOF);
    for (int n = 0; n < count; n++)
        mp_frame_unref(&saved[n]);
    talloc_free(saved);
    free_priv(p);
}

static void test_fallback_transports(void)
{
    struct mp_filter filter = {0};
    struct priv *p = new_priv(&filter);
    p->using_raw = true;
    assert_true(fallback_passthrough(p, AF_FORMAT_RAW_AC3, false));
    assert_false(p->try_raw);
    assert_true(p->try_spdif);
    p->using_raw = false;
    assert_false(fallback_passthrough(p, AF_FORMAT_S_AC3, false));
    assert_false(p->try_spdif);
    p->using_raw = true;
    assert_false(fallback_passthrough(p, AF_FORMAT_RAW_AC3, true));
    assert_false(p->try_spdif);
    free_priv(p);
}

static void test_repeated_acknowledgement(void)
{
    struct mp_filter filter = {0};
    struct priv *p = new_priv(&filter);
    add_packet(p, 1);
    add_packet(p, 2);
    add_output(p, AF_FORMAT_RAW_DTSHD, 1, 1);
    add_output(p, AF_FORMAT_RAW_DTSHD, 1, 1);
    struct mp_aframe *first = mp_aframe_new_ref(p->passthrough_outputs[0].format);
    int initial_wakeups = wakeups;
    mp_decoder_wrapper_accept_passthrough(&p->public, first);
    assert_int_equal(p->num_passthrough_packets, 1);
    assert_int_equal(wakeups, initial_wakeups + 1);
    mp_decoder_wrapper_accept_passthrough(&p->public, first);
    assert_int_equal(p->num_passthrough_packets, 1);
    assert_int_equal(wakeups, initial_wakeups + 1);
    struct mp_aframe *second = mp_aframe_new_ref(p->passthrough_outputs[0].format);
    mp_decoder_wrapper_accept_passthrough(&p->public, second);
    assert_int_equal(p->num_passthrough_packets, 0);
    assert_int_equal(wakeups, initial_wakeups + 2);
    talloc_free(first);
    talloc_free(second);
    free_priv(p);
}

static void test_failure_waits_for_output_acknowledgement(void)
{
    struct mp_filter filter = {0};
    struct priv *p = new_priv(&filter);
    struct mp_filter decoder_filter = {0};
    struct mp_decoder decoder = {.f = &decoder_filter};
    p->decoder = &decoder;
    p->using_spdif = true;
    p->using_raw = true;
    add_packet(p, 1);
    add_packet(p, 2);
    add_packet(p, 3); // The packet whose preparation failed.
    add_output(p, AF_FORMAT_RAW_DTSHD, 1, 1);
    add_output(p, AF_FORMAT_RAW_DTSHD, 2, 1);

    // The decoder reports failure once, while earlier output remains queued.
    decoder_failed = true;
    assert_false(passthrough_decoder_failed(p));
    assert_true(decoder_failed);
    for (int n = 0; n < 2; n++) {
        struct mp_aframe *frame = mp_aframe_new_ref(p->passthrough_outputs[0].format);
        mp_decoder_wrapper_accept_passthrough(&p->public, frame);
        talloc_free(frame);
        if (n == 0) {
            assert_false(passthrough_decoder_failed(p));
            assert_true(decoder_failed);
        }
    }

    // The last ACK is sufficient to continue fallback, without asking the
    // failed decoder to process again or report the same error a second time.
    assert_true(passthrough_decoder_failed(p));
    assert_false(decoder_failed);
    assert_true(fallback_passthrough(p, AF_FORMAT_UNKNOWN, false));
    assert_false(p->try_raw);
    assert_int_equal(p->num_passthrough_packets, 1);
    assert_int_equal(((struct demux_packet *)p->passthrough_packets[0].data)->pos, 3);
    free_priv(p);
}

static void test_dts_hd_fallback(void)
{
    struct mp_filter filter = {0};
    struct priv *p = new_priv(&filter);
    struct mp_codec_params codec = {.type = STREAM_AUDIO, .codec = "dts"};
    struct dec_wrapper_opts opts = {.audio_spdif = "ac3,dts-hd,truehd"};
    p->codec = &codec;
    p->opts = &opts;
    mp_decoder_wrapper_set_spdif_flag(&p->public, true);

    p->using_raw = true;
    assert_true(fallback_passthrough(p, AF_FORMAT_RAW_DTSHD, false));
    assert_false(p->try_raw);
    assert_false(p->dts_core_only);
    assert_string_equal(passthrough_codecs(p), "ac3,dts-hd,truehd");

    p->using_raw = false;
    assert_true(fallback_passthrough(p, AF_FORMAT_S_DTSHD, false));
    assert_false(p->try_raw);
    assert_true(p->dts_core_only);
    assert_string_equal(passthrough_codecs(p), "dts");
    assert_string_equal(opts.audio_spdif, "ac3,dts-hd,truehd");

    // Seeking must not restart the failed HD negotiation or lose the core choice.
    reset_decoder_state(p);
    assert_true(p->dts_core_only);
    assert_string_equal(passthrough_codecs(p), "dts");
    assert_false(fallback_passthrough(p, AF_FORMAT_S_DTS, false));
    assert_false(p->try_spdif);
    assert_false(fallback_passthrough(p, AF_FORMAT_S_DTSHD, false));

    // A segmented stream using another codec keeps its original configuration.
    codec.codec = "truehd";
    assert_string_equal(passthrough_codecs(p), "ac3,dts-hd,truehd");
    codec.codec = "dts";

    // A fresh output route can support HD even if the preceding route rejected it.
    mp_decoder_wrapper_set_spdif_flag(&p->public, true);
    assert_false(p->dts_core_only);
    assert_true(p->try_spdif);
    assert_string_equal(passthrough_codecs(p), "ac3,dts-hd,truehd");
    assert_true(fallback_passthrough(p, AF_FORMAT_S_DTSHD, false));
    mp_decoder_wrapper_set_spdif_flag(&p->public, false);
    assert_false(p->dts_core_only);
    assert_false(p->try_spdif);
    free_priv(p);
}

static void test_dts_hd_fallback_requires_rejected_hd_output(void)
{
    struct mp_filter filter = {0};
    struct priv *p = new_priv(&filter);
    // DTS-HD without a core is rejected during SPDIF preparation, before an AO
    // format exists. Never retry the same packet as core passthrough.
    assert_false(fallback_passthrough(p, AF_FORMAT_UNKNOWN, false));
    assert_false(p->dts_core_only);
    // A configured dts-hd decoder may emit ordinary DTS for a core-only stream.
    assert_false(fallback_passthrough(p, AF_FORMAT_S_DTS, false));
    assert_false(p->dts_core_only);
    assert_false(fallback_passthrough(p, AF_FORMAT_S_TRUEHD, false));
    assert_false(p->dts_core_only);
    // Filters requiring PCM take priority over the DTS core fallback.
    assert_false(fallback_passthrough(p, AF_FORMAT_S_DTSHD, true));
    assert_false(p->dts_core_only);
    p->using_raw = true;
    assert_false(fallback_passthrough(p, AF_FORMAT_RAW_DTSHD, true));
    assert_false(p->try_raw);
    assert_false(p->dts_core_only);
    free_priv(p);
}

static void test_dts_core_fallback_failure(void)
{
    const struct {
        int failed_format;
        bool force_pcm;
    } cases[] = {
        {AF_FORMAT_S_DTS, false},         // The output also rejects the core.
        {AF_FORMAT_UNKNOWN, false},      // No usable core in the packet.
        {AF_FORMAT_UNKNOWN, true},       // A filter now requires PCM.
    };

    for (int n = 0; n < MP_ARRAY_SIZE(cases); n++) {
        struct mp_filter filter = {0};
        struct priv *p = new_priv(&filter);
        assert_true(fallback_passthrough(p, AF_FORMAT_S_DTSHD, false));
        assert_false(fallback_passthrough(p, cases[n].failed_format,
                                         cases[n].force_pcm));
        assert_false(p->try_raw);
        assert_false(p->try_spdif);
        free_priv(p);
    }
}

int main(void)
{
    test_format_acknowledgement();
    test_replay_and_discard_order();
    test_fallback_transports();
    test_dts_hd_fallback();
    test_dts_hd_fallback_requires_rejected_hd_output();
    test_dts_core_fallback_failure();
    test_repeated_acknowledgement();
    test_failure_waits_for_output_acknowledgement();
    return 0;
}
