/*
 * This file is part of MPlayer.
 *
 * Copyright (C) 2012 Naoya OYAMA
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#include "config.h"
#include "mpvcore/mp_msg.h"
#include "mpvcore/av_common.h"
#include "mpvcore/options.h"
#include "ad.h"

#define FILENAME_SPDIFENC "spdif"
#define OUTBUF_SIZE 65536
struct spdifContext {
    AVFormatContext *lavf_ctx;
    int              iec61937_packet_size;
    int              out_buffer_len;
    int              out_buffer_size;
    uint8_t         *out_buffer;
    uint8_t          pb_buffer[OUTBUF_SIZE];
};

static void uninit(sh_audio_t *sh);

static int read_packet(void *p, uint8_t *buf, int buf_size)
{
    // spdifenc does not use read callback.
    return 0;
}

static int write_packet(void *p, uint8_t *buf, int buf_size)
{
    int len;
    struct spdifContext *ctx = p;

    len = FFMIN(buf_size, ctx->out_buffer_size -ctx->out_buffer_len);
    memcpy(&ctx->out_buffer[ctx->out_buffer_len], buf, len);
    ctx->out_buffer_len += len;
    return len;
}

static int64_t seek(void *p, int64_t offset, int whence)
{
    // spdifenc does not use seek callback.
    return 0;
}

static int preinit(sh_audio_t *sh)
{
    sh->samplesize = 2;
    return 1;
}

static int codecs[] = {
    AV_CODEC_ID_AAC,
    AV_CODEC_ID_AC3,
    AV_CODEC_ID_DTS,
    AV_CODEC_ID_EAC3,
    AV_CODEC_ID_MP3,
    AV_CODEC_ID_TRUEHD,
    AV_CODEC_ID_NONE
};

static int init(sh_audio_t *sh, const char *decoder)
{
    int srate, bps, *dtshd_rate;
    AVFormatContext     *lavf_ctx  = NULL;
    AVStream            *stream    = NULL;
    const AVOption      *opt       = NULL;
    struct spdifContext *spdif_ctx = NULL;

    spdif_ctx = av_mallocz(sizeof(*spdif_ctx));
    if (!spdif_ctx)
        goto fail;
    spdif_ctx->lavf_ctx = avformat_alloc_context();
    if (!spdif_ctx->lavf_ctx)
        goto fail;

    sh->context = spdif_ctx;
    lavf_ctx    = spdif_ctx->lavf_ctx;

    lavf_ctx->oformat = av_guess_format(FILENAME_SPDIFENC, NULL, NULL);
    if (!lavf_ctx->oformat)
        goto fail;
    lavf_ctx->priv_data = av_mallocz(lavf_ctx->oformat->priv_data_size);
    if (!lavf_ctx->priv_data)
        goto fail;
    lavf_ctx->pb = avio_alloc_context(spdif_ctx->pb_buffer, OUTBUF_SIZE, 1, spdif_ctx,
                            read_packet, write_packet, seek);
    if (!lavf_ctx->pb)
        goto fail;
    stream = avformat_new_stream(lavf_ctx, 0);
    if (!stream)
        goto fail;
    lavf_ctx->duration   = AV_NOPTS_VALUE;
    lavf_ctx->start_time = AV_NOPTS_VALUE;
    lavf_ctx->streams[0]->codec->codec_id = mp_codec_to_av_codec_id(decoder);
    lavf_ctx->raw_packet_buffer_remaining_size = RAW_PACKET_BUFFER_SIZE;
    if (AVERROR_PATCHWELCOME == lavf_ctx->oformat->write_header(lavf_ctx)) {
        mp_msg(MSGT_DECAUDIO,MSGL_INFO,
               "This codec is not supported by spdifenc.\n");
        goto fail;
    }

    srate = 48000;    //fake value
    bps   = 768000/8; //fake value

    int num_channels = 0;
    switch (lavf_ctx->streams[0]->codec->codec_id) {
    case AV_CODEC_ID_AAC:
        spdif_ctx->iec61937_packet_size = 16384;
        sh->sample_format               = AF_FORMAT_IEC61937_LE;
        sh->samplerate                  = srate;
        num_channels                    = 2;
        sh->i_bps                       = bps;
        break;
    case AV_CODEC_ID_AC3:
        spdif_ctx->iec61937_packet_size = 6144;
        sh->sample_format               = AF_FORMAT_AC3_LE;
        sh->samplerate                  = srate;
        num_channels                    = 2;
        sh->i_bps                       = bps;
        break;
    case AV_CODEC_ID_DTS:
        if(sh->opts->dtshd) {
            opt = av_opt_find(&lavf_ctx->oformat->priv_class,
                              "dtshd_rate", NULL, 0, 0);
            if (!opt)
                goto fail;
            dtshd_rate                      = (int*)(((uint8_t*)lavf_ctx->priv_data) +
                                              opt->offset);
            *dtshd_rate                     = 192000*4;
            spdif_ctx->iec61937_packet_size = 32768;
            sh->sample_format               = AF_FORMAT_IEC61937_LE;
            sh->samplerate                  = 192000; // DTS core require 48000
            num_channels                    = 2*4;
            sh->i_bps                       = bps;
        } else {
            spdif_ctx->iec61937_packet_size = 32768;
            sh->sample_format               = AF_FORMAT_AC3_LE;
            sh->samplerate                  = srate;
            num_channels                    = 2;
            sh->i_bps                       = bps;
        }
        break;
    case AV_CODEC_ID_EAC3:
        spdif_ctx->iec61937_packet_size = 24576;
        sh->sample_format               = AF_FORMAT_IEC61937_LE;
        sh->samplerate                  = 192000;
        num_channels                    = 2;
        sh->i_bps                       = bps;
        break;
    case AV_CODEC_ID_MP3:
        spdif_ctx->iec61937_packet_size = 4608;
        sh->sample_format               = AF_FORMAT_MPEG2;
        sh->samplerate                  = srate;
        num_channels                    = 2;
        sh->i_bps                       = bps;
        break;
    case AV_CODEC_ID_TRUEHD:
        spdif_ctx->iec61937_packet_size = 61440;
        sh->sample_format               = AF_FORMAT_IEC61937_LE;
        sh->samplerate                  = 192000;
        num_channels                    = 8;
        sh->i_bps                       = bps;
        break;
    default:
        break;
    }
    if (num_channels)
        mp_chmap_from_channels(&sh->channels, num_channels);

    return 1;

fail:
    uninit(sh);
    return 0;
}

static int decode_audio(sh_audio_t *sh, unsigned char *buf,
                        int minlen, int maxlen)
{
    struct spdifContext *spdif_ctx = sh->context;
    AVFormatContext     *lavf_ctx  = spdif_ctx->lavf_ctx;
    AVPacket            pkt;

    spdif_ctx->out_buffer_len  = 0;
    spdif_ctx->out_buffer_size = maxlen;
    spdif_ctx->out_buffer      = buf;
    while (spdif_ctx->out_buffer_len + spdif_ctx->iec61937_packet_size < maxlen
           && spdif_ctx->out_buffer_len < minlen) {
        struct demux_packet *mpkt = demux_read_packet(sh->gsh);
        if (!mpkt)
            break;
        mp_set_av_packet(&pkt, mpkt);
        mp_msg(MSGT_DECAUDIO,MSGL_V, "pkt.data[%p] pkt.size[%d]\n",
               pkt.data, pkt.size);
        if (mpkt->pts != MP_NOPTS_VALUE) {
            sh->pts       = mpkt->pts;
            sh->pts_bytes = 0;
        }
        int out_len = spdif_ctx->out_buffer_len;
        int ret = lavf_ctx->oformat->write_packet(lavf_ctx, &pkt);
        avio_flush(lavf_ctx->pb);
        sh->pts_bytes += spdif_ctx->out_buffer_len - out_len;
        talloc_free(mpkt);
        if (ret < 0)
            break;
    }
    return spdif_ctx->out_buffer_len;
}

static int control(sh_audio_t *sh, int cmd, void *arg)
{
    return CONTROL_UNKNOWN;
}

static void uninit(sh_audio_t *sh)
{
    struct spdifContext *spdif_ctx = sh->context;
    AVFormatContext     *lavf_ctx  = spdif_ctx->lavf_ctx;

    if (lavf_ctx) {
        if (lavf_ctx->oformat)
            lavf_ctx->oformat->write_trailer(lavf_ctx);
        av_freep(&lavf_ctx->pb);
        if (lavf_ctx->streams) {
            av_freep(&lavf_ctx->streams[0]->codec);
            av_freep(&lavf_ctx->streams[0]->info);
            av_freep(&lavf_ctx->streams[0]);
        }
        av_freep(&lavf_ctx->streams);
        av_freep(&lavf_ctx->priv_data);
    }
    av_freep(&lavf_ctx);
    av_freep(&spdif_ctx);
}

static void add_decoders(struct mp_decoder_list *list)
{
    for (int n = 0; codecs[n] != AV_CODEC_ID_NONE; n++) {
        const char *format = mp_codec_from_av_codec_id(codecs[n]);
        if (format) {
            mp_add_decoder(list, "spdif", format, format,
                           "libavformat/spdifenc audio pass-through decoder");
        }
    }
}

const struct ad_functions ad_spdif = {
    .name = "spdif",
    .add_decoders = add_decoders,
    .preinit = preinit,
    .init = init,
    .uninit = uninit,
    .control = control,
    .decode_audio = decode_audio,
};
