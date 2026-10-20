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
idSoundVoice_3DS::idSoundVoice_3DS
========================
*/
idSoundVoice_3DS::idSoundVoice_3DS()
	:	leadinSample( NULL ),
		loopingSample( NULL ),
		formatTag( 0 ),
		numChannels( 0 ),
		sampleRate( 0 ),
		hasVUMeter( false ),
		paused( true ),
		playing( false ),
		startTime( 0 ),
		endTime( 0 ) {
}

/*
========================
idSoundVoice_3DS::~idSoundVoice_3DS
========================
*/
idSoundVoice_3DS::~idSoundVoice_3DS() {
	DestroyInternal();
	leadinSample = NULL;
	loopingSample = NULL;
}

/*
========================
idSoundVoice_3DS::CompatibleFormat
========================
*/
bool idSoundVoice_3DS::CompatibleFormat( idSoundSample_3DS * s ) {
	// the null device has no per-format hardware state
	return ( formatTag == s->format.basic.formatTag && numChannels == s->format.basic.numChannels ) || formatTag == 0;
}

/*
========================
idSoundVoice_3DS::Create
========================
*/
void idSoundVoice_3DS::Create( const idSoundSample * leadinSample_, const idSoundSample * loopingSample_ ) {
	if ( IsPlaying() ) {
		// This should never hit
		Stop();
		return;
	}
	leadinSample = (idSoundSample_3DS *)leadinSample_;
	loopingSample = (idSoundSample_3DS *)loopingSample_;

	formatTag = leadinSample->format.basic.formatTag;
	numChannels = leadinSample->format.basic.numChannels;
	sampleRate = leadinSample->format.basic.samplesPerSec;
}

/*
========================
idSoundVoice_3DS::DestroyInternal
========================
*/
void idSoundVoice_3DS::DestroyInternal() {
	playing = false;
	paused = true;
	formatTag = 0;
}

/*
========================
idSoundVoice_3DS::Start
========================
*/
void idSoundVoice_3DS::Start( int offsetMS, int ssFlags ) {
	if ( leadinSample == NULL ) {
		return;
	}
	hasVUMeter = ( ssFlags & SSF_NO_FLICKER ) == 0;

	startTime = Sys_Milliseconds() - offsetMS;
	if ( loopingSample != NULL || ( ssFlags & SSF_LOOPING ) ) {
		endTime = 0;
	} else {
		endTime = startTime + leadinSample->LengthInMsec();
	}
	playing = true;
	paused = false;
	Update();
}

/*
========================
idSoundVoice_3DS::Update
========================
*/
bool idSoundVoice_3DS::Update() {
	if ( !playing ) {
		return false;
	}
	return IsPlaying();
}

/*
========================
idSoundVoice_3DS::IsPlaying
========================
*/
bool idSoundVoice_3DS::IsPlaying() {
	if ( !playing ) {
		return false;
	}
	if ( endTime != 0 && !paused && Sys_Milliseconds() >= endTime ) {
		playing = false;
	}
	return playing;
}

/*
========================
idSoundVoice_3DS::FlushSourceBuffers
========================
*/
void idSoundVoice_3DS::FlushSourceBuffers() {
}

/*
========================
idSoundVoice_3DS::Pause
========================
*/
void idSoundVoice_3DS::Pause() {
	if ( !playing || paused ) {
		return;
	}
	paused = true;
}

/*
========================
idSoundVoice_3DS::UnPause
========================
*/
void idSoundVoice_3DS::UnPause() {
	if ( !playing || !paused ) {
		return;
	}
	paused = false;
}

/*
========================
idSoundVoice_3DS::Stop
========================
*/
void idSoundVoice_3DS::Stop() {
	playing = false;
	paused = true;
}

/*
========================
idSoundVoice_3DS::GetAmplitude
========================
*/
float idSoundVoice_3DS::GetAmplitude() {
	return 1.0f;
}
