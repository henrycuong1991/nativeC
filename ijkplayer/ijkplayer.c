/*
 * ijkplayer.c
 *
 * Copyright (c) 2013 Bilibili
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

#include "ijkplayer.h"
#include "ijkplayer_internal.h"
#include "ijkversion.h"

#define MP_RET_IF_FAILED(ret) \
    do { \
        int retval = ret; \
        if (retval != 0) return (retval); \
    } while(0)

#define MPST_RET_IF_EQ_INT(real, expected, errcode) \
    do { \
        if ((real) == (expected)) return (errcode); \
    } while(0)

#define MPST_RET_IF_EQ(real, expected) \
    MPST_RET_IF_EQ_INT(real, expected, EIJK_INVALID_STATE)

inline static void ijkmp_destroy(IjkMediaPlayer *mp)
{
    if (!mp)
        return;

    ffp_destroy_p(&mp->ffplayer);
    if (mp->msg_thread) {
        SDL_WaitThread(mp->msg_thread, NULL);
        mp->msg_thread = NULL;
    }

    pthread_mutex_destroy(&mp->mutex);
    pthread_cond_destroy(&mp->cond);

    freep((void**)&mp->data_source);
    memset(mp, 0, sizeof(IjkMediaPlayer));
    freep((void**)&mp);
}

inline static void ijkmp_destroy_p(IjkMediaPlayer **pmp)
{
    if (!pmp)
        return;

    ijkmp_destroy(*pmp);
    *pmp = NULL;
}

void ijkmp_global_init()
{
    ffp_global_init();
}

void ijkmp_global_uninit()
{
    ffp_global_uninit();
}

void ijkmp_global_set_log_report(int use_report)
{
    ffp_global_set_log_report(use_report);
}

void ijkmp_global_set_log_level(int log_level)
{
    ffp_global_set_log_level(log_level);
}

void ijkmp_global_set_inject_callback(ijk_inject_callback cb)
{
    ffp_global_set_inject_callback(cb);
}

const char *ijkmp_version()
{
    return IJKPLAYER_VERSION;
}

void ijkmp_io_stat_register(void (*cb)(const char *url, int type, int bytes))
{
    ffp_io_stat_register(cb);
}

void ijkmp_io_stat_complete_register(void (*cb)(const char *url,
                                                int64_t read_bytes, int64_t total_size,
                                                int64_t elpased_time, int64_t total_duration))
{
    ffp_io_stat_complete_register(cb);
}

void ijkmp_change_state_l(IjkMediaPlayer *mp, int new_state)
{
    MPTRACE("%s [%p]: %d -> %d\n", __func__, mp, mp->mp_state, new_state);
    mp->mp_state = new_state;
    ffp_notify_msg1(mp->ffplayer, FFP_MSG_PLAYBACK_STATE_CHANGED);
}

IjkMediaPlayer *ijkmp_create(int (*msg_loop)(void*))
{
    IjkMediaPlayer *mp = (IjkMediaPlayer *) mallocz(sizeof(IjkMediaPlayer));
    if (!mp)
        goto fail;

    mp->ffplayer = ffp_create();
    if (!mp->ffplayer)
        goto fail;

    mp->msg_loop = msg_loop;

    ijkmp_inc_ref(mp);
    pthread_mutex_init(&mp->mutex, NULL);
    pthread_cond_init(&mp->cond, NULL);

    return mp;

    fail:
    ijkmp_destroy_p(&mp);
    return NULL;
}

void *ijkmp_set_inject_opaque(IjkMediaPlayer *mp, void *opaque)
{
    assert(mp);

    MPTRACE("%s(%p)\n", __func__, opaque);
    //void *prev_weak_thiz = ffp_set_inject_opaque(mp->ffplayer, opaque);
    ijkmp_set_option_int(mp, IJKMP_OPT_CATEGORY_FORMAT, "ijkinject-opaque", opaque);
    MPTRACE("%s()=void\n", __func__);
    return opaque;
}

void ijkmp_set_frame_at_time(IjkMediaPlayer *mp, const char *path, int64_t start_time, int64_t end_time, int num, int definition)
{
    assert(mp);

    MPTRACE("%s(%s,%lld,%lld,%d,%d)\n", __func__, path, start_time, end_time, num, definition);
    ffp_set_frame_at_time(mp->ffplayer, path, start_time, end_time, num, definition);
    MPTRACE("%s()=void\n", __func__);
}


void *ijkmp_set_ijkio_inject_opaque(IjkMediaPlayer *mp, void *opaque)
{
    assert(mp);

    MPTRACE("%s(%p)\n", __func__, opaque);
    void *prev_weak_thiz = ffp_set_ijkio_inject_opaque(mp->ffplayer, opaque);
    MPTRACE("%s()=void\n", __func__);
    return prev_weak_thiz;
}

void ijkmp_set_option(IjkMediaPlayer *mp, int opt_category, const char *name, const char *value)
{
    assert(mp);

    // MPTRACE("%s(%s, %s)\n", __func__, name, value);
    pthread_mutex_lock(&mp->mutex);
    ffp_set_option(mp->ffplayer, opt_category, name, value);
    pthread_mutex_unlock(&mp->mutex);
    // MPTRACE("%s()=void\n", __func__);
}

void ijkmp_set_option_int(IjkMediaPlayer *mp, int opt_category, const char *name, int64_t value)
{
    assert(mp);

    // MPTRACE("%s(%s, %"PRId64")\n", __func__, name, value);
    pthread_mutex_lock(&mp->mutex);
    ffp_set_option_int(mp->ffplayer, opt_category, name, value);
    //if (strcmp(name, ""))
    pthread_mutex_unlock(&mp->mutex);
    // MPTRACE("%s()=void\n", __func__);
}

int ijkmp_get_video_codec_info(IjkMediaPlayer *mp, char **codec_info)
{
    VideoClip *player;
    //assert(mp);

    MPTRACE("%s\n", __func__);
    player = ffp_clip_op_get_ci_at_index(mp->ffplayer, 0);
    if (player == NULL) {
        return -1;
    }

    pthread_mutex_lock(&mp->mutex);
    int ret = ffp_get_video_codec_info(mp->ffplayer, codec_info);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("%s()=%d\n", __func__, ret);
    return ret;
}

int ijkmp_get_audio_codec_info(IjkMediaPlayer *mp, char **codec_info)
{
    assert(mp);

    MPTRACE("%s\n", __func__);
    pthread_mutex_lock(&mp->mutex);
    int ret = ffp_get_audio_codec_info(mp->ffplayer, codec_info);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("%s()=void\n", __func__);
    return ret;
}

void ijkmp_set_playback_rate(IjkMediaPlayer *mp, float rate)
{
    assert(mp);

    MPTRACE("%s(%f)\n", __func__, rate);
    pthread_mutex_lock(&mp->mutex);
    ffp_set_playback_rate(mp->ffplayer, rate);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("%s()=void\n", __func__);
}

void ijkmp_set_playback_volume(IjkMediaPlayer *mp, float volume)
{
    assert(mp);

    MPTRACE("%s(%f)\n", __func__, volume);
    pthread_mutex_lock(&mp->mutex);
    ffp_set_playback_volume(mp->ffplayer, volume);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("%s()=void\n", __func__);
}

int ijkmp_set_stream_selected(IjkMediaPlayer *mp, int stream, int selected)
{
    assert(mp);

    MPTRACE("%s(%d, %d)\n", __func__, stream, selected);
    pthread_mutex_lock(&mp->mutex);
    int ret = ffp_set_stream_selected(mp->ffplayer, stream, selected);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("%s(%d, %d)=%d\n", __func__, stream, selected, ret);
    return ret;
}

float ijkmp_get_property_float(IjkMediaPlayer *mp, int id, float default_value)
{
    assert(mp);

    pthread_mutex_lock(&mp->mutex);
    float ret = ffp_get_property_float(mp->ffplayer, id, default_value);
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

void ijkmp_set_property_float(IjkMediaPlayer *mp, int id, float value)
{
    assert(mp);

    pthread_mutex_lock(&mp->mutex);
    ffp_set_property_float(mp->ffplayer, id, value);
    pthread_mutex_unlock(&mp->mutex);
}

int64_t ijkmp_get_property_int64(IjkMediaPlayer *mp, int id, int64_t default_value)
{
    assert(mp);

    pthread_mutex_lock(&mp->mutex);
    int64_t ret = ffp_get_property_int64(mp->ffplayer, id, default_value);
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

void ijkmp_set_property_int64(IjkMediaPlayer *mp, int id, int64_t value)
{
    assert(mp);

    pthread_mutex_lock(&mp->mutex);
    ffp_set_property_int64(mp->ffplayer, id, value);
    pthread_mutex_unlock(&mp->mutex);
}

IjkMediaMeta *ijkmp_get_meta_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MPTRACE("%s\n", __func__);
    IjkMediaMeta *ret = ffp_get_meta_l(mp->ffplayer);
    MPTRACE("%s()=void\n", __func__);
    return ret;
}

void ijkmp_shutdown_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MPTRACE("ijkmp_shutdown_l()\n");
    if (mp->ffplayer) {
        ffp_stop_l(mp->ffplayer);
        ffp_wait_stop_l(mp->ffplayer);
    }
    MPTRACE("ijkmp_shutdown_l()=void\n");
}

void ijkmp_shutdown(IjkMediaPlayer *mp)
{
    return ijkmp_shutdown_l(mp);
}

void ijkmp_inc_ref(IjkMediaPlayer *mp)
{
    assert(mp);
    __sync_fetch_and_add(&mp->ref_count, 1);
}

void ijkmp_dec_ref(IjkMediaPlayer *mp)
{
    if (!mp)
        return;

    int ref_count = __sync_sub_and_fetch(&mp->ref_count, 1);
    if (ref_count == 0) {
        MPTRACE("ijkmp_dec_ref(): ref=0\n");
        ijkmp_shutdown(mp);
        ijkmp_destroy_p(&mp);
    }
}

void ijkmp_dec_ref_p(IjkMediaPlayer **pmp)
{
    if (!pmp)
        return;

    ijkmp_dec_ref(*pmp);
    *pmp = NULL;
}

static int ijkmp_set_data_source_l(IjkMediaPlayer *mp, const char *url)
{
    assert(mp);
    assert(url);

    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_IDLE);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_INITIALIZED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_PREPARED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_STARTED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_PAUSED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_COMPLETED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_STOPPED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_ERROR);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_END);

    freep((void**)&mp->data_source);
    mp->data_source = strdup(url);
    if (!mp->data_source)
        return EIJK_OUT_OF_MEMORY;

    ijkmp_change_state_l(mp, MP_STATE_INITIALIZED);
    return 0;
}

int ijkmp_set_data_source(IjkMediaPlayer *mp, const char *url)
{
    assert(mp);
    assert(url);
    MPTRACE("ijkmp_set_data_source(url=\"%s\")\n", url);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_set_data_source_l(mp, url);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_set_data_source(url=\"%s\")=%d\n", url, retval);
    return retval;
}

static int ijkmp_msg_loop(void *arg, void *arg2)
{
    IjkMediaPlayer *mp = arg;
    int ret = mp->msg_loop(arg);
    return ret;
}

static int ijkmp_prepare_async_l(IjkMediaPlayer *mp, int index)
{
    assert(mp);
    assert(mp->ffplayer);

    //MPTRACE("%s av_opt_set_dict %p", __func__, mp->ffplayer->player_opts);
    av_opt_set_dict(mp->ffplayer, &mp->ffplayer->player_opts);
    //MPTRACE("%s av_opt_set_dict end %p", __func__, mp->ffplayer->player_opts);

    MPTRACE("%s current state: %d", __func__, mp->mp_state);
    if (mp->ffplayer->audio_only != 0) { //audio
        ijkmp_change_state_l(mp,MP_STATE_INITIALIZED);
    }

    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_IDLE);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_INITIALIZED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_PREPARED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_STARTED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_PAUSED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_COMPLETED);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_STOPPED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_ERROR);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_END);

    //assert(mp->data_source);
    ijkmp_change_state_l(mp, MP_STATE_ASYNC_PREPARING);

    MPTRACE("%s msg_queue_start, current state: %d", __func__, mp->mp_state);
    msg_queue_start(&mp->ffplayer->msg_queue);

    // released in msg_loop
    ijkmp_inc_ref(mp);
    mp->msg_thread = SDL_CreateThreadEx(&mp->_msg_thread, ijkmp_msg_loop, mp, 0, "ff_msg_loop");
    // msg_thread is detached inside msg_loop
    // TODO: 9 release weak_thiz if pthread_create() failed;

    int retval = ffp_prepare_async2_l(mp->ffplayer, index);
    if (retval < 0) {
        ijkmp_change_state_l(mp, MP_STATE_ERROR);
        return retval;
    }

    if (mp->ffplayer->audio_only != 0) { //audio
        ijkmp_change_state_l(mp,MP_STATE_PREPARED);
    }

    MPTRACE("%s success, current state: %d", __func__, mp->mp_state);
    return 0;
}

int ijkmp_prepare_async(IjkMediaPlayer *mp, int index)
{
    assert(mp);
    MPTRACE("ijkmp_prepare_async()\n");
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_prepare_async_l(mp, index);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_prepare_async()=%d\n", retval);
    return retval;
}

static int ikjmp_chkst_start_l(int mp_state)
{
    MPST_RET_IF_EQ(mp_state, MP_STATE_IDLE);
    MPST_RET_IF_EQ(mp_state, MP_STATE_INITIALIZED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PREPARED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_STARTED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PAUSED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_COMPLETED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_STOPPED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_ERROR);
    MPST_RET_IF_EQ(mp_state, MP_STATE_END);

    return 0;
}

static int ikjmp_chkst_addvideo_l(int mp_state)
{
//    MPST_RET_IF_EQ(mp_state, MP_STATE_IDLE);
//    MPST_RET_IF_EQ(mp_state, MP_STATE_INITIALIZED);
//    MPST_RET_IF_EQ(mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PREPARED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_STARTED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PAUSED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_COMPLETED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_STOPPED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_ERROR);
    MPST_RET_IF_EQ(mp_state, MP_STATE_END);

    return 0;
}

static int ijkmp_start_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MP_RET_IF_FAILED(ikjmp_chkst_start_l(mp->mp_state));

    ffp_remove_msg(mp->ffplayer, FFP_REQ_START);
    ffp_remove_msg(mp->ffplayer, FFP_REQ_PAUSE);
    ffp_notify_msg1(mp->ffplayer, FFP_REQ_START);

    return 0;
}

int ijkmp_start(IjkMediaPlayer *mp)
{
    assert(mp);
    MPTRACE("ijkmp_start()\n");
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_start_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_start()=%d\n", retval);
    return retval;
}

static int ikjmp_chkst_pause_l(int mp_state)
{
    MPST_RET_IF_EQ(mp_state, MP_STATE_IDLE);
    MPST_RET_IF_EQ(mp_state, MP_STATE_INITIALIZED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PREPARED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_STARTED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PAUSED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_COMPLETED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_STOPPED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_ERROR);
    MPST_RET_IF_EQ(mp_state, MP_STATE_END);

    return 0;
}

static int ijkmp_pause_l(IjkMediaPlayer *mp)
{
//    assert(mp);

    MPTRACE("ijkmp_pause_l: current state = %d\n", mp->mp_state);
    MP_RET_IF_FAILED(ikjmp_chkst_pause_l(mp->mp_state));

    ffp_remove_msg(mp->ffplayer, FFP_REQ_START);
    ffp_remove_msg(mp->ffplayer, FFP_REQ_PAUSE);
    ffp_notify_msg1(mp->ffplayer, FFP_REQ_PAUSE);

    return 0;
}

int ijkmp_pause(IjkMediaPlayer *mp)
{
    assert(mp);
    MPTRACE("ijkmp_pause()\n");
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_pause_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_pause()=%d\n", retval);
    return retval;
}

static int ijkmp_stop_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_IDLE);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_INITIALIZED);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_PREPARED);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_STARTED);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_PAUSED);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_COMPLETED);
    // MPST_RET_IF_EQ(mp->mp_state, MP_STATE_STOPPED);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_ERROR);
    MPST_RET_IF_EQ(mp->mp_state, MP_STATE_END);

    ffp_remove_msg(mp->ffplayer, FFP_REQ_START);
    ffp_remove_msg(mp->ffplayer, FFP_REQ_PAUSE);
    int retval = ffp_stop_l(mp->ffplayer);
    if (retval < 0) {
        return retval;
    }

    ijkmp_change_state_l(mp, MP_STATE_STOPPED);
    return 0;
}

int ijkmp_stop(IjkMediaPlayer *mp)
{
    assert(mp);
    MPTRACE("ijkmp_stop()\n");
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_stop_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_stop()=%d\n", retval);
    return retval;
}

bool ijkmp_is_playing(IjkMediaPlayer *mp)
{
    assert(mp);
    if (mp->mp_state == MP_STATE_PREPARED ||
        mp->mp_state == MP_STATE_STARTED) {
        return true;
    }

    return false;
}

static int ikjmp_chkst_seek_l(int mp_state)
{
    MPST_RET_IF_EQ(mp_state, MP_STATE_IDLE);
    MPST_RET_IF_EQ(mp_state, MP_STATE_INITIALIZED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PREPARED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_STARTED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_PAUSED);
    // MPST_RET_IF_EQ(mp_state, MP_STATE_COMPLETED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_STOPPED);
    MPST_RET_IF_EQ(mp_state, MP_STATE_ERROR);
    MPST_RET_IF_EQ(mp_state, MP_STATE_END);

    return 0;
}

int ijkmp_seek_to_l(IjkMediaPlayer *mp, int64_t msec, int i, char b1, char b2)
{
//    assert(mp);

    MPTRACE("ijkmp_seek_to_l: current state = %d\n", mp->mp_state);
    MP_RET_IF_FAILED(ikjmp_chkst_seek_l(mp->mp_state));

//    mp->seek_req = 1;
//    mp->seek_msec = msec;
    ffp_remove_msg(mp->ffplayer, FFP_REQ_SEEK);
//    ffp_notify_msg2(mp->ffplayer, FFP_REQ_SEEK, (int)msec);
    AVMessage msg;
    msg_init_msg(&msg);
    msg.what = FFP_REQ_SEEK;
    msg.arg1 = i;
    msg.arg2 = b1;
    msg.arg3 = b2;
    msg.longArg1 = msec;
    msg_queue_put(&mp->ffplayer->msg_queue, &msg);
    // TODO: 9 64-bit long?

    return 0;
}

int ijkmp_seek_to(IjkMediaPlayer *mp, int64_t msec, int i, char b1, char b2)
{
//    assert(mp);
    MPTRACE("ijkmp_seek_to(%ld)\n", msec);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_seek_to_l(mp, msec, i, b1, b2);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_seek_to(%ld)=%d\n", msec, retval);

    return retval;
}

int ijkmp_get_state(IjkMediaPlayer *mp)
{
    return mp->mp_state;
}

static int64_t ijkmp_get_current_position_l(IjkMediaPlayer *mp)
{
//    if (mp->seek_req)
//        return mp->seek_msec;

    FFPlayer *ffp = mp->ffplayer;
    if (ffp->audio_only) {
        return ffp_get_pos_audio_only(ffp);
    }

    return ffp_get_current_position_l(mp->ffplayer);
}

int64_t ijkmp_get_current_position(IjkMediaPlayer *mp)
{
    long retval;
    assert(mp);

    pthread_mutex_lock(&mp->mutex);
//    if (mp->seek_req)
//        retval = mp->seek_msec;
//    else

    if (mp->ffplayer->isSaveMode) {
        retval = mp->ffplayer->f_0x450;
    } else {
        retval = ijkmp_get_current_position_l(mp);
    }
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

static long ijkmp_get_duration_l(IjkMediaPlayer *mp)
{
    return ffp_get_duration_l(mp->ffplayer);
}

long ijkmp_get_duration(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    long retval = ijkmp_get_duration_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

static long ijkmp_get_playable_duration_l(IjkMediaPlayer *mp)
{
    return ffp_get_playable_duration_l(mp->ffplayer);
}

long ijkmp_get_playable_duration(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    long retval = ijkmp_get_playable_duration_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

void ijkmp_set_loop(IjkMediaPlayer *mp, int loop)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    ffp_set_loop(mp->ffplayer, loop);
    pthread_mutex_unlock(&mp->mutex);
}

int ijkmp_get_loop(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    int loop = ffp_get_loop(mp->ffplayer);
    pthread_mutex_unlock(&mp->mutex);
    return loop;
}

void *ijkmp_get_weak_thiz(IjkMediaPlayer *mp)
{
    return mp->weak_thiz;
}

void *ijkmp_set_weak_thiz(IjkMediaPlayer *mp, void *weak_thiz)
{
    void *prev_weak_thiz = mp->weak_thiz;

    mp->weak_thiz = weak_thiz;

    return prev_weak_thiz;
}

void ijkmp_start_player_l(IjkMediaPlayer *mp)
{
    int iVar1;

    iVar1 = ffp_is_seeking(mp->ffplayer);
    if (iVar1 == 0) {
        iVar1 = ffp_start_l(mp->ffplayer);
        if (iVar1 == 0) {
            ijkmp_change_state_l(mp, MP_STATE_STARTED);
        }
    } else {
//        *(undefined4 *)(mp + 0xe4) = 0;
//        *(undefined4 *)(mp + 0xe0) = 1;
        mp->seek_req = 1;
        mp->seek_msec = 0;
    }
}

/* need to call msg_free_res for freeing the resouce obtained in msg */
int ijkmp_get_msg(IjkMediaPlayer *mp, AVMessage *msg, int block)
{
    assert(mp);
    while (1) {
        int continue_wait_next_msg = 0;
        int retval = msg_queue_get(&mp->ffplayer->msg_queue, msg, block);
        if (retval <= 0)
            return retval;

        switch (msg->what) {
        case FFP_MSG_PREPARED:
            MPTRACE("ijkmp_get_msg: FFP_MSG_PREPARED\n");
            pthread_mutex_lock(&mp->mutex);
            if (mp->mp_state == MP_STATE_ASYNC_PREPARING) {
                ijkmp_change_state_l(mp, MP_STATE_PREPARED);
            } else {
                // FIXME: 1: onError() ?
                av_log(mp->ffplayer, AV_LOG_DEBUG, "FFP_MSG_PREPARED: expecting mp_state==MP_STATE_ASYNC_PREPARING\n");
            }
            //sonxxx
//            if (!mp->ffplayer->start_on_prepared) {
//                ijkmp_change_state_l(mp, MP_STATE_PAUSED);
//            }
            if (ffp_is_paused_l(mp->ffplayer)) {
                ijkmp_change_state_l(mp, MP_STATE_PAUSED);
            }
            pthread_mutex_unlock(&mp->mutex);
            break;

        case FFP_MSG_COMPLETED:
            MPTRACE("ijkmp_get_msg: FFP_MSG_COMPLETED\n");

            pthread_mutex_lock(&mp->mutex);
            mp->restart = 1;
            mp->restart_from_beginning = 1;
            ijkmp_change_state_l(mp, MP_STATE_COMPLETED);

            ffp_audio_only_complete(mp->ffplayer);

            pthread_mutex_unlock(&mp->mutex);
            break;

        case FFP_MSG_SEEK_COMPLETE:
        case 601:
            MPTRACE("ijkmp_get_msg: FFP_MSG_SEEK_COMPLETE: %d\n", msg->what);
            FFPlayer *ffp = mp->ffplayer;
            int64_t uVar14;
            int iVar4;
            if (ffp->f_0x3fc == 0) {
                uVar14 = ffp_get_current_position_l(ffp);
                ffp_audio_seek_to(ffp, uVar14);
                pthread_mutex_lock(&mp->mutex);
                if (msg->what == 0x259) { //601
                    pthread_mutex_unlock(&mp->mutex);
                    continue_wait_next_msg = 1;
                    break;
                }

                iVar4 = ffp_is_seeking(ffp);
                if ((iVar4 == 0) && (mp->seek_req != 0)) {
                    msg_queue_put_simple1(&ffp->msg_queue, FFP_REQ_START);
                    mp->seek_req = 0;
                }
                pthread_mutex_unlock(&mp->mutex);
            } else {
                AVMessage msgTmp;
                msg_init_msg(&msgTmp);
                msgTmp.what = FFP_REQ_SEEK;
                msgTmp.arg1 = ffp->f_0x3f0;
                msgTmp.arg2 = ffp->f_0x3f8;
                msgTmp.arg3 = ffp->f_0x3f4;
                msgTmp.arg4 = ffp->f_0x400;
                msgTmp.longArg1 = ffp->f_0x3e8;
                msg_queue_put(&mp->ffplayer->msg_queue, &msgTmp);
                ffp->f_0x3fc = 0;
            }
            break;

        case FFP_REQ_START:
            MPTRACE("ijkmp_get_msg: FFP_REQ_START\n");
            continue_wait_next_msg = 1;
            pthread_mutex_lock(&mp->mutex);
            if (mp->ffplayer->audio_only == 0) {
                if (0 == ikjmp_chkst_start_l(mp->mp_state)) {
                    // FIXME: 8 check seekable
                    if (mp->restart) {
//                        if (mp->restart_from_beginning && (local_40 = 0, *(int *)(mp->ffplayer->f_0x3a + 0x3a0) ==1)
//                           ) {
//                          ffp_clip_op_get_play_ci(lVar16,&local_40);
//                        }
                        ijkmp_start_player_l(mp);
                        mp->restart = 0;
                        mp->restart_from_beginning = 0;
                    } else {
                        av_log(mp->ffplayer, AV_LOG_DEBUG, "ijkmp_get_msg: FFP_REQ_START: start on fly\n");
                        ijkmp_start_player_l(mp);
                    }
                }
            } else {
                if (mp->restart) {
                    ffp_audio_seek_to(mp->ffplayer, 0);
                    mp->restart = 0;
                    mp->restart_from_beginning = 0;
                }
                AudioTrackEditOp *audioMgr = mp->ffplayer->audioState;
                audioMgr->c.paused = 0;
                audioMgr->f_0x8c8 = 0;
                SDL_AoutPauseAudio(audioMgr->aOut_0x8c0, 0);
                ijkmp_change_state_l(mp, 4);
            }
            mp->f_0xf0 = 1;
            pthread_mutex_unlock(&mp->mutex);
            break;

        case FFP_REQ_PAUSE:
            MPTRACE("ijkmp_get_msg: FFP_REQ_PAUSE\n");
            continue_wait_next_msg = 1;
            pthread_mutex_lock(&mp->mutex);
            if (mp->ffplayer->audio_only == 0) {
                if (0 == ikjmp_chkst_pause_l(mp->mp_state)) {
                    int pause_ret = ffp_pause_l(mp->ffplayer);
                    if (pause_ret == 0)
                        ijkmp_change_state_l(mp, MP_STATE_PAUSED);

                    mp->seek_req = 0;
                }
            } else {
                AudioTrackEditOp *lVar16 = mp->ffplayer->audioState;
                lVar16->c.paused = 1;
                lVar16->f_0x8c8 = 1;
                SDL_AoutPauseAudio(lVar16->aOut_0x8c0, 1);
                ijkmp_change_state_l(mp, MP_STATE_PAUSED);
            }
            pthread_mutex_unlock(&mp->mutex);
            break;

        case FFP_REQ_SEEK:
            MPTRACE("ijkmp_get_msg: FFP_REQ_SEEK\n");
            continue_wait_next_msg = 1;

            int videoIndex = msg->arg1;
            char b1 = msg->arg2;
            char b2 = msg->arg3;
            int64_t msec = msg->longArg1;
            pthread_mutex_lock(&mp->mutex);
            if (mp->ffplayer->audio_only == 0) {
                pthread_mutex_unlock(&mp->mutex);
                if (videoIndex == -1) {
                    //xac dinh video index va seek value tren video do.
                    //vi gia tri seek trong truong hop nay dang la timeline
                    ClipEditOp *clipMgr = mp->ffplayer->clipState;
                    ClipInfo *info = clipMgr->head_0x18;
                    SDL_LockMutex(clipMgr->mutext_8);
                    if (info == 0) {
                        videoIndex = 0;
                    } else {
                        int64_t timeline = 0;
                        int64_t duration;
                        int found = 0;
                        while (1) {
                            videoIndex++;
                            duration = (int64_t) ((info->end_file - info->begin_file) / info->speed_0x40);
                            if (timeline + duration >= msec) {
                                msec = msec - timeline;
                                found = 1;
                                break;
                            }
                            timeline += duration;
                            info = info->next;
                            if (info == 0) {
                                break;
                            }
                        }
                        if (!found) {
                            msec = duration - 100000; //coi nhu seek den video cuoi, cach end 100000us
                        }
                    }
                    SDL_UnlockMutex(clipMgr->mutext_8);
                }

                int ret = ffp_attach_to_clip(mp->ffplayer, videoIndex);
                if (-1 < ret) {
                    VideoClip *player = 0;
                    ret = ffp_clip_op_get_play_ci(mp->ffplayer, &player);
                    if ((-1 < ret) && (player != 0)) {
                        VideoState *is = player->is_0x30;
                        if ((b2 == 1) || (is->is_image != '\0')) {
                            is->seek_req = 0;
                            is->step = 0;
                            is->f_0xdc = 0;
                        }
                        ffp_seek_to_l(mp->ffplayer, player,
                                      (int64_t) (msec * player->speed_0xa0), b1,
                                      b2, msg->arg4);
                    }
                }

            } else {
                ffp_audio_seek_to(mp->ffplayer, msec);
                mp->restart = 0;
                mp->restart_from_beginning = 0;
                pthread_mutex_unlock(&mp->mutex);
            }

            break;

        case FFP_MSG_0x186a1:
            MPTRACE("ijkmp_get_msg: run video index FFP_MSG_0x186a1\n");
            continue_wait_next_msg = 1;
            pthread_mutex_lock(&mp->mutex);
            if (0 == ikjmp_chkst_pause_l(mp->mp_state)) {
                ffp_update_attach_clips(mp->ffplayer, msg->arg1);
            }
            pthread_mutex_unlock(&mp->mutex);
            break;

        case FFP_MSG_0x186a4:
            MPTRACE("ijkmp_get_msg: FFP_MSG_0x186a4\n");
            continue_wait_next_msg = 1;
            pthread_mutex_lock(&mp->mutex);
            if (0 == ikjmp_chkst_pause_l(mp->mp_state)) {
                ffp_clip_op_exchange(mp->ffplayer, msg->arg1, msg->arg2);
            }
            pthread_mutex_unlock(&mp->mutex);
            break;
        }

        if (continue_wait_next_msg) {
            msg_free_res(msg);
            continue;
        }

        return retval;
    }

    return -1;
}

int ijkmp_add_audio_source(IjkMediaPlayer *mp,
                                 int track_index, int index_on_track,
                                 const char *url,
                                 long long start_timeline, long long begin_file, long long end_file)
{
  int ret;

  pthread_mutex_lock(&mp->mutex);
  ret = ffp_audio_track_op_add
                    (mp->ffplayer, track_index, index_on_track, url, start_timeline, begin_file, end_file);
  pthread_mutex_unlock(&mp->mutex);
  return ret;
}

int ijkmp_set_audio_fade_in_fade_out(IjkMediaPlayer *mp,
                                     int track_index,
                                     int index_on_track,
                                     int fadein,
                                     int fadeout)
{
    pthread_mutex_lock(&mp->mutex);
    ffp_audio_track_fade_in_fade_out(mp->ffplayer, track_index, index_on_track, fadein, fadeout);
    return pthread_mutex_unlock(&mp->mutex);
}

int ijkmp_cut_audio_source(IjkMediaPlayer *mp,
                                 int track_index, int index_on_track,
                                 long long start_timeline, long long begin_file, long long end_file)
{
    int ret;
    pthread_mutex_lock(&mp->mutex);
    ret = ffp_audio_track_op_cut(mp->ffplayer, track_index, index_on_track, start_timeline, begin_file, end_file);
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

int ijkmp_delete_audio_source(IjkMediaPlayer *mp,
                                     int track_index,
                                     int index_on_track)
{
    int ret;
    pthread_mutex_lock(&mp->mutex);
    ret = ffp_audio_track_op_delete(mp->ffplayer, track_index, index_on_track);
    //sonxxx
    if (ret >= 0 && mp->ffplayer->audio_only) {
//        msg_queue_put_simple1(&mp->ffplayer->msg_queue, 300);
        mp->restart = 1;
        ffp_audio_only_complete(mp->ffplayer);
    }
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

int ijkmp_set_audio_volume(IjkMediaPlayer *mp,
                                     int track_index,
                                     int index_on_track,
                                     float value)
{
    pthread_mutex_lock(&mp->mutex);
    if (mp->ffplayer->audio_only) {
        ffp_android_set_volume(mp->ffplayer, value);
    } else {
        ffp_audio_track_set_volume(value, mp->ffplayer, track_index, index_on_track);
    }
    return pthread_mutex_unlock(&mp->mutex);
}

int ijkmp_mute_audio_data_source(IjkMediaPlayer *mp)
{
    AudioTrackEditOp *audioState;

    if (mp->ffplayer != NULL &&
            mp->ffplayer->audioState != NULL) {
        audioState = mp->ffplayer->audioState;
    } else {
        return 0;
    }

    av_gettime_relative();
    if (audioState->mute_0x868 != 0) {
        return audioState->mute_0x868;
    }

    pthread_mutex_lock(&mp->mutex);
    audioState->mute_0x868 = 1;
    return pthread_mutex_unlock(&mp->mutex);
}

int ijkmp_unmute_audio_data_source(IjkMediaPlayer *mp)
{
    pthread_mutex_lock(&mp->mutex);
    if (mp->ffplayer != NULL &&
            mp->ffplayer->audioState != NULL) {
        mp->ffplayer->audioState->mute_0x868 = 0;
    }
    return pthread_mutex_unlock(&mp->mutex);
}

int ijkmp_is_seeking(IjkMediaPlayer *mp)
{
    int ret = 0;

    pthread_mutex_lock(&mp->mutex);
    //ret = ffp_is_seeking(mp->ffplayer);
    pthread_mutex_unlock(&mp->mutex);
    return (ret != 0);
}

int ijkmp_set_play_speed(float speed, IjkMediaPlayer *mp, int index)
{
    pthread_mutex_lock(&mp->mutex);
        ffp_set_playback_rate2(speed, mp->ffplayer, index);
    return pthread_mutex_unlock(&mp->mutex);
}

static int ijkmp_add_data_source_l(IjkMediaPlayer *mp,
                                  int index,
                                  const char *url,
                                  void *surface,
                                  long long begin_file, long long end_file,
                                  char isImage)
{
    int ret;

    MPTRACE("video111 start_time=%lld,end_time=%lld\n",begin_file, end_file);
    if ((begin_file < 0) || (end_file < 0) || (end_file - begin_file < 100000)) {
        av_log(0, AV_LOG_ERROR, "%s %d: start_time=%lld, end_time=%lld ret=%d","ijkmp_add_data_source_l",0x196,
               begin_file, end_file);
        return -1;
    }

    av_log(0,AV_LOG_DEBUG,"%s %d: current state=%d","ijkmp_add_data_source_l",0x1a4, mp->mp_state);
    ret = ikjmp_chkst_addvideo_l(mp->mp_state);
    if (ret) {
        return ret;
    }
    if (mp->mp_state == MP_STATE_IDLE) {
        ijkmp_change_state_l(mp, MP_STATE_INITIALIZED);
    }

    ret = ffp_clip_op_insert(mp->ffplayer, index, url, surface,
                               begin_file, end_file, isImage);
    return ret;
}

int ijkmp_add_data_source(IjkMediaPlayer *mp,
                          int index,
                          const char *url,
                          void *surface,
                          long long begin_file, long long end_file,
                          char isImage)
{
    int ret;

    MPTRACE("ijkmp_add_data_source(index = %d, url=\"%s\",surface_creator=%p.)\n",
           index, url, surface);
    pthread_mutex_lock(&mp->mutex);
    ret = ijkmp_add_data_source_l(mp, index, url, surface, begin_file, end_file, isImage);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_add_data_source(index = %d, url=\"%s\")=%d\n",index, url, ret);
    return ret;
}

int ijkmp_set_playmode(IjkMediaPlayer *mp, int mode)
{
    pthread_mutex_lock(&mp->mutex);
    ffp_set_playmode(mp->ffplayer, mode);
    return pthread_mutex_unlock(&mp->mutex);
}

int ijkmp_get_playmode(IjkMediaPlayer *mp)
{
    int ret;

    pthread_mutex_lock(&mp->mutex);
    ret = ffp_get_playmode(mp->ffplayer);
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

int ijkmp_set_image_loader(JNIEnv *env, IjkMediaPlayer *mp, jobject loader)
{
    FFPlayer *ffp;

    pthread_mutex_lock(&mp->mutex);
    ffp = mp->ffplayer;
    if (ffp->img_loader_0x3b8 != NULL) {
        (*env)->DeleteGlobalRef(env, ffp->img_loader_0x3b8);
        ffp->img_loader_0x3b8 = NULL;
    }

    if (loader != 0) {
        ffp->img_loader_0x3b8 = (*env)->NewGlobalRef(env, loader);
    }

    return pthread_mutex_unlock(&mp->mutex);
}


int ijkmp_cut_data_source_l(IjkMediaPlayer *mp, int index, long long begin_file, long long end_file)
{
    return ffp_clip_op_cut(mp->ffplayer, index, begin_file, end_file);
}

int ijkmp_cut_data_source(IjkMediaPlayer *mp, int index, long long begin_file, long long end_file)
{
    int ret;

    pthread_mutex_lock(&mp->mutex);
    ret = ijkmp_cut_data_source_l(mp, index, begin_file, end_file);
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

int ijkmp_copy_data_source(IjkMediaPlayer *mp, int index, jobject surface)
{
    int ret;

    pthread_mutex_lock(&mp->mutex);
    av_log(0,AV_LOG_DEBUG,"%s %d: current state=%d","ijkmp_copy_data_source",0x1a4, mp->mp_state);
    ret = ikjmp_chkst_pause_l(mp->mp_state);
    if (ret == 0) {
        ret = ffp_clip_op_copy(mp->ffplayer, index, surface);
    }
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

int ijkmp_delete_data_source(IjkMediaPlayer *mp, int index)
{
    pthread_mutex_lock(&mp->mutex);
    ffp_clip_op_delete(mp->ffplayer, index);
    pthread_mutex_unlock(&mp->mutex);
    return 0;
}

int ijkmp_exchange_data_source_l(IjkMediaPlayer *mp, int index1, int index2)
{
    msg_queue_put_simple3(&mp->ffplayer->msg_queue, FFP_MSG_0x186a4, index1, index2);
    return 0;
}

int ijkmp_exchange_data_source(IjkMediaPlayer *mp, int index1, int index2) {
    int uVar1;

    pthread_mutex_lock(&mp->mutex);
    uVar1 = ijkmp_exchange_data_source_l(mp, index1, index2);
    pthread_mutex_unlock(&mp->mutex);
    return uVar1;
}

void ijkmp_set_save_mode(IjkMediaPlayer *mp, char b) {
    ffp_set_savemode(mp->ffplayer, b);
}

void ijkmp_request_render(IjkMediaPlayer *mp, int64_t time) {
    ffp_request_render(mp->ffplayer, time);
}

int ijkmp_exchange_audio_source(IjkMediaPlayer *mp,
                                int trackTo, int indexTo,
                                int64_t timeLine,
                                int trackFrom, int indexFrom)
{
    int ret;

    pthread_mutex_lock(&mp->mutex);
    ret = ffp_audio_track_op_exchange(mp->ffplayer, trackTo, indexTo, timeLine, trackFrom, indexFrom);
    pthread_mutex_unlock(&mp->mutex);
    return ret;
}

