/*
 * ijkplayer_android.c
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

#include "ijkplayer_android.h"

#include <assert.h>
#include "ijksdl/android/ijksdl_android.h"
#include "../ff_fferror.h"
#include "../ff_ffplay.h"
#include "../ijkplayer_internal.h"
#include "../pipeline/ffpipeline_ffplay.h"
#include "pipeline/ffpipeline_android.h"

IjkMediaPlayer *ijkmp_android_create(int(*msg_loop)(void*))
{
    IjkMediaPlayer *mp = ijkmp_create(msg_loop);
    if (!mp)
        goto fail;

//    mp->ffplayer->vout = SDL_VoutAndroid_CreateForAndroidSurface();
//    if (!mp->ffplayer->vout)
//        goto fail;

//    mp->ffplayer->pipeline = ffpipeline_create_from_android(mp->ffplayer);
//    if (!mp->ffplayer->pipeline)
//        goto fail;

//    ffpipeline_set_vout(mp->ffplayer->pipeline, mp->ffplayer->vout);

    return mp;

fail:
    ijkmp_dec_ref_p(&mp);
    return NULL;
}

void ijkmp_android_set_surface_l(JNIEnv *env, IjkMediaPlayer *mp, jobject android_surface)
{
    VideoClip *v0, *v1;
    jobject s0, s1;

    //if (!mp || !mp->ffplayer || !mp->ffplayer->vout)
    if (!mp || !mp->ffplayer) {
        return;
    }

    v0 = ffp_clip_op_get_ci_at_index(mp->ffplayer, 0);
    v1 = ffp_clip_op_get_ci_at_index(mp->ffplayer, 1);

    if (v0->vout_0x8 == NULL || v1->vout_0x8 == NULL) {
        return;
    }

    if (android_surface == NULL) {
        s0 = s1 = NULL;
    } else {
        s0 = (*env)->GetObjectArrayElement(env, android_surface, 0);
        s1 = (*env)->GetObjectArrayElement(env, android_surface, 1);
    }
    SDL_VoutAndroid_SetAndroidSurface(env, v0->vout_0x8, s0);
    ffpipeline_set_surface(env, v0->pipeline_0x10, s0);
    SDL_VoutAndroid_SetAndroidSurface(env, v1->vout_0x8, s1);
    ffpipeline_set_surface(env, v1->pipeline_0x10, s1);
}

void ijkmp_android_set_surface(JNIEnv *env, IjkMediaPlayer *mp, jobject android_surface)
{
    if (!mp)
        return;

    MPTRACE("ijkmp_set_android_surface(surface=%p)", (void*)android_surface);
    pthread_mutex_lock(&mp->mutex);
    ijkmp_android_set_surface_l(env, mp, android_surface);
    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_set_android_surface(surface=%p)=void", (void*)android_surface);
}

void ijkmp_android_set_volume(JNIEnv *env, IjkMediaPlayer *mp, int index, float left, float right)
{
    if (!mp)
        return;

    MPTRACE("ijkmp_android_set_volume(%f, %f)", left, right);
    pthread_mutex_lock(&mp->mutex);

    if (mp && mp->ffplayer) {
        ffp_clip_set_volume(mp->ffplayer, index, left, right);
    }

    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_android_set_volume(%f, %f)=void", left, right);
}

int ijkmp_android_get_audio_session_id(JNIEnv *env, IjkMediaPlayer *mp)
{
    int audio_session_id = 0;
    if (!mp)
        return audio_session_id;

    MPTRACE("%s()", __func__);
    pthread_mutex_lock(&mp->mutex);

    if (mp && mp->ffplayer && mp->ffplayer->aout) {
        audio_session_id = SDL_AoutGetAudioSessionId(mp->ffplayer->aout);
    }

    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("%s()=%d", __func__, audio_session_id);

    return audio_session_id;
}

void ijkmp_android_set_mediacodec_select_callback(IjkMediaPlayer *mp, bool (*callback)(void *opaque, ijkmp_mediacodecinfo_context *mcc), void *opaque)
{
    if (!mp)
        return;

    MPTRACE("ijkmp_android_set_mediacodec_select_callback()");
    pthread_mutex_lock(&mp->mutex);

//    if (mp && mp->ffplayer && mp->ffplayer->pipeline) {
//        ffpipeline_set_mediacodec_select_callback(mp->ffplayer->pipeline, callback, opaque);
//    }
    if (mp->ffplayer != NULL) {
        mp->ffplayer->mediacodec_select_callback_opaque = opaque;
        mp->ffplayer->mediacodec_select_callback = callback;
    }

    pthread_mutex_unlock(&mp->mutex);
    MPTRACE("ijkmp_android_set_mediacodec_select_callback()=void");
}
