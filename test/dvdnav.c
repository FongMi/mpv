#include "test_utils.h"
#include "osdep/threads.h"

// Execute the production read/event/control paths with a scripted DVD VM and
// backing stream. The backing read is latched while player queries run.
static int test_mutex_lock(mp_mutex *mutex);
#pragma push_macro("mp_mutex_lock")
#undef mp_mutex_lock
#define mp_mutex_lock test_mutex_lock
#include "stream/stream_dvdnav.c"
#pragma pop_macro("mp_mutex_lock")

static struct {
    struct priv priv;
    stream_t stream, source;
    mp_mutex lock;
    mp_cond cond;
    bool read_entered, release_read;
    bool watch_vm_lock, control_waiting, control_done;
    bool check_state_lock;
    int queries_done;
    int read_result, control_result;
    int event, next_title;
    int title, angle, reads, commands;
    int64_t time;
    bool menu, position_valid, video_ready;
    bool menu_command;
    int floor_seeks, approximate_seeks;
    uint64_t seek_time;
    bool seek_succeeds;
    int active_audio, wait_reads;
} fixture;

static int test_mutex_lock(mp_mutex *mutex)
{
    if (mutex == &fixture.priv.vm_lock) {
        mp_mutex_lock(&fixture.lock);
        if (fixture.watch_vm_lock) {
            fixture.control_waiting = true;
            mp_cond_broadcast(&fixture.cond);
        }
        mp_mutex_unlock(&fixture.lock);
    }
    return mp_mutex_lock(mutex);
}

static void dvd_boundary(void)
{
    if (fixture.check_state_lock) {
        // No libdvdnav function may run with the publication lock held.
        assert_int_equal(mp_mutex_trylock(&fixture.priv.lock), 0);
        mp_mutex_unlock(&fixture.priv.lock);
    }
}

int stream_read(stream_t *s, void *buffer, int size)
{
    assert_true(s == &fixture.source);
    mp_mutex_lock(&fixture.lock);
    fixture.read_entered = true;
    fixture.reads++;
    mp_cond_broadcast(&fixture.cond);
    while (!fixture.release_read)
        mp_cond_wait(&fixture.cond, &fixture.lock);
    mp_mutex_unlock(&fixture.lock);
    memset(buffer, 0, size);
    return size;
}

void stream_drop_buffers(stream_t *s) {}

// The real bridge into the backing stream runs under fill_buffer's VM lock.
dvdnav_status_t dvdnav_get_next_block(dvdnav_t *nav, uint8_t *buf,
                                     int32_t *event, int32_t *len)
{
    dvd_boundary();
    *len = dvdnav_stream_read(&fixture.source, buf, 2048);
    *event = fixture.event;
    fixture.event = DVDNAV_BLOCK_OK;
    fixture.title = fixture.next_title;
    fixture.time = 6 * DVD_TIMEBASE;
    fixture.angle = 2;
    return DVDNAV_STATUS_OK;
}

dvdnav_status_t dvdnav_get_next_block_with_wait(dvdnav_t *nav, uint8_t *buf,
                                               int32_t *event, int32_t *len)
{
    fixture.wait_reads++;
    return dvdnav_get_next_block(nav, buf, event, len);
}

dvdnav_status_t dvdnav_current_title_info(dvdnav_t *nav, int32_t *title,
                                         int32_t *part)
{
    dvd_boundary();
    *title = fixture.title;
    *part = 1;
    return fixture.title < 0 ? DVDNAV_STATUS_ERR : DVDNAV_STATUS_OK;
}

int64_t dvdnav_get_current_time(dvdnav_t *nav)
{
    dvd_boundary();
    return fixture.time;
}

dvdnav_status_t dvdnav_get_number_of_titles(dvdnav_t *nav, int32_t *titles)
{
    dvd_boundary();
    *titles = 3;
    return DVDNAV_STATUS_OK;
}

dvdnav_status_t dvdnav_get_angle_info(dvdnav_t *nav, int32_t *angle,
                                     int32_t *angles)
{
    dvd_boundary();
    *angle = fixture.angle;
    *angles = 2;
    return DVDNAV_STATUS_OK;
}

dvdnav_status_t dvdnav_get_video_resolution(dvdnav_t *nav, uint32_t *w, uint32_t *h)
{
    dvd_boundary();
    if (!fixture.video_ready)
        return DVDNAV_STATUS_ERR;
    *w = 720;
    *h = 480;
    return DVDNAV_STATUS_OK;
}

dvdnav_status_t dvdnav_menu_available(dvdnav_t *nav, DVDMenuID_t menu)
{
    dvd_boundary();
    return DVDNAV_STATUS_OK;
}

dvdnav_status_t dvdnav_title_play(dvdnav_t *nav, int32_t title)
{
    dvd_boundary();
    fixture.commands++;
    fixture.title = title;
    fixture.menu = false;
    fixture.position_valid = false;
    fixture.time = 0;
    return DVDNAV_STATUS_OK;
}

dvdnav_status_t dvdnav_menu_call(dvdnav_t *nav, DVDMenuID_t menu)
{
    dvdnav_status_t result = dvdnav_title_play(nav, 0);
    fixture.menu = true;
    return result;
}

dvdnav_status_t dvdnav_get_position(dvdnav_t *nav, uint32_t *pos, uint32_t *len)
{
    dvd_boundary();
    *pos = 0;
    *len = 10;
    return fixture.position_valid ? DVDNAV_STATUS_OK : DVDNAV_STATUS_ERR;
}

int8_t dvdnav_is_domain_vmgm(dvdnav_t *nav) { dvd_boundary(); return fixture.menu; }
int8_t dvdnav_is_domain_vts(dvdnav_t *nav) { dvd_boundary(); return !fixture.menu; }
int8_t dvdnav_is_domain_vtsm(dvdnav_t *nav) { dvd_boundary(); return 0; }
int8_t dvdnav_is_domain_fp(dvdnav_t *nav) { dvd_boundary(); return 0; }
int8_t dvdnav_get_audio_logical_stream(dvdnav_t *nav, uint8_t n)
{ dvd_boundary(); return n == 0 || n == 6 ? 0 : -1; }
int8_t dvdnav_get_number_of_stream_attributes(dvdnav_t *nav, dvdnav_stream_type_t type)
{ dvd_boundary(); return type == DVD_AUDIO_STREAM ? 7 : 0; }
int8_t dvdnav_get_active_logical_stream(dvdnav_t *nav, dvdnav_stream_type_t type)
{ dvd_boundary(); return type == DVD_AUDIO_STREAM ? fixture.active_audio : -1; }
int8_t dvdnav_get_active_spu_stream(dvdnav_t *nav)
{ dvd_boundary(); return -1; }
dvdnav_status_t dvdnav_set_active_stream(dvdnav_t *nav, uint8_t logical,
                                        dvdnav_stream_type_t type)
{
    dvd_boundary();
    if (type != DVD_AUDIO_STREAM || (logical != 0 && logical != 6))
        return DVDNAV_STATUS_ERR;
    fixture.active_audio = logical;
    return DVDNAV_STATUS_OK;
}
dvdnav_status_t dvdnav_toggle_spu_stream(dvdnav_t *nav, uint8_t visibility)
{ dvd_boundary(); return DVDNAV_STATUS_OK; }
int8_t dvdnav_get_number_of_streams(dvdnav_t *nav, dvdnav_stream_type_t type)
{ dvd_boundary(); return type == DVD_AUDIO_STREAM ? 1 : 0; }
int8_t dvdnav_get_spu_logical_stream(dvdnav_t *nav, uint8_t n) { return -1; }
uint16_t dvdnav_audio_stream_format(dvdnav_t *nav, uint8_t n)
{ dvd_boundary(); return DVD_AUDIO_FORMAT_AC3; }
uint16_t dvdnav_audio_stream_to_lang(dvdnav_t *nav, uint8_t n) { return 0xffff; }
uint16_t dvdnav_spu_stream_to_lang(dvdnav_t *nav, uint8_t n) { return 0xffff; }
uint8_t dvdnav_get_video_aspect(dvdnav_t *nav) { return 0; }
const char *dvdnav_err_to_string(dvdnav_t *nav) { return "test"; }
pci_t *dvdnav_get_current_nav_pci(dvdnav_t *nav) { return NULL; }
dvdnav_status_t dvdnav_get_current_highlight(dvdnav_t *nav, int32_t *button)
{ *button = 0; return DVDNAV_STATUS_OK; }
dvdnav_status_t dvdnav_wait_skip(dvdnav_t *nav)
{ dvd_boundary(); return DVDNAV_STATUS_OK; }
dvdnav_status_t dvdnav_still_skip(dvdnav_t *nav)
{ dvd_boundary(); return DVDNAV_STATUS_OK; }
dvdnav_status_t dvdnav_go_up(dvdnav_t *nav) { return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_button_select(dvdnav_t *nav, pci_t *pci, int32_t n)
{ return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_button_activate(dvdnav_t *nav, pci_t *pci)
{ return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_mouse_select(dvdnav_t *nav, pci_t *pci, int32_t x, int32_t y)
{ return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_mouse_activate(dvdnav_t *nav, pci_t *pci, int32_t x, int32_t y)
{ return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_get_number_of_parts(dvdnav_t *nav, int32_t n, int32_t *parts)
{ return DVDNAV_STATUS_ERR; }
uint32_t dvdnav_describe_title_chapters(dvdnav_t *nav, int32_t n,
                                      uint64_t **parts, uint64_t *duration)
{ return 0; }
dvdnav_status_t dvdnav_get_title_string(dvdnav_t *nav, const char **title)
{ return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_next_pg_search(dvdnav_t *nav) { return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_prev_pg_search(dvdnav_t *nav) { return DVDNAV_STATUS_ERR; }
dvdnav_status_t dvdnav_time_search_floor(dvdnav_t *nav, uint64_t time)
{
    dvd_boundary();
    fixture.floor_seeks++;
    fixture.seek_time = time;
    return fixture.seek_succeeds ? DVDNAV_STATUS_OK : DVDNAV_STATUS_ERR;
}

dvdnav_status_t dvdnav_time_search(dvdnav_t *nav, uint64_t time)
{ fixture.approximate_seeks++; return DVDNAV_STATUS_ERR; }
#if DVDNAV_VERSION >= DVDNAV_VERSION_CODE(7, 0, 0)
dvdnav_status_t dvdnav_jump_to_sector_by_time(dvdnav_t *nav, uint64_t time, int32_t mode)
{ fixture.approximate_seeks++; return DVDNAV_STATUS_ERR; }
#endif
dvdnav_status_t dvdnav_angle_change(dvdnav_t *nav, int32_t angle)
{ fixture.angle = angle; return DVDNAV_STATUS_OK; }

static void init_fixture(void)
{
    memset(&fixture, 0, sizeof(fixture));
    fixture.title = 1;
    fixture.next_title = 2;
    fixture.angle = 1;
    fixture.time = 5 * DVD_TIMEBASE;
    fixture.event = DVDNAV_BLOCK_OK;
    fixture.position_valid = true;
    fixture.video_ready = true;
    mp_mutex_init(&fixture.lock);
    mp_cond_init(&fixture.cond);
    mp_mutex_init(&fixture.priv.vm_lock);
    mp_mutex_init(&fixture.priv.lock);
    fixture.priv.dvdnav = (dvdnav_t *)&fixture;
    fixture.priv.num_titles = 3;
    fixture.priv.audio_physical = 0;
    fixture.priv.sub_physical = -1;
    fixture.stream.priv = &fixture.priv;
    mp_mutex_lock(&fixture.priv.vm_lock);
    publish_state(&fixture.priv);
    mp_mutex_unlock(&fixture.priv.vm_lock);
}

static void uninit_fixture(void)
{
    mp_mutex_destroy(&fixture.priv.lock);
    mp_mutex_destroy(&fixture.priv.vm_lock);
    mp_cond_destroy(&fixture.cond);
    mp_mutex_destroy(&fixture.lock);
}

// A deadline makes a blocking getter fail without leaving a worker hanging.
static bool wait_for(bool *flag)
{
    int64_t deadline = mp_time_ns() + MP_TIME_S_TO_NS(2);
    mp_mutex_lock(&fixture.lock);
    while (!*flag) {
        if (mp_cond_timedwait_until(&fixture.cond, &fixture.lock, deadline))
            break;
    }
    bool result = *flag;
    mp_mutex_unlock(&fixture.lock);
    return result;
}

static void release_read(void)
{
    mp_mutex_lock(&fixture.lock);
    fixture.release_read = true;
    mp_cond_broadcast(&fixture.cond);
    mp_mutex_unlock(&fixture.lock);
}

static MP_THREAD_VOID read_thread(void *arg)
{
    uint8_t buffer[2048];
    fixture.read_result = fill_buffer(&fixture.stream, buffer, sizeof(buffer));
    MP_THREAD_RETURN();
}

struct query {
    int cmd, result;
    union {
        struct stream_nav_state nav;
        unsigned int title;
        double time;
    } value;
};

static MP_THREAD_VOID query_thread(void *arg)
{
    struct query *query = arg;
    query->result = control(&fixture.stream, query->cmd, &query->value);
    mp_mutex_lock(&fixture.lock);
    fixture.queries_done++;
    mp_cond_broadcast(&fixture.cond);
    mp_mutex_unlock(&fixture.lock);
    MP_THREAD_RETURN();
}

static void test_queries_during_read(void)
{
    init_fixture();
    mp_thread reader;
    assert_int_equal(mp_thread_create(&reader, read_thread, NULL), 0);
    assert_true(wait_for(&fixture.read_entered));
    struct query queries[] = {
        {.cmd = STREAM_CTRL_GET_NAV_STATE},
        {.cmd = STREAM_CTRL_GET_NUM_TITLES},
        {.cmd = STREAM_CTRL_GET_CURRENT_TITLE},
        {.cmd = STREAM_CTRL_GET_CURRENT_TIME},
    };
    mp_thread threads[MP_ARRAY_SIZE(queries)];
    for (int n = 0; n < MP_ARRAY_SIZE(queries); n++)
        assert_int_equal(mp_thread_create(&threads[n], query_thread, &queries[n]), 0);
    int64_t deadline = mp_time_ns() + MP_TIME_S_TO_NS(2);
    mp_mutex_lock(&fixture.lock);
    while (fixture.queries_done < MP_ARRAY_SIZE(queries)) {
        if (mp_cond_timedwait_until(&fixture.cond, &fixture.lock, deadline))
            break;
    }
    int completed = fixture.queries_done;
    mp_mutex_unlock(&fixture.lock);
    release_read();
    mp_thread_join(reader);
    for (int n = 0; n < MP_ARRAY_SIZE(queries); n++) {
        mp_thread_join(threads[n]);
        assert_int_equal(queries[n].result, STREAM_OK);
    }
    assert_int_equal(completed, MP_ARRAY_SIZE(queries));
    assert_int_equal(queries[0].value.nav.angle, 1);
    assert_int_equal(queries[0].value.nav.active_audio_id, 0x80);
    assert_int_equal(queries[0].value.nav.src_h, 480);
    assert_int_equal(queries[1].value.title, 3);
    assert_int_equal(queries[2].value.title, 0);
    assert_float_equal(queries[3].value.time, 5, 0);
    assert_int_equal(fixture.read_result, 2048);
    struct stream_nav_state nav;
    unsigned int title;
    double time;
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_GET_NAV_STATE, &nav), STREAM_OK);
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_GET_CURRENT_TITLE, &title), STREAM_OK);
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_GET_CURRENT_TIME, &time), STREAM_OK);
    assert_int_equal(nav.angle, 2);
    assert_int_equal(title, 1);
    assert_float_equal(time, 6, 0);
    uninit_fixture();
}

static void test_initial_vts_resolution(void)
{
    init_fixture();
    fixture.video_ready = false;
    fixture.priv.src_w = fixture.priv.src_h = 0;
    fixture.priv.drain_enabled = true;
    mp_mutex_lock(&fixture.priv.vm_lock);
    publish_state(&fixture.priv);
    mp_mutex_unlock(&fixture.priv.vm_lock);
    struct stream_nav_state nav;
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_GET_NAV_STATE, &nav), STREAM_OK);
    assert_int_equal(nav.src_h, DVD_SRC_H_DEFAULT);

    fixture.video_ready = true;
    fixture.event = DVDNAV_VTS_CHANGE;
    release_read();
    uint8_t buffer[2048];
    assert_int_equal(fill_buffer(&fixture.stream, buffer, sizeof(buffer)), 2048);
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_GET_NAV_STATE, &nav), STREAM_OK);
    assert_int_equal(nav.src_w, 720);
    assert_int_equal(nav.src_h, 480);
    assert_false(nav.drain_pending);
    assert_int_equal(nav.discontinuity_id, 0);
    uninit_fixture();
}

static MP_THREAD_VOID control_thread(void *arg)
{
    unsigned int title = 1;
    struct stream_nav_cmd cmd = {.action = STREAM_NAV_MENU_ROOT};
    fixture.control_result = fixture.menu_command
        ? control(&fixture.stream, STREAM_CTRL_NAV_CMD, &cmd)
        : control(&fixture.stream, STREAM_CTRL_SET_CURRENT_TITLE, &title);
    mp_mutex_lock(&fixture.lock);
    fixture.control_done = true;
    mp_cond_broadcast(&fixture.cond);
    mp_mutex_unlock(&fixture.lock);
    MP_THREAD_RETURN();
}

static void test_stop_then_jump(bool menu)
{
    init_fixture();
    fixture.check_state_lock = true;
    fixture.event = DVDNAV_STOP;
    fixture.menu_command = menu;
    fixture.priv.still_active = true;
    fixture.priv.still_duration = 5;
    fixture.priv.still_id = 7;
    fixture.priv.drain_enabled = true;
    mp_thread reader, command;
    assert_int_equal(mp_thread_create(&reader, read_thread, NULL), 0);
    assert_true(wait_for(&fixture.read_entered));
    mp_mutex_lock(&fixture.lock);
    fixture.watch_vm_lock = true;
    mp_mutex_unlock(&fixture.lock);
    assert_int_equal(mp_thread_create(&command, control_thread, NULL), 0);
    assert_true(wait_for(&fixture.control_waiting));
    mp_mutex_lock(&fixture.lock);
    bool command_was_waiting = !fixture.control_done;
    mp_mutex_unlock(&fixture.lock);
    release_read();
    mp_thread_join(reader);
    mp_thread_join(command);
    assert_true(command_was_waiting);
    assert_int_equal(fixture.read_result, 0);
    assert_int_equal(fixture.control_result, STREAM_OK);
    assert_int_equal(fixture.commands, 1);
    assert_false(fixture.priv.terminal_stop);
    struct stream_nav_state nav;
    unsigned int title;
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_GET_NAV_STATE, &nav), STREAM_OK);
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_GET_CURRENT_TITLE, &title), STREAM_OK);
    assert_int_equal(title, menu ? (unsigned int)-1 : 1);
    assert_int_equal(nav.menu_active, menu);
    assert_false(nav.still_active);
    assert_true(nav.drain_pending);
    assert_true(nav.drain_immediate);
    assert_int_equal(nav.discontinuity_id, 1);
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_NAV_DRAIN_ACK, NULL), STREAM_OK);
    fixture.event = DVDNAV_BLOCK_OK;
    uint8_t buffer[2048];
    assert_int_equal(fill_buffer(&fixture.stream, buffer, sizeof(buffer)), 2048);
    assert_int_equal(fixture.reads, 2);
    uninit_fixture();
}

static void test_floor_seek(void)
{
    init_fixture();
    fixture.priv.duration = 120 * DVD_TIMEBASE;
    fixture.seek_succeeds = true;
    double args[] = {60, SEEK_HR};
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_SEEK_TO_TIME, args), STREAM_OK);
    assert_int_equal(fixture.floor_seeks, 1);
    assert_int_equal(fixture.approximate_seeks, 0);
    assert_int_equal(fixture.seek_time, 60 * DVD_TIMEBASE);

    args[0] = 120;
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_SEEK_TO_TIME, args), STREAM_OK);
    assert_int_equal(fixture.seek_time, fixture.priv.duration - 1);

    fixture.seek_succeeds = false;
    args[0] = 30;
    fixture.priv.terminal_stop = true;
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_SEEK_TO_TIME, args), STREAM_UNSUPPORTED);
    assert_int_equal(fixture.floor_seeks, 3);
    assert_int_equal(fixture.approximate_seeks, 0);
    assert_true(fixture.priv.terminal_stop);

    args[1] = 0;
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_SEEK_TO_TIME, args), STREAM_UNSUPPORTED);
    assert_int_equal(fixture.floor_seeks, 3);
#if DVDNAV_VERSION >= DVDNAV_VERSION_CODE(7, 0, 0)
    assert_int_equal(fixture.approximate_seeks, 2);
#else
    assert_int_equal(fixture.approximate_seeks, 1);
#endif
    uninit_fixture();
}

static void test_selection_during_wait(void)
{
    init_fixture();
    fixture.release_read = true;
    fixture.event = DVDNAV_WAIT;
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_NAV_DRAIN_ENABLE, NULL), STREAM_OK);
    uint64_t generation = fixture.priv.dvd_streams.generation;
    uint8_t buffer[2048];
    assert_int_equal(fill_buffer(&fixture.stream, buffer, sizeof(buffer)), 0);
    assert_true(fixture.priv.wait_pending);
    assert_int_equal(fixture.wait_reads, 1);
    assert_int_equal(fixture.priv.dvd_streams.generation, generation);
    struct stream_dvd_select select = {
        .generation = generation, .type = STREAM_AUDIO, .logical = 6,
    };
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_SET_DVD_STREAM, &select), STREAM_OK);
    assert_int_equal(fixture.active_audio, 6);
    assert_int_equal(fixture.priv.nav_state.active_audio_logical, 6);
    assert_int_equal(fill_buffer(&fixture.stream, buffer, sizeof(buffer)), 0);
    assert_int_equal(fixture.wait_reads, 1);
    assert_int_equal(control(&fixture.stream, STREAM_CTRL_NAV_DRAIN_ACK, NULL), STREAM_OK);
    assert_int_equal(fill_buffer(&fixture.stream, buffer, sizeof(buffer)), 2048);
    assert_int_equal(fixture.wait_reads, 2);
    uninit_fixture();
}

int main(void)
{
    test_selection_during_wait();
    test_floor_seek();
    test_queries_during_read();
    test_initial_vts_resolution();
    test_stop_then_jump(false);
    test_stop_then_jump(true);
    return 0;
}
