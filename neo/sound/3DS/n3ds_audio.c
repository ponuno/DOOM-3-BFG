/*
================================================================================================

	New 3DS audio output (NDSP). See n3ds_audio.h.

================================================================================================
*/
#include <3ds.h>
#include <string.h>

#include "n3ds_audio.h"

#define MAX_WAVE_BUFFERS	8

static const char *			lastError = "";
static int					initialized = 0;
static volatile int			running = 0;
static Thread				mixThread = NULL;
static LightLock			mixLock;
static LightEvent			frameEvent;
static ndspWaveBuf			waveBufs[MAX_WAVE_BUFFERS];
static short *				waveData = NULL;
static int					numBufs = 0;
static int					bufFrames = 0;
static n3dsMixCallback_t	mixCallback = NULL;
static void *				mixUser = NULL;
static volatile int			underruns = 0;
static volatile u64			mixTicks = 0;
static u64					mixTicksReported = 0;
static u64					reportStart = 0;

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

int N3DSAudio_Underruns( void ) {
	return underruns;
}

float N3DSAudio_MixMsPerSecond( void ) {
	const u64 now = svcGetSystemTick();
	const u64 ticks = mixTicks;
	const u64 elapsed = now - reportStart;
	float r = 0.0f;
	if ( elapsed > 0 ) {
		// ms of mixing per second of wall time
		r = (float)( (double)( ticks - mixTicksReported ) / (double)elapsed * 1000.0 );
	}
	mixTicksReported = ticks;
	reportStart = now;
	return r;
}

// DSP frame callback (about every 5 ms): wake the mixer
static void N3DS_FrameCallback( void * data ) {
	(void)data;
	LightEvent_Signal( &frameEvent );
}

static void N3DS_FillBuffer( ndspWaveBuf * buf ) {
	const u64 start = svcGetSystemTick();
	// the callback takes the mixer lock itself, per voice, so the main thread never
	// waits long (this thread can be preempted on the time-limited system core)
	mixCallback( buf->data_pcm16, bufFrames, mixUser );
	DSP_FlushDataCache( buf->data_pcm16, bufFrames * 2 * sizeof( short ) );
	ndspChnWaveBufAdd( 0, buf );
	mixTicks += svcGetSystemTick() - start;
}

static void N3DS_MixThread( void * arg ) {
	(void)arg;
	while ( running ) {
		int queued = 0;
		for ( int i = 0; i < numBufs; i++ ) {
			ndspWaveBuf * buf = &waveBufs[i];
			if ( buf->status == NDSP_WBUF_DONE || buf->status == NDSP_WBUF_FREE ) {
				N3DS_FillBuffer( buf );
			} else {
				queued++;
			}
		}
		if ( queued == 0 ) {
			underruns++;		// every buffer had played out: the DSP was starved
		}
		LightEvent_Wait( &frameEvent );
	}
}

int N3DSAudio_Init( int sampleRate, int framesPerBuffer, int numBuffers, n3dsMixCallback_t mix, void * user ) {
	if ( initialized ) {
		return 1;
	}
	if ( numBuffers < 2 ) {
		numBuffers = 2;
	}
	if ( numBuffers > MAX_WAVE_BUFFERS ) {
		numBuffers = MAX_WAVE_BUFFERS;
	}
	if ( R_FAILED( ndspInit() ) ) {
		lastError = "ndspInit failed: no DSP firmware (dump it to sdmc:/3ds/dspfirm.cdc with DSP1)";
		return 0;
	}
	numBufs = numBuffers;
	bufFrames = framesPerBuffer;
	mixCallback = mix;
	mixUser = user;

	waveData = (short *)linearAlloc( numBufs * bufFrames * 2 * sizeof( short ) );
	if ( waveData == NULL ) {
		lastError = "no linear memory for the wave buffers";
		ndspExit();
		return 0;
	}
	memset( waveData, 0, numBufs * bufFrames * 2 * sizeof( short ) );

	ndspSetOutputMode( NDSP_OUTPUT_STEREO );
	ndspChnReset( 0 );
	ndspChnSetInterp( 0, NDSP_INTERP_LINEAR );
	ndspChnSetRate( 0, (float)sampleRate );
	ndspChnSetFormat( 0, NDSP_FORMAT_STEREO_PCM16 );
	float mixLevels[12];
	memset( mixLevels, 0, sizeof( mixLevels ) );
	mixLevels[0] = 1.0f;		// front left
	mixLevels[1] = 1.0f;		// front right
	ndspChnSetMix( 0, mixLevels );

	memset( waveBufs, 0, sizeof( waveBufs ) );
	for ( int i = 0; i < numBufs; i++ ) {
		waveBufs[i].data_pcm16 = waveData + i * bufFrames * 2;
		waveBufs[i].nsamples = bufFrames;
		waveBufs[i].status = NDSP_WBUF_FREE;
	}

	LightLock_Init( &mixLock );
	LightEvent_Init( &frameEvent, RESET_ONESHOT );
	initialized = 1;
	running = 1;
	reportStart = svcGetSystemTick();
	ndspSetCallback( N3DS_FrameCallback, NULL );

	// above the main and game threads; on the system core (core 1), which the app
	// gets a share of (n3ds_core1Percent); fall back to core 0
	mixThread = threadCreate( N3DS_MixThread, NULL, 32 * 1024, 0x18, 1, false );
	if ( mixThread == NULL ) {
		mixThread = threadCreate( N3DS_MixThread, NULL, 32 * 1024, 0x18, 0, false );
	}
	if ( mixThread == NULL ) {
		lastError = "could not create the mixer thread";
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
	threadJoin( mixThread, U64_MAX );
	threadFree( mixThread );
	mixThread = NULL;
	ndspSetCallback( NULL, NULL );
	ndspChnWaveBufClear( 0 );
	ndspExit();
	linearFree( waveData );
	waveData = NULL;
	initialized = 0;
}
