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

#include "config.h"
#include "mpv_talloc.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/path.h"
#include "stream.h"

static bool has_iso_extension(bstr path)
{
    int query = bstrchr(path, '?');
    int fragment = bstrchr(path, '#');
    size_t end = path.len;
    if (query >= 0 && (size_t)query < end)
        end = query;
    if (fragment >= 0 && (size_t)fragment < end)
        end = fragment;
    path.len = end;
    return bstr_case_endswith(path, bstr0(".iso"));
}

static int iso_stream_open(stream_t *stream)
{
    struct MPOpts *opts = mp_get_config_group(stream, stream->global, &mp_opt_root);
    bool force_iso = opts->force_iso;
    talloc_free(opts);

    bool is_iso_candidate = force_iso || has_iso_extension(bstr0(stream->url));
    if (!is_iso_candidate)
        return STREAM_UNSUPPORTED;
    if (!stream->access_references)
        return STREAM_UNSUPPORTED;

    void *tmp = talloc_new(NULL);
#if HAVE_LIBBLURAY || HAVE_DVDNAV
    char *path = mp_file_get_path(tmp, bstr0(stream->url));
    bool local_path = path && path[0];

    // Let the native disc handlers probe local images. In particular, their
    // ordering preserves DVD-Audio preference for hybrid images.
    if (local_path && !force_iso) {
        talloc_free(tmp);
        return STREAM_UNSUPPORTED;
    }
#endif

#if HAVE_LIBBLURAY
    MP_VERBOSE(stream, "ISO detected. Trying as Blu-ray.\n");
    int r = stream_open_bluray_iso(stream, local_path ? path : stream->url,
                                   local_path);
    if (r == STREAM_OK) {
        MP_INFO(stream, "ISO detected as Blu-ray.\n");
        talloc_free(tmp);
        return STREAM_OK;
    }

    if (r == STREAM_UNSUPPORTED) {
        MP_VERBOSE(stream, "ISO image is not Blu-ray.\n");
    } else {
        MP_ERR(stream, "Failed to open Blu-ray ISO.\n");
        talloc_free(tmp);
        return r;
    }
#else
    int r = STREAM_UNSUPPORTED;
    MP_VERBOSE(stream, "Blu-ray ISO support was not compiled in.\n");
#endif

#if HAVE_DVDNAV
    MP_VERBOSE(stream, "Trying ISO as DVD.\n");
    r = stream_open_dvd_iso(stream, local_path ? path : stream->url,
                            local_path);
    if (r == STREAM_OK) {
        MP_INFO(stream, "ISO detected as DVD.\n");
        talloc_free(tmp);
        return STREAM_OK;
    }
    if (r == STREAM_UNSUPPORTED) {
        MP_VERBOSE(stream, "ISO image is not DVD; trying other handlers.\n");
    } else {
        MP_ERR(stream, "Failed to open DVD ISO.\n");
    }
#else
    MP_VERBOSE(stream, "DVD ISO support was not compiled in; trying other handlers.\n");
#endif

    talloc_free(tmp);
    return r;
}

const stream_info_t stream_info_iso = {
    .name = "iso",
    .open = iso_stream_open,
    .protocols = (const char*const[]){ "file", "http", "https", "proxy", "",
                                       NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};
