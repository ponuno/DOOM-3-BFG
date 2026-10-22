/*
================================================================================================

	New 3DS audio output (NDSP), plain C interface.

	n3ds_audio.c is the only sound file that includes <3ds.h>. Every engine voice that
	plays gets one of the DSP's 24 channels: the DSP resamples it (any rate, pitch is the
	channel rate), applies its left/right levels and mixes all channels in hardware. The
	CPU only fills each channel's few PCM16 wave buffers (decoding MS ADPCM on the way).

	A feeder thread on the system core is woken by the DSP every audio frame (~5 ms) and
	calls the feed callback, which refills the buffers the DSP has finished. The feeder
	takes the mixer lock around each voice; the engine takes the same lock to start /
	stop voices and to free sample data.

================================================================================================
*/
#ifndef __N3DS_AUDIO_H__
#define __N3DS_AUDIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#define N3DS_AUDIO_CHANNELS		24		// DSP channels
#define N3DS_AUDIO_BUFFERS		3		// wave buffers per channel
#define N3DS_AUDIO_FRAMES		1024	// frames per wave buffer (stereo PCM16 room)
// DSP-ADPCM (sound pack): 8-byte frames of 14 samples, up to this many frames per wave buffer
// (1168 bytes of the 4096 a buffer has; 2044 samples, 93 ms at 22 kHz)
#define N3DS_AUDIO_ADPCM_FRAMES	146

typedef void ( *n3dsFeedCallback_t )( void * user );

// 1 on success; on failure N3DSAudio_LastError says why (no DSP firmware, ...)
int				N3DSAudio_Init( n3dsFeedCallback_t feed, void * user );
void			N3DSAudio_Shutdown( void );
const char *	N3DSAudio_LastError( void );

void			N3DSAudio_Lock( void );
void			N3DSAudio_Unlock( void );

void			N3DSAudio_SetMasterVolume( float volume );

// channels (callers own a channel between Start and Stop)
void			N3DSAudio_ChnStart( int ch, int stereo, float rate );		// clears and configures
// a mono DSP-ADPCM channel with the 8 predictor pairs (c1, c2) of the sound pack
void			N3DSAudio_ChnStartAdpcm( int ch, float rate, const short * coefs );
void			N3DSAudio_ChnStop( int ch );								// drops everything queued
void			N3DSAudio_ChnSetRate( int ch, float rate );
void			N3DSAudio_ChnSetMix( int ch, float left, float right );
void			N3DSAudio_ChnSetPaused( int ch, int paused );

// wave buffers of a channel: 'free' ones (never queued or played out) can be filled
short *			N3DSAudio_BufData( int ch, int buf );						// N3DS_AUDIO_FRAMES stereo frames
int				N3DSAudio_BufFree( int ch, int buf );
int				N3DSAudio_BufPlaying( int ch, int buf );
void			N3DSAudio_BufQueue( int ch, int buf, int frames, int stereo );
// copies 'bytes' of DSP-ADPCM frames into the buffer and queues 'samples' samples of them;
// setState: the decoder starts from (index = frame header, history h0, h1), else it goes on
// from the previous buffer
void			N3DSAudio_BufQueueAdpcm( int ch, int buf, const void * frames, int bytes, int samples,
										 int setState, int index, int h0, int h1 );
int				N3DSAudio_ChnBusy( int ch );								// something queued or playing

// diagnostics
float			N3DSAudio_FeedMsPerSecond( void );		// feeder time per second of wall time, since the last call

#ifdef __cplusplus
}
#endif

#endif
