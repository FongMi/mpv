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

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/opt.h>

#include "common/av_common.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux.h"
#include "demux/packet.h"
#include "demux/packet_pool.h"
#include "demux/stheader.h"
#include "dovi_split.h"
#include "mpv_talloc.h"

enum dovi_filter_mode {
    DOVI_FILTER_SPLIT,
    DOVI_FILTER_HDR10,
    DOVI_FILTER_P81,
};

struct mp_dovi_split {
    struct demuxer *demuxer;
    struct sh_stream *bl;
    struct sh_stream *el;
    AVBSFContext *bsf;
    AVPacket *staging;
    enum dovi_filter_mode mode;
};

static void mp_dovi_split_destructor(void *p)
{
    struct mp_dovi_split *s = p;
    av_packet_free(&s->staging);
    av_bsf_free(&s->bsf);
}

static int init_bsf(struct mp_dovi_split *s, const char *name,
                    const char *option, const char *value)
{
    const AVBitStreamFilter *def = av_bsf_get_by_name(name);
    if (!def)
        return AVERROR(ENOENT);

    AVBSFContext *bsf = NULL;
    int ret = av_bsf_alloc(def, &bsf);
    if (ret < 0)
        return ret;

    AVCodecParameters *par = mp_codec_params_to_av(s->bl->codec);
    if (!par) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = avcodec_parameters_copy(bsf->par_in, par);
    avcodec_parameters_free(&par);
    if (ret < 0)
        goto fail;

    bsf->time_base_in = mp_get_codec_timebase(s->bl->codec);
    ret = av_opt_set(bsf, option, value, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0)
        goto fail;
    ret = av_bsf_init(bsf);
    if (ret < 0)
        goto fail;

    s->bsf = bsf;
    return 0;

fail:
    av_bsf_free(&bsf);
    return ret;
}

static const AVDOVIDecoderConfigurationRecord *get_dovi_config(
    const AVCodecParameters *par)
{
    if (!par)
        return NULL;

    for (int i = 0; i < par->nb_coded_side_data; i++) {
        const AVPacketSideData *sd = &par->coded_side_data[i];
        if (sd->type == AV_PKT_DATA_DOVI_CONF &&
            sd->size >= sizeof(AVDOVIDecoderConfigurationRecord))
        {
            return (const void *)sd->data;
        }
    }
    return NULL;
}

static int sync_decoder_parameters(struct mp_dovi_split *s)
{
    struct mp_codec_params *codec = s->bl->codec;
    uint8_t source_profile = mp_codec_params_source_dv_profile(codec);
    uint8_t source_level = codec->source_dv_profile
        ? codec->source_dv_level : codec->dv_level;

    if (codec->lav_codecpar) {
        AVCodecParameters *filtered = avcodec_parameters_alloc();
        if (!filtered)
            return AVERROR(ENOMEM);

        int ret = avcodec_parameters_copy(filtered, s->bsf->par_out);
        if (ret < 0) {
            avcodec_parameters_free(&filtered);
            return ret;
        }
        MPSWAP(AVCodecParameters, *codec->lav_codecpar, *filtered);
        avcodec_parameters_free(&filtered);
    }

    const AVDOVIDecoderConfigurationRecord *dovi =
        get_dovi_config(s->bsf->par_out);
    bool output_dovi = dovi && dovi->rpu_present_flag;
    codec->source_dv_profile = source_profile;
    codec->source_dv_level = source_level;
    codec->dovi = output_dovi;
    codec->dv_profile = output_dovi ? dovi->dv_profile : 0;
    codec->dv_level = output_dovi ? dovi->dv_level : 0;
    return 0;
}

struct mp_dovi_split *mp_dovi_split_create(struct demuxer *demuxer,
                                           struct sh_stream *bl)
{
    if (!bl || bl->type != STREAM_VIDEO || !bl->codec ||
        !bl->codec->codec || strcmp(bl->codec->codec, "hevc") != 0)
        return NULL;

    enum demux_dovi_profile7_mode requested = DEMUX_DOVI_PROFILE7_PRESERVE;
    if (mp_codec_params_source_dv_profile(bl->codec) == 7)
        requested = demuxer->opts->dovi_profile7_mode;
    const AVDOVIDecoderConfigurationRecord *dovi =
        get_dovi_config(bl->codec->lav_codecpar);
    if (requested != DEMUX_DOVI_PROFILE7_PRESERVE && dovi &&
        !dovi->bl_present_flag)
    {
        MP_WARN(demuxer, "Dolby Vision Profile 7: %s requires an independently "
                         "decodable base layer; leaving this stream unchanged.\n",
                requested == DEMUX_DOVI_PROFILE7_P81
                    ? "P8.1 conversion" : "HDR10 fallback");
        return NULL;
    }
    if (requested == DEMUX_DOVI_PROFILE7_PRESERVE &&
        !bl->codec->dv_el_present)
    {
        return NULL;
    }

    struct mp_dovi_split *s = talloc_zero(demuxer, struct mp_dovi_split);
    talloc_set_destructor(s, mp_dovi_split_destructor);
    s->demuxer = demuxer;
    s->bl = bl;

    int ret = AVERROR(ENOMEM);
    s->staging = av_packet_alloc();
    if (!s->staging)
        goto fail;

    if (requested == DEMUX_DOVI_PROFILE7_P81) {
        s->mode = DOVI_FILTER_P81;
        ret = init_bsf(s, "dovi_rpu", "convert", "p81");
        if (ret < 0) {
            MP_WARN(demuxer, "Dolby Vision Profile 7: P8.1 conversion is "
                             "unavailable (%s); using the HDR10 base layer.\n",
                    av_err2str(ret));
            s->mode = DOVI_FILTER_HDR10;
            ret = init_bsf(s, "dovi_split", "mode", "bl");
        }
    } else if (requested == DEMUX_DOVI_PROFILE7_HDR10) {
        s->mode = DOVI_FILTER_HDR10;
        ret = init_bsf(s, "dovi_split", "mode", "bl");
    } else {
        s->mode = DOVI_FILTER_SPLIT;
        ret = init_bsf(s, "dovi_split", "mode", "el_rpu");
    }
    if (ret < 0)
        goto fail;

    if (s->mode != DOVI_FILTER_SPLIT) {
        ret = sync_decoder_parameters(s);
        if (ret < 0)
            goto fail;
        bl->codec->dv_el_present = false;
        if (s->mode == DOVI_FILTER_HDR10)
            bl->codec->dv_p7_hdr10_fallback = true;
        if (s->mode == DOVI_FILTER_P81)
            MP_INFO(demuxer, "Dolby Vision Profile 7: converting to Profile 8.1.\n");
        else
            MP_INFO(demuxer, "Dolby Vision Profile 7: using HDR10 base layer.\n");
        return s;
    }

    const AVCodecParameters *par_out = s->bsf->par_out;

    // Allocate the virtual EL sh_stream.
    struct sh_stream *el = demux_alloc_sh_stream(STREAM_VIDEO);
    el->codec->codec = "hevc";
    el->codec->native_tb_num = bl->codec->native_tb_num;
    el->codec->native_tb_den = bl->codec->native_tb_den;
    el->codec->fps = bl->codec->fps;
    el->codec->disp_w = par_out->width;
    el->codec->disp_h = par_out->height;
    if (par_out->extradata_size > 0) {
        el->codec->extradata = talloc_memdup(el->codec, par_out->extradata,
                                             par_out->extradata_size);
        el->codec->extradata_size = par_out->extradata_size;
    }
    for (int i = 0; i < par_out->nb_coded_side_data; i++) {
        const AVPacketSideData *sd = &par_out->coded_side_data[i];
        if (sd->type != AV_PKT_DATA_DOVI_CONF)
            continue;
        const AVDOVIDecoderConfigurationRecord *cfg = (const void *)sd->data;
        el->codec->dovi = true;
        el->codec->dv_profile = cfg->dv_profile;
        el->codec->dv_level = cfg->dv_level;
        el->codec->dv_el_present = cfg->el_present_flag;
        break;
    }
    el->title = talloc_strdup(el, "Dolby Vision enhancement layer");
    el->dependent_track = true;

    demux_add_sh_stream(demuxer, el);
    s->el = el;

    // Bind BL and EL into a sh_stream_group.
    struct sh_stream_group *group = talloc_zero(bl, struct sh_stream_group);
    MP_TARRAY_APPEND(group, group->members, group->num_members, bl);
    MP_TARRAY_APPEND(group, group->members, group->num_members, el);
    bl->group = group;
    el->group = group;

    MP_VERBOSE(demuxer, "Dolby Vision Profile 7 splitter: BL stream %d, "
               "virtual EL stream %d (dependent_track).\n",
               bl->index, el->index);
    return s;

fail:
    MP_WARN(demuxer, "Dolby Vision: failed to initialize bitstream filter: %s. "
                     "Leaving the bitstream unchanged.\n",
            av_err2str(ret));
    talloc_free(s);
    return NULL;
}

void mp_dovi_split_reset(struct mp_dovi_split *s)
{
    if (!s || !s->bsf)
        return;
    av_bsf_flush(s->bsf);
}

struct sh_stream *mp_dovi_split_el_stream(struct mp_dovi_split *s)
{
    return s ? s->el : NULL;
}

static AVPacket *copy_packet_data(struct demux_packet *dp)
{
    if (dp->len > INT_MAX)
        return NULL;

    AVPacket *copy = av_packet_alloc();
    if (!copy)
        return NULL;

    int ret;
    if (dp->avpacket && dp->avpacket->data == dp->buffer &&
        dp->avpacket->size == (int)dp->len)
    {
        ret = av_packet_ref(copy, dp->avpacket);
    } else {
        ret = av_new_packet(copy, (int)dp->len);
        if (ret >= 0) {
            memcpy(copy->data, dp->buffer, dp->len);
            if (dp->avpacket)
                ret = av_packet_copy_props(copy, dp->avpacket);
        }
    }
    if (ret < 0) {
        av_packet_free(&copy);
        return NULL;
    }
    copy->flags &= ~AV_PKT_FLAG_KEY;
    if (dp->keyframe)
        copy->flags |= AV_PKT_FLAG_KEY;
    return copy;
}

enum dovi_packet_result {
    DOVI_PACKET_ERROR = -1,
    DOVI_PACKET_EMPTY,
    DOVI_PACKET_READY,
};

static enum dovi_packet_result filter_packet(struct mp_dovi_split *s,
                                             struct demux_packet *src,
                                             int stream,
                                             struct demux_packet **out)
{
    *out = NULL;
    if (!s || !s->bsf || !src || !src->buffer || !src->len)
        return DOVI_PACKET_ERROR;

    AVPacket *copy = copy_packet_data(src);
    if (!copy)
        return DOVI_PACKET_ERROR;

    int ret = av_bsf_send_packet(s->bsf, copy);
    av_packet_free(&copy);
    if (ret < 0) {
        MP_VERBOSE(s->demuxer, "%s: BSF send failed: %s\n",
                   s->bsf->filter->name, av_err2str(ret));
        return DOVI_PACKET_ERROR;
    }

    ret = av_bsf_receive_packet(s->bsf, s->staging);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return DOVI_PACKET_EMPTY;
    if (ret < 0) {
        MP_VERBOSE(s->demuxer, "%s: BSF receive failed: %s; flushing.\n",
                   s->bsf->filter->name, av_err2str(ret));
        av_bsf_flush(s->bsf);
        return DOVI_PACKET_ERROR;
    }

    struct demux_packet *dst =
        new_demux_packet_from_avpacket(s->demuxer->packet_pool, s->staging);
    av_packet_unref(s->staging);
    if (!dst)
        return DOVI_PACKET_ERROR;

    demux_packet_copy_attribs(dst, src);
    dst->stream = stream;
    *out = dst;
    return DOVI_PACKET_READY;
}

bool mp_dovi_split_filter_base(struct mp_dovi_split *s,
                               struct demux_packet **bl_dp)
{
    if (!s || s->mode == DOVI_FILTER_SPLIT)
        return true;
    if (!bl_dp || !*bl_dp)
        return false;

    struct demux_packet *filtered = NULL;
    enum dovi_packet_result ret =
        filter_packet(s, *bl_dp, s->bl->index, &filtered);
    if (ret == DOVI_PACKET_ERROR) {
        MP_ERR(s->demuxer, "Dolby Vision Profile 7: %s filtering failed.\n",
               s->mode == DOVI_FILTER_P81 ? "P8.1" : "HDR10");
        return false;
    }

    free_demux_packet(*bl_dp);
    *bl_dp = filtered;
    return true;
}

struct demux_packet *mp_dovi_split_dispatch(struct mp_dovi_split *s,
                                            struct demux_packet *bl_dp)
{
    if (!s || s->mode != DOVI_FILTER_SPLIT || !s->el)
        return NULL;

    struct demux_packet *el_dp = NULL;
    return filter_packet(s, bl_dp, s->el->index, &el_dp) == DOVI_PACKET_READY
               ? el_dp : NULL;
}
