/* Contracts for moving rendered subtitles without changing their layout. */
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MPMIN(a, b) ((a) < (b) ? (a) : (b))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MPCLAMP(v, lo, hi) MPMIN(MPMAX(v, lo), hi)

struct mp_rect { int x0, y0, x1, y1; };
struct sub_bitmap { int x, y, dw, dh; };
struct sub_bitmaps { struct sub_bitmap *parts; int num_parts; };

#include "subtitle_offset_functions.h"

static void test_video_borders(void)
{
    // 800x600 viewport, with the video covering y=75..525.
    struct sub_bitmap parts[] = {{200, 480, 180, 24}, {400, 492, 40, 16}};
    struct sub_bitmaps track = {parts, 2};
    mp_sub_bitmaps_shift_y(&track, 600, -10);
    assert(parts[0].y == 540 && parts[1].y == 552);
    assert(parts[0].dw == 180 && parts[0].dh == 24);
    assert(parts[1].y - parts[0].y == 12);

    mp_sub_bitmaps_shift_y(&track, 600, 90);
    assert(parts[0].y == 0 && parts[1].y == 12);
    assert(parts[0].x == 200 && parts[1].x == 400);
}

static void test_viewport_limits(void)
{
    struct sub_bitmap part = {10, 250, 100, 30};
    struct sub_bitmaps track = {&part, 1};
    mp_sub_bitmaps_shift_y(&track, 600, -100);
    assert(part.y == 570);
    mp_sub_bitmaps_shift_y(&track, 600, 100);
    assert(part.y == 0);
}

static void test_original_and_independent_tracks(void)
{
    struct sub_bitmap source[] = {{10, 500, 100, 25}, {120, 510, 50, 20}};
    struct sub_bitmap primary[2], secondary[2];
    memcpy(primary, source, sizeof(source));
    memcpy(secondary, source, sizeof(source));
    struct sub_bitmaps tracks[] = {{primary, 2}, {secondary, 2}};
    mp_sub_bitmaps_shift_y(&tracks[0], 600, -8);
    mp_sub_bitmaps_shift_y(&tracks[1], 600, 80);
    assert(primary[0].y == 548 && secondary[0].y == 20);
    assert(source[0].y == 500 && source[1].y == 510);

    struct sub_bitmaps original = {source, 2};
    mp_sub_bitmaps_shift_y(&original, 100, 0);
    assert(source[0].y == 500 && source[1].y == 510);
    mp_sub_bitmaps_shift_y(NULL, 600, 10);
    struct sub_bitmaps empty = {0};
    mp_sub_bitmaps_shift_y(&empty, 600, 10);
}

static void test_track_taller_than_viewport(void)
{
    struct sub_bitmap parts[] = {{10, -50, 100, 1000}, {120, 20, 50, 100}};
    struct sub_bitmaps track = {parts, 2};
    mp_sub_bitmaps_shift_y(&track, 600, -20);
    assert(parts[0].y == 70 && parts[1].y == 140);
    assert(parts[1].y - parts[0].y == 70);
    assert(parts[0].dh == 1000 && parts[1].dh == 100);
}

static void test_initially_outside_viewport(void)
{
    struct sub_bitmap parts[] = {{10, -30, 100, 20}, {120, 10, 50, 20}};
    struct sub_bitmaps track = {parts, 2};
    mp_sub_bitmaps_shift_y(&track, 600, 0);
    assert(parts[0].y == -30 && parts[1].y == 10);
    mp_sub_bitmaps_shift_y(&track, 600, 10);
    assert(parts[0].y == 0 && parts[1].y == 40);
    assert(parts[1].y - parts[0].y == 40);
}

static void test_fractional_shift_is_shared(void)
{
    struct sub_bitmap parts[] = {{10, 40, 100, 20}, {120, 44, 50, 20}};
    struct sub_bitmaps track = {parts, 2};
    mp_sub_bitmaps_shift_y(&track, 101, 25);
    assert(parts[0].y == 15 && parts[1].y == 19);
    assert(parts[1].y - parts[0].y == 4);
}

int main(void)
{
    test_video_borders();
    test_viewport_limits();
    test_original_and_independent_tracks();
    test_track_taller_than_viewport();
    test_initially_outside_viewport();
    test_fractional_shift_is_shared();
    puts("Rendered subtitle offset contracts passed");
    return 0;
}
