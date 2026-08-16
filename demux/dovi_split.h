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

struct demuxer;
struct sh_stream;
struct demux_packet;

// Dolby Vision Profile 7 enhancement-layer splitter for HEVC streams that
// carry the EL bitstream interleaved as HEVC_NAL_UNSPEC63.
//
// Preserve mode creates a virtual EL sh_stream and binds it to the BL via
// sh_stream_group. HDR10 and P8.1 modes filter the input without exposing an
// EL stream.
struct mp_dovi_split;

// Create a splitter on `bl`. Preserve mode adds the virtual EL sh_stream to
// `demuxer`; fallback modes keep a single base-layer packet stream.
//
// Returns NULL if the BSF is unavailable or initialization fails. The
// returned context is talloc-attached to `demuxer`.
struct mp_dovi_split *mp_dovi_split_create(struct demuxer *demuxer,
                                           struct sh_stream *bl);

void mp_dovi_split_reset(struct mp_dovi_split *s);

// Return the virtual EL sh_stream created by mp_dovi_split_create. The
// returned pointer remains valid for the lifetime of the splitter.
struct sh_stream *mp_dovi_split_el_stream(struct mp_dovi_split *s);

// Replace `*bl_dp` with a clean HDR10 base-layer packet or a converted P8.1
// packet. This is a no-op in preserve mode. Returns false on filtering errors;
// the caller retains ownership of the original packet in that case. A
// successful filter may set `*bl_dp` to NULL when an access unit contains no
// selected NAL units.
bool mp_dovi_split_filter_base(struct mp_dovi_split *s,
                               struct demux_packet **bl_dp);

// Apply the BSF to `bl_dp` and produce a companion EL demux_packet, if any.
// Caller owns the returned packet. Returns NULL if the access unit contained
// no EL NALs or on any non-fatal BSF error.
//
// The emitted packet inherits pts/dts/duration/keyframe from `bl_dp` so it
// lines up with the matching BL packet for PTS-based pairing downstream.
struct demux_packet *mp_dovi_split_dispatch(struct mp_dovi_split *s,
                                            struct demux_packet *bl_dp);
