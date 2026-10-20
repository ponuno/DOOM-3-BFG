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

/*
========================
idSoundHardware_3DS::idSoundHardware_3DS
========================
*/
idSoundHardware_3DS::idSoundHardware_3DS() {
	outputChannels = 2;
	channelMask = 0;
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
	idLib::Printf( "Sound: null 3DS device, %d voices\n", voices.Num() );
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
	voices.Clear();
	freeVoices.Clear();
	zombieVoices.Clear();
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

	// Try to find a free voice that matches the format
	// But fallback to the last free voice if none match the format
	idSoundVoice * voice = NULL;
	for ( int i = 0; i < freeVoices.Num(); i++ ) {
		if ( freeVoices[i]->IsPlaying() ) {
			continue;
		}
		voice = (idSoundVoice *)freeVoices[i];
		if ( voice->CompatibleFormat( (idSoundSample_3DS*)leadinSample ) ) {
			break;
		}
	}
	if ( voice != NULL ) {
		voice->Create( leadinSample, loopingSample );
		freeVoices.Remove( voice );
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
	for ( int i = 0; i < zombieVoices.Num(); i++ ) {
		zombieVoices[i]->FlushSourceBuffers();
		if ( !zombieVoices[i]->IsPlaying() ) {
			freeVoices.Append( zombieVoices[i] );
			zombieVoices.RemoveIndexFast( i );
			i--;
		}
	}
}
