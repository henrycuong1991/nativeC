#include "ff_audio.h"
#include "ijkplayer.h"

static void release_decode_context(AudioDecodeContext *decodeCtx) {
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

int audio_track_op_add(AudioTrackInfo *track, int index, AudioClip *clip)
{
    int size;

    size = track->size;
    if (size < index) {
        return -1;
    }

    if (size == index) { //insert vao cuoi list
        if (track->tail == NULL) {
            track->head = clip;
            track->tail = clip;
        } else {
            track->tail->next = clip;
            track->tail = clip;
        }
    } else {
        if (index < 1) { //insert vao dau
            clip->next = track->head;
            track->head = clip;
        } else { //insert vao giua
            //tim den vi tri index - 1
            AudioClip *p;
            p = track->head;
            for (int i = 0; i < index - 1; i++) {
                p = p->next;
            }
            clip->next = p->next;
            p->next = clip;
        }
    }

    track->size++;
    //*(int *)((longlong)param_1 + 0x34) = *(int *)((longlong)param_1 + 0x34) + 1;
    track->f_0x34++;
    return 0;
}

void audio_track_op_close(AudioTrackEditOp *audioState)
{
    if (audioState != NULL) {
        audioState->abort_req = 1;
        SDL_CondSignal(audioState->cond);
        packet_queue_abort(&audioState->pktQueue);
        frame_queue_signal(&audioState->frameQueue);
        SDL_WaitThread(audioState->thread_0x5d0, NULL);
        audioState->thread_0x5d0 = NULL;
        SDL_AoutCloseAudio(audioState->aOut_0x8c0);
        packet_queue_flush(&audioState->pktQueue);
    }
}

int audio_accurate_seek(AVFormatContext *fmtCtx,int stream_index, int64_t pos)
{
    AVRational timebase;
    int ret;
    AVPacket pkt;
    int64_t pts, pkt_duration;

    ret = avformat_seek_file(fmtCtx, -1, INT64_MIN, pos, pos, AVSEEK_FLAG_ANY);
    if (ret < 0) {
        av_log(0,0x10,"%s:%d accurate_seek failed %d","accurate_seek",0x96,ret);
        return ret;
    }

    av_init_packet(&pkt);
    timebase = fmtCtx->streams[stream_index]->time_base;
    do {
        while( true ) {
            ret = av_read_frame(fmtCtx, &pkt);
            if (ret < 0) {
                return ret;
            }
            if (pkt.stream_index == stream_index) break;
            av_packet_unref(&pkt);
        }

        pts = pkt.pts;
        if (pts == AV_NOPTS_VALUE) {
            pts = pkt.dts;
        }
        pkt_duration = pkt.duration;
        pts = av_rescale_q(pts, timebase, AV_TIME_BASE_Q);
        pkt_duration = av_rescale_q(pkt_duration, timebase, AV_TIME_BASE_Q);
        av_packet_unref(&pkt);
    } while (pts + pkt_duration < pos);

    return 0;
}

int audio_track_swap_to_next_clip(AudioTrackInfo *track)
{
    AudioClip *curClip;
    AudioClip *nextClip;
    AudioDecodeContext *decodeCtx;
    long long pos;
    long long pre_end_timeline;
    int ret;

    curClip = track->clip_0x9;
    if (curClip == 0) {
        av_log(0, AV_LOG_ERROR, "%s:%d clip is null","audio_track_swap_to_next_clip", 0x72);
        return -1;
    }

    MPTRACE("%s: %p, %p, %p, %p \n", __func__, curClip, track->tail, curClip->next, curClip->decodeCtx);
    curClip->decodeCtx->f_0x34 = 0;
    if (track->ffp) {
        pos = ffp_get_current_position_l(track->ffp);
        pre_end_timeline = curClip->start_timeline - curClip->pre_distance_0x10;
        if (pre_end_timeline < pos && pos < curClip->start_timeline) {
            curClip->pre_end_timeline_0x18 = pos;
        } else {
            curClip->pre_end_timeline_0x18 = pre_end_timeline;
        }
    }

    if (track->tail != curClip) {
        nextClip = curClip->next;
        decodeCtx = nextClip->decodeCtx;
        av_log(0,AV_LOG_ERROR,"%s:%d swap to next clip","audio_track_swap_to_next_clip",0x84);
        if (decodeCtx != NULL && decodeCtx->fmtCtx != NULL) {
            ret = audio_accurate_seek(decodeCtx->fmtCtx, decodeCtx->stream_index_3, nextClip->begin_file);
            if (ret >= 0) {
                if (track->ffp) {
                    pos = ffp_get_current_position_l(track->ffp);
                    pre_end_timeline = nextClip->start_timeline - nextClip->pre_distance_0x10;
                    if (pre_end_timeline < pos && pos < nextClip->start_timeline) {
                        nextClip->pre_end_timeline_0x18 = pos;
                    } else {
                        nextClip->pre_end_timeline_0x18 = pre_end_timeline;
                    }
                }
                track->clip_0x9 = nextClip;
                return 0;
            }
        }

        av_log(0,AV_LOG_ERROR,"%s:%d avformat_seek_file failed","audio_track_swap_to_next_clip",0x88);
        return -1;
    }

    track->f_0xe = 1;
    track->clip_0x9 = NULL;
    av_log(0,AV_LOG_ERROR,"%s:%d meet last clip","audio_track_swap_to_next_clip",0x7f);
    return 0;
}

int audio_track_op_delete(AudioTrackInfo *track, int index)
{
    AudioClip *p;
    AudioClip *pre;

    if (index < 1) { //delete head
        if (track->head == NULL) {
            return -1;
        }
        p = track->head;

        //switch next clip neu clip bi delete la current clip
        if (track->clip_0x9 == p) {
            audio_track_swap_to_next_clip(track);
        }

        track->head = track->head->next;
        if (track->head == NULL) { //empty list
            track->tail = NULL;
        }
    } else {
        pre = track->head;
        for (int i = 0; i < index - 1; i++) {
            if (pre == NULL) {
                return -1;
            }
            pre = pre->next;
        }
        p = pre->next;
        if (p == NULL) {
            return -1;
        }

        if (track->clip_0x9 == p) {
            audio_track_swap_to_next_clip(track);
        }

        pre->next = p->next;
        if (pre->next == NULL) { //phan tu cuoi
            track->tail = pre;
        }
    }

    track->size--;

    //free p
    release_decode_context(p->decodeCtx);
    p->next = 0;
    av_freep(&p->url);
    av_freep(&p);

    if (track->size == 0) {
        track->f_0xe = 1;
        track->clip_0x9 = NULL;
    }

    return 0;
}

AudioClip *audio_track_op_remove(AudioTrackInfo *track, int index)
{
    AudioClip *p;
    AudioClip *pre;

    if (index < 1) { //delete head
        if (track->head == NULL) {
            return NULL;
        }
        p = track->head;

        //switch next clip neu clip bi delete la current clip
        if (track->clip_0x9 == p) {
            audio_track_swap_to_next_clip(track);
        }

        track->head = track->head->next;
        if (track->head == NULL) { //empty list
            track->tail = NULL;
        }
    } else {
        pre = track->head;
        for (int i = 0; i < index - 1; i++) {
            if (pre == NULL) {
                return NULL;
            }
            pre = pre->next;
        }
        p = pre->next;
        if (p == NULL) {
            return NULL;
        }

        if (track->clip_0x9 == p) {
            audio_track_swap_to_next_clip(track);
        }

        pre->next = p->next;
        if (pre->next == NULL) { //phan tu cuoi
            track->tail = pre;
        }
    }

    track->size--;
    p->next = 0;
    if (track->size == 0) {
        track->f_0xe = 1;
        track->clip_0x9 = NULL;
    }

    return p;
}

void audio_track_seek(AudioTrackEditOp *audioState, long long pos)
{
    MPTRACE("qqq %s begin: %d", __func__, pos);
    SDL_LockMutex(audioState->seek_mutex);
    audioState->f_0x870 = 0;
    audioState->f_0x8a0 = 1;
    audioState->f_0x8b0 = pos;
    SDL_CondSignal(audioState->cond);
    SDL_UnlockMutex(audioState->seek_mutex);
}
