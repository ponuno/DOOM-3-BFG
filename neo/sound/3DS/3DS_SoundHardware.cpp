/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/
#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../snd_local.h"
#include "n3ds_audio.h"

idCVar n3ds_soundRate( "n3ds_soundRate", "22050", CVAR_SOUND | CVAR_INTEGER | CVAR_INIT, "New 3DS: mixing rate in Hz", 8000, 48000 );
idCVar n3ds_soundBufferFrames( "n3ds_soundBufferFrames", "512", CVAR_SOUND | CVAR_INTEGER | CVAR_INIT, "New 3DS: frames per output buffer (4 are queued)", 128, 4096 );
idCVar n3ds_soundVolume( "n3ds_soundVolume", "1", CVAR_SOUND | CVAR_FLOAT, "New 3DS: master volume of the mixer" );
idCVar n3ds_soundStats( "n3ds_soundStats", "1", CVAR_SOUND | CVAR_BOOL, "New 3DS: log a sound line every 2 s with the PERF lines" );

static bool soundLockReady = false;

void N3DS_SoundLock() {
	if ( soundLockReady ) {
		N3DSAudio_Lock();
	}
}

void N3DS_SoundUnlock() {
	if ( soundLockReady ) {
		N3DSAudio_Unlock();
	}
}

static void N3DS_MixCallback( short * out, int frames, void * user ) {
	( (idSoundHardware_3DS *)user )->MixBlock( out, frames );
}

/*
========================
idSoundHardware_3DS::idSoundHardware_3DS
========================
*/
idSoundHardware_3DS::idSoundHardware_3DS() {
	outputChannels = 2;
	channelMask = 0;
	audioOk = false;
	outputRate = 22050;
	lastUpdateMs = 0;
	lastReportMs = 0;
	mixBuffer = NULL;
	mixBufferFrames = 0;
	voices.SetNum( 0 );
	zombieVoices.SetNum( 0 );
	freeVoices.SetNum( 0 );
}

/*
========================
idSoundHardware_3DS::Init
========================
*/
void idSoundHardware_3DS::Init() {
	// New 3DS: stereo speakers / headphones
	outputChannels = 2;
	channelMask = ( 1 << idWaveFile::CHANNEL_INDEX_FRONT_LEFT ) | ( 1 << idWaveFile::CHANNEL_INDEX_FRONT_RIGHT );
	idSoundVoice::InitSurround( outputChannels, channelMask );

	voices.SetNum( voices.Max() );
	freeVoices.SetNum( voices.Max() );
	zombieVoices.SetNum( 0 );
	for ( int i = 0; i < voices.Num(); i++ ) {
		freeVoices[i] = &voices[i];
	}

	outputRate = n3ds_soundRate.GetInteger();
	mixBufferFrames = Max( n3ds_soundBufferFrames.GetInteger(), 4096 );
	mixBuffer = (int *)Mem_Alloc( mixBufferFrames * 2 * sizeof( int ), TAG_AUDIO );
	lastUpdateMs = Sys_Milliseconds();
	lastReportMs = lastUpdateMs;

	audioOk = N3DSAudio_Init( outputRate, n3ds_soundBufferFrames.GetInteger(), 4, N3DS_MixCallback, this ) != 0;
	soundLockReady = audioOk;
	if ( audioOk ) {
		idLib::Printf( "Sound: NDSP, software mixer at %d Hz, %d voices, %d frames x 4 buffers\n",
			outputRate, voices.Num(), n3ds_soundBufferFrames.GetInteger() );
	} else {
		idLib::Printf( "Sound: no output (%s), %d silent voices\n", N3DSAudio_LastError(), voices.Num() );
	}
}

/*
========================
idSoundHardware_3DS::Shutdown
========================
*/
void idSoundHardware_3DS::Shutdown() {
	if ( audioOk ) {
		N3DSAudio_Shutdown();
		audioOk = false;
	}
	soundLockReady = false;
	for ( int i = 0; i < voices.Num(); i++ ) {
		voices[ i ].DestroyInternal();
	}
	voices.Clear();
	freeVoices.Clear();
	zombieVoices.Clear();
	Mem_Free( mixBuffer );
	mixBuffer = NULL;
}

/*
========================
idSoundHardware_3DS::MixBlock

Mixer thread, mixer lock held.
========================
*/
void idSoundHardware_3DS::MixBlock( short * out, int frames ) {
	if ( frames > mixBufferFrames || mixBuffer == NULL ) {
		memset( out, 0, frames * 2 * sizeof( short ) );
		return;
	}
	memset( mixBuffer, 0, frames * 2 * sizeof( int ) );
	for ( int i = 0; i < voices.Num(); i++ ) {
		voices[i].Mix( mixBuffer, frames, outputRate );
	}
	const int master = (int)( idMath::ClampFloat( 0.0f, 4.0f, n3ds_soundVolume.GetFloat() ) * 256.0f );
	for ( int i = 0; i < frames * 2; i++ ) {
		int v = ( mixBuffer[i] * master ) >> 8;
		if ( v > 32767 ) {
			v = 32767;
		} else if ( v < -32768 ) {
			v = -32768;
		}
		out[i] = (short)v;
	}
}

/*
========================
idSoundHardware_3DS::AllocateVoice
========================
*/
idSoundVoice * idSoundHardware_3DS::AllocateVoice( const idSoundSample * leadinSample, const idSoundSample * loopingSample ) {
	if ( leadinSample == NULL ) {
		return NULL;
	}
	if ( loopingSample != NULL ) {
		if ( ( leadinSample->format.basic.formatTag != loopingSample->format.basic.formatTag ) || ( leadinSample->format.basic.numChannels != loopingSample->format.basic.numChannels ) ) {
			idLib::Warning( "Leadin/looping format mismatch: %s & %s", leadinSample->GetName(), loopingSample->GetName() );
			loopingSample = NULL;
		}
	}

	for ( int i = 0; i < freeVoices.Num(); i++ ) {
		if ( freeVoices[i]->IsPlaying() ) {
			continue;
		}
		idSoundVoice * voice = (idSoundVoice *)freeVoices[i];
		voice->Create( leadinSample, loopingSample );
		freeVoices.RemoveIndex( i );
		return voice;
	}
	return NULL;
}

/*
========================
idSoundHardware_3DS::FreeVoice
========================
*/
void idSoundHardware_3DS::FreeVoice( idSoundVoice * voice ) {
	voice->Stop();
	zombieVoices.Append( voice );
}

/*
========================
idSoundHardware_3DS::Update
========================
*/
void idSoundHardware_3DS::Update() {
	const int now = Sys_Milliseconds();
	if ( !audioOk ) {
		// no output: run the voices on the clock so that sounds still end
		const int frames = Min( ( now - lastUpdateMs ) * outputRate / 1000, mixBufferFrames );
		if ( frames > 0 && mixBuffer != NULL ) {
			memset( mixBuffer, 0, frames * 2 * sizeof( int ) );
			for ( int i = 0; i < voices.Num(); i++ ) {
				voices[i].Mix( mixBuffer, frames, outputRate );
			}
		}
	}
	lastUpdateMs = now;

	for ( int i = 0; i < zombieVoices.Num(); i++ ) {
		zombieVoices[i]->FlushSourceBuffers();
		if ( !zombieVoices[i]->IsPlaying() ) {
			freeVoices.Append( zombieVoices[i] );
			zombieVoices.RemoveIndexFast( i );
			i--;
		}
	}

	if ( n3ds_soundStats.GetBool() && now - lastReportMs >= 2000 ) {
		int active = 0;
		for ( int i = 0; i < voices.Num(); i++ ) {
			if ( voices[i].IsPlaying() ) {
				active++;
			}
		}
		idLib::Printf( "PERF   sound: %d voices playing, mixing %.1f ms/s, %d underruns, sample data %d KB, %d read on demand, %d dropped\n",
			active, audioOk ? N3DSAudio_MixMsPerSecond() : 0.0f, audioOk ? N3DSAudio_Underruns() : 0,
			N3DS_SoundDataKB(), N3DS_SoundLoadsOnDemand(), N3DS_SoundEvictions() );
		lastReportMs = now;
	}
}
