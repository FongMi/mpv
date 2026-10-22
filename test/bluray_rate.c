#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#define STREAM_OK 1
#define STREAM_UNSUPPORTED -1
#define STREAM_CTRL_SET_PLAYBACK_RATE 1
#define BD_TIME_FROM_S(x) ((int64_t)((x) * 90000))

struct bluray_priv_s {
    bool hdmv_mode, clip_open, pending_rate_set;
    double pending_rate;
    int pending_angle, overlay_lock;
    void *bd;
};

static int64_t rates[4];
static int num_rates, lock_depth;

static void mp_mutex_lock(int *lock)
{
    assert(lock && lock_depth++ == 0);
}

static void mp_mutex_unlock(int *lock)
{
    assert(lock && lock_depth-- == 1);
}

static void bd_set_rate(void *bd, int64_t rate)
{
    assert(bd && lock_depth == 0 && num_rates < 4);
    rates[num_rates++] = rate;
}

static void bd_seamless_angle_change(void *bd, int angle)
{
    (void)bd;
    (void)angle;
    assert(false);
}

#include "bluray_rate_functions.h"

static void test_pause_cancels_pending_rate(bool clip_open)
{
    struct bluray_priv_s b = {.hdmv_mode = true, .bd = &b};
    num_rates = 0;
    assert(set_rate(&b, 1) == STREAM_OK);
    assert(b.pending_rate_set && num_rates == 0);
    b.clip_open = clip_open;
    assert(set_rate(&b, 0) == STREAM_OK);
    assert(!b.pending_rate_set && num_rates == 1 && rates[0] == 0);
    b.clip_open = true;
    apply_pending(&b);
    assert(num_rates == 1);
    assert(set_rate(&b, 2) == STREAM_OK);
    assert(num_rates == 1);
    apply_pending(&b);
    assert(num_rates == 2 && rates[1] == 180000 && !b.pending_rate_set);
}

int main(void)
{
    test_pause_cancels_pending_rate(false);
    test_pause_cancels_pending_rate(true);
    struct bluray_priv_s b = {0};
    assert(set_rate(&b, 1) == STREAM_UNSUPPORTED);
    assert(!b.pending_rate_set && lock_depth == 0);
    return 0;
}
