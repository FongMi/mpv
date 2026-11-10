/* SPDX-License-Identifier: LGPL-2.1-or-later */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#include "video/out/vulkan/present.h"

int main(void)
{
    // A historical 30 ns latency predicts a future presentation for the
    // latest submission. The two clocks have deliberately different origins.
    assert(mpvk_present_time(200, 100, 130, 210, 1010, 214) == 1028);
    assert(mpvk_present_time(200, 100, 130, 210, 110, 214) == 128);
    // The latest submission itself may already have actual feedback.
    assert(mpvk_present_time(100, 100, 130, 140, 940, 140) == 930);
    assert(mpvk_present_time(100, 100, 100, 100, 900, 100) == 900);

    // Missing data, inconsistent ordering and failed clock conversion must
    // remain unknown, not become a zero-latency presentation.
    assert(!mpvk_present_time(0, 0, 0, 210, 1010, 214));
    assert(!mpvk_present_time(99, 100, 130, 210, 1010, 214));
    assert(!mpvk_present_time(200, 100, 99, 210, 1010, 214));
    assert(!mpvk_present_time(200, 100, 215, 210, 1010, 214));
    assert(!mpvk_present_time(215, 100, 130, 210, 1010, 214));
    assert(!mpvk_present_time(200, 100, 130, 214, 1010, 210));
    assert(!mpvk_present_time(200, 100, 130, 0, 1010, 214));
    assert(!mpvk_present_time(200, 100, 130, 210, 0, 214));
    assert(!mpvk_present_time(INT64_MAX, 1, 2, 1, 1, INT64_MAX));
    assert(!mpvk_present_time(200, 100, 130, 210, INT64_MAX, 214));
    assert(!mpvk_present_time(100, 100, 130, 140, 10, 140));
    assert(mpvk_present_time(INT64_MAX, INT64_MAX, INT64_MAX,
                             INT64_MAX, 1, INT64_MAX) == 1);
    puts("Vulkan presentation estimate: latency, clocks and invalid input passed");
    return 0;
}
