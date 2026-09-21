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

#include <limits.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/ac3_parser.h>
#include <libavutil/intreadwrite.h>

#include "audio/aframe.h"
#include "audio/chmap_avchannel.h"
#include "audio/format.h"
#include "common/av_common.h"
#include "common/codecs.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "misc/bstr.h"

// Match Media3's grouping of TrueHD access units. Keep the decoded sample
// count of a final, shorter chunk instead of padding it at EOF.
#define TRUEHD_CHUNK_ACCESS_UNITS 16

struct priv {
    struct mp_decoder public;
    struct mp_codec_params *codec;
    AVCodecContext *avctx;
    AVCodecParserContext *parser;
    bool use_dts_hd;
    bool truehd_synced;
    bool failed;
    struct mp_aframe *chunk_format;
    uint8_t *chunk;
    int chunk_bytes;
    int chunk_samples;
    int chunk_access_units;
    struct mp_aframe *pending;
};

static bool init_parser(struct priv *p)
{
    int codec_id = mp_codec_to_av_codec_id(p->codec->codec);
    p->parser = av_parser_init(codec_id);
    p->avctx = avcodec_alloc_context3(NULL);
    if (!p->parser || !p->avctx)
        return false;
    p->avctx->codec_type = AVMEDIA_TYPE_AUDIO;
    p->avctx->codec_id = codec_id;
    if (mp_set_avctx_codec_headers(p->avctx, p->codec) < 0)
        return false;
    // The demuxer supplies complete access units. In particular, an E-AC-3
    // access unit includes its dependent substreams, not just its core frame.
    p->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    return true;
}

static void clear_chunk(struct priv *p)
{
    TA_FREEP(&p->chunk_format);
    p->chunk_bytes = p->chunk_samples = p->chunk_access_units = 0;
}

static void destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    av_parser_close(p->parser);
    avcodec_free_context(&p->avctx);
}

static void reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    destroy(f);
    p->parser = NULL;
    p->truehd_synced = false;
    p->public.passthrough_consumed = p->public.passthrough_discarded = 0;
    clear_chunk(p);
    TA_FREEP(&p->pending);
    p->failed = false;
    mp_filter_has_failed(f);
    if (!init_parser(p))
        mp_filter_internal_mark_failed(f);
}

static int raw_format(struct priv *p)
{
    switch (p->avctx->codec_id) {
    case AV_CODEC_ID_AC3:    return AF_FORMAT_RAW_AC3;
    case AV_CODEC_ID_EAC3:   return AF_FORMAT_RAW_EAC3;
    case AV_CODEC_ID_TRUEHD: return AF_FORMAT_RAW_TRUEHD;
    case AV_CODEC_ID_DTS:
        switch (p->avctx->profile) {
        case AV_PROFILE_DTS:
        case AV_PROFILE_DTS_ES:
        case AV_PROFILE_DTS_96_24:
            return AF_FORMAT_RAW_DTS;
        case AV_PROFILE_DTS_HD_HRA:
        case AV_PROFILE_DTS_HD_MA:
        case AV_PROFILE_DTS_HD_MA_X:
        case AV_PROFILE_DTS_HD_MA_X_IMAX:
            return p->use_dts_hd ? AF_FORMAT_RAW_DTSHD : 0;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

static struct mp_aframe *parse_packet(struct priv *p, struct demux_packet *pkt)
{
    if (!pkt->len || pkt->len > INT_MAX)
        return NULL;

    if (p->avctx->codec_id == AV_CODEC_ID_AC3 ||
        p->avctx->codec_id == AV_CODEC_ID_EAC3)
    {
        // E-AC-3 dependent syncframes add channels to the same access unit.
        // A second independent syncframe starts another access unit, whose
        // duration cannot be represented by the parser's single-frame result.
        size_t offset = 0;
        while (offset < pkt->len) {
            uint8_t bitstream_id;
            uint16_t frame_size;
            if (av_ac3_parse_header(pkt->buffer + offset, pkt->len - offset,
                                    &bitstream_id, &frame_size) < 0 ||
                !frame_size || frame_size > pkt->len - offset)
                return NULL;
            bool dependent = bitstream_id > 10 &&
                             (pkt->buffer[offset + 2] >> 6) == 1;
            if ((offset == 0) == dependent)
                return NULL;
            offset += frame_size;
        }
    }

    // TrueHD's parser retains the major-sync sample count for the following
    // access units. Other parsers must report a duration for every packet.
    if (p->avctx->codec_id != AV_CODEC_ID_TRUEHD)
        p->parser->duration = 0;
    uint8_t *data = NULL;
    int bytes = 0;
    int consumed = av_parser_parse2(p->parser, p->avctx, &data, &bytes,
                                   pkt->buffer, pkt->len, AV_NOPTS_VALUE,
                                   AV_NOPTS_VALUE, pkt->pos);
    int samples = p->parser->duration;
    int format = raw_format(p);
    if (consumed != (int)pkt->len || bytes != (int)pkt->len || !data || samples <= 0 ||
        p->avctx->sample_rate <= 0 || !format)
        return NULL;

    struct mp_chmap channels;
    mp_chmap_from_av_layout(&channels, &p->avctx->ch_layout);
    if (!channels.num)
        return NULL;

    struct mp_aframe *frame = mp_aframe_create();
    if (!mp_aframe_set_format(frame, format) ||
        !mp_aframe_set_rate(frame, p->avctx->sample_rate) ||
        !mp_aframe_set_chmap(frame, &channels) ||
        !mp_aframe_set_encoded_data(frame, data, bytes, samples))
    {
        talloc_free(frame);
        return NULL;
    }
    mp_aframe_set_pts(frame, pkt->pts);
    p->codec->codec_profile = avcodec_profile_name(p->avctx->codec_id,
                                                 p->avctx->profile);
    return frame;
}

static struct mp_aframe *take_chunk(struct priv *p)
{
    if (!p->chunk_format)
        return NULL;
    struct mp_aframe *frame = talloc_steal(NULL, p->chunk_format);
    p->chunk_format = NULL;
    bool ok = mp_aframe_set_encoded_data(frame, p->chunk, p->chunk_bytes,
                                         p->chunk_samples);
    clear_chunk(p);
    if (!ok)
        TA_FREEP(&frame);
    return frame;
}

static bool append_chunk(struct priv *p, struct mp_aframe *frame)
{
    int bytes = mp_aframe_get_encoded_size(frame);
    int samples = mp_aframe_get_size(frame);
    if (bytes > INT_MAX - p->chunk_bytes || samples > INT_MAX - p->chunk_samples)
        return false;
    if (!p->chunk_format) {
        p->chunk_format = mp_aframe_create();
        talloc_steal(p, p->chunk_format);
        mp_aframe_config_copy(p->chunk_format, frame);
    }
    MP_TARRAY_GROW(p, p->chunk, p->chunk_bytes + bytes);
    memcpy(p->chunk + p->chunk_bytes, mp_aframe_get_encoded_data(frame), bytes);
    p->chunk_bytes += bytes;
    p->chunk_samples += samples;
    p->chunk_access_units++;
    return true;
}

static void process(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->failed) {
        mp_filter_internal_mark_failed(f);
        return;
    }
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    struct mp_aframe *frame = p->pending;
    p->pending = NULL;
    if (!frame) {
        struct mp_frame input = mp_pin_out_read(f->ppins[0]);
        if (!input.type)
            return;
        if (input.type == MP_FRAME_EOF) {
            if (p->chunk_format) {
                mp_pin_out_repeat_eof(f->ppins[0]);
                frame = take_chunk(p);
                if (!frame)
                    goto fail;
                goto output;
            }
            mp_pin_in_write(f->ppins[1], input);
            return;
        }
        if (input.type != MP_FRAME_PACKET) {
            mp_frame_unref(&input);
            goto fail;
        }

        struct demux_packet *pkt = input.data;
        if (p->avctx->codec_id == AV_CODEC_ID_TRUEHD && !p->truehd_synced) {
            if (pkt->len < 8 || AV_RB32(pkt->buffer + 4) != 0xf8726fba) {
                p->public.passthrough_discarded++;
                talloc_free(pkt);
                mp_filter_internal_mark_progress(f);
                return;
            }
            p->truehd_synced = true;
        }
        frame = parse_packet(p, pkt);
        if (!frame) {
            p->failed = true;
            talloc_free(pkt);
            // Finish already accepted access units before reporting the failed
            // packet. The wrapper retains the input for the next decoder.
            if (p->chunk_format) {
                frame = take_chunk(p);
                if (frame)
                    goto output;
            }
            goto fail;
        }
        talloc_free(pkt);
    }

    if (mp_aframe_get_format(frame) == AF_FORMAT_RAW_TRUEHD) {
        if (p->chunk_format && !mp_aframe_config_equals(p->chunk_format, frame)) {
            p->pending = talloc_steal(p, frame);
            frame = take_chunk(p);
            if (!frame)
                goto fail;
            goto output;
        }
        bool ok = append_chunk(p, frame);
        talloc_free(frame);
        if (!ok)
            goto fail;
        p->public.passthrough_consumed++;
        if (p->chunk_access_units < TRUEHD_CHUNK_ACCESS_UNITS) {
            mp_filter_internal_mark_progress(f);
            return;
        }
        frame = take_chunk(p);
        if (!frame)
            goto fail;
    } else {
        p->public.passthrough_consumed++;
    }

output:
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, frame));
    return;
fail:
    MP_ERR(f, "Unable to prepare a compressed audio access unit.\n");
    mp_filter_internal_mark_failed(f);
}

static const struct mp_filter_info passthrough_filter = {
    .name = "ad_passthrough",
    .priv_size = sizeof(struct priv),
    .process = process,
    .reset = reset,
    .destroy = destroy,
};

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_filter *f = mp_filter_create(parent, &passthrough_filter);
    if (!f)
        return NULL;
    struct priv *p = f->priv;
    p->public.f = f;
    p->codec = codec;
    p->use_dts_hd = strcmp(decoder, "passthrough_dts_hd") == 0;
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    if (!init_parser(p)) {
        talloc_free(f);
        return NULL;
    }
    return &p->public;
}

struct mp_decoder_list *select_passthrough_codec(const char *codec,
                                                const char *pref)
{
    struct mp_decoder_list *list = talloc_zero(NULL, struct mp_decoder_list);
    int id = mp_codec_to_av_codec_id(codec);
    if (id != AV_CODEC_ID_AC3 && id != AV_CODEC_ID_EAC3 &&
        id != AV_CODEC_ID_DTS && id != AV_CODEC_ID_TRUEHD)
        return list;
    bool allowed = false;
    bool dts_hd = false;
    bstr selection = bstr0(pref);
    while (selection.len) {
        bstr name;
        bstr_split_tok(selection, ",", &name, &selection);
        allowed |= bstr_equals0(name, codec);
        if (id == AV_CODEC_ID_DTS && bstr_equals0(name, "dts-hd"))
            allowed = dts_hd = true;
    }
    if (allowed) {
        char name[80];
        snprintf(name, sizeof(name), "passthrough_%s", dts_hd ? "dts_hd" : codec);
        mp_add_decoder(list, codec, name, "Compressed audio access units");
    }
    return list;
}

const struct mp_decoder_fns ad_passthrough = {
    .create = create,
};
