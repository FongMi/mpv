#include "test_utils.h"

// Script the filter boundary while exercising the actual AO buffering code.
#define mp_pin_out_read test_pin_out_read
#define mp_frame_unref test_frame_unref
#define ao_post_process_data test_post_process_data
#include "audio/out/buffer.c"

static struct {
    bool initial_eof;
    bool final_eof;
    int frames;
    int pcm_samples;
    int queued_samples;
    int written_samples;
    int reads;
    int writes;
    int starts;
    int drains;
    bool playing;
} stream;

struct mp_frame test_pin_out_read(struct mp_pin *pin)
{
    stream.reads++;
    if (stream.initial_eof) {
        stream.initial_eof = false;
        return MP_EOF_FRAME;
    }
    if (stream.frames > 0) {
        stream.frames--;
        struct mp_aframe *frame = mp_aframe_create();
        struct mp_chmap channels = MP_CHMAP_INIT_STEREO;
        int format = stream.pcm_samples ? AF_FORMAT_FLOAT : AF_FORMAT_RAW_TRUEHD;
        assert_true(mp_aframe_set_format(frame, format));
        assert_true(mp_aframe_set_rate(frame, 48000));
        assert_true(mp_aframe_set_chmap(frame, &channels));
        if (stream.pcm_samples) {
            assert_true(mp_aframe_alloc_data(frame, stream.pcm_samples));
            float *data = (float *)mp_aframe_get_data_rw(frame)[0];
            for (int n = 0; n < stream.pcm_samples * 2; n++)
                data[n] = 0.25f;
        } else {
            uint8_t data[100] = {0};
            assert_true(mp_aframe_set_encoded_data(frame, data, sizeof(data), 40));
        }
        return MAKE_FRAME(MP_FRAME_AUDIO, frame);
    }
    if (stream.final_eof) {
        stream.final_eof = false;
        return MP_EOF_FRAME;
    }
    return MP_NO_FRAME;
}

void test_frame_unref(struct mp_frame *frame)
{
    assert_true(frame->type == MP_FRAME_EOF || frame->type == MP_FRAME_NONE);
    *frame = MP_NO_FRAME;
}

void test_post_process_data(struct ao *ao, void **data, int samples)
{
    // A raw frame must never enter PCM gain/silence processing.
    if (!af_fmt_is_pcm(ao->format))
        abort();
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    *state = (struct mp_pcm_state){
        .free_samples = 1,
        .playing = stream.playing,
    };
}

static bool write_data(struct ao *ao, void **data, int samples)
{
    assert_int_equal(samples, 1);
    struct mp_aframe *frame = *(struct mp_aframe **)data;
    assert_int_equal(mp_aframe_get_encoded_size(frame), 100);
    assert_int_equal(mp_aframe_get_size(frame), 40);
    stream.writes++;
    return true;
}

static void start(struct ao *ao)
{
    stream.starts++;
    stream.playing = true;
}

static void drain(struct ao *ao)
{
    stream.drains++;
}

static void wakeup(void *ctx)
{
}

static void test_frame_writer_continuation(void)
{
    memset(&stream, 0, sizeof(stream));
    struct ao_driver driver = {
        .write_frames = true,
        .get_state = get_state,
        .write = write_data,
        .start = start,
        .drain = drain,
    };
    struct mp_pin *pin = NULL;
    struct mp_filter input = {.pins = &pin};
    struct buffer_state buffer = {.playing = true, .input = &input};
    mp_cond_init(&buffer.wakeup);
    struct ao ao = {
        .driver = &driver,
        .buffer_state = &buffer,
        .format = AF_FORMAT_RAW_TRUEHD,
        .samplerate = 48000,
        .wakeup_cb = wakeup,
    };
    stream.initial_eof = true;
    stream.final_eof = true;
    stream.frames = 200;
    for (int n = 0; n < 200; n++) {
        // 40-sample access units are shorter than the device polling period.
        // Accepting one must request immediate refill, not a timed wait.
        assert_true(ao_play_data(&ao));
        assert_int_equal(stream.writes, n + 1);
        assert_int_equal(stream.drains, 0);
        assert_true(buffer.pending == NULL);
    }
    assert_int_equal(stream.starts, 1);

    assert_true(ao_play_data(&ao));
    assert_int_equal(stream.drains, 1);
    assert_true(buffer.playing);
    assert_true(buffer.streaming);
    // The driver remains active until the hardware has drained, at which
    // point the core must allow the next stream to call start() again.
    stream.playing = false;
    assert_true(ao_play_data(&ao));
    assert_false(buffer.playing);
    assert_false(buffer.streaming);

    buffer.playing = true;
    stream.frames = 1;
    assert_true(ao_play_data(&ao));
    assert_int_equal(stream.writes, 201);
    assert_int_equal(stream.starts, 2);
    assert_int_equal(stream.drains, 1);
    assert_true(buffer.pending == NULL);
    mp_cond_destroy(&buffer.wakeup);
}

static void pcm_get_state(struct ao *ao, struct mp_pcm_state *state)
{
    *state = (struct mp_pcm_state){
        .free_samples = 7200 - stream.queued_samples,
        .queued_samples = stream.queued_samples,
        .delay = stream.queued_samples / (double)ao->samplerate,
        .playing = stream.playing && stream.queued_samples > 0,
    };
}

static bool pcm_write_data(struct ao *ao, void **data, int samples)
{
    assert_true(samples > 0);
    assert_true(samples <= 7200 - stream.queued_samples);
    float *pcm = data[0];
    for (int n = 0; n < samples * 2; n++)
        assert_float_equal(pcm[n], 0.25f, 0);
    stream.writes++;
    stream.written_samples += samples;
    stream.queued_samples += samples;
    return true;
}

static const struct ao_driver pcm_driver = {
    .get_state = pcm_get_state,
    .write = pcm_write_data,
    .start = start,
    .drain = drain,
};

static const struct ao_driver pull_driver = {0};

struct pcm_test {
    struct mp_pin *pin;
    struct mp_filter input;
    struct buffer_state buffer;
    struct ao ao;
};

static void init_pcm_test(struct pcm_test *test, bool push)
{
    memset(&stream, 0, sizeof(stream));
    stream.pcm_samples = 2400;
    stream.frames = 1;
    *test = (struct pcm_test){
        .input = {.pins = &test->pin},
        .buffer = {.playing = true, .input = &test->input},
        .ao = {
            .driver = push ? &pcm_driver : &pull_driver,
            .buffer_state = &test->buffer,
            .format = AF_FORMAT_FLOAT,
            .samplerate = 48000,
            .channels = MP_CHMAP_INIT_STEREO,
            .num_planes = 1,
            .sstride = 2 * sizeof(float),
            .wakeup_cb = wakeup,
        },
    };
    mp_cond_init(&test->buffer.wakeup);
}

static void uninit_pcm_test(struct pcm_test *test)
{
    talloc_free(test->buffer.pending);
    talloc_free(test->buffer.temp_buf);
    mp_cond_destroy(&test->buffer.wakeup);
}

static void test_pcm_reader_short_read(void)
{
    struct pcm_test test;
    init_pcm_test(&test, false);
    float pcm[7200 * 2];
    for (int n = 0; n < MP_ARRAY_SIZE(pcm); n++)
        pcm[n] = 1.0f;
    void *data[] = {pcm};
    bool eof = false;
    int64_t out_time_ns = 150000000;

    assert_int_equal(ao_read_data_locked(&test.ao, data, 7200, out_time_ns,
                                         &eof, true), 2400);
    for (int n = 0; n < MP_ARRAY_SIZE(pcm); n++)
        assert_float_equal(pcm[n], n < 2400 * 2 ? 0.25f : 0.0f, 0);
    assert_false(eof);
    // Pull underrun uses the requested end time, including the padded tail.
    assert_false(test.buffer.playing);
    assert_int_equal(test.buffer.end_time_ns, out_time_ns);
    uninit_pcm_test(&test);
}

static void test_pcm_writer_short_read(void)
{
    struct pcm_test test;
    init_pcm_test(&test, true);

    assert_true(ao_play_data(&test.ao));
    assert_int_equal(stream.writes, 1);
    assert_int_equal(stream.written_samples, 2400);
    assert_int_equal(stream.queued_samples, 2400);
    assert_int_equal(stream.starts, 1);
    assert_true(test.buffer.playing);
    assert_true(test.buffer.streaming);

    // No input is ready, but the hardware still has the short write queued.
    assert_false(ao_play_data(&test.ao));
    assert_int_equal(stream.writes, 1);
    assert_true(test.buffer.playing);
    assert_true(test.buffer.streaming);
    assert_int_equal(stream.drains, 0);

    // A real device underrun must still stop the logical stream.
    stream.queued_samples = 0;
    assert_true(ao_play_data(&test.ao));
    assert_false(test.buffer.playing);
    assert_false(test.buffer.streaming);
    uninit_pcm_test(&test);
}

static void test_pcm_writer_short_eof(void)
{
    struct pcm_test test;
    init_pcm_test(&test, true);
    stream.final_eof = true;

    assert_true(ao_play_data(&test.ao));
    assert_int_equal(stream.writes, 1);
    assert_int_equal(stream.written_samples, 2400);
    assert_int_equal(stream.queued_samples, 2400);
    assert_int_equal(stream.starts, 1);
    assert_int_equal(stream.drains, 1);
    assert_true(test.buffer.playing);
    assert_true(test.buffer.streaming);
    assert_false(ao_play_data(&test.ao));
    assert_int_equal(stream.drains, 1);
    assert_true(test.buffer.playing);

    stream.queued_samples = 0;
    assert_true(ao_play_data(&test.ao));
    assert_false(test.buffer.playing);
    assert_false(test.buffer.streaming);
    uninit_pcm_test(&test);
}

static void test_pcm_writer_paused(void)
{
    struct pcm_test test;
    init_pcm_test(&test, true);
    assert_true(ao_play_data(&test.ao));
    stream.frames = 1;
    int reads = stream.reads;
    test.buffer.paused = true;

    assert_false(ao_play_data(&test.ao));
    assert_int_equal(stream.reads, reads);
    assert_int_equal(stream.frames, 1);
    assert_int_equal(stream.writes, 1);
    assert_int_equal(stream.queued_samples, 2400);
    assert_true(test.buffer.playing);
    assert_true(test.buffer.streaming);

    test.buffer.paused = false;
    assert_true(ao_play_data(&test.ao));
    assert_int_equal(stream.frames, 0);
    assert_int_equal(stream.writes, 2);
    assert_int_equal(stream.written_samples, 4800);
    assert_int_equal(stream.queued_samples, 4800);
    assert_int_equal(stream.starts, 1);
    uninit_pcm_test(&test);
}

int main(void)
{
    test_frame_writer_continuation();
    test_pcm_reader_short_read();
    test_pcm_writer_short_read();
    test_pcm_writer_short_eof();
    test_pcm_writer_paused();
    return 0;
}
