/*
================================================================================================

	New 3DS audio output (NDSP), plain C interface.

	n3ds_audio.c is the only sound file that includes <3ds.h>. One NDSP channel plays
	stereo 16-bit PCM; a mixer thread on the system core keeps a few wave buffers
	queued and fills each one through the callback (the engine's software mixer,
	3DS_SoundHardware.cpp). The callback runs on the mixer thread with the mixer
	lock held; the engine takes the same lock to change voices.

================================================================================================
*/
#ifndef __N3DS_AUDIO_H__
#define __N3DS_AUDIO_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef void ( *n3dsMixCallback_t )( short * out, int frames, void * user );

// 1 on success; on failure N3DSAudio_LastError says why (no DSP firmware, ...)
int				N3DSAudio_Init( int sampleRate, int framesPerBuffer, int numBuffers, n3dsMixCallback_t mix, void * user );
void			N3DSAudio_Shutdown( void );
const char *	N3DSAudio_LastError( void );

void			N3DSAudio_Lock( void );
void			N3DSAudio_Unlock( void );

// diagnostics: buffers that ran dry (the DSP played silence), mixing time
int				N3DSAudio_Underruns( void );
float			N3DSAudio_MixMsPerSecond( void );		// since the last call

#ifdef __cplusplus
}
#endif

#endif
