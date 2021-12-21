/*
 * ijkplayer_jni.c
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

#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <jni.h>
#include <unistd.h>
#include <time.h>

#include "j4a/class/java/util/ArrayList.h"
#include "j4a/class/android/os/Bundle.h"
#include "j4a/class/tv/danmaku/ijk/media/player/IjkMediaPlayer.h"
#include "j4a/class/tv/danmaku/ijk/media/player/misc/IMediaDataSource.h"
//#include "j4a/class/tv/danmaku/ijk/media/player/misc/IAndroidIO.h"
#include "ijksdl/ijksdl_log.h"
#include "../ff_ffplay.h"
#include "ffmpeg_api_jni.h"
#include "ijkplayer_android_def.h"
#include "ijkplayer_android.h"
#include "ijksdl/android/ijksdl_android_jni.h"
#include "ijksdl/android/ijksdl_codec_android_mediadef.h"
#include "ijkavformat/ijkavformat.h"

#include "libavutil/bprint.h"

#define JNI_MODULE_PACKAGE      "tv/danmaku/ijk/media/player"
#define JNI_CLASS_IJKPLAYER     "tv/danmaku/ijk/media/player/IjkMediaPlayer"
#define JNI_IJK_MEDIA_EXCEPTION "tv/danmaku/ijk/media/player/exceptions/IjkMediaException"

#define IJK_CHECK_MPRET_GOTO(retval, env, label) \
    JNI_CHECK_GOTO((retval != EIJK_INVALID_STATE), env, "java/lang/IllegalStateException", NULL, label); \
    JNI_CHECK_GOTO((retval != EIJK_OUT_OF_MEMORY), env, "java/lang/OutOfMemoryError", NULL, label); \
    JNI_CHECK_GOTO((retval == 0), env, JNI_IJK_MEDIA_EXCEPTION, NULL, label);

static JavaVM* g_jvm;

typedef struct player_fields_t {
    pthread_mutex_t mutex;
    jclass clazz;
} player_fields_t;
static player_fields_t g_clazz;

static int inject_callback(void *opaque, int type, void *data, size_t data_size);
static bool mediacodec_select_callback(void *opaque, ijkmp_mediacodecinfo_context *mcc);

static int invalid_license() {
    time_t cur = time(NULL);
    return (cur > 1643673600UL); //2022-02-01
}

static IjkMediaPlayer *jni_get_media_player(JNIEnv* env, jobject thiz)
{
    pthread_mutex_lock(&g_clazz.mutex);

    IjkMediaPlayer *mp = (IjkMediaPlayer *) (intptr_t) J4AC_IjkMediaPlayer__mNativeMediaPlayer__get__catchAll(env, thiz);
    if (mp) {
        ijkmp_inc_ref(mp);
    }

    pthread_mutex_unlock(&g_clazz.mutex);
    return mp;
}

static IjkMediaPlayer *jni_set_media_player(JNIEnv* env, jobject thiz, IjkMediaPlayer *mp)
{
    pthread_mutex_lock(&g_clazz.mutex);

    IjkMediaPlayer *old = (IjkMediaPlayer*) (intptr_t) J4AC_IjkMediaPlayer__mNativeMediaPlayer__get__catchAll(env, thiz);
    if (mp) {
        ijkmp_inc_ref(mp);
    }
    J4AC_IjkMediaPlayer__mNativeMediaPlayer__set__catchAll(env, thiz, (intptr_t) mp);

    pthread_mutex_unlock(&g_clazz.mutex);

    // NOTE: ijkmp_dec_ref may block thread
    if (old != NULL ) {
        ijkmp_dec_ref_p(&old);
    }

    return old;
}

static int64_t jni_set_media_data_source(JNIEnv* env, jobject thiz, jobject media_data_source)
{
    int64_t nativeMediaDataSource = 0;

    pthread_mutex_lock(&g_clazz.mutex);

    jobject old = (jobject) (intptr_t) J4AC_IjkMediaPlayer__mNativeMediaDataSource__get__catchAll(env, thiz);
    if (old) {
        J4AC_IMediaDataSource__close__catchAll(env, old);
        J4A_DeleteGlobalRef__p(env, &old);
        J4AC_IjkMediaPlayer__mNativeMediaDataSource__set__catchAll(env, thiz, 0);
    }

    if (media_data_source) {
        jobject global_media_data_source = (*env)->NewGlobalRef(env, media_data_source);
        if (J4A_ExceptionCheck__catchAll(env) || !global_media_data_source)
            goto fail;

        nativeMediaDataSource = (int64_t) (intptr_t) global_media_data_source;
        J4AC_IjkMediaPlayer__mNativeMediaDataSource__set__catchAll(env, thiz, (jlong) nativeMediaDataSource);
    }

fail:
    pthread_mutex_unlock(&g_clazz.mutex);
    return nativeMediaDataSource;
}

//static int64_t jni_set_ijkio_androidio(JNIEnv* env, jobject thiz, jobject ijk_io)
//{
//    int64_t nativeAndroidIO = 0;

//    pthread_mutex_lock(&g_clazz.mutex);

//    jobject old = (jobject) (intptr_t) J4AC_IjkMediaPlayer__mNativeAndroidIO__get__catchAll(env, thiz);
//    if (old) {
//        J4AC_IAndroidIO__close__catchAll(env, old);
//        J4A_DeleteGlobalRef__p(env, &old);
//        J4AC_IjkMediaPlayer__mNativeAndroidIO__set__catchAll(env, thiz, 0);
//    }

//    if (ijk_io) {
//        jobject global_ijkio_androidio = (*env)->NewGlobalRef(env, ijk_io);
//        if (J4A_ExceptionCheck__catchAll(env) || !global_ijkio_androidio)
//            goto fail;

//        nativeAndroidIO = (int64_t) (intptr_t) global_ijkio_androidio;
//        J4AC_IjkMediaPlayer__mNativeAndroidIO__set__catchAll(env, thiz, (jlong) nativeAndroidIO);
//    }

//fail:
//    pthread_mutex_unlock(&g_clazz.mutex);
//    return nativeAndroidIO;
//}

static int message_loop(void *arg);

static void
IjkMediaPlayer_setDataSourceAndHeaders(
    JNIEnv *env, jobject thiz, jstring path,
    jobjectArray keys, jobjectArray values)
{
    MPTRACE("%s\n", __func__);
    int retval = 0;
    const char *c_path = NULL;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(path, env, "java/lang/IllegalArgumentException", "mpjni: setDataSource: null path", LABEL_RETURN);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: setDataSource: null mp", LABEL_RETURN);

    c_path = (*env)->GetStringUTFChars(env, path, NULL );
    JNI_CHECK_GOTO(c_path, env, "java/lang/OutOfMemoryError", "mpjni: setDataSource: path.string oom", LABEL_RETURN);

    ALOGV("setDataSource: path %s", c_path);
    retval = ijkmp_set_data_source(mp, c_path);
    (*env)->ReleaseStringUTFChars(env, path, c_path);

    IJK_CHECK_MPRET_GOTO(retval, env, LABEL_RETURN);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_setDataSourceFd(JNIEnv *env, jobject thiz, jint fd)
{
    MPTRACE("%s\n", __func__);
    int retval = 0;
    int dupFd = 0;
    char uri[128];
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(fd > 0, env, "java/lang/IllegalArgumentException", "mpjni: setDataSourceFd: null fd", LABEL_RETURN);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: setDataSourceFd: null mp", LABEL_RETURN);

    dupFd = dup(fd);

    ALOGV("setDataSourceFd: dup(%d)=%d\n", fd, dupFd);
    snprintf(uri, sizeof(uri), "pipe:%d", dupFd);
    retval = ijkmp_set_data_source(mp, uri);

    IJK_CHECK_MPRET_GOTO(retval, env, LABEL_RETURN);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_setDataSourceCallback(JNIEnv *env, jobject thiz, jobject callback)
{
    MPTRACE("%s\n", __func__);
    int retval = 0;
    char uri[128];
    int64_t nativeMediaDataSource = 0;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(callback, env, "java/lang/IllegalArgumentException", "mpjni: setDataSourceCallback: null fd", LABEL_RETURN);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: setDataSourceCallback: null mp", LABEL_RETURN);

    nativeMediaDataSource = jni_set_media_data_source(env, thiz, callback);
    JNI_CHECK_GOTO(nativeMediaDataSource, env, "java/lang/IllegalStateException", "mpjni: jni_set_media_data_source: NewGlobalRef", LABEL_RETURN);

    ALOGV("setDataSourceCallback: %"PRId64"\n", nativeMediaDataSource);
    snprintf(uri, sizeof(uri), "ijkmediadatasource:%"PRId64, nativeMediaDataSource);

    retval = ijkmp_set_data_source(mp, uri);

    IJK_CHECK_MPRET_GOTO(retval, env, LABEL_RETURN);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

//static void
//IjkMediaPlayer_setAndroidIOCallback(JNIEnv *env, jobject thiz, jobject callback) {
//    MPTRACE("%s\n", __func__);
//    int64_t nativeAndroidIO = 0;

//    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
//    JNI_CHECK_GOTO(callback, env, "java/lang/IllegalArgumentException", "mpjni: setAndroidIOCallback: null fd", LABEL_RETURN);
//    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: setAndroidIOCallback: null mp", LABEL_RETURN);

//    nativeAndroidIO = jni_set_ijkio_androidio(env, thiz, callback);
//    JNI_CHECK_GOTO(nativeAndroidIO, env, "java/lang/IllegalStateException", "mpjni: jni_set_ijkio_androidio: NewGlobalRef", LABEL_RETURN);

//    ijkmp_set_option_int(mp, FFP_OPT_CATEGORY_FORMAT, "androidio-inject-callback", nativeAndroidIO);

//LABEL_RETURN:
//    ijkmp_dec_ref_p(&mp);
//}

static void
IjkMediaPlayer_setVideoSurface(JNIEnv *env, jobject thiz, jobject jsurface)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: setVideoSurface: null mp", LABEL_RETURN);

    ijkmp_android_set_surface(env, mp, jsurface);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return;
}

static void
IjkMediaPlayer_prepareAsync(JNIEnv *env, jobject thiz, jint i)
{
    MPTRACE("%s\n", __func__);
    int retval = 0;

    if (invalid_license()) {
        return;
    }

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: prepareAsync: null mp", LABEL_RETURN);

    retval = ijkmp_prepare_async(mp, i);
    IJK_CHECK_MPRET_GOTO(retval, env, LABEL_RETURN);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_start(JNIEnv *env, jobject thiz)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: start: null mp", LABEL_RETURN);

    ijkmp_start(mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_stop(JNIEnv *env, jobject thiz)
{
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: stop: null mp", LABEL_RETURN);

    ijkmp_stop(mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_pause(JNIEnv *env, jobject thiz)
{
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: pause: null mp", LABEL_RETURN);

    ijkmp_pause(mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_seekTo(JNIEnv *env, jobject thiz, jlong msec, jint i, jboolean b1, jboolean b2)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: seekTo: null mp", LABEL_RETURN);

    ijkmp_pause(mp);
    ijkmp_seek_to(mp, msec, i, b1, b2);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static jboolean
IjkMediaPlayer_isPlaying(JNIEnv *env, jobject thiz)
{
    jboolean retval = JNI_FALSE;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: isPlaying: null mp", LABEL_RETURN);

    retval = ijkmp_is_playing(mp) ? JNI_TRUE : JNI_FALSE;

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return retval;
}

static jlong
IjkMediaPlayer_getCurrentPosition(JNIEnv *env, jobject thiz)
{
    jlong retval = 0;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: getCurrentPosition: null mp", LABEL_RETURN);

    retval = ijkmp_get_current_position(mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return retval;
}

static jlong
IjkMediaPlayer_getDuration(JNIEnv *env, jobject thiz)
{
    jlong retval = 0;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: getDuration: null mp", LABEL_RETURN);

    retval = ijkmp_get_duration(mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return retval;
}

static void
IjkMediaPlayer_release(JNIEnv *env, jobject thiz)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (!mp)
        return;

    ijkmp_android_set_surface(env, mp, NULL);
    // explicit shutdown mp, in case it is not the last mp-ref here
    ijkmp_shutdown(mp);

    ijkmp_set_image_loader(env, mp, NULL);

    //only delete weak_thiz at release
    jobject weak_thiz = (jobject) ijkmp_set_weak_thiz(mp, NULL );
    (*env)->DeleteGlobalRef(env, weak_thiz);
    jni_set_media_player(env, thiz, NULL);
    jni_set_media_data_source(env, thiz, NULL);

    ijkmp_dec_ref_p(&mp);
}

static void IjkMediaPlayer_native_setup(JNIEnv *env, jobject thiz, jobject weak_this);

static void
IjkMediaPlayer_reset(JNIEnv *env, jobject thiz)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (!mp)
        return;

    jobject weak_thiz = (jobject) ijkmp_set_weak_thiz(mp, NULL );

    IjkMediaPlayer_release(env, thiz);
    IjkMediaPlayer_native_setup(env, thiz, weak_thiz);

    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_setLoopCount(JNIEnv *env, jobject thiz, jint loop_count)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: setLoopCount: null mp", LABEL_RETURN);

    ijkmp_set_loop(mp, loop_count);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static jint
IjkMediaPlayer_getLoopCount(JNIEnv *env, jobject thiz)
{
    jint loop_count = 1;
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: getLoopCount: null mp", LABEL_RETURN);

    loop_count = ijkmp_get_loop(mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return loop_count;
}

static jfloat
ijkMediaPlayer_getPropertyFloat(JNIEnv *env, jobject thiz, jint id, jfloat default_value)
{
    jfloat value = default_value;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: getPropertyFloat: null mp", LABEL_RETURN);

    value = ijkmp_get_property_float(mp, id, default_value);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return value;
}

static void
ijkMediaPlayer_setPropertyFloat(JNIEnv *env, jobject thiz, jint id, jfloat value)
{
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: setPropertyFloat: null mp", LABEL_RETURN);

    ijkmp_set_property_float(mp, id, value);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return;
}

static jlong
ijkMediaPlayer_getPropertyLong(JNIEnv *env, jobject thiz, jint id, jlong default_value)
{
    jlong value = default_value;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: getPropertyLong: null mp", LABEL_RETURN);

    value = ijkmp_get_property_int64(mp, id, default_value);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return value;
}

static void
ijkMediaPlayer_setPropertyLong(JNIEnv *env, jobject thiz, jint id, jlong value)
{
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: setPropertyLong: null mp", LABEL_RETURN);

    ijkmp_set_property_int64(mp, id, value);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return;
}

static void
ijkMediaPlayer_setStreamSelected(JNIEnv *env, jobject thiz, jint stream, jboolean selected)
{
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    int ret = 0;
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: setStreamSelected: null mp", LABEL_RETURN);

    ret = ijkmp_set_stream_selected(mp, stream, selected);
    if (ret < 0) {
        ALOGE("failed to %s %d", selected ? "select" : "deselect", stream);
        goto LABEL_RETURN;
    }

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return;
}

static void
IjkMediaPlayer_setVolume(JNIEnv *env, jobject thiz, jint index, jfloat leftVolume, jfloat rightVolume)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: setVolume: null mp", LABEL_RETURN);

    ijkmp_android_set_volume(env, mp, index, leftVolume, rightVolume);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static jint
IjkMediaPlayer_getAudioSessionId(JNIEnv *env, jobject thiz)
{
    jint audio_session_id = 0;
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: getAudioSessionId: null mp", LABEL_RETURN);

    audio_session_id = ijkmp_android_get_audio_session_id(env, mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return audio_session_id;
}

static void
IjkMediaPlayer_setOption(JNIEnv *env, jobject thiz, jint category, jobject name, jobject value)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    const char *c_name = NULL;
    const char *c_value = NULL;
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: setOption: null mp", LABEL_RETURN);

    if (!name) {
        goto LABEL_RETURN;
    }

    c_name = (*env)->GetStringUTFChars(env, name, NULL );
    JNI_CHECK_GOTO(c_name, env, "java/lang/OutOfMemoryError", "mpjni: setOption: name.string oom", LABEL_RETURN);
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: IjkMediaPlayer_setOption: null name", LABEL_RETURN);

    if (value) {
        c_value = (*env)->GetStringUTFChars(env, value, NULL );
        JNI_CHECK_GOTO(c_name, env, "java/lang/OutOfMemoryError", "mpjni: setOption: name.string oom", LABEL_RETURN);
    }

    ijkmp_set_option(mp, category, c_name, c_value);

LABEL_RETURN:
    if (c_name)
        (*env)->ReleaseStringUTFChars(env, name, c_name);
    if (c_value)
        (*env)->ReleaseStringUTFChars(env, value, c_value);
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_setOptionLong(JNIEnv *env, jobject thiz, jint category, jobject name, jlong value)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    const char *c_name = NULL;
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: setOptionLong: null mp", LABEL_RETURN);

    c_name = (*env)->GetStringUTFChars(env, name, NULL );
    JNI_CHECK_GOTO(c_name, env, "java/lang/OutOfMemoryError", "mpjni: setOptionLong: name.string oom", LABEL_RETURN);

    ijkmp_set_option_int(mp, category, c_name, value);

LABEL_RETURN:
    if (c_name)
        (*env)->ReleaseStringUTFChars(env, name, c_name);
    ijkmp_dec_ref_p(&mp);
}

static jstring
IjkMediaPlayer_getColorFormatName(JNIEnv *env, jclass clazz, jint mediaCodecColorFormat)
{
    const char *codec_name = SDL_AMediaCodec_getColorFormatName(mediaCodecColorFormat);
    if (!codec_name)
        return NULL ;

    return (*env)->NewStringUTF(env, codec_name);
}

static jstring
IjkMediaPlayer_getVideoCodecInfo(JNIEnv *env, jobject thiz)
{
    MPTRACE("%s\n", __func__);
    jstring jcodec_info = NULL;
    int ret = 0;
    char *codec_info = NULL;

    if (invalid_license()) {
        return NULL;
    }

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: getVideoCodecInfo: null mp", LABEL_RETURN);

    ret = ijkmp_get_video_codec_info(mp, &codec_info);
    if (ret < 0 || !codec_info)
        goto LABEL_RETURN;

    jcodec_info = (*env)->NewStringUTF(env, codec_info);
LABEL_RETURN:
    if (codec_info)
        free(codec_info);

    ijkmp_dec_ref_p(&mp);
    return jcodec_info;
}

static jstring
IjkMediaPlayer_getAudioCodecInfo(JNIEnv *env, jobject thiz)
{
    MPTRACE("%s\n", __func__);
    jstring jcodec_info = NULL;
    int ret = 0;
    char *codec_info = NULL;

    if (invalid_license()) {
        return NULL;
    }

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: getAudioCodecInfo: null mp", LABEL_RETURN);

    ret = ijkmp_get_audio_codec_info(mp, &codec_info);
    if (ret < 0 || !codec_info)
        goto LABEL_RETURN;

    jcodec_info = (*env)->NewStringUTF(env, codec_info);
LABEL_RETURN:
    if (codec_info)
        free(codec_info);

    ijkmp_dec_ref_p(&mp);
    return jcodec_info;
}

inline static void fillMetaInternal(JNIEnv *env, jobject jbundle, IjkMediaMeta *meta, const char *key, const char *default_value)
{
    const char *value = ijkmeta_get_string_l(meta, key);
    if (value == NULL )
        value = default_value;

    J4AC_Bundle__putString__withCString__catchAll(env, jbundle, key, value);
}

static jobject
IjkMediaPlayer_getMediaMeta(JNIEnv *env, jobject thiz)
{
    MPTRACE("%s\n", __func__);
    bool is_locked = false;
    jobject jret_bundle = NULL;
    jobject jlocal_bundle = NULL;
    jobject jstream_bundle = NULL;
    jobject jarray_list = NULL;
    IjkMediaMeta *meta = NULL;
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: getMediaMeta: null mp", LABEL_RETURN);

    meta = ijkmp_get_meta_l(mp);
    if (!meta)
        goto LABEL_RETURN;

    ijkmeta_lock(meta);
    is_locked = true;

    jlocal_bundle = J4AC_Bundle__Bundle(env);
    if (J4A_ExceptionCheck__throwAny(env)) {
        goto LABEL_RETURN;
    }

    fillMetaInternal(env, jlocal_bundle, meta, IJKM_KEY_FORMAT, NULL );
    fillMetaInternal(env, jlocal_bundle, meta, IJKM_KEY_DURATION_US, NULL );
    fillMetaInternal(env, jlocal_bundle, meta, IJKM_KEY_START_US, NULL );
    fillMetaInternal(env, jlocal_bundle, meta, IJKM_KEY_BITRATE, NULL );

    fillMetaInternal(env, jlocal_bundle, meta, IJKM_KEY_VIDEO_STREAM, "-1");
    fillMetaInternal(env, jlocal_bundle, meta, IJKM_KEY_AUDIO_STREAM, "-1");
    fillMetaInternal(env, jlocal_bundle, meta, IJKM_KEY_TIMEDTEXT_STREAM, "-1");

    jarray_list = J4AC_ArrayList__ArrayList(env);
    if (J4A_ExceptionCheck__throwAny(env)) {
        goto LABEL_RETURN;
    }

    size_t count = ijkmeta_get_children_count_l(meta);
    for (size_t i = 0; i < count; ++i) {
        IjkMediaMeta *streamRawMeta = ijkmeta_get_child_l(meta, i);
        if (streamRawMeta) {
            jstream_bundle = J4AC_Bundle__Bundle(env);
            if (J4A_ExceptionCheck__throwAny(env)) {
                goto LABEL_RETURN;
            }

            fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_TYPE,     IJKM_VAL_TYPE__UNKNOWN);
            fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_LANGUAGE, NULL);
            const char *type = ijkmeta_get_string_l(streamRawMeta, IJKM_KEY_TYPE);
            if (type) {
                fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_CODEC_NAME, NULL );
                fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_CODEC_PROFILE, NULL );
                fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_CODEC_LEVEL, NULL );
                fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_CODEC_LONG_NAME, NULL );
                fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_CODEC_PIXEL_FORMAT, NULL );
                fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_BITRATE, NULL );
                fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_CODEC_PROFILE_ID, NULL );

                if (0 == strcmp(type, IJKM_VAL_TYPE__VIDEO)) {
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_WIDTH, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_HEIGHT, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_FPS_NUM, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_FPS_DEN, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_TBR_NUM, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_TBR_DEN, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_SAR_NUM, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_SAR_DEN, NULL );
                } else if (0 == strcmp(type, IJKM_VAL_TYPE__AUDIO)) {
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_SAMPLE_RATE, NULL );
                    fillMetaInternal(env, jstream_bundle, streamRawMeta, IJKM_KEY_CHANNEL_LAYOUT, NULL );
                }
                J4AC_ArrayList__add(env, jarray_list, jstream_bundle);
                if (J4A_ExceptionCheck__throwAny(env)) {
                    goto LABEL_RETURN;
                }
            }

            SDL_JNI_DeleteLocalRefP(env, &jstream_bundle);
        }
    }

    J4AC_Bundle__putParcelableArrayList__withCString__catchAll(env, jlocal_bundle, IJKM_KEY_STREAMS, jarray_list);
    jret_bundle = jlocal_bundle;
    jlocal_bundle = NULL;
LABEL_RETURN:
    if (is_locked && meta)
        ijkmeta_unlock(meta);

    SDL_JNI_DeleteLocalRefP(env, &jstream_bundle);
    SDL_JNI_DeleteLocalRefP(env, &jlocal_bundle);
    SDL_JNI_DeleteLocalRefP(env, &jarray_list);

    ijkmp_dec_ref_p(&mp);
    return jret_bundle;
}

static void
IjkMediaPlayer_native_init(JNIEnv *env)
{
    MPTRACE("%s\n", __func__);
}

static void
IjkMediaPlayer_native_setup(JNIEnv *env, jobject thiz, jobject weak_this)
{
    MPTRACE("%s\n", __func__);

    if (invalid_license()) {
        return;
    }

    IjkMediaPlayer *mp = ijkmp_android_create(message_loop);
    JNI_CHECK_GOTO(mp, env, "java/lang/OutOfMemoryError", "mpjni: native_setup: ijkmp_create() failed", LABEL_RETURN);

    jni_set_media_player(env, thiz, mp);
    ijkmp_set_weak_thiz(mp, (*env)->NewGlobalRef(env, weak_this));
    ijkmp_set_inject_opaque(mp, ijkmp_get_weak_thiz(mp));
//    ijkmp_set_ijkio_inject_opaque(mp, ijkmp_get_weak_thiz(mp));
    ijkmp_android_set_mediacodec_select_callback(mp, mediacodec_select_callback, ijkmp_get_weak_thiz(mp));

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
}

static void
IjkMediaPlayer_native_finalize(JNIEnv *env, jobject thiz, jobject name, jobject value)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer_release(env, thiz);
}

// NOTE: support to be called from read_thread
static int
inject_callback(void *opaque, int what, void *data, size_t data_size)
{
    JNIEnv     *env     = NULL;
    jobject     jbundle = NULL;
    int         ret     = -1;
    SDL_JNI_SetupThreadEnv(&env);

    jobject weak_thiz = (jobject) opaque;
    if (weak_thiz == NULL )
        goto fail;
    switch (what) {
        case AVAPP_CTRL_WILL_HTTP_OPEN:
        case AVAPP_CTRL_WILL_LIVE_OPEN:
        case AVAPP_CTRL_WILL_CONCAT_SEGMENT_OPEN: {
            AVAppIOControl *real_data = (AVAppIOControl *)data;
            real_data->is_handled = 0;

            jbundle = J4AC_Bundle__Bundle__catchAll(env);
            if (!jbundle) {
                ALOGE("%s: J4AC_Bundle__Bundle__catchAll failed for case %d\n", __func__, what);
                goto fail;
            }
            J4AC_Bundle__putString__withCString__catchAll(env, jbundle, "url", real_data->url);
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "segment_index", real_data->segment_index);
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "retry_counter", real_data->retry_counter);
            real_data->is_handled = J4AC_IjkMediaPlayer__onNativeInvoke(env, weak_thiz, what, jbundle);
            if (J4A_ExceptionCheck__catchAll(env)) {
                goto fail;
            }

            J4AC_Bundle__getString__withCString__asCBuffer(env, jbundle, "url", real_data->url, sizeof(real_data->url));
            if (J4A_ExceptionCheck__catchAll(env)) {
                goto fail;
            }
            ret = 0;
            break;
        }
        case AVAPP_EVENT_WILL_HTTP_OPEN:
        case AVAPP_EVENT_DID_HTTP_OPEN:
        case AVAPP_EVENT_WILL_HTTP_SEEK:
        case AVAPP_EVENT_DID_HTTP_SEEK: {
            AVAppHttpEvent *real_data = (AVAppHttpEvent *) data;
            jbundle = J4AC_Bundle__Bundle__catchAll(env);
            if (!jbundle) {
                ALOGE("%s: J4AC_Bundle__Bundle__catchAll failed for case %d\n", __func__, what);
                goto fail;
            }
            J4AC_Bundle__putString__withCString__catchAll(env, jbundle, "url", real_data->url);
            J4AC_Bundle__putLong__withCString__catchAll(env, jbundle, "offset", real_data->offset);
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "error", real_data->error);
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "http_code", real_data->http_code);
            J4AC_Bundle__putLong__withCString__catchAll(env, jbundle, "file_size", real_data->filesize);
            J4AC_IjkMediaPlayer__onNativeInvoke(env, weak_thiz, what, jbundle);
            if (J4A_ExceptionCheck__catchAll(env))
                goto fail;
            ret = 0;
            break;
        }
        case AVAPP_CTRL_DID_TCP_OPEN:
        case AVAPP_CTRL_WILL_TCP_OPEN: {
            AVAppTcpIOControl *real_data = (AVAppTcpIOControl *)data;
            jbundle = J4AC_Bundle__Bundle__catchAll(env);
            if (!jbundle) {
                ALOGE("%s: J4AC_Bundle__Bundle__catchAll failed for case %d\n", __func__, what);
                goto fail;
            }
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "error", real_data->error);
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "family", real_data->family);
            J4AC_Bundle__putString__withCString__catchAll(env, jbundle, "ip", real_data->ip);
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "port", real_data->port);
            J4AC_Bundle__putInt__withCString__catchAll(env, jbundle, "fd", real_data->fd);
            J4AC_IjkMediaPlayer__onNativeInvoke(env, weak_thiz, what, jbundle);
            if (J4A_ExceptionCheck__catchAll(env))
                goto fail;
            ret = 0;
            break;
        }
        default: {
            ret = 0;
        }
    }
fail:
    SDL_JNI_DeleteLocalRefP(env, &jbundle);
    return ret;
}

static bool mediacodec_select_callback(void *opaque, ijkmp_mediacodecinfo_context *mcc)
{
    JNIEnv *env = NULL;
    jobject weak_this = (jobject) opaque;
    const char *found_codec_name = NULL;

    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("%s: SetupThreadEnv failed\n", __func__);
        return -1;
    }

    found_codec_name = J4AC_IjkMediaPlayer__onSelectCodec__withCString__asCBuffer(env, weak_this, mcc->mime_type, mcc->profile, mcc->level, mcc->codec_name, sizeof(mcc->codec_name));
    if (J4A_ExceptionCheck__catchAll(env) || !found_codec_name) {
        ALOGE("%s: onSelectCodec failed\n", __func__);
        goto fail;
    }

fail:
    return found_codec_name;
}

inline static void post_event(JNIEnv *env, jobject weak_this, int what, int arg1, int arg2)
{
    MPTRACE("post_event(%p, %p, %d, %d, %d)", (void*)env, (void*) weak_this, what, arg1, arg2);
    J4AC_IjkMediaPlayer__postEventFromNative(env, weak_this, what, arg1, arg2, NULL);
    // MPTRACE("post_event()=void");
}

inline static void post_event2(JNIEnv *env, jobject weak_this, int what, int arg1, int arg2, jobject obj)
{
    MPTRACE("post_event2(%p, %p, %d, %d, %d, %p)", (void*)env, (void*) weak_this, what, arg1, arg2, (void*)obj);
    J4AC_IjkMediaPlayer__postEventFromNative(env, weak_this, what, arg1, arg2, obj);
    // MPTRACE("post_event2()=void");
}

static void message_loop_n(JNIEnv *env, IjkMediaPlayer *mp)
{
    jobject weak_thiz = (jobject) ijkmp_get_weak_thiz(mp);
    JNI_CHECK_GOTO(weak_thiz, env, NULL, "mpjni: message_loop_n: null weak_thiz", LABEL_RETURN);

    while (1) {
        AVMessage msg;

        int retval = ijkmp_get_msg(mp, &msg, 1);
        if (retval < 0)
            break;

        // block-get should never return 0
        assert(retval > 0);

        switch (msg.what) {
        case FFP_MSG_FLUSH:
            MPTRACE("videotrack FFP_MSG_FLUSH:\n");
            post_event(env, weak_thiz, MEDIA_NOP, 0, 0);
            break;
        case FFP_MSG_ERROR:
            MPTRACE("videotrack FFP_MSG_ERROR: %d\n", msg.arg1);
            post_event(env, weak_thiz, MEDIA_ERROR, MEDIA_ERROR_IJK_PLAYER, msg.arg1);
            break;
        case FFP_MSG_PREPARED:
            MPTRACE("videotrack FFP_MSG_PREPARED:\n");
            post_event(env, weak_thiz, MEDIA_PREPARED, 0, 0);
            break;
        case FFP_MSG_COMPLETED:
            MPTRACE("videotrack FFP_MSG_COMPLETED:\n");
            post_event(env, weak_thiz, MEDIA_PLAYBACK_COMPLETE, 0, 0);
            break;
        case FFP_MSG_VIDEO_SIZE_CHANGED:
            MPTRACE("videotrack FFP_MSG_VIDEO_SIZE_CHANGED: %d, %d\n", msg.arg1, msg.arg2);
            post_event(env, weak_thiz, MEDIA_SET_VIDEO_SIZE, msg.arg1, msg.arg2);
            break;
        case FFP_MSG_SAR_CHANGED:
            MPTRACE("videotrack FFP_MSG_SAR_CHANGED: %d, %d\n", msg.arg1, msg.arg2);
            post_event(env, weak_thiz, MEDIA_SET_VIDEO_SAR, msg.arg1, msg.arg2);
            break;
        case FFP_MSG_VIDEO_RENDERING_START:
            MPTRACE("videotrack FFP_MSG_VIDEO_RENDERING_START:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_VIDEO_RENDERING_START, 0);
            break;
        case FFP_MSG_AUDIO_RENDERING_START:
            MPTRACE("videotrack FFP_MSG_AUDIO_RENDERING_START:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_AUDIO_RENDERING_START, 0);
            break;
        case FFP_MSG_VIDEO_ROTATION_CHANGED:
            MPTRACE("videotrack FFP_MSG_VIDEO_ROTATION_CHANGED: %d\n", msg.arg1);
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_VIDEO_ROTATION_CHANGED, msg.arg1);
            break;
        case FFP_MSG_AUDIO_DECODED_START:
            MPTRACE("videotrack FFP_MSG_AUDIO_DECODED_START:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_AUDIO_DECODED_START, 0);
            break;
        case FFP_MSG_VIDEO_DECODED_START:
            MPTRACE("videotrack FFP_MSG_VIDEO_DECODED_START:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_VIDEO_DECODED_START, 0);
            break;
        case FFP_MSG_OPEN_INPUT:
            MPTRACE("FFP_MSG_OPEN_INPUT:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_OPEN_INPUT, 0);
            break;
        case FFP_MSG_FIND_STREAM_INFO:
            MPTRACE("videotrack FFP_MSG_FIND_STREAM_INFO:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_FIND_STREAM_INFO, 0);
            break;
        case FFP_MSG_COMPONENT_OPEN:
            MPTRACE("videotrack FFP_MSG_COMPONENT_OPEN:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_COMPONENT_OPEN, 0);
            break;
        case FFP_MSG_BUFFERING_START:
            MPTRACE("videotrack FFP_MSG_BUFFERING_START:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_BUFFERING_START, msg.arg1);
            break;
        case FFP_MSG_BUFFERING_END:
            MPTRACE("videotrack FFP_MSG_BUFFERING_END:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_BUFFERING_END, msg.arg1);
            break;
        case FFP_MSG_BUFFERING_UPDATE:
             MPTRACE("videotrack FFP_MSG_BUFFERING_UPDATE: %d, %d", msg.arg1, msg.arg2);
            post_event(env, weak_thiz, MEDIA_BUFFERING_UPDATE, msg.arg1, msg.arg2);
            break;
        case FFP_MSG_BUFFERING_BYTES_UPDATE:
            break;
        case FFP_MSG_BUFFERING_TIME_UPDATE:
            break;
        case FFP_MSG_SEEK_COMPLETE:
            MPTRACE("videotrack FFP_MSG_SEEK_COMPLETE:\n");
            post_event(env, weak_thiz, MEDIA_SEEK_COMPLETE, 0, 0);
            break;
        case FFP_MSG_ACCURATE_SEEK_COMPLETE:
            MPTRACE("videotrack FFP_MSG_ACCURATE_SEEK_COMPLETE:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_MEDIA_ACCURATE_SEEK_COMPLETE, msg.arg1);
            break;
        case FFP_MSG_PLAYBACK_STATE_CHANGED:
            break;
        case FFP_MSG_TIMED_TEXT:
            MPTRACE("videotrack FFP_MSG_TIMED_TEXT:\n");
            if (msg.obj) {
                jstring text = (*env)->NewStringUTF(env, (char *)msg.obj);
                post_event2(env, weak_thiz, MEDIA_TIMED_TEXT, 0, 0, text);
                J4A_DeleteLocalRef__p(env, &text);
            }
            else {
                post_event2(env, weak_thiz, MEDIA_TIMED_TEXT, 0, 0, NULL);
            }
            break;
        case FFP_MSG_GET_IMG_STATE:
            if (msg.obj) {
                jstring file_name = (*env)->NewStringUTF(env, (char *)msg.obj);
                MPTRACE("%s videotrack FFP_MSG_GET_IMG_STATE filename frame %s", __func__,file_name);
                post_event2(env, weak_thiz, MEDIA_GET_IMG_STATE, msg.arg1, msg.arg2, file_name);
                J4A_DeleteLocalRef__p(env, &file_name);
            }
            else {
                MPTRACE("videotrack FFP_MSG_GET_IMG_STATE update data :\n");
                post_event2(env, weak_thiz, MEDIA_GET_IMG_STATE, msg.arg1, msg.arg2, NULL);
            }
            break;
        case FFP_MSG_VIDEO_SEEK_RENDERING_START:
            MPTRACE("videotrack FFP_MSG_VIDEO_SEEK_RENDERING_START:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_VIDEO_SEEK_RENDERING_START, msg.arg1);
            break;
        case FFP_MSG_AUDIO_SEEK_RENDERING_START:
            MPTRACE("videotrack FFP_MSG_AUDIO_SEEK_RENDERING_START:\n");
            post_event(env, weak_thiz, MEDIA_INFO, MEDIA_INFO_AUDIO_SEEK_RENDERING_START, msg.arg1);
            break;
        default:
            ALOGE("unknown FFP_MSG_xxx(%d)\n", msg.what);
            break;
        }
        msg_free_res(&msg);
    }

LABEL_RETURN:
    ;
}

static int message_loop(void *arg)
{
    MPTRACE("%s\n", __func__);

    JNIEnv *env = NULL;
    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("%s: SetupThreadEnv failed\n", __func__);
        return -1;
    }

    IjkMediaPlayer *mp = (IjkMediaPlayer*) arg;
    JNI_CHECK_GOTO(mp, env, NULL, "mpjni: native_message_loop: null mp", LABEL_RETURN);

    message_loop_n(env, mp);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);

    MPTRACE("message_loop exit");
    return 0;
}

// ----------------------------------------------------------------------------
void monstartup(const char *libname);
void moncleanup(void);

static void
IjkMediaPlayer_native_profileBegin(JNIEnv *env, jclass clazz, jstring libName)
{
    MPTRACE("%s\n", __func__);

    const char *c_lib_name = NULL;
    static int s_monstartup = 0;

    if (!libName)
        return;

    if (s_monstartup) {
        ALOGW("monstartup already called\b");
        return;
    }

    c_lib_name = (*env)->GetStringUTFChars(env, libName, NULL );
    JNI_CHECK_GOTO(c_lib_name, env, "java/lang/OutOfMemoryError", "mpjni: monstartup: libName.string oom", LABEL_RETURN);

    s_monstartup = 1;
    monstartup(c_lib_name);
    ALOGD("monstartup: %s\n", c_lib_name);

LABEL_RETURN:
    if (c_lib_name)
        (*env)->ReleaseStringUTFChars(env, libName, c_lib_name);
}

static void
IjkMediaPlayer_native_profileEnd(JNIEnv *env, jclass clazz)
{
    MPTRACE("%s\n", __func__);
    static int s_moncleanup = 0;

    if (s_moncleanup) {
        ALOGW("moncleanu already called\b");
        return;
    }

    s_moncleanup = 1;
    moncleanup();
    ALOGD("moncleanup\n");
}

static void
IjkMediaPlayer_native_setLogLevel(JNIEnv *env, jclass clazz, jint level)
{
    MPTRACE("%s(%d)\n", __func__, level);
    ijkmp_global_set_log_level(level);
    ALOGD("moncleanup\n");
}

//convert level log tu av sang ijk
int log_level_av_to_ijk(int av_level)
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

void append_log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    int config_level;
    int ijk_log_level;
    jbyteArray log_data;
    JNIEnv *env;
    int ret;
    AVBPrint part4;
    char line[1024];
    int line_length;

    config_level = av_log_get_level();
    if (level <= config_level) {
        ijk_log_level = log_level_av_to_ijk(level);

        //printf log nhu binh thuong
        memset(line, 0, 0x400);
        av_bprint_init_for_buffer(&part4, line, 0x400);
        av_vbprintf(&part4, fmt, vl);

        //put log len java
        pthread_mutex_lock(&g_clazz.mutex);
        //lay ve jnienv tu javaenv
        ret = (*g_jvm)->GetEnv(g_jvm, (void**) &env, JNI_VERSION_1_4);
        if (ret != JNI_OK) {
            ret = SDL_JNI_SetupThreadEnv(&env);
        }
        if (ret == JNI_OK) {
            line_length = strlen(line);
            log_data = (*env)->NewByteArray(env, line_length);
            if (log_data != NULL) {
                (*env)->SetByteArrayRegion(env, log_data, 0, line_length, line);
                J4AC_tv_danmaku_ijk_media_player_IjkMediaPlayer__appendLogData__catchAll
                        (env, log_data, ijk_log_level);
                (*env)->DeleteLocalRef(env, log_data);
            }
        }
        pthread_mutex_unlock(&g_clazz.mutex);
    }
}

void no_append_log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    //do nothing
}

static void
IjkMediaPlayer_native_setLogLevel2(JNIEnv *env, jclass clazz, jint level, jboolean b)
{
    MPTRACE("%s(%d,%d)\n", __func__, level, b);
    ijkmp_global_set_log_level(level);
    if (b) {
        av_log_set_callback(append_log_callback);
    } else {
        av_log_set_callback(no_append_log_callback);
    }
    ALOGD("moncleanup\n");
}

static void
IjkMediaPlayer_setFrameAtTime(JNIEnv *env, jobject thiz, jstring path, jlong start_time, jlong end_time, jint num, jint definition) {
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    const char *c_path = NULL;
    JNI_CHECK_GOTO(path, env, "java/lang/IllegalArgumentException", "mpjni: setFrameAtTime: null path", LABEL_RETURN);
    JNI_CHECK_GOTO(mp, env, "java/lang/IllegalStateException", "mpjni: setFrameAtTime: null mp", LABEL_RETURN);

    c_path = (*env)->GetStringUTFChars(env, path, NULL );
    JNI_CHECK_GOTO(c_path, env, "java/lang/OutOfMemoryError", "mpjni: setFrameAtTime: path.string oom", LABEL_RETURN);

    ALOGV("setFrameAtTime: path %s", c_path);
    ijkmp_set_frame_at_time(mp, c_path, start_time, end_time, num, definition);
    (*env)->ReleaseStringUTFChars(env, path, c_path);

LABEL_RETURN:
    ijkmp_dec_ref_p(&mp);
    return;
}

static jint
IjkMediaPlayer_addAudioDataSource(JNIEnv *env, jobject thiz, jint track_index,
                                  jint index_on_track,
                                  jstring file,
                                  jlong start_timeline,
                                  jlong begin_file,
                                  jlong end_file) {


    int ret;
    MPTRACE("%s\n", __func__);

    if (invalid_license()) {
        return -1;
    }

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: addAudioDataSource: null mp");
        return -1;
    }

    const char *url = (*env)->GetStringUTFChars(env, file, NULL);
    ret = ijkmp_add_audio_source(mp, track_index, index_on_track, url, start_timeline, begin_file, end_file);
    (*env)->ReleaseStringUTFChars(env, file, url);
    ijkmp_dec_ref(mp);
    return ret;
}

static jint
IjkMediaPlayer_setAudioFadeInFadeOut(JNIEnv *env, jobject thiz, jint track_index,
                                     jint index_on_track,
                                     jint fadein,
                                     jint fadeout) {

    MPTRACE("%s\n", __func__);

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        return -1;
    }

    ijkmp_set_audio_fade_in_fade_out(mp, track_index, index_on_track, fadein, fadeout);
    ijkmp_dec_ref(mp);
    return 0;
}

static jint
IjkMediaPlayer_cutAudioDataSource(JNIEnv *env, jobject thiz, jint track_index,
                                  jint index_on_track,
                                  jlong start_timeline,
                                  jlong begin_file,
                                  jlong end_file) {

    MPTRACE("%s\n", __func__);

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: cutAudioSource: null mp");
        return -1;
    }

    ijkmp_cut_audio_source(mp, track_index, index_on_track, start_timeline, begin_file, end_file);
    ijkmp_dec_ref(mp);
    return 0;
}

static jint
IjkMediaPlayer_deleteAudioDataSource(JNIEnv *env, jobject thiz, jint track_index,
                                  jint index_on_track) {

    int ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: deleteAudioSource: null mp");
        return -1;
    }

    ret = ijkmp_delete_audio_source(mp, track_index, index_on_track);
    ijkmp_dec_ref(mp);
    return ret;
}

static jint
IjkMediaPlayer_setAudioVolume(JNIEnv *env, jobject thiz, jint track_index,
                                  jint index_on_track, float volume) {

    int ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_setAudioVolume: null mp");
        return -1;
    }

    ijkmp_set_audio_volume(mp, track_index, index_on_track, volume);
    ijkmp_dec_ref(mp);
    return 0;
}

static void
IjkMediaPlayer_muteAudioDataSource(JNIEnv *env, jobject thiz) {
    int ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        //SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_setAudioVolume: null mp");
        return;
    }

    ijkmp_mute_audio_data_source(mp);
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_unmuteAudioDataSource(JNIEnv *env, jobject thiz) {
    int ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        //SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_setAudioVolume: null mp");
        return;
    }

    ijkmp_unmute_audio_data_source(mp);
    ijkmp_dec_ref(mp);
}

static jboolean
IjkMediaPlayer_isSeeking(JNIEnv *env, jobject thiz) {

    jboolean ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        //SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_setAudioVolume: null mp");
        return JNI_FALSE;
    }

    ret = ijkmp_is_seeking(mp);
    ijkmp_dec_ref(mp);
    return ret;
}

static void
IjkMediaPlayer_exchangeDataSource(JNIEnv *env, jobject thiz, jint i1, jint i2) {

    jboolean ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_exchangeDataSource: null mp");
        return;
    }

    ijkmp_exchange_data_source(mp, i1, i2);
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_addDataSource(JNIEnv *env, jobject thiz, jint index,
                              jstring url,
                              jobjectArray arr1,
                              jobjectArray arr2,
                              jobject surface,
                              jlong begin_file,
                              jlong end_file,
                              jboolean isImage)
{
    MPTRACE("%s video111 \n", __func__);

    if (invalid_license()) {
        return;
    }

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: _addDataSource: null mp");
        return;
    }

    const char *c_url = (*env)->GetStringUTFChars(env, url, NULL);
    ijkmp_add_data_source(mp, index, c_url, surface, begin_file, end_file, isImage);
    (*env)->ReleaseStringUTFChars(env, url, c_url);
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_setPlayMode(JNIEnv *env, jobject thiz, jint mode)
{
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        return;
    }

    ijkmp_set_playmode(mp, mode);
    ijkmp_dec_ref(mp);
}

static jint
IjkMediaPlayer_getPlayMode(JNIEnv *env, jobject thiz)
{
    int ret;
    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        return -1;
    }

    ret = ijkmp_get_playmode(mp);
    ijkmp_dec_ref(mp);
    return ret;
}

static void
IjkMediaPlayer_setImageLoader(JNIEnv *env, jobject thiz, jobject loader)
{
    int ret;

    MPTRACE("%s\n", __func__);

    if (invalid_license()) {
        return;
    }

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        return;
    }

    ret = ijkmp_set_image_loader(env, mp, loader);
    ijkmp_dec_ref(mp);
}

static void
    IjkMediaPlayer_setPlaySpeed(JNIEnv *env, jobject thiz, int index, float speed)
{
    int ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        return;
    }

    ret = ijkmp_set_play_speed(speed, mp, index);
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_copyDataSource(JNIEnv *env, jobject thiz, jint index, jobject surface) {

    jboolean ret;

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_copyDataSource: null mp");
        return;
    }

    ret = ijkmp_copy_data_source(mp, index, surface);
    //check ret de throw cac exception tuong ung???
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_cutDataSource(JNIEnv *env, jobject thiz, jint index,
                              jlong begin_file,
                              jlong end_file)
{
    MPTRACE("%s\n", __func__);

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        //SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: _addDataSource: null mp");
        return;
    }

    ijkmp_cut_data_source(mp, index, begin_file, end_file);
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_deleteDataSource(JNIEnv *env, jobject thiz, jint index)
{

    MPTRACE("%s\n", __func__);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_deleteDataSource: null mp");
        return;
    }

    ijkmp_delete_data_source(mp, index);
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_setSaveMode(JNIEnv *env, jobject thiz, jboolean b)
{

    MPTRACE("%s: %d\n", __func__, b);
    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
//        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_setSaveMode: null mp");
        return;
    }

    ijkmp_set_save_mode(mp, b);
    ijkmp_dec_ref(mp);
}

static void
IjkMediaPlayer_native_requestRender(JNIEnv *env, jobject thiz, jlong j)
{

    MPTRACE("%s: %ld\n", __func__, j);

    if (invalid_license()) {
        return;
    }

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
//        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_setSaveMode: null mp");
        return;
    }

    ijkmp_request_render(mp, j);
    ijkmp_dec_ref(mp);
}

static jint
IjkMediaPlayer_exchangeAudioDataSource(JNIEnv *env, jobject thiz, jint i1, jint i2, jlong j, jint i3, jint i4)
{
    //MPTRACE("%s: %ld\n", __func__, j);
    int ret;

    IjkMediaPlayer *mp = jni_get_media_player(env, thiz);
    if (mp == NULL) {
        SDL_JNI_ThrowException(env, "java/lang/IllegalStateException", "mpjni: IjkMediaPlayer_exchangeAudioDataSource: null mp");
        return -1;
    }

    ret = ijkmp_exchange_audio_source(mp, i1, i2, j, i3, i4);
    ijkmp_dec_ref(mp);
    return ret;
}
// ----------------------------------------------------------------------------

static JNINativeMethod g_methods[] = {
//    {
//        "_setDataSource",
//        "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
//        (void *) IjkMediaPlayer_setDataSourceAndHeaders
//    },
    { "_setDataSourceFd",       "(I)V",     (void *) IjkMediaPlayer_setDataSourceFd },
//    { "_setDataSource",         "(Ltv/danmaku/ijk/media/player/misc/IMediaDataSource;)V", (void *)IjkMediaPlayer_setDataSourceCallback },
//    { "_setAndroidIOCallback",  "(Ltv/danmaku/ijk/media/player/misc/IAndroidIO;)V", (void *)IjkMediaPlayer_setAndroidIOCallback },

//    { "_setVideoSurface",       "(Landroid/view/Surface;)V", (void *) IjkMediaPlayer_setVideoSurface },
    { "_prepareAsync",          "(I)V",      (void *) IjkMediaPlayer_prepareAsync },
    { "_start",                 "()V",      (void *) IjkMediaPlayer_start },
    { "_stop",                  "()V",      (void *) IjkMediaPlayer_stop },
    { "_seekTo",                 "(JIZZ)V",     (void *) IjkMediaPlayer_seekTo },
    { "_pause",                 "()V",      (void *) IjkMediaPlayer_pause },
    { "isPlaying",              "()Z",      (void *) IjkMediaPlayer_isPlaying },
    { "getCurrentPosition",     "()J",      (void *) IjkMediaPlayer_getCurrentPosition },
    { "getDuration",            "()J",      (void *) IjkMediaPlayer_getDuration },
    { "_release",               "()V",      (void *) IjkMediaPlayer_release },
    { "_reset",                 "()V",      (void *) IjkMediaPlayer_reset },
    { "setVolume",              "(IFF)V",    (void *) IjkMediaPlayer_setVolume },
    { "getAudioSessionId",      "()I",      (void *) IjkMediaPlayer_getAudioSessionId },
    { "native_init",            "()V",      (void *) IjkMediaPlayer_native_init },
    { "native_setup",           "(Ljava/lang/Object;)V", (void *) IjkMediaPlayer_native_setup },
    { "native_finalize",        "()V",      (void *) IjkMediaPlayer_native_finalize },

    { "_setOption",             "(ILjava/lang/String;Ljava/lang/String;)V", (void *) IjkMediaPlayer_setOption },
    { "_setOption",             "(ILjava/lang/String;J)V",                  (void *) IjkMediaPlayer_setOptionLong },

    { "_getColorFormatName",    "(I)Ljava/lang/String;",    (void *) IjkMediaPlayer_getColorFormatName },
    { "_getVideoCodecInfo",     "()Ljava/lang/String;",     (void *) IjkMediaPlayer_getVideoCodecInfo },
    { "_getAudioCodecInfo",     "()Ljava/lang/String;",     (void *) IjkMediaPlayer_getAudioCodecInfo },
    { "_getMediaMeta",          "()Landroid/os/Bundle;",    (void *) IjkMediaPlayer_getMediaMeta },
    { "_setLoopCount",          "(I)V",                     (void *) IjkMediaPlayer_setLoopCount },
    { "_getLoopCount",          "()I",                      (void *) IjkMediaPlayer_getLoopCount },
    { "_getPropertyFloat",      "(IF)F",                    (void *) ijkMediaPlayer_getPropertyFloat },
    { "_setPropertyFloat",      "(IF)V",                    (void *) ijkMediaPlayer_setPropertyFloat },
    { "_getPropertyLong",       "(IJ)J",                    (void *) ijkMediaPlayer_getPropertyLong },
    { "_setPropertyLong",       "(IJ)V",                    (void *) ijkMediaPlayer_setPropertyLong },
    { "_setStreamSelected",     "(IZ)V",                    (void *) ijkMediaPlayer_setStreamSelected },

    { "native_profileBegin",    "(Ljava/lang/String;)V",    (void *) IjkMediaPlayer_native_profileBegin },
    { "native_profileEnd",      "()V",                      (void *) IjkMediaPlayer_native_profileEnd },

//    { "native_setLogLevel",     "(I)V",                     (void *) IjkMediaPlayer_native_setLogLevel },
    { "native_setLogLevel",     "(IZ)V",                     (void *) IjkMediaPlayer_native_setLogLevel2 },
//    { "_setFrameAtTime",        "(Ljava/lang/String;JJII)V", (void *) IjkMediaPlayer_setFrameAtTime },

    { "_addAudioDataSource",    "(IILjava/lang/String;JJJ)I", (void *) IjkMediaPlayer_addAudioDataSource},
    { "_addDataSource",    "(ILjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/Object;JJZ)V", (void *) IjkMediaPlayer_addDataSource},
    { "_copyDataSource",    "(ILjava/lang/Object;)V", (void *) IjkMediaPlayer_copyDataSource},
    { "_cutAudioDataSource",    "(IIJJJ)I", (void *) IjkMediaPlayer_cutAudioDataSource},
    { "_cutDataSource",    "(IJJ)V", (void *) IjkMediaPlayer_cutDataSource},
    { "_deleteAudioDataSource",    "(II)I", (void *) IjkMediaPlayer_deleteAudioDataSource},
    { "_deleteDataSource",    "(I)V", (void *) IjkMediaPlayer_deleteDataSource},
    { "_exchangeAudioDataSource",    "(IIJII)I", (void *) IjkMediaPlayer_exchangeAudioDataSource},
    { "_exchangeDataSource",    "(II)V", (void *) IjkMediaPlayer_exchangeDataSource},
    { "_muteAudioDataSource",    "()V", (void *) IjkMediaPlayer_muteAudioDataSource},
    { "_setAudioFadeInFadeOut",    "(IIII)V", (void *) IjkMediaPlayer_setAudioFadeInFadeOut},
    { "_setAudioVolume",    "(IIF)V", (void *) IjkMediaPlayer_setAudioVolume},
    { "_setImageLoader",    "(Ljava/lang/Object;)V", (void *) IjkMediaPlayer_setImageLoader},
    { "_setPlaySpeed",    "(IF)V", (void *) IjkMediaPlayer_setPlaySpeed},
    { "_setSaveMode",    "(Z)V", (void *) IjkMediaPlayer_setSaveMode},
    { "_unmuteAudioDataSource",    "()V", (void *) IjkMediaPlayer_unmuteAudioDataSource},
    { "getPlayMode",    "()I", (void *) IjkMediaPlayer_getPlayMode},
    { "setPlayMode",    "(I)V", (void *) IjkMediaPlayer_setPlayMode},
    { "native_requestRender",    "(J)V", (void *) IjkMediaPlayer_native_requestRender},
    { "isSeeking",    "()Z", (void *) IjkMediaPlayer_isSeeking},
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv* env = NULL;

    g_jvm = vm;
    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        return -1;
    }
    assert(env != NULL);

    pthread_mutex_init(&g_clazz.mutex, NULL );

    // FindClass returns LocalReference
    IJK_FIND_JAVA_CLASS(env, g_clazz.clazz, JNI_CLASS_IJKPLAYER);
    (*env)->RegisterNatives(env, g_clazz.clazz, g_methods, NELEM(g_methods) );

    ijkmp_global_init();
    ijkmp_global_set_inject_callback(inject_callback);

    FFmpegApi_global_init(env);

    return JNI_VERSION_1_4;
}

JNIEXPORT void JNI_OnUnload(JavaVM *jvm, void *reserved)
{
    ijkmp_global_uninit();

    pthread_mutex_destroy(&g_clazz.mutex);
}
