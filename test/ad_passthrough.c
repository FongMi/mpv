#include "test_utils.h"
#include <libavformat/avformat.h>

// Keep the parser, packet ownership, chunking and EOF implementation intact.
// The filter pins are synchronous and container headers come from the fixture
// declaration. FFmpeg still parses the actual access-unit bytes.
#include "audio/decode/ad_passthrough.c"

struct mp_pin {
    struct mp_frame frame;
};

static bool failed;

int mp_codec_to_av_codec_id(const char *name)
{
    const AVCodecDescriptor *codec = avcodec_descriptor_get_by_name(name);
    return codec ? codec->id : AV_CODEC_ID_NONE;
}

int mp_set_avctx_codec_headers(AVCodecContext *avctx, const struct mp_codec_params *codec)
{
    avctx->sample_rate = codec->samplerate;
    mp_chmap_to_av_layout(&avctx->ch_layout, &codec->channels);
    return 0;
}

bool mp_pin_in_needs_data(struct mp_pin *pin)
{
    return !pin->frame.type;
}

struct mp_frame mp_pin_out_read(struct mp_pin *pin)
{
    struct mp_frame frame = pin->frame;
    pin->frame = MP_NO_FRAME;
    return frame;
}

bool mp_pin_in_write(struct mp_pin *pin, struct mp_frame frame)
{
    assert_false(pin->frame.type);
    pin->frame = frame;
    return true;
}

void mp_pin_out_repeat_eof(struct mp_pin *pin)
{
    assert_false(pin->frame.type);
    pin->frame = MP_EOF_FRAME;
}

void mp_frame_unref(struct mp_frame *frame)
{
    talloc_free(frame->data);
    *frame = MP_NO_FRAME;
}

void mp_filter_internal_mark_progress(struct mp_filter *f)
{
}

void mp_filter_internal_mark_failed(struct mp_filter *f)
{
    failed = true;
}

bool mp_filter_has_failed(struct mp_filter *f)
{
    bool result = failed;
    failed = false;
    return result;
}

static struct demux_packet *make_packet(const uint8_t *data, int bytes, double pts)
{
    struct demux_packet *pkt = talloc_zero(NULL, struct demux_packet);
    pkt->buffer = talloc_zero_size(pkt, bytes + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(pkt->buffer, data, bytes);
    pkt->len = bytes;
    pkt->pts = pkt->dts = pts;
    pkt->pos = -1;
    return pkt;
}

static struct priv *make_decoder(struct mp_codec_params *codec)
{
    struct priv *p = talloc_zero(NULL, struct priv);
    p->codec = codec;
    p->use_dts_hd = true;
    assert_true(init_parser(p));
    return p;
}

static void test_stream(const char *directory, const char *file,
                        const char *codec_name, int expected_format,
                        int samples_per_access_unit)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", directory, file);
    AVFormatContext *demux = NULL;
    assert_int_equal(avformat_open_input(&demux, path,
                                         av_find_input_format(codec_name), NULL), 0);
    assert_true(avformat_find_stream_info(demux, NULL) >= 0);
    struct mp_codec_params codec = {
        .type = STREAM_AUDIO, .codec = codec_name, .samplerate = 48000,
    };
    mp_chmap_from_channels(&codec.channels, 6);
    struct priv *p = make_decoder(&codec);
    struct mp_pin in = {0}, out = {0};
    struct mp_pin *pins[] = {&in, &out};
    struct mp_filter filter = {.priv = p, .ppins = pins};
    int input_count = 0, output_count = 0, output_samples = 0;
    int input_bytes = 0, output_bytes = 0;
    AVPacket *packet = av_packet_alloc();
    while (av_read_frame(demux, packet) >= 0) {
        double pts = input_count * samples_per_access_unit / 48000.0;
        in.frame = MAKE_FRAME(MP_FRAME_PACKET,
            make_packet(packet->data, packet->size, pts));
        input_count++;
        input_bytes += packet->size;
        process(&filter);
        assert_false(failed);
        if (out.frame.type) {
            assert_int_equal(out.frame.type, MP_FRAME_AUDIO);
            struct mp_aframe *frame = out.frame.data;
            assert_int_equal(mp_aframe_get_format(frame), expected_format);
            assert_int_equal(mp_aframe_get_rate(frame), 48000);
            assert_int_equal(mp_aframe_get_channels(frame), 6);
            output_count++;
            output_bytes += mp_aframe_get_encoded_size(frame);
            output_samples += mp_aframe_get_size(frame);
            if (expected_format != AF_FORMAT_RAW_TRUEHD) {
                assert_int_equal(mp_aframe_get_size(frame), samples_per_access_unit);
                assert_memcmp(mp_aframe_get_encoded_data(frame),
                              packet->data, packet->size);
            }
            mp_frame_unref(&out.frame);
        }
        av_packet_unref(packet);
    }
    in.frame = MP_EOF_FRAME;
    process(&filter);
    if (out.frame.type == MP_FRAME_AUDIO) {
        output_count++;
        output_bytes += mp_aframe_get_encoded_size(out.frame.data);
        output_samples += mp_aframe_get_size(out.frame.data);
        assert_int_equal(expected_format, AF_FORMAT_RAW_TRUEHD);
        assert_int_equal(mp_aframe_get_size(out.frame.data),
                         (input_count % 16) * samples_per_access_unit);
        mp_frame_unref(&out.frame);
        process(&filter);
    }
    assert_int_equal(out.frame.type, MP_FRAME_EOF);
    mp_frame_unref(&out.frame);
    process(&filter);
    assert_false(out.frame.type);
    assert_false(failed);
    assert_true(input_count > 0);
    assert_int_equal(output_samples, input_count * samples_per_access_unit);
    assert_int_equal(output_bytes, input_bytes);
    assert_int_equal(output_count,
        expected_format == AF_FORMAT_RAW_TRUEHD ? (input_count + 15) / 16 : input_count);
    assert_int_equal(p->public.passthrough_consumed, input_count);
    reset(&filter);
    assert_false(p->truehd_synced);
    assert_int_equal(p->public.passthrough_consumed, 0);
    assert_false(p->chunk_format);
    assert_false(failed);
    destroy(&filter);
    talloc_free(p);
    av_packet_free(&packet);
    avformat_close_input(&demux);
}

static void test_independent_eac3_frames(const char *directory)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/silence.eac3", directory);
    AVFormatContext *demux = NULL;
    assert_int_equal(avformat_open_input(&demux, path,
                                         av_find_input_format("eac3"), NULL), 0);
    AVPacket *packet = av_packet_alloc();
    assert_true(av_read_frame(demux, packet) >= 0);
    struct mp_codec_params codec = {
        .type = STREAM_AUDIO, .codec = "eac3", .samplerate = 48000,
    };
    mp_chmap_from_channels(&codec.channels, 6);
    struct priv *p = make_decoder(&codec);
    uint8_t *data = talloc_size(p, packet->size * 2);
    memcpy(data, packet->data, packet->size);
    memcpy(data + packet->size, packet->data, packet->size);
    struct demux_packet *pkt = make_packet(data, packet->size * 2, 0);
    assert_false(parse_packet(p, pkt));
    talloc_free(pkt);
    struct mp_filter filter = {.priv = p};
    destroy(&filter);
    talloc_free(p);
    av_packet_free(&packet);
    avformat_close_input(&demux);
}

static void test_dts_hd_sample(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert_true(file);
    uint8_t data[65536];
    int bytes = fread(data, 1, sizeof(data), file);
    fclose(file);
    struct mp_codec_params codec = {
        .type = STREAM_AUDIO, .codec = "dts", .samplerate = 48000,
    };
    mp_chmap_from_channels(&codec.channels, 6);
    struct priv *p = make_decoder(&codec);
    struct demux_packet *pkt = make_packet(data, bytes, 0.002);
    struct mp_aframe *frame = parse_packet(p, pkt);
    assert_true(frame);
    assert_int_equal(p->avctx->profile, AV_PROFILE_DTS_HD_MA);
    assert_int_equal(mp_aframe_get_format(frame), AF_FORMAT_RAW_DTSHD);
    assert_int_equal(mp_aframe_get_rate(frame), 48000);
    assert_int_equal(mp_aframe_get_channels(frame), 6);
    assert_int_equal(mp_aframe_get_size(frame), 512);
    assert_int_equal(mp_aframe_get_encoded_size(frame), bytes);
    assert_memcmp(mp_aframe_get_encoded_data(frame), data, bytes);
    printf("DTS-HD: %d bytes, 48000 Hz, 6 channels, 512 decoded samples\n", bytes);
    talloc_free(frame);
    talloc_free(pkt);
    struct mp_filter filter = {.priv = p};
    destroy(&filter);
    talloc_free(p);
}

static void test_truehd_reset(const char *directory)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/silence.thd", directory);
    AVFormatContext *demux = NULL;
    assert_int_equal(avformat_open_input(&demux, path,
                                         av_find_input_format("truehd"), NULL), 0);
    AVPacket *first = av_packet_alloc(), *second = av_packet_alloc();
    assert_true(av_read_frame(demux, first) >= 0);
    assert_true(av_read_frame(demux, second) >= 0);
    struct mp_codec_params codec = {
        .type = STREAM_AUDIO, .codec = "truehd", .samplerate = 48000,
    };
    mp_chmap_from_channels(&codec.channels, 6);
    struct priv *p = make_decoder(&codec);
    struct mp_pin in = {0}, out = {0};
    struct mp_pin *pins[] = {&in, &out};
    struct mp_filter filter = {.priv = p, .ppins = pins};
    in.frame = MAKE_FRAME(MP_FRAME_PACKET, make_packet(first->data, first->size, 0));
    process(&filter);
    assert_false(failed);
    assert_int_equal(p->chunk_access_units, 1);
    reset(&filter);
    assert_int_equal(p->chunk_access_units, 0);
    assert_false(p->chunk_format);
    assert_false(p->truehd_synced);
    in.frame = MAKE_FRAME(MP_FRAME_PACKET, make_packet(second->data, second->size, 0));
    process(&filter);
    assert_false(out.frame.type);
    assert_int_equal(p->public.passthrough_discarded, 1);
    in.frame = MAKE_FRAME(MP_FRAME_PACKET, make_packet(first->data, first->size, 1));
    process(&filter);
    in.frame = MP_EOF_FRAME;
    process(&filter);
    assert_int_equal(out.frame.type, MP_FRAME_AUDIO);
    assert_int_equal(mp_aframe_get_size(out.frame.data), 40);
    assert_memcmp(mp_aframe_get_encoded_data(out.frame.data), first->data, first->size);
    mp_frame_unref(&out.frame);
    process(&filter);
    assert_int_equal(out.frame.type, MP_FRAME_EOF);
    mp_frame_unref(&out.frame);
    assert_false(failed);
    destroy(&filter);
    talloc_free(p);
    av_packet_free(&first);
    av_packet_free(&second);
    avformat_close_input(&demux);
}

static void mark_frame_destroyed(void *ptr)
{
    **(bool **)ptr = true;
}

static void test_truehd_output_lifetime(const char *directory, int packets,
                                        bool malformed_tail)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/silence.thd", directory);
    AVFormatContext *demux = NULL;
    assert_int_equal(avformat_open_input(&demux, path,
                                         av_find_input_format("truehd"), NULL), 0);
    AVPacket *packet = av_packet_alloc();
    assert_true(av_read_frame(demux, packet) >= 0);
    struct mp_codec_params codec = {
        .type = STREAM_AUDIO, .codec = "truehd", .samplerate = 48000,
    };
    mp_chmap_from_channels(&codec.channels, 6);
    struct priv *p = make_decoder(&codec);
    struct mp_pin in = {0}, out = {0};
    struct mp_pin *pins[] = {&in, &out};
    struct mp_filter filter = {.priv = p, .ppins = pins};
    for (int n = 0; n < packets; n++) {
        in.frame = MAKE_FRAME(MP_FRAME_PACKET,
            make_packet(packet->data, packet->size, n * 40.0 / 48000));
        process(&filter);
    }
    if (malformed_tail) {
        const uint8_t invalid[] = {0, 0, 0};
        in.frame = MAKE_FRAME(MP_FRAME_PACKET,
            make_packet(invalid, sizeof(invalid), packets * 40.0 / 48000));
        process(&filter);
        assert_true(p->failed);
    } else if (!out.frame.type) {
        in.frame = MP_EOF_FRAME;
        process(&filter);
    }
    assert_int_equal(out.frame.type, MP_FRAME_AUDIO);
    assert_int_equal(mp_aframe_get_size(out.frame.data), packets * 40);

    // The AO may reject this output and destroy the decoder before the
    // output chain releases the frame. Observe its lifetime without reading
    // freed memory if the decoder still owns it.
    bool destroyed = false;
    bool **observer = talloc(out.frame.data, bool *);
    *observer = &destroyed;
    talloc_set_destructor(observer, mark_frame_destroyed);
    destroy(&filter);
    talloc_free(p);
    assert_false(destroyed);
    assert_int_equal(mp_aframe_get_size(out.frame.data), packets * 40);
    assert_int_equal(mp_aframe_get_encoded_size(out.frame.data), packets * packet->size);
    assert_memcmp(mp_aframe_get_encoded_data(out.frame.data), packet->data, packet->size);
    mp_frame_unref(&out.frame);
    assert_true(destroyed);
    av_packet_free(&packet);
    avformat_close_input(&demux);
}

int main(int argc, char **argv)
{
    assert_true(argc >= 2);
    test_stream(argv[1], "silence.ac3", "ac3", AF_FORMAT_RAW_AC3, 1536);
    test_stream(argv[1], "silence.eac3", "eac3", AF_FORMAT_RAW_EAC3, 1536);
    test_stream(argv[1], "silence.dts", "dts", AF_FORMAT_RAW_DTS, 512);
    test_stream(argv[1], "silence.thd", "truehd", AF_FORMAT_RAW_TRUEHD, 40);
    test_independent_eac3_frames(argv[1]);
    test_truehd_reset(argv[1]);
    test_truehd_output_lifetime(argv[1], 16, false);
    test_truehd_output_lifetime(argv[1], 1, false);
    test_truehd_output_lifetime(argv[1], 1, true);
    if (argc > 2)
        test_dts_hd_sample(argv[2]);
    return 0;
}
