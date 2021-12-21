/*
 * Copyright (c) 2003 Bilibili
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ff_ffplay.h"

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <android/log.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#if CONFIG_AVDEVICE
#include "libavdevice/avdevice.h"
#endif
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

//#if CONFIG_AVFILTER
# include "libavcodec/avcodec.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
//#endif

#include "ijksdl/ijksdl_log.h"
#include "ijksdl/android/ijksdl_vout_android_surface.h"
#include "ijksdl/android/ijksdl_android_jni.h"
#include "ijkavformat/ijkavformat.h"
#include "ff_cmdutils.h"
#include "ff_fferror.h"
#include "ff_ffpipeline.h"
#include "ff_ffpipenode.h"
#include "ff_ffplay_debug.h"
#include "ijkmeta.h"
#include "ijkversion.h"
#include "ijkplayer.h"
#include <stdatomic.h>
#if defined(__ANDROID__)
#include "ijksoundtouch/ijksoundtouch_wrap.h"
#endif

#ifndef AV_CODEC_FLAG2_FAST
#define AV_CODEC_FLAG2_FAST CODEC_FLAG2_FAST
#endif

#ifndef AV_CODEC_CAP_DR1
#define AV_CODEC_CAP_DR1 CODEC_CAP_DR1
#endif

#include "android/pipeline/ffpipeline_android.h"
#include "ff_audio.h"
#include "ijksdl/android/ijksdl_android_jni.h"

// FIXME: 9 work around NDKr8e or gcc4.7 bug
// isnan() may not recognize some double NAN, so we test both double and float
#if defined(__ANDROID__)
#ifdef isnan
#undef isnan
#endif
#define isnan(x) (isnan((double)(x)) || isnanf((float)(x)))
#endif

#if defined(__ANDROID__)
#define printf(...) ALOGD(__VA_ARGS__)
#endif

#define FFP_IO_STAT_STEP (50 * 1024)

#define FFP_BUF_MSG_PERIOD (3)

// static const AVOption ffp_context_options[] = ...
#include "ff_ffplay_options.h"

static AVPacket flush_pkt;

#if CONFIG_AVFILTER
// FFP_MERGE: opt_add_vfilter
#endif

#define IJKVERSION_GET_MAJOR(x)     ((x >> 16) & 0xFF)
#define IJKVERSION_GET_MINOR(x)     ((x >>  8) & 0xFF)
#define IJKVERSION_GET_MICRO(x)     ((x      ) & 0xFF)

#define  LOG_TAG    "ijkplayer"
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)

static void sdl_audio_callback(void *opaque, void *data2, Uint8 *stream, int len);
static void clip_op_queue_flush(ClipEditOp *clipState);
long long clip_op_queue_calc_start_timeline(ClipInfo *head, ClipInfo *clip);
void ffp_clip_update_time(FFPlayer *ffp, VideoClip *clip, int64_t begin_timeline);

#if CONFIG_AVFILTER
static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{MPTRACE("%s begin", __func__);
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline
int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
{MPTRACE("%s begin", __func__);
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}
#endif

static void free_picture(Frame *vp);

int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    //MPTRACE("%s begin", __func__);
    MyAVPacketList *pkt1;

    if (q->abort_request) {
        //MPTRACE("%s end", __func__);
       return -1;
    }

#ifdef FFP_MERGE
    pkt1 = av_malloc(sizeof(MyAVPacketList));
#else
    pkt1 = q->recycle_pkt;
    if (pkt1) {
        q->recycle_pkt = pkt1->next;
        q->recycle_count++;
    } else {
        q->alloc_count++;
        pkt1 = av_malloc(sizeof(MyAVPacketList));
    }
#ifdef FFP_SHOW_PKT_RECYCLE
    int total_count = q->recycle_count + q->alloc_count;
    if (!(total_count % 50)) {
        av_log(ffp, AV_LOG_DEBUG, "pkt-recycle \t%d + \t%d = \t%d\n", q->recycle_count, q->alloc_count, total_count);
    }
#endif
#endif
    if (!pkt1) {
        //MPTRACE("%s end", __func__);
        return -1;
    }
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    q->duration += FFMAX(pkt1->pkt.duration, MIN_PKT_DURATION);
    MPTRACE("%s videolag count put %d queue %p ToTal %d", __func__,q->nb_packets,q,q->alloc_count);
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    //MPTRACE("%s end", __func__);
    return 0;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    //MPTRACE("%s begin", __func__);
    int ret;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    //MPTRACE("%s end", __func__);
    return ret;
}

int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    //MPTRACE("%s begin", __func__);
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    int ret = packet_queue_put(q, pkt);
    //MPTRACE("%s end", __func__);
    return ret;
}

/* packet queue handling */
int packet_queue_init(PacketQueue *q)
{
    //MPTRACE("%s begin", __func__);
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        //MPTRACE("%s end", __func__);
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        //MPTRACE("%s end", __func__);
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    //MPTRACE("%s end", __func__);
    return 0;
}

void packet_queue_flush(PacketQueue *q)
{
    //MPTRACE("%s begin", __func__);
    MyAVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
#ifdef FFP_MERGE
        av_freep(&pkt);
#else
        pkt->next = q->recycle_pkt;
        q->recycle_pkt = pkt;
#endif
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
    //MPTRACE("%s end", __func__);
}

void packet_queue_destroy(PacketQueue *q)
{
    //MPTRACE("%s begin", __func__);
    packet_queue_flush(q);

    SDL_LockMutex(q->mutex);
    while(q->recycle_pkt) {
        MyAVPacketList *pkt = q->recycle_pkt;
        if (pkt)
            q->recycle_pkt = pkt->next;
        av_freep(&pkt);
    }
    SDL_UnlockMutex(q->mutex);

    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
    //MPTRACE("%s end", __func__);
}

void packet_queue_abort(PacketQueue *q)
{
    //MPTRACE("%s begin", __func__);
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    //MPTRACE("%s end", __func__);
}

void packet_queue_start(PacketQueue *q)
{
    //MPTRACE("%s begin", __func__);
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
    //MPTRACE("%s end", __func__);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial, int *b)
{
    MPTRACE("%s video111 begin", __func__);
    MPTRACE("%s video113 get frame  %p video %s", __func__,q->cond, q->urlVid);
    MyAVPacketList *pkt1;
    int ret;
    int bVal = 0;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            if (pkt1->pkt.data == 0) {
                q->first_pkt = pkt1->next;
                if (!q->first_pkt)
                    q->last_pkt = NULL;

                bVal = 0;
            } else {
                if (pkt1->next == NULL) {
                    if (!block) {
                        ret = 0;
                        break;
                    }
                    goto LBL_WAIT_NEXT_MSG;
                }

                q->first_pkt = pkt1->next;
                bVal = (pkt1->next->pkt.data == NULL);
            }

            q->nb_packets--;
            MPTRACE("%s videolag count package remain %d queue %p", __func__,q->nb_packets,q);
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
//            q->duration -= FFMAX(pkt1->pkt.duration, MIN_PKT_DURATION);
            q->duration -= FFMAX(pkt1->pkt.duration, 0);
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            if (b) {
                *b = bVal;
            }
#ifdef FFP_MERGE
            av_free(pkt1);
#else
            pkt1->next = q->recycle_pkt;
            q->recycle_pkt = pkt1;
#endif
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
LBL_WAIT_NEXT_MSG:
            MPTRACE("%s video222 wait condition  %p video %s", __func__,q->cond, q->urlVid);
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    //MPTRACE("%s end", __func__);
    return ret;
}

int packet_queue_get_or_buffering(FFPlayer *ffp, VideoClip *player, PacketQueue *q, AVPacket *pkt, int *serial, int *finished, int *bVal)
{
    MPTRACE("%s video111 begin", __func__);
    int ret;
//    assert(finished);
    if (!ffp->packet_buffering) {
        ret = packet_queue_get(q, pkt, 1, serial, bVal);
        MPTRACE("%s end", __func__);
        return ret;
    }

    while (1) {
        int new_packet = packet_queue_get(q, pkt, 0, serial, bVal);
        MPTRACE("%s video111 packet_queue_get %d", __func__,new_packet);
        if (new_packet < 0) {
            MPTRACE("%s end", __func__);
            return -1;
        } else if (new_packet == 0) {
            if (q->is_buffer_indicator && !*finished)
                ffp_toggle_buffering(ffp, player, 1);
            new_packet = packet_queue_get(q, pkt, 1, serial, bVal);
            if (new_packet < 0) {
                MPTRACE("%s end", __func__);
                return -1;
            }
        }

        if (*finished == *serial) {
            av_packet_unref(pkt);
            continue;
        }
        else
            break;
    }

    //MPTRACE("%s end", __func__);
    return 1;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond, char *urlVideo) {

    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->url = av_strdup(urlVideo);
    d->first_frame_decoded_time = SDL_GetTickHR();
    d->first_frame_decoded = 0;
    d->queue->urlVid= av_strdup(urlVideo);
    SDL_ProfilerReset(&d->decode_profiler, -1);
    MPTRACE("%s end", __func__);
}

static int convert_image(FFPlayer *ffp, VideoClip *player, AVFrame *src_frame, int64_t src_frame_pts, int width, int height) {
    MPTRACE("%s begin", __func__);
    GetImgInfo *img_info = ffp->get_img_info;
    VideoState *is = player->is_0x30;
    AVFrame *dst_frame = NULL;
    AVPacket avpkt;
    int got_packet = 0;
    int dst_width = 0;
    int dst_height = 0;
    int bytes = 0;
    void *buffer = NULL;
    char file_path[1024] = {0};
    char file_name[16] = {0};
    int fd = -1;
    int ret = 0;
    int tmp = 0;
    float origin_dar = 0;
    float dar = 0;
    AVRational display_aspect_ratio;
    int file_name_length = 0;

    if (!height || !width || !img_info->width || !img_info->height) {
        ret = -1;
        MPTRACE("%s end", __func__);
        return ret;
    }

    dar = (float) img_info->width / img_info->height;

    if (is->viddec.avctx) {
        av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
            is->viddec.avctx->width * (int64_t)is->viddec.avctx->sample_aspect_ratio.num,
            is->viddec.avctx->height * (int64_t)is->viddec.avctx->sample_aspect_ratio.den,
            1024 * 1024);

        if (!display_aspect_ratio.num || !display_aspect_ratio.den) {
            origin_dar = (float) width / height;
        } else {
            origin_dar = (float) display_aspect_ratio.num / display_aspect_ratio.den;
        }
    } else {
        ret = -1;
        MPTRACE("%s end", __func__);
        return ret;
    }

    if ((int)(origin_dar * 100) != (int)(dar * 100)) {
        tmp = img_info->width / origin_dar;
        if (tmp > img_info->height) {
            img_info->width = img_info->height * origin_dar;
        } else {
            img_info->height = tmp;
        }
        av_log(NULL, AV_LOG_INFO, "%s img_info->width = %d, img_info->height = %d\n", __func__, img_info->width, img_info->height);
    }

    dst_width = img_info->width;
    dst_height = img_info->height;

    av_init_packet(&avpkt);
    avpkt.size = 0;
    avpkt.data = NULL;

    if (!img_info->frame_img_convert_ctx) {
        img_info->frame_img_convert_ctx = sws_getContext(width,
		    height,
		    src_frame->format,
		    dst_width,
		    dst_height,
		    AV_PIX_FMT_RGB24,
		    SWS_BICUBIC,
		    NULL,
		    NULL,
		    NULL);

        if (!img_info->frame_img_convert_ctx) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s sws_getContext failed\n", __func__);
            goto fail0;
        }
    }

    if (!img_info->frame_img_codec_ctx) {
        AVCodec *image_codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
        if (!image_codec) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s avcodec_find_encoder failed\n", __func__);
            goto fail0;
        }
	    img_info->frame_img_codec_ctx = avcodec_alloc_context3(image_codec);
        if (!img_info->frame_img_codec_ctx) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s avcodec_alloc_context3 failed\n", __func__);
            goto fail0;
        }
        img_info->frame_img_codec_ctx->bit_rate = ffp->stat.bit_rate;
        img_info->frame_img_codec_ctx->width = dst_width;
        img_info->frame_img_codec_ctx->height = dst_height;
        img_info->frame_img_codec_ctx->pix_fmt = AV_PIX_FMT_RGB24;
        img_info->frame_img_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        img_info->frame_img_codec_ctx->time_base.num = is->video_st->time_base.num;
        img_info->frame_img_codec_ctx->time_base.den = is->video_st->time_base.den;
        avcodec_open2(img_info->frame_img_codec_ctx, image_codec, NULL);
    }

    dst_frame = av_frame_alloc();
    if (!dst_frame) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s av_frame_alloc failed\n", __func__);
        goto fail0;
    }
    bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dst_width, dst_height, 1);
	buffer = (uint8_t *) av_malloc(bytes * sizeof(uint8_t));
    if (!buffer) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s av_image_get_buffer_size failed\n", __func__);
        goto fail1;
    }

    dst_frame->format = AV_PIX_FMT_RGB24;
    dst_frame->width = dst_width;
    dst_frame->height = dst_height;

    ret = av_image_fill_arrays(dst_frame->data,
            dst_frame->linesize,
            buffer,
            AV_PIX_FMT_RGB24,
            dst_width,
            dst_height,
            1);

    if (ret < 0) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s av_image_fill_arrays failed\n", __func__);
        goto fail2;
    }

    ret = sws_scale(img_info->frame_img_convert_ctx,
            (const uint8_t * const *) src_frame->data,
            src_frame->linesize,
            0,
            src_frame->height,
            dst_frame->data,
            dst_frame->linesize);

    if (ret <= 0) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s sws_scale failed\n", __func__);
        goto fail2;
    }

    ret = avcodec_encode_video2(img_info->frame_img_codec_ctx, &avpkt, dst_frame, &got_packet);
            MPTRACE("%s videotrack avcodec_encode_video2 filename", __func__);
    if (ret >= 0 && got_packet > 0) {
        strcpy(file_path, img_info->img_path);
        strcat(file_path, "/");
        sprintf(file_name, "%lld", src_frame_pts);
        strcat(file_name, ".png");
        strcat(file_path, file_name);

        fd = open(file_path, O_RDWR | O_TRUNC | O_CREAT, 0600);
        if (fd < 0) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s open path = %s failed %s\n", __func__, file_path, strerror(errno));
            goto fail2;
        }
        write(fd, avpkt.data, avpkt.size);
        close(fd);

        img_info->count--;

        file_name_length = (int)strlen(file_name) + 1;
        MPTRACE("%s videotrack FFP_MSG_GET_IMG_STATE filename frame %s", __func__,file_path);
        if (img_info->count <= 0)
            ffp_notify_msg4(ffp, FFP_MSG_GET_IMG_STATE, (int) src_frame_pts, 1, file_name, file_name_length);
        else
            ffp_notify_msg4(ffp, FFP_MSG_GET_IMG_STATE, (int) src_frame_pts, 0, file_name, file_name_length);

        ret = 0;
    }

fail2:
    av_free(buffer);
fail1:
    av_frame_free(&dst_frame);
fail0:
    av_packet_unref(&avpkt);

    MPTRACE("%s end", __func__);
    return ret;
}

static int decoder_decode_frame(FFPlayer *ffp, VideoClip *player, Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    //MPTRACE("%s begin", __func__);
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request) {
                    MPTRACE("%s end", __func__);
                    return -1;
                }

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            ffp->stat.vdps = SDL_SpeedSamplerAdd(&ffp->vdps_sampler, FFP_SHOW_VDPS_AVCODEC, "vdps[avcodec]");
                            if (ffp->decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!ffp->decoder_reorder_pts) {
                                MPTRACE("%s : %f speedrate 669 %f now ", __func__,player->speed_0xa0, frame->pts);
                                //frame->pts = frame->pkt_dts* player->speed_0xa0;
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        MPTRACE("%s : %f speedrate 669 %f now ", __func__,player->speed_0xa0, frame->pts);
                        //frame->pts = (int) (frame->pts* ffp->fps_now);
                        //frame->pts = frame->pts* 1/player->speed_0xa0;
                        MPTRACE("%s : %f speedrate 674 %f now ", __func__,player->speed_0xa0, frame->pts);
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            //frame->sample_rate = (int) (frame->sample_rate* 1/player->speed_0xa0);
                            AVRational tb = (AVRational){1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                            MPTRACE("%s AVMEDIA_TYPE_AUDIO : %f  %f  speed %f begin", __func__,frame->sample_rate, frame->pts, ffp->fps_now);
                            //frame->pts = (int) (frame->pts* 1/player->speed_0xa0);
                            MPTRACE("%s AVMEDIA_TYPE_AUDIO after:%f  %f  speed %f begin", __func__,frame->sample_rate, frame->pts, ffp->fps_now);
                        }
                        break;
                    default:
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    MPTRACE("%s end", __func__);
                    return 0;
                }
                if (ret >= 0) {
                    MPTRACE("%s end", __func__);
                    return 1;
                }
            } while (ret != AVERROR(EAGAIN));
        }

        do {
        bool done = false;
            MPTRACE("%s videolag count show count packet %d queue %p", __func__,d->queue->nb_packets,d->queue);
            if (d->queue->nb_packets == 0){
                MPTRACE("%s video222 run video index CondSignal cond %p file name %s", __func__,d->empty_queue_cond,d->url);
                SDL_CondSignal(d->empty_queue_cond);
                MPTRACE("%s videotrack1 run video index PLAY !!!! done video, switch next", __func__);

                done = true;
                //ffp_notify_msg1(ffp, FFP_MSG_0x186a1);
                //SDL_CondSignal(ffp->empty_queue_next);
            }
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                    //MPTRACE("%s video111 chuan bi stop condition ", __func__);
                    if(done){
                        player->doneVideo = 1;
                    }
                if (packet_queue_get_or_buffering(ffp, player, d->queue, &pkt, &d->pkt_serial, &d->finished, 0) < 0) {
                    MPTRACE("%s end", __func__);

                    return -1;
                }
            }
            if(done){
                player->doneVideo = 1;
            }
        } while (d->queue->serial != d->pkt_serial);

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                       d->packet_pending = 1;
                       av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);
        }
    }
    //MPTRACE("%s end", __func__);
}

static void decoder_destroy(Decoder *d) {
    MPTRACE("%s begin", __func__);
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
    MPTRACE("%s end", __func__);
}

void frame_queue_unref_item(Frame *vp)
{
    //MPTRACE("%s begin", __func__);
    av_frame_unref(vp->frame);
    SDL_VoutUnrefYUVOverlay(vp->bmp);
    avsubtitle_free(&vp->sub);
    //MPTRACE("%s end", __func__);
}

int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    //MPTRACE("%s begin", __func__);
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        //MPTRACE("%s end", __func__);
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        //MPTRACE("%s end", __func__);
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc())) {
            //MPTRACE("%s end", __func__);
            return AVERROR(ENOMEM);
        }

    //MPTRACE("%s end", __func__);
    return 0;
}

void frame_queue_destory(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
        free_picture(vp);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
    //MPTRACE("%s end", __func__);
}

void frame_queue_signal(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
    //MPTRACE("%s end", __func__);
}

Frame *frame_queue_peek(FrameQueue *f)
{
    Frame *ret = &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
    //MPTRACE("%s ret=%p", __func__, ret);
    return ret;
}

Frame *frame_queue_peek_next(FrameQueue *f)
{
    Frame *ret = &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
    //MPTRACE("%s ret=%p", __func__, ret);
    return ret;
}

Frame *frame_queue_peek_last(FrameQueue *f)
{
    Frame *ret = &f->queue[f->rindex];
    //MPTRACE("%s ret=%p", __func__, ret);
    return ret;
}

Frame *frame_queue_peek_writable(FrameQueue *f)
{

    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
           MPTRACE("%s video222 wait %p", __func__,f->cond);
        SDL_CondWait(f->cond, f->mutex);
           MPTRACE("%s video222 exit wait %p", __func__,f->cond);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request) {
        //MPTRACE("%s end", __func__);
        return NULL;
    }

    //MPTRACE("%s end", __func__);
    return &f->queue[f->windex];
}

Frame *frame_queue_peek_readable(FrameQueue *f)
{
    MPTRACE("%s video111 begin", __func__);
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
           MPTRACE("%s video222 wait %p", __func__,f->cond);
        SDL_CondWait(f->cond, f->mutex);
        MPTRACE("%s video222 exit wait %p", __func__,f->cond);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request) {
        //MPTRACE("%s end", __func__);
        return NULL;
    }

    //MPTRACE("%s end", __func__);
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void frame_queue_push(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
    //MPTRACE("%s end", __func__);
}

int frame_queue_prev(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    int ret = f->rindex_shown;
    f->rindex_shown = 0;
    //MPTRACE("%s end: %d", __func__, ret);
    return ret;
}

void frame_queue_next(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        //MPTRACE("%s end", __func__);
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
    //MPTRACE("%s end", __func__);
}

/* return the number of undisplayed frames in the queue */
int frame_queue_nb_remaining(FrameQueue *f)
{
    int ret = f->size - f->rindex_shown;
    MPTRACE("%s ret=%d", __func__, ret);
    return ret;
}

/* return last shown position */
#ifdef FFP_MERGE
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}
#endif

void frame_queue_write_break(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    SDL_LockMutex(f->mutex);
    f->windex = 1;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
    //MPTRACE("%s end", __func__);
}

void frame_queue_write_unbreak(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    SDL_LockMutex(f->mutex);
    f->windex = 0;
    SDL_UnlockMutex(f->mutex);
    //MPTRACE("%s end", __func__);
}

FrameQueue *frame_queue_flush(FrameQueue *f)
{
    //MPTRACE("%s begin", __func__);
    int iVar1;

    while( true ) {
        iVar1 = frame_queue_nb_remaining(f);
        if (iVar1 < 1) break;
        frame_queue_next(f);
    }
    //MPTRACE("%s end", __func__);
    return f;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    MPTRACE("%s begin", __func__);
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
    MPTRACE("%s end", __func__);
}

// FFP_MERGE: fill_rectangle
// FFP_MERGE: fill_border
// FFP_MERGE: ALPHA_BLEND
// FFP_MERGE: RGBA_IN
// FFP_MERGE: YUVA_IN
// FFP_MERGE: YUVA_OUT
// FFP_MERGE: BPP
// FFP_MERGE: blend_subrect

static void free_picture(Frame *vp)
{
    MPTRACE("%s begin", __func__);
    if (vp->bmp) {
        SDL_VoutFreeYUVOverlay(vp->bmp);
        vp->bmp = NULL;
    }
    MPTRACE("%s end", __func__);
}

// FFP_MERGE: realloc_texture
// FFP_MERGE: calculate_display_rect
// FFP_MERGE: upload_texture
// FFP_MERGE: video_image_display

static size_t parse_ass_subtitle(const char *ass, char *output)
{
    MPTRACE("%s begin", __func__);
    char *tok = NULL;
    tok = strchr(ass, ':'); if (tok) tok += 1; // skip event
    tok = strchr(tok, ','); if (tok) tok += 1; // skip layer
    tok = strchr(tok, ','); if (tok) tok += 1; // skip start_time
    tok = strchr(tok, ','); if (tok) tok += 1; // skip end_time
    tok = strchr(tok, ','); if (tok) tok += 1; // skip style
    tok = strchr(tok, ','); if (tok) tok += 1; // skip name
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_l
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_r
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_v
    tok = strchr(tok, ','); if (tok) tok += 1; // skip effect
    if (tok) {
        char *text = tok;
        size_t idx = 0;
        do {
            char *found = strstr(text, "\\N");
            if (found) {
                size_t n = found - text;
                memcpy(output+idx, text, n);
                output[idx + n] = '\n';
                idx = n + 1;
                text = found + 2;
            }
            else {
                size_t left_text_len = strlen(text);
                memcpy(output+idx, text, left_text_len);
                if (output[idx + left_text_len - 1] == '\n')
                    output[idx + left_text_len - 1] = '\0';
                else
                    output[idx + left_text_len] = '\0';
                break;
            }
        } while(1);
        MPTRACE("%s end", __func__);
        return strlen(output) + 1;
    }
    MPTRACE("%s end", __func__);
    return 0;
}

static void video_image_display2(FFPlayer *ffp, VideoClip *player)
{
    MPTRACE("%s begin", __func__);
    VideoState *is = player->is_0x30;
    Frame *vp;
    Frame *sp = NULL;

    if (((is->video_st == 0) && (is->is_image == '\0')) ||
        (vp = frame_queue_peek(&is->pictq), vp->bmp == 0)) {

        return;
    }

    SDL_VoutDisplayYUVOverlay(player->vout_0x8, vp->bmp);
    ffp->stat.vfps = SDL_SpeedSamplerAdd(&ffp->vfps_sampler, FFP_SHOW_VFPS_FFPLAY, "vfps[ffplay]");

    if (!ffp->first_video_frame_rendered) {
        ffp->first_video_frame_rendered = 1;
        ffp_notify_msg1(ffp, FFP_MSG_VIDEO_RENDERING_START);
    }

    is->f_0x101460 = (int64_t) (vp->pts * 1000000.0); //vp->pts

    MPTRACE("%s end", __func__);
}

// FFP_MERGE: compute_mod
// FFP_MERGE: video_audio_display

//static void stream_component_close(FFPlayer *ffp, int stream_index)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    AVFormatContext *ic = is->ic;
//    AVCodecParameters *codecpar;

//    if (stream_index < 0 || stream_index >= ic->nb_streams) {
//        MPTRACE("%s end", __func__);
//        return;
//    }
//    codecpar = ic->streams[stream_index]->codecpar;

//    switch (codecpar->codec_type) {
//    case AVMEDIA_TYPE_AUDIO:
//        decoder_abort(&is->auddec, &is->sampq);
//        SDL_AoutCloseAudio(ffp->aout);

//        decoder_destroy(&is->auddec);
//        swr_free(&is->swr_ctx);
//        av_freep(&is->audio_buf1);
//        is->audio_buf1_size = 0;
//        is->audio_buf = NULL;

//#ifdef FFP_MERGE
//        if (is->rdft) {
//            av_rdft_end(is->rdft);
//            av_freep(&is->rdft_data);
//            is->rdft = NULL;
//            is->rdft_bits = 0;
//        }
//#endif
//        break;
//    case AVMEDIA_TYPE_VIDEO:
//        decoder_abort(&is->viddec, &is->pictq);
//        decoder_destroy(&is->viddec);
//        break;
//    case AVMEDIA_TYPE_SUBTITLE:
//        decoder_abort(&is->subdec, &is->subpq);
//        decoder_destroy(&is->subdec);
//        break;
//    default:
//        break;
//    }

//    ic->streams[stream_index]->discard = AVDISCARD_ALL;
//    switch (codecpar->codec_type) {
//    case AVMEDIA_TYPE_AUDIO:
//        is->audio_st = NULL;
//        is->audio_stream = -1;
//        break;
//    case AVMEDIA_TYPE_VIDEO:
//        is->video_st = NULL;
//        is->video_stream = -1;
//        break;
//    case AVMEDIA_TYPE_SUBTITLE:
//        is->subtitle_st = NULL;
//        is->subtitle_stream = -1;
//        break;
//    default:
//        break;
//    }
//    MPTRACE("%s end", __func__);
//}

static void stream_component_close2(VideoClip *video, VideoState *is, int stream_index)
{
    MPTRACE("%s begin", __func__);
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        MPTRACE("%s end", __func__);
        return;
    }

    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_AoutCloseAudio(video->aout_0);

        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

#ifdef FFP_MERGE
        if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
#endif
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
    MPTRACE("%s end", __func__);
}

//static void stream_close(FFPlayer *ffp)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    /* XXX: use a special url_shutdown call to abort parse cleanly */
//    is->abort_request = 1;
//    packet_queue_abort(&is->videoq);
//    packet_queue_abort(&is->audioq);
//    av_log(NULL, AV_LOG_DEBUG, "wait for read_tid\n");
//    SDL_WaitThread(is->read_tid, NULL);

//    /* close each stream */
//    if (is->audio_stream >= 0)
//        stream_component_close(ffp, is->audio_stream);
//    if (is->video_stream >= 0)
//        stream_component_close(ffp, is->video_stream);
//    if (is->subtitle_stream >= 0)
//        stream_component_close(ffp, is->subtitle_stream);

//    avformat_close_input(&is->ic);

//    av_log(NULL, AV_LOG_DEBUG, "wait for video_refresh_tid\n");
//    SDL_WaitThread(is->video_refresh_tid, NULL);

//    packet_queue_destroy(&is->videoq);
//    packet_queue_destroy(&is->audioq);
//    packet_queue_destroy(&is->subtitleq);

//    /* free all pictures */
//    frame_queue_destory(&is->pictq);
//    frame_queue_destory(&is->sampq);
//    frame_queue_destory(&is->subpq);
//    SDL_DestroyCond(is->audio_accurate_seek_cond);
//    SDL_DestroyCond(is->video_accurate_seek_cond);
//    SDL_DestroyCond(is->continue_read_thread);
//    SDL_DestroyMutex(is->accurate_seek_mutex);
//    SDL_DestroyMutex(is->play_mutex);
//#if !CONFIG_AVFILTER
//    sws_freeContext(is->img_convert_ctx);
//#endif
//#ifdef FFP_MERGE
//    sws_freeContext(is->sub_convert_ctx);
//#endif

//#if defined(__ANDROID__)
//    if (ffp->soundtouch_enable && is->handle != NULL) {
//        ijk_soundtouch_destroy(is->handle);
//    }
//#endif
//    if (ffp->get_img_info) {
//        if (ffp->get_img_info->frame_img_convert_ctx) {
//            sws_freeContext(ffp->get_img_info->frame_img_convert_ctx);
//        }
//        if (ffp->get_img_info->frame_img_codec_ctx) {
//            avcodec_free_context(&ffp->get_img_info->frame_img_codec_ctx);
//        }
//        av_freep(&ffp->get_img_info->img_path);
//        av_freep(&ffp->get_img_info);
//    }
//    av_free(is->filename);
//    av_free(is);
//    ffp->is = NULL;
//    MPTRACE("%s end", __func__);
//}

static void stream_close2(FFPlayer *ffp, VideoClip *video)
{
    MPTRACE("%s begin", __func__);
    VideoState *is;
    int ret;

    is = video->is_0x30;
    if (is == NULL) {
        MPTRACE("%s end", __func__);
        return;
    }

    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    packet_queue_abort(&is->videoq);
    packet_queue_abort(&is->audioq);
    av_log(NULL, AV_LOG_DEBUG, "wait for read_tid\n");
    SDL_WaitThread(is->read_tid, NULL);
    av_log(NULL, AV_LOG_DEBUG, "wait for image load thread\n");
    SDL_WaitThread(is->image_load_thread, NULL);
    is->read_tid = NULL;
    is->image_load_thread = NULL;

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close2(video, is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close2(video, is, is->video_stream);
//    if (is->subtitle_stream >= 0)
//        stream_component_close2(video, is, is->subtitle_stream);

    if (is->ic != 0) {
        avformat_close_input(&is->ic);
    }

    if (is->is_image) {
        if (is->img_handler_0xa24 != 0) {
            av_frame_free(&is->img_handler_0xa24);
            is->img_handler_0xa24 = 0;
        }
    }

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);

    SDL_DestroyCond(is->continue_read_thread);

#if defined(__ANDROID__)
    if (ffp->soundtouch_enable && is->handle != NULL) {
        ijk_soundtouch_destroy(is->handle);
    }
#endif
    SDL_DestroyMutex(is->mutex_2_0x8);
    SDL_DestroyCond(is->cond_4_0x10);

#if !CONFIG_AVFILTER
    sws_freeContext(is->img_convert_ctx);
#endif
#ifdef FFP_MERGE
    sws_freeContext(is->sub_convert_ctx);
#endif

    ijkmeta_destroy_p(&is->meta);
    ijkmeta_reset(is->meta);

    SDL_LockMutex(ffp->mutex_0x408_0x81_0x102);
    Clock *clock = ffp->clock_0x410;
    if ((clock == &is->vidclk) || (clock == &is->audclk) || (clock == &is->extclk)){
        ffp->clock_0x410 = 0;
        ffp->cur_clip_begin_timeline = ffp_get_current_position_l(ffp);
    }
    SDL_UnlockMutex(ffp->mutex_0x408_0x81_0x102);

    JNIEnv *env = 0;
    ret = SDL_JNI_SetupThreadEnv(&env);
    if (ret == 0) {
        if (is->surface) {
            SDL_JNI_DeleteGlobalRefP(env, &is->surface);
        }

        if (is->surface_creator) {
            J4AC_tv_danmaku_ijk_media_player_ISurfaceCreator__releaseSurface(env, is->surface_creator);
            SDL_JNI_DeleteGlobalRefP(env, &is->surface_creator);
        }
    } else {
        av_log(ffp, 0x10,"%s: SetupThreadEnv failed\n", "stream_close_l");
    }

    av_free(is->filename);
    av_free(is);
    video->is_0x30 = NULL;

    //free: 0x38, 40, 48, 50

    SDL_VoutFreeP(&video->vout_0x8);
    SDL_AoutFreeP(&video->aout_0);
    ffpipenode_free_p(&video->node_vdec_0x18);
    ffpipeline_free_p(&video->pipeline_0x10);
    SDL_DestroyMutexP(&video->mutext_0x90);
    MPTRACE("%s end", __func__);
}

// FFP_MERGE: do_exit
// FFP_MERGE: sigterm_handler
// FFP_MERGE: video_open
// FFP_MERGE: video_display

/* display the current picture, if any */
static void video_display2(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    if (is->video_st)
//        video_image_display2(ffp);
    MPTRACE("%s end", __func__);
}

static double get_clock(Clock *c)
{
    //MPTRACE("%s begin", __func__);
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
    //MPTRACE("%s end", __func__);
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    //MPTRACE("%s begin: %p, %f, %d, %f", __func__, c, pts, serial, time);
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
    //MPTRACE("%s end", __func__);
}

static void set_clock(Clock *c, double pts, int serial)
{
    //MPTRACE("%s begin: %p, %f, %d", __func__, c, pts, serial);
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
    //MPTRACE("%s end", __func__);
}

static void set_clock_speed(Clock *c, double speed)
{
    MPTRACE("%s begin: %p, %f", __func__, c, speed);
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
    //MPTRACE("%s end", __func__);
}

static void init_clock(Clock *c, int *queue_serial)
{
    //MPTRACE("%s begin", __func__);
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
    //MPTRACE("%s end", __func__);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    //MPTRACE("%s begin: %p, %p", __func__, c, slave);
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
    //MPTRACE("%s end", __func__);
}

static int get_master_sync_type(VideoState *is) {
    //MPTRACE("%s begin. syntype=%d", __func__, is->av_sync_type);
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

static Clock *get_clock_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st) {
            return &is->vidclk;
        } else {
            if (is->audio_st) {
                return &is->audclk;
            }
            return &is->extclk;
        }
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return &is->audclk;
        else
            return &is->extclk;
    } else {
        return &is->extclk;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is) {
    MPTRACE("%s begin", __func__);
   if ((is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
       (is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)) {
       set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
   } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
              (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
       set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
       double speed = is->extclk.speed;
       if (speed != 1.0)
           set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
   }
   MPTRACE("%s end", __func__);
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    MPTRACE("%s begin", __func__);
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
    MPTRACE("%s end", __func__);
}

/* pause or resume the video */
//static void stream_toggle_pause_l(FFPlayer *ffp, int pause_on)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    if (is->paused && !pause_on) {
//        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;

//#ifdef FFP_MERGE
//        if (is->read_pause_return != AVERROR(ENOSYS)) {
//            is->vidclk.paused = 0;
//        }
//#endif
//        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
//        set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
//    } else {
//    }
//    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
//    if (is->step && (is->pause_req || is->buffering_on)) {
//        is->paused = is->vidclk.paused = is->extclk.paused = pause_on;
//    } else {
//        is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = pause_on;
//        SDL_AoutPauseAudio(ffp->aout, pause_on);
//    }
//    MPTRACE("%s end", __func__);
//}

static void stream_toggle_pause2_l(FFPlayer *ffp, VideoClip *clip, int pause_on)
{
    MPTRACE("%s begin. player=%p, pause_on=%d", __func__, clip, pause_on);
    VideoState *is = clip->is_0x30;
    AudioTrackEditOp *audioMgr;

    if (pause_on) {
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
        set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
        set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
        is->vidclk.paused = is->extclk.paused = is->audclk.paused = 1;
        is->paused = 1;

        SDL_AoutPauseAudio(clip->aout_0, 1);
        if (clip->isUsed_0x60) {
            audioMgr = ffp->audioState;
            audioMgr->c.paused = 1;
            audioMgr->f_0x8c8 = 1;
            SDL_AoutPauseAudio(audioMgr->aOut_0x8c0, 1);
        }
        MPTRACE("%s end", __func__);
        return;
    }

    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;

#ifdef FFP_MERGE
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
#endif
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
        set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);

    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = 0;
    SDL_AoutPauseAudio(clip->aout_0, 0);

    audioMgr = ffp->audioState;
    audioMgr->c.paused = 0;
    audioMgr->f_0x8c8 = 0;
    SDL_AoutPauseAudio(audioMgr->aOut_0x8c0, 0);

    if (is->is_image && is->img_handler_0xa24 && clip->isUsed_0x60) {
        is->force_refresh = 1;
    }
    MPTRACE("%s end", __func__);
}

//static void stream_update_pause_l(FFPlayer *ffp)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    if (!is->step && (is->pause_req || is->buffering_on)) {
//        stream_toggle_pause_l(ffp, 1);
//    } else {
//        stream_toggle_pause_l(ffp, 0);
//    }
//    MPTRACE("%s end", __func__);
//}

static void stream_update_pause2_l(FFPlayer *ffp, VideoClip *clip)
{
    VideoState *is = clip->is_0x30;
    MPTRACE("%s begin: player: %p, %d, %d, %d", __func__, clip, is->step, is->pause_req, is->buffering_on);
    if (!is->step && (is->pause_req || is->buffering_on)) {
        stream_toggle_pause2_l(ffp, clip, 1);
    } else {
        stream_toggle_pause2_l(ffp, clip, 0);
    }
    MPTRACE("%s end", __func__);
}

//static void toggle_pause_l(FFPlayer *ffp, int pause_on)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    if (is->pause_req && !pause_on) {
//        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
//        set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
//    }
//    is->pause_req = pause_on;
//    ffp->auto_resume = !pause_on;
//    stream_update_pause_l(ffp);
//    is->step = 0;
//    MPTRACE("%s end", __func__);
//}

//static void toggle_pause(FFPlayer *ffp, int pause_on)
//{
//    MPTRACE("%s begin", __func__);
//    SDL_LockMutex(ffp->is->play_mutex);
//    toggle_pause_l(ffp, pause_on);
//    SDL_UnlockMutex(ffp->is->play_mutex);
//    MPTRACE("%s end", __func__);
//}

static void toggle_pause2(FFPlayer *ffp, VideoClip *clip, int pause_on)
{
    MPTRACE("%s begin. pause_on=%d", __func__, pause_on);
    SDL_LockMutex(ffp->mutex_0x18);
    clip->is_0x30->pause_req = pause_on;
    ffp->auto_resume = !pause_on;
    stream_update_pause2_l(ffp, clip);
    SDL_UnlockMutex(ffp->mutex_0x18);
    MPTRACE("%s end", __func__);
}

// FFP_MERGE: toggle_mute
// FFP_MERGE: update_volume

//static void step_to_next_frame_l(FFPlayer *ffp)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    is->step = 1;
//    /* if the stream is paused unpause it, then step */
//    if (is->paused)
//        stream_toggle_pause_l(ffp, 0);
//    MPTRACE("%s end", __func__);
//}

static void step_to_next_frame2_l(FFPlayer *ffp, VideoClip *clip)
{
    MPTRACE("%s begin: %p, %d", __func__, clip, clip->is_0x30->paused);
    VideoState *is = clip->is_0x30;
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause2_l(ffp, clip, 0);
    is->step = 1;
    //MPTRACE("%s end", __func__);
}

static double compute_target_delay(FFPlayer *ffp, double delay, VideoState *is)
{
    MPTRACE("%s begin", __func__);
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        /* -- by bbcallen: replace is->max_frame_duration with AV_NOSYNC_THRESHOLD */
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    if (ffp) {
        ffp->stat.avdelay = delay;
        ffp->stat.avdiff  = diff;
    }
#ifdef FFP_SHOW_AUDIO_DELAY
    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);
#endif

    MPTRACE("%s end", __func__);
    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    //MPTRACE("%s begin", __func__);
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration) {
            MPTRACE("%s end: %f", __func__, vp->duration);
            return vp->duration;
        } else {
            MPTRACE("%s end: %f", __func__, duration);
            return duration;
        }
    } else {
        MPTRACE("%s end: 0", __func__);
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    MPTRACE("%s begin: %p, %f, %ld, %d", __func__, is, pts, pos, serial);
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
    //MPTRACE("%s end", __func__);
}

/* called to display each frame */
//static void video_refresh(FFPlayer *opaque, double *remaining_time)
//{
//    MPTRACE("%s begin", __func__);
//    FFPlayer *ffp = opaque;
//    VideoState *is = ffp->is;
//    double time;

//    Frame *sp, *sp2;

//    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
//        check_external_clock_speed(is);

//    if (!ffp->display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
//        time = av_gettime_relative() / 1000000.0;
//        if (is->force_refresh || is->last_vis_time + ffp->rdftspeed < time) {
//            video_display2(ffp);
//            is->last_vis_time = time;
//        }
//        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + ffp->rdftspeed - time);
//    }

//    if (is->video_st) {
//retry:
//        if (frame_queue_nb_remaining(&is->pictq) == 0) {
//            // nothing to do, no picture to display in the queue
//        } else {
//            double last_duration, duration, delay;
//            Frame *vp, *lastvp;

//            /* dequeue the picture */
//            lastvp = frame_queue_peek_last(&is->pictq);
//            vp = frame_queue_peek(&is->pictq);

//            if (vp->serial != is->videoq.serial) {
//                frame_queue_next(&is->pictq);
//                goto retry;
//            }

//            if (lastvp->serial != vp->serial)
//                is->frame_timer = av_gettime_relative() / 1000000.0;

//            if (is->paused)
//                goto display;

//            /* compute nominal last_duration */
//            last_duration = vp_duration(is, lastvp, vp);
//            delay = compute_target_delay(ffp, last_duration, is);

//            time= av_gettime_relative()/1000000.0;
//            if (isnan(is->frame_timer) || time < is->frame_timer)
//                is->frame_timer = time;
//            if (time < is->frame_timer + delay) {
//                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
//                goto display;
//            }

//            is->frame_timer += delay;
//            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
//                is->frame_timer = time;

//            SDL_LockMutex(is->pictq.mutex);
//            if (!isnan(vp->pts))
//                update_video_pts(is, vp->pts, vp->pos, vp->serial);
//            SDL_UnlockMutex(is->pictq.mutex);

//            if (frame_queue_nb_remaining(&is->pictq) > 1) {
//                Frame *nextvp = frame_queue_peek_next(&is->pictq);
//                duration = vp_duration(is, vp, nextvp);
//                if(!is->step && (ffp->framedrop > 0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
//                    frame_queue_next(&is->pictq);
//                    goto retry;
//                }
//            }

//            if (is->subtitle_st) {
//                while (frame_queue_nb_remaining(&is->subpq) > 0) {
//                    sp = frame_queue_peek(&is->subpq);

//                    if (frame_queue_nb_remaining(&is->subpq) > 1)
//                        sp2 = frame_queue_peek_next(&is->subpq);
//                    else
//                        sp2 = NULL;

//                    if (sp->serial != is->subtitleq.serial
//                            || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
//                            || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
//                    {
//                        if (sp->uploaded) {
//                            ffp_notify_msg4(ffp, FFP_MSG_TIMED_TEXT, 0, 0, "", 1);
//                        }
//                        frame_queue_next(&is->subpq);
//                    } else {
//                        break;
//                    }
//                }
//            }

//            frame_queue_next(&is->pictq);
//            is->force_refresh = 1;

//            SDL_LockMutex(ffp->is->play_mutex);
//            if (is->step) {
//                is->step = 0;
//                if (!is->paused)
//                    stream_update_pause_l(ffp);
//            }
//            SDL_UnlockMutex(ffp->is->play_mutex);
//        }
//display:
//        /* display picture */
//        if (!ffp->display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
//            video_display2(ffp);
//    }
//    is->force_refresh = 0;
//    if (ffp->show_status) {
//        static int64_t last_time;
//        int64_t cur_time;
//        int aqsize, vqsize, sqsize __unused;
//        double av_diff;

//        cur_time = av_gettime_relative();
//        if (!last_time || (cur_time - last_time) >= 30000) {
//            aqsize = 0;
//            vqsize = 0;
//            sqsize = 0;
//            if (is->audio_st)
//                aqsize = is->audioq.size;
//            if (is->video_st)
//                vqsize = is->videoq.size;
//#ifdef FFP_MERGE
//            if (is->subtitle_st)
//                sqsize = is->subtitleq.size;
//#else
//            sqsize = 0;
//#endif
//            av_diff = 0;
//            if (is->audio_st && is->video_st)
//                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
//            else if (is->video_st)
//                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
//            else if (is->audio_st)
//                av_diff = get_master_clock(is) - get_clock(&is->audclk);
//            av_log(NULL, AV_LOG_INFO,
//                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
//                   get_master_clock(is),
//                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
//                   av_diff,
//                   is->frame_drops_early + is->frame_drops_late,
//                   aqsize / 1024,
//                   vqsize / 1024,
//                   sqsize,
//                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
//                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
//            fflush(stdout);
//            last_time = cur_time;
//        }
//    }
//    MPTRACE("%s end", __func__);
//}

/* allocate a picture (needs to do that in main thread to avoid
   potential locking problems */
//static void alloc_picture_old(FFPlayer *ffp, int frame_format)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    Frame *vp;
//#ifdef FFP_MERGE
//    int sdl_format;
//#endif

//    vp = &is->pictq.queue[is->pictq.windex];

//    free_picture(vp);

//#ifdef FFP_MERGE
//    video_open(is, vp);
//#endif

//    SDL_VoutSetOverlayFormat(ffp->vout, ffp->overlay_format);
//    vp->bmp = SDL_Vout_CreateOverlay(vp->width, vp->height,
//                                   frame_format,
//                                   ffp->vout);
//#ifdef FFP_MERGE
//    if (vp->format == AV_PIX_FMT_YUV420P)
//        sdl_format = SDL_PIXELFORMAT_YV12;
//    else
//        sdl_format = SDL_PIXELFORMAT_ARGB8888;

//    if (realloc_texture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, 0) < 0) {
//#else
//    /* RV16, RV32 contains only one plane */
//    if (!vp->bmp || (!vp->bmp->is_private && vp->bmp->pitches[0] < vp->width)) {
//#endif
//        /* SDL allocates a buffer smaller than requested if the video
//         * overlay hardware is unable to support the requested size. */
//        av_log(NULL, AV_LOG_FATAL,
//               "Error: the video system does not support an image\n"
//                        "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
//                        "to reduce the image size.\n", vp->width, vp->height );
//        free_picture(vp);
//    }

//    SDL_LockMutex(is->pictq.mutex);
//    vp->allocated = 1;
//    SDL_CondSignal(is->pictq.cond);
//    SDL_UnlockMutex(is->pictq.mutex);
//    MPTRACE("%s end", __func__);
//}

static void alloc_picture(FFPlayer *ffp, VideoClip *clip, int frame_format)
{
    MPTRACE("%s begin", __func__);
    VideoState *is = clip->is_0x30;
    Frame *vp;
#ifdef FFP_MERGE
    int sdl_format;
#endif

    vp = &is->pictq.queue[is->pictq.windex];

    free_picture(vp);

#ifdef FFP_MERGE
    video_open(is, vp);
#endif

    SDL_VoutSetOverlayFormat(clip->vout_0x8, ffp->overlay_format);
    vp->bmp = SDL_Vout_CreateOverlay(vp->width, vp->height,
                                   frame_format,
                                   clip->vout_0x8);
#ifdef FFP_MERGE
    if (vp->format == AV_PIX_FMT_YUV420P)
        sdl_format = SDL_PIXELFORMAT_YV12;
    else
        sdl_format = SDL_PIXELFORMAT_ARGB8888;

    if (realloc_texture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, 0) < 0) {
#else
    /* RV16, RV32 contains only one plane */
    if (!vp->bmp || (!vp->bmp->is_private && vp->bmp->pitches[0] < vp->width)) {
#endif
        /* SDL allocates a buffer smaller than requested if the video
         * overlay hardware is unable to support the requested size. */
        av_log(NULL, AV_LOG_FATAL,
               "Error: the video system does not support an image\n"
                        "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                        "to reduce the image size.\n", vp->width, vp->height );
        free_picture(vp);
    }

    SDL_LockMutex(is->pictq.mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq.cond);
    SDL_UnlockMutex(is->pictq.mutex);
    MPTRACE("%s end", __func__);
}

//static int queue_picture(FFPlayer *ffp, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
//{
//    MPTRACE("%s begin", __func__);
//    VideoState *is = ffp->is;
//    Frame *vp;
//    int video_accurate_seek_fail = 0;
//    int64_t video_seek_pos = 0;
//    int64_t now = 0;
//    int64_t deviation = 0;

//    int64_t deviation2 = 0;
//    int64_t deviation3 = 0;

//    if (ffp->enable_accurate_seek && is->video_accurate_seek_req && !is->seek_req) {
//        if (!isnan(pts)) {
//            video_seek_pos = is->seek_pos;
//            is->accurate_seek_vframe_pts = pts * 1000 * 1000;
//            deviation = llabs((int64_t)(pts * 1000 * 1000) - is->seek_pos);
//            if ((pts * 1000 * 1000 < is->seek_pos) || deviation > MAX_DEVIATION) {
//                now = av_gettime_relative() / 1000;
//                if (is->drop_vframe_count == 0) {
//                    SDL_LockMutex(is->accurate_seek_mutex);
//                    if (is->accurate_seek_start_time <= 0 && (is->audio_stream < 0 || is->audio_accurate_seek_req)) {
//                        is->accurate_seek_start_time = now;
//                    }
//                    SDL_UnlockMutex(is->accurate_seek_mutex);
//                    av_log(NULL, AV_LOG_INFO, "video accurate_seek start, is->seek_pos=%lld, pts=%lf, is->accurate_seek_time = %lld\n", is->seek_pos, pts, is->accurate_seek_start_time);
//                }
//                is->drop_vframe_count++;

//                while (is->audio_accurate_seek_req && !is->abort_request) {
//                    int64_t apts = is->accurate_seek_aframe_pts ;
//                    deviation2 = apts - pts * 1000 * 1000;
//                    deviation3 = apts - is->seek_pos;

//                    if (deviation2 > -100 * 1000 && deviation3 < 0) {
//                        break;
//                    } else {
//                        av_usleep(20 * 1000);
//                    }
//                    now = av_gettime_relative() / 1000;
//                    if ((now - is->accurate_seek_start_time) > ffp->accurate_seek_timeout) {
//                        break;
//                    }
//                }

//                if ((now - is->accurate_seek_start_time) <= ffp->accurate_seek_timeout) {
//                    MPTRACE("%s end", __func__);
//                    return 1;  // drop some old frame when do accurate seek
//                } else {
//                    av_log(NULL, AV_LOG_WARNING, "video accurate_seek is error, is->drop_vframe_count=%d, now = %lld, pts = %lf\n", is->drop_vframe_count, now, pts);
//                    video_accurate_seek_fail = 1;  // if KEY_FRAME interval too big, disable accurate seek
//                }
//            } else {
//                av_log(NULL, AV_LOG_INFO, "video accurate_seek is ok, is->drop_vframe_count =%d, is->seek_pos=%lld, pts=%lf\n", is->drop_vframe_count, is->seek_pos, pts);
//                if (video_seek_pos == is->seek_pos) {
//                    is->drop_vframe_count       = 0;
//                    SDL_LockMutex(is->accurate_seek_mutex);
//                    is->video_accurate_seek_req = 0;
//                    SDL_CondSignal(is->audio_accurate_seek_cond);
//                    if (video_seek_pos == is->seek_pos && is->audio_accurate_seek_req && !is->abort_request) {
//                        SDL_CondWaitTimeout(is->video_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
//                    } else {
//                        ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(pts * 1000));
//                    }
//                    if (video_seek_pos != is->seek_pos && !is->abort_request) {
//                        is->video_accurate_seek_req = 1;
//                        SDL_UnlockMutex(is->accurate_seek_mutex);
//                        MPTRACE("%s end", __func__);
//                        return 1;
//                    }

//                    SDL_UnlockMutex(is->accurate_seek_mutex);
//                }
//            }
//        } else {
//            video_accurate_seek_fail = 1;
//        }

//        if (video_accurate_seek_fail) {
//            is->drop_vframe_count = 0;
//            SDL_LockMutex(is->accurate_seek_mutex);
//            is->video_accurate_seek_req = 0;
//            SDL_CondSignal(is->audio_accurate_seek_cond);
//            if (is->audio_accurate_seek_req && !is->abort_request) {
//                SDL_CondWaitTimeout(is->video_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
//            } else {
//                if (!isnan(pts)) {
//                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(pts * 1000));
//                } else {
//                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, 0);
//                }
//            }
//            SDL_UnlockMutex(is->accurate_seek_mutex);
//        }
//        is->accurate_seek_start_time = 0;
//        video_accurate_seek_fail = 0;
//        is->accurate_seek_vframe_pts = 0;
//    }

//#if defined(DEBUG_SYNC)
//    printf("frame_type=%c pts=%0.3f\n",
//           av_get_picture_type_char(src_frame->pict_type), pts);
//#endif

//    if (!(vp = frame_queue_peek_writable(&is->pictq))) {
//        MPTRACE("%s end", __func__);
//        return -1;
//    }

//    vp->sar = src_frame->sample_aspect_ratio;
//#ifdef FFP_MERGE
//    vp->uploaded = 0;
//#endif

//    /* alloc or resize hardware picture buffer */
//    if (!vp->bmp || !vp->allocated ||
//        vp->width  != src_frame->width ||
//        vp->height != src_frame->height ||
//        vp->format != src_frame->format) {

//        if (vp->width != src_frame->width || vp->height != src_frame->height)
//            ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, src_frame->width, src_frame->height);

//        vp->allocated = 0;
//        vp->width = src_frame->width;
//        vp->height = src_frame->height;
//        vp->format = src_frame->format;

//        /* the allocation must be done in the main thread to avoid
//           locking problems. */
//        alloc_picture_old(ffp, src_frame->format);

//        if (is->videoq.abort_request) {
//            MPTRACE("%s end", __func__);
//            return -1;
//        }
//    }

//    /* if the frame is not skipped, then display it */
//    if (vp->bmp) {
//        /* get a pointer on the bitmap */
//        SDL_VoutLockYUVOverlay(vp->bmp);

//#ifdef FFP_MERGE
//#if CONFIG_AVFILTER
//        // FIXME use direct rendering
//        av_image_copy(data, linesize, (const uint8_t **)src_frame->data, src_frame->linesize,
//                        src_frame->format, vp->width, vp->height);
//#else
//        // sws_getCachedContext(...);
//#endif
//#endif
//        // FIXME: set swscale options
//        if (SDL_VoutFillFrameYUVOverlay(vp->bmp, src_frame) < 0) {
//            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
//            exit(1);
//        }
//        /* update the bitmap content */
//        SDL_VoutUnlockYUVOverlay(vp->bmp);

//        vp->pts = pts;
//        vp->duration = duration;
//        vp->pos = pos;
//        vp->serial = serial;
//        vp->sar = src_frame->sample_aspect_ratio;
//        vp->bmp->sar_num = vp->sar.num;
//        vp->bmp->sar_den = vp->sar.den;

//#ifdef FFP_MERGE
//        av_frame_move_ref(vp->frame, src_frame);
//#endif
//        frame_queue_push(&is->pictq);
//        if (!is->viddec.first_frame_decoded) {
//            ALOGD("Video: first frame decoded\n");
//            ffp_notify_msg1(ffp, FFP_MSG_VIDEO_DECODED_START);
//            is->viddec.first_frame_decoded_time = SDL_GetTickHR();
//            is->viddec.first_frame_decoded = 1;
//        }
//    }
//    MPTRACE("%s end", __func__);
//    return 0;
//}

static int queue_picture2(FFPlayer *ffp, VideoClip *clip, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    MPTRACE("%s begin", __func__);
    VideoState *is = clip->is_0x30;
    Frame *vp;
    int64_t lVar13;
    int64_t lVar6;
    int64_t lVar10;

    int queue_size;

    if (serial != is->videoq.serial) {
        return 1;
    }

    if (!is->seek_req && is->f_0xdc) {
        if (!is->is_image) {
            lVar13 = is->seek_pos;
            if (!isnan(pts)) {
                lVar6 = is->ic->start_time;
                lVar10 = lVar13 + lVar6;
                if (lVar6 < 1) {
                    lVar10 = lVar13;
                }
                if ((pts * 1000000.0 < lVar10) && (is->viddec.packet_pending == 0)) {
                    return 1;
                }
                is->f_0xdc = 0;
            }
        }  else {
LAB_IS_IMAGE:
            queue_size = frame_queue_nb_remaining(&is->pictq);
            if (queue_size > 1) {
                    vp = frame_queue_peek_next(&is->pictq);
                vp->pts = pts;
                vp->duration = duration;
                vp->pos = pos;
                vp->serial = serial;
                return 0;
            }
        }
    } else {
        if (is->is_image) {
            goto LAB_IS_IMAGE;
        }
    }

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    //---------------
    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    //vp->sar = src_frame->sample_aspect_ratio;
#ifdef FFP_MERGE
    vp->uploaded = 0;
#endif

    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || !vp->allocated || vp->f_0x34) {
        if (vp->width != src_frame->width || vp->height != src_frame->height) {
LAB_ALLOC_FRAME:
            ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, src_frame->width, src_frame->height);
        }

        vp->allocated = 0;
        vp->f_0x34 = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;
        //vp->format = src_frame->format;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        alloc_picture(ffp, clip, src_frame->format);

        if (is->videoq.abort_request)
            return -1;
    } else {
        if (vp->width != src_frame->width || vp->height != src_frame->height) {
            goto LAB_ALLOC_FRAME;
        }
    }
    //if(clip->doneVideo == 1){
     //   return 0;
     //   }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        /* get a pointer on the bitmap */
        SDL_VoutLockYUVOverlay(vp->bmp);

#ifdef FFP_MERGE
#if CONFIG_AVFILTER
        // FIXME use direct rendering
        av_image_copy(data, linesize, (const uint8_t **)src_frame->data, src_frame->linesize,
                        src_frame->format, vp->width, vp->height);
#else
        // sws_getCachedContext(...);
#endif
#endif
        // FIXME: set swscale options
        if (SDL_VoutFillFrameYUVOverlay(vp->bmp, src_frame) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            exit(1);
        }
        /*int SDL_VoutFillFrameYUVOverlay(SDL_VoutOverlay *overlay, const AVFrame *frame)
        {
            if (!overlay || !overlay->func_fill_frame)
                return -1;

            return overlay->func_fill_frame(overlay, frame);
        }
        /* update the bitmap content */
        SDL_VoutUnlockYUVOverlay(vp->bmp);

        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;
        vp->sar = src_frame->sample_aspect_ratio;
        vp->bmp->sar_num = vp->sar.num;
        vp->bmp->sar_den = vp->sar.den;

#ifdef FFP_MERGE
        av_frame_move_ref(vp->frame, src_frame);
#endif
        frame_queue_push(&is->pictq);
        if (!is->viddec.first_frame_decoded) {
            ALOGD("Video: first frame decoded\n");
            //ffp_notify_msg1(ffp, FFP_MSG_VIDEO_DECODED_START);
            is->viddec.first_frame_decoded_time = SDL_GetTickHR();
            is->viddec.first_frame_decoded = 1;
        }
    }
    return 0;
}

static int get_video_frame(FFPlayer *ffp, VideoClip *player, AVFrame *frame)
{
    //MPTRACE("%s video111 begin", __func__);
    VideoState *is = player->is_0x30;
    int got_picture;

    ffp_video_statistic_l(ffp, player);
    if ((got_picture = decoder_decode_frame(ffp, player, &is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (ffp->framedrop>0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            ffp->stat.decode_frame_count++;
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    is->continuous_frame_drops_early++;
                    if (is->continuous_frame_drops_early > ffp->framedrop) {
                        is->continuous_frame_drops_early = 0;
                    } else {
                        ffp->stat.drop_frame_count++;
                        ffp->stat.drop_frame_rate = (float)(ffp->stat.drop_frame_count) / (float)(ffp->stat.decode_frame_count);
                        av_frame_unref(frame);
                        got_picture = 0;
                    }
                }
            }
        }
    }

    return got_picture;
}

#if CONFIG_AVFILTER
static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    MPTRACE("%s begin", __func__);
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(FFPlayer *ffp, AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    MPTRACE("%s begin", __func__);
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGRA, AV_PIX_FMT_NONE };
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    AVDictionaryEntry *e = NULL;

    while ((e = av_dict_get(ffp->sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (ffp->autorotate) {
        double theta  = get_rotation(is->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);
            INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }

#ifdef FFP_AVFILTER_PLAYBACK_RATE
    MPTRACE("%s playback index index begin %d", __func__,ffp->pf_playback_rate);
    if (fabsf(ffp->pf_playback_rate) > 0.00001 &&
        fabsf(ffp->pf_playback_rate - 1.0f) > 0.00001) {
        char setpts_buf[256];
        float rate = 1.0f / ffp->pf_playback_rate;
        rate = av_clipf_c(rate, 0.5f, 2.0f);
        av_log(ffp, AV_LOG_INFO, "vf_rate=%f(1/%f)\n", ffp->pf_playback_rate, rate);
        snprintf(setpts_buf, sizeof(setpts_buf), "%f*PTS", rate);
        INSERT_FILT("setpts", setpts_buf);
    }
#endif

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

static int configure_audio_filters(FFPlayer *ffp, const char *afilters, int force_output_format)
{
    MPTRACE("%s begin", __func__);
    VideoState *is = ffp->is;
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;
    char afilters_args[4096];

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);

    while ((e = av_dict_get(ffp->swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%"PRIx64,  is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;


    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels       [0] = is->audio_tgt.channels;
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts" , channels       ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    afilters_args[0] = 0;
    if (afilters)
        snprintf(afilters_args, sizeof(afilters_args), "%s", afilters);

#ifdef FFP_AVFILTER_PLAYBACK_RATE
    if (fabsf(ffp->pf_playback_rate) > 0.00001 &&
        fabsf(ffp->pf_playback_rate - 1.0f) > 0.00001) {
        if (afilters_args[0])
            av_strlcatf(afilters_args, sizeof(afilters_args), ",");

        av_log(ffp, AV_LOG_INFO, "af_rate=%f\n", ffp->pf_playback_rate);
        av_strlcatf(afilters_args, sizeof(afilters_args), "atempo=%f", ffp->pf_playback_rate);
    }
#endif

    if ((ret = configure_filtergraph(is->agraph, afilters_args[0] ? afilters_args : NULL, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}
#endif  /* CONFIG_AVFILTER */

static int audio_thread(void *arg, void *arg2)
{
    MPTRACE("%s begin", __func__);
    FFPlayer *ffp = arg;
    VideoClip *player = arg2;
    VideoState *is = player->is_0x30;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;
    int audio_accurate_seek_fail = 0;
    int64_t audio_seek_pos = 0;
    double frame_pts = 0;
    double audio_clock = 0;
    int64_t now = 0;
    double samples_duration = 0;
    int64_t deviation = 0;
    int64_t deviation2 = 0;
    int64_t deviation3 = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        ffp_audio_statistic_l(ffp, player);
        if ((got_frame = decoder_decode_frame(ffp, player, &is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};
                if (ffp->enable_accurate_seek && is->audio_accurate_seek_req && !is->seek_req) {
                    frame_pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                    now = av_gettime_relative() / 1000;
                    if (!isnan(frame_pts)) {
                        samples_duration = (double) frame->nb_samples / frame->sample_rate;
                        audio_clock = frame_pts + samples_duration;
                        is->accurate_seek_aframe_pts = audio_clock * 1000 * 1000;
                        audio_seek_pos = is->seek_pos;
                        deviation = llabs((int64_t)(audio_clock * 1000 * 1000) - is->seek_pos);
                        if ((audio_clock * 1000 * 1000 < is->seek_pos ) || deviation > MAX_DEVIATION) {
                            if (is->drop_aframe_count == 0) {
                                SDL_LockMutex(is->accurate_seek_mutex);
                                if (is->accurate_seek_start_time <= 0 && (is->video_stream < 0 || is->video_accurate_seek_req)) {
                                    is->accurate_seek_start_time = now;
                                }
                                SDL_UnlockMutex(is->accurate_seek_mutex);
                                av_log(NULL, AV_LOG_INFO, "audio accurate_seek start, is->seek_pos=%lld, audio_clock=%lf, is->accurate_seek_start_time = %lld\n", is->seek_pos, audio_clock, is->accurate_seek_start_time);
                            }
                            is->drop_aframe_count++;
                            while (is->video_accurate_seek_req && !is->abort_request) {
                                int64_t vpts = is->accurate_seek_vframe_pts;
                                deviation2 = vpts  - audio_clock * 1000 * 1000;
                                deviation3 = vpts  - is->seek_pos;
                                if (deviation2 > -100 * 1000 && deviation3 < 0) {

                                    break;
                                } else {
                                    av_usleep(20 * 1000);
                                }
                                now = av_gettime_relative() / 1000;
                                if ((now - is->accurate_seek_start_time) > ffp->accurate_seek_timeout) {
                                    break;
                                }
                            }

                            if(!is->video_accurate_seek_req && is->video_stream >= 0 && audio_clock * 1000 * 1000 > is->accurate_seek_vframe_pts) {
                                audio_accurate_seek_fail = 1;
                            } else {
                                now = av_gettime_relative() / 1000;
                                if ((now - is->accurate_seek_start_time) <= ffp->accurate_seek_timeout) {
                                    av_frame_unref(frame);
                                    continue;  // drop some old frame when do accurate seek
                                } else {
                                    audio_accurate_seek_fail = 1;
                                }
                            }
                        } else {
                            if (audio_seek_pos == is->seek_pos) {
                                av_log(NULL, AV_LOG_INFO, "audio accurate_seek is ok, is->drop_aframe_count=%d, audio_clock = %lf\n", is->drop_aframe_count, audio_clock);
                                is->drop_aframe_count       = 0;
                                SDL_LockMutex(is->accurate_seek_mutex);
                                is->audio_accurate_seek_req = 0;
                                SDL_CondSignal(is->video_accurate_seek_cond);
                                if (audio_seek_pos == is->seek_pos && is->video_accurate_seek_req && !is->abort_request) {
                                    SDL_CondWaitTimeout(is->audio_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
                                } else {
                                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(audio_clock * 1000));
                                }

                                if (audio_seek_pos != is->seek_pos && !is->abort_request) {
                                    is->audio_accurate_seek_req = 1;
                                    SDL_UnlockMutex(is->accurate_seek_mutex);
                                    av_frame_unref(frame);
                                    continue;
                                }

                                SDL_UnlockMutex(is->accurate_seek_mutex);
                            }
                        }
                    } else {
                        audio_accurate_seek_fail = 1;
                    }
                    if (audio_accurate_seek_fail) {
                        av_log(NULL, AV_LOG_INFO, "audio accurate_seek is error, is->drop_aframe_count=%d, now = %lld, audio_clock = %lf\n", is->drop_aframe_count, now, audio_clock);
                        is->drop_aframe_count       = 0;
                        SDL_LockMutex(is->accurate_seek_mutex);
                        is->audio_accurate_seek_req = 0;
                        SDL_CondSignal(is->video_accurate_seek_cond);
                        if (is->video_accurate_seek_req && !is->abort_request) {
                            SDL_CondWaitTimeout(is->audio_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
                        } else {
                            ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(audio_clock * 1000));
                        }
                        SDL_UnlockMutex(is->accurate_seek_mutex);
                    }
                    is->accurate_seek_start_time = 0;
                    audio_accurate_seek_fail = 0;
                }

#if CONFIG_AVFILTER
                dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   frame->format, frame->channels)    ||
                    is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    is->auddec.pkt_serial               != last_serial        ||
                    ffp->af_changed;

                if (reconfigure) {
                    SDL_LockMutex(ffp->af_mutex);
                    ffp->af_changed = 0;
                    char buf1[1024], buf2[1024];
                    av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                    av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                    av_log(NULL, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, frame->channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                    is->audio_filter_src.fmt            = frame->format;
                    is->audio_filter_src.channels       = frame->channels;
                    is->audio_filter_src.channel_layout = dec_channel_layout;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial;

                    if ((ret = configure_audio_filters(ffp, ffp->afilters, 1)) < 0) {
                        SDL_UnlockMutex(ffp->af_mutex);
                        goto the_end;
                    }
                    SDL_UnlockMutex(ffp->af_mutex);
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

static int sdl_audio_open(FFPlayer *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    MPTRACE("%s begin", __func__);
    FFPlayer *ffp = opaque;
//    VideoState *is = ffp->is;
    AudioTrackEditOp *audioState = ffp->audioState;
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
#ifdef FFP_MERGE
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
#endif
    static const int next_sample_rates[] = {0, 44100, 48000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AoutGetAudioPerSecondCallBacks(audioState->aOut_0x8c0)));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (SDL_AoutOpenAudio(audioState->aOut_0x8c0, &wanted_spec, &spec) < 0) {
        /* avoid infinity loop on exit. --by bbcallen */
        if (audioState->abort_req)
            return -1;
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    SDL_AoutSetDefaultLatencySeconds(audioState->aOut_0x8c0, ((double)(2 * spec.size)) / audio_hw_params->bytes_per_sec);
    return spec.size;
}

AudioDecodeContext *audio_component_open(AudioClip *audioClip, char *url)
{
    int iVar2;
    int iVar3;
    AVCodecContext *lVar4;
    int64_t lVar5;
    AudioDecodeContext *plVar6;
    AVStream *lVar7;
    AVFormatContext *local_18;
    AVCodec *local_10;

    if ((url == 0) || (audioClip == 0)) {
        plVar6 = 0;
        goto LAB_00120bd4;
    }

    plVar6 = av_mallocz(sizeof(AudioDecodeContext));
    if (plVar6 == 0x0) {
        av_log(0,8,"malloc audiostate instance failed");
        plVar6 = 0;
        goto LAB_00120bd4;
    }

    local_18 = avformat_alloc_context();
    local_10 = 0;
    if (local_18 == 0) {
        av_log(0,8,"%s:Could not allocate context.\n","audio_component_open");
joined_r0x00120c74:
        if (local_18 != 0) {
LAB_00120b94:
            avformat_close_input(&local_18);
            plVar6->fmtCtx = 0;
        }
        if (plVar6->codecCtx != 0) {
            avcodec_close(plVar6->codecCtx);
            plVar6->codecCtx = 0;
        }
    } else {
        iVar2 = avformat_open_input(&local_18, url, 0, 0);
        if (iVar2 < 0) goto joined_r0x00120c74;
        iVar2 = avformat_find_stream_info(local_18, 0);
        if (iVar2 < 0) {
            av_log(0,0x10,"%s:could not find stream info for %s","audio_component_open",url);
            goto joined_r0x00120c74;
        }
        iVar2 = av_find_best_stream(local_18, AVMEDIA_TYPE_AUDIO, -1, -1, &local_10,0);
        if (iVar2 < 0) {
            av_log(0,0x10,"%s:get audio stream failed, ast_index","audio_component_open");
            goto joined_r0x00120c74;
        }
        lVar4 = avcodec_alloc_context3(0);
        if (lVar4 == 0) {
            av_log(0,0x10,"%s:avcodec_alloc_context3 failed!","audio_component_open");
            goto joined_r0x00120c74;
        }
        iVar3 = avcodec_parameters_to_context (lVar4, local_18->streams[iVar2]->codecpar);
        if (-1 < iVar3) {
            av_codec_set_pkt_timebase (lVar4, local_18->streams[iVar2]->time_base);
            av_opt_set_int(lVar4,"refcounted_frames",1,0);
            iVar3 = avcodec_open2(lVar4,local_10,0);
            if (-1 < iVar3) {
                lVar7 = local_18->streams[iVar2];
                lVar5 = lVar7->duration;
                if ((lVar5 != AV_NOPTS_VALUE) &&
                        (lVar5 = av_rescale_q(lVar5, lVar7->time_base, AV_TIME_BASE_Q),
                         lVar5 < audioClip->end_file)) {
                    audioClip->end_file = lVar5;
                    audioClip->duration = lVar5 - audioClip->begin_file;
                }

                for (int i = 0; i < local_18->nb_streams; i++) {
                    if (iVar2 != i) {
                        local_18->streams[i]->discard = AVDISCARD_ALL;
                    }
                }

                plVar6->fmtCtx = local_18;
                plVar6->codecCtx = lVar4;
                plVar6->f_0x30 = 1;
                plVar6->stream = local_18->streams[iVar2];
                plVar6->f_0x34 = 0;
                plVar6->timebase = plVar6->stream->time_base;
                plVar6->stream_index_3 = iVar2;
                plVar6->stream_start_time = plVar6->stream->start_time;
                goto LAB_00120bd4;
            }
            av_log(0,0x10,"cannot open audio decoder for %s",url);
        }
        avcodec_close(lVar4);
        plVar6->codecCtx = 0;
        if (local_18 != 0) goto LAB_00120b94;
    }

    if (plVar6->fmtCtx != 0) {
        avformat_close_input(&plVar6->fmtCtx);
    }
    av_free(plVar6);
    plVar6 = 0x0;

LAB_00120bd4:
    MPTRACE("%s(%s) return %p", __func__, url, plVar6);
    return plVar6;
}

int audio_track_open_decode_clips(AudioTrackEditOp *audioMgr)
{
    int iVar1;
    AudioTrackInfo *lVar2;
    AudioDecodeContext *puVar3;
    AudioClip *puVar4;
    int iVar5;
    int ret;

    if (0 < audioMgr->f_0x20) {
        iVar5 = 0;
        for (int i = 0; i < 4; i++) {
            lVar2 = audioMgr->arr[i];
            if ((lVar2 != 0) && (0 < lVar2->size)) {
                if ((lVar2->f_0xe == 0) &&
                    (puVar4 = lVar2->clip_0x9, puVar4 != 0x0 && puVar4->decodeCtx == 0)) {
                    puVar3 = audio_component_open(puVar4, puVar4->url);
                    puVar4->decodeCtx = puVar3;
                    if (puVar3 == 0x0) {
                        av_log(0,0x10,"audio component open failed");
                        ret = -1;
                        goto EXIT;
                    }

                    iVar1 = audio_accurate_seek(puVar3->fmtCtx, puVar3->stream_index_3, puVar4->begin_file);
                    if (iVar1 < 0) {
                        av_log(0,0x10,"audio seek error");
                        ret = -2;
                        goto EXIT;
                    }
                }
                iVar5++;
                if (iVar5 >= audioMgr->f_0x20) {
                    ret = 0;
                    goto EXIT;
                }
            }
        }

    }
    ret = 0;
EXIT:
    MPTRACE("%s() return %d", __func__, ret);
    return ret;
}

void audio_decode_context_flush(AudioDecodeContext *context) {
    avcodec_flush_buffers(context->codecCtx);

    context->f_0xf0 = 0;
    context->f_0x108 = context->stream_start_time;
    context->timebase2 = context->timebase;
    context->f_0x34 = 0;
}

int audio_thread_p1(AudioTrackEditOp *audioMgr) {
    int iVar6;
    AudioTrackInfo *puVar24;
    int uVar11;
    AudioClip *puVar18;
    AudioClip *curClip;
    int uVar7;
    int64_t lVar13;
    int64_t lVar21;
    int64_t lVar19;
    AudioDecodeContext *puVar25;

    for (int i = 0; i < MAX_AUDIO_TRACK; i++) {
        if (audioMgr->arr[i] != NULL) {
            audioMgr->arr[i]->f_0x10 = -1.0;
        }
    }

    lVar21 = audioMgr->f_0x8b0;
    uVar11 = 0;
    audioMgr->f_0x968 = 0;
    uVar7 = 0;
    iVar6 = 0;
    audioMgr->f_0x960 = -1.0;
    do {
        if (audioMgr->f_0x20 <= iVar6) break;
        puVar24 = audioMgr->arr[uVar11];
        if (puVar24->size != 0) {
            puVar18 = puVar24->head;
            iVar6 = iVar6 + 1;
            if (puVar18 == 0x0) {
                uVar7 = 0;
                if (puVar24->f_0xe == 0) {
LAB_00124500:
                    audioMgr->f_0x968++;
                }
            } else {
                do {
                    lVar13 = puVar18->start_timeline;
                    if (lVar21 <= lVar13 + puVar18->duration) {
                        puVar25 = puVar18->decodeCtx;
                        if (puVar25 == 0x0) {
                            puVar25 = audio_component_open(puVar18, puVar18->url);
                            puVar18->decodeCtx = puVar25;
                            if (puVar25 != 0x0) {
                                goto LAB_00124438;
                            }
                            uVar7 = -1;
                            av_log(0,0x10,"audio component open failed");
                        } else {
LAB_00124438:
                            if (lVar21 < lVar13) {
                                puVar18->pre_end_timeline_0x18 = lVar21;
                                uVar7 = audio_accurate_seek(puVar25->fmtCtx, puVar25->stream_index_3, puVar18->begin_file);
                                if (-1 < uVar7) {
LAB_00124464:
                                    curClip = puVar24->clip_0x9;
                                    if ((curClip != puVar18) && (curClip != 0x0)) {
                                        audio_accurate_seek(curClip->decodeCtx->fmtCtx,
                                                             curClip->decodeCtx->stream_index_3, curClip->begin_file);

                                        audio_decode_context_flush(curClip->decodeCtx);
                                        curClip->pre_end_timeline_0x18 = curClip->start_timeline - curClip->pre_distance_0x10;
                                    }

                                    audio_decode_context_flush(puVar18->decodeCtx);

                                    puVar24->clip_0x9 = puVar18;
                                    puVar24->f_0xe = 0;
                                    goto LAB_00124500;
                                }
                                av_log(0,0x10,"audio seek error");
                            } else {
                                lVar19 = (lVar21 - lVar13) + puVar18->begin_file;
                                uVar7 = puVar25->stream_index_3;
                                puVar18->pre_end_timeline_0x18 = lVar13;
                                uVar7 = audio_accurate_seek(puVar25->fmtCtx,
                                                              puVar25->stream_index_3, lVar19);
                                if (-1 < uVar7) goto LAB_00124464;
                                av_log(0,0x10,
                                       "audio seek to adjust seek poserror,errcode:%d,m_audioStreamIndex:%d,pos:%lld,flags:%d"
                                       ,uVar7, puVar25->stream_index_3, lVar19, 4);
                            }
                        }
                        av_log(0,0x10,"audio track:%d do seek pos failed", uVar11);
//                        uVar10 = 0x10;
//                        if (!bVar4) {
//                            uVar10 = 8;
//                        }
//                        bVar4 = true;
//                        av_log(0, uVar10,"audio_track_seek_to_pos failed");
                        return -1;
                    }
                    puVar18 = puVar18->next;
                } while (puVar18 != 0x0);
                puVar24->clip_0x9 = 0;
                uVar7 = 0;
                puVar24->f_0xe = 1;
            }
        }
        uVar11 = uVar11 + 1;
    } while (uVar11 != 4);

    audioMgr->f_0x8a0 = 0;
    return 0;
}

//check audio format
int check_audio_format_change(AudioTrackEditOp *audioMgr, char bVar1, int local_bc) {
    AudioTrackInfo *lVar21;
    AVCodecContext *lVar19;
    uint64_t lVar13;
    int count = 0;

    if ((bVar1) || (audioMgr->af_change)) {
        return 1;
    }

    if (audioMgr->f_0x20 < 1) {
        return 0;
    }

    for (int i = 0; i < MAX_AUDIO_TRACK; i++) {
        lVar21 = audioMgr->arr[i];
        if (lVar21->size > 0) {
            if (lVar21->f_0xe == 0) {
                lVar19 = lVar21->clip_0x9->decodeCtx->codecCtx;
                lVar13 = lVar19->channel_layout;
                if (lVar13 == 0) {
                    lVar13 = av_get_default_channel_layout(lVar19->channels);
                    lVar19->channel_layout = lVar13;
                }

                if (((lVar21->chan_layout != lVar13) ||
                     (lVar21->num_chans != lVar19->channels)) ||
                        (lVar21->sampleFormat != lVar19->sample_fmt)) {
                    return 1;
                }

                if ((lVar21->sample_rate != lVar19->sample_rate) || (local_bc != audioMgr->pktQueue.serial)) {
                    return 1;
                }
            }
            count++;
            if (count >= audioMgr->f_0x20) {
                return 0;
            }
        }
    }

    return 0;
}

int get_audio_frame_from_audio_clip(AudioTrackEditOp *audioMgr, AudioClip *clip, AVFrame *frame, int *gotFrame, int *uVar7) {
    AudioDecodeContext *puVar24;
    AVPacket pkt;
    int64_t lVar26;
//    int uVar7;
    AVRational uVar15;
    int64_t framePts;
    uint8_t *data;

    puVar24 = clip->decodeCtx;
    if (puVar24->f_0xf0 == 0) {
        av_init_packet(&pkt);
        while (av_read_frame(puVar24->fmtCtx, &pkt) != AVERROR_EOF) {
            if (pkt.stream_index == puVar24->stream_index_3) {
                lVar26 = pkt.pts;
                if (lVar26 == AV_NOPTS_VALUE) {
                    lVar26 = pkt.dts;
                }
                lVar26 = av_rescale_q(lVar26, puVar24->stream->time_base, AV_TIME_BASE_Q);
                if (clip->begin_file <= lVar26) {
                    if (lVar26 <= clip->end_file) goto LAB_00124118;
                    break;
                }
            }
            av_packet_unref(&pkt);
        }
        av_packet_unref(&pkt);
LAB_00124118:
        av_packet_unref(&puVar24->pkt);
        //av_packet_unref(&puVar24->pkt2);
        puVar24->f_0xf0 = 1;
        puVar24->pkt = pkt;
        puVar24->pkt2 = pkt;
    }
    *uVar7 = avcodec_decode_audio4(puVar24->codecCtx, frame, gotFrame, &puVar24->pkt2);
    if (-1 < *uVar7) {
        if (*gotFrame == 0) {
            if (puVar24->pkt2.data != 0) goto LAB_001242ac;
LAB_00124314:
            puVar24->f_0xf0 = 0;
            puVar24->f_0x34 = 1;
            avcodec_flush_buffers(puVar24->codecCtx);
            //goto LAB_001242c8;
            return 0;
        }
        framePts = frame->pts;
        if (frame->pts == AV_NOPTS_VALUE) {
            if (puVar24->f_0x108 != AV_NOPTS_VALUE) {
                lVar26 = av_rescale_q(puVar24->f_0x108, puVar24->timebase2, AV_TIME_BASE_Q);
                frame->pts = lVar26;
                goto LAB_00124200;
            }
            framePts = frame->pkt_dts;
            if (frame->pkt_dts != AV_NOPTS_VALUE) goto LAB_001241c4;
        } else {
LAB_001241c4:
            uVar15 = av_codec_get_pkt_timebase(puVar24->codecCtx);
            lVar26 = av_rescale_q(framePts, uVar15, AV_TIME_BASE_Q);
            frame->pts = lVar26;
LAB_00124200:
            if (lVar26 != AV_NOPTS_VALUE) {
                puVar24->f_0x108 = frame->nb_samples + lVar26;
                puVar24->timebase2 = AV_TIME_BASE_Q;
                frame->pts = (lVar26 - clip->begin_file) + clip->start_timeline;
            }
        }
        if (puVar24->pkt2.data != 0) {
LAB_001242ac:
            puVar24->pkt2.data += (*uVar7);
            puVar24->pkt2.size -= (*uVar7);
            if (puVar24->pkt2.size < 1) {
                puVar24->f_0xf0 = 0;
            }
//            goto LAB_001242c8;
            return 0;
        }
        if (*gotFrame == 0) goto LAB_00124314;
        if (!audioMgr->abort_req) {
            //goto LAB_00123d38;
            return -2;
        }

        *uVar7 = -1;
//        goto LAB_001243b0;
        return -3;
    }

    av_log(0,0x18,"%s:warnning, avcodec_decode_audio4 failed", "get_audio_frame_from_audio_clip");
    puVar24->f_0xf0 = 0;
    return 0;
}

void audio_check_1(FFPlayer *ffp, AudioTrackEditOp *audioMgr) {
    int count = 0;
    AudioTrackInfo *track;

    if (audioMgr->f_0x20 < 1) {
LAB_0012401c:
        MPTRACE("%s 870 change to 1: %d", __func__, audioMgr->f_0x870);
        audioMgr->f_0x870 = 1;
        if (ffp->audio_only) {
            packet_queue_put(&audioMgr->pktQueue, &flush_pkt);
        }
    } else {
        for (int i = 0; i < MAX_AUDIO_TRACK; i++) {
            track = audioMgr->arr[i];
            if (track->size > 0) {
                if (track->f_0xe == 0) {
                    audioMgr->f_0x870 = 0;
                    return;
                }
                count++;
                if (count >= audioMgr->f_0x20) {
                    goto LAB_0012401c;
                }
            }
        }
    }
}

int audio_thread_p3(FFPlayer *ffp, AudioTrackEditOp *audioMgr, int *iVar6, int *uVar7) {
    int uVar10;
    AudioTrackInfo *track;
    AVFrame *frameTmp;
    AudioClip *clip;
    AVCodecContext *lVar23;
    int64_t lVar22;
    int64_t lVar26;
    int64_t lVar14;
    int uVar11;
    int bufferSize;
    uint8_t *buf;
    int ret;

    *iVar6 = audioMgr->f_0x8a0;
    uVar10 = 0;
    do {
        if (audioMgr->f_0x8a0 != 0) {
            //goto LAB_00124040;
            return -1;
        }

        *iVar6 = audioMgr->abort_req;
        if (audioMgr->abort_req) {
            //goto LAB_00123b80;
            return -2;
        }

        track = audioMgr->arr[uVar10];
        if ((track->size == 0) || (track->f_0xe != 0)) {
LAB_00123b70:
            uVar10 = uVar10 + 1;
        } else {
            clip = track->clip_0x9;
            frameTmp = track->frame_0xf;
            lVar23 = clip->decodeCtx->codecCtx;
            if (lVar23->channel_layout == 0) {
                lVar23->channel_layout = av_get_default_channel_layout(lVar23->channels);
            }
            *iVar6 = (int) lVar23->channel_layout;
            lVar22 = clip->pre_end_timeline_0x18;
            lVar26 = clip->start_timeline;
            if (lVar22 < lVar26) {
                lVar14 = av_rescale_q(0x400, lVar23->time_base, AV_TIME_BASE_Q);
                if (lVar22 + lVar14 < lVar26) {
                    uVar11 = 0x400;
                    lVar26 = lVar22 + lVar14;
                } else {
                    lVar14 = ((lVar26 - lVar22) * lVar23->sample_rate) / 1000000;
                    lVar22 = lVar14 + 0x100;
                    *uVar7 = (int) lVar14 + 0x1ff;
                    if (-1 < lVar22) {
                        *uVar7 = lVar22;
                    }
                    uVar11 = (*uVar7 & 0xffffff00);
                }
                clip->pre_end_timeline_0x18 = lVar26;
                frameTmp->nb_samples = uVar11;
                frameTmp->format = lVar23->sample_fmt;
                frameTmp->channels = lVar23->channels;
                frameTmp->channel_layout = lVar23->channel_layout;
                frameTmp->sample_rate = lVar23->sample_rate;
                frameTmp->pts = lVar26;
                bufferSize = av_samples_get_buffer_size(0, frameTmp->channels, frameTmp->nb_samples, frameTmp->format, 0);
                buf = av_malloc(bufferSize);
                if (buf == 0x0) {
                    *uVar7 = -1;
                    av_log(0,8,"av_malloc failed");
                } else {
                    memset(buf, 0, bufferSize);
                    *uVar7 = avcodec_fill_audio_frame(frameTmp, frameTmp->channels,
                                                      frameTmp->format, buf, bufferSize, 0);
                    if (-1 < *uVar7) {
                        //local_94 = 1;
LAB_00123d38:
                        track->f_0x10 = frameTmp->pts;
                        if (audioMgr->f_0x960 < track->f_0x10) {
                            audioMgr->f_0x960 = track->f_0x10;
                        }
                        *uVar7 = av_buffersrc_add_frame(track->filterCtxAbuf, frameTmp);
                        if (-1 < *uVar7) {
                            if (audioMgr->f_0x960 <= track->f_0x10) {
LAB_00123d7c:
                                *iVar6 = audioMgr->f_0x8a0;
                                goto LAB_00123b70;
                            }
                            *iVar6 = audioMgr->f_0x8a0;
                            goto LAB_00123b74;
                        }
                        av_log(0,0x10,"av_buffersrc_add_frame failed");
                        //goto LAB_00124040;
                        return -1;
                    }
                }
            } else {
                do {
                    ret = get_audio_frame_from_audio_clip(audioMgr, clip, frameTmp, iVar6, uVar7);
                    if (ret == -2) goto LAB_00123d38;
                    if (ret == -3) goto LAB_001243b0;
                    if ((*iVar6 != 0) || (clip->decodeCtx->f_0x34 != 0) || (audioMgr->f_0x8a0 != 0)) {
                        if (audioMgr->abort_req) {
                            *uVar7 = -1;
                            goto LAB_001243b0;
                        }
                        if (*uVar7 < 0) goto LAB_001243b0;
                        if (*iVar6 != 0) goto LAB_00123d38;
                        if (track->clip_0x9->decodeCtx->f_0x34 == 0) goto LAB_00123d7c;
                        ret = audio_track_swap_to_next_clip(track);
                        if (-1 < ret) {
                            if (track->f_0xe != 0) {
                                audioMgr->f_0x968--;
                                audio_check_1(ffp, audioMgr);
                            }
//                            goto LAB_00124040;
                            return -1;
                        }
//                        uVar17 = 0x10;
//                        if (!bVar4) {
//                            uVar17 = 8;
//                        }
//                        bVar4 = true;
                        *uVar7 = -1;
                        av_log(0, 0x10,"audio_track_swap_to_next_clip failed");
                        SDL_UnlockMutex(audioMgr->mutex);
                        *iVar6 = audioMgr->f_0x8a0;
                        goto LAB_00123b74;
                    }
                } while (!audioMgr->abort_req);
                *uVar7 = -1;
            }
LAB_001243b0:
//            uVar17 = 0x10;
//            if (!bVar4) {
//                uVar17 = 8;
//            }
//            bVar4 = true;
            av_log(0, 0x10,"get_audio_frame_from_queue audio track:%d decode frame failed", uVar10);
            SDL_UnlockMutex(audioMgr->mutex);
            *iVar6 = audioMgr->f_0x8a0;
        }
LAB_00123b74:
        if (uVar10 >= 4) {
            break;
        }
    } while (uVar10 < 4);

    return 0;
}

int audio_track_count_0x70_0(AudioTrackEditOp *audioMgr) {
    int count = 0;
    int count_0x70_0 = 0;
    AudioTrackInfo *track;

    if (audioMgr->f_0x20 < 1) {
        return 0;
    }

    for (int i = 0; i < MAX_AUDIO_TRACK; i++) {
        track = audioMgr->arr[i];
        if (track->size > 0) {
            count++;
            if (track->f_0xe == 0) {
                count_0x70_0++;
            }
            if (count >= audioMgr->f_0x20) {
                break;
            }
        }
    }

    MPTRACE("%s return %d", __func__, count_0x70_0);
    return count_0x70_0;
}

int reconfigure_audio_filter(AudioTrackEditOp *audioMgr)
{
    int uVar1;
    double fVar4;
    int uVar10;
    int uVar11;
    int iVar12;
    AudioTrackInfo *track;
    int64_t lVar16;
    int64_t lVar18;
    int uVar19;
    int uVar20;
    int64_t lVar21;
    int64_t lVar22;
    char acStack776 [256];
    char acStack520 [512];
    AVCodecContext *codecCtx;
    char filterName[20];
    const char *str;
    AVFilter *filter;
    AudioClip *clip;
    AVFilterInOut *avInout1, *avInout2;
    int value32;
    int64_t value64;
    int index;
    AVFilterContext *filterCtx1, *filterCtx2;
    int ret;

    if (0 < audioMgr->f_0x20) {
        uVar10 = audio_track_count_0x70_0(audioMgr);
        if (uVar10 > 0) {
            avfilter_graph_free(&audioMgr->filterGraph);
            audioMgr->filterGraph = avfilter_graph_alloc();
            fVar4 = 0.000001;
            if (audioMgr->filterGraph == 0) {
                av_log(0,0x10,"Unable to create filter graph.\n");
                return -1;
            }

            uVar19 = 0;
            iVar12 = 0;
            do {
                uVar20 = uVar19;
                if (audioMgr->f_0x20 <= iVar12) break;
                track = audioMgr->arr[uVar19];
                track->filterCtxAbuf = 0;
                track->filterCtxVol = 0;
                track->filterCtxFadeIn = 0;
                track->filterCtxFadeOut = 0;
                if ((track->size != 0) && (iVar12 = iVar12 + 1, track->f_0xe == 0)) {
                    codecCtx = track->clip_0x9->decodeCtx->codecCtx;
                    if (codecCtx->channel_layout == 0) {
LAB_00121040:
                        codecCtx->channel_layout = av_get_default_channel_layout(codecCtx->channels);
                        uVar11 = codecCtx->channels;
                    } else {
                        uVar11 = av_get_channel_layout_nb_channels(codecCtx->channel_layout);
                        if (uVar11 != codecCtx->channels) goto LAB_00121040;
                    }
                    track->num_chans = uVar11;
                    track->sample_rate = codecCtx->sample_rate;
                    track->sampleFormat = codecCtx->sample_fmt;
                    track->chan_layout = codecCtx->channel_layout;
                    snprintf(filterName, sizeof(filterName), "abuffer%d", uVar20);
                    uVar1 = codecCtx->sample_rate;
                    str = av_get_sample_fmt_name(codecCtx->sample_fmt);
                    snprintf(acStack776, 0x100,
                             "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d:channel_layout=0x%lx",
                             codecCtx->sample_rate, str, codecCtx->channels,1,1000000,
                             codecCtx->channel_layout);
                    filter = avfilter_get_by_name("abuffer");
                    ret = avfilter_graph_create_filter(&track->filterCtxAbuf, filter,
                                                          filterName, acStack776, 0, audioMgr->filterGraph);
                    if (ret < 0) goto LAB_00121538;
                    snprintf(acStack776, 0x100, "volume=%0.3f", track->clip_0x9->volume_0x44);
                    snprintf(filterName, sizeof (filterName), "volume%d", uVar20);

                    filter = avfilter_get_by_name("volume");
                    ret = avfilter_graph_create_filter(&track->filterCtxVol, filter,
                                                          filterName, acStack776, 0, audioMgr->filterGraph);
                    if (ret < 0) goto LAB_00121538;

                    lVar16 = ffp_get_duration_l(track->ffp);
                    clip = track->clip_0x9;
                    lVar18 = clip->fadeout_0x3c;
                    lVar21 = clip->fadein_0x38_7;
                    lVar22 = (clip->end_file - clip->begin_file) - lVar18;
                    if (lVar16 < clip->start_timeline + clip->duration) {
                        lVar16 = lVar16 - clip->start_timeline;
                        if ((lVar16 < 1000000) && (0 < clip->fadein_0x38_7)) {
                            lVar21 = lVar16;
                        }
                        if (0 < clip->fadeout_0x3c) {
                            if (clip->fadein_0x38_7 < 1) {
                                if (1000000 < lVar16) goto LAB_00121634;
                                lVar22 = 0;
                                lVar18 = lVar16;
                            } else {
                                if (lVar16 < 0x1e8481) {
                                    if (lVar16 < 0xf4241) {
                                        lVar22 = 0;
                                        lVar18 = 0;
                                    } else {
                                        lVar22 = 1000000;
                                        lVar18 = lVar16 - 1000000;
                                    }
                                } else {
LAB_00121634:
                                    lVar22 = lVar16 - 1000000;
                                    lVar18 = 1000000;
                                }
                            }
                        }
                    }

                    if (0 < lVar21) {
                        snprintf(filterName, 0x10, "afadein%d", uVar20);
                        lVar16 = clip->start_timeline;
                        snprintf(acStack776,0x100,"type=in:st=%f:d=%f",lVar16 * fVar4, lVar21 * fVar4);
                        filter = avfilter_get_by_name("afade");
                        ret = avfilter_graph_create_filter(&track->filterCtxFadeIn, filter,
                                                           filterName, acStack776, 0, audioMgr->filterGraph);
                        if (ret < 0) goto LAB_00121538;
                    }

                    if (0 < lVar18) {
                        snprintf(filterName, 0x10, "afadeout%d", uVar20);
                        lVar21 = clip->start_timeline;
                        snprintf(acStack776,0x100,"type=out:st=%f:d=%f",(lVar22 + lVar21) * fVar4, lVar18 * fVar4);
                        filter = avfilter_get_by_name("afade");
                        ret = avfilter_graph_create_filter(&track->filterCtxFadeOut, filter,
                                                           filterName, acStack776, 0, audioMgr->filterGraph);
                        if (ret < 0) goto LAB_00121538;
                    }

                    if (clip != 0 && track->filterCtxVol != 0 && audioMgr->filterGraph != 0 && track->filterCtxAbuf !=0) {
                        avInout1 = avfilter_inout_alloc();
                        avInout2 = avfilter_inout_alloc();
                        memset(acStack520, 0, 0x200);
                        str = av_get_sample_fmt_name(audioMgr->hwAudioParams.fmt);
                        snprintf(acStack520, 0x200, "aresample=%d,aformat=sample_fmts=%s:channel_layouts=%d",
                                 audioMgr->hwAudioParams.freq, str, audioMgr->hwAudioParams.channels);
                        if ((avInout2 != 0) && (avInout1 != 0)) {
                            avInout1->name = av_strdup("in");
                            avInout1->filter_ctx = track->filterCtxAbuf;
                            avInout1->pad_idx = 0;
                            avInout1->next = 0;

                            avInout2->name = av_strdup("out");
                            avInout2->filter_ctx = track->filterCtxVol;
                            avInout2->pad_idx = 0;
                            avInout2->next = 0;
                            avfilter_graph_parse_ptr(audioMgr->filterGraph, acStack520, &avInout2, &avInout1, 0);
                        }
                        avfilter_inout_free(&avInout2);
                        avfilter_inout_free(&avInout1);
                    }
                }
                uVar19 = uVar19 + 1;
            } while (uVar19 != 4);

            snprintf(acStack776, 0x100, "inputs=%d:dropout_transition=0", uVar10);
            filter = avfilter_get_by_name("amix");
            ret = avfilter_graph_create_filter(&audioMgr->filterCtxAmix, filter,
                                                  "amix", acStack776, 0, audioMgr->filterGraph);
            if (ret < 0) goto LAB_00121538;

            snprintf(acStack776, 0x100,"volume=%d", uVar10);
            filter = avfilter_get_by_name("volume");
            ret = avfilter_graph_create_filter(&audioMgr->filterCtxVol, filter,
                                               "volume_f", acStack776, 0, audioMgr->filterGraph);
            if (ret < 0) goto LAB_00121538;

            filter = avfilter_get_by_name("abuffersink");
            ret = avfilter_graph_create_filter(&audioMgr->filterCtxAbuf, filter,
                                               "audiotrack_abuffer",0,0,audioMgr->filterGraph);
            if (ret < 0) goto LAB_00121538;

            ret = av_opt_set_int(audioMgr->filterCtxAbuf, "all_channel_counts",0,1);
            if (ret < 0) goto LAB_00121538;

            value32 = audioMgr->hwAudioParams.fmt;
            ret = av_opt_set_bin(audioMgr->filterCtxAbuf, "sample_fmts", (uint8_t *) &value32,
                                 sizeof (value32), 1);
            if (ret < 0) goto LAB_00121538;

            value64 = audioMgr->hwAudioParams.channel_layout;
            ret = av_opt_set_bin(audioMgr->filterCtxAbuf, "channel_layouts", (uint8_t *) &value64,
                                 sizeof (value64), 1);
            if (ret < 0) goto LAB_00121538;

            value32 = audioMgr->hwAudioParams.channels;
            ret = av_opt_set_bin(audioMgr->filterCtxAbuf, "channel_counts", (uint8_t *) &value32,
                                 sizeof (value32), 1);
            if (ret < 0) goto LAB_00121538;

            value32 = audioMgr->hwAudioParams.freq;
            ret = av_opt_set_bin(audioMgr->filterCtxAbuf,"sample_rates", (uint8_t *) &value32,
                                 sizeof (value32),1);
            if (ret < 0) goto LAB_00121538;

            index = 0;
            uVar19 = 0;
            do {
                track = audioMgr->arr[index];
                if ((track->size != 0) && (track->f_0xe == 0)) {
                    filterCtx2 = track->filterCtxFadeIn;
                    filterCtx1 = track->filterCtxVol;
                    if ((filterCtx2 != 0) && (filterCtx2->name != 0)) {
                        ret = avfilter_link(filterCtx1, 0, filterCtx2, 0);
                        if (ret < 0) goto LAB_00121538;
                        filterCtx1 = track->filterCtxFadeIn;
                    }

                    filterCtx2 = track->filterCtxFadeOut;
                    if ((filterCtx2 != 0) && (filterCtx2->name != 0)) {
                        ret = avfilter_link(filterCtx1, 0, filterCtx2, 0);
                        if (ret < 0) goto LAB_00121538;
                        filterCtx1 = track->filterCtxFadeOut;
                    }

                    ret = avfilter_link(filterCtx1, 0, audioMgr->filterCtxAmix, uVar19);
                    if (ret < 0) goto LAB_00121538;
                    uVar19++;
                }
                index++;
            } while (index < 4);

            ret = avfilter_link(audioMgr->filterCtxAmix, 0, audioMgr->filterCtxVol, 0);
            if (ret < 0) goto LAB_00121538;

            ret = avfilter_link(audioMgr->filterCtxVol, 0, audioMgr->filterCtxAbuf, 0);
            if (ret < 0) goto LAB_00121538;

            ret = avfilter_graph_config(audioMgr->filterGraph, 0);
            if (-1 < ret) {
                return ret;
            }
LAB_00121538:
            avfilter_graph_free(&audioMgr->filterGraph);
            return ret;
        }
    }
    return 0;
}

int audio_track_decode_thread(void *param, void *param2)
{
    MPTRACE("%s begin", __func__);
    AudioTrackEditOp *audioState;
    FFPlayer *ffp;
    AVFrame *frame;
    int ret;
    int specSize;
    char bVar1, bVar5;
    int uVar10 = 0x10;
    int local_bc;
    int iVar6;
    int uVar7;
    Frame *puVar24;
    double dVar28;
    char *errDes;

    if (param == NULL) {
        return -1;
    }

    ffp = (FFPlayer *) param;
    audioState = ffp->audioState;
    frame = av_frame_alloc();
    if (frame == NULL) {
        av_log(0, AV_LOG_FATAL, "malloc out frame error!");
        return -1;
    }

    packet_queue_start(&audioState->pktQueue);
    if (audioState->f_0x86c == 0) {
        ret = sdl_audio_open(ffp, 3, 2, 44100, &audioState->hwAudioParams);
        if (ret >= 0) {
            local_bc = audioState->pktQueue.serial;
            audioState->f_0x86c = 1;
            if (audioState->f_0x8c8 == 0) {
                audioState->c.paused = 0;
                audioState->f_0x8c8 = 0;
                SDL_AoutPauseAudio(audioState->aOut_0x8c0, 0);
            }
            specSize = ret;
            goto LAB_00123574;
        }
        av_log(0,0x10,"sdl_audio_open failed");
        if (audioState->abort_req == 0) {
LAB_001237e4:
            av_log(0,8,"%s exit unexpected!","audio_track_decode_thread");
            if (-1 < uVar7) goto LAB_001235e8;
        }
LAB_00123808:
        SDL_UnlockMutex(audioState->mutex);
    } else {
        specSize = -1;
        local_bc = -1;
LAB_00123574:
//        bVar4 = 0;
        bVar1 = 0;
        bVar5 = 0;
LAB_00123598:
        if (!audioState->abort_req) {
            if (specSize < 0) {
                usleep(200000);
            }
            SDL_LockMutex(audioState->mutex);
            while (audioState->f_0x870 != 0 || audioState->f_0x20 == 0) {
                SDL_CondWaitTimeout(audioState->cond, audioState->mutex, 100);
                if (audioState->abort_req) {
                    SDL_UnlockMutex(audioState->mutex);
                    //giai phong tai nguyen
                    avfilter_graph_free(&audioState->filterGraph);
                    av_frame_free(&frame);
                    return 0;
                }
            }

            ret = audio_track_open_decode_clips(audioState);
            if (-1 < ret) {
                //SDL_UnlockMutex(audioState->mutex);
                SDL_LockMutex(audioState->seek_mutex);
                if (audioState->f_0x8a0 != 0) goto code_r0x00123834;
                SDL_UnlockMutex(audioState->seek_mutex);
                if (bVar5) {
                    uVar7 = 0;
                    goto LAB_001238d8;
                }
                ret = audio_track_open_decode_clips(audioState);
                if (-1 < ret) goto LAB_001238d8;
//                if (!bVar4) {
//                    uVar10 = 8;
//                }
                errDes = "audio_track_open_decode_clips failed";
                goto LAB_00123ec4;
            }
//            if (!bVar4) {
//                uVar10 = 8;
//            }
            uVar7 = 1;
//            bVar4 = 1;
            av_log(0, uVar10,"audio_track_open_decode_clips failed");
            SDL_UnlockMutex(audioState->mutex);
            goto LAB_00123598;
        }

LAB_001235e4:
        if (uVar7 < 0) goto LAB_00123808;
    }

LAB_001235e8:
    avfilter_graph_free(&audioState->filterGraph);
    av_frame_free(&frame);
//    uVar11 = (ulonglong)uVar7;
//    return uVar11;
    return 0;

code_r0x00123834:
    packet_queue_flush(&audioState->pktQueue);
    packet_queue_put(&audioState->pktQueue, &flush_pkt);

    ret = audio_thread_p1(audioState);
    MPTRACE("audio_thread_p1 return %d", ret);
    if (ret < 0) {
        SDL_UnlockMutex(audioState->seek_mutex);
        SDL_UnlockMutex(audioState->mutex);
        goto LAB_00123598;
    }
    bVar5 = true;
    SDL_UnlockMutex(audioState->seek_mutex);

LAB_001238d8:
    ret = check_audio_format_change(audioState, bVar1, local_bc);
    MPTRACE("audio_thread_p2(%d, %d) return %d", bVar1, local_bc, ret);
    if (!ret) {
        goto LAB_00123b34;
    }

    uVar7 = reconfigure_audio_filter(audioState);
    MPTRACE("reconfigure_audio_filter return %d", uVar7);
    local_bc = audioState->pktQueue.serial;
    if (uVar7 < 0) {
//        uVar10 = 0x10;
//        if (!bVar4) {
//            uVar10 = 8;
//        }
        errDes = "reconfigure_audio_filter failed";
LAB_00123ec4:
//        bVar4 = true;
        av_log(0,0x10, "%s", errDes);
        SDL_UnlockMutex(audioState->mutex);
        goto LAB_00123598;
    }
    audioState->af_change = 0;

LAB_00123b34:
    ret = audio_thread_p3(ffp, audioState, &iVar6, &uVar7);
    SDL_UnlockMutex(audioState->mutex);
    MPTRACE("audio_thread_p3(%d, %d) return %d", iVar6, uVar7, ret);
    if (ret == -1) goto LAB_00124040;

    if ((iVar6 == 0 || ret == -2)) {
        if (audioState->filterCtxAbuf != 0) {
            if (audioState->abort_req) {
                SDL_UnlockMutex(audioState->mutex);
                if (audioState->abort_req == 0) goto LAB_001237e4;
                goto LAB_001235e4;
            }
LAB_00123bec:
            ret = av_buffersink_get_frame_flags(audioState->filterCtxAbuf, frame, 0);
            //MPTRACE("av_buffersink_get_frame_flags: %d, %d", ret, audioState->f_0x8a0);
            if ((ret < 0) || (audioState->f_0x8a0 != 0)) goto LAB_00124528;
            iVar6 = audioState->hwAudioParams.freq;
            puVar24 = frame_queue_peek_writable(&audioState->frameQueue);
            //MPTRACE("audio_thread 01 %p, %d", puVar24, audioState->f_0x530);
            if (puVar24 == 0x0) {
                if (audioState->f_0x530 != 0) {
//                    bVar4 = false;

                    SDL_CondWait(audioState->cond, audioState->mutex);
LAB_00124528:
                    if (audioState->f_processing_0x5c8) {
                        SDL_CondWait(audioState->cond, audioState->mutex);
                    }
                    bVar1 = 0;
                    SDL_UnlockMutex(audioState->mutex);
                    goto LAB_00123598;
                }
                SDL_UnlockMutex(audioState->mutex);
            } else {
                dVar28 = NAN;
                if (frame->pts != AV_NOPTS_VALUE) {
                    //dVar28 = (double) frame->pts / (double) iVar6;
                    dVar28 = (double) frame->pts * av_q2d(av_buffersink_get_time_base(audioState->filterCtxAbuf));
                }
                puVar24->serial = audioState->pktQueue.serial;
                puVar24->pts = dVar28;
                puVar24->pos = frame->pkt_pos;
                puVar24->duration = (double) frame->nb_samples / (double) frame->sample_rate;
                av_frame_move_ref(puVar24->frame, frame);
                frame_queue_push(&audioState->frameQueue);
            }
//            bVar4 = false;
            goto LAB_00123bec;
        }
    }
LAB_00124040:
    bVar1 = 1;
    SDL_UnlockMutex(audioState->mutex);
    goto LAB_00123598;
}

static int decoder_start(Decoder *d, int (*fn)(void *, void *), void *arg, void *arg2, const char *name)
{
    MPTRACE("%s begin", __func__);
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThreadEx(&d->_decoder_tid, fn, arg, arg2, name);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int ffplay_video_thread(void *arg, void *arg2, char flag)
{
    MPTRACE("%s begin", __func__);
    FFPlayer *ffp = arg;
    VideoClip *player = arg2;
    VideoState *is = player->is_0x30;
    MPTRACE("%s begin video111 read file name %s", __func__,is->filename);
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
    int64_t dst_pts = -1;
    int64_t last_dst_pts = -1;
    int retry_convert_image = 0;
    int convert_frame_count = 0;

#if CONFIG_AVFILTER
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
    if (!graph) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

#else
    ffp_notify_msg2(ffp, FFP_MSG_VIDEO_ROTATION_CHANGED, ffp_get_video_rotate_degrees(ffp, player));
#endif

    if (!frame) {
#if CONFIG_AVFILTER
        avfilter_graph_free(&graph);
#endif
        return AVERROR(ENOMEM);
    }

    if (flag) {
        ffp_notify_msg3(ffp, FFP_REQ_SEEK, -1, 1);
    }

    for (;;) {
        MPTRACE("%s read frame video111 read file name %s", __func__,is->filename);
        ret = get_video_frame(ffp, player, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        if (ffp->get_frame_mode) {
            if (!ffp->get_img_info || ffp->get_img_info->count <= 0) {
                av_frame_unref(frame);
                continue;
            }

            last_dst_pts = dst_pts;

            if (dst_pts < 0) {
                dst_pts = ffp->get_img_info->start_time;
            } else {
                dst_pts += (ffp->get_img_info->end_time - ffp->get_img_info->start_time) / (ffp->get_img_info->num - 1);
            }

            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb); //convert avrelation to double
            pts = pts * 1000;
            if (pts >= dst_pts) {
                while (retry_convert_image <= MAX_RETRY_CONVERT_IMAGE) {
                    ret = convert_image(ffp, player, frame, (int64_t)pts, frame->width, frame->height);
                    if (!ret) {
                        convert_frame_count++;
                        break;
                    }
                    retry_convert_image++;
                    av_log(NULL, AV_LOG_ERROR, "convert image error retry_convert_image = %d\n", retry_convert_image);
                }

                retry_convert_image = 0;
                if (ret || ffp->get_img_info->count <= 0) {
                    if (ret) {
                        av_log(NULL, AV_LOG_ERROR, "convert image abort ret = %d\n", ret);
                        ffp_notify_msg3(ffp, FFP_MSG_GET_IMG_STATE, 0, ret);
                    } else {
                        av_log(NULL, AV_LOG_INFO, "convert image complete convert_frame_count = %d\n", convert_frame_count);
                    }
                    goto the_end;
                }
            } else {
                dst_pts = last_dst_pts;
            }
            av_frame_unref(frame);
            continue;
        }

#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || ffp->vf_changed
            || last_vfilter_idx != is->vfilter_idx) {
            SDL_LockMutex(ffp->vf_mutex);
            ffp->vf_changed = 0;
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = configure_video_filters(ffp, graph, is, ffp->vfilters_list ? ffp->vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                // FIXME: post error
                SDL_UnlockMutex(ffp->vf_mutex);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
            SDL_UnlockMutex(ffp->vf_mutex);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            //frame->pts = frame->pts* 1/player->speed_0xa0;
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture2(ffp, player, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
        }
#endif

        if (ret < 0)
            goto the_end;
    }
 the_end:
    MPTRACE("%s read done frame video111 file name %s", __func__,is->filename);
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_log(NULL, AV_LOG_INFO, "convert image convert_frame_count = %d\n", convert_frame_count);
    av_frame_free(&frame);
    return 0;
}

static int video_thread(void *arg, void *arg2)
{
    MPTRACE("%s begin", __func__);
    FFPlayer *ffp = (FFPlayer *)arg;
    VideoClip *player = arg2;
    int       ret = 0;

    if (player->node_vdec_0x18) {
        ret = ffpipenode_run_sync(player->node_vdec_0x18);
    }
    return ret;
}

static int subtitle_thread(void *arg, void *arg2)
{
    MPTRACE("%s begin", __func__);
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(ffp, NULL, &is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;
#ifdef FFP_MERGE
        if (got_subtitle && sp->sub.format == 0) {
#else
        if (got_subtitle) {
#endif
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
#ifdef FFP_MERGE
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
#endif
        }
    }
    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    MPTRACE("%s begin", __func__);
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples)
{
    MPTRACE("%s begin", __func__);
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(FFPlayer *ffp, VideoState *is)
{
    //MPTRACE("%s begin", __func__);
//    VideoState *is = player->is_0x30;
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;
#if defined(__ANDROID__)
    int translate_time = 1;
#endif

    if (is->paused || is->step)
        return -1;

    if (ffp->sync_av_start &&                       /* sync enabled */
        is->video_st &&                             /* has video stream */
        !is->viddec.first_frame_decoded &&          /* not hot */
        is->viddec.finished != is->videoq.serial) { /* not finished */
        /* waiting for first video frame */
        Uint64 now = SDL_GetTickHR();
        if (now < is->viddec.first_frame_decoded_time ||
            now > is->viddec.first_frame_decoded_time + 2000) {
            is->viddec.first_frame_decoded = 1;
        } else {
            /* video pipeline is not ready yet */
            return -1;
        }
    }
reload:
    do {
#if defined(_WIN32) || defined(__APPLE__)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - ffp->audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, af->frame->channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    dec_channel_layout =
        (af->frame->channel_layout && af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
        af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt            ||
        dec_channel_layout       != is->audio_src.channel_layout ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        AVDictionary *swr_opts = NULL;
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                         dec_channel_layout,           af->frame->format, af->frame->sample_rate,
                                         0, NULL);
        if (!is->swr_ctx) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            return -1;
        }
        av_dict_copy(&swr_opts, ffp->swr_opts, 0);
        if (af->frame->channel_layout == AV_CH_LAYOUT_5POINT1_BACK)
            av_opt_set_double(is->swr_ctx, "center_mix_level", ffp->preset_5_1_center_mix_level, 0);
        av_opt_set_dict(is->swr_ctx, &swr_opts);
        av_dict_free(&swr_opts);

        if (swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels       = af->frame->channels;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int)((int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256);
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);

        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        int bytes_per_sample = av_get_bytes_per_sample(is->audio_tgt.fmt);
        resampled_data_size = len2 * is->audio_tgt.channels * bytes_per_sample;
#if defined(__ANDROID__)
        if (ffp->soundtouch_enable && ffp->pf_playback_rate != 1.0f && !is->abort_request) {
            av_fast_malloc(&is->audio_new_buf, &is->audio_new_buf_size, out_size * translate_time);
            for (int i = 0; i < (resampled_data_size / 2); i++)
            {
                is->audio_new_buf[i] = (is->audio_buf1[i * 2] | (is->audio_buf1[i * 2 + 1] << 8));
            }

            int ret_len = ijk_soundtouch_translate(is->handle, is->audio_new_buf, (float)(ffp->pf_playback_rate), (float)(1.0f/ffp->pf_playback_rate),
                    resampled_data_size / 2, bytes_per_sample, is->audio_tgt.channels, af->frame->sample_rate);
            if (ret_len > 0) {
                is->audio_buf = (uint8_t*)is->audio_new_buf;
                resampled_data_size = ret_len;
            } else {
                translate_time++;
                goto reload;
            }
        }
#endif
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef FFP_SHOW_AUDIO_DELAY
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    if (!is->auddec.first_frame_decoded) {
        ALOGD("avcodec/Audio: first frame decoded\n");
        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_DECODED_START);
        is->auddec.first_frame_decoded_time = SDL_GetTickHR();
        is->auddec.first_frame_decoded = 1;
    }
    return resampled_data_size;
}

/* prepare a new audio buffer */
//static void sdl_audio_callback(void *opaque, void *data2, Uint8 *stream, int len)
//{
//    MPTRACE("%s begin", __func__);
//    FFPlayer *ffp = opaque;
//    AudioTrackEditOp *audioMgr = ffp->audioState;
//    ClipEditOp *videoMgr;
//    VideoClip *player = 0;
//    VideoState *is;
//    int audio_size, len1;
//    Uint64 tickHR;

//    if (!audioMgr) {
//        memset(stream, 0, len);
//        return;
//    }

//    if (!ffp->audio_only) {
//        videoMgr = ffp->clipState;
//        SDL_LockMutex(videoMgr->mutext_8);
//        ffp_clip_op_get_play_ci(ffp, &player);
//        if ((player == 0) || (is = player->is_0x30, is == 0) ||
//                (is->step != 0)) {
//            memset(stream, 0, len);
//            SDL_UnlockMutex(videoMgr->mutext_8);
//            return;
//        }

//        if ((ffp->sync_av_start != 0) &&
//                (is->video_st != 0) &&
//                  (is->viddec.first_frame_decoded == 0) &&
//                    (is->viddec.finished != is->videoq.serial)) {
//            tickHR = SDL_GetTickHR();
//            if ((is->viddec.first_frame_decoded_time <= tickHR) &&
//                    (tickHR <= is->viddec.first_frame_decoded_time + 2000)) {
//                SDL_UnlockMutex(videoMgr->mutext_8);
//                return;
//            }
//            is->viddec.first_frame_decoded = 1;
//        }
//        SDL_UnlockMutex(videoMgr->mutext_8);
//    }


//    ffp->audio_callback_time = av_gettime_relative();

//    if (ffp->pf_playback_rate_changed) {
//        ffp->pf_playback_rate_changed = 0;
//#if defined(__ANDROID__)
//        if (!ffp->soundtouch_enable) {
//            SDL_AoutSetPlaybackRate(audioMgr->aOut_0x8c0, ffp->pf_playback_rate);
//        }
//#else
//        SDL_AoutSetPlaybackRate(audioMgr->aOut_0x8c0, ffp->pf_playback_rate);
//#endif
//    }
//    if (ffp->pf_playback_volume_changed) {
//        ffp->pf_playback_volume_changed = 0;
//        SDL_AoutSetPlaybackVolume(audioMgr->aOut_0x8c0, ffp->pf_playback_volume);
//    }

//    while (len > 0) {
//        if (is->audio_buf_index >= is->audio_buf_size) {
//           audio_size = audio_decode_frame(ffp, player);
//           if (audio_size < 0) {
//                /* if error, just output silence */
//               is->audio_buf = NULL;
//               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
//           } else {
//               if (is->show_mode != SHOW_MODE_VIDEO)
//                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
//               is->audio_buf_size = audio_size;
//           }
//           is->audio_buf_index = 0;
//        }
//        if (is->auddec.pkt_serial != is->audioq.serial) {
//            is->audio_buf_index = is->audio_buf_size;
//            memset(stream, 0, len);
//            // stream += len;
//            // len = 0;
//            SDL_AoutFlushAudio(audioMgr->aOut_0x8c0);
//            break;
//        }
//        len1 = is->audio_buf_size - is->audio_buf_index;
//        if (len1 > len)
//            len1 = len;
//        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
//            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
//        else {
//            memset(stream, 0, len1);
//            if (!is->muted && is->audio_buf)
//                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
//        }
//        len -= len1;
//        stream += len1;
//        is->audio_buf_index += len1;
//    }
//    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
//    /* Let's assume the audio driver that is used by SDL has two periods. */
//    if (!isnan(is->audio_clock)) {
//        set_clock_at(&is->audclk, is->audio_clock - (double)(is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec - SDL_AoutGetLatencySeconds(ffp->aout), is->audio_clock_serial, ffp->audio_callback_time / 1000000.0);
//        sync_clock_to_slave(&is->extclk, &is->audclk);
//    }
//    if (!ffp->first_audio_frame_rendered) {
//        ffp->first_audio_frame_rendered = 1;
//        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_RENDERING_START);
//    }

//    if (is->latest_audio_seek_load_serial == is->audio_clock_serial) {
//        int latest_audio_seek_load_serial = __atomic_exchange_n(&(is->latest_audio_seek_load_serial), -1, memory_order_seq_cst);
//        if (latest_audio_seek_load_serial == is->audio_clock_serial) {
//            if (ffp->av_sync_type == AV_SYNC_AUDIO_MASTER) {
//                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 1);
//            } else {
//                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 0);
//            }
//        }
//    }

//    if (ffp->render_wait_start && !ffp->start_on_prepared && is->pause_req) {
//        while (is->pause_req && !is->abort_request) {
//            SDL_Delay(20);
//        }
//    }
//}

static void sdl_audio_callback(void *opaque, void *data2, Uint8 *stream, int len)
{
    //MPTRACE("%s begin", __func__);
    FFPlayer *ffp = opaque;
    AudioTrackEditOp *audioMgr = ffp->audioState;
    ClipEditOp *videoMgr;
    VideoClip *player = 0;
    VideoState *is;
    Uint64 tickHR;

    int uVar14;
    int uVar15;
    int uVar5;
    int uVar3;
    int bVar4;
    int uVar10;
    int iVar6, iVar7;
    int iVar1;
    Frame *frame;
    AVFrame *avFrame;
    double dVar17;
    int __n;

    if (!audioMgr) {
        memset(stream, 0, len);
        return;
    }

    if (!ffp->audio_only) {
        videoMgr = ffp->clipState;
        SDL_LockMutex(videoMgr->mutext_8);
        ffp_clip_op_get_play_ci(ffp, &player);
        if ((player == 0) || (is = player->is_0x30, is == 0) ||
                (is->step != 0)) {
            memset(stream, 0, len);
            SDL_UnlockMutex(videoMgr->mutext_8);
            return;
        }

        if ((ffp->sync_av_start != 0) &&
                (is->video_st != 0) &&
                  (is->viddec.first_frame_decoded == 0) &&
                    (is->viddec.finished != is->videoq.serial)) {
            tickHR = SDL_GetTickHR();
            if ((is->viddec.first_frame_decoded_time <= tickHR) &&
                    (tickHR <= is->viddec.first_frame_decoded_time + 2000)) {
                SDL_UnlockMutex(videoMgr->mutext_8);
                return;
            }
            is->viddec.first_frame_decoded = 1;
        }
        SDL_UnlockMutex(videoMgr->mutext_8);
    }


    ffp->audio_callback_time = av_gettime_relative();

    if (len < 1) {
        uVar14 = audioMgr->audio_buf_size;
        uVar15 = audioMgr->audio_buf_index;
    } else {
        uVar15 = audioMgr->audio_buf_index;
        uVar14 = 0;
LAB_00121a30:
        do {
            uVar5 = audioMgr->audio_buf_size;
            if (audioMgr->audio_buf_size <= audioMgr->audio_buf_index) {
                if (audioMgr->f_0x8c8 == 0) {
                    uVar14 = 0;
                    bVar4 = 0;
                    do {
                        while( true ) {
                            frame = frame_queue_peek_readable(&audioMgr->frameQueue);
                            if (frame == 0x0) goto LAB_00121ebc;
                            if (frame->serial != audioMgr->pktQueue.serial) {
                                uVar14 = 1;
                            }
                            if (!bVar4) break;
                            bVar4 = 1;
                            frame_queue_next(&audioMgr->frameQueue);
                            if (frame->serial == audioMgr->pktQueue.serial) goto LAB_00121acc;
                        }
                        //MPTRACE("%s check finish: %d, %d, %d, %d", __func__, ffp->audio_only, frame->serial, audioMgr->pktQueue.serial, audioMgr->f_0x870);
                        if (ffp->audio_only &&
                             (frame->serial != audioMgr->pktQueue.serial) &&
                                (audioMgr->f_0x870 != 0)) {

                            msg_queue_put_simple1(&ffp->msg_queue, FFP_MSG_COMPLETED);
                            bVar4 = 1;
                        }
                        frame_queue_next(&audioMgr->frameQueue);
                    } while (frame->serial != audioMgr->pktQueue.serial);
LAB_00121acc:
                    avFrame = frame->frame;
                    uVar10 = av_frame_get_channels(frame->frame);
                    uVar5 = av_samples_get_buffer_size(0, uVar10, avFrame->nb_samples, avFrame->format, 1);
                    if (avFrame->channel_layout == 0) {
                        avFrame->channel_layout = av_get_default_channel_layout(uVar10);
                    } else {
                        iVar7 = av_get_channel_layout_nb_channels(avFrame->channel_layout);
                        if (uVar10 != iVar7) {
                            avFrame->channel_layout = av_get_default_channel_layout(uVar10);
                        }
                    }
                    iVar6 = avFrame->nb_samples;
                    if (audioMgr->swrCtx != 0) {
                        iVar7 = 0;
                        if (avFrame->sample_rate != 0) {
                            iVar7 = (iVar6 * audioMgr->hwAudioParams.freq) /
                                          avFrame->sample_rate;
                        }
                        uVar15 = iVar7 + 0x100;
                        iVar7 = av_samples_get_buffer_size(0, audioMgr->hwAudioParams.channels,
                                                           uVar15, audioMgr->hwAudioParams.fmt, 0);
                        if (iVar7 < 0) {
                            av_log(0,0x10,"av_samples_get_buffer_size() failed\n");
                        } else {
                            iVar1 = avFrame->nb_samples;
                            if (iVar6 != iVar1) {
                                uVar5 = 0;
                                if (avFrame->sample_rate != 0) {
                                    uVar5 = ((iVar6 - iVar1) * audioMgr->hwAudioParams.freq) / avFrame->sample_rate;
                                }

                                uVar3 = 0;
                                if (avFrame->sample_rate != 0) {
                                    uVar3 = (iVar6 * audioMgr->hwAudioParams.freq) / avFrame->sample_rate;
                                }

                                iVar6 = swr_set_compensation(audioMgr->swrCtx, uVar5, uVar3);
                                if (iVar6 < 0) {
                                    av_log(0,0x10,"swr_set_compensation() failed\n");
                                    goto LAB_00121ebc;
                                }
                            }
                            av_fast_malloc(&audioMgr->audio_buf1, &audioMgr->audio_buf1_size, iVar7);
                            if (audioMgr->audio_buf1 != 0) {
                                uVar5 = swr_convert(audioMgr->swrCtx, &audioMgr->audio_buf1, uVar15,
                                                    avFrame->extended_data, avFrame->nb_samples);
                                if (-1 < uVar5) {
                                    if (uVar15 == uVar5) {
                                        av_log(0,0x18,"audio buffer is probably too small\n");
                                        iVar6 = swr_init(audioMgr->swrCtx);
                                        if (iVar6 < 0) {
                                            swr_free(&audioMgr->swrCtx);
                                        }
                                    }
                                    iVar6 = audioMgr->hwAudioParams.channels;
                                    audioMgr->audio_buf = audioMgr->audio_buf1;
                                    iVar7 = av_get_bytes_per_sample(audioMgr->hwAudioParams.fmt);
                                    uVar5 = uVar5 * iVar6 * iVar7;
                                    goto LAB_00121c18;
                                }
                                av_log(0,0x10,"swr_convert() failed\n");
                            }
                        }
                        goto LAB_00121ebc;
                    }
                    audioMgr->audio_buf = avFrame->data[0];
LAB_00121c18:
                    iVar6 = avFrame->nb_samples;
                    iVar7 = avFrame->sample_rate;
                    dVar17 = frame->pts;
                    audioMgr->audio_clock_serial = frame->serial;
                    if (!isnan(frame->pts)) {
                        audioMgr->nextPts = (double)avFrame->nb_samples / (double)avFrame->sample_rate + frame->pts;
                    } else {
                        audioMgr->nextPts = NAN;
                    }

                    if (audioMgr->f_0x940 == 0) {
                        audioMgr->tickHr = SDL_GetTickHR();
                        audioMgr->f_0x940 = 1;
                    }

                    if (!ffp->first_audio_frame_rendered) {
                        ffp->first_audio_frame_rendered = 1;
                        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_RENDERING_START);
                    }

                    if (uVar5 < 0) goto LAB_00121ebc;
                } else {
                    uVar14 = 0;
LAB_00121ebc:
                    iVar6 = audioMgr->hwAudioParams.frame_size;
                    audioMgr->audio_buf = audioMgr->f_0x644;
                    iVar7 = 0;
                    if (iVar6 != 0) {
                        iVar7 = (0x200 / iVar6);
                    }
                    uVar5 = iVar7 * iVar6;
                }
                audioMgr->audio_buf_size = uVar5;
                uVar15 = 0;
                audioMgr->audio_buf_index = 0;
            }
            __n = uVar5 - uVar15;
            if (len < __n) {
                __n = len;
            }
            if (audioMgr->mute_0x868 || uVar14 || audioMgr->f_0x8a0) {
                memset(stream, audioMgr->f_0x644[0], __n);
                SDL_AoutFlushAudio(audioMgr->aOut_0x8c0);
                if (audioMgr->mute_0x868 == 0) {
                    uVar15 = audioMgr->audio_buf_size;
                    audioMgr->audio_buf_index = audioMgr->audio_buf_size;
                    uVar14 = uVar15;
                    goto LAB_00121e60;
                }
                len = len - __n;
                stream = stream + __n;
                uVar15 = __n + audioMgr->audio_buf_index;
                audioMgr->audio_buf_index += __n;
                if (len < 1) break;
                goto LAB_00121a30;
            }
            len = len - __n;
            memcpy(stream, audioMgr->audio_buf + uVar15, __n);
            stream = stream + __n;
            uVar15 = __n + audioMgr->audio_buf_index;
            audioMgr->audio_buf_index += __n;
        } while (0 < len);
        uVar14 = audioMgr->audio_buf_size;
    }

LAB_00121e60:
    audioMgr->audio_write_buf_size = uVar14 - uVar15;
    if (!isnan(audioMgr->nextPts)) {
        set_clock_at(&audioMgr->c,
                     (audioMgr->nextPts - audioMgr->audio_write_buf_size / (double) audioMgr->hwAudioParams.bytes_per_sec)
                     - SDL_AoutGetLatencySeconds(audioMgr->aOut_0x8c0),
                     audioMgr->audio_clock_serial, ffp->audio_callback_time * 0.000001);
    }
}

static void audio_callback(void *opaque, void *data2, Uint8 *stream, int len)
{
    //MPTRACE("%s begin", __func__);
    FFPlayer *ffp = opaque;
    VideoClip *player = data2;
    VideoState *is = player->is_0x30;
    int audio_size, len1;

    if (!is) {
        memset(stream, 0, len);
        return;
    }


    ffp->audio_callback_time = av_gettime_relative();

    if (ffp->pf_playback_rate_changed) {
        ffp->pf_playback_rate_changed = 0;
#if defined(__ANDROID__)
        if (!ffp->soundtouch_enable) {
            SDL_AoutSetPlaybackRate(player->aout_0, ffp->pf_playback_rate);
        }
#else
        SDL_AoutSetPlaybackRate(player->aout_0, ffp->pf_playback_rate);
#endif
    }
    if (ffp->pf_playback_volume_changed) {
        ffp->pf_playback_volume_changed = 0;
        SDL_AoutSetPlaybackVolume(player->aout_0, ffp->pf_playback_volume);
    }

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = audio_decode_frame(ffp, player->is_0x30);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf = NULL;
               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
           } else {
               if (is->show_mode != SHOW_MODE_VIDEO)
                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        if (is->auddec.pkt_serial != is->audioq.serial) {
            is->audio_buf_index = is->audio_buf_size;
            memset(stream, 0, len);
            // stream += len;
            // len = 0;
            SDL_AoutFlushAudio(player->aout_0);
            break;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec - SDL_AoutGetLatencySeconds(player->aout_0), is->audio_clock_serial, ffp->audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
    if (!ffp->first_audio_frame_rendered) {
        ffp->first_audio_frame_rendered = 1;
        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_RENDERING_START);
    }

    if (is->latest_audio_seek_load_serial == is->audio_clock_serial) {
        int latest_audio_seek_load_serial = __atomic_exchange_n(&(is->latest_audio_seek_load_serial), -1, memory_order_seq_cst);
        if (latest_audio_seek_load_serial == is->audio_clock_serial) {
            if (ffp->av_sync_type == AV_SYNC_AUDIO_MASTER) {
                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 1);
            } else {
                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 0);
            }
        }
    }

    if (ffp->render_wait_start && !ffp->start_on_prepared && is->pause_req) {
        while (is->pause_req && !is->abort_request) {
            SDL_Delay(20);
        }
    }
}

static int audio_open(FFPlayer *opaque, VideoClip *player, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    MPTRACE("%s begin", __func__);
//    FFPlayer *ffp = opaque;
    VideoState *is = player->is_0x30;
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
#ifdef FFP_MERGE
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
#endif
    static const int next_sample_rates[] = {0, 44100, 48000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AoutGetAudioPerSecondCallBacks(player->aout_0)));
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = opaque;
    wanted_spec.userdata2 = player;
    while (SDL_AoutOpenAudio(player->aout_0, &wanted_spec, &spec) < 0) {
        /* avoid infinity loop on exit. --by bbcallen */
        if (is->abort_request)
            return -1;
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    SDL_AoutSetDefaultLatencySeconds(player->aout_0, ((double)(2 * spec.size)) / audio_hw_params->bytes_per_sec);
    return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(FFPlayer *ffp, VideoClip *player, int stream_index)
{
    MPTRACE("%s video111 begin", __func__);
    VideoState *is = player->is_0x30;
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec = NULL;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = ffp->lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name = ffp->audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = ffp->subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name = ffp->video_codec_name; break;
        default: break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with id %d\n", avctx->codec_id);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if(stream_lowres > av_codec_get_max_lowres(codec)){
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if(stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
    if (ffp->fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
    if(codec->capabilities & AV_CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif

    opts = filter_codec_opts(ffp->codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
#ifdef FFP_MERGE
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
#endif
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
                    MPTRACE("%s video111 5091", __func__);
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq           = avctx->sample_rate;
            is->audio_filter_src.channels       = avctx->channels;
            is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            SDL_LockMutex(ffp->af_mutex);
            if ((ret = configure_audio_filters(ffp, ffp->afilters, 0)) < 0) {
                SDL_UnlockMutex(ffp->af_mutex);
                goto fail;
            }
            ffp->af_changed = 0;
            SDL_UnlockMutex(ffp->af_mutex);
            sink = is->out_audio_filter;
            sample_rate    = av_buffersink_get_sample_rate(sink);
            nb_channels    = av_buffersink_get_channels(sink);
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output */
        if ((ret = audio_open(ffp, player, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        ffp_set_audio_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = 2.0 * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];
                        //MPTRACE("%s video111 5137 audio ", __func__);
        decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread, NULL);
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, ffp, player, "ff_audio_dec")) < 0)
            goto out;
        SDL_AoutPauseAudio(player->aout_0, 1); //sonxxx, old: 0
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if (ffp->async_init_decoder) {
            while (!is->initialized_decoder) {
                SDL_Delay(5);
            }
            if (player->node_vdec_0x18) {
                is->viddec.avctx = avctx;
                ret = ffpipeline_config_video_decoder(player->pipeline_0x10, ffp);
            }
            if (ret || !player->node_vdec_0x18) {
                MPTRACE("%s video111 5160 condition %p set %s", __func__,is->continue_read_thread);
                decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread,is->filename);
                ffp->empty_queue_next = is->continue_read_thread;
                player->node_vdec_0x18 = ffpipeline_open_video_decoder(player->pipeline_0x10, ffp, player);
                if (!player->node_vdec_0x18)
                    goto fail;
            }
        } else {
            MPTRACE("%s video111 5166 condition %p set %s", __func__,is->continue_read_thread,is->filename);
            decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread, is->filename);
            ffp->empty_queue_next = is->continue_read_thread;
            player->node_vdec_0x18 = ffpipeline_open_video_decoder(player->pipeline_0x10, ffp, player);
            if (!player->node_vdec_0x18)
                goto fail;
        }
        if ((ret = decoder_start(&is->viddec, video_thread, ffp, player, "ff_video_dec")) < 0)
            goto out;

        is->queue_attachments_req = 1;

        if (ffp->max_fps >= 0) {
            if(is->video_st->avg_frame_rate.den && is->video_st->avg_frame_rate.num) {
                double fps = av_q2d(is->video_st->avg_frame_rate);
                SDL_ProfilerReset(&is->viddec.decode_profiler, fps + 0.5);
                if (fps > ffp->max_fps && fps < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", fps);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", fps);
                }
            }
            if(is->video_st->r_frame_rate.den && is->video_st->r_frame_rate.num) {
                double tbr = av_q2d(is->video_st->r_frame_rate);
                if (tbr > ffp->max_fps && tbr < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", tbr);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", tbr);
                }
            }
        }

        if (is->is_video_high_fps) {
            avctx->skip_frame       = FFMAX(avctx->skip_frame, AVDISCARD_NONREF);
            avctx->skip_loop_filter = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
            avctx->skip_idct        = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
        }

        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (!ffp->subtitle) break;

        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        ffp_set_subtitle_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));
        MPTRACE("%s video111 5214 SUBTITLE ", __func__);
        decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread, NULL);
        if ((ret = decoder_start(&is->subdec, subtitle_thread, ffp, NULL, "ff_subtitle_dec")) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    //MPTRACE("%s begin", __func__);
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue, int min_frames) {
    //MPTRACE("%s begin", __func__);
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
#ifdef FFP_MERGE
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
#endif
           queue->nb_packets > min_frames;
}

static int is_realtime(AVFormatContext *s)
{
    //MPTRACE("%s begin", __func__);
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->filename, "rtp:", 4)
                 || !strncmp(s->filename, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg, void *arg2)
{
    MPTRACE("%s video111 start read_thread ", __func__);
    FFPlayer *ffp = arg;
    VideoClip *player = arg2;
    VideoState *is = player->is_0x30; //is VideoState : ffp, is_image, url
    AVFormatContext *ic = NULL;
    int err, i, ret __unused;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int completed = 0;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;
    int last_error = 0;
    int64_t prev_io_tick_counter = 0;
    int64_t io_tick_counter = 0;
    int init_ijkmeta = 0;

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
//    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(ffp->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&ffp->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    if (av_stristart(is->filename, "rtmp", NULL) ||
        av_stristart(is->filename, "rtsp", NULL)) {
        // There is total different meaning for 'timeout' option in rtmp
        av_log(ffp, AV_LOG_WARNING, "remove 'timeout' option for rtmp.\n");
        av_dict_set(&ffp->format_opts, "timeout", NULL, 0);
    }

    if (ffp->skip_calc_frame_rate) {
        av_dict_set_int(&ic->metadata, "skip-calc-frame-rate", ffp->skip_calc_frame_rate, 0);
        av_dict_set_int(&ffp->format_opts, "skip-calc-frame-rate", ffp->skip_calc_frame_rate, 0);
    }

    if (ffp->iformat_name)
        is->iformat = av_find_input_format(ffp->iformat_name);

    err = avformat_open_input(&ic, is->filename, is->iformat, &ffp->format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    ffp_notify_msg1(ffp, FFP_MSG_OPEN_INPUT);

    if (scan_all_pmts_set)
        av_dict_set(&ffp->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(ffp->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
#ifdef FFP_MERGE
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
#endif
    }
    is->ic = ic;

    if (ffp->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);
    //
    //AVDictionary **opts;
    //int orig_nb_streams;
    //opts = setup_find_stream_info_opts(ic, ffp->codec_opts);
    //orig_nb_streams = ic->nb_streams;


    if (ffp->find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, ffp->codec_opts);
        int orig_nb_streams = ic->nb_streams;

        do {
            if (av_stristart(is->filename, "data:", NULL) && orig_nb_streams > 0) {
                for (i = 0; i < orig_nb_streams; i++) {
                    if (!ic->streams[i] || !ic->streams[i]->codecpar || ic->streams[i]->codecpar->profile == FF_PROFILE_UNKNOWN) {
                        break;
                    }
                }

                if (i == orig_nb_streams) {
                    break;
                }
            }
            err = avformat_find_stream_info(ic, opts);
        } while(0);
        ffp_notify_msg1(ffp, FFP_MSG_FIND_STREAM_INFO);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }
    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (ffp->seek_by_bytes < 0)
        ffp->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    is->max_frame_duration = 10.0;
    av_log(ffp, AV_LOG_INFO, "max_frame_duration: %.3f\n", is->max_frame_duration);

#ifdef FFP_MERGE
    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

#endif
    /* if seeking requested, we execute it */
    if (ffp->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = ffp->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    av_dump_format(ic, 0, is->filename, 0);

    int video_stream_count = 0;
    int h264_stream_count = 0;
    int first_h264_stream = -1;
    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && ffp->wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, ffp->wanted_stream_spec[type]) > 0)
                st_index[type] = i;

        // choose first h264

        if (type == AVMEDIA_TYPE_VIDEO) {
            enum AVCodecID codec_id = st->codecpar->codec_id;
            video_stream_count++;
            if (codec_id == AV_CODEC_ID_H264) {
                h264_stream_count++;
                if (first_h264_stream < 0)
                    first_h264_stream = i;
            }
        }
    }
    if (video_stream_count > 1 && st_index[AVMEDIA_TYPE_VIDEO] < 0) {
        st_index[AVMEDIA_TYPE_VIDEO] = first_h264_stream;
        av_log(NULL, AV_LOG_WARNING, "multiple video stream found, prefer first h264 stream: %d\n", first_h264_stream);
    }
    if (!ffp->video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!ffp->audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
//    if (!ffp->video_disable && !ffp->subtitle_disable)
//        st_index[AVMEDIA_TYPE_SUBTITLE] =
//            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
//                                st_index[AVMEDIA_TYPE_SUBTITLE],
//                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
//                                 st_index[AVMEDIA_TYPE_AUDIO] :
//                                 st_index[AVMEDIA_TYPE_VIDEO]),
//                                NULL, 0);

    is->show_mode = ffp->show_mode;
#ifdef FFP_MERGE // bbc: dunno if we need this
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);
    }
#endif

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0 && !ffp->isSaveMode) {
        stream_component_open(ffp, player, st_index[AVMEDIA_TYPE_AUDIO]);
    } else {
        //ffp->av_sync_type = AV_SYNC_VIDEO_MASTER; //sonxxx
        is->av_sync_type  = AV_SYNC_VIDEO_MASTER;
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(ffp, player, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

//    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
//        stream_component_open(ffp, st_index[AVMEDIA_TYPE_SUBTITLE]);
//    }
    ffp_notify_msg1(ffp, FFP_MSG_COMPONENT_OPEN);

    if (!ffp->ijkmeta_delay_init) {
        ijkmeta_set_avformat_context_l(ffp->meta, ic);
    }

    ffp->stat.bit_rate = ic->bit_rate;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_VIDEO_STREAM, st_index[AVMEDIA_TYPE_VIDEO]);
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_AUDIO_STREAM, st_index[AVMEDIA_TYPE_AUDIO]);
//    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0)
//        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_TIMEDTEXT_STREAM, st_index[AVMEDIA_TYPE_SUBTITLE]);

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }
    if (is->audio_stream >= 0) {
        is->audioq.is_buffer_indicator = 1;
        is->buffer_indicator_queue = &is->audioq;
    } else if (is->video_stream >= 0) {
        is->videoq.is_buffer_indicator = 1;
        is->buffer_indicator_queue = &is->videoq;
    } else {
        assert("invalid streams");
    }

    if (ffp->infinite_buffer < 0 && is->realtime)
        ffp->infinite_buffer = 1;

    if (!ffp->render_wait_start && !ffp->start_on_prepared)
        toggle_pause2(ffp, player, 1);

    if (player->isUsed_0x60) {
        if (is->video_st && is->video_st->codecpar) {
            AVCodecParameters *codecpar = is->video_st->codecpar;
            ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, codecpar->width, codecpar->height);
            ffp_notify_msg3(ffp, FFP_MSG_SAR_CHANGED, codecpar->sample_aspect_ratio.num, codecpar->sample_aspect_ratio.den);
        }
        ffp->prepared = true;
        ffp_notify_msg1(ffp, FFP_MSG_PREPARED);
        //sonxxx
        //    if (!ffp->render_wait_start && !ffp->start_on_prepared) {
        //        while (is->pause_req && !is->abort_request) {
        //            SDL_Delay(20);
        //        }
        //    }
        //cho den khi no duoc select de play

        if (ffp->auto_resume) {
            ffp_notify_msg1(ffp, FFP_REQ_START);
            ffp->auto_resume = 0;
        }
    }

    //doi den khi duoc su dung
    MPTRACE("%s video111 set play ********** read_thread %s isUsed_0x60 %s abort_request %s", __func__,is->filename,player->isUsed_0x60 ? "true":"false" , is->abort_request? "true":"false");
    while (!player->isUsed_0x60 && !is->abort_request) {
    MPTRACE("%s videotrack1 doi den khi duoc su dung read_thread %s", __func__,is->filename,player->isUsed_0x60 , is->abort_request);
        SDL_Delay(20);
    }
    MPTRACE("%s videotrack1 chay tiep PPPPPPPPPPPPPP read_thread %s", __func__,is->filename,player->isUsed_0x60 , is->abort_request);

    /* offset should be seeked*/
    //sonxxx
//    if (ffp->seek_at_start > 0) {
//        ffp_seek_to_l(ffp, (long)(ffp->seek_at_start));
//    }

    for (;;) {
        if (is->abort_request){
        MPTRACE("%s video111 break read_thread %s", __func__,is->filename);
            break;
        }

#ifdef FFP_MERGE
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#endif
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(ffp->input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
                MPTRACE("%s video111 lock read_thread %s", __func__,is->filename);
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ffp_toggle_buffering(ffp, player, 1);
            ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_UPDATE, 0, 0);
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->filename);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                    // TODO: clear invaild audio data
                    // SDL_AoutFlushAudio(ffp->aout);
                }
//                if (is->subtitle_stream >= 0) {
//                    packet_queue_flush(&is->subtitleq);
//                    packet_queue_put(&is->subtitleq, &flush_pkt);
//                }
                if (is->video_stream >= 0) {
                    if (player->node_vdec_0x18) {
                        ffpipenode_flush(player->node_vdec_0x18);
                    }
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->vidclk, NAN, 0);
                } else {
                   set_clock(&is->vidclk, seek_target / (double)AV_TIME_BASE, 0);
                }
                sync_clock_to_slave(&is->extclk, &is->vidclk);

                is->latest_video_seek_load_serial = is->videoq.serial;
                is->latest_audio_seek_load_serial = is->audioq.serial;
                is->latest_seek_load_start_at = av_gettime();
            }
            ffp->dcc.current_high_water_mark_in_ms = ffp->dcc.first_high_water_mark_in_ms;
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
#ifdef FFP_MERGE
            if (is->paused)
                step_to_next_frame(is);
#endif
            completed = 0;
            MPTRACE("%s video111 run here 5656 read_thread %s", __func__,is->filename);
            //SDL_LockMutex(is->play_mutex); //sonxxx
            SDL_LockMutex(ffp->mutex_0x18);
            if (ffp->auto_resume) {
                is->pause_req = 0;
                if (ffp->packet_buffering)
                    is->buffering_on = 1;
                ffp->auto_resume = 0;
                stream_update_pause2_l(ffp, player);
            }
            if (is->pause_req)
                step_to_next_frame2_l(ffp, player);
//            SDL_UnlockMutex(is->play_mutex);
            SDL_UnlockMutex(ffp->mutex_0x18);

            if (ffp->enable_accurate_seek) {
                is->drop_aframe_count = 0;
                is->drop_vframe_count = 0;
                SDL_LockMutex(is->accurate_seek_mutex);
                if (is->video_stream >= 0) {
                    is->video_accurate_seek_req = 1;
                }
                if (is->audio_stream >= 0) {
                    is->audio_accurate_seek_req = 1;
                }
                SDL_CondSignal(is->audio_accurate_seek_cond);
                SDL_CondSignal(is->video_accurate_seek_cond);
                SDL_UnlockMutex(is->accurate_seek_mutex);
            }
            MPTRACE("%s videotrack1 FFP_MSG_SEEK_COMPLETE file name %s", __func__,is->filename);

            ffp_notify_msg3(ffp, FFP_MSG_SEEK_COMPLETE, (int)fftime_to_milliseconds(seek_target), ret);
            ffp_toggle_buffering(ffp, player, 1);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                AVPacket copy = { 0 };
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (ffp->infinite_buffer<1 && !is->seek_req &&
#ifdef FFP_MERGE
              (is->audioq.size + is->videoq.size > MAX_QUEUE_SIZE
#else
              (is->audioq.size + is->videoq.size > ffp->dcc.max_buffer_size
#endif
            || (   stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq, MIN_FRAMES)
                && stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq, MIN_FRAMES)))) {
            if (!is->eof) {
                ffp_toggle_buffering(ffp, player, 0); //sonxxx
            }
            MPTRACE("%s video111 the queue are full, no need to read more %s", __func__,is->filename);
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            MPTRACE("%s  SDL_CondWaitTimeout %p 5703", __func__,is->continue_read_thread);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            MPTRACE("%s  SDL_CondWaitTimeout continue àter  %p 5703", __func__,is->continue_read_thread);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if ((!is->paused || completed) &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (ffp->loop != 1 && (!ffp->loop || --ffp->loop)) {
                stream_seek(is, ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0, 0, 0);
            } else if (ffp->autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            } else {
                ffp_statistic_l(ffp, player);
                if (completed) {
                    av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: eof\n");
                    SDL_LockMutex(wait_mutex);
                    // infinite wait may block shutdown
                    while(!is->abort_request && !is->seek_req){
                        MPTRACE("%s video111 SDL_CondWaitTimeout %p 5723 %s", __func__,is->continue_read_thread, is->filename);
                        SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 100);
                    }
                    SDL_UnlockMutex(wait_mutex);
                    if (!is->abort_request){

                        MPTRACE("%s video111 5742 lock read_thread %s", __func__,is->filename);
                        continue;
                    }

                } else {
                    completed = 1;
                    ffp->auto_resume = 0;

                    // TODO: 0 it's a bit early to notify complete here
                    ffp_toggle_buffering(ffp, player, 0);
                    toggle_pause2(ffp, player, 1);
                    if (ffp->error) {
                        av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: error: %d\n", ffp->error);
                        ffp_notify_msg1(ffp, FFP_MSG_ERROR);
                    } else {
                        av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: completed: OK\n");
                        //ffp_notify_msg1(ffp, FFP_MSG_COMPLETED); //sonxxx
                    }
                }
            }
        }
        pkt->flags = 0;
        ret = av_read_frame(ic, pkt);
        if (ret < 0) { //erro read frame or end of frame
            int pb_eof = 0;
            int pb_error = 0;
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                ffp_check_buffering_l(ffp, is);
                pb_eof = 1;
                // check error later
            }
            if (ic->pb && ic->pb->error) {
                pb_eof = 1;
                pb_error = ic->pb->error;
            }
            if (ret == AVERROR_EXIT) {
                pb_eof = 1;
                pb_error = AVERROR_EXIT;
            }

            if (pb_eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
//                if (is->subtitle_stream >= 0)
//                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
            }
            if (pb_error) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
//                if (is->subtitle_stream >= 0)
//                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
                ffp->error = pb_error;
                av_log(ffp, AV_LOG_ERROR, "av_read_frame error: %s\n", ffp_get_error_string(ffp->error));
                // break;
            } else {
                ffp->error = 0;
            }
            if (is->eof) {
                ffp_toggle_buffering(ffp, player, 0);
                SDL_Delay(100);
            }
            SDL_LockMutex(wait_mutex);
            MPTRACE("%s video111 SDL_CondWaitTimeout 5792 %p name : %s", __func__,is->continue_read_thread,is->filename);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            //MPTRACE("%s video111 SDL_CondWaitTimeout continue àter  %p 5792", __func__,is->continue_read_thread);

            SDL_UnlockMutex(wait_mutex);
            ffp_statistic_l(ffp, player);
            continue;
        } else {
            is->eof = 0;
        }
        MPTRACE("%s video111 thread %s cond %p running", __func__,is->filename, is->continue_read_thread);

        if (pkt->flags & AV_PKT_FLAG_DISCONTINUITY) {
            if (is->audio_stream >= 0) {
                packet_queue_put(&is->audioq, &flush_pkt);
            }
//            if (is->subtitle_stream >= 0) {
//                packet_queue_put(&is->subtitleq, &flush_pkt);
//            }
            if (is->video_stream >= 0) {
                MPTRACE("%s video112 put data to queue %s", __func__,is->filename);
                packet_queue_put(&is->videoq, &flush_pkt);
            }
        }

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = ffp->duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0) / 1000000
                <= ((double)ffp->duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))) {
                   MPTRACE("%s video113 put data to queue %s", __func__,is->filename);
            packet_queue_put(&is->videoq, pkt);
        } else {
            av_packet_unref(pkt);
        }

        ffp_statistic_l(ffp, player);

        if (ffp->ijkmeta_delay_init && !init_ijkmeta &&
                (ffp->first_video_frame_rendered || !is->video_st) && (ffp->first_audio_frame_rendered || !is->audio_st)) {
            ijkmeta_set_avformat_context_l(ffp->meta, ic);
            init_ijkmeta = 1;
        }

        if (ffp->packet_buffering) {
            io_tick_counter = SDL_GetTickHR();
            if ((!ffp->first_video_frame_rendered && is->video_st) || (!ffp->first_audio_frame_rendered && is->audio_st)) {
                if (abs((int)(io_tick_counter - prev_io_tick_counter)) > FAST_BUFFERING_CHECK_PER_MILLISECONDS) {
                    prev_io_tick_counter = io_tick_counter;
                    ffp->dcc.current_high_water_mark_in_ms = ffp->dcc.first_high_water_mark_in_ms;
                    ffp_check_buffering_l(ffp, is);
                }
            } else {
                if (abs((int)(io_tick_counter - prev_io_tick_counter)) > BUFFERING_CHECK_PER_MILLISECONDS) {
                    prev_io_tick_counter = io_tick_counter;
                    ffp_check_buffering_l(ffp, is);
                }
            }
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    if (!ffp->prepared || !is->abort_request) {
        ffp->last_error = last_error;
        ffp_notify_msg2(ffp, FFP_MSG_ERROR, last_error);
    }
    MPTRACE("%s video111 done thread !!!! 5792 %p %s", __func__,is->continue_read_thread,is->filename);
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

static int video_refresh_thread(void *arg, void *arg2);
static VideoState *stream_open(FFPlayer *ffp, const char *filename, AVInputFormat *iformat)
{
    return NULL;
//    MPTRACE("%s begin", __func__);
//    //assert(!ffp->is);
//    VideoState *is;

//    MPTRACE("%s begin", __func__);

//    is = av_mallocz(sizeof(VideoState));
//    if (!is)
//        return NULL;
//    is->filename = av_strdup(filename);
//    if (!is->filename)
//        goto fail;
//    is->iformat = iformat;
//    is->ytop    = 0;
//    is->xleft   = 0;
//#if defined(__ANDROID__)
//    if (ffp->soundtouch_enable) {
//        is->handle = ijk_soundtouch_create();
//    }
//#endif

//    /* start video display */
//    if (frame_queue_init(&is->pictq, &is->videoq, ffp->pictq_size, 1) < 0)
//        goto fail;
//    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
//        goto fail;
//    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
//        goto fail;

//    if (packet_queue_init(&is->videoq) < 0 ||
//        packet_queue_init(&is->audioq) < 0 ||
//        packet_queue_init(&is->subtitleq) < 0)
//        goto fail;

//    if (!(is->continue_read_thread = SDL_CreateCond())) {
//        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
//        goto fail;
//    }

//    if (!(is->video_accurate_seek_cond = SDL_CreateCond())) {
//        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
//        ffp->enable_accurate_seek = 0;
//    }

//    if (!(is->audio_accurate_seek_cond = SDL_CreateCond())) {
//        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
//        ffp->enable_accurate_seek = 0;
//    }

//    init_clock(&is->vidclk, &is->videoq.serial);
//    init_clock(&is->audclk, &is->audioq.serial);
//    init_clock(&is->extclk, &is->extclk.serial);
//    is->audio_clock_serial = -1;
//    if (ffp->startup_volume < 0)
//        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", ffp->startup_volume);
//    if (ffp->startup_volume > 100)
//        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", ffp->startup_volume);
//    ffp->startup_volume = av_clip(ffp->startup_volume, 0, 100);
//    ffp->startup_volume = av_clip(SDL_MIX_MAXVOLUME * ffp->startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
//    is->audio_volume = ffp->startup_volume;
//    is->muted = 0;
//    is->av_sync_type = ffp->av_sync_type;

//    is->play_mutex = SDL_CreateMutex();
//    is->accurate_seek_mutex = SDL_CreateMutex();
//    ffp->is = is;
//    is->pause_req = !ffp->start_on_prepared;

//    is->video_refresh_tid = SDL_CreateThreadEx(&is->_video_refresh_tid, video_refresh_thread, ffp, 0, "ff_vout");
//    if (!is->video_refresh_tid) {
//        av_freep(&ffp->is);
//        return NULL;
//    }

//    is->initialized_decoder = 0;
//    is->read_tid = SDL_CreateThreadEx(&is->_read_tid, read_thread, ffp, 0, "ff_read");
//    if (!is->read_tid) {
//        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
//        goto fail;
//    }

//    if (ffp->async_init_decoder && !ffp->video_disable && ffp->video_mime_type && strlen(ffp->video_mime_type) > 0
//                    && ffp->mediacodec_default_name && strlen(ffp->mediacodec_default_name) > 0) {
//        if (ffp->mediacodec_all_videos || ffp->mediacodec_avc || ffp->mediacodec_hevc || ffp->mediacodec_mpeg2) {
//            decoder_init(&is->viddec, NULL, &is->videoq, is->continue_read_thread);
//            ffp->node_vdec = ffpipeline_init_video_decoder(ffp->pipeline, ffp);
//        }
//    }
//    is->initialized_decoder = 1;

//    return is;
//fail:
//    is->initialized_decoder = 1;
//    is->abort_request = true;
//    if (is->video_refresh_tid)
//        SDL_WaitThread(is->video_refresh_tid, NULL);
//    stream_close(ffp);
//    return NULL;
}

int load_image_clip(JNIEnv *env, FFPlayer *ffp, VideoClip *clip)
{
    MPTRACE("%s begin", __func__);
    VideoState *is;
    jlong result;

    is = clip->is_0x30;
    if (is->img_handler_0xa24 == 0) {
        result = J4AC_tv_danmaku_ijk_media_player_IImageLoader__loadImage__withCString__catchAll
                        (env, ffp->img_loader_0x3b8, is->filename);
        is->img_handler_0xa24 = (AVFrame *) result;
        return (result != 0 ? 1 : -1);
    }
    return 1;
}

void send_seek_complete_message(MessageQueue *queue, VideoState *is)
{
    int what;

    if (is->step != 0) {
        return;
    }

    if (is->f_0xf8 == 0) {
        what = FFP_MSG_SEEK_COMPLETE; //600
    } else {
        what = 0x259; //601
    }
    msg_queue_put_simple1(queue, what);
}

void process_after_load_image(FFPlayer *ffp, VideoClip *clip) {
    MPTRACE("%s begin", __func__);
    VideoState *is;
    int64_t oldPos;
    AVFrame *imgFrame;

    if (ffp == 0 || clip == 0) {
        return;
    }

    SDL_LockMutex(clip->mutext_0x20);
    is = clip->is_0x30;
    send_seek_complete_message(&ffp->msg_queue, is);
    is->seek_req = 0;
    is->f_0x100 = 0;
    SDL_CondSignal(clip->con_0x28);

    if (is->img_handler_0xa24 != 0) {
        SDL_LockMutex(is->mutex_2_0x8);
        if (is->f_0 == 0) {
            is->f_0 = 1;
            SDL_CondSignal(is->cond_4_0x10);
            SDL_UnlockMutex(is->mutex_2_0x8);
        } else {
            SDL_UnlockMutex(is->mutex_2_0x8);
        }

        if ((clip->isUsed_0x60 != 0) && (ffp->prepared == 0)) {
            ffp->prepared = 1;
            msg_queue_put_simple1(&ffp->msg_queue, FFP_MSG_PREPARED);
        }

        SDL_LockMutex(ffp->mutex_0x18);
        oldPos = is->seek_pos;
        is->img_handler_0xa24->pts = oldPos;
        imgFrame = is->img_handler_0xa24;
        is->img_handler_0xa24->pkt_dts = oldPos;
        ffp_queue_picture2(ffp, clip, imgFrame, imgFrame->pts * 0.000001, clip->end_time_0x70 * 0.000001,
                               imgFrame->pts, is->videoq.serial);
        ffp->f_0x140 = 0;
        SDL_UnlockMutex(ffp->mutex_0x18);
        if ((is->seek_flags & AVSEEK_FLAG_BYTE) == 0) {
            set_clock(&is->vidclk, is->seek_pos * 0.000001, is->videoq.serial);
        } else {
            set_clock(&is->vidclk, NAN, 0);
        }
        if (is->f_0xf8 == 0) {
            SDL_LockMutex(ffp->mutex_0x18);
            if (ffp->auto_resume != 0) {
                is->pause_req = 0;
                ffp->auto_resume = 0;
                stream_update_pause2_l(ffp, clip);
            }
            if (is->pause_req != 0) {
                step_to_next_frame2_l(ffp, clip);
                SDL_AoutFlushAudio(clip->aout_0);
            }
            SDL_UnlockMutex(ffp->mutex_0x18);
        }
        SDL_UnlockMutex(clip->mutext_0x20);
        if (is->seek_pos != oldPos) {
            is->seek_req = 1;
        }
        return;
    }
    SDL_UnlockMutex(clip->mutext_0x20);
}

int image_load_thread(void *param, void *param2)
{
    MPTRACE("%s begin", __func__);
    int ret;
    JNIEnv *env;
    FFPlayer *ffp;
    VideoClip *clip;
    VideoState *is;

    ret = SDL_JNI_SetupThreadEnv(&env);
    if (ret != JNI_OK) {
        av_log(0, AV_LOG_ERROR, "%s:SDL_JNI_SetupThreadEnv failed","image_load_thread");
        return -1;
    }

    clip = (VideoClip *) param;
    is = clip->is_0x30;
    ffp = is->ffp;

    packet_queue_start(is->pictq.pktq);
    packet_queue_start(&is->videoq);

    MPTRACE("image_load_thread start");
    while (!is->abort_request) {
        while (is->seek_req != 0) {
            packet_queue_flush(&is->videoq);
            packet_queue_put(&is->videoq, &flush_pkt);
            ret = load_image_clip(env, ffp, clip);
            if (ret < 0) {
                break;
            }

            process_after_load_image(ffp, clip);
            if (is->abort_request) {
                return 0;
            }
        }
        av_usleep(5000);
    }

    MPTRACE("image_load_thread exit");
    return 0;
}

int stream_open_image(FFPlayer *ffp, VideoClip *useClip, ClipInfo *clip, int flag) {
    MPTRACE("%s begin", __func__);
    IJKFF_Pipeline *pipe;
    int ret;
    JNIEnv *env;
    jobject surface;
    VideoState *is;
    SDL_Vout *vout = SDL_VoutAndroid_CreateForAndroidSurface();
    useClip->vout_0x8 = vout;
    if (vout == NULL) {
        return -1;
    }

    pipe = ffpipeline_create_from_android(ffp);
    useClip->pipeline_0x10 = pipe;
    if (pipe == NULL) {
        return -1;
    }

    ffpipeline_set_vout(pipe, useClip->vout_0x8);
    ret = SDL_JNI_SetupThreadEnv(&env);
    if (ret != JNI_OK) {
        av_log(ffp, AV_LOG_ERROR, "%s: SetupThreadEnv failed\n","stream_open_image");
        return -1;
    }

    surface = J4AC_tv_danmaku_ijk_media_player_ISurfaceCreator__getSurface__asGlobalRef__catchAll
               (env, clip->surface_creator);
    if (surface == NULL) {
        return -1;
    }

    SDL_VoutAndroid_SetAndroidSurface(env, useClip->vout_0x8, surface);
    ffpipeline_set_surface(env, useClip->pipeline_0x10, surface);
    is = av_mallocz(sizeof(VideoState));
    if (is == NULL) {
        SDL_JNI_DeleteGlobalRefP(env, &surface);
        return -1;
    }

    is->surface = surface;
    is->surface_creator = (*env)->NewGlobalRef(env, clip->surface_creator);
    is->meta = ijkmeta_create();
    is->f_0x100 = 1;
    is->f_0xf8 = 1;

    is->play_mutex = SDL_CreateMutex();
    is->mutex_2_0x8 = SDL_CreateMutex();
    is->cond_4_0x10 = SDL_CreateCond();
    is->continue_read_thread = SDL_CreateCond();
    is->filename = av_strdup(clip->url);
    if (is->filename == NULL) {
        stream_close2(ffp, useClip);
        return -1;
    }

//    *(undefined8 *)(puVar6 + 0x2e) = param_4;
    is->audio_stream = -1;
    is->video_stream = -1;
    is->ffp = ffp;
    is->is_image = clip->is_image_4;
    is->img_handler_0xa24 = 0;
    is->play_rate_0xa0c = 1.0;
    ret = frame_queue_init(&is->pictq, &is->videoq, ffp->pictq_size, 1);
    ret = packet_queue_init(&is->videoq);
    is->vidclk.ext1 = 0x646976;
    is->audclk.ext1 = 0x647561;
    is->extclk.ext1 = 0x747865;
    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->videoq.serial);
    is->av_sync_type = ffp->av_sync_type;
//    puVar6[0x495] = 0x80;
//    puVar6[0x3e6] = -1;
    is->seek_req = 1;
    useClip->is_0x30 = is;
    is->pause_req = (ffp->start_on_prepared == 0);
    //is->paused = is->pause_req;
    is->image_load_thread = SDL_CreateThreadEx(&is->_image_load_thread, image_load_thread, useClip, 0,
                               "image_load_thread");
    return 0;
}

static VideoClip *stream_open_video(FFPlayer *ffp, ClipInfo *clip, int isUsed, int switchClip)
{
    MPTRACE("%s begin", __func__);
    JNIEnv *env;
    VideoState *is;
    ClipEditOp *clipState;
    VideoClip *pClip;
    int index;
    int found;
    int ret;
    jobject surface;

    if (clip == NULL) {
        return NULL;
    }

    //tim 1 ci free de su dung, neu khong co thi free ci 0 va dung no
    clipState = ffp->clipState;
    index = clip_op_queue_index_of_clip_list(clipState->head_0x18, clip);
    for (int i = 0; i < clipState->f_2; i++) {
        pClip = ffp_clip_op_get_ci_at_index(ffp, i);
        if (pClip->clip_id_0x58 == clip->id_0) {
            pClip->begin_time_0x68 = clip->begin_file;
            pClip->end_time_0x70 = clip->end_file;
            pClip->duration_0x78 = clip->duration;
            pClip->volume_0x88 = clip->volume;
            pClip->volume2_0x8c = clip->volume2;
            pClip->speed_0xa0 = clip->speed_0x40;

            if (pClip->is_0x30 != NULL) {
                pClip->is_0x30->play_rate_0xa0c = clip->speed_0x40;
            }
            pClip->queue_index_0x5c = index;
            pClip->isUsed_0x60 = isUsed;

            if(isUsed && pClip->changeVideo){
                ffp->pf_playback_rate = clip->speed_0x40;
                ffp->pf_playback_rate_changed = 1;
            }else{
                ffp->pf_playback_rate_changed = 0;
            }

            MPTRACE("%s videotrack1 return  &&*********************&&& found ci=%d of clip id=%d url %s ", __func__, i, clip->id_0,clip->url);
            return pClip;
        }
    }

    //tim 1 ci free
    found = 0;
    for (int i = 0; i < clipState->f_2; i++) {
        pClip = ffp_clip_op_get_ci_at_index(ffp, i);
        if (pClip->clip_id_0x58 < 0) {
            found = 1;
            break;
        }
    }

    //ko co thi free ci 0
    if (!found) {
        pClip = ffp_clip_op_get_ci_at_index(ffp, 0);
        clip_op_ci_release_to_pool(ffp, pClip);
    }

    //su dung pClip
    pClip->begin_time_0x68 = clip->begin_file;
    pClip->end_time_0x70 = clip->end_file;
    pClip->duration_0x78 = clip->duration;
    pClip->volume_0x88 = clip->volume;
    pClip->volume2_0x8c = clip->volume2;
    pClip->speed_0xa0 = clip->speed_0x40;
    if (pClip->is_0x30 != NULL) {
        pClip->is_0x30->play_rate_0xa0c = clip->speed_0x40;
    }
    pClip->queue_index_0x5c = index;
    pClip->clip_id_0x58 = clip->id_0;
    pClip->isUsed_0x60 = isUsed; //sonxxx. set luon de con dieu huong cho read thread

    if(isUsed && pClip->changeVideo){
        ffp->pf_playback_rate = pClip->speed_0xa0;
        ffp->pf_playback_rate_changed = 1;
    }else{
        ffp->pf_playback_rate_changed = 0;
    }

    if (!clip->is_image_4) {
        pClip->vout_0x8 = SDL_VoutAndroid_CreateForAndroidSurface();
        pClip->pipeline_0x10 = ffpipeline_create_from_android(ffp);
        ffpipeline_set_vout(pClip->pipeline_0x10, pClip->vout_0x8);
        SDL_JNI_SetupThreadEnv(&env);
        surface = J4AC_tv_danmaku_ijk_media_player_ISurfaceCreator__getSurface__asGlobalRef__catchAll(env, clip->surface_creator);
        SDL_VoutAndroid_SetAndroidSurface(env, pClip->vout_0x8, surface);
        ffpipeline_set_surface(env, pClip->pipeline_0x10, surface);
        pClip->aout_0 = ffpipeline_open_audio_output(pClip->pipeline_0x10, ffp);

        is = av_mallocz(sizeof(VideoState));
        is->surface = surface;
        is->surface_creator = (*env)->NewGlobalRef(env, clip->surface_creator);
        is->meta = ijkmeta_create();
        is->f_0 = 0;
        is->f_0x100 = 1;
        is->f_0xf8 = 1;

        is->mutex_2_0x8 = SDL_CreateMutex();
        is->cond_4_0x10 = SDL_CreateCond();
        is->play_mutex = SDL_CreateMutex();

        is->filename = av_strdup(clip->url);
        is->audio_stream = -1;
        is->video_stream = -1;
        is->play_rate_0xa0c = 1.0;
        is->ffp = ffp;
        is->is_image = clip->is_image_4;
        is->f_0x101448 = AV_NOPTS_VALUE;
        is->f_0x101450 = AV_NOPTS_VALUE;
        is->f_0x101458 = AV_NOPTS_VALUE;
        is->f_0x101460 = AV_NOPTS_VALUE;

#if defined(__ANDROID__)
        if (ffp->soundtouch_enable) {
            is->handle = ijk_soundtouch_create();
        }
#endif
        /* start video display */
        if (frame_queue_init(&is->pictq, &is->videoq, ffp->pictq_size, 1) < 0)
            goto fail;
        if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
            goto fail;

        if (packet_queue_init(&is->videoq) < 0 ||
            packet_queue_init(&is->audioq) < 0)
            goto fail;

        if (!(is->continue_read_thread = SDL_CreateCond())) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
            goto fail;
        }

        is->vidclk.ext1 = 0x646976;
        is->audclk.ext1 = 0x647561;
        is->extclk.ext1 = 0x747865;
        init_clock(&is->vidclk, &is->videoq.serial);
        init_clock(&is->audclk, &is->audioq.serial);
        init_clock(&is->extclk, &is->videoq.serial);

        is->av_sync_type = ffp->av_sync_type;
        is->audio_volume = 128;
//        puVar8[0x3e6] = 0xffffffff;
        is->pause_req = (!ffp->start_on_prepared);

        ffpipeline_set_volume(pClip->pipeline_0x10, pClip->aout_0, pClip->volume_0x88, pClip->volume2_0x8c);
        pClip->is_0x30 = is;
        is->read_tid = SDL_CreateThreadEx(&is->_read_tid, read_thread, ffp, pClip, "ff_read");
        if (!is->read_tid) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
            goto fail;
        }
        pClip->f_0x64_100 = 1;
        return pClip;
    } else {
        ret = stream_open_image(ffp, pClip, clip, 0);
        if (ret > -1) {
            pClip->f_0x64_100 = 1;
            return pClip;
        }
    }

fail:
    MPTRACE("stream open video fail");
    is->initialized_decoder = 1;
    is->abort_request = true;
    if (is->video_refresh_tid)
        SDL_WaitThread(is->video_refresh_tid, NULL);
    stream_close2(ffp, pClip);
    return NULL;
}

int ffp_clip_op_queue_remaining_nb(FFPlayer *ffp)
{
    return ffp->clipState->nb_clips_5_0x28;
}

int ffp_switch_to_next_clip(FFPlayer *ffp, int clipId)
{
    MPTRACE("%s videotrack1 run video index clipid %d ", __func__,clipId);
    int iVar4;
    int uVar5;
    ClipInfo *lVar6;
    VideoClip *lVar7;
    VideoClip *player2;
    ClipEditOp *lVar10;
    uint uVar11;
    int64_t lVar12;
    VideoClip *local_48;

    local_48 = 0;
    SDL_LockMutex(ffp->clipState->mutext_8);
    iVar4 = ffp_clip_op_get_play_ci(ffp, &local_48);
    if (iVar4 < 0) {
        SDL_UnlockMutex(ffp->clipState->mutext_8);
        uVar5 = -1;
        goto LAB_0011c1a8;
    }

    MPTRACE("%s video111 run video index begin. clipId=%d, player=%p, player cid=%d, player cindx=%d", __func__, clipId, local_48, local_48->clip_id_0x58, local_48->queue_index_0x5c);
    if (local_48->clip_id_0x58 != clipId) {
        av_log(ffp,0x18,"%s: curplay clip_id is changed,%d-->%d","ffp_switch_to_next_clip",
               clipId, local_48->clip_id_0x58);
        SDL_UnlockMutex(ffp->clipState->mutext_8);
        uVar5 = 0;
        goto LAB_0011c1a8;
    }

    local_48->is_0x30->pause_req = 1;
    ffp->auto_resume = 0;
    stream_update_pause2_l(ffp, local_48);

//    if (ffp->clock_0x410 != NULL) {
//        set_clock(ffp->clock_0x410, NAN, 0);
//    }

    if ((ffp->f_0x3a0 != 1) &&
            (iVar4 = ffp_clip_op_queue_remaining_nb(ffp), iVar4 != local_48->queue_index_0x5c + 1)) {
        lVar6 = clip_op_queue_get(ffp->clipState->head_0x18, local_48->queue_index_0x5c + 1);
        lVar7 = stream_open_video(ffp, lVar6, 1,1);
        if (lVar7 != 0) {
            lVar10 = ffp->clipState;
            lVar12 = clip_op_queue_calc_start_timeline(lVar10->head_0x18, lVar6);

            uVar11 = 0;
            if (0 < lVar10->f_2) {
                do {
                    while( true ) {
                        player2 = ffp_clip_op_get_ci_at_index(ffp, uVar11);
                        player2->isUsed_0x60 = (lVar7 == player2);
                        if(player2->isUsed_0x60){
                            MPTRACE("%s videotrack1 play video clipid %d ", __func__,clipId);
                        }
                        if (lVar7 != player2) break;
                        uVar11 = uVar11 + 1;
                        ffp_clip_update_time(ffp, player2, lVar12);
                        if (lVar10->f_2 <= uVar11) goto LAB_0011c2e4;
                    }
                    uVar11 = uVar11 + 1;
                } while (uVar11 < lVar10->f_2);
            }

LAB_0011c2e4:
            lVar7->is_0x30->pause_req = 0;
            ffp->auto_resume = 1;
            lVar7->is_0x30->seek_req = 1; //sonxxx
            stream_update_pause2_l(ffp, lVar7);
            local_48->is_0x30->f_0x9f4 =0;
        }
        SDL_UnlockMutex(ffp->clipState->mutext_8);
        MPTRACE("%s run video index FFP_MSG_0x186a1 : %d", __func__, local_48->queue_index_0x5c + 1);
        msg_queue_put_simple2(&ffp->msg_queue, FFP_MSG_0x186a1, local_48->queue_index_0x5c + 1);
        uVar5 = 0;
        goto LAB_0011c1a8;
    }

    msg_queue_put_simple1(&ffp->msg_queue, FFP_MSG_COMPLETED);
    SDL_UnlockMutex(ffp->clipState->mutext_8);
    uVar5 = 0;

LAB_0011c1a8:
    MPTRACE("%s end. ret: %d", __func__, uVar5);
    return uVar5;
}

void refresh_not_444(FFPlayer *ffp) {
        //MPTRACE("%s run video index ", __func__);
    double remaining_time = 0.0;
    VideoState *is;
    VideoClip *player = 0;
    int playerIndex;
    double time;
    int64_t duration;
    double currentTime;
    double dVar25;
    double dVar26;
    double delay;
    int iVar4;
    int uVar7;
    int64_t lVar6;
    int64_t lVar9;
    Frame *lVar10;
    Frame *lVar11;
    int64_t lVar13;
    int64_t lVar14;
    int64_t lVar17;
    int64_t lVar20;
    int64_t lVar21;
    float fVar22;
    char bVar2;
    char bVar3;

LAB_0011c528:
    while(!ffp->abort_request) {
        if (remaining_time > 0) {
            av_usleep((unsigned) (remaining_time * 1000000.0));
            //MPTRACE("%s sleeping", __func__);
            //av_usleep((unsigned) (1 * 1000000.0));
        }

        if (player == NULL) {
            //MPTRACE("%s player is null", __func__);
            remaining_time = 0.01;
            goto LAB_0011c6b4;
        }

        is = player->is_0x30;
        if (is == 0 || is->abort_request || is->show_mode == SHOW_MODE_NONE ||
                (is->paused && !is->force_refresh)) {
            //MPTRACE("%s invalid values", __func__);
            remaining_time = 0.01;
            goto LAB_END_DISPLAY;
        }

        if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime) {
            check_external_clock_speed(is);
        }

        if ((ffp->display_disable == 0) && (is->show_mode != SHOW_MODE_VIDEO)) {
            //MPTRACE("%s show mode not video: %d", __func__, is->show_mode);
            if (is->audio_st == 0) {
                //MPTRACE("%s play_rate_0xa0c=%f", __func__, is->play_rate_0xa0c);
                if (2.0 < is->play_rate_0xa0c) {
LAB_0011cd4c:
                    remaining_time = 0.001;
                } else {
                    remaining_time = 0.01;
                }
                if (is->play_rate_0xa0c == is->extclk.speed) goto LAB_0011c770;
            } else {
                time = av_gettime_relative() * 0.000001; //thoi diem hien tai
                dVar25 = ffp->rdftspeed + is->last_vis_time; //thoi diem hien thi tiep theo
                if ((is->force_refresh) || (dVar25 < time)) {
                    video_image_display2(ffp, player);
                    is->last_vis_time = time;
                    dVar25 = time + ffp->rdftspeed; //next show
                }
                remaining_time = FFMIN(dVar25 - time, 0.01); //thoi gian cho de show tiep
                if (2.00000000 < is->play_rate_0xa0c) goto LAB_0011c76c;
LAB_0011c5fc:
                if ((is->audio_st != 0) || (dVar25 = is->play_rate_0xa0c, dVar25 == is->extclk.speed)) goto LAB_0011c770;
            }

            av_log(ffp, 0x20,"didn\'t select an audio track, setclock speedinstead");
            set_clock_speed(&is->extclk, is->play_rate_0xa0c);
            if (is->video_st == 0) goto LAB_0011c63c;
LAB_0011c778:
            if (is->force_refresh == 0) {
                uVar7 = 0;
            } else {
                uVar7 = frame_queue_prev(&is->pictq);
            }
            lVar6 = (player->end_time_0x70);
            lVar9 = (player->begin_time_0x68);
            fVar22 = (player->speed_0xa0);
            bVar2 = 0;
LAB_0011c7b0:
            if (frame_queue_nb_remaining(&is->pictq) != 0) {
                while( true ) {
                   // MPTRACE("%s run video index wiating  %d", __func__);
                    lVar21 = (player->begin_time_0x68);//start time
                    lVar20 = (player->duration_0x78); //duration
                    lVar10 = frame_queue_peek_last(&is->pictq);//lay frame cuoi
                    lVar11 = frame_queue_peek(&is->pictq);
                    if (lVar11->serial != is->videoq.serial) break;
                    dVar25 = lVar11->pts * 1000000.00000000;
                    lVar13 = (int64_t) dVar25;
                    lVar14 = lVar13;
                    if ((is->is_image == '\0') &&
                         (lVar17 = is->ic->start_time, lVar17 < lVar13) &&
                         (lVar14 = lVar13 - lVar17, lVar17 < 1)) {

                        lVar14 = lVar13;
                    }
                    bVar2 = player->doneVideo;
                    if (lVar21 + lVar20 <= lVar14) { //khi start time + duration < time của frame cuối: hết video
                        bVar2 = 1;
                    }
                    //if(player->begin_time_0x68 + player->duration_0x78 >=player->end_time_0x70){
                     //   bVar2 = 1;
                    //}
                    if(player->doneVideo == 1) {
                        MPTRACE("%s run video index done video, switch next  %d", __func__);
                    }

                    iVar4 = uVar7;
                    bVar3 = (iVar4 == 0);
                    if (bVar3 && (lVar11->serial != lVar10->serial)) {
                        is->frame_timer = av_gettime_relative() * 0.000001;
                    }
                    currentTime = av_gettime_relative() * 0.00000100;
                    if (is->paused == 0) {
                        /* compute nominal last_duration */
                        dVar26 = vp_duration(is, lVar10, lVar11);
                        delay = 0.00000000;
                        if (iVar4 == 0) {
                            delay = compute_target_delay(ffp, dVar26, is);
                            bVar3 = is->is_image;
                            if (!bVar3) {
                                if ((0.05000000 <= dVar26 / player->speed_0xa0) &&
                                        (dVar25 = currentTime - ffp->f_0x3b0, 0.03000000 < dVar25)) {
                                    video_image_display2(ffp, player);
                                    ffp->f_0x3b0 = currentTime;
                                    //MPTRACE("%s 01", __func__);
                                    goto LAB_0011c7b0;
                                }
                                bVar3 = 1;
                            }
                        }

                        if (isnan(is->frame_timer) || currentTime < is->frame_timer) {
                            is->frame_timer = currentTime;
                        }
                        if ((currentTime < is->frame_timer + delay) && bVar3) {
                            remaining_time = FFMIN(remaining_time, is->frame_timer + delay - currentTime);
                            //MPTRACE("%s 02", __func__);
                            goto LAB_0011cc10;
                        }
                        is->frame_timer += delay;
                        if (delay > 0 && currentTime - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {
                            is->frame_timer = currentTime;
                        }

                        SDL_LockMutex(is->pictq.mutex);
                        if (bVar3) {
                            if (!isnan(lVar11->pts)) {
                                update_video_pts(is, lVar11->pts, lVar11->pos, lVar11->serial);
                            }
                        }
                        SDL_UnlockMutex(is->pictq.mutex);

                        if (1 < frame_queue_nb_remaining(&is->pictq)) {
                            lVar10 = frame_queue_peek_next(&is->pictq);
                            dVar25 = vp_duration(is, lVar11, lVar10);

                            if (is->step == 0) {
                                if (iVar4 == 0) {
                                    if ((0 < ffp->framedrop || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
                                         && (dVar25 + is->frame_timer < currentTime)) {
LAB_0011ca8c:
                                        is->frame_drops_late = is->frame_drops_late + 1;
                                        break;
                                    }
                                } else {
                                    if (dVar25 + is->frame_timer < currentTime) {
                                        if (bVar3) {
                                            //MPTRACE("%s 03", __func__);
                                            goto LAB_0011ca8c;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    //MPTRACE("%s 04: %d", __func__, is->step);
                    if ((ffp->display_disable != 0) || (is->show_mode != 0)) {
LAB_0011cbc8:
                        if (is->f_0x101414 != 0) {
                            is->f_0x101414 = 0;
                        }
                        frame_queue_next(&is->pictq);
                        SDL_LockMutex(ffp->mutex_0x18);
                        if (((lVar11->serial == is->videoq.serial) &&
                             (player->end_time_0x70 != AV_NOPTS_VALUE)) &&
                                (player->end_time_0x70 * 0.00000100 <= lVar11->pts)) {

                            av_log(ffp,0x20,"%s:reach clip end_time:%f", "video_refresh", lVar11->pts);
                            ffp->f_0x140 = 1;
                        }

                        if (is->step != 0) {
                            is->step = 0;
                            send_seek_complete_message(ffp, is);
                            if (is->paused == 0) {
                                get_clock_sync_type(is)->paused = 1;
                                stream_update_pause2_l(ffp, player);
                            }
                        }
                        SDL_UnlockMutex(ffp->mutex_0x18);
                        is->force_refresh = 0;
                        goto LAB_0011cc10;
                    }
                    dVar25 = lVar11->pts;
                    dVar26 = ffp->f_0x3a8;
                    if (dVar25 < dVar26) {
                        dVar26 = 0.00000000;
                        ffp->f_0x3a8 = 0;
                        ffp->f_0x3b0 = 0;
                    }
                    if (((is->step != 0) || (dVar25 = dVar25 - dVar26, 0.02000000 <=dVar25))
                            || (is->is_image != '\0')) {
                        video_image_display2(ffp, player);
                        ffp->f_0x3b0 = currentTime;
                        ffp->f_0x3a8 = lVar11->pts;
                        goto LAB_0011cbc8;
                    }
                    frame_queue_peek(&is->pictq);
                    frame_queue_next(&is->pictq);
                    iVar4 = frame_queue_nb_remaining(&is->pictq);
                    if (iVar4 == 0) goto LAB_0011c9a4;
                }
                uVar7 = 0;
                frame_queue_next(&is->pictq);
                goto LAB_0011c7b0;
            }
LAB_0011c9a4:
            duration = (lVar6 - lVar9);
            if (is->is_image == '\0') {
                if ((is->audio_st != 0) || (is->viddec.packet_pending == 0))
                    goto LAB_0011cccc;
LAB_0011cce8:
                bVar2 = true;
            } else {
                if (is->f_0 != 0) {
LAB_0011cccc:
                    if (duration / fVar22 < (ffp_get_current_position_l(ffp) - ffp->cur_clip_begin_timeline))
                        goto LAB_0011cce8;
                }
                if ((is->viddec.packet_pending != 0) && (is->audclk.paused != 0)) goto LAB_0011cce8;
                if (!bVar2) goto LAB_0011c9e4;
            }
            is->viddec.packet_pending = 0;
            is->force_refresh = 0;
LAB_0011cc10:

            if (bVar2) {
                SDL_Delay(500);
                //msg_queue_put_simple1(&ffp->msg_queue, FFP_MSG_COMPLETED);
                MPTRACE("%s run video index bVar2 %d", __func__,bVar2);
                ffp_switch_to_next_clip(ffp, player->clip_id_0x58);
                player->doneVideo = 0;
            }
        } else {
            fVar22 = is->play_rate_0xa0c;
            remaining_time = 0.01000000;
            if (fVar22 <= 2.00000000) goto LAB_0011c5fc;
LAB_0011c76c:
            if (lVar6 == 0) goto LAB_0011cd4c;
LAB_0011c770:
            if (is->video_st != 0) goto LAB_0011c778;
LAB_0011c63c:
            if (is->is_image != '\0') {
                goto LAB_0011c778;
            }
LAB_0011c9e4:
            is->force_refresh = 0;
        }
        if (player == 0) goto LAB_0011c6b4;

LAB_END_DISPLAY:
        if ((player->is_0x30 == 0) ||
                (player->is_0x30->abort_request == 0 && player->isUsed_0x60 != 0)) {
            //MPTRACE("%s goto LAB_0011c528", __func__);
            goto LAB_0011c528;
        }
        SDL_LockMutex(ffp->clipState->mutext_8);
        player->is_0x30->f_0x9f4 = 0;
        SDL_CondSignal(ffp->clipState->con_9);
        SDL_UnlockMutex(ffp->clipState->mutext_8);
        player = 0;

LAB_0011c6b4:
        SDL_LockMutex(ffp->clipState->mutext_8);
        playerIndex = ffp_clip_op_get_play_ci(ffp, &player);
        if ((playerIndex < 0) || (player == 0) ||
            (is = player->is_0x30, is == 0) ||
            (player->f_0x64_100 == 0) || is->abort_request) {

            player = 0;
        } else {
            is->f_0x9f4 = 1;
        }
        SDL_UnlockMutex(ffp->clipState->mutext_8);
        //MPTRACE("%s got player: %p", __func__, player);

    }

}

void refresh_444(FFPlayer *ffp) {
    MPTRACE("%s begin", __func__);
    VideoClip *player = 0;
    int playerIndex;
    VideoState *is;
    int size;
    Frame *frame;
    int64_t pts;

    //show_ijkplayer_base_address();
    //show_ijksdl_base_address();
    while (!ffp->abort_request) {
        if (player == 0) {
            SDL_LockMutex(ffp->clipState->mutext_8);
            playerIndex = ffp_clip_op_get_play_ci(ffp, &player);
            if ((playerIndex < 0) || (player == 0) ||
                 (is = player->is_0x30, is == 0) ||
                    (player->f_0x64_100 == 0) || is->abort_request) {

                player = 0;
            } else {
                is->f_0x9f4 = 1;
            }
            SDL_UnlockMutex(ffp->clipState->mutext_8);
            if (player != 0) {
                if (!ffp->abort_request) goto LAB_0011cf0c;
                goto LAB_0011cf4c;
            }
            av_usleep(10000);
        } else {
LAB_0011cf0c:
            while (ffp->flag_0x458) {
                SDL_LockMutex(ffp->mutex_0x460);
                SDL_CondWaitTimeout(ffp->cond_0x468, ffp->mutex_0x460, 50);
                SDL_UnlockMutex(ffp->mutex_0x460);
                if (ffp->abort_request) break;
            }
LAB_0011cf4c:
            is = player->is_0x30;
            if (is != 0) {
                if (is->abort_request != 0) {
LAB_0011cf5c:
                    SDL_LockMutex(ffp->clipState->mutext_8);
                    is->f_0x9f4 = 0;
                    SDL_CondSignal(ffp->clipState->con_9);
                    SDL_UnlockMutex(ffp->clipState->mutext_8);
                    break;
                }
                if ((is->show_mode == SHOW_MODE_NONE) ||
                        (is->paused && (is->force_refresh == 0))) {
LAB_0011d074:
                    if (player->isUsed_0x60 == 0) goto LAB_0011cf5c;
                } else {
                    if (is->video_st == 0) goto LAB_0011d074;
                    while (size = frame_queue_nb_remaining(&is->pictq), size != 0) {
                        while( true ) {
                            frame = frame_queue_peek(&is->pictq);
                            pts = (int64_t) (frame->pts * 1000000.0);
                            if (pts > is->ic->start_time) {
                                pts = pts - is->ic->start_time;
                            }
                            if (frame->serial == is->videoq.serial) break;
                            frame_queue_next(&is->pictq);
                            size = frame_queue_nb_remaining(&is->pictq);
                            av_log(0,0x20,
                                   "refresh, video queue is unserial,ignore pts = %f,queue_size = %d,retry.\n",
                                   frame->pts, size);
                            if (size == 0) goto LAB_0011d054;
                        }
                        if (ffp->f_0x448 <= pts) {
                            pts = pts - player->begin_time_0x68;
                            ffp->f_0x450 = pts;
                            if (pts < 1000000) {
                                av_log(0, 0x20,
                                       "refresh, before do real video display, pts = %f,step =%d,timestamp:%ld,clip_duration:%ld!!.%p\n",
                                       frame->pts, is->step, pts, player->duration_0x78 / 1000, ffp);
                            }
                            video_image_display2(ffp, player);
                            SDL_LockMutex(ffp->mutex_0x460);
                            ffp->flag_0x458 = 1;
                            SDL_UnlockMutex(ffp->mutex_0x460);
                            frame_queue_next(&is->pictq);
                            goto LAB_0011d05c;
                        }
                        frame_queue_next(&is->pictq);
                    }
LAB_0011d054:
                    if (is->viddec.packet_pending != 0) {
                        av_log(0,0x20,"%s:current is packets all decoded","video_refresh_save_mode");
                        ffp_notify_msg1(ffp, FFP_MSG_COMPLETED);
                        is->viddec.packet_pending = 0;
                        break;
                    }
LAB_0011d05c:
                    if ((player != 0) && (player->is_0x30 != 0)) {
                        if (player->is_0x30->abort_request) goto LAB_0011cf5c;
                        goto LAB_0011d074;
                    }
                }
            }
        }
    }
}

// FFP_MERGE: stream_cycle_channel
// FFP_MERGE: toggle_full_screen
// FFP_MERGE: toggle_audio_display
// FFP_MERGE: refresh_loop_wait_event
// FFP_MERGE: event_loop
// FFP_MERGE: opt_frame_size
// FFP_MERGE: opt_width
// FFP_MERGE: opt_height
// FFP_MERGE: opt_format
// FFP_MERGE: opt_frame_pix_fmt
// FFP_MERGE: opt_sync
// FFP_MERGE: opt_seek
// FFP_MERGE: opt_duration
// FFP_MERGE: opt_show_mode
// FFP_MERGE: opt_input_file
// FFP_MERGE: opt_codec
// FFP_MERGE: dummy
// FFP_MERGE: options
// FFP_MERGE: show_usage
// FFP_MERGE: show_help_default
static int video_refresh_thread(void *arg, void *arg2)
{
    MPTRACE("%s run video index begin", __func__);
    FFPlayer *ffp = arg;
//    VideoState *is = ffp->is;
//    double remaining_time = 0.0;
//    MPTRACE("ff_vout begin loop, is=%p, ffp=%p", is, ffp);
//    while (!is->abort_request) {
//        if (remaining_time > 0.0)
//            av_usleep((int)(int64_t)(remaining_time * 1000000.0));
//        remaining_time = REFRESH_RATE;
//        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
//            video_refresh(ffp, &remaining_time);
//    }
        MPTRACE("%s run video index isSaveMode %d", __func__,ffp->isSaveMode);
    if (ffp->isSaveMode == 0) {
        refresh_not_444(ffp);
//        refresh_444(ffp);
    } else {
        refresh_444(ffp);
    }

    MPTRACE("ff_vout end");
    return 0;
}

static int lockmgr(void **mtx, enum AVLockOp op)
{
    MPTRACE("%s begin", __func__);
    switch (op) {
    case AV_LOCK_CREATE:
        *mtx = SDL_CreateMutex();
        if (!*mtx) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            return 1;
        }
        return 0;
    case AV_LOCK_OBTAIN:
        return !!SDL_LockMutex(*mtx);
    case AV_LOCK_RELEASE:
        return !!SDL_UnlockMutex(*mtx);
    case AV_LOCK_DESTROY:
        SDL_DestroyMutex(*mtx);
        return 0;
    }
    return 1;
}

// FFP_MERGE: main

/*****************************************************************************
 * end last line in ffplay.c
 ****************************************************************************/

static bool g_ffmpeg_global_inited = false;

inline static int log_level_av_to_ijk(int av_level)
{
    int ijk_level = IJK_LOG_VERBOSE;
    if      (av_level <= AV_LOG_PANIC)      ijk_level = IJK_LOG_FATAL;
    else if (av_level <= AV_LOG_FATAL)      ijk_level = IJK_LOG_FATAL;
    else if (av_level <= AV_LOG_ERROR)      ijk_level = IJK_LOG_ERROR;
    else if (av_level <= AV_LOG_WARNING)    ijk_level = IJK_LOG_WARN;
    else if (av_level <= AV_LOG_INFO)       ijk_level = IJK_LOG_INFO;
    // AV_LOG_VERBOSE means detailed info
    else if (av_level <= AV_LOG_VERBOSE)    ijk_level = IJK_LOG_INFO;
    else if (av_level <= AV_LOG_DEBUG)      ijk_level = IJK_LOG_DEBUG;
    else if (av_level <= AV_LOG_TRACE)      ijk_level = IJK_LOG_VERBOSE;
    else                                    ijk_level = IJK_LOG_VERBOSE;
    return ijk_level;
}

inline static int log_level_ijk_to_av(int ijk_level)
{
    int av_level = IJK_LOG_VERBOSE;
    if      (ijk_level >= IJK_LOG_SILENT)   av_level = AV_LOG_QUIET;
    else if (ijk_level >= IJK_LOG_FATAL)    av_level = AV_LOG_FATAL;
    else if (ijk_level >= IJK_LOG_ERROR)    av_level = AV_LOG_ERROR;
    else if (ijk_level >= IJK_LOG_WARN)     av_level = AV_LOG_WARNING;
    else if (ijk_level >= IJK_LOG_INFO)     av_level = AV_LOG_INFO;
    // AV_LOG_VERBOSE means detailed info
    else if (ijk_level >= IJK_LOG_DEBUG)    av_level = AV_LOG_DEBUG;
    else if (ijk_level >= IJK_LOG_VERBOSE)  av_level = AV_LOG_TRACE;
    else if (ijk_level >= IJK_LOG_DEFAULT)  av_level = AV_LOG_TRACE;
    else if (ijk_level >= IJK_LOG_UNKNOWN)  av_level = AV_LOG_TRACE;
    else                                    av_level = AV_LOG_TRACE;
    return av_level;
}

static void ffp_log_callback_brief(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;

    int ffplv __unused = log_level_av_to_ijk(level);
    VLOG(ffplv, IJK_LOG_TAG, fmt, vl);
}

static void ffp_log_callback_report(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;

    int ffplv __unused = log_level_av_to_ijk(level);

    va_list vl2;
    char line[1024];
    static int print_prefix = 1;

    va_copy(vl2, vl);
    // av_log_default_callback(ptr, level, fmt, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);

    ALOG(ffplv, IJK_LOG_TAG, "%s", line);
}

int ijkav_register_all(void);
void ffp_global_init()
{
    MPTRACE("%s begin", __func__);
    if (g_ffmpeg_global_inited)
        return;

    ALOGD("ijkmediaplayer version : %s", ijkmp_version());
    /* register all codecs, demux and protocols */
    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();

    ijkav_register_all();

    avformat_network_init();

    av_lockmgr_register(lockmgr);
    av_log_set_callback(ffp_log_callback_brief);

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    g_ffmpeg_global_inited = true;
}

void ffp_global_uninit()
{
    MPTRACE("%s begin", __func__);
    if (!g_ffmpeg_global_inited)
        return;

    av_lockmgr_register(NULL);

    // FFP_MERGE: uninit_opts

    avformat_network_deinit();

    g_ffmpeg_global_inited = false;
}

void ffp_global_set_log_report(int use_report)
{
    MPTRACE("%s begin", __func__);
    if (use_report) {
        av_log_set_callback(ffp_log_callback_report);
    } else {
        av_log_set_callback(ffp_log_callback_brief);
    }
}

void ffp_global_set_log_level(int log_level)
{
    MPTRACE("%s begin", __func__);
    int av_level = log_level_ijk_to_av(log_level);
    av_log_set_level(av_level);
}

static ijk_inject_callback s_inject_callback;
int inject_callback(void *opaque, int type, void *data, size_t data_size)
{
    MPTRACE("%s begin", __func__);
    if (s_inject_callback)
        return s_inject_callback(opaque, type, data, data_size);
    return 0;
}

void ffp_global_set_inject_callback(ijk_inject_callback cb)
{
    MPTRACE("%s begin", __func__);
    s_inject_callback = cb;
}

void ffp_io_stat_register(void (*cb)(const char *url, int type, int bytes))
{
    // avijk_io_stat_register(cb);
}

void ffp_io_stat_complete_register(void (*cb)(const char *url,
                                              int64_t read_bytes, int64_t total_size,
                                              int64_t elpased_time, int64_t total_duration))
{
    // avijk_io_stat_complete_register(cb);
}

static const char *ffp_context_to_name(void *ptr)
{
    return "FFPlayer";
}


static void *ffp_context_child_next(void *obj, void *prev)
{
    return NULL;
}

static const AVClass *ffp_context_child_class_next(const AVClass *prev)
{
    return NULL;
}

const AVClass ffp_context_class = {
    .class_name       = "FFPlayer",
    .item_name        = ffp_context_to_name,
    .option           = ffp_context_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .child_next       = ffp_context_child_next,
    .child_class_next = ffp_context_child_class_next,
};

static const char *ijk_version_info()
{
    return IJKPLAYER_VERSION;
}

FFPlayer *ffp_create()
{
    MPTRACE("%s begin", __func__);
    int ret;

    av_log(NULL, AV_LOG_INFO, "av_version_info: %s\n", av_version_info());
    av_log(NULL, AV_LOG_INFO, "ijk_version_info: %s\n", ijk_version_info());

    FFPlayer* ffp = (FFPlayer*) av_mallocz(sizeof(FFPlayer));
    if (!ffp)
        return NULL;

    ret = ffp_clip_op_create(ffp);
    if (ret < 0) {
        av_free(ffp);
        return NULL;
    }

    ret = ffp_audio_track_op_create(ffp);
    if (ret < 0) {
        ffp_clip_op_destory(ffp);
        ffp_audio_track_op_destroy(ffp);
        av_free(ffp);
        return NULL;
    }

    msg_queue_init(&ffp->msg_queue);
//    ffp->af_mutex = SDL_CreateMutex();
//    ffp->vf_mutex = SDL_CreateMutex();
    ffp->mutex_0x18 = SDL_CreateMutex();
    ffp->mutex_0x408_0x81_0x102 = SDL_CreateMutex();
    ffp->cond_0x20_4_8 = SDL_CreateCond();


    ffp_reset_internal(ffp);
    clip_op_queue_flush(ffp->clipState);
    MPTRACE("%s ffp->player_opts: %p", __func__, ffp->player_opts);
    ffp->av_class = &ffp_context_class;
//    ffp->meta = ijkmeta_create();

    av_opt_set_defaults(ffp);

    return ffp;
}

void ffp_destroy(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    AudioTrackEditOp *audioState;
    VideoClip *player;
    if (!ffp)
        return;

    ffp->abort_request = 1;
    audioState = ffp->audioState;
    audioState->abort_req = 1;
    SDL_CondSignal(audioState->cond);
    av_log(0, AV_LOG_INFO, "%s:wait for video_refresh_tid\n","ffp_destroy");
    SDL_WaitThread(ffp->thread_vout, 0);
    ffp->thread_vout = NULL;
    //???? chua ro thread nay la gi
//    SDL_WaitThread(*(undefined8 *)(param_1 + 0x78),0);
//    *(undefined8 *)(param_1 + 0x78) = 0;
    av_log(ffp, AV_LOG_INFO,"%s:video_refresh thread finished.\n","ffp_destroy");

    audio_track_op_close(audioState);

    for (int i = 0; i < ffp->clipState->f_2; i++) {
        player = ffp_clip_op_get_ci_at_index(ffp, i);
        if (player != NULL) {
            stream_close2(ffp, player);
        }
    }

    ffp_reset_internal(ffp);

    ffp_clip_op_destory(ffp);
    ffp_audio_track_op_destroy(ffp);
    SDL_DestroyMutexP(&ffp->mutex_0x18);
    SDL_DestroyCondP(&ffp->cond_0x20_4_8);

    msg_queue_destroy(&ffp->msg_queue);

    SDL_DestroyMutexP(&ffp->mutex_0x460);
    SDL_DestroyCondP(&ffp->cond_0x468);

    av_free(ffp);
}

void ffp_destroy_p(FFPlayer **pffp)
{
    MPTRACE("%s begin", __func__);
    if (!pffp)
        return;

    ffp_destroy(*pffp);
    *pffp = NULL;
}

static AVDictionary **ffp_get_opt_dict(FFPlayer *ffp, int opt_category)
{
    MPTRACE("%s begin", __func__);
    assert(ffp);

    switch (opt_category) {
        case FFP_OPT_CATEGORY_FORMAT:   return &ffp->format_opts;
        case FFP_OPT_CATEGORY_CODEC:    return &ffp->codec_opts;
        case FFP_OPT_CATEGORY_SWS:      return &ffp->sws_dict;
        case FFP_OPT_CATEGORY_PLAYER:   return &ffp->player_opts;
        case FFP_OPT_CATEGORY_SWR:      return &ffp->swr_opts;
        default:
            av_log(ffp, AV_LOG_ERROR, "unknown option category %d\n", opt_category);
            return NULL;
    }
}

static int app_func_event(AVApplicationContext *h, int message ,void *data, size_t size)
{
    MPTRACE("%s begin", __func__);
    if (!h || !h->opaque || !data)
        return 0;

    FFPlayer *ffp = (FFPlayer *)h->opaque;
    if (!ffp->inject_opaque)
        return 0;
    if (message == AVAPP_EVENT_IO_TRAFFIC && sizeof(AVAppIOTraffic) == size) {
        AVAppIOTraffic *event = (AVAppIOTraffic *)(intptr_t)data;
        if (event->bytes > 0) {
            ffp->stat.byte_count += event->bytes;
            SDL_SpeedSampler2Add(&ffp->stat.tcp_read_sampler, event->bytes);
        }
    } else if (message == AVAPP_EVENT_ASYNC_STATISTIC && sizeof(AVAppAsyncStatistic) == size) {
        AVAppAsyncStatistic *statistic =  (AVAppAsyncStatistic *) (intptr_t)data;
        ffp->stat.buf_backwards = statistic->buf_backwards;
        ffp->stat.buf_forwards = statistic->buf_forwards;
        ffp->stat.buf_capacity = statistic->buf_capacity;
    }
    return inject_callback(ffp->inject_opaque, message , data, size);
}

static int ijkio_app_func_event(IjkIOApplicationContext *h, int message ,void *data, size_t size)
{
    MPTRACE("%s begin", __func__);
    if (!h || !h->opaque || !data)
        return 0;

    FFPlayer *ffp = (FFPlayer *)h->opaque;
    if (!ffp->ijkio_inject_opaque)
        return 0;

    if (message == IJKIOAPP_EVENT_CACHE_STATISTIC && sizeof(IjkIOAppCacheStatistic) == size) {
        IjkIOAppCacheStatistic *statistic =  (IjkIOAppCacheStatistic *) (intptr_t)data;
        ffp->stat.cache_physical_pos      = statistic->cache_physical_pos;
        ffp->stat.cache_file_forwards     = statistic->cache_file_forwards;
        ffp->stat.cache_file_pos          = statistic->cache_file_pos;
        ffp->stat.cache_count_bytes       = statistic->cache_count_bytes;
        ffp->stat.logical_file_size       = statistic->logical_file_size;
    }

    return 0;
}

void ffp_set_frame_at_time(FFPlayer *ffp, const char *path, int64_t start_time, int64_t end_time, int num, int definition) {
    MPTRACE("%s begin", __func__);
    if (!ffp->get_img_info) {
        ffp->get_img_info = av_mallocz(sizeof(GetImgInfo));
        if (!ffp->get_img_info) {
            ffp_notify_msg3(ffp, FFP_MSG_GET_IMG_STATE, 0, -1);
            return;
        }
    }

    if (start_time >= 0 && num > 0 && end_time >= 0 && end_time >= start_time) {
        ffp->get_img_info->img_path   = av_strdup(path);
        ffp->get_img_info->start_time = start_time;
        ffp->get_img_info->end_time   = end_time;
        ffp->get_img_info->num        = num;
        ffp->get_img_info->count      = num;
        if (definition== HD_IMAGE) {
            ffp->get_img_info->width  = 640;
            ffp->get_img_info->height = 360;
        } else if (definition == SD_IMAGE) {
            ffp->get_img_info->width  = 320;
            ffp->get_img_info->height = 180;
        } else {
            ffp->get_img_info->width  = 160;
            ffp->get_img_info->height = 90;
        }
    } else {
        ffp->get_img_info->count = 0;
        ffp_notify_msg3(ffp, FFP_MSG_GET_IMG_STATE, 0, -1);
    }
}

void *ffp_set_ijkio_inject_opaque(FFPlayer *ffp, void *opaque)
{
    MPTRACE("%s begin", __func__);
    if (!ffp)
        return NULL;
    void *prev_weak_thiz = ffp->ijkio_inject_opaque;
    ffp->ijkio_inject_opaque = opaque;

    ijkio_manager_destroyp(&ffp->ijkio_manager_ctx);
    ijkio_manager_create(&ffp->ijkio_manager_ctx, ffp);
    ijkio_manager_set_callback(ffp->ijkio_manager_ctx, ijkio_app_func_event);
    ffp_set_option_int(ffp, FFP_OPT_CATEGORY_FORMAT, "ijkiomanager", (int64_t)(intptr_t)ffp->ijkio_manager_ctx);

    return prev_weak_thiz;
}

void *ffp_set_inject_opaque(FFPlayer *ffp, void *opaque)
{
    MPTRACE("%s begin", __func__);
    if (!ffp)
        return NULL;
    void *prev_weak_thiz = ffp->inject_opaque;
    ffp->inject_opaque = opaque;

    av_application_closep(&ffp->app_ctx);
    av_application_open(&ffp->app_ctx, ffp);
    ffp_set_option_int(ffp, FFP_OPT_CATEGORY_FORMAT, "ijkapplication", (int64_t)(intptr_t)ffp->app_ctx);

    ffp->app_ctx->func_on_app_event = app_func_event;
    return prev_weak_thiz;
}

void ffp_set_option(FFPlayer *ffp, int opt_category, const char *name, const char *value)
{
    MPTRACE("%s begin", __func__);
    if (!ffp)
        return;

    AVDictionary **dict = ffp_get_opt_dict(ffp, opt_category);
    av_dict_set(dict, name, value, 0);
}

void ffp_set_option_int(FFPlayer *ffp, int opt_category, const char *name, int64_t value)
{
    MPTRACE("%s begin", __func__);
    if (!ffp)
        return;

    AVDictionary **dict = ffp_get_opt_dict(ffp, opt_category);
    av_dict_set_int(dict, name, value, 0);
}

void ffp_set_overlay_format(FFPlayer *ffp, int chroma_fourcc)
{
    MPTRACE("%s begin", __func__);
    switch (chroma_fourcc) {
        case SDL_FCC__GLES2:
        case SDL_FCC_I420:
        case SDL_FCC_YV12:
        case SDL_FCC_RV16:
        case SDL_FCC_RV24:
        case SDL_FCC_RV32:
            ffp->overlay_format = chroma_fourcc;
            break;
#ifdef __APPLE__
        case SDL_FCC_I444P10LE:
            ffp->overlay_format = chroma_fourcc;
            break;
#endif
        default:
            av_log(ffp, AV_LOG_ERROR, "ffp_set_overlay_format: unknown chroma fourcc: %d\n", chroma_fourcc);
            break;
    }
}

int ffp_get_video_codec_info(VideoClip *player, char **codec_info)
{
    MPTRACE("%s begin", __func__);
    if (!codec_info || !player)
        return -1;

    // FIXME: not thread-safe
    if (player->video_codec_info) {
        *codec_info = strdup(player->video_codec_info);
    } else {
        *codec_info = NULL;
    }
    return 0;
}

int ffp_get_audio_codec_info(FFPlayer *ffp, char **codec_info)
{
    //MPTRACE("%s begin", __func__);
//    if (!codec_info)
//        return -1;

//    // FIXME: not thread-safe
//    if (ffp->audio_codec_info) {
//        *codec_info = strdup(ffp->audio_codec_info);
//    } else {
//        *codec_info = NULL;
//    }
//    return 0;

    if (codec_info != 0) {
        *codec_info = 0;
    }
    return -1;
}

static void ffp_show_dict(FFPlayer *ffp, const char *tag, AVDictionary *dict)
{
    MPTRACE("%s begin", __func__);
    AVDictionaryEntry *t = NULL;

    while ((t = av_dict_get(dict, "", t, AV_DICT_IGNORE_SUFFIX))) {
        av_log(ffp, AV_LOG_INFO, "%-*s: %-*s = %s\n", 12, tag, 28, t->key, t->value);
    }
}

#define FFP_VERSION_MODULE_NAME_LENGTH 13
static void ffp_show_version_str(FFPlayer *ffp, const char *module, const char *version)
{
    MPTRACE("%s begin", __func__);
        av_log(ffp, AV_LOG_INFO, "%-*s: %s\n", FFP_VERSION_MODULE_NAME_LENGTH, module, version);
}

static void ffp_show_version_int(FFPlayer *ffp, const char *module, unsigned version)
{
    MPTRACE("%s begin", __func__);
    av_log(ffp, AV_LOG_INFO, "%-*s: %u.%u.%u\n",
           FFP_VERSION_MODULE_NAME_LENGTH, module,
           (unsigned int)IJKVERSION_GET_MAJOR(version),
           (unsigned int)IJKVERSION_GET_MINOR(version),
           (unsigned int)IJKVERSION_GET_MICRO(version));
}

int ffp_prepare_async_l(FFPlayer *ffp, const char *file_name)
{
    MPTRACE("%s begin", __func__);
    assert(ffp);
    assert(!ffp->is);
    assert(file_name);

    if (av_stristart(file_name, "rtmp", NULL) ||
        av_stristart(file_name, "rtsp", NULL)) {
        // There is total different meaning for 'timeout' option in rtmp
        av_log(ffp, AV_LOG_WARNING, "remove 'timeout' option for rtmp.\n");
        av_dict_set(&ffp->format_opts, "timeout", NULL, 0);
    }

    /* there is a length limit in avformat */
    if (strlen(file_name) + 1 > 1024) {
        av_log(ffp, AV_LOG_ERROR, "%s too long url\n", __func__);
        if (avio_find_protocol_name("ijklongurl:")) {
            av_dict_set(&ffp->format_opts, "ijklongurl-url", file_name, 0);
            file_name = "ijklongurl:";
        }
    }

    av_log(NULL, AV_LOG_INFO, "===== versions =====\n");
    ffp_show_version_str(ffp, "ijkplayer",      ijk_version_info());
    ffp_show_version_str(ffp, "FFmpeg",         av_version_info());
    ffp_show_version_int(ffp, "libavutil",      avutil_version());
    ffp_show_version_int(ffp, "libavcodec",     avcodec_version());
    ffp_show_version_int(ffp, "libavformat",    avformat_version());
    ffp_show_version_int(ffp, "libswscale",     swscale_version());
    ffp_show_version_int(ffp, "libswresample",  swresample_version());
    av_log(NULL, AV_LOG_INFO, "===== options =====\n");
    ffp_show_dict(ffp, "player-opts", ffp->player_opts);
    ffp_show_dict(ffp, "format-opts", ffp->format_opts);
    ffp_show_dict(ffp, "codec-opts ", ffp->codec_opts);
    ffp_show_dict(ffp, "sws-opts   ", ffp->sws_dict);
    ffp_show_dict(ffp, "swr-opts   ", ffp->swr_opts);
    av_log(NULL, AV_LOG_INFO, "===================\n");

    av_opt_set_dict(ffp, &ffp->player_opts);
    if (!ffp->aout) {
        ffp->aout = ffpipeline_open_audio_output(ffp->pipeline, ffp);
        if (!ffp->aout)
            return -1;
    }

#if CONFIG_AVFILTER
    if (ffp->vfilter0) {
        GROW_ARRAY(ffp->vfilters_list, ffp->nb_vfilters);
        ffp->vfilters_list[ffp->nb_vfilters - 1] = ffp->vfilter0;
    }
#endif

    VideoState *is = stream_open(ffp, file_name, NULL);
    if (!is) {
        av_log(NULL, AV_LOG_WARNING, "ffp_prepare_async_l: stream_open failed OOM");
        return EIJK_OUT_OF_MEMORY;
    }

    ffp->is = is;
    ffp->input_filename = av_strdup(file_name);
    return 0;
}

int ffp_start_from_l(FFPlayer *ffp, long msec)
{
    MPTRACE("%s begin", __func__);
//    assert(ffp);
//    VideoState *is = ffp->is;
//    if (!is)
//        return EIJK_NULL_IS_PTR;

//    ffp->auto_resume = 1;
//    ffp_toggle_buffering(ffp, 1);
//    ffp_seek_to_l(ffp, msec);
    return 0;
}

int ffp_start_l(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
//    assert(ffp);
//    VideoState *is = ffp->is;
//    if (!is)
//        return EIJK_NULL_IS_PTR;

//    toggle_pause(ffp, 0);
//    return 0;

    ClipEditOp *clipMgr = ffp->clipState;
    VideoClip *player;
    int bVar2 = 1;
    int playerIndex = 0;
    if (0 < clipMgr->f_2) {
        do {
            player = ffp_clip_op_get_ci_at_index(ffp, playerIndex);
            bVar2 = (bVar2 && (player->is_0x30 == 0));
            playerIndex++;
        } while (playerIndex < clipMgr->f_2);

        if (!bVar2) {
            playerIndex = ffp_clip_op_get_play_ci(ffp, &player);
            if (playerIndex < 0) {
                return -1;
            } else {
                toggle_pause2(ffp, player, 0);
                return 0;
            }
        }
    }

    return -4;
}

int ffp_pause_l(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    assert(ffp);
    ClipEditOp *lVar5;
    char bVar2;
    int uVar4;
    VideoClip *lVar3;

//    if (!is)
//        return EIJK_NULL_IS_PTR;

//    toggle_pause(ffp, 1);

    lVar5 = ffp->clipState;
    bVar2 = 1;
    uVar4 = 0;
    if (0 < lVar5->f_2) {
        do {
            while( true ) {
                lVar3 = ffp_clip_op_get_ci_at_index(ffp, uVar4);
                uVar4++;
                if ((lVar3->is_0x30 == 0) || (lVar3->isUsed_0x60 == 0)) break;
                toggle_pause2(ffp, lVar3, 1);
                bVar2 = bVar2 && (lVar3->is_0x30 == 0);
                if (lVar5->f_2 <= uVar4) goto LAB_00114b8c;
            }
            bVar2 = bVar2 && (lVar3->is_0x30 == 0);
        } while (uVar4 < lVar5->f_2);
LAB_00114b8c:
        if (!bVar2) {
            return 0;
        }
    }
    return -4;
}

int ffp_is_paused_l(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
//    assert(ffp);
//    VideoState *is = ffp->is;
//    if (!is)
//        return 1;

//    return is->paused;

    char bVar2;
    int iVar3;
    VideoClip *lVar4;
    int index;
    ClipEditOp *lVar6;
    VideoClip *local_10;

    lVar6 = ffp->clipState;
    bVar2 = 1;
    index = 0;
    if (0 < lVar6->f_2) {
        do {
            lVar4 = ffp_clip_op_get_ci_at_index(ffp, index);
            bVar2 = bVar2 && (lVar4->is_0x30 == 0);
            index++;
        } while (index < lVar6->f_2);

        if ((!bVar2) && (iVar3 = ffp_clip_op_get_play_ci(ffp, &local_10), -1 < iVar3)) {
            return local_10->is_0x30->paused;
        }
    }

    return 1;
}

int ffp_stop_l(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    ClipEditOp *clipState;
    VideoClip *clipInfo;
    VideoState *is;

    assert(ffp);

    clipState = ffp->clipState;
    for (int i = 0; i < clipState->f_2; i++) {
        clipInfo = ffp_clip_op_get_ci_at_index(ffp, i);
        is = clipInfo->is_0x30;
        if (is != NULL) {
            clipInfo->is_0x30->abort_request = 1;
            SDL_LockMutex(clipInfo->mutext_0x20);
            if (is->f_0x100 == 0) {
                is->f_0x100 = 1;
                if (is->seek_req != 0) {
                    av_log(0,0x20,
                           "%s:stop the player, but last seek has not completed, so set signal,index:%d",
                           "ffp_stop_l", i);
                    is->seek_req = 0;
                    SDL_CondSignal(clipInfo->con_0x28);
                }
            }
            SDL_UnlockMutex(clipInfo->mutext_0x20);
        }
    }

    msg_queue_abort(&ffp->msg_queue);
    return 0;
}

int ffp_wait_stop_l(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    VideoClip *video;
    AudioTrackEditOp *audioState;

    assert(ffp);

//    if (ffp->is) {
//        ffp_stop_l(ffp);
//        stream_close(ffp);
//        ffp->is = NULL;
//    }
    video = ffp_clip_op_get_ci_at_index(ffp, 0);
    if (video->is_0x30 != NULL) {
        video->is_0x30->abort_request = 1;
    }
    video = ffp_clip_op_get_ci_at_index(ffp, 1);
    if (video->is_0x30 != NULL) {
        video->is_0x30->abort_request = 1;
    }

    audioState = ffp->audioState;
    ffp->abort_request = 1;
    audioState->abort_req = 1;
    SDL_CondSignal(audioState->cond);
    av_log(0, AV_LOG_INFO, "%s:wait for video_refresh_tid\n","ffp_wait_stop_l");
    SDL_WaitThread(ffp->thread_vout, 0);
    ffp->thread_vout = NULL;
    //???? chua ro thread nay la gi
//    SDL_WaitThread(*(undefined8 *)(param_1 + 0x78),0);
//    *(undefined8 *)(param_1 + 0x78) = 0;
    av_log(ffp, AV_LOG_INFO,"%s:video_refresh thread finished.\n","ffp_wait_stop_l");

    ffp_stop_l(ffp);

    av_log(ffp, AV_LOG_INFO, "%s:stream_close begin","ffp_wait_stop_l");
    audio_track_op_close(audioState);

    for (int i = 0; i < ffp->clipState->f_2; i++) {
        video = ffp_clip_op_get_ci_at_index(ffp, i);
        if (video != NULL) {
            stream_close2(ffp, video);
        }
    }
    return 0;
}

//isAccurate=b1, cancelPendingSeek=b2
int ffp_seek_to_l(FFPlayer *ffp, VideoClip *clipPlayer, int64_t msec, char b1, char b2, int param6)
{
    MPTRACE("%s begin: %d", __func__, msec);
    //assert(ffp);
    VideoState *is = clipPlayer->is_0x30;
    int64_t start_time = 0;

    if (!is)
        return EIJK_NULL_IS_PTR;

    FFPlayer *ffp2 = is->ffp;
    if (((is->seek_req != 0) || (is->f_0xdc != 0) || (is->step != 0))
            && (param6 == 0) && (is->is_image == 0)) {

        ffp2->f_0x3fc = 1;
        ffp2->f_0x3f4 = b2;
        ffp2->f_0x3f0 = clipPlayer->queue_index_0x5c;
        ffp2->f_0x3f8 = b1;
        ffp2->f_0x400 = 0;
        ffp2->f_0x3e8 = (int64_t) (msec / clipPlayer->speed_0xa0);
        return 0;
    }

    start_time = clipPlayer->begin_time_0x68 + msec;
    is->seek_pos = start_time;
    is->f_0x108 = start_time;
    is->f_0x110 = 1;
    is->seek_rel = 0;
    is->f_0xf8 = param6;
    is->f_0xdc = b1;
    is->f_0xe0 = b2;
    is->seek_flags = is->seek_flags & 0xfffffffd | 1;
    is->seek_req = 1;
    is->pause_req = 1;
    //is->paused = 0;
    is->force_refresh = 1;
    ffp2->f_0x3fc = 0;
    if (param6 == 0) {
        SDL_LockMutex(ffp2->mutex_0x408_0x81_0x102);
        ffp2->pos_0x420 = start_time;
        SDL_UnlockMutex(ffp2->mutex_0x408_0x81_0x102);
    }
    if (!is->is_image) {
        SDL_CondSignal(is->continue_read_thread);
    }

    if (ffp2->isSaveMode != 0) {
        show_ijkplayer_base_address();
        show_ijksdl_base_address();
    }

    return 0;
}

int64_t ffp_get_pos_audio_only(FFPlayer *ffp) {
    double dVar4;
    int64_t lVar3;
    AudioClip *audioFile;

    dVar4 = get_clock(&ffp->audioState->c);
    lVar3 = (int64_t) (dVar4 * 1000000.0);
    audioFile = ffp->audioState->arr[0]->head; //audio only la khi select 1 audio de add vao project
    MPTRACE("%s %f %d", __func__, dVar4, audioFile->duration);
    if ((audioFile != 0) && (audioFile->duration < lVar3)) {
        lVar3 = audioFile->duration + 10000;
    }
    return lVar3;
}

int64_t ffp_get_current_position_l(FFPlayer *ffp)
{
    //MPTRACE("%s begin", __func__);
    int64_t offset;
    int64_t tempPos;
    int64_t pos;
    int lockRst;

    lockRst = SDL_LockMutex(ffp->mutex_0x408_0x81_0x102);
    if (ffp->clock_0x410 == 0) {
        pos = ffp->cur_clip_begin_timeline;
        if (lockRst != EDEADLK) {
            SDL_UnlockMutex(ffp->mutex_0x408_0x81_0x102);
        }
        MPTRACE("%s end: %d", __func__, pos);
        return pos;
    }

    double clockValue = get_clock(ffp->clock_0x410);
    MPTRACE("%s clock %p value: %f, state=%d", __func__, ffp->clock_0x410, clockValue, ffp->clock_0x410->paused);

    if (isnanf(clockValue)) {
        tempPos = ffp->pos_0x420;
        offset = 0;
    } else {
        offset = (float) ffp->stream_offset_0x428;
        tempPos = (int64_t) (clockValue * 1000000);
    }

    pos = ffp->cur_clip_begin_timeline;
    tempPos = tempPos - ffp->f_0x430;
    if (tempPos < 0) {
        tempPos = 0;
    }

    tempPos = (int64_t) (tempPos / ffp->speed_0x440 + pos -
                            offset / ffp->speed_0x440);

    if (pos <= tempPos) {
        pos = pos + ffp->cur_clip_duration + 10000;
        if (pos > tempPos) {
            pos = tempPos;
        }
    }
    MPTRACE("%s end: %d. pos_0x420=%d, stream_offset_0x428=%d, pos_0x418=%d, f_0x430=%d, speed_0x440=%f, f_0x438=%d, tempos=%d",
            __func__, pos, ffp->pos_0x420, ffp->stream_offset_0x428, ffp->cur_clip_begin_timeline, ffp->f_0x430,
            ffp->speed_0x440, ffp->cur_clip_duration, tempPos);

    if (lockRst != EDEADLK) {
        SDL_UnlockMutex(ffp->mutex_0x408_0x81_0x102);
    }
    return pos;
}

int64_t ffp_get_duration_l(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    assert(ffp);
    int64_t rs;

    if (ffp->audio_only && ffp->audioState->arr[0]->head != NULL) {
        return ffp->audioState->arr[0]->head->duration + 1000;
    }

    rs = ffp->clipState->total_duration_6_0x30;
    if (rs < 0) {
        rs = 0;
    }
    return rs;
//    VideoState *is = ffp->is;
//    if (!is || !is->ic)
//        return 0;

//    int64_t duration = fftime_to_milliseconds(is->ic->duration);
//    if (duration < 0)
//        return 0;

//    return (long)duration;
}

long ffp_get_playable_duration_l(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    assert(ffp);
    if (!ffp)
        return 0;

    return (long)ffp->playable_duration_ms;
}

void ffp_set_loop(FFPlayer *ffp, int loop)
{
    MPTRACE("%s begin", __func__);
    assert(ffp);
    if (!ffp)
        return;
    ffp->loop = loop;
}

int ffp_get_loop(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    assert(ffp);
    if (!ffp)
        return 1;
    return ffp->loop;
}

int ffp_packet_queue_init(PacketQueue *q)
{
    MPTRACE("%s begin", __func__);
    return packet_queue_init(q);
}

void ffp_packet_queue_destroy(PacketQueue *q)
{MPTRACE("%s begin", __func__);
    return packet_queue_destroy(q);
}

void ffp_packet_queue_abort(PacketQueue *q)
{MPTRACE("%s begin", __func__);
    return packet_queue_abort(q);
}

void ffp_packet_queue_start(PacketQueue *q)
{MPTRACE("%s begin", __func__);
    return packet_queue_start(q);
}

void ffp_packet_queue_flush(PacketQueue *q)
{MPTRACE("%s begin", __func__);
    return packet_queue_flush(q);
}

int ffp_packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MPTRACE("%s video111 begin", __func__);
    return packet_queue_get(q, pkt, block, serial, 0); //fix tam, ko dung
}

int ffp_packet_queue_get_or_buffering(FFPlayer *ffp, VideoClip *player, PacketQueue *q,
                                      AVPacket *pkt, int *serial, int *finished, int *bVal)
{
    MPTRACE("%s video111 begin", __func__);
    return packet_queue_get_or_buffering(ffp, player, q, pkt, serial, finished, bVal);
}

int ffp_packet_queue_put(PacketQueue *q, AVPacket *pkt)
{MPTRACE("%s begin", __func__);
    return packet_queue_put(q, pkt);
}

bool ffp_is_flush_packet(AVPacket *pkt)
{MPTRACE("%s begin", __func__);
    if (!pkt)
        return false;

    return pkt->data == flush_pkt.data;
}

Frame *ffp_frame_queue_peek_writable(FrameQueue *f)
{MPTRACE("%s begin", __func__);
    return frame_queue_peek_writable(f);
}

void ffp_frame_queue_push(FrameQueue *f)
{MPTRACE("%s begin", __func__);
    return frame_queue_push(f);
}

//int ffp_queue_picture(FFPlayer *ffp, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
//{
//    MPTRACE("%s begin", __func__);
//    return queue_picture(ffp, src_frame, pts, duration, pos, serial);
//}

int ffp_queue_picture2(FFPlayer *ffp, VideoClip *clip, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    MPTRACE("%s begin", __func__);
    return queue_picture2(ffp, clip, src_frame, pts, duration, pos, serial);
}

int ffp_get_master_sync_type(VideoState *is)
{MPTRACE("%s begin", __func__);
    return get_master_sync_type(is);
}

double ffp_get_master_clock(VideoState *is)
{MPTRACE("%s begin", __func__);
    return get_master_clock(is);
}

void ffp_toggle_buffering_l(FFPlayer *ffp, VideoClip *player, int buffering_on)
{
    int msg;

    if (!ffp->packet_buffering)
        return;

    VideoState *is = player->is_0x30;
    if (!buffering_on) {
        if (!is->buffering_on) {
            return;
        }
        is->buffering_on = 0;
        msg = FFP_MSG_BUFFERING_END;
    } else {
        if (is->buffering_on != 0) {
            return;
        }
        is->buffering_on = 1;
        msg = FFP_MSG_BUFFERING_START;
    }

    MPTRACE("%s begin: %p, %p, %d", __func__, ffp, player, buffering_on);
    stream_update_pause2_l(ffp, player);
    ffp_notify_msg2(ffp, msg, 0);

}

void ffp_toggle_buffering(FFPlayer *ffp, VideoClip *player, int start_buffering)
{
    //MPTRACE("%s begin", __func__);
    SDL_LockMutex(ffp->mutex_0x18);
    ffp_toggle_buffering_l(ffp, player, start_buffering);
    SDL_UnlockMutex(ffp->mutex_0x18);
}

void ffp_track_statistic_l(FFPlayer *ffp, AVStream *st, PacketQueue *q, FFTrackCacheStatistic *cache)
{
    //MPTRACE("%s begin", __func__);
    assert(cache);

    if (q) {
        cache->bytes   = q->size;
        cache->packets = q->nb_packets;
    }

    if (q && st && st->time_base.den > 0 && st->time_base.num > 0) {
        cache->duration = q->duration * av_q2d(st->time_base) * 1000;
    }
}

void ffp_audio_statistic_l(FFPlayer *ffp, VideoClip *player)
{
    //MPTRACE("%s begin", __func__);
    if (player != 0) {
        VideoState *is = player->is_0x30;
        ffp_track_statistic_l(ffp, is->audio_st, &is->audioq, &ffp->stat.audio_cache);
    }
}

void ffp_video_statistic_l(FFPlayer *ffp, VideoClip *player)
{
    //MPTRACE("%s begin", __func__);
    if (player != 0) {
        VideoState *is = player->is_0x30;
        ffp_track_statistic_l(ffp, is->video_st, &is->videoq, &ffp->stat.video_cache);
    }
}

void ffp_statistic_l(FFPlayer *ffp, VideoClip *player)
{
    //MPTRACE("%s begin", __func__);
    ffp_audio_statistic_l(ffp, player);
    ffp_video_statistic_l(ffp, player);
}

void ffp_check_buffering_l(FFPlayer *ffp, VideoState *is)
{
    MPTRACE("%s begin", __func__);
//    VideoState *is            = ffp->is;
    int hwm_in_ms             = ffp->dcc.current_high_water_mark_in_ms; // use fast water mark for first loading
    int buf_size_percent      = -1;
    int buf_time_percent      = -1;
    int hwm_in_bytes          = ffp->dcc.high_water_mark_in_bytes;
    int need_start_buffering  = 0;
    int audio_time_base_valid = 0;
    int video_time_base_valid = 0;
    int64_t buf_time_position = -1;

    if(is->audio_st)
        audio_time_base_valid = is->audio_st->time_base.den > 0 && is->audio_st->time_base.num > 0;
    if(is->video_st)
        video_time_base_valid = is->video_st->time_base.den > 0 && is->video_st->time_base.num > 0;

    if (hwm_in_ms > 0) {
        int     cached_duration_in_ms = -1;
        int64_t audio_cached_duration = -1;
        int64_t video_cached_duration = -1;

        if (is->audio_st && audio_time_base_valid) {
            audio_cached_duration = ffp->stat.audio_cache.duration;
#ifdef FFP_SHOW_DEMUX_CACHE
            int audio_cached_percent = (int)av_rescale(audio_cached_duration, 1005, hwm_in_ms * 10);
            av_log(ffp, AV_LOG_DEBUG, "audio cache=%%%d milli:(%d/%d) bytes:(%d/%d) packet:(%d/%d)\n", audio_cached_percent,
                  (int)audio_cached_duration, hwm_in_ms,
                  is->audioq.size, hwm_in_bytes,
                  is->audioq.nb_packets, MIN_FRAMES);
#endif
        }

        if (is->video_st && video_time_base_valid) {
            video_cached_duration = ffp->stat.video_cache.duration;
#ifdef FFP_SHOW_DEMUX_CACHE
            int video_cached_percent = (int)av_rescale(video_cached_duration, 1005, hwm_in_ms * 10);
            av_log(ffp, AV_LOG_DEBUG, "video cache=%%%d milli:(%d/%d) bytes:(%d/%d) packet:(%d/%d)\n", video_cached_percent,
                  (int)video_cached_duration, hwm_in_ms,
                  is->videoq.size, hwm_in_bytes,
                  is->videoq.nb_packets, MIN_FRAMES);
#endif
        }

        if (video_cached_duration > 0 && audio_cached_duration > 0) {
            cached_duration_in_ms = (int)IJKMIN(video_cached_duration, audio_cached_duration);
        } else if (video_cached_duration > 0) {
            cached_duration_in_ms = (int)video_cached_duration;
        } else if (audio_cached_duration > 0) {
            cached_duration_in_ms = (int)audio_cached_duration;
        }

        if (cached_duration_in_ms >= 0) {
            buf_time_position = ffp_get_current_position_l(ffp) + cached_duration_in_ms;
            ffp->playable_duration_ms = buf_time_position;

            buf_time_percent = (int)av_rescale(cached_duration_in_ms, 1005, hwm_in_ms * 10);
#ifdef FFP_SHOW_DEMUX_CACHE
            av_log(ffp, AV_LOG_DEBUG, "time cache=%%%d (%d/%d)\n", buf_time_percent, cached_duration_in_ms, hwm_in_ms);
#endif
#ifdef FFP_NOTIFY_BUF_TIME
        MPTRACE("%s videotrack FFP_NOTIFY_BUF_TIME %s", __func__,is->filename);
        ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_TIME_UPDATE, cached_duration_in_ms, hwm_in_ms);
#endif
        }
    }

    int cached_size = is->audioq.size + is->videoq.size;
    if (hwm_in_bytes > 0) {
        buf_size_percent = (int)av_rescale(cached_size, 1005, hwm_in_bytes * 10);
#ifdef FFP_SHOW_DEMUX_CACHE
        av_log(ffp, AV_LOG_DEBUG, "size cache=%%%d (%d/%d)\n", buf_size_percent, cached_size, hwm_in_bytes);
#endif
#ifdef FFP_NOTIFY_BUF_BYTES
        MPTRACE("%s videotrack FFP_NOTIFY_BUF_BYTES %s", __func__,is->filename);
        ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_BYTES_UPDATE, cached_size, hwm_in_bytes);
#endif
    }

    int buf_percent = -1;
    if (buf_time_percent >= 0) {
        // alwas depend on cache duration if valid
        if (buf_time_percent >= 100)
            need_start_buffering = 1;
        buf_percent = buf_time_percent;
    } else {
        if (buf_size_percent >= 100)
            need_start_buffering = 1;
        buf_percent = buf_size_percent;
    }

    if (buf_time_percent >= 0 && buf_size_percent >= 0) {
        buf_percent = FFMIN(buf_time_percent, buf_size_percent);
    }
    if (buf_percent) {
#ifdef FFP_SHOW_BUF_POS
        av_log(ffp, AV_LOG_DEBUG, "buf pos=%"PRId64", %%%d\n", buf_time_position, buf_percent);
#endif
        MPTRACE("%s videotrack FFP_MSG_BUFFERING_UPDATE %s", __func__,is->filename);
        ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_UPDATE, (int)buf_time_position, buf_percent);
    }

    if (need_start_buffering) {
        if (hwm_in_ms < ffp->dcc.next_high_water_mark_in_ms) {
            hwm_in_ms = ffp->dcc.next_high_water_mark_in_ms;
        } else {
            hwm_in_ms *= 2;
        }

        if (hwm_in_ms > ffp->dcc.last_high_water_mark_in_ms)
            hwm_in_ms = ffp->dcc.last_high_water_mark_in_ms;

        ffp->dcc.current_high_water_mark_in_ms = hwm_in_ms;

        if (is->buffer_indicator_queue && is->buffer_indicator_queue->nb_packets > 0) {
            if (   (is->audioq.nb_packets >= MIN_MIN_FRAMES || is->audio_stream < 0 || is->audioq.abort_request)
                && (is->videoq.nb_packets >= MIN_MIN_FRAMES || is->video_stream < 0 || is->videoq.abort_request)) {
                //ffp_toggle_buffering(ffp, 0);
            }
        }
    }
}

int ffp_video_thread(FFPlayer *ffp, VideoClip *player)
{
    MPTRACE("%s begin: %p, %p", __func__, ffp, player);
    return ffplay_video_thread(ffp, player, 0);
}

void ffp_set_video_codec_info(FFPlayer *ffp, VideoClip *player, const char *module, const char *codec, const char *ext)
{
    MPTRACE("%s begin", __func__);
    av_freep(&player->video_codec_info);
    player->video_codec_info = av_asprintf("%s, %s, %s", module ? module : "", codec ? codec : "", ext ? ext : "");
    av_log(ffp, AV_LOG_INFO, "VideoCodec: %s\n", player->video_codec_info);
}

void ffp_set_audio_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    MPTRACE("%s begin", __func__);
//    av_freep(&ffp->audio_codec_info);
//    ffp->audio_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
//    av_log(ffp, AV_LOG_INFO, "AudioCodec: %s\n", ffp->audio_codec_info);
}

void ffp_set_subtitle_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    MPTRACE("%s begin", __func__);
    av_freep(&ffp->subtitle_codec_info);
    ffp->subtitle_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
    av_log(ffp, AV_LOG_INFO, "SubtitleCodec: %s\n", ffp->subtitle_codec_info);
}

void ffp_set_playback_rate(FFPlayer *ffp, float rate)
{
    MPTRACE("%s begin", __func__);
    if (!ffp)
        return;

    av_log(ffp, AV_LOG_INFO, "Playback rate: %f\n", rate);
    ffp->pf_playback_rate = rate;
    ffp->pf_playback_rate_changed = 1;
}

void ffp_set_playback_volume(FFPlayer *ffp, float volume)
{
    MPTRACE("%s begin", __func__);
    if (!ffp)
        return;
    ffp->pf_playback_volume = volume;
    ffp->pf_playback_volume_changed = 1;
}

int ffp_get_video_rotate_degrees(FFPlayer *ffp, VideoClip *player)
{
    MPTRACE("%s begin", __func__);
    VideoState *is = player->is_0x30;
    if (!is)
        return 0;

    int theta  = abs((int)((int64_t)round(fabs(get_rotation(is->video_st))) % 360));
    switch (theta) {
        case 0:
        case 90:
        case 180:
        case 270:
            break;
        case 360:
            theta = 0;
            break;
        default:
            ALOGW("Unknown rotate degress: %d\n", theta);
            theta = 0;
            break;
    }

    MPTRACE("%s end: %d", __func__, theta);
    return theta;
}

int ffp_set_stream_selected(FFPlayer *ffp, int stream, int selected)
{
    MPTRACE("%s begin", __func__);
    return 0;

//    VideoState        *is = ffp->is;
//    AVFormatContext   *ic = NULL;
//    AVCodecParameters *codecpar = NULL;
//    if (!is)
//        return -1;
//    ic = is->ic;
//    if (!ic)
//        return -1;

//    if (stream < 0 || stream >= ic->nb_streams) {
//        av_log(ffp, AV_LOG_ERROR, "invalid stream index %d >= stream number (%d)\n", stream, ic->nb_streams);
//        return -1;
//    }

//    codecpar = ic->streams[stream]->codecpar;

//    if (selected) {
//        switch (codecpar->codec_type) {
//            case AVMEDIA_TYPE_VIDEO:
//                if (stream != is->video_stream && is->video_stream >= 0)
//                    stream_component_close(ffp, is->video_stream);
//                break;
//            case AVMEDIA_TYPE_AUDIO:
//                if (stream != is->audio_stream && is->audio_stream >= 0)
//                    stream_component_close(ffp, is->audio_stream);
//                break;
//            case AVMEDIA_TYPE_SUBTITLE:
//                if (stream != is->subtitle_stream && is->subtitle_stream >= 0)
//                    stream_component_close(ffp, is->subtitle_stream);
//                break;
//            default:
//                av_log(ffp, AV_LOG_ERROR, "select invalid stream %d of video type %d\n", stream, codecpar->codec_type);
//                return -1;
//        }
//        return stream_component_open(ffp, NULL, stream); //sonxxx
//    } else {
//        switch (codecpar->codec_type) {
//            case AVMEDIA_TYPE_VIDEO:
//                if (stream == is->video_stream)
//                    stream_component_close(ffp, is->video_stream);
//                break;
//            case AVMEDIA_TYPE_AUDIO:
//                if (stream == is->audio_stream)
//                    stream_component_close(ffp, is->audio_stream);
//                break;
//            case AVMEDIA_TYPE_SUBTITLE:
//                if (stream == is->subtitle_stream)
//                    stream_component_close(ffp, is->subtitle_stream);
//                break;
//            default:
//                av_log(ffp, AV_LOG_ERROR, "select invalid stream %d of audio type %d\n", stream, codecpar->codec_type);
//                return -1;
//        }
//        return 0;
//    }
}

float ffp_get_property_float(FFPlayer *ffp, int id, float default_value)
{
    MPTRACE("%s begin", __func__);
    switch (id) {
        case FFP_PROP_FLOAT_VIDEO_DECODE_FRAMES_PER_SECOND:
            return ffp ? ffp->stat.vdps : default_value;
        case FFP_PROP_FLOAT_VIDEO_OUTPUT_FRAMES_PER_SECOND:
            return ffp ? ffp->stat.vfps : default_value;
        case FFP_PROP_FLOAT_PLAYBACK_RATE:
            return ffp ? ffp->pf_playback_rate : default_value;
        case FFP_PROP_FLOAT_AVDELAY:
            return ffp ? ffp->stat.avdelay : default_value;
        case FFP_PROP_FLOAT_AVDIFF:
            return ffp ? ffp->stat.avdiff : default_value;
        case FFP_PROP_FLOAT_PLAYBACK_VOLUME:
            return ffp ? ffp->pf_playback_volume : default_value;
        case FFP_PROP_FLOAT_DROP_FRAME_RATE:
            return ffp ? ffp->stat.drop_frame_rate : default_value;
        default:
            return default_value;
    }
}

void ffp_set_property_float(FFPlayer *ffp, int id, float value)
{
    MPTRACE("%s begin", __func__);
    switch (id) {
        case FFP_PROP_FLOAT_PLAYBACK_RATE:
            ffp_set_playback_rate(ffp, value);
            break;
        case FFP_PROP_FLOAT_PLAYBACK_VOLUME:
            ffp_set_playback_volume(ffp, value);
            break;
        default:
            return;
    }
}

int64_t ffp_get_property_int64(FFPlayer *ffp, int id, int64_t default_value)
{
    MPTRACE("%s begin", __func__);
    VideoClip *player = ffp_clip_op_get_ci_at_index(ffp, 0);
    VideoState *is = player->is_0x30;
    switch (id) {
        case FFP_PROP_INT64_SELECTED_VIDEO_STREAM:
            if (!ffp || !is)
                return default_value;
            return is->video_stream;
        case FFP_PROP_INT64_SELECTED_AUDIO_STREAM:
            if (!ffp || !is)
                return default_value;
            return is->audio_stream;
        case FFP_PROP_INT64_SELECTED_TIMEDTEXT_STREAM:
            if (!ffp || !is)
                return default_value;
            return is->subtitle_stream;
        case FFP_PROP_INT64_VIDEO_DECODER:
            if (!ffp)
                return default_value;
            return ffp->stat.vdec_type;
        case FFP_PROP_INT64_AUDIO_DECODER:
            return FFP_PROPV_DECODER_AVCODEC;

        case FFP_PROP_INT64_VIDEO_CACHED_DURATION:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.duration;
        case FFP_PROP_INT64_AUDIO_CACHED_DURATION:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.duration;
        case FFP_PROP_INT64_VIDEO_CACHED_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.bytes;
        case FFP_PROP_INT64_AUDIO_CACHED_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.bytes;
        case FFP_PROP_INT64_VIDEO_CACHED_PACKETS:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.packets;
        case FFP_PROP_INT64_AUDIO_CACHED_PACKETS:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.packets;
        case FFP_PROP_INT64_BIT_RATE:
            return ffp ? ffp->stat.bit_rate : default_value;
        case FFP_PROP_INT64_TCP_SPEED:
            return ffp ? SDL_SpeedSampler2GetSpeed(&ffp->stat.tcp_read_sampler) : default_value;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_BACKWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_backwards;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_FORWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_forwards;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_CAPACITY:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_capacity;
        case FFP_PROP_INT64_LATEST_SEEK_LOAD_DURATION:
            return ffp ? ffp->stat.latest_seek_load_duration : default_value;
        case FFP_PROP_INT64_TRAFFIC_STATISTIC_BYTE_COUNT:
            return ffp ? ffp->stat.byte_count : default_value;
        case FFP_PROP_INT64_CACHE_STATISTIC_PHYSICAL_POS:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_physical_pos;
       case FFP_PROP_INT64_CACHE_STATISTIC_FILE_FORWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_file_forwards;
       case FFP_PROP_INT64_CACHE_STATISTIC_FILE_POS:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_file_pos;
       case FFP_PROP_INT64_CACHE_STATISTIC_COUNT_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_count_bytes;
       case FFP_PROP_INT64_LOGICAL_FILE_SIZE:
            if (!ffp)
                return default_value;
            return ffp->stat.logical_file_size;
        default:
            return default_value;
    }
}

void ffp_set_property_int64(FFPlayer *ffp, int id, int64_t value)
{MPTRACE("%s begin", __func__);
    switch (id) {
        // case FFP_PROP_INT64_SELECTED_VIDEO_STREAM:
        // case FFP_PROP_INT64_SELECTED_AUDIO_STREAM:
        case FFP_PROP_INT64_SHARE_CACHE_DATA:
            if (ffp) {
                if (value) {
                    ijkio_manager_will_share_cache_map(ffp->ijkio_manager_ctx);
                } else {
                    ijkio_manager_did_share_cache_map(ffp->ijkio_manager_ctx);
                }
            }
            break;
        case FFP_PROP_INT64_IMMEDIATE_RECONNECT:
            if (ffp) {
                ijkio_manager_immediate_reconnect(ffp->ijkio_manager_ctx);
            }
        default:
            break;
    }
}

IjkMediaMeta *ffp_get_meta_l(FFPlayer *ffp)
{MPTRACE("%s begin", __func__);
    if (!ffp)
        return NULL;

    return ffp->meta;
}

static void release_decode_context(AudioDecodeContext *decodeCtx) {
    MPTRACE("%s begin", __func__);
    if (decodeCtx != NULL) {
        if (decodeCtx->codecCtx != NULL) {
            avcodec_close(decodeCtx->codecCtx);
            decodeCtx->codecCtx = NULL;
        }

        if (decodeCtx->fmtCtx != NULL) {
            avformat_close_input(&decodeCtx->fmtCtx);
        }

        av_free(decodeCtx);
    }
}

static void reset_track(AudioTrackInfo *track) {
    MPTRACE("%s begin", __func__);
    track->head = track->tail = NULL;
    track->size = 0;
    track->f_0x7 = 0;
    track->f_0x34 = 0;
    track->f_0xe = 1;
    track->clip_0x9 = NULL;
    track->f_0x10 = -1.0;
    av_frame_unref(track->frame_0xf);
    track->frame_0xf = NULL;
}

static void adjust_time_track(FFPlayer *ffp, AudioTrackInfo *track) {
    MPTRACE("%s begin", __func__);
    long long current_pos;
    AudioClip *pTmp;
    long long pre_end_timeline;
    long long pre_end_timeline2;
    AudioTrackEditOp *audioState = ffp->audioState;

    pTmp = track->head;
    if (pTmp != NULL) {
        pre_end_timeline = 0;
        do {
            pTmp->pre_distance_0x10 = pTmp->start_timeline - pre_end_timeline;
            pre_end_timeline = pTmp->start_timeline + pTmp->duration;
            if (track->ffp) {
                current_pos = ffp_get_current_position_l(ffp);
                pre_end_timeline2 = pTmp->start_timeline - pTmp->pre_distance_0x10;
                if (current_pos > pre_end_timeline2 && current_pos < pTmp->start_timeline) {
                    pTmp->pre_end_timeline_0x18 = current_pos;
                } else {
                    pTmp->pre_end_timeline_0x18 = pre_end_timeline2;
                }
            }
            pTmp = pTmp->next;
        } while (pTmp != NULL);
    }

    audio_track_seek(audioState, ffp_get_current_position_l(ffp));
}

int ffp_audio_track_op_add(FFPlayer *ffp, int track_index, int index_on_track,
                           const char *url,
                           long long start_timeline,
                           long long begin_file, long long end_file)

{
    MPTRACE("%s begin", __func__);
    AudioClip *clipAudio;
    char *url2;
    AudioTrackEditOp *audioState;
    AVFrame *frame;
    AudioTrackInfo *track;
    int ret;

    if (3 < track_index || index_on_track < 0 || (end_file - begin_file < 5000)) {
        av_log(ffp, AV_LOG_ERROR, "%s:audio track add op failed. track_index:%d, index:%d",
               "ffp_audio_track_op_add", track_index, index_on_track);
        return -1;
    }

    clipAudio = (AudioClip *) av_mallocz(sizeof(AudioClip));
    if (clipAudio == NULL) {
        av_log(ffp, AV_LOG_FATAL, "%s:malloc AudioClipList instance failed.", "ffp_audio_track_op_add");
        return -1;
    }

    url2 = av_strdup(url);
    clipAudio->url = url2;
    if (url2 == NULL) {
        av_log(ffp, AV_LOG_FATAL,"%s:copy file url:%s OOM","ffp_audio_track_op_add",url);
        av_free(clipAudio);
        return -1;
    }

    audioState = ffp->audioState;
    clipAudio->begin_file = begin_file;
    clipAudio->duration = end_file - begin_file;
    clipAudio->start_timeline = start_timeline;
    clipAudio->end_file = end_file;
    clipAudio->fadein_0x38_7 = 0;
    clipAudio->fadeout_0x3c = 0;
    clipAudio->next = NULL;
    clipAudio->volume_0x44 = 1.0;
    track = audioState->arr[track_index];
    audioState->f_processing_0x5c8 = 1;
    frame_queue_write_break(&audioState->frameQueue);
    SDL_LockMutex(audioState->mutex);
    clipAudio->f_0x4c = track->f_0x34;
    clipAudio->index_on_track = index_on_track;
    ret = audio_track_op_add(audioState->arr[track_index],
                             index_on_track, clipAudio);
    if (ret == 0) {
        if (track->size == 1) {
            frame = track->frame_0xf;
            audioState->f_0x20++;
            track->f_0xe = 1;
            track->clip_0x9 = NULL;
            track->f_0x10 = -1.0;
            if (frame == 0) {
                frame = av_frame_alloc();
                track->frame_0xf = frame;
            }
        }

        adjust_time_track(ffp, track);

        frame_queue_write_unbreak(&audioState->frameQueue);
        audioState->f_processing_0x5c8 = 0;
        SDL_CondSignal(audioState->cond);
        SDL_UnlockMutex(audioState->mutex);
        return 0;
    }

    frame_queue_write_unbreak(&audioState->frameQueue);
    audioState->f_processing_0x5c8 = 0;
    SDL_CondSignal(audioState->cond);
    SDL_UnlockMutex(audioState->mutex);
    av_free(clipAudio->url);
    av_free(clipAudio);
    return -1;
}

int ffp_audio_track_op_create(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    AudioTrackEditOp *audioState;
    IJKFF_Pipeline *pipeline;
    SDL_Aout *aOut;
    SDL_mutex *mutex;
    SDL_cond   *cond;
    AudioTrackInfo *p;

    audioState = (AudioTrackEditOp *) av_mallocz(sizeof(AudioTrackEditOp));
    if (audioState == 0) {
        av_log(ffp, AV_LOG_FATAL, "create AudioTrackEditOp OOM");
        return AVERROR(ENOMEM);
    }

    pipeline = ffpipeline_create_from_android(ffp);
    audioState->pipeline = pipeline;
    if (pipeline == 0) {
        av_log(ffp, AV_LOG_FATAL,"create ffpipeline failed");
        return AVERROR(ENOMEM);
    }

    aOut = ffpipeline_open_audio_output(pipeline, ffp);
    audioState->aOut_0x8c0 = aOut;
    if (aOut == 0) {
        av_log(ffp, AV_LOG_ERROR, "ffpipeline open audio output failed");
        return -1;
    }

    mutex = SDL_CreateMutex();
    audioState->mutex = mutex;
    if (mutex == 0) {
        av_log(ffp, AV_LOG_ERROR,"create mutex failed");
        return -1;
    }

    cond = SDL_CreateCond();
    audioState->cond = cond;
    if (cond == 0) {
        av_log(ffp, AV_LOG_ERROR, "create cond failed");
        return -1;
    }

    mutex = SDL_CreateMutex();
    audioState->seek_mutex = mutex;
    if (mutex == 0) {
        av_log(ffp, AV_LOG_ERROR, "create seek_mux failed");
        return -1;
    }

    mutex = SDL_CreateMutex();
    audioState->volume_fade_mutex = mutex;
    if (mutex == 0) {
        av_log(ffp, AV_LOG_ERROR,"create volume_fade_mutex failed");
        return -1;
    }

    packet_queue_init(&audioState->pktQueue);
    frame_queue_init(&audioState->frameQueue, &audioState->pktQueue, 9, 1);
    audioState->f_0x900 = 0x6f7461;
    init_clock(&audioState->c, &audioState->pktQueue.serial);
    audioState->f_0x940 = 0;
    audioState->f_0x8c8 = 1;
    audioState->audio_clock_serial = -1;
    audioState->c.paused = 1;
    audioState->af_change = 0;
    audioState->f_0x948 = 0;
    audioState->f_0x94c = 0;

    for (int i = 0; i < 4; i++) {
        p = (AudioTrackInfo *) av_mallocz(sizeof(AudioTrackInfo));
        audioState->arr[i] = p;
        p->ffp = ffp;
    }
    ffp->audioState = audioState;
    return 0;
}


int ffp_audio_track_op_cut(FFPlayer *ffp, int track_index,int index_on_track,
                           long long start_timeline, long long begin_file,
                           long long end_file)
{MPTRACE("%s begin", __func__);
    AudioClip *searchClip;
    AudioTrackEditOp *audioState;
    AudioTrackInfo *track;

    if ((3 < track_index) || (index_on_track < 0)) {
        return -1;
    }

    audioState = ffp->audioState;
    track = audioState->arr[track_index];
    audioState->f_processing_0x5c8 = 1;
    frame_queue_write_break(&audioState->frameQueue);
    SDL_LockMutex(audioState->mutex);
    if (index_on_track < 1) {
        searchClip = track->head;
    } else {
        int i = 0;
        searchClip = track->head;
        if (searchClip != NULL) {
            do {
                i++;
                searchClip = searchClip->next;
                if (i == index_on_track) break;
            } while (searchClip != NULL);
        }
    }

    if (searchClip == NULL) {
        frame_queue_write_unbreak(&audioState->frameQueue);
        audioState->f_processing_0x5c8 = 0;
        SDL_CondSignal(audioState->cond);
        SDL_UnlockMutex(audioState->mutex);
        return -1;
    }

    if (begin_file >= 0 && end_file >= 0 && end_file > begin_file) {
        searchClip->start_timeline = start_timeline;
        searchClip->begin_file = begin_file;
        searchClip->end_file = end_file;
        searchClip->duration = end_file - begin_file;

        adjust_time_track(ffp, track);
    }

    frame_queue_write_unbreak(&audioState->frameQueue);
    audioState->f_processing_0x5c8 = 0;
    SDL_CondSignal(audioState->cond);
    SDL_UnlockMutex(audioState->mutex);
    return 0;
}

void ffp_audio_track_op_destroy(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    AudioTrackInfo *track;
    AudioTrackEditOp *audioState;
    AudioClip *clip;
    AudioClip *next;
    AudioDecodeContext *decodeCtx;

    audioState = ffp->audioState;
    //for cac audio track
    for (int i = 0; i < 4; i++) {
        track = audioState->arr[i];
        clip = track->head;
        while (clip != NULL) {
            next = clip->next; //lay truoc neu ko khi free se ko lay duoc
            decodeCtx = clip->decodeCtx;
            release_decode_context(decodeCtx);
            av_freep(&clip->url);
            av_free(clip);
            clip = next;
        }

        track->head = track->tail = NULL;
        track->size = 0;
        av_frame_unref(track->frame_0xf);
        av_free(track);
    }

    //av_freep(local_20 + 0x850);
    frame_queue_destory(&audioState->frameQueue);
    packet_queue_destroy(&audioState->pktQueue);
    SDL_AoutFreeP(&audioState->aOut_0x8c0);
    ffpipeline_free_p(&audioState->pipeline);
    SDL_DestroyMutexP(&audioState->mutex);
    SDL_DestroyCondP(&audioState->cond);
    SDL_DestroyMutexP(&audioState->seek_mutex);
    SDL_DestroyMutexP(&audioState->volume_fade_mutex);

    av_freep(&ffp->audioState);
}


int ffp_audio_track_op_delete(FFPlayer *ffp, int track_index, int index_on_track)
{
    MPTRACE("%s begin: %d, %d", __func__, track_index, index_on_track);
    AudioTrackEditOp *audioState;
    AudioTrackInfo *track;
    AudioClip *clip;
    AudioClip *next;
    int ret;

    if (track_index < 4) {
        audioState = ffp->audioState;
        audioState->f_processing_0x5c8 = 1;
        frame_queue_write_break(&audioState->frameQueue);
        SDL_LockMutex(audioState->mutex);
        ret = 0;
        if (track_index < 0) { //delete all track
            int i = 0;
            do {
                track = audioState->arr[i];
                clip = track->head;
                while (clip != NULL) {
                    release_decode_context(clip->decodeCtx);
                    next = clip->next;
                    av_freep(&clip->url);
                    av_freep(&clip);
                    clip = next;
                }

                reset_track(track);
                i++;
            } while (i < 4);

            audioState->f_0x20 = 0;
            audioState->f_0x870 = 1;
        } else {
            track = audioState->arr[track_index];
            if (index_on_track < 0) { //delete all audio on track
                clip = track->head;
                while (clip != 0) {
                    release_decode_context(clip->decodeCtx);
                    next = clip->next;
                    av_freep(&clip->url);
                    av_freep(&clip);
                    clip = next;
                }
                reset_track(track);

                if (audioState->f_0x20 < 1) {
                    audioState->f_0x870 = 1;
                } else {
                    audioState->af_change = 1;
                    audioState->f_0x20--;
                }
            } else {
                ret = audio_track_op_delete(track, index_on_track);
                if (ret == 0) {
                    if (track->size == 0) {
                        audioState->af_change = 1;
                        audioState->f_0x20--;
                    }
                    adjust_time_track(ffp, track);
                }
            }
        }
        frame_queue_write_unbreak(&audioState->frameQueue);
        SDL_AoutFlushAudio(audioState->aOut_0x8c0);
        audioState->audio_buf_size = 0;
        audioState->audio_buf_index = 0;
        audioState->f_processing_0x5c8 = 0;
        SDL_CondSignal(audioState->cond);
        SDL_UnlockMutex(audioState->mutex);
        return ret;
    } else {
        return -1;
    }
}

void ffp_audio_track_set_volume(float volume, FFPlayer *ffp, int track_index, int index_on_track)
{
    MPTRACE("%s begin", __func__);
    AudioTrackEditOp *audioState;
    AudioTrackInfo *track;
    AudioClip *clip;
    int index;
    int track_count;

    if (MAX_AUDIO_TRACK <= track_index) {
        return;
    }

    audioState = ffp->audioState;
    SDL_LockMutex(audioState->volume_fade_mutex);
    if (track_index < 0) { //set for all track
        index = 0;
        track_count = 0;
        do {
            if (audioState->f_0x20 <= track_count) break;
            track = audioState->arr[index];
            if (0 < track->size) {
                track_count++;
                clip = track->head;
                while (clip != 0) {
                    clip->volume_0x44 = volume;
                    clip = clip->next;
                }
                audioState->f_0x948 = 1;
            }
            index++;
        } while (index < MAX_AUDIO_TRACK);
    } else {
        track = audioState->arr[track_index];
        if (index_on_track < 0) { //set for all clip on track
            clip = track->head;
            while (clip != 0) {
                clip->volume_0x44 = volume;
                clip = clip->next;
            }
        } else {
            //tim den vi tri clip va set volume
            clip = track->head;
            index = 0;
            while (clip != NULL) {
                if (index == index_on_track) {
                    clip->volume_0x44 = volume;
                    break;
                }
                index++;
                clip = clip->next;
            }
        }
    }
    audioState->f_0x948 = 1;
    frame_queue_flush(&audioState->frameQueue);
    audio_track_seek(audioState, ffp_get_current_position_l(ffp));
    SDL_UnlockMutex(audioState->volume_fade_mutex);
}

void ffp_android_set_volume(FFPlayer *ffp, float volume) {
    AudioTrackEditOp *audioMgr;

    if (volume != 0) {
        audioMgr = ffp->audioState;
        ffpipeline_set_volume(audioMgr->pipeline, audioMgr->aOut_0x8c0, volume, volume);
    }
}

void ffp_audio_track_fade_in_fade_out(FFPlayer *ffp, int track_index, int index_on_track,
                                      int fadein, int fadeout)
{MPTRACE("%s begin", __func__);
    AudioTrackEditOp *audioState;
    AudioTrackInfo *track;
    AudioClip *clip;
    int index;

    if (MAX_AUDIO_TRACK <= track_index) {
        return;
    }

    audioState = ffp->audioState;
    track = audioState->arr[track_index];
    SDL_LockMutex(audioState->volume_fade_mutex);
    index = 0;
    clip = track->head;
    while (clip != NULL) {
        if (index == index_on_track) {
            clip->fadein_0x38_7 = fadein;
            clip->fadeout_0x3c = fadeout;
            break;
        }
        index++;
        clip = clip->next;
    }

    frame_queue_flush(&audioState->frameQueue);
    SDL_AoutFlushAudio(audioState->aOut_0x8c0);
    audioState->audio_buf_size = 0;
    audioState->audio_buf_index = 0;
    audio_track_seek(audioState, ffp_get_current_position_l(ffp));
    SDL_UnlockMutex(audioState->volume_fade_mutex);
}

int ffp_audio_track_op_exchange(FFPlayer *ffp, int trackIdxTo, int indexOnTrackTo, int64_t startTimeline, int trackIdxFrom, int indexOnTrackFrom)
{
    int iVar1;
    int uVar5;
    AudioTrackEditOp *audioMgr;
    AudioTrackInfo *trackOld, *trackNew;
    AudioClip *clip;

    if ((trackIdxTo < MAX_AUDIO_TRACK) && (trackIdxFrom < MAX_AUDIO_TRACK)) {
        audioMgr = ffp->audioState;
        trackOld = audioMgr->arr[trackIdxFrom];
        trackNew = audioMgr->arr[trackIdxTo];
        if (indexOnTrackFrom < trackOld->size) {
            audioMgr->f_processing_0x5c8 = 1;
            frame_queue_write_break(&audioMgr->frameQueue);
            SDL_LockMutex(audioMgr->mutex);
            //remove clip ra khoi old track
            clip = audio_track_op_remove(trackOld, indexOnTrackFrom);
            if (clip != NULL) {
                if (trackOld->size == 0) {
                    audioMgr->f_0x20--;
                }
                clip->start_timeline = startTimeline;
                clip->f_0x4c = trackNew->f_0x34;
                clip->index_on_track = indexOnTrackTo;
                iVar1 = audio_track_op_add(trackNew, indexOnTrackTo, clip);
                if ((iVar1 == 0) && (trackNew->size == 1)) {
                    audioMgr->f_0x20++;
                    trackNew->f_0xe = 1;
                    trackNew->clip_0x9 = 0;
                    trackNew->f_0x10 = -1.0;
                    if (trackNew->frame_0xf == 0) {
                        trackNew->frame_0xf = av_frame_alloc();
                    }
                }

                adjust_time_track(trackOld->ffp, trackOld); //ko can seek
                adjust_time_track(trackNew->ffp, trackNew);
                uVar5 = 0;
                goto LAB_00122dbc;
            }
            goto LAB_00122db8;
        }
    }
    return -1;

LAB_00122db8:
    uVar5 = -1;
LAB_00122dbc:
    frame_queue_write_unbreak(&audioMgr->frameQueue);
    audioMgr->f_processing_0x5c8 = 0;
    SDL_CondSignal(audioMgr->cond);
    SDL_UnlockMutex(audioMgr->mutex);
    return uVar5;
}

//ffp_clip
static void clip_op_queue_flush(ClipEditOp *clipState) {
    MPTRACE("%s begin", __func__);
    int ret;
    JNIEnv *env;
    ClipInfo *p, *next;

    ret = SDL_JNI_SetupThreadEnv(&env);
    if (ret == JNI_OK) {
        SDL_LockMutex(clipState->mutext_8);
        p = clipState->head_0x18;
        while (p != NULL) {
            next = p->next;
            av_freep(&p->url);
            if (p->surface_creator != 0) {
                SDL_JNI_DeleteGlobalRefP(env, &p->surface_creator);
            }
            av_free(p);
            p = next;
        }

        clipState->head_0x18 = NULL;
        clipState->tail_0x20 = NULL;
        clipState->nb_clips_5_0x28 = 0;
        clipState->total_duration_6_0x30 = 0;
        clipState->id_gen_0x2c = 0;
        SDL_UnlockMutex(clipState->mutext_8);
    } else {
        av_log(0,0x10,"%s:SDL_JNI_SetupThreadEnv failed","clip_op_queue_flush");
    }
}

static int clip_op_queue_delete(ClipEditOp *clipState, int index)
{MPTRACE("%s begin", __func__);
    ClipInfo *clip;
    JNIEnv *env;
    int ret;
    ClipInfo *preClip;

    if (clipState == NULL || index < 0) {
        return -1;
    }

    preClip = NULL;
    if (index == 0) {
        clip = clipState->head_0x18;
        if (clip == NULL) {
            return -1;
        }

        clipState->head_0x18 = clip->next;
    } else {
        int i = 0;

        clip = clipState->head_0x18;
        while (clip != NULL) {
            preClip = clip;
            i++;
            clip = clip->next;
            if (i == index) {
                break;
            }
        }
        if (clip == NULL) {
            return -1;
        }

        preClip->next = clip->next;
    }

    if (clipState->tail_0x20 == clip) {
        clipState->tail_0x20 = preClip;
    }
    clipState->nb_clips_5_0x28--;
    clipState->total_duration_6_0x30 -= (clip->duration / clip->speed_0x40);

    //free clip
    ret = 0;
    if (clip->surface_creator != NULL) {
        ret = SDL_JNI_SetupThreadEnv(&env);
        if (ret != JNI_OK) {
            ret = 0; //van tra ve thanh cong vi van remove duoc item
            av_log(0, AV_LOG_ERROR, "%s:SDL_JNI_SetupThreadEnv failed","clip_op_queue_delete");
        } else {
            SDL_JNI_DeleteGlobalRefP(env, &clip->surface_creator);
        }
    }

    av_freep(&clip->url);
    av_free(clip);
    return ret;
}

int clip_op_queue_index_of_clip_list(ClipInfo *head, ClipInfo *clip)
{MPTRACE("%s begin", __func__);
    int index;
    ClipInfo *p;

    index = 0;
    p = head;
    while (p != NULL) {
        if (p == clip) {
            return index;
        }

        index++;
        p = p->next;
    }

    return -1; //not found
}

long long clip_op_queue_calc_start_timeline(ClipInfo *head, ClipInfo *clip)
{
    MPTRACE("%s begin", __func__);
    long long total = 0;
    ClipInfo *p;

    p = head;
    while (p != NULL && p != clip) {
        total = total + (long long) (p->duration / p->speed_0x40);
        p = p->next;
    }

    if (p == NULL) {
        total = 0;
    }

    return total;
}

void clip_op_ci_release_to_pool(FFPlayer *ffp, VideoClip *pClip) {
    MPTRACE("%s begin", __func__);
    VideoState *is;
    ClipEditOp *clipState;
    int lockRst;

    clipState = ffp->clipState;
    is = pClip->is_0x30;
    if (is != NULL) {
        is->abort_request = 1;
        //doan nay cho 1 cai gi do ket thuc, sau khi bat co abort_request kia
        lockRst = SDL_LockMutex(clipState->mutext_8);
        while (is->f_0x9f4 != 0) {
          SDL_CondWaitTimeout(clipState->con_9, clipState->mutext_8, 50);
        }
        if (lockRst != EDEADLK) { //neu this thread dang own lock o ben ngoai thi ko unlock trong nay
            SDL_UnlockMutex(clipState->mutext_8);
        }

        stream_close2(ffp, pClip);
    }
    pClip->clip_id_0x58 = -1;
    pClip->queue_index_0x5c = -1;
    pClip->isUsed_0x60 = 0;
}

void ffp_clip_update_time(FFPlayer *ffp, VideoClip *clip, int64_t begin_timeline) {
    MPTRACE("%s begin. %p begin_timeline=%ld", __func__, clip, begin_timeline);
    VideoState *is;

    SDL_LockMutex(ffp->mutex_0x408_0x81_0x102);
    if (clip == NULL || clip->is_0x30 == NULL) {
        ffp->clock_0x410 = 0;
        ffp->cur_clip_begin_timeline = ffp_get_current_position_l(ffp);
        SDL_UnlockMutex(ffp->mutex_0x408_0x81_0x102);
        MPTRACE("%s end 1", __func__);
        return;
    }

    is = clip->is_0x30;
    ffp->stream_offset_0x428 = 0;
    if (!is->is_image && is->ic != 0 && is->ic->start_time > 0) {
//        if (is->ic == 0) {
//            ffp->clock_0x410 = 0;
//            ffp->cur_clip_begin_timeline = ffp_get_current_position_l(ffp);
//            SDL_UnlockMutex(ffp->mutex_0x408_0x81_0x102);
//            MPTRACE("%s end 2", __func__);
//            return;
//        }

        ffp->stream_offset_0x428 = is->ic->start_time;
    }
    ffp->speed_0x440 = clip->speed_0xa0;
    //ffp->pf_playback_rate = clip->speed_0xa0;
    //ffp->pf_playback_rate_changed = 1;
    ffp->f_0x430 = clip->begin_time_0x68;
    ffp->pos_0x420 = is->seek_pos;
    ffp->cur_clip_begin_timeline = begin_timeline;
    ffp->cur_clip_duration = (int64_t) (clip->duration_0x78 / clip->speed_0xa0);

    Clock *c = get_clock_sync_type(is);
    if (ffp->clock_0x410 != c && ffp->clock_0x410 != 0) {
        set_clock(ffp->clock_0x410, NAN, 0);
    }
    ffp->clock_0x410 = c;
    SDL_UnlockMutex(ffp->mutex_0x408_0x81_0x102);
    MPTRACE("%s end", __func__);
}

static int clip_op_prepare(FFPlayer *ffp, int index) {
    MPTRACE("%s run video index clip_op_prepare : %d", __func__, index);
    ClipEditOp *clipState;
    VideoClip *pClip, *pClipTmp,*pClipUsed;
    VideoState *is;
    ClipInfo *currentClip, *nextClip,*nextClip2;
    int nextIndex;
    long long start_timeline;

    clipState = ffp->clipState;

    if (index >= clipState->nb_clips_5_0x28) {
        return -1;
    }

    av_log(ffp, AV_LOG_DEBUG, "%s: do prepare, clipcq_index:%d", "clip_op_prepare", index);
    if (clipState->head_0x18 == NULL) {
        for (int i = 0; i < clipState->f_2; i++) {
            pClip = ffp_clip_op_get_ci_at_index(ffp, i);
            if (pClip != NULL && pClip->clip_id_0x58 >= 0) {
                clip_op_ci_release_to_pool(ffp, pClip);
            }
        }
        ffp->cur_clip_begin_timeline = 0;
        return 0;
    }

    currentClip = clip_op_queue_get(clipState->head_0x18, index);
    if (currentClip == NULL) {
        MPTRACE("%s clip_op_queue_get return null: %d", __func__, index);
        return -1;
    }

    nextClip = NULL;
    if (clipState->nb_clips_5_0x28 > 1) {
        nextIndex = index + 1;
        if (nextIndex == clipState->nb_clips_5_0x28) {
            nextIndex = 0;
        }
        nextClip = clip_op_queue_get(clipState->head_0x18, nextIndex);
    }

    for (int i = 0; i < clipState->f_2; i++) {
        pClip = ffp_clip_op_get_ci_at_index(ffp, i);
        if (pClip->clip_id_0x58 < 0 || pClip->clip_id_0x58 == currentClip->id_0
                || (nextClip != NULL && nextClip->id_0 == pClip->clip_id_0x58)) {
             if(pClip->clip_id_0x58 == currentClip->id_0){
                pClipUsed = pClip;
             }
            continue;
        }

        //release
        clip_op_ci_release_to_pool(ffp, pClip);
    }

    //stream open
    /*
    if(currentClip != NULL){
        if(pClipUsed != NULL && currentClip->isPlay == 1){
            pClipUsed->isUsed_0x60 = 1;

        }else{
            currentClip->isPlay = 1;

        }
    }*/
    pClip = stream_open_video(ffp, currentClip, 1, 0);

    if (pClip != NULL) {
        start_timeline = clip_op_queue_calc_start_timeline(clipState->head_0x18, currentClip);
        for (int i = 0; i < clipState->f_2; i++) {
            pClipTmp = ffp_clip_op_get_ci_at_index(ffp, i);
            pClipTmp->isUsed_0x60 = (pClip == pClipTmp);
            if (pClipTmp == pClip) {
                ffp_clip_update_time(ffp, pClip, start_timeline);
                //break;
            }
        }
    }

    //nextClip = NULL; //sonxxx
    if (nextClip != NULL) {
        MPTRACE("prepare for next clip: %s", nextClip->url);
        pClip = stream_open_video(ffp, nextClip, 0,0);
        if (pClip != NULL && pClip->is_0x30 != NULL) {
            is = pClip->is_0x30;
            is->seek_pos = pClip->begin_time_0x68;
            is->f_0x108 = pClip->begin_time_0x68;
            is->f_0x110 = 1;
            is->seek_rel = 0;
            is->f_0xf8 = 1;
            is->f_0xdc = 1;
            is->f_0xe0 = 0;
            is->seek_flags = is->seek_flags & 0xfffffffd | 1;
            //is->seek_req = 1; //sonxxx
            is->pause_req = 1;
            is->ffp->f_0x3fc = 0;

            if (is->is_image == 0) {
                SDL_CondSignal(is->continue_read_thread);
            }

            if (is->ffp->isSaveMode != 0) {
                show_ijkplayer_base_address();
                show_ijksdl_base_address();
            }

            if (is->is_image) {
                frame_queue_flush(&is->pictq);
                toggle_pause2(ffp, pClip, 1);
            }
        }
    }
    return 0;
}

VideoClip *ffp_clip_info_create()
{MPTRACE("%s begin", __func__);
    VideoClip *info;

    info = av_mallocz(sizeof (VideoClip));
    if (info != 0) {
        info->clip_id_0x58 = -1;
        info->queue_index_0x5c = -1;
        info->mutext_0x20 = SDL_CreateMutex();
        info->mutext_0x90 = SDL_CreateMutex();
        info->con_0x28 = SDL_CreateCond();
        info->speed_0xa0 = 1.0;
    }
    return info;
}

static int ffp_clip_op_create_l(ClipEditOp *clipState)
{MPTRACE("%s begin", __func__);
    VideoClip *info;
    const char *sdl_err_des;

    clipState->f_2 = 2;
    info = ffp_clip_info_create();
    clipState->info[0] = info;
    if (info == 0) {
        return -1;
    }

    info = ffp_clip_info_create();
    clipState->info[1] = info;
    if (info == 0) {
        return -1;
    }

    clipState->head_0x18 = 0;
    clipState->tail_0x20 = 0;
    clipState->nb_clips_5_0x28 = 0;
    clipState->total_duration_6_0x30 = 0;
    clipState->f_7 = 0;
    clipState->mutext_8 = SDL_CreateMutex();
    if (clipState->mutext_8 == 0) {
        sdl_err_des = SDL_GetError();
        av_log(0, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", sdl_err_des);
        return AVERROR(ENOMEM);
    }

    clipState->con_9 = SDL_CreateCond();
    if (clipState->con_9 == 0) {
        sdl_err_des = SDL_GetError();
        av_log(0, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", sdl_err_des);
        return AVERROR(ENOMEM);
    }
    return 0;
}

void ffp_clip_op_destory_ci_pool(FFPlayer *ffp)
{MPTRACE("%s begin", __func__);
    ClipEditOp *clipState;
    VideoClip *cInfo;

    clipState = ffp->clipState;
    for (int i = 0; i < clipState->f_2; i++) {
        cInfo = ffp_clip_op_get_ci_at_index(ffp, i);
        if (cInfo != NULL) {
            SDL_DestroyMutexP(&cInfo->mutext_0x20);
            SDL_DestroyMutexP(&cInfo->mutext_0x90);
            SDL_DestroyCondP(&cInfo->con_0x28);
            free(cInfo);
            clipState->info[i] = NULL;
        }
    }
}

void ffp_clip_op_destory(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    ClipEditOp *clipState;

    ffp_clip_op_destory_ci_pool(ffp);

    clipState = ffp->clipState;
    clip_op_queue_flush(clipState);
    if (clipState->mutext_8 != NULL) {
        SDL_DestroyMutex(clipState->mutext_8);
    }
    if (clipState->con_9 != NULL) {
        SDL_DestroyCond(clipState->con_9);
    }

    av_freep(&ffp->clipState);
}

int ffp_clip_op_create(FFPlayer *ffp)
{MPTRACE("%s begin", __func__);
    ClipEditOp *clipState;
    int ret;

    clipState = (ClipEditOp *) av_mallocz(sizeof (ClipEditOp));
    if (clipState == NULL) {
        return AVERROR(ENOMEM);
    }

    ffp->clipState = clipState;
    ret = ffp_clip_op_create_l(clipState);
    if (ret < 0) {
        ffp_clip_op_destory(ffp);
        return ret;
    }
    return 0;
}

int ffp_clip_op_cut(FFPlayer *ffp, int index, long long begin_file, long long end_file)
{MPTRACE("%s begin", __func__);
    ClipInfo *clip;
    long long old_dur;
    int ret;
    ClipEditOp *clipState;

    clipState = ffp->clipState;
    SDL_LockMutex(clipState->mutext_8);
    clip = clip_op_queue_get(clipState->head_0x18, index);
    if (clip != NULL && (end_file - begin_file) >= 100000) {
        old_dur = clip->duration;
        clip->begin_file = begin_file;
        clip->end_file = end_file;
        clip->duration = end_file - begin_file;

        //tinh lai total duration cua toan bo clips
        clipState->total_duration_6_0x30 = clipState->total_duration_6_0x30 -
                (old_dur / clip->speed_0x40) +
                (clip->duration / clip->speed_0x40);
        ret = 0;
    } else {
        ret = -1;
    }
    SDL_UnlockMutex(clipState->mutext_8);
    return ret;
}

int ffp_clip_op_copy(FFPlayer *ffp, int index, jobject surface_creator)
{
    MPTRACE("%s begin", __func__);
    int ret = 0;
    ClipInfo *clip;
    ClipInfo *clone_clip;
    JNIEnv *env;
    ClipEditOp *clipState;

    clipState = ffp->clipState;
    SDL_LockMutex(clipState->mutext_8);
    clip = clip_op_queue_get(clipState->head_0x18, index);
    if (clip != NULL) {
        clone_clip = (ClipInfo *) av_mallocz(sizeof (ClipInfo));
        if (clone_clip != NULL) {
            *clone_clip = *clip;
            clone_clip->url = strdup(clip->url);
            clone_clip->id_0 = clipState->id_gen_0x2c;
            ret = SDL_JNI_SetupThreadEnv(&env);
            if (ret != JNI_OK) {
                av_log(0,AV_LOG_ERROR,"%s:SDL_JNI_SetupThreadEnv failed","ffp_clip_op_copy");
                if (clone_clip->url != NULL) {
                    av_freep(&clone_clip->url);
                }
                av_free(clone_clip);
                ret = -1;
            } else {
                clone_clip->surface_creator = (*env)->NewGlobalRef(env, surface_creator);

                clone_clip->next = clip->next;
                clip->next = clone_clip;
                if (clipState->tail_0x20 == clip) {
                    clipState->tail_0x20 = clone_clip;
                }

                clipState->id_gen_0x2c++;
                clipState->nb_clips_5_0x28++;
                clipState->total_duration_6_0x30 += clone_clip->duration / clone_clip->speed_0x40;
                    MPTRACE("%s run video index : %d", __func__, index+1);
                clip_op_prepare(ffp, index + 1);
            }
        } else {
            av_log(0,AV_LOG_FATAL,"%s:alloc mem failed","ffp_clip_op_copy");
            ret = AVERROR(ENOMEM);
        }
    } else {
        ret = -1;
    }

    SDL_UnlockMutex(clipState->mutext_8);
    return ret;
}

VideoClip *ffp_clip_op_get_ci_at_index(FFPlayer *ffp, int index)
{
    //MPTRACE("%s begin, index: %d", __func__, index);
    if ((index < 0) || (ffp->clipState->f_2 <= index)) {
        return NULL;
    }
    return ffp->clipState->info[index];
}

static int clip_op_queue_insert(FFPlayer *ffp, int index, const char *url,
                                void *surface_creator,
                                long long begin_file, long long end_file, int is_image)
{
    MPTRACE("%s begin", __func__);
    int ret;
    ClipInfo *clip;
    ClipInfo *preClip;
    ClipEditOp *clipState;
    JNIEnv     *env     = NULL;
    VideoClip *video;

    clipState = ffp->clipState;

    if (index < 0 || index > clipState->nb_clips_5_0x28) {
        av_log(0, AV_LOG_ERROR,"%s %d:index=%d, nb_clips=%d","clip_op_queue_insert",0xa5, index,
               clipState->nb_clips_5_0x28);
        return -1;
    }

    ret = SDL_JNI_SetupThreadEnv(&env);
    if (ret != JNI_OK) {
        av_log(0,AV_LOG_ERROR,"%s %d:SDL_JNI_SetupThreadEnv failed","clip_op_queue_insert",0xab);
        return -1;
    }

    clip = (ClipInfo *) av_mallocz(sizeof (ClipInfo));
    if (clip == NULL) {
        av_log(0,AV_LOG_ERROR,"%s %d:malloc cliplist failed","clip_op_queue_insert",0xb4);
//        if (clipState != NULL) {
//            if (clipState->info[1] != 0) {
//                av_freep(&clipState->info[1]);
//            }
//            av_freep(&clipState->info[0]);
//        }
        return -1;
    }

    clip->surface_creator = (*env)->NewGlobalRef(env, surface_creator);
    if (!is_image) {
        clip->begin_file = begin_file;
        clip->end_file = end_file;
        clip->duration = end_file - begin_file;
    } else {
        clip->begin_file = 0;
        clip->end_file = end_file - begin_file;
        clip->duration = end_file - begin_file;
    }
    clip->speed_0x40 = 1.0;
    clip->url = av_strdup(url);
    clip->id_0 = clipState->id_gen_0x2c;
    clip->is_image_4 = is_image;
    clip->volume = 1.0;
    clip->volume2 = 1.0;
    if (index == clipState->nb_clips_5_0x28) { //them vao cuoi
        if (clipState->tail_0x20 == 0) {
            clipState->head_0x18 = clipState->tail_0x20 = clip;
        } else {
            clipState->tail_0x20->next = clip;
            clipState->tail_0x20 = clip;
        }
    } else {
        if (index == 0) { //insert vao dau
            clip->next = clipState->head_0x18;
            clipState->head_0x18 = clip;
        } else { //insert vao giua
            preClip = clipState->head_0x18;
            for (int i = 0; i < index - 1; i++) {
                preClip = preClip->next;
            }
            clip->next = preClip->next;
            preClip->next = clip;
        }
    }
    clipState->nb_clips_5_0x28++;
    clipState->id_gen_0x2c++;
    clipState->total_duration_6_0x30 += clip->duration / clip->speed_0x40;
    for (int i = 0; i < clipState->f_2; i++) {
        video = ffp_clip_op_get_ci_at_index(ffp, i);
        if (video != NULL && video->queue_index_0x5c >= index) {
            video->queue_index_0x5c++;
        }
    }

    return 0;
}

int ffp_clip_op_insert(FFPlayer *ffp, int index, const char *url,
                       void *surface_creator,
                       long long begin_file, long long end_file, int isImage)
{
    MPTRACE("%s video111 begin", __func__);
    int ret;
    ClipEditOp *clipState;

    LOGD("ffp_clip_op_insert begin: %p, %d, %s, %p, %lld, %lld, %d", ffp, index, url, surface_creator, begin_file, end_file, isImage);
    //LOGD("clipState: %p", ffp->clipState);
    //LOGD("mutext_8: %p", ffp->clipState->mutext_8);
    clipState = ffp->clipState;
    SDL_LockMutex(clipState->mutext_8);
    ret = clip_op_queue_insert(ffp, index, url, surface_creator, begin_file, end_file, isImage);
    SDL_UnlockMutex(clipState->mutext_8);
    LOGD("ffp_clip_op_insert end: %d", ret);
    return ret;
}


int ffp_clip_op_delete(FFPlayer *ffp, int index)
{
    MPTRACE("%s begin", __func__);
    int ret;
    VideoClip *vClip;
    ClipEditOp *clipState;

    clipState = ffp->clipState;
    SDL_LockMutex(clipState->mutext_8);
    if (index != -10000) {
        if (index < 0 || index >= clipState->nb_clips_5_0x28) {
            ret = -1;
        } else {
            ret = clip_op_queue_delete(clipState, index);
            if (ret > -1 && clipState->f_2 > 0) {
                int i = 0;
                do {
                    vClip = ffp_clip_op_get_ci_at_index(ffp, i);
                    if ((vClip != 0) && vClip->queue_index_0x5c > index) {
                        vClip->queue_index_0x5c--;
                    }
                    i++;
                } while (clipState->f_2 > i);
            }
        }
    } else {
        //delete all clip
        for (int i = 0; i < clipState->nb_clips_5_0x28; i++) {
            clip_op_queue_delete(clipState, i);
        }
    }

    SDL_UnlockMutex(clipState->mutext_8);
    return ret;
}

int ffp_clip_op_get_backup_ci(FFPlayer *ffp, VideoClip **param_2)
{
    VideoClip *lVar2;
    VideoClip *lVar3;

    lVar3 = ffp->clipState->info[0];
    lVar2 = ffp->clipState->info[1];
    if (lVar3->isUsed_0x60) {
        *param_2 = lVar2;
        return 1;
    }

    if (lVar2->isUsed_0x60) {
        *param_2 = lVar3;
        return 0;
    }

    *param_2 = 0;
    return -1;
}

void ffp_set_playmode(FFPlayer *ffp, int mode)
{
    MPTRACE("%s begin", __func__);
    if (ffp != NULL) {
//        ffp->show_mode = mode;
        ffp->f_0x3a0 = mode;
    }
}

int ffp_get_playmode(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    if (ffp != 0) {
//        return ffp->show_mode;
        return ffp->f_0x3a0;
    }
    return -1;
}

void ffp_set_playback_rate2(float speed, FFPlayer *ffp, int index)
{
    MPTRACE("%s begin", __func__);
    ClipInfo *clip;
    long long pos;
    ClipEditOp *clipState;
    float realSpeed;
    VideoClip *video;

    if (ffp != 0) {
        clipState = ffp->clipState;
        realSpeed = speed > 2.0 ? 2.0 : speed;
        SDL_LockMutex(clipState->mutext_8);
        //ffp->fps_now = 1/speed;
        MPTRACE("%s playback index realSpeed %f speed %f",__func__, __func__,realSpeed,speed);

        clip = clip_op_queue_get(clipState->head_0x18, index);

        if (clip != 0) {

            clipState->total_duration_6_0x30 = clipState->total_duration_6_0x30 -
                    (clip->duration / clip->speed_0x40) +
                    (clip->duration / realSpeed);

            clip->speed_0x40 = realSpeed;
            clip->speed_cfg = speed;
            clip->change_rate = 1;
        }

        ffp_clip_op_get_play_ci(ffp, &video);
        if ((video != 0) && (video->queue_index_0x5c == index)) {
                MPTRACE("%s playback index clip->speed_0x40 %f total_duration_6_0x30 %f", __func__,clip->speed_0x40,clipState->total_duration_6_0x30);
            ffp->pf_playback_rate = speed;
            ffp->pf_playback_rate_changed = 1;
            video->speed_0xa0 = realSpeed;
            video->changeVideo = 1;
            video->is_0x30->play_rate_0xa0c = realSpeed;
            ffp->speed_0x440 = realSpeed;

            ffp->cur_clip_duration = (int64_t) ((video->end_time_0x70 - video->begin_time_0x68) / realSpeed);

        }

        pos = ffp_get_current_position_l(ffp);
        ffp_audio_seek_to(ffp, pos);
        SDL_UnlockMutex(clipState->mutext_8);
    }
}

ClipInfo *clip_op_queue_get(ClipInfo *head, int index)
{
    ClipInfo *p;
    int i;

    MPTRACE("%s begin", __func__);
    p = head;
    i = 0;
    do {
        if (i == index) {
            break;
        }
        i++;
        p = p->next;
    } while (p != NULL);
    return p;
}

void clip_op_queue_set_volume(ClipEditOp *clipMgr, int index, float vLeft, float vRight)
{
    ClipInfo *p;
    int i;

    MPTRACE("%s begin", __func__);
    p = clipMgr->head_0x18;
    if (p == NULL) {
        return;
    }

    i = 0;
    do {
        if (i == index) {
            p->volume = vLeft;
            p->volume2 = vRight;
            break;
        }
        if (index == -1) { //set for all video
            p->volume = vLeft;
            p->volume2 = vRight;
        }
        i++;
        p = p->next;
    } while (p != NULL);
}

int ffp_clip_op_get_play_ci(FFPlayer *ffp, VideoClip **video)
{
    //MPTRACE("%s begin", __func__);
    VideoClip *vclip;

    vclip = ffp->clipState->info[0];
    if (vclip->isUsed_0x60) {
        *video = vclip;
        return 0;
    }

    vclip = ffp->clipState->info[1];
    if (vclip->isUsed_0x60) {
        *video = vclip;
        return 1;
    }

    *video = NULL;
    return -1;
}

int ffp_audio_seek_to(FFPlayer *ffp, long long pos)
{
    MPTRACE("%s begin", __func__);
    audio_track_seek(ffp->audioState, pos);
    return 0;
}

int ffp_prepare_async2_l(FFPlayer *ffp, int videoIndex)
{
    MPTRACE("%s begin", __func__);
                MPTRACE("%s run video index videoIndex : %d", __func__, videoIndex);
    int ret;

//    assert(ffp);
    //assert(!ffp->is);

    av_log(NULL, AV_LOG_INFO, "===== versions =====\n");
    ffp_show_version_str(ffp, "ijkplayer",      ijk_version_info());
    ffp_show_version_str(ffp, "FFmpeg",         av_version_info());
    ffp_show_version_int(ffp, "libavutil",      avutil_version());
    ffp_show_version_int(ffp, "libavcodec",     avcodec_version());
    ffp_show_version_int(ffp, "libavformat",    avformat_version());
    ffp_show_version_int(ffp, "libswscale",     swscale_version());
    ffp_show_version_int(ffp, "libswresample",  swresample_version());
    av_log(NULL, AV_LOG_INFO, "===== options =====\n");
    ffp_show_dict(ffp, "player-opts", ffp->player_opts);
    ffp_show_dict(ffp, "format-opts", ffp->format_opts);
    ffp_show_dict(ffp, "codec-opts ", ffp->codec_opts);
    ffp_show_dict(ffp, "sws-opts   ", ffp->sws_dict);
    ffp_show_dict(ffp, "swr-opts   ", ffp->swr_opts);
    av_log(NULL, AV_LOG_INFO, "===================\n");

#if CONFIG_AVFILTER
    MPTRACE("%s: use avfilter", __func__);
    if (ffp->vfilter0) {
        GROW_ARRAY(ffp->vfilters_list, ffp->nb_vfilters);
        ffp->vfilters_list[ffp->nb_vfilters - 1] = ffp->vfilter0;
    }
#endif

    MPTRACE("qqq %s ffp->is_audio_0x100: %d", __func__, ffp->audio_only);
    if (ffp->audio_only == 0) { //video
        SDL_LockMutex(ffp->clipState->mutext_8);
            MPTRACE("%s run video index videoIndex : %d", __func__, videoIndex);
        ret = clip_op_prepare(ffp, videoIndex);
        //MPTRACE("qqq SDL_UnlockMutex");
        SDL_UnlockMutex(ffp->clipState->mutext_8);
        if (ret < 0) {
            av_log(ffp, 0x10, "%s:clip_op_prepare failed","ffp_prepare_async2_l");
            return -1;
        }

        ClipInfo *clip = clip_op_queue_get(ffp->clipState->head_0x18, videoIndex);
        if (clip == NULL) {
            MPTRACE("%s:get_start_time_at_clip failed", "ffp_prepare_async2_l");
            av_log(ffp, 0x10,"%s:get_start_time_at_clip failed", "ffp_prepare_async2_l");
            return -1;
        }

        if (clip->f_0x20 < 0) {
            MPTRACE("%s:get_start_time_at_clip failed", "ffp_prepare_async2_l");
            av_log(ffp, 0x10,"%s:get_start_time_at_clip failed", "ffp_prepare_async2_l");
            return -1;
        }

        audio_track_seek(ffp->audioState, 0);
                        MPTRACE("%s run video index call video_refresh_thread : %d", __func__, videoIndex);
        ffp->thread_vout = SDL_CreateThreadEx(&ffp->_thread_vout, video_refresh_thread, ffp, 0, "ff_vout");
        if (ffp->thread_vout == NULL) {
            return -1;
        }
        ffp->audioState->thread_0x5d0 = SDL_CreateThreadEx(&ffp->audioState->thread_0x5d8, audio_track_decode_thread, ffp, 0,
                                            "audio_track_decode");
        return ffp->audioState->thread_0x5d0 == 0 ? -1 : 0;
    } else {
        ffp->audioState->thread_0x5d0 = SDL_CreateThreadEx(&ffp->audioState->thread_0x5d8, audio_track_decode_thread, ffp, 0,
                                            "audio_track_decode");
        return ffp->audioState->thread_0x5d0 == 0 ? -1 : 0;
    }
}

int ffp_attach_to_clip(FFPlayer *ffp, int videoIndex) {
    MPTRACE("%s begin: %d", __func__, videoIndex);
    int ret;
    ClipEditOp *clipMgr = ffp->clipState;

    SDL_LockMutex(clipMgr->mutext_8);
                MPTRACE("%s run video index videoIndex 9927 : %d", __func__, videoIndex);
    ret = clip_op_prepare(ffp, videoIndex);
    SDL_UnlockMutex(clipMgr->mutext_8);
    MPTRACE("%s end: %d", __func__, ret);
    return ret;
}

int ffp_is_seeking(FFPlayer *ffp)
{
    MPTRACE("%s begin", __func__);
    int uVar1;
    int _uVar1;

    VideoClip *player = 0;
    ffp_clip_op_get_play_ci(ffp, &player);
    if (player == NULL) {
        return 0;
    }

    VideoState *is = player->is_0x30;
    if (is == 0) {
        return 0;
    }

    if ((is->seek_req == 0) || (is->f_0xf8 != 0)) {
        _uVar1 = ffp->f_0x3fc;
        if (ffp->f_0x3fc != 0) {
            _uVar1 = 0;
            uVar1 = 0;
            if (ffp->f_0x3f0 == player->queue_index_0x5c) goto LAB_00117c54;
        }
    } else {
        if (ffp->f_0x3fc == 0) {
            return 1;
        }
        _uVar1 = 1;
        uVar1 = 1;
        if (ffp->f_0x3f0 == player->queue_index_0x5c) {
LAB_00117c54:
            if (ffp->f_0x400 == 0) {
                uVar1 = 1;
            }
            return uVar1;
        }
    }
    return _uVar1;
}


int ffp_update_attach_clips(FFPlayer *ffp, int videoIdx)
{
    MPTRACE("%s begin. video index: %d", __func__, videoIdx);
    int iVar1;
    int uVar2;
    ClipEditOp *lVar3;
    VideoClip *local_10;

    lVar3 = ffp->clipState;
    local_10 = 0;
    SDL_LockMutex(lVar3->mutext_8);
                MPTRACE("%s run video index videoIdx 9987 : %d", __func__, videoIdx);
    uVar2 = clip_op_prepare(ffp, videoIdx);
    iVar1 = ffp_clip_op_get_play_ci(ffp, &local_10);
    if (-1 < iVar1) {
        local_10->is_0x30->pause_req = 0;
        ffp->auto_resume = 1;
        stream_update_pause2_l(ffp, local_10);
    }
    SDL_UnlockMutex(lVar3->mutext_8);

    MPTRACE("%s end. ret: %d", __func__, uVar2);
    return uVar2;
}

int ffp_clip_op_exchange(FFPlayer *ffp, int index1, int index2)
{
    int uVar1;
    int uVar2;
    int bVar3;
    int iVar4;
    ClipInfo *lVar5;
    ClipInfo *lVar6;
    int64_t uVar7;
    int uVar8;
    int bVar9;
    ClipInfo *lVar10;
    ClipInfo *lVar11;
    ClipInfo *lVar12;
    ClipInfo *lVar13;
    ClipInfo *lVar14;
    ClipEditOp *lVar15;
    ClipInfo *tmp;
    int uVar16;
    VideoClip *local_10;

    lVar15 = ffp->clipState;
    local_10 = 0;
    SDL_LockMutex(lVar15->mutext_8);
    iVar4 = ffp_clip_op_get_play_ci(ffp, &local_10);
    if (iVar4 < 0) {
        uVar16 = -1;
        SDL_UnlockMutex(lVar15->mutext_8);
    } else {
        if (index1 != index2 && index1 >= 0 && index2 >= 0) {
            if (index1 < index2) {
                uVar2 = index2;
                uVar1 = index1;
            } else {
                uVar2 = index1;
                uVar1 = index2;
            }
            if ((uVar2 < lVar15->nb_clips_5_0x28) && (lVar15->head_0x18 != 0)) {
                uVar8 = 0;
                lVar11 = 0;
                lVar10 = 0;
                lVar14 = 0;
                lVar12 = 0;
                bVar9 = 0;
                bVar3 = 0;
                lVar6 = lVar15->head_0x18;
                do {
                    lVar13 = lVar6;
                    if (uVar1 == uVar8) {
LAB_00117314:
                        bVar3 = 1;
                        if (bVar9) {
LAB_001172ec:
                            lVar5 = lVar6->next;
                            lVar12 = lVar13;
                            if (bVar9 && bVar3) {
                                if ((lVar14 != 0) && (lVar13 != 0)) {
                                    if (index1 < index2) {
                                        if (lVar10 == 0) {
                                            lVar15->head_0x18 = lVar13->next;
                                        } else {
                                            lVar10->next = lVar13->next;
                                        }
                                        lVar6 = lVar14->next;
                                        lVar14->next = lVar13;
                                        lVar13->next = lVar6;
                                        if (lVar6 == 0) {
                                            lVar15->tail_0x20 = lVar13;
                                        }
                                    } else {
                                        lVar6 = lVar14->next;
                                        lVar11->next = lVar6;
                                        if (lVar6 == 0) {
                                            lVar15->tail_0x20 = lVar11;
                                        }
                                        if (lVar10 == 0) {
                                            lVar14->next = lVar13;
                                            lVar15->head_0x18 = lVar14;
                                        } else {
                                            tmp = lVar10->next;
                                            lVar10->next = lVar14;
                                            lVar14->next = tmp;
                                        }
                                    }
                                    uVar16 = 0;
                                                    MPTRACE("%s run video index index2 10086 : %d", __func__, index2);
                                    clip_op_prepare(ffp, index2);
                                    goto LAB_00117380;
                                }
                                break;
                            }
                        } else {
LAB_0011731c:
                            lVar5 = lVar6->next;
                            lVar11 = lVar6;
                            lVar12 = lVar13;
                        }
                    } else {
                        lVar13 = lVar12;
                        if (uVar2 != uVar8) {
                            if (bVar3) goto LAB_00117314;
                            lVar10 = lVar6;
                            if (bVar9) goto LAB_001172ec;
                            goto LAB_0011731c;
                        }
                        bVar9 = 1;
                        lVar14 = lVar6;
                        if (bVar3) goto LAB_00117314;
                        bVar9 = 1;
                        lVar5 = lVar6->next;
                        lVar10 = lVar6;
                    }
                    uVar8 = uVar8 + 1;
                    lVar6 = lVar5;
                } while (lVar5 != 0);
            }
        }

        uVar16 = -1;
LAB_00117380:
        uVar7 = ffp_get_current_position_l(ffp);
        ffp_audio_seek_to(ffp, uVar7);
        SDL_UnlockMutex(lVar15->mutext_8);
    }

    return uVar16;
}

void ffp_clip_set_volume(FFPlayer *ffp, int index, float left, float right) {
    VideoClip *local_10;

    if (ffp->clipState == 0) {
        return;
    }

    clip_op_queue_set_volume(ffp->clipState, index, left, right);

    ffp_clip_op_get_play_ci(ffp, &local_10);
    if ((local_10 == 0) || (local_10->pipeline_0x10 == 0) ||
            (index != local_10->queue_index_0x5c && index != -1)) {
        ffp_clip_op_get_backup_ci(ffp, &local_10);
    } else {
        ffpipeline_set_volume(local_10->pipeline_0x10, local_10->aout_0, left, right);
        local_10->volume_0x88 = left;
        local_10->volume2_0x8c = right;
        ffp_clip_op_get_backup_ci(ffp, &local_10);
    }

    if ((local_10 != 0) && (local_10->pipeline_0x10 != 0) &&
            (index == local_10->queue_index_0x5c || index == -1)) {

        ffpipeline_set_volume(local_10->pipeline_0x10, local_10->aout_0, left, right);
        local_10->volume_0x88 = left;
        local_10->volume2_0x8c = right;
    }
}

void ffp_set_savemode(FFPlayer *ffp, char b) {
    ffp->isSaveMode = b;
    ffp->av_sync_type = 1;
    //ffp->f_0xf8 = 1;

    ffp->f_0x448 = -1;

    ffp->mutex_0x460 = SDL_CreateMutex();
    ffp->cond_0x468 = SDL_CreateCond();
}

void ffp_request_render(FFPlayer *ffp, int64_t pts) {
    ffp->f_0x448 = pts;
    SDL_LockMutex(ffp->mutex_0x460);
    ffp->flag_0x458 = 0;
    SDL_CondSignal(ffp->cond_0x468);
    SDL_UnlockMutex(ffp->mutex_0x460);
}

void ffp_audio_only_complete(FFPlayer *ffp) {
    AudioTrackEditOp *audioMgr;

    if (ffp->audio_only && ffp->audioState != 0) {
        audioMgr = ffp->audioState;
        set_clock(&audioMgr->c, get_clock(&audioMgr->c), audioMgr->c.serial);
//        set_clock(&audioMgr->c, 0, audioMgr->c.serial);

        audioMgr->c.paused = 1;
        audioMgr->f_0x8c8 = 1;
        SDL_AoutPauseAudio(audioMgr->aOut_0x8c0, 1);
    }
}
