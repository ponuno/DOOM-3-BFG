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

idCVar n3ds_soundVolume( "n3ds_soundVolume", "1", CVAR_SOUND | CVAR_FLOAT, "New 3DS: master volume of the DSP mix" );
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

static void N3DS_FeedCallback( void * user ) {
	( (idSoundHardware_3DS *)user )->FeedVoices();
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
	lastUpdateMs = 0;
	lastReportMs = 0;
	lastMasterVolume = -1.0f;
	underruns = 0;
	droppedVoices = 0;
	memset( channelUsed, 0, sizeof( channelUsed ) );
	memset( channelStarted, 0, sizeof( channelStarted ) );
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
	memset( channelUsed, 0, sizeof( channelUsed ) );
	lastUpdateMs = Sys_Milliseconds();
	lastReportMs = lastUpdateMs;

	audioOk = N3DSAudio_Init( N3DS_FeedCallback, this ) != 0;
	soundLockReady = audioOk;
	if ( audioOk ) {
		idLib::Printf( "Sound: NDSP, %d DSP channels mixed in hardware, %d x %d-frame buffers each\n",
			N3DS_AUDIO_CHANNELS, N3DS_AUDIO_BUFFERS, N3DS_AUDIO_FRAMES );
		if ( !fileSystem->FindFile( "_sound_pc.resources" ) ) {
			// the sound data lives in its own resource files
			idLib::Printf( "Sound: WARNING: no _sound_pc.resources in sdmc:/doom3/base -- copy _sound_pc.resources and _sound_pc_<lang>.resources from the PC game's base folder\n" );
		}
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
	for ( int i = 0; i < voices.Num(); i++ ) {
		voices[ i ].DestroyInternal();
	}
	if ( audioOk ) {
		N3DSAudio_Shutdown();
		audioOk = false;
	}
	soundLockReady = false;
	voices.Clear();
	freeVoices.Clear();
	zombieVoices.Clear();
}

/*
========================
idSoundHardware_3DS::AllocChannel / FreeChannel

Mixer lock held.
========================
*/
int idSoundHardware_3DS::AllocChannel() {
	for ( int ch = 0; ch < N3DS_AUDIO_CHANNELS; ch++ ) {
		if ( !channelUsed[ch] ) {
			channelUsed[ch] = true;
			channelStarted[ch] = false;
			return ch;
		}
	}
	return -1;
}

void idSoundHardware_3DS::FreeChannel( int ch ) {
	if ( ch >= 0 && ch < N3DS_AUDIO_CHANNELS ) {
		channelUsed[ch] = false;
		channelStarted[ch] = false;
	}
}

/*
========================
idSoundHardware_3DS::FeedVoices

Feeder thread: refill every playing voice's channel, one voice per lock hold so
the main thread never waits long.
========================
*/
void idSoundHardware_3DS::FeedVoices() {
	for ( int i = 0; i < voices.Num(); i++ ) {
		if ( !voices[i].playing ) {
			continue;
		}
		N3DS_SoundLock();
		voices[i].Feed();
		N3DS_SoundUnlock();
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
		const int ms = Min( now - lastUpdateMs, 1000 );
		if ( ms > 0 ) {
			for ( int i = 0; i < voices.Num(); i++ ) {
				voices[i].RunSilent( ms );
			}
		}
	} else {
		const float volume = idMath::ClampFloat( 0.0f, 4.0f, n3ds_soundVolume.GetFloat() );
		if ( volume != lastMasterVolume ) {
			N3DSAudio_SetMasterVolume( volume );
			lastMasterVolume = volume;
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
		int channels = 0;
		for ( int i = 0; i < voices.Num(); i++ ) {
			if ( voices[i].IsPlaying() ) {
				active++;
			}
		}
		for ( int ch = 0; ch < N3DS_AUDIO_CHANNELS; ch++ ) {
			channels += channelUsed[ch] ? 1 : 0;
		}
		idLib::Printf( "PERF   sound: %d voices playing on %d / %d DSP channels, feeding %.1f ms/s, %d underruns, %d voices without a channel, sample data %d KB, %d read on demand, %d dropped\n",
			active, channels, N3DS_AUDIO_CHANNELS, audioOk ? N3DSAudio_FeedMsPerSecond() : 0.0f, underruns, droppedVoices,
			N3DS_SoundDataKB(), N3DS_SoundLoadsOnDemand(), N3DS_SoundEvictions() );
		underruns = 0;
		droppedVoices = 0;
		lastReportMs = now;
	}
}
