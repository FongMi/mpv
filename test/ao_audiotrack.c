#include "test_utils.h"

// Exercise the AudioTrack driver against a deterministic JNI boundary. No
// Android media service is needed, and the production packet callbacks run
// unchanged, including their ownership and partial-write handling.
#define mp_time_ns test_time_ns
#define mp_raw_time_ns test_raw_time_ns
#define ao_request_reload test_request_reload
#include "osdep/threads.h"

static struct init_fixture {
    mp_mutex *mutex;
    mp_cond *cond;
    int mutex_destroys;
    int cond_destroys;
    int discovery_calls;
    int fail_discovery_at;
    int timestamp_calls;
    bool fail_timestamp;
    bool running;
    int encoding;
    int track_calls;
    int release_calls;
} init_fixture;

static int test_private_mutex_destroy(mp_mutex *mutex)
{
    int ret = mp_mutex_destroy(mutex);
    if (mutex == init_fixture.mutex) {
        assert_int_equal(ret, 0);
        init_fixture.mutex_destroys++;
    }
    return ret;
}

static int test_private_cond_destroy(mp_cond *cond)
{
    int ret = mp_cond_destroy(cond);
    if (cond == init_fixture.cond) {
        assert_int_equal(ret, 0);
        init_fixture.cond_destroys++;
    }
    return ret;
}

// Observe destruction of the two private synchronization objects, while
// executing their real implementations. Do not intercept core locks.
#pragma push_macro("mp_mutex_destroy")
#pragma push_macro("mp_cond_destroy")
#undef mp_mutex_destroy
#define mp_mutex_destroy test_private_mutex_destroy
#define mp_cond_destroy test_private_cond_destroy
#include "audio/out/ao_audiotrack.c"
#pragma pop_macro("mp_cond_destroy")
#pragma pop_macro("mp_mutex_destroy")

// Keep the real class-reference cleanup. Environment lookup and discovery
// remain JNI boundaries; unused VM helpers are discarded by section GC.
#define mp_jni_get_env unused_jni_get_env
#define mp_jni_exception_check unused_jni_exception_check
#define mp_jni_init_jfields unused_jni_init_jfields
#include "misc/jni.c"
#undef mp_jni_init_jfields
#undef mp_jni_exception_check
#undef mp_jni_get_env

// Keep the real core refill loop and driver callbacks. Only the source and
// device clock are scripted; the wait hook must not affect the pull driver.
#include "filters/filter_internal.h"
#include "osdep/threads.h"
static int test_buffer_timedwait(mp_cond *cond, mp_mutex *mutex, int64_t timeout);
static struct mp_frame test_buffer_pin_read(struct mp_pin *pin);
static void test_buffer_frame_unref(struct mp_frame *frame);
static void test_buffer_post_process(struct ao *ao, void **data, int samples);
#define ao_thread buffer_ao_thread
#define ao_read_data buffer_ao_read_data
#define mp_cond_timedwait test_buffer_timedwait
#define mp_pin_out_read test_buffer_pin_read
#define mp_frame_unref test_buffer_frame_unref
#define ao_post_process_data test_buffer_post_process
#include "audio/out/buffer.c"
#undef ao_post_process_data
#undef mp_frame_unref
#undef mp_pin_out_read
#undef mp_cond_timedwait
#undef ao_read_data
#undef ao_thread

enum {
    METHOD_POSITION = 1,
    METHOD_HEAD,
    METHOD_WRITE,
    METHOD_PAUSE,
    METHOD_PLAY,
    METHOD_FLUSH,
    METHOD_STOP,
    METHOD_STATE,
    METHOD_LATENCY,
    METHOD_TIMESTAMP,
    METHOD_CAPACITY,
    METHOD_NEW_TIMESTAMP,
    METHOD_MIN_BUFFER_SIZE,
    METHOD_NEW_BUILDER,
    METHOD_NEW_TRACK,
    METHOD_BUILD,
    METHOD_SET_ENCODING,
    METHOD_SET_SAMPLE_RATE,
    METHOD_SET_CHANNEL_MASK,
    METHOD_SET_USAGE,
    METHOD_INITIALIZED,
    METHOD_RELEASE,
};

struct test_buffer {
    const uint8_t *data;
    int size;
    int position;
    int refs;
};

static struct test_device {
    int64_t time_ns;
    uint32_t head;
    int write_limit;
    int write_calls;
    int stop_calls;
    int pause_calls;
    int play_calls;
    int flush_calls;
    bool stopped;
    int reloads;
    int buffers;
    int size;
    int read_calls;
    int capacity;
    struct ao *core_ao;
    bool output_started;
    int waits;
    int underruns;
    int core_frames;
    int core_samples;
    bool core_eof;
    bool core_reading;
    int core_queue_resets;
    int core_filter_resets;
    int core_resumes;
    int flushed_bytes;
    int fail_method;
    bool exception;
    int exceptions;
    uint8_t bytes[131072];
} device;

static int test_buffer_timedwait(mp_cond *cond, mp_mutex *mutex, int64_t timeout)
{
    struct ao *ao = device.core_ao;
    assert_true(ao != NULL);
    struct buffer_state *buffer = ao->buffer_state;
    assert_true(cond == &buffer->pt_wakeup);
    assert_true(mutex == &buffer->pt_lock);
    // An underrun may make the real core stop polling altogether.
    if (timeout == INT64_MAX) {
        buffer->terminate = true;
        return ETIMEDOUT;
    }
    assert_true(timeout > 0);
    device.time_ns += timeout;
    int queued = device.size / ao->sstride - device.head;
    if (device.play_calls && queued >= device.capacity)
        device.output_started = true;
    if (device.output_started) {
        int consumed = timeout * ao->samplerate / 1000000000;
        // An empty device makes the real driver report playing=false.
        if (consumed >= queued)
            device.underruns++;
        device.head += MPMIN(consumed, queued);
    }
    if (++device.waits == 64)
        buffer->terminate = true;
    return ETIMEDOUT;
}

static struct mp_frame test_buffer_pin_read(struct mp_pin *pin)
{
    assert_true(device.core_ao != NULL);
    assert_true(device.core_reading);
    device.read_calls++;
    if (!device.core_frames) {
        if (device.core_eof) {
            device.core_eof = false;
            return MP_EOF_FRAME;
        }
        return MP_NO_FRAME;
    }
    if (device.core_frames > 0)
        device.core_frames--;
    struct mp_aframe *frame = mp_aframe_create();
    struct mp_chmap channels = MP_CHMAP_INIT_STEREO;
    assert_true(mp_aframe_set_format(frame, AF_FORMAT_FLOAT));
    assert_true(mp_aframe_set_rate(frame, 48000));
    assert_true(mp_aframe_set_chmap(frame, &channels));
    assert_true(mp_aframe_alloc_data(frame, device.core_samples));
    float *data = (float *)mp_aframe_get_data_rw(frame)[0];
    for (int n = 0; n < device.core_samples * 2; n++)
        data[n] = 0.25f;
    return MAKE_FRAME(MP_FRAME_AUDIO, frame);
}

static void test_buffer_frame_unref(struct mp_frame *frame)
{
    assert_true(frame->type == MP_FRAME_EOF || frame->type == MP_FRAME_NONE);
    *frame = MP_NO_FRAME;
}

static void test_buffer_post_process(struct ao *ao, void **data, int samples)
{
    assert_true(af_fmt_is_pcm(ao->format));
}

static void test_buffer_wakeup(void *ctx)
{
}

// The scripted filter source is the queue boundary. Reset must discard its
// queued frames and EOF marker before reading is resumed by the real core.
static void check_core_queue(struct mp_async_queue *queue)
{
    assert_true(device.core_ao != NULL);
    struct buffer_state *buffer = device.core_ao->buffer_state;
    assert_true(queue != NULL && queue == buffer->queue);
}

int64_t mp_async_queue_get_samples(struct mp_async_queue *queue)
{
    check_core_queue(queue);
    assert_true(device.core_frames >= 0);
    return (int64_t)device.core_frames * device.core_samples;
}

void mp_async_queue_reset(struct mp_async_queue *queue)
{
    check_core_queue(queue);
    device.core_frames = 0;
    device.core_eof = false;
    device.core_reading = false;
    device.core_queue_resets++;
}

void mp_async_queue_resume_reading(struct mp_async_queue *queue)
{
    check_core_queue(queue);
    device.core_reading = true;
    device.core_resumes++;
}

void mp_filter_reset(struct mp_filter *filter)
{
    assert_true(device.core_ao != NULL);
    struct buffer_state *buffer = device.core_ao->buffer_state;
    assert_true(filter != NULL && filter == buffer->filter_root);
    assert_true(buffer->pending == NULL);
    device.core_filter_resets++;
}

int64_t test_time_ns(void)
{
    return device.time_ns;
}

uint64_t test_raw_time_ns(void)
{
    return device.time_ns;
}

// Legacy writer init tests keep AudioTrack paused and must not read audio.
int ao_read_data(struct ao *ao, void **data, int samples, int64_t end_time,
                 bool *eof, bool pad_silence, bool blocking)
{
    abort();
}

void test_request_reload(struct ao *ao)
{
    device.reloads++;
}

static jobject JNICALL new_buffer(JNIEnv *env, void *data, jlong size)
{
    struct test_buffer *buffer = malloc(sizeof(*buffer));
    *buffer = (struct test_buffer){.data = data, .size = size, .refs = 1};
    device.buffers++;
    return (jobject)buffer;
}

static jobject JNICALL new_object(JNIEnv *env, jclass clazz, jmethodID method, ...)
{
    switch ((uintptr_t)method) {
    case METHOD_NEW_TIMESTAMP:
        init_fixture.timestamp_calls++;
        if (init_fixture.fail_timestamp)
            return NULL;
        break;
    case METHOD_NEW_BUILDER:
        break;
    case METHOD_NEW_TRACK:
        init_fixture.track_calls++;
        break;
    default:
        abort();
    }
    return new_buffer(env, NULL, 0);
}

static jshortArray JNICALL new_short_array(JNIEnv *env, jsize size)
{
    return (jshortArray)new_buffer(env, NULL, size * sizeof(jshort));
}

static jint JNICALL call_static_int(JNIEnv *env, jclass clazz, jmethodID method, ...)
{
    assert_int_equal((uintptr_t)method, METHOD_MIN_BUFFER_SIZE);
    va_list args;
    va_start(args, method);
    assert_int_equal(va_arg(args, int), 48000);
    assert_int_equal(va_arg(args, int), AudioFormat.CHANNEL_OUT_STEREO);
    assert_int_equal(va_arg(args, int), init_fixture.encoding);
    va_end(args);
    return 4096;
}

static jobject JNICALL new_ref(JNIEnv *env, jobject object)
{
    ((struct test_buffer *)object)->refs++;
    return object;
}

static void JNICALL delete_ref(JNIEnv *env, jobject object)
{
    struct test_buffer *buffer = (void *)object;
    if (--buffer->refs == 0) {
        free(buffer);
        device.buffers--;
    }
}

static jobject JNICALL call_object(JNIEnv *env, jobject object, jmethodID method, ...)
{
    if ((uintptr_t)method == METHOD_BUILD)
        return new_buffer(env, NULL, 0);
    if ((uintptr_t)method >= METHOD_SET_ENCODING &&
        (uintptr_t)method <= METHOD_SET_USAGE)
        return new_ref(env, object);
    assert_int_equal((uintptr_t)method, METHOD_POSITION);
    va_list args;
    va_start(args, method);
    struct test_buffer *buffer = (void *)object;
    buffer->position = va_arg(args, int);
    va_end(args);
    assert_true(buffer->position >= 0 && buffer->position <= buffer->size);
    return new_ref(env, object);
}

static jint JNICALL call_int(JNIEnv *env, jobject object, jmethodID method, ...)
{
    if (device.fail_method && (uintptr_t)method == device.fail_method)
        device.exception = true;
    if ((uintptr_t)method == METHOD_HEAD)
        return device.head;
    if ((uintptr_t)method == METHOD_STATE)
        return init_fixture.running ? AudioTrack.PLAYSTATE_PAUSED :
                                      AudioTrack.PLAYSTATE_PLAYING;
    if ((uintptr_t)method == METHOD_INITIALIZED)
        return AudioTrack.STATE_INITIALIZED;
    if ((uintptr_t)method == METHOD_LATENCY)
        return 0;
    if ((uintptr_t)method == METHOD_CAPACITY)
        return device.capacity;

    assert_int_equal((uintptr_t)method, METHOD_WRITE);
    assert_false(device.stopped);
    va_list args;
    va_start(args, method);
    struct test_buffer *buffer = (void *)va_arg(args, jobject);
    int size = va_arg(args, int);
    assert_int_equal(va_arg(args, int), AudioTrack.WRITE_NON_BLOCKING);
    va_end(args);
    assert_true(size <= buffer->size - buffer->position);
    device.write_calls++;
    if (device.write_limit < 0)
        return device.write_limit;
    int accepted = MPMIN(size, device.write_limit);
    if (device.core_ao) {
        int queued = device.size - device.flushed_bytes -
                     device.head * device.core_ao->sstride;
        int free = device.capacity * device.core_ao->sstride - queued;
        assert_true(free >= 0);
        accepted = MPMIN(accepted, free);
    }
    if (!device.core_ao || device.size + accepted <= sizeof(device.bytes)) {
        assert_true(device.size + accepted <= sizeof(device.bytes));
        memcpy(device.bytes + device.size, buffer->data + buffer->position, accepted);
    }
    device.size += accepted;
    buffer->position += accepted;
    return accepted;
}

static jboolean JNICALL call_boolean(JNIEnv *env, jobject object, jmethodID method, ...)
{
    assert_int_equal((uintptr_t)method, METHOD_TIMESTAMP);
    return false;
}

static jlong JNICALL get_long(JNIEnv *env, jobject object, jfieldID field)
{
    return 0;
}

static void JNICALL call_void(JNIEnv *env, jobject object, jmethodID method, ...)
{
    if (device.fail_method && (uintptr_t)method == device.fail_method)
        device.exception = true;
    switch ((uintptr_t)method) {
    case METHOD_PAUSE:
        assert_false(device.stopped);
        device.pause_calls++;
        break;
    case METHOD_PLAY:
        assert_false(device.stopped);
        device.play_calls++;
        break;
    case METHOD_FLUSH:
        device.flush_calls++;
        device.stopped = false;
        device.head = 0;
        device.flushed_bytes = device.size;
        break;
    case METHOD_STOP:
        device.stop_calls++;
        device.stopped = true;
        device.head = 0;
        break;
    case METHOD_RELEASE:
        init_fixture.release_calls++;
        break;
    default:
        abort();
    }
}

static const struct JNINativeInterface mock_jni = {
    .NewObject = new_object,
    .NewShortArray = new_short_array,
    .NewDirectByteBuffer = new_buffer,
    .NewGlobalRef = new_ref,
    .DeleteGlobalRef = delete_ref,
    .DeleteLocalRef = delete_ref,
    .CallObjectMethod = call_object,
    .CallIntMethod = call_int,
    .CallStaticIntMethod = call_static_int,
    .CallBooleanMethod = call_boolean,
    .GetLongField = get_long,
    .CallVoidMethod = call_void,
};
static JNIEnv mock_env = &mock_jni;

JNIEnv *mp_jni_get_env(struct mp_log *log)
{
    return &mock_env;
}

int mp_jni_exception_check(JNIEnv *env, int logging, struct mp_log *log)
{
    if (device.exception) {
        device.exception = false;
        device.exceptions++;
        return -1;
    }
    return 0;
}

int mp_jni_init_jfields(JNIEnv *env, void *fields,
                        const struct MPJniField *mapping, int global,
                        struct mp_log *log)
{
    assert_int_equal(global, 1);
    if (++init_fixture.discovery_calls == init_fixture.fail_discovery_at)
        return -1;
    // Supply one owned class reference per successful discovery. Method and
    // field values are configured by the fixture for the requested API path.
    assert_int_equal(mapping[0].type, MP_JNI_CLASS);
    jclass *clazz = (void *)((uint8_t *)fields + mapping[0].offset);
    assert_true(*clazz == NULL);
    *clazz = (jclass)new_buffer(env, NULL, 0);
    return 0;
}

static struct ao make_ao(struct priv *p)
{
    assert_int_equal(device.buffers, 0);
    device = (struct test_device){.time_ns = 1000000000,
                                 .write_limit = INT_MAX, .capacity = 7200,
                                 .core_frames = -1, .core_samples = 7200};
    *p = (struct priv){.audiotrack = (jobject)&device};
    ByteBuffer.position = (jmethodID)METHOD_POSITION;
    AudioTrack.getPlaybackHeadPosition = (jmethodID)METHOD_HEAD;
    AudioTrack.writeBufferV21 = (jmethodID)METHOD_WRITE;
    AudioTrack.pause = (jmethodID)METHOD_PAUSE;
    AudioTrack.play = (jmethodID)METHOD_PLAY;
    AudioTrack.flush = (jmethodID)METHOD_FLUSH;
    AudioTrack.stop = (jmethodID)METHOD_STOP;
    AudioTrack.getPlayState = (jmethodID)METHOD_STATE;
    AudioTrack.getLatency = (jmethodID)METHOD_LATENCY;
    AudioTrack.getTimestamp = (jmethodID)METHOD_TIMESTAMP;
    AudioTrack.getBufferSizeInFramesV23 = (jmethodID)METHOD_CAPACITY;
    AudioTrack.PLAYSTATE_PLAYING = 3;
    AudioTrack.WRITE_BLOCKING = 0;
    AudioTrack.WRITE_NON_BLOCKING = 1;
    AudioTrack.ERROR = -1;
    AudioFormat.ENCODING_PCM_FLOAT = 4;
    AudioFormat.ENCODING_IEC61937 = 13;
    return (struct ao){.priv = p, .samplerate = 48000, .format = AF_FORMAT_RAW_DTSHD};
}

static struct mp_aframe *make_frame(int bytes, int samples, int value)
{
    struct mp_aframe *frame = mp_aframe_create();
    struct mp_chmap channels = MP_CHMAP_INIT_STEREO;
    assert_true(mp_aframe_set_format(frame, AF_FORMAT_RAW_DTSHD));
    assert_true(mp_aframe_set_rate(frame, 48000));
    assert_true(mp_aframe_set_chmap(frame, &channels));
    uint8_t data[8192];
    assert_true(bytes <= sizeof(data));
    memset(data, value, bytes);
    assert_true(mp_aframe_set_encoded_data(frame, data, bytes, samples));
    return frame;
}

static void write_frame(struct ao *ao, int bytes, int samples, int value)
{
    struct mp_aframe *frame = make_frame(bytes, samples, value);
    assert_true(encoded_write(ao, (void **)&frame, 1));
    // The AO must retain its own reference after the queue releases this one.
    talloc_free(frame);
}

static void test_partial_write(void)
{
    struct priv p;
    struct ao ao = make_ao(&p);
    struct mp_pcm_state state;
    device.write_limit = 1000;
    write_frame(&ao, 6544, 512, 0x5a);
    assert_int_equal(p.encoded_written, 0);
    assert_int_equal(p.encoded_offset, 1000);
    encoded_start(&ao);

    device.write_limit = 0;
    encoded_get_state(&ao, &state);
    assert_int_equal(state.free_samples, 0);
    assert_int_equal(state.queued_samples, 512);
    assert_float_equal(state.delay, 512.0 / 48000, 1e-12);
    assert_true(encoded_set_pause(&ao, true));
    device.write_limit = INT_MAX;
    int calls = device.write_calls;
    encoded_get_state(&ao, &state);
    assert_int_equal(device.write_calls, calls);
    assert_true(encoded_set_pause(&ao, false));
    encoded_get_state(&ao, &state);
    assert_int_equal(device.size, 6544);
    assert_int_equal(p.encoded_written, 512);
    assert_int_equal(device.buffers, 0);
    for (int n = 0; n < device.size; n++)
        assert_int_equal(device.bytes[n], 0x5a);

    // A different byte length with the same decoded duration advances the
    // audio clock by the same number of samples.
    write_frame(&ao, 1000, 512, 0xa5);
    device.head = 512;
    encoded_get_state(&ao, &state);
    assert_int_equal(p.encoded_written, 1024);
    assert_int_equal(state.queued_samples, 512);
    assert_float_equal(state.delay, 512.0 / 48000, 1e-12);
    encoded_reset(&ao);
}

static void test_reset_partial_write(void)
{
    struct priv p;
    struct ao ao = make_ao(&p);
    device.write_limit = 17;
    write_frame(&ao, 1000, 512, 0x11);
    encoded_reset(&ao);
    assert_int_equal(device.buffers, 0);
    assert_int_equal(p.encoded_offset, 0);
    assert_int_equal(p.encoded_written, 0);
    device.write_limit = INT_MAX;
    write_frame(&ao, 123, 512, 0x22);
    assert_int_equal(device.size, 140);
    for (int n = 17; n < device.size; n++)
        assert_int_equal(device.bytes[n], 0x22);
    encoded_reset(&ao);
}

static void test_short_eos_and_pause(void)
{
    struct priv p;
    struct ao ao = make_ao(&p);
    struct mp_pcm_state state;
    write_frame(&ao, 100, 480, 0x33);
    encoded_start(&ao);
    encoded_drain(&ao);
    assert_int_equal(device.stop_calls, 1);
    assert_int_equal(device.size, 100);
    device.time_ns += 2000000;
    encoded_get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 384);
    assert_true(state.playing);
    assert_true(encoded_set_pause(&ao, true));
    device.time_ns += 2000000;
    encoded_get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 288);
    assert_int_equal(device.pause_calls, 0);
    assert_true(encoded_set_pause(&ao, false));
    assert_int_equal(device.play_calls, 1);
    device.time_ns += 6000000;
    encoded_get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 0);
    assert_false(state.playing);
    assert_int_equal(device.size, 100);

    // Reusing the stopped track starts a new timeline only after all old
    // audio has drained. A new write flushes it before start() plays again.
    assert_int_equal(state.free_samples, 1);
    write_frame(&ao, 123, 240, 0x55);
    assert_false(p.encoded_draining);
    assert_false(p.encoded_started);
    assert_int_equal(device.flush_calls, 1);
    assert_int_equal(device.pause_calls, 0);
    encoded_start(&ao);
    assert_int_equal(device.play_calls, 2);
    device.head = 120;
    encoded_get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 120);
    encoded_reset(&ao);
    assert_false(p.encoded_draining);
}

static void test_playhead_wrap_and_write_error(void)
{
    struct priv p;
    struct ao ao = make_ao(&p);
    struct mp_pcm_state state;
    p.encoded_started = true;
    p.encoded_written = 48000 * 5;
    encoded_get_state(&ao, &state);
    assert_float_equal(state.delay, 5.0, 1e-12);
    p.encoded_playhead = UINT32_MAX - 10;
    device.head = 15;
    assert_int_equal(encoded_get_playhead(&ao), (uint64_t)UINT32_MAX + 16);
    encoded_reset(&ao);
    device.write_limit = -6; // AudioTrack.ERROR_DEAD_OBJECT
    struct mp_aframe *frame = make_frame(100, 512, 0x44);
    assert_false(encoded_write(&ao, (void **)&frame, 1));
    talloc_free(frame);
    assert_int_equal(device.reloads, 1);
    encoded_write_pending(&ao);
    assert_int_equal(device.write_calls, 1);
    assert_int_equal(device.reloads, 1);
    encoded_reset(&ao);
    assert_int_equal(device.buffers, 0);
}

static const struct ao_driver pcm_driver = {
    .name = "test-audiotrack",
    .write = pcm_write,
    .get_state = pcm_get_state,
    .start = pcm_start,
    .set_pause = pcm_set_pause,
    .reset = pcm_reset,
    .drain = pcm_drain,
};

static struct ao make_pcm_ao(struct priv *p)
{
    struct ao ao = make_ao(p);
    // Only the runtime callbacks belong in this fixture. Keeping the public
    // descriptor would also retain unrelated init/options and JNI discovery.
    ao.driver = &pcm_driver;
    ao.format = AF_FORMAT_FLOAT;
    ao.sstride = 8;
    ao.num_planes = 1;
    ao.device_buffer = 7200;
    p->format = AudioFormat.ENCODING_PCM_FLOAT;
    p->chunksize = 7200 * ao.sstride;
    p->chunk = talloc_size(NULL, p->chunksize);
    p->bbuf = new_buffer(&mock_env, p->chunk, p->chunksize);
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->wakeup);
    return ao;
}

static void free_pcm_ao(struct ao *ao)
{
    struct priv *p = ao->priv;
    ao->driver->reset(ao);
    delete_ref(&mock_env, p->bbuf);
    talloc_free(p->chunk);
    mp_cond_destroy(&p->wakeup);
    mp_mutex_destroy(&p->lock);
}

static void test_pcm_does_not_write_read_ahead_silence(void)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    struct mp_pcm_state state;
    uint8_t pcm[7200 * 8];
    memset(pcm, 0x5a, sizeof(pcm));
    void *data = pcm;

    // A push AO accepts only the actual source data. Capacity must
    // prevent a second read until the first chunk has advanced.
    assert_true(ao.driver->write(&ao, &data, 7200));
    ao.driver->start(&ao);
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.free_samples, 0);
    assert_true(state.playing);
    device.head = 2400;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.free_samples, 2400);
    assert_true(ao.driver->write(&ao, &data, 2400));
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 7200);
    assert_float_equal(state.delay, 7200.0 / 48000, 1e-9);

    free_pcm_ao(&ao);
    // No driver may turn the 9600 source frames into 14400 output frames.
    assert_int_equal(device.size, 9600 * ao.sstride);
    for (int n = 0; n < device.size; n++)
        assert_int_equal(device.bytes[n], 0x5a);
}

static void test_pcm_partial_zero_write_and_pause(void)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    struct mp_pcm_state state;
    uint8_t pcm[800];
    for (int n = 0; n < sizeof(pcm); n++)
        pcm[n] = n;
    uint8_t expected[sizeof(pcm)];
    memcpy(expected, pcm, sizeof(pcm));
    void *data = pcm;

    device.write_limit = 11; // Split a complete stereo frame across writes.
    assert_true(ao.driver->write(&ao, &data, 100));
    memset(pcm, 0xee, sizeof(pcm)); // The core releases/reuses its input buffer.
    assert_int_equal(p.pcm_written, 1);
    assert_int_equal(p.pending_bytes, 789);
    assert_int_equal(device.play_calls, 0); // write must not start the track.
    ao.driver->start(&ao);
    device.write_limit = 0;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.free_samples, 0);
    assert_int_equal(state.queued_samples, 100);
    assert_float_equal(state.delay, 100.0 / 48000, 1e-12);

    assert_true(ao.driver->set_pause(&ao, true));
    int writes = device.write_calls;
    device.write_limit = INT_MAX;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(device.write_calls, writes);
    assert_int_equal(state.queued_samples, 100);
    assert_true(ao.driver->set_pause(&ao, false));
    ao.driver->get_state(&ao, &state);
    assert_int_equal(device.size, sizeof(pcm));
    assert_memcmp(device.bytes, expected, sizeof(expected));
    assert_int_equal(p.pcm_written, 100);
    assert_int_equal(p.pending_bytes, 0);
    assert_int_equal(state.queued_samples, 100);
    device.head = 100;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 0);
    assert_false(state.playing); // Real AO queue exhaustion stays visible.
    free_pcm_ao(&ao);
}

static void test_pcm_reset_discards_old_partial_data(void)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    uint8_t old_data[800], new_data[80];
    memset(old_data, 0x11, sizeof(old_data));
    memset(new_data, 0x22, sizeof(new_data));
    void *data = old_data;
    device.write_limit = 11;
    assert_true(ao.driver->write(&ao, &data, 100));
    ao.driver->reset(&ao);
    assert_int_equal(p.pending_bytes, 0);
    assert_int_equal(p.pending_total, 0);
    assert_int_equal(p.pcm_written, 0);
    assert_int_equal(p.pcm_playhead, 0);
    device.write_limit = INT_MAX;
    data = new_data;
    assert_true(ao.driver->write(&ao, &data, 10));
    assert_int_equal(device.size, 11 + sizeof(new_data));
    assert_memcmp(device.bytes + 11, new_data, sizeof(new_data));
    free_pcm_ao(&ao);
}

static void test_pcm_short_eof_with_pending_write(void)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    struct mp_pcm_state state;
    uint8_t pcm[480 * 8] = {1};
    void *data = pcm;
    device.write_limit = 11;
    assert_true(ao.driver->write(&ao, &data, 480));
    ao.driver->start(&ao);
    ao.driver->drain(&ao);
    assert_int_equal(device.stop_calls, 0); // Never stop before pending is sent.
    device.write_limit = INT_MAX;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(device.stop_calls, 1);
    assert_memcmp(device.bytes, pcm, sizeof(pcm));
    assert_int_equal(state.queued_samples, 480);
    assert_float_equal(state.delay, .010, 1e-12);
    device.time_ns += 10000000;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 0);
    assert_false(state.playing);
    assert_int_equal(state.free_samples, 7200);
    assert_true(ao.driver->write(&ao, &data, 480));
    assert_false(p.pcm_draining);
    assert_false(p.pcm_started);
    ao.driver->start(&ao);
    assert_int_equal(device.play_calls, 2);
    free_pcm_ao(&ao);
}

static void test_pcm_playhead_wrap_and_write_error(void)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    struct mp_pcm_state state;
    p.pcm_started = true;
    p.pcm_playhead = UINT32_MAX - 10;
    p.pcm_written = (uint64_t)UINT32_MAX + 100;
    p.written_frames = p.pcm_written;
    device.head = 15;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(p.pcm_playhead, (uint64_t)UINT32_MAX + 16);
    assert_int_equal(state.queued_samples, 84);
    ao.driver->reset(&ao);

    uint8_t pcm[80] = {1};
    void *data = pcm;
    device.write_limit = -6;
    assert_false(ao.driver->write(&ao, &data, 10));
    assert_int_equal(device.reloads, 1);
    int calls = device.write_calls;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(device.write_calls, calls);
    assert_false(state.playing);
    assert_int_equal(device.reloads, 1);
    free_pcm_ao(&ao);
}

static void test_pcm_capacity_larger_than_chunk(void)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    struct mp_pcm_state state;
    uint8_t pcm[7200 * 8] = {1};
    void *data = pcm;
    device.capacity = 9600;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.free_samples, 7200); // One write fits our chunk.
    assert_true(ao.driver->write(&ao, &data, 7200));
    ao.driver->start(&ao);
    // Playback need not advance before Android's full start threshold.
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.free_samples, 2400);
    assert_true(ao.driver->write(&ao, &data, 2400));
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.free_samples, 0);
    assert_int_equal(state.queued_samples, 9600);
    assert_float_equal(state.delay, .2, 1e-12);

    device.capacity = 14400; // Routing can enlarge the effective buffer.
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.free_samples, 4800);
    assert_true(ao.driver->write(&ao, &data, 4800));
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 14400);
    assert_int_equal(state.free_samples, 0);
    device.head = 14400;
    ao.driver->get_state(&ao, &state);
    assert_int_equal(state.queued_samples, 0);
    assert_false(state.playing);
    free_pcm_ao(&ao);
}

static void test_pcm_zero_write_keeps_owned_audio(void)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    struct mp_pcm_state state;
    uint8_t pcm[800] = {1};
    void *data = pcm;
    device.write_limit = 0;
    assert_true(ao.driver->write(&ao, &data, 100));
    ao.driver->start(&ao);
    ao.driver->get_state(&ao, &state);
    assert_int_equal(device.size, 0);
    assert_int_equal(state.queued_samples, 100);
    assert_int_equal(state.free_samples, 0);
    // The AO still owns data and must remain eligible for the core's refill
    // loop. This state is not proof that the platform has output a sample.
    assert_true(state.playing);
    device.write_limit = INT_MAX;
    ao.driver->get_state(&ao, &state);
    assert_memcmp(device.bytes, pcm, sizeof(pcm));
    assert_int_equal(p.pending_bytes, 0);
    device.head = 100;
    ao.driver->get_state(&ao, &state);
    assert_false(state.playing);
    free_pcm_ao(&ao);
}

struct pcm_core_test {
    struct priv p;
    struct ao ao;
    struct mp_pin *pin;
    struct mp_filter input;
    struct buffer_state buffer;
};

static void init_pcm_core_test(struct pcm_core_test *test)
{
    *test = (struct pcm_core_test){
        .input = {.pins = &test->pin},
        .buffer = {.playing = true, .input = &test->input,
                   .filter_root = &test->input,
                   .queue = (struct mp_async_queue *)test},
    };
    test->ao = make_pcm_ao(&test->p);
    mp_mutex_init(&test->buffer.lock);
    mp_mutex_init(&test->buffer.pt_lock);
    mp_cond_init(&test->buffer.wakeup);
    mp_cond_init(&test->buffer.pt_wakeup);
    test->ao.buffer_state = &test->buffer;
    test->ao.wakeup_cb = test_buffer_wakeup;
    test->ao.channels = (struct mp_chmap)MP_CHMAP_INIT_STEREO;
    device.core_ao = &test->ao;
    device.core_reading = true;
}

static void uninit_pcm_core_test(struct pcm_core_test *test)
{
    talloc_free(test->buffer.pending);
    talloc_free(test->buffer.temp_buf);
    mp_cond_destroy(&test->buffer.pt_wakeup);
    mp_cond_destroy(&test->buffer.wakeup);
    mp_mutex_destroy(&test->buffer.pt_lock);
    mp_mutex_destroy(&test->buffer.lock);
    device.core_ao = NULL;
    free_pcm_ao(&test->ao);
}

static void test_pcm_core_refill_large_capacity(void)
{
    struct pcm_core_test test;
    init_pcm_core_test(&test);
    device.capacity = 57600; // Eight write chunks; Android enlarged the buffer.

    // A continuous source must survive real core scheduling after reaching
    // the start threshold. Polling at capacity / rate / 4 consumes 14400
    // frames between writes of at most 7200 and eventually underruns.
    buffer_ao_thread(&test.ao);
    assert_int_equal(device.underruns, 0);
    assert_int_equal(device.waits, 64);
    assert_true(device.output_started);
    assert_true(device.head > device.capacity);
    assert_true(test.buffer.playing);
    assert_int_equal(device.play_calls, 1);
    assert_int_equal(device.reloads, 0);

    uninit_pcm_core_test(&test);
}

static void test_pcm_core_eof_continuation(bool pause_pending, bool pause_draining)
{
    struct pcm_core_test test;
    init_pcm_core_test(&test);
    device.core_frames = 1;
    device.core_samples = 480;
    device.core_eof = true;
    device.write_limit = pause_pending ? 11 : INT_MAX;

    // Execute the real read_buffer -> PCM write -> start -> drain sequence.
    // The final 10 ms tail is shorter than the AudioTrack start threshold.
    assert_true(ao_play_data(&test.ao));
    assert_true(test.buffer.playing);
    assert_true(test.buffer.streaming);
    assert_int_equal(device.play_calls, 1);
    assert_int_equal(device.stop_calls, pause_pending ? 0 : 1);

    if (pause_pending) {
        device.write_limit = 0;
        ao_set_paused(&test.ao, true, false);
        int writes = device.write_calls;
        int reads = device.read_calls;
        assert_true(test.buffer.paused);
        assert_true(test.p.pcm_paused);
        assert_int_equal(test.p.pending_bytes, 480 * 8 - 11);
        device.write_limit = INT_MAX;
        device.time_ns += 100000000;
        assert_false(ao_play_data(&test.ao));
        assert_int_equal(device.write_calls, writes);
        assert_int_equal(device.read_calls, reads);
        assert_int_equal(device.stop_calls, 0);
        ao_set_paused(&test.ao, false, false);
        assert_false(ao_play_data(&test.ao));
        assert_int_equal(test.p.pending_bytes, 0);
        assert_int_equal(device.pause_calls, 1);
        assert_int_equal(device.stop_calls, 1);
    }

    if (pause_draining) {
        int pauses = device.pause_calls;
        int plays = device.play_calls;
        ao_set_paused(&test.ao, true, false);
        device.time_ns += 5000000;
        assert_false(ao_play_data(&test.ao));
        ao_set_paused(&test.ao, false, false);
        // STREAM stop() continues draining. Pause/resume must not change
        // the stopped AudioTrack or restart the tail's elapsed-time clock.
        assert_int_equal(device.pause_calls, pauses);
        assert_int_equal(device.play_calls, plays);
        assert_false(ao_play_data(&test.ao));
        assert_true(test.buffer.playing);
        struct mp_pcm_state state;
        pcm_get_state(&test.ao, &state);
        assert_int_equal(state.queued_samples, 240);
        assert_float_equal(state.delay, .005, 1e-12);
    }

    device.time_ns += 10000000;
    assert_true(ao_play_data(&test.ao));
    assert_false(test.buffer.playing);
    assert_false(test.buffer.streaming);
    assert_int_equal(device.size, 480 * 8);
    assert_int_equal(device.stop_calls, 1);

    device.core_frames = 1;
    device.core_eof = true;
    ao_start(&test.ao);
    assert_true(ao_play_data(&test.ao));
    assert_true(test.buffer.playing);
    assert_true(test.buffer.streaming);
    assert_int_equal(device.flush_calls, 1);
    assert_int_equal(device.play_calls, pause_pending ? 3 : 2);
    assert_int_equal(device.stop_calls, 2);
    assert_int_equal(device.size, 2 * 480 * 8);
    for (int n = 0; n < device.size; n += sizeof(float)) {
        float sample;
        memcpy(&sample, device.bytes + n, sizeof(sample));
        assert_float_equal(sample, 0.25f, 0);
    }
    device.time_ns += 10000000;
    assert_true(ao_play_data(&test.ao));
    assert_false(test.buffer.playing);
    assert_false(test.buffer.streaming);
    assert_int_equal(device.reloads, 0);
    uninit_pcm_core_test(&test);
}

static void test_pcm_core_reset_while_paused(void)
{
    struct pcm_core_test test;
    init_pcm_core_test(&test);
    device.core_frames = 1;
    device.core_samples = 4800;
    assert_true(ao_play_data(&test.ao));
    device.head = 2400;
    ao_set_paused(&test.ao, true, false);
    assert_float_equal(ao_get_delay(&test.ao), .05, 1e-12);

    // The old source still has data and EOF queued when a paused seek resets
    // the AO. Exercise the real queue/filter reset and PCM reset call flow.
    device.core_frames = 2;
    device.core_eof = true;
    assert_float_equal(ao_get_delay(&test.ao), .25, 1e-12);
    ao_reset(&test.ao);
    assert_int_equal(device.core_queue_resets, 1);
    assert_int_equal(device.core_filter_resets, 1);
    assert_int_equal(device.core_resumes, 1);
    assert_int_equal(device.core_frames, 0);
    assert_false(device.core_eof);
    assert_true(test.buffer.paused);
    assert_float_equal(ao_get_delay(&test.ao), 0, 0);

    // Starting the new stream while still paused must not resurrect the
    // cached delay from the audio discarded by reset.
    ao_start(&test.ao);
    assert_true(test.buffer.playing);
    assert_true(test.buffer.paused);
    assert_float_equal(ao_get_delay(&test.ao), 0, 0);
    device.core_frames = 1;
    device.core_samples = 960;
    device.core_eof = true;
    assert_float_equal(ao_get_delay(&test.ao), .02, 1e-12);

    ao_set_paused(&test.ao, false, false);
    assert_true(ao_play_data(&test.ao));
    assert_int_equal(device.play_calls, 2);
    assert_int_equal(device.stop_calls, 1);
    device.time_ns += 20000000;
    assert_true(ao_play_data(&test.ao));
    assert_false(test.buffer.playing);
    assert_float_equal(ao_get_delay(&test.ao), 0, 0);
    assert_int_equal(device.reloads, 0);
    uninit_pcm_core_test(&test);
}

static void test_pcm_jni_exception_requests_one_reload(int method)
{
    struct priv p;
    struct ao ao = make_pcm_ao(&p);
    struct mp_pcm_state state;
    uint8_t pcm[80] = {1};
    void *data = pcm;
    device.fail_method = method;
    if (method == METHOD_WRITE) {
        assert_false(ao.driver->write(&ao, &data, 10));
    } else if (method == METHOD_PAUSE) {
        assert_false(ao.driver->set_pause(&ao, true));
    } else {
        assert_int_equal(method, METHOD_CAPACITY);
        ao.driver->get_state(&ao, &state);
    }
    assert_int_equal(device.exceptions, 1);
    assert_true(p.pcm_failed);
    assert_int_equal(device.reloads, 1);
    int writes = device.write_calls;
    // Repeated callbacks after the fault cannot write more audio or request
    // another reload. Capacity lookup itself may continue to throw.
    for (int n = 0; n < 3; n++) {
        ao.driver->get_state(&ao, &state);
        ao.driver->start(&ao);
        assert_false(state.playing);
        assert_int_equal(state.free_samples, 0);
    }
    assert_int_equal(device.write_calls, writes);
    assert_int_equal(device.reloads, 1);
    device.fail_method = 0;
    free_pcm_ao(&ao);
    assert_int_equal(device.reloads, 1);
}

static struct ao *make_init_ao(void)
{
    assert_int_equal(jni_static_use_count, 0);
    struct ao *ao = talloc_zero(NULL, struct ao);
    struct priv *p = talloc_zero(ao, struct priv);
    *ao = make_ao(p);
    p->audiotrack = NULL; // init() has not constructed a track yet.
    ao->driver = &pcm_driver;
    ao->format = AF_FORMAT_S_AC3;
    ao->channels = (struct mp_chmap)MP_CHMAP_INIT_STEREO;
    AudioFormat.CHANNEL_OUT_STEREO = 12;
    AudioTrack.getMinBufferSize = (jmethodID)METHOD_MIN_BUFFER_SIZE;
    AudioTimestamp.ctor = (jmethodID)METHOD_NEW_TIMESTAMP;
    init_fixture = (struct init_fixture){
        .mutex = &p->lock,
        .cond = &p->wakeup,
        .encoding = AudioFormat.ENCODING_IEC61937,
    };
    return ao;
}

static void acquire_existing_jni_owner(struct ao *ao)
{
    assert_int_equal(init_jni(ao), 0);
    assert_int_equal(jni_static_use_count, 1);
    assert_int_equal(init_fixture.discovery_calls, MP_ARRAY_SIZE(jclass_list));
    assert_int_equal(device.buffers, MP_ARRAY_SIZE(jclass_list));
}

static void check_failed_init_with_existing_owner(struct ao *ao)
{
    assert_int_equal(jni_static_use_count, 1);
    assert_int_equal(init_fixture.discovery_calls, MP_ARRAY_SIZE(jclass_list));
    assert_int_equal(device.buffers, MP_ARRAY_SIZE(jclass_list));
    assert_int_equal(init_fixture.mutex_destroys, 1);
    assert_int_equal(init_fixture.cond_destroys, 1);
    assert_int_equal(device.stop_calls, 0);
    assert_int_equal(device.flush_calls, 0);
    // Release the pre-existing owner through the real cleanup implementation.
    uninit_jni(ao);
    assert_int_equal(jni_static_use_count, 0);
    assert_int_equal(device.buffers, 0);
    init_fixture.mutex = NULL;
    init_fixture.cond = NULL;
    talloc_free(ao);
}

static void test_init_unsupported_iec_preserves_existing_owner(void)
{
    struct ao *ao = make_init_ao();
    acquire_existing_jni_owner(ao);
    AudioTrack.writeShortV23 = NULL;
    assert_int_equal(init(ao), -1);
    assert_int_equal(init_fixture.timestamp_calls, 0);
    check_failed_init_with_existing_owner(ao);
}

static void test_init_timestamp_failure_preserves_existing_owner(void)
{
    struct ao *ao = make_init_ao();
    acquire_existing_jni_owner(ao);
    AudioTrack.writeShortV23 = (jmethodID)METHOD_WRITE;
    init_fixture.fail_timestamp = true;
    assert_int_equal(init(ao), -1);
    assert_int_equal(init_fixture.timestamp_calls, 1);
    check_failed_init_with_existing_owner(ao);
}

static void test_init_discovery_failure_does_not_release_unowned_jni(void)
{
    struct ao *ao = make_init_ao();
    // Fail after acquiring a class reference, so real reset_jfields must
    // release that partial discovery without decrementing an unowned count.
    init_fixture.fail_discovery_at = 2;
    assert_int_equal(init(ao), -1);
    assert_int_equal(init_fixture.discovery_calls, 2);
    assert_int_equal(jni_static_use_count, 0);
    assert_int_equal(device.buffers, 0);
    assert_int_equal(init_fixture.mutex_destroys, 1);
    assert_int_equal(init_fixture.cond_destroys, 1);
    assert_int_equal(init_fixture.timestamp_calls, 0);
    init_fixture.mutex = NULL;
    init_fixture.cond = NULL;
    talloc_free(ao);
}

static void test_init_driver_selection(int api, int format)
{
    struct ao *ao = make_init_ao();
    struct priv *p = ao->priv;
    ao->format = format;
    p->cfg_pcm_float = true;
    init_fixture.running = true;
    init_fixture.encoding = format == AF_FORMAT_FLOAT ? 4 :
                            format == AF_FORMAT_RAW_AC3 ? 5 : 13;
    BuildVersion.SDK_INT = api;
    AudioFormat.ENCODING_AC3 = 5;
    AudioTrack.PLAYSTATE_PAUSED = 2;
    AudioTrack.STATE_INITIALIZED = 1;
    AudioTrack.getState = (jmethodID)METHOD_INITIALIZED;
    AudioTrack.getBufferSizeInFramesV23 = api >= 23 ?
        (jmethodID)METHOD_CAPACITY : NULL;
    AudioTrack.writeShortV23 = api >= 23 ? (jmethodID)METHOD_WRITE : NULL;
    AudioTrack.ctorV21 = (jmethodID)METHOD_NEW_TRACK;
    AudioTrack.release = (jmethodID)METHOD_RELEASE;
    AudioFormatBuilder.ctor = (jmethodID)METHOD_NEW_BUILDER;
    AudioFormatBuilder.setEncoding = (jmethodID)METHOD_SET_ENCODING;
    AudioFormatBuilder.setSampleRate = (jmethodID)METHOD_SET_SAMPLE_RATE;
    AudioFormatBuilder.setChannelMask = (jmethodID)METHOD_SET_CHANNEL_MASK;
    AudioFormatBuilder.build = (jmethodID)METHOD_BUILD;
    AudioAttributesBuilder.ctor = (jmethodID)METHOD_NEW_BUILDER;
    AudioAttributesBuilder.setUsage = (jmethodID)METHOD_SET_USAGE;
    AudioAttributesBuilder.build = (jmethodID)METHOD_BUILD;

    // JNI discovery/AudioTrack construction are scripted, but init() runs
    // unchanged and really creates/joins the legacy writer when selected.
    assert_int_equal(init(ao), 1);
    bool pull = af_fmt_is_spdif(format) || (af_fmt_is_pcm(format) && api < 23);
    assert_true(ao->driver == (pull ? &audio_out_audiotrack_pull :
        af_fmt_is_encoded(format) ? &audio_out_audiotrack_encoded : &pcm_driver));
    assert_int_equal(p->thread_created, pull);
    assert_int_equal(init_fixture.track_calls, 1);
    assert_int_equal(jni_static_use_count, 1);
    assert_int_equal(init_fixture.timestamp_calls, af_fmt_is_encoded(format) ? 0 : 1);
    assert_int_equal(device.write_calls, 0);
    uninit(ao);
    assert_int_equal(init_fixture.release_calls, 1);
    assert_int_equal(init_fixture.mutex_destroys, 1);
    assert_int_equal(init_fixture.cond_destroys, 1);
    assert_int_equal(jni_static_use_count, 0);
    assert_int_equal(device.buffers, 0);
    init_fixture = (struct init_fixture){0};
    talloc_free(ao);
}

int main(void)
{
    test_partial_write();
    test_reset_partial_write();
    test_short_eos_and_pause();
    test_playhead_wrap_and_write_error();
    test_pcm_does_not_write_read_ahead_silence();
    test_pcm_partial_zero_write_and_pause();
    test_pcm_reset_discards_old_partial_data();
    test_pcm_short_eof_with_pending_write();
    test_pcm_playhead_wrap_and_write_error();
    test_pcm_capacity_larger_than_chunk();
    test_pcm_zero_write_keeps_owned_audio();
    test_pcm_core_refill_large_capacity();
    test_pcm_core_eof_continuation(false, false);
    test_pcm_core_eof_continuation(true, false);
    test_pcm_core_eof_continuation(true, true);
    test_pcm_core_reset_while_paused();
    test_pcm_jni_exception_requests_one_reload(METHOD_CAPACITY);
    test_pcm_jni_exception_requests_one_reload(METHOD_PAUSE);
    test_pcm_jni_exception_requests_one_reload(METHOD_WRITE);
    test_init_unsupported_iec_preserves_existing_owner();
    test_init_timestamp_failure_preserves_existing_owner();
    test_init_discovery_failure_does_not_release_unowned_jni();
    test_init_driver_selection(21, AF_FORMAT_FLOAT);
    test_init_driver_selection(22, AF_FORMAT_FLOAT);
    test_init_driver_selection(23, AF_FORMAT_FLOAT);
    test_init_driver_selection(35, AF_FORMAT_FLOAT);
    test_init_driver_selection(23, AF_FORMAT_S_AC3);
    test_init_driver_selection(35, AF_FORMAT_S_AC3);
    test_init_driver_selection(21, AF_FORMAT_RAW_AC3);
    test_init_driver_selection(35, AF_FORMAT_RAW_AC3);
    return 0;
}
