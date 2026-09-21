#include "test_utils.h"

// Keep the chain's negotiation and processing-policy code intact while
// supplying synchronous pins at its input and output boundaries.
#include "filters/f_output_chain.c"

struct mp_pin {
    struct mp_frame frame;
};

static int wakeups;
static int speed_commands;
static double applied_speed;

bool mp_pin_can_transfer_data(struct mp_pin *dst, struct mp_pin *src)
{
    return !dst->frame.type && src->frame.type;
}

struct mp_frame mp_pin_out_read(struct mp_pin *pin)
{
    struct mp_frame frame = pin->frame;
    pin->frame = MP_NO_FRAME;
    return frame;
}

void mp_pin_out_unread(struct mp_pin *pin, struct mp_frame frame)
{
    assert_false(pin->frame.type);
    pin->frame = frame;
}

bool mp_pin_in_write(struct mp_pin *pin, struct mp_frame frame)
{
    mp_pin_out_unread(pin, frame);
    return true;
}

void mp_filter_wakeup(struct mp_filter *filter)
{
    wakeups++;
}

bool mp_filter_command(struct mp_filter *filter, struct mp_filter_command *command)
{
    speed_commands++;
    if (command->type == MP_FILTER_COMMAND_SET_SPEED)
        applied_speed = command->speed;
    return false;
}

double mp_frame_get_pts(struct mp_frame frame)
{
    return frame.type == MP_FRAME_AUDIO ? mp_aframe_get_pts(frame.data) : MP_NOPTS_VALUE;
}

static struct mp_frame make_frame(int format)
{
    struct mp_aframe *frame = mp_aframe_create();
    struct mp_chmap channels = MP_CHMAP_INIT_STEREO;
    assert_true(mp_aframe_set_format(frame, format));
    assert_true(mp_aframe_set_rate(frame, 48000));
    assert_true(mp_aframe_set_chmap(frame, &channels));
    if (af_fmt_is_encoded(format)) {
        uint8_t data[100] = {0};
        assert_true(mp_aframe_set_encoded_data(frame, data, sizeof(data), 512));
    } else {
        assert_true(mp_aframe_alloc_data(frame, 512));
    }
    return MAKE_FRAME(MP_FRAME_AUDIO, frame);
}

static void test_processing_negotiation(void)
{
    struct mp_pin in = {0}, out = {0}, filter_in = {0}, filter_out = {0};
    struct mp_pin *pins[] = {&in, &out};
    struct chain chain = {
        .type = MP_OUTPUT_CHAIN_AUDIO,
        .filters_in = &filter_in,
        .filters_out = &filter_out,
    };
    struct mp_filter filter = {.priv = &chain, .ppins = pins};
    chain.f = chain.public.f = &filter;
    chain.public.input_aformat = mp_aframe_create();

    // A passthrough frame with no requested processing can reach the chain.
    in.frame = make_frame(AF_FORMAT_RAW_DTSHD);
    assert_false(mp_output_chain_requires_pcm(&chain.public));
    output_chain_process(&filter);
    assert_false(chain.public.passthrough_rejected);
    assert_false(in.frame.type);
    assert_true(filter_in.frame.type == MP_FRAME_AUDIO);
    talloc_free(filter_in.frame.data);
    filter_in.frame = MP_NO_FRAME;

    // An enabled user filter must never consume the first negotiation frame.
    // Preserve it for the core's PCM renegotiation instead of disabling the
    // requested filter after it has already lost the access unit.
    chain.num_user_filters = 1;
    assert_true(mp_output_chain_requires_pcm(&chain.public));
    in.frame = make_frame(AF_FORMAT_RAW_DTSHD);
    void *held = in.frame.data;
    output_chain_process(&filter);
    assert_true(chain.public.passthrough_rejected);
    assert_true(in.frame.data == held);
    assert_false(filter_in.frame.type);
    assert_int_equal(wakeups, 1);
    output_chain_process(&filter);
    assert_true(in.frame.data == held);
    assert_int_equal(wakeups, 1);

    // Reset and PCM replay enable the originally requested processing.
    output_chain_reset(&filter);
    talloc_free(in.frame.data);
    in.frame = make_frame(AF_FORMAT_S16);
    output_chain_process(&filter);
    assert_false(chain.public.passthrough_rejected);
    assert_true(filter_in.frame.type == MP_FRAME_AUDIO);
    assert_int_equal(mp_aframe_get_format(filter_in.frame.data), AF_FORMAT_S16);
    talloc_free(filter_in.frame.data);
    filter_in.frame = MP_NO_FRAME;

    in.frame = make_frame(AF_FORMAT_S_AC3);
    output_chain_process(&filter);
    assert_true(chain.public.passthrough_rejected);
    assert_true(in.frame.type == MP_FRAME_AUDIO);
    assert_false(filter_in.frame.type);
    output_chain_reset(&filter);
    talloc_free(in.frame.data);
    in.frame = MP_NO_FRAME;

    chain.num_user_filters = 0;
    const double processing[][3] = {{1.25, 1, 1}, {1, 1.01, 1}, {1, 1, 0.9}};
    for (int n = 0; n < MP_ARRAY_SIZE(processing); n++) {
        mp_output_chain_set_audio_speed(&chain.public,
            processing[n][0], processing[n][1], processing[n][2]);
        assert_true(mp_output_chain_requires_pcm(&chain.public));
        in.frame = make_frame(AF_FORMAT_RAW_DTSHD);
        output_chain_process(&filter);
        assert_true(chain.public.passthrough_rejected);
        assert_true(in.frame.type == MP_FRAME_AUDIO);
        assert_false(filter_in.frame.type);
        output_chain_reset(&filter);
        talloc_free(in.frame.data);
        in.frame = MP_NO_FRAME;
    }

    mp_output_chain_set_audio_speed(&chain.public, 1, 1, 1);
    assert_false(mp_output_chain_requires_pcm(&chain.public));
    in.frame = make_frame(AF_FORMAT_RAW_DTSHD);
    output_chain_process(&filter);
    assert_false(chain.public.passthrough_rejected);
    assert_true(filter_in.frame.type == MP_FRAME_AUDIO);
    talloc_free(filter_in.frame.data);
    talloc_free(chain.public.input_aformat);
}

static void test_buffered_tail_policy_change(void)
{
    struct mp_pin in = {.frame = MP_EOF_FRAME}, out = {0};
    struct mp_pin filter_in = {0}, filter_out = {0};
    struct mp_pin *pins[] = {&in, &out};
    struct chain chain = {
        .type = MP_OUTPUT_CHAIN_AUDIO,
        .filters_in = &filter_in,
        .filters_out = &filter_out,
    };
    struct mp_filter filter = {.priv = &chain, .ppins = pins};
    struct mp_filter inner = {0};
    struct mp_user_filter user_filter = {.p = &chain, .f = &inner};
    struct mp_user_filter *post_filters[] = {&user_filter};
    chain.f = chain.public.f = &filter;
    chain.post_filters = post_filters;
    chain.num_post_filters = MP_ARRAY_SIZE(post_filters);
    chain.public.input_aformat = mp_aframe_create();

    in.frame = make_frame(AF_FORMAT_RAW_DTSHD);
    output_chain_process(&filter);
    assert_int_equal(mp_aframe_get_format(chain.public.input_aformat), AF_FORMAT_RAW_DTSHD);
    filter_out.frame = filter_in.frame;
    filter_in.frame = MP_NO_FRAME;
    in.frame = MP_EOF_FRAME;

    // There is no next compressed input frame: only the tail inside the
    // chain can reveal that applying a speed command would require PCM.
    speed_commands = 0;
    mp_output_chain_set_audio_speed(&chain.public, 1.25, 1.01, 0.9);
    assert_true(chain.public.passthrough_rejected);
    assert_true(chain.audio_speed_pending);
    assert_int_equal(speed_commands, 0);
    output_chain_process(&filter);
    assert_int_equal(speed_commands, 0);
    assert_int_equal(in.frame.type, MP_FRAME_EOF);
    assert_int_equal(filter_out.frame.type, MP_FRAME_AUDIO);
    assert_false(out.frame.type);

    // The caller resets buffered data and replays its retained packet as PCM.
    // Apply the stored policy before that frame reaches a processing filter.
    output_chain_reset(&filter);
    talloc_free(filter_out.frame.data);
    filter_out.frame = MP_NO_FRAME;
    mp_aframe_reset(chain.public.input_aformat);
    in.frame = make_frame(AF_FORMAT_S16);
    output_chain_process(&filter);
    assert_false(chain.public.passthrough_rejected);
    assert_false(chain.audio_speed_pending);
    assert_int_equal(speed_commands, 3);
    assert_true(applied_speed == 1.25);
    assert_int_equal(mp_aframe_get_format(filter_in.frame.data), AF_FORMAT_S16);
    talloc_free(filter_in.frame.data);
    talloc_free(chain.public.input_aformat);
}

int main(void)
{
    test_processing_negotiation();
    test_buffered_tail_policy_change();
    return 0;
}
