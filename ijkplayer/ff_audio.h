#ifndef FF_AUDIO_H
#define FF_AUDIO_H
#include "ff_ffplay_def.h"
#include "ff_ffplay.h"

int audio_track_op_add(AudioTrackInfo *track, int index, AudioClip *clip);
void audio_track_op_close(AudioTrackEditOp *audioState);
int audio_track_op_delete(AudioTrackInfo *track, int index);
void audio_track_seek(AudioTrackEditOp *audioState, long long pos);
int audio_accurate_seek(AVFormatContext *fmtCtx,int stream_index, int64_t pos);
int audio_track_swap_to_next_clip(AudioTrackInfo *track);
AudioClip *audio_track_op_remove(AudioTrackInfo *track, int index);
#endif // FF_AUDIO_H
