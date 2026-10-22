/*
================================================================================================

	New 3DS audio output (NDSP). See n3ds_audio.h.

================================================================================================
*/
#include <3ds.h>
#include <string.h>

#include "n3ds_audio.h"

static const char *			lastError = "";
static int					initialized = 0;
static volatile int			running = 0;
static Thread				feedThread = NULL;
static LightLock			mixLock;
static LightEvent			frameEvent;
static n3dsFeedCallback_t	feedCallback = NULL;
static void *				feedUser = NULL;
static volatile u64			feedTicks = 0;
static u64					feedTicksReported = 0;
static u64					reportStart = 0;

// wave buffers and their data (linear memory: the DSP reads it)
static ndspWaveBuf			waveBufs[N3DS_AUDIO_CHANNELS][N3DS_AUDIO_BUFFERS];
static short *				waveData = NULL;

const char * N3DSAudio_LastError( void ) {
	return lastError;
}

void N3DSAudio_Lock( void ) {
	if ( initialized ) {
		LightLock_Lock( &mixLock );
	}
}

void N3DSAudio_Unlock( void ) {
	if ( initialized ) {
		LightLock_Unlock( &mixLock );
	}
}

float N3DSAudio_FeedMsPerSecond( void ) {
	const u64 now = svcGetSystemTick();
	const u64 ticks = feedTicks;
	const u64 elapsed = now - reportStart;
	float r = 0.0f;
	if ( elapsed > 0 ) {
		r = (float)( (double)( ticks - feedTicksReported ) / (double)elapsed * 1000.0 );
	}
	feedTicksReported = ticks;
	reportStart = now;
	return r;
}

void N3DSAudio_SetMasterVolume( float volume ) {
	if ( initialized ) {
		ndspSetMasterVol( volume );
	}
}

void N3DSAudio_ChnStart( int ch, int stereo, float rate ) {
	ndspChnWaveBufClear( ch );
	ndspChnReset( ch );
	ndspChnSetInterp( ch, NDSP_INTERP_POLYPHASE );
	ndspChnSetFormat( ch, stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16 );
	ndspChnSetRate( ch, rate );
	for ( int i = 0; i < N3DS_AUDIO_BUFFERS; i++ ) {
		waveBufs[ch][i].status = NDSP_WBUF_FREE;
	}
}

void N3DSAudio_ChnStop( int ch ) {
	ndspChnWaveBufClear( ch );
	ndspChnSetPaused( ch, false );
}

void N3DSAudio_ChnSetRate( int ch, float rate ) {
	ndspChnSetRate( ch, rate );
}

void N3DSAudio_ChnSetMix( int ch, float left, float right ) {
	float mix[12];
	memset( mix, 0, sizeof( mix ) );
	mix[0] = left;		// front left
	mix[1] = right;		// front right
	ndspChnSetMix( ch, mix );
}

void N3DSAudio_ChnSetPaused( int ch, int paused ) {
	ndspChnSetPaused( ch, paused != 0 );
}

short * N3DSAudio_BufData( int ch, int buf ) {
	return waveData + ( ch * N3DS_AUDIO_BUFFERS + buf ) * N3DS_AUDIO_FRAMES * 2;
}

int N3DSAudio_BufFree( int ch, int buf ) {
	const u8 s = waveBufs[ch][buf].status;
	return s == NDSP_WBUF_FREE || s == NDSP_WBUF_DONE;
}

int N3DSAudio_BufPlaying( int ch, int buf ) {
	return waveBufs[ch][buf].status == NDSP_WBUF_PLAYING;
}

void N3DSAudio_BufQueue( int ch, int buf, int frames, int stereo ) {
	ndspWaveBuf * wb = &waveBufs[ch][buf];
	wb->data_pcm16 = N3DSAudio_BufData( ch, buf );
	wb->nsamples = frames;
	wb->looping = false;
	DSP_FlushDataCache( wb->data_pcm16, frames * ( stereo ? 2 : 1 ) * sizeof( short ) );
	ndspChnWaveBufAdd( ch, wb );
}

int N3DSAudio_ChnBusy( int ch ) {
	for ( int i = 0; i < N3DS_AUDIO_BUFFERS; i++ ) {
		const u8 s = waveBufs[ch][i].status;
		if ( s == NDSP_WBUF_QUEUED || s == NDSP_WBUF_PLAYING ) {
			return 1;
		}
	}
	return 0;
}

// DSP frame callback (about every 5 ms): wake the feeder
static void N3DS_FrameCallback( void * data ) {
	(void)data;
	LightEvent_Signal( &frameEvent );
}

static void N3DS_FeedThread( void * arg ) {
	(void)arg;
	while ( running ) {
		const u64 start = svcGetSystemTick();
		feedCallback( feedUser );
		feedTicks += svcGetSystemTick() - start;
		LightEvent_Wait( &frameEvent );
	}
}

int N3DSAudio_Init( n3dsFeedCallback_t feed, void * user ) {
	if ( initialized ) {
		return 1;
	}
	if ( R_FAILED( ndspInit() ) ) {
		lastError = "ndspInit failed: no DSP firmware (dump it to sdmc:/3ds/dspfirm.cdc with DSP1)";
		return 0;
	}
	const int bytes = N3DS_AUDIO_CHANNELS * N3DS_AUDIO_BUFFERS * N3DS_AUDIO_FRAMES * 2 * sizeof( short );
	waveData = (short *)linearAlloc( bytes );
	if ( waveData == NULL ) {
		lastError = "no linear memory for the wave buffers";
		ndspExit();
		return 0;
	}
	memset( waveData, 0, bytes );
	memset( waveBufs, 0, sizeof( waveBufs ) );
	for ( int ch = 0; ch < N3DS_AUDIO_CHANNELS; ch++ ) {
		for ( int i = 0; i < N3DS_AUDIO_BUFFERS; i++ ) {
			waveBufs[ch][i].status = NDSP_WBUF_FREE;
		}
		ndspChnReset( ch );
	}
	ndspSetOutputMode( NDSP_OUTPUT_STEREO );
	ndspSetMasterVol( 1.0f );

	feedCallback = feed;
	feedUser = user;
	LightLock_Init( &mixLock );
	LightEvent_Init( &frameEvent, RESET_ONESHOT );
	initialized = 1;
	running = 1;
	reportStart = svcGetSystemTick();
	ndspSetCallback( N3DS_FrameCallback, NULL );

	// above the main and game threads; on the system core (core 1), which the app
	// gets a share of (n3ds_core1Percent); fall back to core 0
	feedThread = threadCreate( N3DS_FeedThread, NULL, 32 * 1024, 0x18, 1, false );
	if ( feedThread == NULL ) {
		feedThread = threadCreate( N3DS_FeedThread, NULL, 32 * 1024, 0x18, 0, false );
	}
	if ( feedThread == NULL ) {
		lastError = "could not create the feeder thread";
		running = 0;
		initialized = 0;
		ndspSetCallback( NULL, NULL );
		linearFree( waveData );
		waveData = NULL;
		ndspExit();
		return 0;
	}
	return 1;
}

void N3DSAudio_Shutdown( void ) {
	if ( !initialized ) {
		return;
	}
	running = 0;
	LightEvent_Signal( &frameEvent );
	threadJoin( feedThread, U64_MAX );
	threadFree( feedThread );
	feedThread = NULL;
	ndspSetCallback( NULL, NULL );
	for ( int ch = 0; ch < N3DS_AUDIO_CHANNELS; ch++ ) {
		ndspChnWaveBufClear( ch );
	}
	ndspExit();
	linearFree( waveData );
	waveData = NULL;
	initialized = 0;
}
