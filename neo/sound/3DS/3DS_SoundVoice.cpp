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
MS ADPCM
========================
*/
static const int adpcmAdapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
static const int adpcmDefaultCoef[7][2] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 } };

static inline int AdpcmNibble( int nibble, int & s1, int & s2, int & delta, int c1, int c2 ) {
	const int signedNibble = ( nibble & 8 ) ? nibble - 16 : nibble;
	int pred = ( s1 * c1 + s2 * c2 ) >> 8;
	pred += signedNibble * delta;
	if ( pred > 32767 ) {
		pred = 32767;
	} else if ( pred < -32768 ) {
		pred = -32768;
	}
	s2 = s1;
	s1 = pred;
	delta = ( adpcmAdapt[nibble] * delta ) >> 8;
	if ( delta < 16 ) {
		delta = 16;
	}
	return pred;
}

static inline short ReadS16( const byte * p ) {
	return (short)( p[0] | ( p[1] << 8 ) );
}

// decodes one block into out[samplesPerBlock * channels] (interleaved)
static void DecodeAdpcmBlock( const idWaveFile::waveFmt_t & fmt, const byte * block, int blockBytes, short * out ) {
	const int channels = fmt.basic.numChannels;
	const int spb = fmt.extra.adpcm.samplesPerBlock;
	int coef[2][2];
	int delta[2], s1[2], s2[2];
	const byte * p = block;
	for ( int c = 0; c < channels; c++ ) {
		int pred = p[c];
		if ( pred > 6 ) {
			pred = 6;
		}
		if ( fmt.extra.adpcm.numCoef == 7 ) {
			coef[c][0] = fmt.extra.adpcm.aCoef[pred].coef1;
			coef[c][1] = fmt.extra.adpcm.aCoef[pred].coef2;
		} else {
			coef[c][0] = adpcmDefaultCoef[pred][0];
			coef[c][1] = adpcmDefaultCoef[pred][1];
		}
	}
	p += channels;
	for ( int c = 0; c < channels; c++ ) {
		delta[c] = ReadS16( p + c * 2 );
	}
	p += channels * 2;
	for ( int c = 0; c < channels; c++ ) {
		s1[c] = ReadS16( p + c * 2 );
	}
	p += channels * 2;
	for ( int c = 0; c < channels; c++ ) {
		s2[c] = ReadS16( p + c * 2 );
	}
	p += channels * 2;
	// the two header samples come first, oldest first
	for ( int c = 0; c < channels; c++ ) {
		out[c] = (short)s2[c];
		out[channels + c] = (short)s1[c];
	}
	const int headerBytes = 7 * channels;
	const byte * end = block + blockBytes;
	int n = 2 * channels;
	const int total = spb * channels;
	int c = 0;
	for ( ; p < end && n < total; p++ ) {
		out[n++] = (short)AdpcmNibble( *p >> 4, s1[c], s2[c], delta[c], coef[c][0], coef[c][1] );
		c = ( c + 1 ) % channels;
		if ( n >= total ) {
			break;
		}
		out[n++] = (short)AdpcmNibble( *p & 15, s1[c], s2[c], delta[c], coef[c][0], coef[c][1] );
		c = ( c + 1 ) % channels;
	}
	(void)headerBytes;
	for ( ; n < total; n++ ) {
		out[n] = 0;		// short last block
	}
}

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
		pendingStart( false ),
		pendingOffsetMS( 0 ),
		pendingFlags( 0 ),
		pendingSince( 0 ),
		current( NULL ),
		position( 0 ),
		pitchScale( 1.0f ),
		amplitude( 0.0f ),
		cacheNext( 0 ),
		cacheSize( 0 ) {
	memset( levels, 0, sizeof( levels ) );
	for ( int i = 0; i < ADPCM_CACHE; i++ ) {
		cacheSample[i] = NULL;
		cacheBlock[i] = -1;
		cacheData[i] = NULL;
	}
}

/*
========================
idSoundVoice_3DS::~idSoundVoice_3DS
========================
*/
idSoundVoice_3DS::~idSoundVoice_3DS() {
	DestroyInternal();
	for ( int i = 0; i < ADPCM_CACHE; i++ ) {
		Mem_Free( cacheData[i] );
		cacheData[i] = NULL;
	}
}

/*
========================
idSoundVoice_3DS::CompatibleFormat
========================
*/
bool idSoundVoice_3DS::CompatibleFormat( idSoundSample_3DS * s ) {
	// a software voice plays any format
	return true;
}

/*
========================
idSoundVoice_3DS::SetSamples

Keeps the samples' voice counts (a sample a voice holds keeps its data).
========================
*/
void idSoundVoice_3DS::SetSamples( idSoundSample_3DS * leadin, idSoundSample_3DS * looping ) {
	N3DS_SoundLock();
	if ( leadinSample != NULL ) {
		leadinSample->activeVoices--;
	}
	if ( loopingSample != NULL ) {
		loopingSample->activeVoices--;
	}
	leadinSample = leadin;
	loopingSample = looping;
	if ( leadinSample != NULL ) {
		leadinSample->activeVoices++;
	}
	if ( loopingSample != NULL ) {
		loopingSample->activeVoices++;
	}
	current = NULL;
	playing = false;
	paused = true;
	for ( int i = 0; i < ADPCM_CACHE; i++ ) {
		cacheSample[i] = NULL;
		cacheBlock[i] = -1;
	}
	N3DS_SoundUnlock();
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
	SetSamples( (idSoundSample_3DS *)leadinSample_, (idSoundSample_3DS *)loopingSample_ );
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
	pendingStart = false;
	SetSamples( NULL, NULL );
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

	// a sample whose file is missing is the default beep on the PC; here it stays silent
	if ( leadinSample->IsDefault() ) {
		static int warned = 0;
		if ( warned++ < 5 ) {
			idLib::Printf( "Sound: no data for %s (missing from the resources), not played\n", leadinSample->GetName() );
		}
		return;
	}
	// sample data not in memory is read by the loader thread; the voice starts when it is
	const int leadin = leadinSample->PrepareData();
	if ( leadin < 0 ) {
		return;
	}
	int looping = 1;
	if ( loopingSample != NULL && loopingSample != leadinSample ) {
		looping = loopingSample->PrepareData();
	}
	if ( leadin == 0 || looping == 0 ) {
		pendingStart = true;
		pendingOffsetMS = offsetMS;
		pendingFlags = ssFlags;
		pendingSince = Sys_Milliseconds();
		return;
	}
	if ( looping < 0 ) {
		SetSamples( leadinSample, NULL );
	}
	StartNow( offsetMS, ssFlags );
}

/*
========================
idSoundVoice_3DS::StartNow

Start() with the data in memory.
========================
*/
void idSoundVoice_3DS::StartNow( int offsetMS, int ssFlags ) {
	pendingStart = false;
	if ( leadinSample == NULL ) {
		return;
	}
	if ( leadinSample->format.basic.formatTag != idWaveFile::FORMAT_PCM && leadinSample->format.basic.formatTag != idWaveFile::FORMAT_ADPCM ) {
		return;
	}

	int offsetSamples = MsecToSamples( Max( offsetMS, 0 ), leadinSample->SampleRate() );
	idSoundSample_3DS * sample = leadinSample;
	if ( offsetSamples >= leadinSample->playLength ) {
		if ( loopingSample == NULL || loopingSample->playLength <= 0 ) {
			return;
		}
		offsetSamples %= loopingSample->playLength;
		sample = loopingSample;
	}

	// size the ADPCM cache for the biggest block of the two samples
	int need = 0;
	for ( int k = 0; k < 2; k++ ) {
		const idSoundSample_3DS * s = ( k == 0 ) ? leadinSample : loopingSample;
		if ( s != NULL && s->format.basic.formatTag == idWaveFile::FORMAT_ADPCM ) {
			need = Max( need, (int)s->format.extra.adpcm.samplesPerBlock * (int)s->format.basic.numChannels );
		}
	}

	N3DS_SoundLock();
	if ( need > cacheSize ) {
		for ( int i = 0; i < ADPCM_CACHE; i++ ) {
			Mem_Free( cacheData[i] );
			cacheData[i] = (short *)Mem_Alloc( need * sizeof( short ), TAG_AUDIO );
		}
		cacheSize = need;
	}
	for ( int i = 0; i < ADPCM_CACHE; i++ ) {
		cacheSample[i] = NULL;
		cacheBlock[i] = -1;
	}
	current = sample;
	position = (int64)offsetSamples << 32;
	amplitude = 0.0f;
	playing = true;
	paused = false;
	N3DS_SoundUnlock();

	Update();
}

/*
========================
idSoundVoice_3DS::Update
========================
*/
bool idSoundVoice_3DS::Update() {
	if ( leadinSample == NULL ) {
		return false;
	}
	if ( pendingStart ) {
		const int leadin = leadinSample->PrepareData();
		int looping = 1;
		if ( loopingSample != NULL && loopingSample != leadinSample ) {
			looping = loopingSample->PrepareData();
		}
		if ( leadin < 0 ) {
			pendingStart = false;		// cannot be read: stays silent
			return false;
		}
		if ( leadin == 0 || looping == 0 ) {
			return true;				// still reading
		}
		if ( looping < 0 ) {
			SetSamples( leadinSample, NULL );
		}
		// long sounds (speech with lip sync, music) keep their timing, short ones start whole
		int offset = pendingOffsetMS;
		if ( leadinSample->LengthInMsec() > 2000 ) {
			offset += Sys_Milliseconds() - pendingSince;
		}
		StartNow( offset, pendingFlags );		// does an Update() itself
		return true;
	}
	const int srcChannels = Min( (int)leadinSample->NumChannels(), 2 );
	float pLevelMatrix[ MAX_CHANNELS_PER_VOICE * MAX_CHANNELS_PER_VOICE ] = { 0 };
	CalculateSurround( srcChannels, pLevelMatrix, 1.0f );

	int newLevels[2][2];
	for ( int src = 0; src < 2; src++ ) {
		for ( int dst = 0; dst < 2; dst++ ) {
			float l = 0.0f;
			if ( src < srcChannels ) {
				l = pLevelMatrix[ srcChannels * dst + src ] * gain;
			}
			newLevels[src][dst] = (int)( idMath::ClampFloat( 0.0f, 4.0f, l ) * 65536.0f );
		}
	}
	// plain stores the mixer may see half-updated for one block: harmless, no lock
	memcpy( levels, newLevels, sizeof( levels ) );
	pitchScale = idMath::ClampFloat( 0.1f, 8.0f, pitch );
	return true;
}

/*
========================
idSoundVoice_3DS::AdpcmBlock
========================
*/
const short * idSoundVoice_3DS::AdpcmBlock( idSoundSample_3DS * s, int block ) {
	for ( int i = 0; i < ADPCM_CACHE; i++ ) {
		if ( cacheSample[i] == s && cacheBlock[i] == block ) {
			return cacheData[i];
		}
	}
	if ( cacheSize <= 0 ) {
		return NULL;
	}
	// find the block in the buffers (each holds whole blocks)
	const int spb = s->format.extra.adpcm.samplesPerBlock;
	const int blockBytes = s->format.basic.blockSize;
	if ( spb <= 0 || blockBytes <= 0 || spb * s->format.basic.numChannels > cacheSize ) {
		return NULL;
	}
	const int frame = block * spb;
	int previous = 0;
	for ( int b = 0; b < s->buffers.Num(); b++ ) {
		const idSoundSample_3DS::sampleBuffer_t & buf = s->buffers[b];
		if ( frame < buf.numSamples ) {
			if ( buf.buffer == NULL ) {
				return NULL;
			}
			const int local = ( frame - previous ) / spb;
			const int offset = local * blockBytes;
			if ( offset >= buf.bufferSize ) {
				return NULL;
			}
			const int slot = cacheNext;
			cacheNext = ( cacheNext + 1 ) % ADPCM_CACHE;
			DecodeAdpcmBlock( s->format, (const byte *)buf.buffer + offset, Min( blockBytes, buf.bufferSize - offset ), cacheData[slot] );
			cacheSample[slot] = s;
			cacheBlock[slot] = block;
			return cacheData[slot];
		}
		previous = buf.numSamples;
	}
	return NULL;
}

/*
========================
idSoundVoice_3DS::FetchFrame
========================
*/
bool idSoundVoice_3DS::FetchFrame( idSoundSample_3DS * s, int frame, int & left, int & right ) {
	if ( frame < 0 || frame >= s->playLength ) {
		return false;
	}
	frame += s->playBegin;
	const int channels = s->format.basic.numChannels;
	if ( s->format.basic.formatTag == idWaveFile::FORMAT_ADPCM ) {
		const int spb = s->format.extra.adpcm.samplesPerBlock;
		if ( spb <= 0 ) {
			return false;
		}
		const short * data = AdpcmBlock( s, frame / spb );
		if ( data == NULL ) {
			return false;
		}
		const short * f = data + ( frame % spb ) * channels;
		left = f[0];
		right = ( channels > 1 ) ? f[1] : f[0];
		return true;
	}
	// 16-bit PCM
	int previous = 0;
	for ( int b = 0; b < s->buffers.Num(); b++ ) {
		const idSoundSample_3DS::sampleBuffer_t & buf = s->buffers[b];
		if ( frame < buf.numSamples ) {
			if ( buf.buffer == NULL ) {
				return false;
			}
			const int index = ( frame - previous ) * channels;
			if ( ( index + channels ) * (int)sizeof( short ) > buf.bufferSize ) {
				return false;
			}
			const short * f = (const short *)buf.buffer + index;
			left = f[0];
			right = ( channels > 1 ) ? f[1] : f[0];
			return true;
		}
		previous = buf.numSamples;
	}
	return false;
}

/*
========================
idSoundVoice_3DS::Mix

Mixer thread, mixer lock held.
========================
*/
void idSoundVoice_3DS::Mix( int * out, int frames, int outputRate ) {
	if ( !playing || paused || current == NULL ) {
		return;
	}
	int peak = 0;
	int64 step = (int64)( (double)pitchScale * current->SampleRate() / outputRate * 4294967296.0 );
	for ( int i = 0; i < frames; i++ ) {
		const int frame = (int)( position >> 32 );
		int l0, r0;
		if ( !FetchFrame( current, frame, l0, r0 ) ) {
			// end of this sample: on to the looping one, or done
			if ( loopingSample != NULL && loopingSample->playLength > 0 ) {
				const int over = frame - current->playLength;
				current = loopingSample;
				position = (int64)( over > 0 ? over % loopingSample->playLength : 0 ) << 32;
				step = (int64)( (double)pitchScale * current->SampleRate() / outputRate * 4294967296.0 );
				i--;
				continue;
			}
			playing = false;
			break;
		}
		int l1 = l0, r1 = r0;
		FetchFrame( current, frame + 1, l1, r1 );		// past the end: hold the last frame
		const int frac = (int)( ( position >> 16 ) & 0xFFFF );
		const int l = l0 + ( ( ( l1 - l0 ) * frac ) >> 16 );
		const int r = r0 + ( ( ( r1 - r0 ) * frac ) >> 16 );
		const int outL = (int)( ( (int64)l * levels[0][0] + (int64)r * levels[1][0] ) >> 16 );
		const int outR = (int)( ( (int64)l * levels[0][1] + (int64)r * levels[1][1] ) >> 16 );
		out[i * 2 + 0] += outL;
		out[i * 2 + 1] += outR;
		const int a = abs( l ) > abs( r ) ? abs( l ) : abs( r );
		if ( a > peak ) {
			peak = a;
		}
		position += step;
	}
	amplitude = peak * ( 1.0f / 32768.0f );
}

/*
========================
idSoundVoice_3DS::IsPlaying
========================
*/
bool idSoundVoice_3DS::IsPlaying() {
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
	paused = true;
}

/*
========================
idSoundVoice_3DS::UnPause
========================
*/
void idSoundVoice_3DS::UnPause() {
	if ( playing ) {
		paused = false;
	}
}

/*
========================
idSoundVoice_3DS::Stop
========================
*/
void idSoundVoice_3DS::Stop() {
	pendingStart = false;
	N3DS_SoundLock();
	playing = false;
	paused = true;
	N3DS_SoundUnlock();
}

/*
========================
idSoundVoice_3DS::GetAmplitude
========================
*/
float idSoundVoice_3DS::GetAmplitude() {
	if ( !hasVUMeter ) {
		return 1.0f;
	}
	return amplitude;
}
