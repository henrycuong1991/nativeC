/*
 * Copyright (c) 2003 Bilibili
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2013-2015 Zhang Rui <bbcallen@gmail.com>
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

#ifndef FFPLAY__FF_FFPLAY_DEF_H
#define FFPLAY__FF_FFPLAY_DEF_H

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
#include <jni.h>

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
// FFP_MERGE: #include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

//#if CONFIG_AVFILTER
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
//#endif

#include <stdbool.h>
#include "ijkavformat/ijkiomanager.h"
#include "ijkavformat/ijkioapplication.h"
#include "ff_ffinc.h"
#include "ff_ffmsg_queue.h"
#include "ff_ffpipenode.h"
#include "ijkmeta.h"

#include "android/ijkplayer_android_def.h"
typedef struct IJKFF_Pipeline IJKFF_Pipeline;
#define MAX_AUDIO_TRACK     4

#define DEFAULT_HIGH_WATER_MARK_IN_BYTES        (256 * 1024)

/*
 * START: buffering after prepared/seeked
 * NEXT:  buffering for the second time after START
 * MAX:   ...
 */
#define DEFAULT_FIRST_HIGH_WATER_MARK_IN_MS     (100)
#define DEFAULT_NEXT_HIGH_WATER_MARK_IN_MS      (1 * 1000)
#define DEFAULT_LAST_HIGH_WATER_MARK_IN_MS      (5 * 1000)

#define BUFFERING_CHECK_PER_BYTES               (512)
#define BUFFERING_CHECK_PER_MILLISECONDS        (500)
#define FAST_BUFFERING_CHECK_PER_MILLISECONDS   (50)
#define MAX_RETRY_CONVERT_IMAGE                 (3)

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MAX_ACCURATE_SEEK_TIMEOUT (5000)
#ifdef FFP_MERGE
#define MIN_FRAMES 25
#endif
#define DEFAULT_MIN_FRAMES  50000
#define MIN_MIN_FRAMES      2
#define MAX_MIN_FRAMES      50000
#define MIN_FRAMES (ffp->dcc.min_frames)
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control */
#define SDL_VOLUME_STEP (SDL_MIX_MAXVOLUME / 50)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.15
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 100.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define MIN_PKT_DURATION 15

#ifdef FFP_MERGE
#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

static unsigned sws_flags = SWS_BICUBIC;
#endif

#define HD_IMAGE 2  // 640*360
#define SD_IMAGE 1  // 320*180
#define LD_IMAGE 0  // 160*90
#define MAX_DEVIATION 1200000   // 1200ms

typedef struct GetImgInfo {
    char *img_path;
    int64_t start_time;
    int64_t end_time;
    int64_t frame_interval;
    int num;
    int count;
    int width;
    int height;
    AVCodecContext *frame_img_codec_ctx;
    struct SwsContext *frame_img_convert_ctx;
} GetImgInfo;

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
    MyAVPacketList *recycle_pkt;
    int recycle_count;
    int alloc_count;
    char* urlVid;
    int is_buffer_indicator;
} PacketQueue;

// #define VIDEO_PICTURE_QUEUE_SIZE 3
#define VIDEO_PICTURE_QUEUE_SIZE_MIN        (3)
#define VIDEO_PICTURE_QUEUE_SIZE_MAX        (16)
#define VIDEO_PICTURE_QUEUE_SIZE_DEFAULT    (VIDEO_PICTURE_QUEUE_SIZE_MIN)
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE_MAX, SUBPICTURE_QUEUE_SIZE))

#define VIDEO_MAX_FPS_DEFAULT 30

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
    //??? co them 1 truong nua o day
    int ext1;
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub; //bo
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
#ifdef FFP_MERGE
    SDL_Texture *bmp;
#else
    SDL_VoutOverlay *bmp; //0x28
#endif
    int allocated; //0x30
    //??? co 1 truong nua o day
    int f_0x34;
    int width; //0x38
    int height; //0x3c
    int format;
    AVRational sar;
    int uploaded;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex; //0x6f0
    SDL_cond *cond; //0x6f8
    PacketQueue *pktq;
} FrameQueue;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder {
    AVPacket pkt;
    AVPacket pkt_temp;
    PacketQueue *queue;
    AVCodecContext *avctx; //0xe98
    int pkt_serial; //0xea0
    int finished; //0xea4
    int packet_pending; //0xea8
    int bfsc_ret;
    uint8_t *bfsc_data;
    char *url;
    SDL_cond *empty_queue_cond;

    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
    SDL_Thread _decoder_tid;

    SDL_Profiler decode_profiler;
    Uint64 first_frame_decoded_time;
    int    first_frame_decoded;
} Decoder;

struct IjkMediaMeta;
typedef struct FFPlayer FFPlayer;
typedef struct VideoState {
    int f_0;
    SDL_mutex *mutex_2_0x8;
    SDL_cond *cond_4_0x10;
    SDL_Thread *read_tid; //0x18
    SDL_Thread _read_tid; //0x20
    AVInputFormat *iformat;
    int abort_request; //0xc0
    int force_refresh; //0xc4
    int paused; //0xc8
    int last_paused;
    int queue_attachments_req;
    volatile int seek_req; //0xd4
    int seek_flags; //0xd8
    //co 2 truong dc, e0 o day
    int64_t seek_pos; //0xe8
    int64_t seek_rel; //0xf0
    int f_0xf8;
#ifdef FFP_MERGE
    int read_pause_return;
#endif
    AVFormatContext *ic; //0x118
    int realtime; //0x120

    Clock audclk; //0x4a - 0x128
    Clock vidclk; //0x58 - 0x160
    Clock extclk; //0x66-0x198

    FrameQueue pictq; //0x1d0
    FrameQueue subpq; //bo
    FrameQueue sampq; //0x708

    Decoder auddec; //0xc40
    Decoder viddec; //0xde0
    Decoder subdec; //bo

    int audio_stream; //0xf80

    int av_sync_type; //0x3e1 (0xf84)
    void *handle; //0x40508 (0x101420)
    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st; //0xfc0
    PacketQueue audioq; //0xfc8, serial: 0x3fd (0xff4)
    int audio_hw_buf_size;
    uint8_t *audio_buf; //0x1228
    uint8_t *audio_buf1; //0x1230
    short *audio_new_buf;  /* for soundtouch buf */
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size; //0x1244
    unsigned int audio_new_buf_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume; //0x1254???
    int muted;
    struct AudioParams audio_src;
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx; //0x12c8
    int frame_drops_early; //0x12d0
    int frame_drops_late; //0x12d4
    int continuous_frame_drops_early; //0x12d8

    enum ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    } show_mode; //0x12dc
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
#ifdef FFP_MERGE
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
#endif
    double last_vis_time; //0x1012e8
#ifdef FFP_MERGE
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
#endif

    int subtitle_stream; //bo
    AVStream *subtitle_st; //bo
    PacketQueue subtitleq; //bo

    double frame_timer; //0x1012f0
    double frame_last_returned_time; //0x1012f8
    double frame_last_filter_delay; //0x101300
    int video_stream; //0x101308
    AVStream *video_st; //0x101310
    PacketQueue videoq; //0x101318, serial: 0x404d1 (0x101344)
    double max_frame_duration; //0x101370???     // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
#ifdef FFP_SUB
    struct SwsContext *sub_convert_ctx;
#endif
    int eof;

    char *filename; //0x101380
    int width, height, xleft, ytop;
    volatile int step; //0x10139C, 0x980

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph
#endif

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread; //0x1013E0, 0x404F8

    /* extra fields */
    SDL_mutex  *play_mutex; // only guard state, do not block any long operation //bo
    SDL_Thread *video_refresh_tid; //bo
    SDL_Thread _video_refresh_tid; //bo

    int buffering_on; //0x9cc 0x1013E8
    volatile int pause_req; //0x9d0, //0x1013EC, 0x404FB

    int dropping_frame;
    int is_video_high_fps; // above 30fps
    int is_video_high_res; // above 1080p

    PacketQueue *buffer_indicator_queue;

    volatile int latest_video_seek_load_serial;
    volatile int latest_audio_seek_load_serial;
    volatile int64_t latest_seek_load_start_at;

    int drop_aframe_count;
    int drop_vframe_count;
    int64_t accurate_seek_start_time;
    volatile int64_t accurate_seek_vframe_pts;
    volatile int64_t accurate_seek_aframe_pts;
    int audio_accurate_seek_req;
    int video_accurate_seek_req;
    SDL_mutex *accurate_seek_mutex;
    SDL_cond  *video_accurate_seek_cond;
    SDL_cond  *audio_accurate_seek_cond;
    volatile int initialized_decoder;
    int seek_buffering;
    float play_rate_0xa0c; //0x101428

    FFPlayer *ffp; //0xa14 - 0x4050c - 0x101430
    char is_image; //0x101438, 0x4050E, 0xa1c
    AVFrame *img_handler_0xa24; //0x40510, 0x101440
    int64_t f_0x101448;
    int64_t f_0x101450;
    int64_t f_0x101458;
    int64_t f_0x101460;
    jobject surface; //0x101468 (0x4051a)
    jobject surface_creator; //0x101470 (0x4051c)
    int f_0x101414;
    struct IjkMediaMeta *meta; //0x101418 (0x40506)

    SDL_Thread *image_load_thread; //0x68
    SDL_Thread _image_load_thread; //0x70

    int f_0xdc;
    int f_0xe0;
    //int f_0xc4_0x18;
    int f_0x100;
    int64_t f_0x108;
    int f_0x110;
    volatile int f_0x9f4; //0x40504, 0x101410
} VideoState;

/* options specified by the user */
#ifdef FFP_MERGE
static AVInputFormat *file_iformat;
static const char *input_filename;
static const char *window_title;
static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static int display_disable;
static int show_status = 1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static enum ShowMode show_mode = SHOW_MODE_NONE;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
#if CONFIG_AVFILTER
static const char **vfilters_list = NULL;
static int nb_vfilters = 0;
static char *afilters = NULL;
#endif
static int autorotate = 1;
static int find_stream_info = 1;

/* current context */
static int is_full_screen;
static int64_t audio_callback_time;

static AVPacket flush_pkt;
static AVPacket eof_pkt;

#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;
#endif

/*****************************************************************************
 * end at line 330 in ffplay.c
 * near packet_queue_put
 ****************************************************************************/
typedef struct FFTrackCacheStatistic
{
    int64_t duration;
    int64_t bytes;
    int64_t packets;
} FFTrackCacheStatistic;

typedef struct FFStatistic
{
    int64_t vdec_type;

    float vfps;
    float vdps;
    float avdelay;
    float avdiff;
    int64_t bit_rate;

    FFTrackCacheStatistic video_cache;
    FFTrackCacheStatistic audio_cache;

    int64_t buf_backwards;
    int64_t buf_forwards;
    int64_t buf_capacity;
    SDL_SpeedSampler2 tcp_read_sampler;
    int64_t latest_seek_load_duration;
    int64_t byte_count;
    int64_t cache_physical_pos;
    int64_t cache_file_forwards;
    int64_t cache_file_pos;
    int64_t cache_count_bytes;
    int64_t logical_file_size;
    int drop_frame_count;
    int decode_frame_count;
    float drop_frame_rate;
} FFStatistic;

#define FFP_TCP_READ_SAMPLE_RANGE 2000
inline static void ffp_reset_statistic(FFStatistic *dcc)
{
    memset(dcc, 0, sizeof(FFStatistic));
    SDL_SpeedSampler2Reset(&dcc->tcp_read_sampler, FFP_TCP_READ_SAMPLE_RANGE);
}

typedef struct FFDemuxCacheControl
{
    int min_frames; //0x370
    int max_buffer_size; //0x374
    int high_water_mark_in_bytes; //0x378

    int first_high_water_mark_in_ms;
    int next_high_water_mark_in_ms;
    int last_high_water_mark_in_ms;
    int current_high_water_mark_in_ms; //0x388
} FFDemuxCacheControl;

inline static void ffp_reset_demux_cache_control(FFDemuxCacheControl *dcc)
{
    dcc->min_frames                = DEFAULT_MIN_FRAMES;
    dcc->max_buffer_size           = MAX_QUEUE_SIZE;
    dcc->high_water_mark_in_bytes  = DEFAULT_HIGH_WATER_MARK_IN_BYTES;

    dcc->first_high_water_mark_in_ms    = DEFAULT_FIRST_HIGH_WATER_MARK_IN_MS;
    dcc->next_high_water_mark_in_ms     = DEFAULT_NEXT_HIGH_WATER_MARK_IN_MS;
    dcc->last_high_water_mark_in_ms     = DEFAULT_LAST_HIGH_WATER_MARK_IN_MS;
    dcc->current_high_water_mark_in_ms  = DEFAULT_FIRST_HIGH_WATER_MARK_IN_MS;
}

/* audio decode context */
typedef struct _AudioDecodeContext {
    AVFormatContext *fmtCtx; //0x0
    AVStream *stream; //0x8
    AVCodecContext *codecCtx; //0x10
    int stream_index_3; //0x18
    int f_0x30;
    int f_0x34;
    AVPacket pkt; //0x40
    AVPacket pkt2; //0x98
    int f_0xf0;
    int64_t stream_start_time; //0xf8
    AVRational timebase; //0x100
    int64_t f_0x108;
    AVRational timebase2; //0x110
} AudioDecodeContext;

/* AudioClipList */
typedef struct _AudioClip {
    char *url;
    long long start_timeline;
    //2 - khoang cach voi audio truoc
    long long pre_distance_0x10;
    //3 - end timeline cua audio truoc
    long long pre_end_timeline_0x18;
    long long begin_file;
    long long end_file;
    long long duration;
    //7:gom 2 truong int
    int fadein_0x38_7;
    int fadeout_0x3c;
    //8: 2 truong
    float volume_0x44;
    //9: 2 truong
    int f_0x4c; //clip id
    //10: 2 truong
    int index_on_track;
    //11
    AudioDecodeContext *decodeCtx; //0x58
    struct _AudioClip *next; //0x60
} AudioClip;

//to chuc danh sach lien ket
typedef struct _AudioTrackInfo {
    AudioClip *head; //0x0
    AudioClip *tail; //0x8
    AVFilterContext *filterCtxAbuf; //0x10
    AVFilterContext *filterCtxVol; //0x18
    AVFilterContext *filterCtxFadeIn; //0x20
    AVFilterContext *filterCtxFadeOut; //0x28
    int size; //0x30
    int f_0x34; //giong nhu id gen
    int f_0x7; //0x38
    AudioClip *clip_0x9; //0x48, current play clip
    int sample_rate; //0x50
    int num_chans; //0x54
    uint64_t chan_layout; //0x58
    enum AVSampleFormat sampleFormat; //0x60
    int f_0xe; //0x70
    AVFrame *frame_0xf; //0x78
    double f_0x10; //0x80
    FFPlayer *ffp; //0x88
} AudioTrackInfo;

#define MAX_AUDIO_TRACK     4
typedef struct _AudioTrackEditOp {
    AudioTrackInfo *arr[MAX_AUDIO_TRACK];
    int f_0x20; //so track co file
    FrameQueue frameQueue; //0x28

    int f_0x530;
    PacketQueue pktQueue; //0x560
    SDL_mutex  *mutex; //0x5b8
    SDL_cond   *cond; //0x5c0
    int f_processing_0x5c8;
    SDL_Thread *thread_0x5d0;
    SDL_Thread thread_0x5d8;
    AVFilterContext *filterCtxAmix; //0x620
    AVFilterContext *filterCtxVol; //0x628
    AVFilterContext *filterCtxAbuf; //0x630
    AVFilterGraph *filterGraph; //0x638
    uint8_t f_0x644[0x200]; //0x644

    uint8_t *audio_buf; //0x848
    uint8_t *audio_buf1; //0x850
    int audio_buf_size; //0x858
    int audio_buf1_size; //0x85c
    int audio_buf_index; //0x860
    int audio_write_buf_size; //0x864
    int mute_0x868;
    int f_0x86c;
    volatile int f_0x870;
    AudioParams hwAudioParams; //0x878
    struct SwrContext *swrCtx; //0x898
    volatile int f_0x8a0;
    SDL_mutex  *seek_mutex; //0x8a8
    long long f_0x8b0;
    IJKFF_Pipeline *pipeline; //0x8b8
    SDL_Aout *aOut_0x8c0;
    volatile int f_0x8c8;
    Clock c; //0x8d0

    int f_0x900;
    double nextPts; //0x908
    int audio_clock_serial; //0x918
    volatile int abort_req; //0x93c
    int f_0x940;
    int af_change; //0x944
    int f_0x948;
    int f_0x94c;
    SDL_mutex  *volume_fade_mutex; //0x950
    int64_t tickHr; //0x958
    double f_0x960;
    int f_0x968;
} AudioTrackEditOp;

typedef struct _ClipInfo {
    int id_0;
    char is_image_4;
    jobject surface_creator; //0x10
    long long begin_file; //0x28
    long long end_file; //0x30
    long long duration; //0x18
    int64_t f_0x20;
    float volume; //0x38
    float volume2; //0x3c
    float speed_0x40;
    float speed_cfg;
    int change_rate;
    const char *url; //0x8
    int isPlay;
    struct _ClipInfo *next; //0x48
} ClipInfo;

typedef struct _VideoClip {
    int clip_id_0x58;
    int queue_index_0x5c;
    volatile int isUsed_0x60;
    int f_0x64_100;
    long long begin_time_0x68;
    long long end_time_0x70;
    long long duration_0x78;
    float volume_0x88;
    float volume2_0x8c;
    SDL_mutex *mutext_0x20;
    SDL_mutex *mutext_0x90;
    SDL_cond *con_0x28;
    float speed_0xa0;
    int changeVideo;
    VideoState *is_0x30;
    char *video_codec_info; //0x48
    SDL_Aout *aout_0;
    SDL_Vout *vout_0x8;
    int doneVideo;
    struct IJKFF_Pipeline *pipeline_0x10;
    struct IJKFF_Pipenode *node_vdec_0x18;
} VideoClip;

typedef struct _ClipEditOp {
    VideoClip *info[2];
    long long f_2;
    int id_gen_0x2c;
    ClipInfo *head_0x18;
    ClipInfo *tail_0x20;
    int nb_clips_5_0x28;
    long long total_duration_6_0x30;
    long long f_7;
    SDL_mutex *mutext_8;
    SDL_cond *con_9;
} ClipEditOp;

/* ffplayer */
struct IJKFF_Pipeline;
struct FFPlayer {
    const AVClass *av_class;

    /* ffplay context */
    ClipEditOp *clipState; //0x8
    AudioTrackEditOp *audioState; //0x10
    VideoState *is;

    volatile int abort_request; //0xc8

    /* format/codec options */
    AVDictionary *format_opts; //0xd0
    AVDictionary *codec_opts; //0xd8
    AVDictionary *sws_dict; //0xe0
    AVDictionary *player_opts; //0xe8
    AVDictionary *swr_opts; //0xf0
    AVDictionary *swr_preset_opts;

    /* ffplay options specified by the user */
#ifdef FFP_MERGE
    AVInputFormat *file_iformat;
#endif
    char *input_filename;
#ifdef FFP_MERGE
    const char *window_title;
    int fs_screen_width;
    int fs_screen_height;
    int default_width;
    int default_height;
    int screen_width;
    int screen_height;
#endif
    int audio_disable;
    int video_disable;
    int subtitle_disable;
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB];
    int seek_by_bytes; //0x130
    int display_disable; //0x134
    int show_status; //0x138
    int av_sync_type; //0x13c
    int f_0x140;
    int64_t start_time; //bo
    int64_t duration; //bo
    int fast; //0x144
    int genpts; //0x148
    int lowres; //0x14c
    int decoder_reorder_pts; //0x150
    int autoexit; //0x154
#ifdef FFP_MERGE
    int exit_on_keydown;
    int exit_on_mousedown;
#endif
    int loop; //0x158
    int framedrop; //0x15c
    int64_t seek_at_start;
    int subtitle;
    int infinite_buffer; //0x160
    enum ShowMode show_mode; //0x164
    char *audio_codec_name; //bo
    char *subtitle_codec_name; //bo
    char *video_codec_name; //bo
    double rdftspeed; //0x168
#ifdef FFP_MERGE
    int64_t cursor_last_shown; //bo
    int cursor_hidden; //bo
#endif
#if CONFIG_AVFILTER
    const char **vfilters_list; //0x170
    int nb_vfilters; //0x178
    char *afilters; //0x180
    char *vfilter0; //0x188
#endif
    int autorotate; //0x190
    int find_stream_info; //0x194
    unsigned sws_flags; //bo
    float fps_now;
    /* current context */
#ifdef FFP_MERGE
    int is_full_screen;
#endif
    int64_t audio_callback_time; //0x198
#ifdef FFP_MERGE
    SDL_Surface *screen;
#endif
    SDL_cond *empty_queue_next;
    /* extra fields */
    SDL_Aout *aout;
    SDL_Vout *vout;
    struct IJKFF_Pipeline *pipeline;
    struct IJKFF_Pipenode *node_vdec;
    int sar_num;
    int sar_den;

    char *video_codec_info;
    char *audio_codec_info;
    char *subtitle_codec_info;
    Uint32 overlay_format; //0x1a8

    int last_error; //0x1ac
    int prepared; //0x1b0
    volatile int auto_resume; //0x1b4
    int error; //0x1b8
    int error_count; //0x1bc
    int start_on_prepared; //0x1c0
    int first_video_frame_rendered; //0x1c4
    int first_audio_frame_rendered; //0x1c8
    int sync_av_start; //0x1cc

    MessageQueue msg_queue; //0x1d0 - 0x200

    int64_t playable_duration_ms; //0x208

    int packet_buffering; //0x210
    int pictq_size; //0x214
    int max_fps; //0x218
    int startup_volume;

    int videotoolbox;
    int vtb_max_frame_width;
    int vtb_async;
    int vtb_wait_async;
    int vtb_handle_resolution_change;

    int mediacodec_all_videos; //0x22c
    int mediacodec_avc; //0x230
    int mediacodec_hevc; //0x234
    int mediacodec_mpeg2; //0x238
    int mediacodec_mpeg4;
    int mediacodec_handle_resolution_change;
    int mediacodec_auto_rotate;

    int opensles; //0x240
    int soundtouch_enable; //0x244

    char *iformat_name;

    int no_time_adjust;
    double preset_5_1_center_mix_level;

    struct IjkMediaMeta *meta;

    SDL_SpeedSampler vfps_sampler; //0x250
    SDL_SpeedSampler vdps_sampler; //0x2b8

    /* filters */
    SDL_mutex  *vf_mutex;
    SDL_mutex  *af_mutex;
    int         vf_changed;
    int         af_changed;
    float       pf_playback_rate;
    int         pf_playback_rate_changed;
    float       pf_playback_volume;
    int         pf_playback_volume_changed;

    void               *inject_opaque;
    void               *ijkio_inject_opaque;
    FFStatistic         stat; //0x320, //avdiff: 0x334, avdelay: 0x330
    FFDemuxCacheControl dcc; //0x370 -> 0x38c

    AVApplicationContext *app_ctx;
    IjkIOManagerContext *ijkio_manager_ctx;

    int enable_accurate_seek;
    int accurate_seek_timeout;
    int mediacodec_sync;
    int skip_calc_frame_rate;
    int get_frame_mode;
    GetImgInfo *get_img_info;
    int async_init_decoder;
    char *video_mime_type;
    char *mediacodec_default_name;
    int ijkmeta_delay_init;
    int render_wait_start;

    //cac truong them moi
    int f_0x3a0;
    double f_0x3a8;
    double f_0x3b0;
    jobject img_loader_0x3b8;
    int f_0x400;
    Clock *clock_0x410;
    int64_t cur_clip_begin_timeline; //0x418
    long long pos_0x420;
    long long stream_offset_0x428;
    int64_t f_0x430;
    int64_t cur_clip_duration; //0x438
    float speed_0x440;
    int isSaveMode; //0x444
    int64_t f_0x448;
    int64_t f_0x450;
    int audio_only; //0x100
    SDL_Thread *thread_vout; //0x28
    SDL_Thread _thread_vout; //0x30
    bool         (*mediacodec_select_callback)(void *opaque, ijkmp_mediacodecinfo_context *mcc);
    void          *mediacodec_select_callback_opaque;
    SDL_mutex *mutex_0x18;
    SDL_cond *cond_0x20_4_8;
    SDL_mutex *mutex_0x408_0x81_0x102;

    int64_t f_0x3e8;
    int f_0x3f0;
    int f_0x3f4;
    int f_0x3f8;
    int f_0x3fc;

    volatile char flag_0x458;
    SDL_mutex *mutex_0x460;
    SDL_cond *cond_0x468;
};

#define fftime_to_milliseconds(ts) (av_rescale(ts, 1000, AV_TIME_BASE))
#define milliseconds_to_fftime(ms) (av_rescale(ms, AV_TIME_BASE, 1000))

inline static void ffp_reset_internal(FFPlayer *ffp)
{
    /* ffp->is closed in stream_close() */
    av_opt_free(ffp);

    /* format/codec options */
    av_dict_free(&ffp->format_opts);
    av_dict_free(&ffp->codec_opts);
    av_dict_free(&ffp->sws_dict);
    av_dict_free(&ffp->player_opts);
    av_dict_free(&ffp->swr_opts);
    av_dict_free(&ffp->swr_preset_opts);

    /* ffplay options specified by the user */
    av_freep(&ffp->input_filename);
    ffp->audio_disable          = 0;
    ffp->video_disable          = 0;
    memset(ffp->wanted_stream_spec, 0, sizeof(ffp->wanted_stream_spec));
    ffp->seek_by_bytes          = -1;
    ffp->display_disable        = 0;
    ffp->show_status            = 0;
    ffp->av_sync_type           = AV_SYNC_AUDIO_MASTER;
    ffp->start_time             = AV_NOPTS_VALUE;
    ffp->duration               = AV_NOPTS_VALUE;
    ffp->fast                   = 1;
    ffp->genpts                 = 0;
    ffp->lowres                 = 0;
    ffp->decoder_reorder_pts    = -1;
    ffp->autoexit               = 0;
    ffp->loop                   = 1;
    ffp->framedrop              = 0; // option
    ffp->seek_at_start          = 0;
    ffp->infinite_buffer        = -1;
    ffp->show_mode              = SHOW_MODE_NONE;
    av_freep(&ffp->audio_codec_name);
    av_freep(&ffp->video_codec_name);
    ffp->rdftspeed              = 0.02;
#if CONFIG_AVFILTER
    av_freep(&ffp->vfilters_list);
    ffp->nb_vfilters            = 0;
    ffp->afilters               = NULL;
    ffp->vfilter0               = NULL;
#endif
    ffp->autorotate             = 1;
    ffp->find_stream_info       = 1;

    ffp->sws_flags              = SWS_FAST_BILINEAR;

    /* current context */
    ffp->audio_callback_time    = 0;

    /* extra fields */
    ffp->aout                   = NULL; /* reset outside */
    ffp->vout                   = NULL; /* reset outside */
    ffp->pipeline               = NULL;
    ffp->node_vdec              = NULL;
    ffp->sar_num                = 0;
    ffp->sar_den                = 0;

    av_freep(&ffp->video_codec_info);
    av_freep(&ffp->audio_codec_info);
    av_freep(&ffp->subtitle_codec_info);
    ffp->overlay_format         = SDL_FCC_RV32;

    ffp->last_error             = 0;
    ffp->prepared               = 0;
    ffp->auto_resume            = 0;
    ffp->error                  = 0;
    ffp->error_count            = 0;
    ffp->start_on_prepared      = 1;
    ffp->first_video_frame_rendered = 0;
    ffp->sync_av_start          = 1;
    ffp->enable_accurate_seek   = 0;
    ffp->accurate_seek_timeout  = MAX_ACCURATE_SEEK_TIMEOUT;

    ffp->playable_duration_ms           = 0;

    ffp->packet_buffering               = 1;
    ffp->pictq_size                     = VIDEO_PICTURE_QUEUE_SIZE_DEFAULT; // option
    ffp->max_fps                        = 31; // option

    ffp->videotoolbox                   = 0; // option
    ffp->vtb_max_frame_width            = 0; // option
    ffp->vtb_async                      = 0; // option
    ffp->vtb_handle_resolution_change   = 0; // option
    ffp->vtb_wait_async                 = 0; // option

    ffp->mediacodec_all_videos          = 0; // option sonxxx
    ffp->mediacodec_avc                 = 0; // option
    ffp->mediacodec_hevc                = 0; // option
    ffp->mediacodec_mpeg2               = 0; // option
    ffp->mediacodec_handle_resolution_change = 0; // option
    ffp->mediacodec_auto_rotate         = 0; // option

    ffp->opensles                       = 0; // option
    ffp->soundtouch_enable              = 0; // option

    ffp->iformat_name                   = NULL; // option

    ffp->no_time_adjust                 = 0; // option
    ffp->async_init_decoder             = 0; // option
    ffp->video_mime_type                = NULL; // option
    ffp->mediacodec_default_name        = NULL; // option
    ffp->ijkmeta_delay_init             = 0; // option
    ffp->render_wait_start              = 0;

//    ijkmeta_reset(ffp->meta);

    SDL_SpeedSamplerReset(&ffp->vfps_sampler);
    SDL_SpeedSamplerReset(&ffp->vdps_sampler);

    ffp->f_0x3a0 = 0;

    /* filters */
    ffp->vf_changed                     = 0;
    ffp->af_changed                     = 0;
    ffp->pf_playback_rate               = 1.0f;
    ffp->pf_playback_rate_changed       = 0;
    ffp->pf_playback_volume             = 1.0f;
    ffp->pf_playback_volume_changed     = 0;

//    av_application_closep(&ffp->app_ctx);
//    ijkio_manager_destroyp(&ffp->ijkio_manager_ctx);

    msg_queue_flush(&ffp->msg_queue);

    ffp->inject_opaque = NULL;
    ffp->ijkio_inject_opaque = NULL;
    ffp_reset_statistic(&ffp->stat);
    ffp_reset_demux_cache_control(&ffp->dcc);
}

inline static void ffp_notify_msg1(FFPlayer *ffp, int what) {
    msg_queue_put_simple3(&ffp->msg_queue, what, 0, 0);
}

inline static void ffp_notify_msg2(FFPlayer *ffp, int what, int arg1) {
    msg_queue_put_simple3(&ffp->msg_queue, what, arg1, 0);
}

inline static void ffp_notify_msg3(FFPlayer *ffp, int what, int arg1, int arg2) {
    msg_queue_put_simple3(&ffp->msg_queue, what, arg1, arg2);
}

inline static void ffp_notify_msg4(FFPlayer *ffp, int what, int arg1, int arg2, void *obj, int obj_len) {
    msg_queue_put_simple4(&ffp->msg_queue, what, arg1, arg2, obj, obj_len);
}

inline static void ffp_remove_msg(FFPlayer *ffp, int what) {
    msg_queue_remove(&ffp->msg_queue, what);
}

inline static const char *ffp_get_error_string(int error) {
    switch (error) {
        case AVERROR(ENOMEM):       return "AVERROR(ENOMEM)";       // 12
        case AVERROR(EINVAL):       return "AVERROR(EINVAL)";       // 22
        case AVERROR(EAGAIN):       return "AVERROR(EAGAIN)";       // 35
        case AVERROR(ETIMEDOUT):    return "AVERROR(ETIMEDOUT)";    // 60
        case AVERROR_EOF:           return "AVERROR_EOF";
        case AVERROR_EXIT:          return "AVERROR_EXIT";
    }
    return "unknown";
}

#define FFTRACE ALOGW

#define AVCODEC_MODULE_NAME    "avcodec"
#define MEDIACODEC_MODULE_NAME "MediaCodec"

#endif
