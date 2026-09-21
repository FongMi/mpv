/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct android_osd_overlay;
struct vo;

struct android_osd_overlay *android_osd_overlay_create(struct vo *vo);
bool android_osd_overlay_set_surface(struct android_osd_overlay *ctx,
                                     int64_t wid);
void android_osd_overlay_invalidate_geometry(struct android_osd_overlay *ctx);
bool android_osd_overlay_render(struct android_osd_overlay *ctx, double pts);
bool android_osd_overlay_present(struct android_osd_overlay *ctx);
bool android_osd_overlay_get_size(struct android_osd_overlay *ctx,
                                  int *w, int *h);
void android_osd_overlay_destroy(struct android_osd_overlay *ctx);
