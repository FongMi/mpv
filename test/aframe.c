#include "test_utils.h"
#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"

static struct mp_aframe *make_encoded_frame(int bytes)
{
    struct mp_aframe *frame = mp_aframe_create();
    struct mp_chmap channels;
    mp_chmap_from_channels(&channels, 6);
    assert_true(mp_aframe_set_format(frame, AF_FORMAT_RAW_DTSHD));
    assert_true(mp_aframe_set_chmap(frame, &channels));
    assert_true(mp_aframe_set_rate(frame, 48000));
    mp_aframe_set_pts(frame, 2.0);

    uint8_t data[6544];
    for (int n = 0; n < sizeof(data); n++)
        data[n] = n;
    assert_true(mp_aframe_set_encoded_data(frame, data, bytes, 512));
    assert_memcmp(mp_aframe_get_encoded_data(frame), data, bytes);
    return frame;
}

static void test_encoded_timing(void)
{
    struct mp_aframe *large = make_encoded_frame(6544);
    struct mp_aframe *small = make_encoded_frame(1000);
    assert_int_equal(mp_aframe_get_size(large), 512);
    assert_int_equal(mp_aframe_get_encoded_size(large), 6544);
    assert_int_equal(mp_aframe_get_encoded_size(small), 1000);
    assert_float_equal(mp_aframe_duration(large), 512.0 / 48000, 1e-12);
    assert_float_equal(mp_aframe_duration(large), mp_aframe_duration(small), 1e-12);
    assert_float_equal(mp_aframe_end_pts(large), 2.0 + 512.0 / 48000, 1e-12);
    assert_true(mp_aframe_approx_byte_size(large) > mp_aframe_approx_byte_size(small));
    talloc_free(large);
    talloc_free(small);
}

static void test_encoded_ownership(void)
{
    struct mp_aframe *frame = make_encoded_frame(6544);
    struct mp_aframe *ref = mp_aframe_new_ref(frame);
    const uint8_t *data = mp_aframe_get_encoded_data(ref);
    assert_true(data == mp_aframe_get_encoded_data(frame));
    assert_true(mp_aframe_config_equals(frame, ref));
    assert_int_equal(mp_aframe_get_size(ref), 512);
    mp_aframe_reset(frame);
    assert_false(mp_aframe_is_allocated(frame));
    assert_true(mp_aframe_is_allocated(ref));
    assert_int_equal(mp_aframe_get_encoded_size(ref), 6544);
    assert_int_equal(data[6543], 6543 & 255);
    mp_aframe_unref_data(ref);
    assert_false(mp_aframe_is_allocated(ref));
    assert_true(mp_aframe_config_is_valid(ref));
    assert_int_equal(mp_aframe_get_encoded_size(ref), 0);
    assert_int_equal(mp_aframe_get_size(ref), 0);
    talloc_free(frame);
    talloc_free(ref);
}

static void test_encoded_access_units(void)
{
    struct mp_aframe *frame = make_encoded_frame(6544);
    assert_false(af_fmt_is_pcm(AF_FORMAT_RAW_DTSHD));
    assert_false(af_fmt_is_spdif(AF_FORMAT_RAW_DTSHD));
    assert_int_equal(af_fmt_to_bytes(AF_FORMAT_RAW_DTSHD), 0);
    assert_true(mp_aframe_get_data_ro(frame) == NULL);
    assert_true(mp_aframe_get_data_rw(frame) == NULL);
    assert_true(mp_aframe_to_avframe(frame) == NULL);
    assert_false(mp_aframe_set_silence(frame, 0, 512));
    assert_false(mp_aframe_set_size(frame, 256));
    assert_false(mp_aframe_reverse(frame));

    // A seek boundary inside an access unit must not truncate its payload.
    mp_aframe_clip_timestamps(frame, 2.001, 2.005);
    assert_int_equal(mp_aframe_get_size(frame), 512);
    assert_int_equal(mp_aframe_get_encoded_size(frame), 6544);
    mp_aframe_skip_samples(frame, 512);
    assert_int_equal(mp_aframe_get_size(frame), 0);
    assert_float_equal(mp_aframe_get_pts(frame), 2.0 + 512.0 / 48000, 1e-12);
    talloc_free(frame);
}

int main(void)
{
    test_encoded_timing();
    test_encoded_ownership();
    test_encoded_access_units();
    return 0;
}
