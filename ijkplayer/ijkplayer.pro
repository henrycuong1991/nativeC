#-------------------------------------------------
#
# Project created by QtCreator 2020-07-14T09:34:04
#
#-------------------------------------------------

QT       -= core gui

TARGET = ijkplayer
TEMPLATE = lib

DEFINES += IJKPLAYER_LIBRARY

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

INCLUDEPATH += "D:\setup\android\ndk\android-ndk-r13b\platforms\android-19\arch-x86\usr\include" \
                .. \
                ..\ijkj4a \
                ..\ffmpeg\include

SOURCES += \
    android/ffmpeg_api_jni.c \
    android/ijkplayer_android.c \
    android/ijkplayer_jni.c \
    android/pipeline/ffpipeline_android.c \
    android/pipeline/ffpipenode_android_mediacodec_vdec.c \
    ff_audio.c \
    ff_cmdutils.c \
    ff_ffpipeline.c \
    ff_ffpipenode.c \
    ff_ffplay.c \
    ijkavformat/allformats.c \
    ijkavformat/ijkasync.c \
    ijkavformat/ijkio.c \
    ijkavformat/ijkioandroidio.c \
    ijkavformat/ijkioapplication.c \
    ijkavformat/ijkiocache.c \
    ijkavformat/ijkioffio.c \
    ijkavformat/ijkiomanager.c \
    ijkavformat/ijkioprotocol.c \
    ijkavformat/ijkiourlhook.c \
    ijkavformat/ijklivehook.c \
    ijkavformat/ijklongurl.c \
    ijkavformat/ijkmediadatasource.c \
    ijkavformat/ijksegment.c \
    ijkavformat/ijkurlhook.c \
    ijkavutil/ijkdict.c \
    ijkavutil/ijkfifo.c \
    ijkavutil/ijkstl.cpp \
    ijkavutil/ijkthreadpool.c \
    ijkavutil/ijktree.c \
    ijkavutil/ijkutils.c \
    ijkmeta.c \
    ijkplayer.c \
    pipeline/ffpipeline_ffplay.c \
    pipeline/ffpipenode_ffplay_vdec.c

HEADERS += \
    android/ffmpeg_api_jni.h \
    android/ijkplayer_android.h \
    android/ijkplayer_android_def.h \
    android/pipeline/ffpipeline_android.h \
    android/pipeline/ffpipenode_android_mediacodec_vdec.h \
    android/pipeline/h264_nal.h \
    android/pipeline/hevc_nal.h \
    android/pipeline/mpeg4_esds.h \
    config.h \
    ff_audio.h \
    ff_cmdutils.h \
    ff_fferror.h \
    ff_ffinc.h \
    ff_ffmsg.h \
    ff_ffmsg_queue.h \
    ff_ffpipeline.h \
    ff_ffpipenode.h \
    ff_ffplay.h \
    ff_ffplay_debug.h \
    ff_ffplay_def.h \
    ff_ffplay_options.h \
    ijkavformat/ijkavformat.h \
    ijkavformat/ijkioapplication.h \
    ijkavformat/ijkiomanager.h \
    ijkavformat/ijkioprotocol.h \
    ijkavformat/ijkiourl.h \
    ijkavutil/ijkdict.h \
    ijkavutil/ijkfifo.h \
    ijkavutil/ijkstl.h \
    ijkavutil/ijkthreadpool.h \
    ijkavutil/ijktree.h \
    ijkavutil/ijkutils.h \
    ijkavutil/opt.h \
    ijkmeta.h \
    ijkplayer.h \
    ijkplayer_internal.h \
    pipeline/ffpipeline_ffplay.h \
    pipeline/ffpipenode_ffplay_vdec.h

unix {
    target.path = /usr/lib
    INSTALLS += target
}
