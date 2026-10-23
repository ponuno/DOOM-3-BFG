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
static volatile u32			feedCalls = 0;		// feeder runs (watchdog: is the feeder alive)
static u64					feedTicksReported = 0;
static u64					reportStart = 0;

// wave buffers and their data (linear memory: the DSP reads it)
static ndspWaveBuf			waveBufs[N3DS_AUDIO_CHANNELS][N3DS_AUDIO_BUFFERS];
// decoder state a DSP-ADPCM buffer starts with (read when NDSP hands the buffer to the DSP)
static ndspAdpcmData		adpcmState[N3DS_AUDIO_CHANNELS][N3DS_AUDIO_BUFFERS];
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

void N3DSAudio_Heartbeat( unsigned int * feeds, unsigned int * dspFrames ) {
	*feeds = feedCalls;
	*dspFrames = initialized ? ndspGetFrameCount() : 0;
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

void N3DSAudio_ChnStartAdpcm( int ch, float rate, const short * coefs ) {
	ndspChnWaveBufClear( ch );
	ndspChnReset( ch );
	ndspChnSetInterp( ch, NDSP_INTERP_POLYPHASE );
	ndspChnSetFormat( ch, NDSP_FORMAT_MONO_ADPCM );
	ndspChnSetRate( ch, rate );
	u16 c[16];
	memcpy( c, coefs, sizeof( c ) );
	ndspChnSetAdpcmCoefs( ch, c );
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
	wb->adpcm_data = NULL;
	DSP_FlushDataCache( wb->data_pcm16, frames * ( stereo ? 2 : 1 ) * sizeof( short ) );
	ndspChnWaveBufAdd( ch, wb );
}

void N3DSAudio_BufQueueAdpcm( int ch, int buf, const void * frames, int bytes, int samples,
		int setState, int index, int h0, int h1 ) {
	ndspWaveBuf * wb = &waveBufs[ch][buf];
	u8 * dst = (u8 *)N3DSAudio_BufData( ch, buf );
	memcpy( dst, frames, bytes );
	DSP_FlushDataCache( dst, bytes );
	wb->data_adpcm = dst;
	wb->nsamples = samples;
	wb->looping = false;
	if ( setState ) {
		adpcmState[ch][buf].index = (u16)index;
		adpcmState[ch][buf].history0 = (s16)h0;
		adpcmState[ch][buf].history1 = (s16)h1;
		wb->adpcm_data = &adpcmState[ch][buf];
	} else {
		wb->adpcm_data = NULL;
	}
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
		feedCalls++;
		LightEvent_Wait( &frameEvent );
	}
}

int N3DSAudio_Init( n3dsFeedCallback_t feed, void * user, int feederCore ) {
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

	// above the main and game threads, on the New 3DS's extra core (core 2, the game
	// thread's), falling back to core 0. It used to run on the system core (core 1),
	// which the app only gets a time share of (n3ds_core1Percent): the feeder was
	// descheduled while it held the sound lock for a voice, and the main thread, updating
	// the voices in soundSystem->Render, waited 10-16 ms per frame for it (SPIKE lines,
	// "lock waits") - frames over 33 ms whenever several sounds played. On core 2 it runs
	// to the end of each refill (a few short copies) before anything else there.
	feedThread = threadCreate( N3DS_FeedThread, NULL, 32 * 1024, 0x18, feederCore, false );
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

// the feeder thread to the lowest priority while the game is in the HOME menu / asleep
void N3DSAudio_SetBackground( int background ) {
	if ( feedThread != NULL ) {
		svcSetThreadPriority( threadGetHandle( feedThread ), background ? 0x3F : 0x18 );
	}
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
