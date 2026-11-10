/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef MP_VULKAN_PRESENT_H
#define MP_VULKAN_PRESENT_H

#include <stdint.h>

// Estimate the latest accepted presentation using a matched historical latency.
// All inputs except mp_now use CLOCK_MONOTONIC. The bracket maps its midpoint
// to mp_time_ns(), without assuming that mpv uses the same underlying clock.
// Zero means unknown. No presentation ID is treated as a physical vsync count.
static inline int64_t mpvk_present_time(int64_t submit, int64_t matched_submit,
                                       int64_t actual, int64_t mono_before,
                                       int64_t mp_now, int64_t mono_after)
{
    if (matched_submit <= 0 || submit < matched_submit ||
        actual < matched_submit || mono_before <= 0 ||
        mono_after < mono_before || submit > mono_after ||
        actual > mono_after || mp_now <= 0) {
        return 0;
    }

    int64_t latency = actual - matched_submit;
    if (submit > INT64_MAX - latency)
        return 0;
    int64_t predicted = submit + latency;
    int64_t midpoint = mono_before + (mono_after - mono_before) / 2;
    int64_t delta = predicted - midpoint;
    if ((delta > 0 && mp_now > INT64_MAX - delta) ||
        (delta < 0 && mp_now <= -delta)) {
        return 0;
    }
    return mp_now + delta;
}

#endif
