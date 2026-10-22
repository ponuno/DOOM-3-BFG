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
		channel( -1 ),
		stereo( false ),
		sourceDone( true ),
		current( NULL ),
		frame( 0 ),
		pitchScale( 1.0f ),
		adpcmSample( NULL ),
		adpcmBlock( -1 ),
		adpcmData( NULL ),
		adpcmSize( 0 ) {
	memset( levels, 0, sizeof( levels ) );
	memset( bufferPeak, 0, sizeof( bufferPeak ) );
}

/*
========================
idSoundVoice_3DS::~idSoundVoice_3DS
========================
*/
idSoundVoice_3DS::~idSoundVoice_3DS() {
	DestroyInternal();
	Mem_Free( adpcmData );
	adpcmData = NULL;
}

/*
========================
idSoundVoice_3DS::CompatibleFormat
========================
*/
bool idSoundVoice_3DS::CompatibleFormat( idSoundSample_3DS * s ) {
	// a voice plays any format (the DSP channel is configured at every start)
	return true;
}

/*
========================
idSoundVoice_3DS::StopLocked

Mixer lock held: gives the DSP channel back.
========================
*/
void idSoundVoice_3DS::StopLocked() {
	if ( channel >= 0 ) {
		N3DSAudio_ChnStop( channel );
		soundSystemLocal.hardware.FreeChannel( channel );
		channel = -1;
	}
	playing = false;
	paused = true;
	sourceDone = true;
	current = NULL;
}

/*
========================
idSoundVoice_3DS::SetSamples

Keeps the samples' voice counts (a sample a voice holds keeps its data).
========================
*/
void idSoundVoice_3DS::SetSamples( idSoundSample_3DS * leadin, idSoundSample_3DS * looping ) {
	N3DS_SoundLock();
	StopLocked();
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
	adpcmSample = NULL;
	adpcmBlock = -1;
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

	// room for the biggest decoded ADPCM block of the two samples
	int need = 0;
	for ( int k = 0; k < 2; k++ ) {
		const idSoundSample_3DS * s = ( k == 0 ) ? leadinSample : loopingSample;
		if ( s != NULL && s->format.basic.formatTag == idWaveFile::FORMAT_ADPCM ) {
			need = Max( need, (int)s->format.extra.adpcm.samplesPerBlock * (int)s->format.basic.numChannels );
		}
	}

	N3DS_SoundLock();
	StopLocked();
	if ( need > adpcmSize ) {
		Mem_Free( adpcmData );
		adpcmData = (short *)Mem_Alloc( need * sizeof( short ), TAG_AUDIO );
		adpcmSize = need;
	}
	adpcmSample = NULL;
	adpcmBlock = -1;
	current = sample;
	frame = offsetSamples;
	stereo = leadinSample->NumChannels() > 1;
	sourceDone = false;
	memset( bufferPeak, 0, sizeof( bufferPeak ) );
	playing = true;
	paused = false;
	if ( soundSystemLocal.hardware.AudioRunning() ) {
		channel = soundSystemLocal.hardware.AllocChannel();
		if ( channel < 0 ) {
			// more voices than DSP channels (s_maxEmitterChannels keeps them below)
			soundSystemLocal.hardware.CountDropped();
			playing = false;
			sourceDone = true;
			current = NULL;
			N3DS_SoundUnlock();
			return;
		}
		N3DSAudio_ChnStart( channel, stereo ? 1 : 0, (float)sampleRate );
	}
	N3DS_SoundUnlock();

	Update();		// levels and rate before the first buffer plays

	N3DS_SoundLock();
	Feed();			// the first buffers right away instead of at the next DSP frame
	N3DS_SoundUnlock();
}

/*
========================
idSoundVoice_3DS::ApplyLevels

Mixer lock held.
========================
*/
void idSoundVoice_3DS::ApplyLevels() {
	if ( channel < 0 ) {
		return;
	}
	if ( stereo ) {
		// the matrix is applied while filling (the DSP has no cross terms for stereo)
		N3DSAudio_ChnSetMix( channel, 1.0f, 1.0f );
	} else {
		N3DSAudio_ChnSetMix( channel, levels[0][0] * ( 1.0f / 65536.0f ), levels[0][1] * ( 1.0f / 65536.0f ) );
	}
	if ( current != NULL ) {
		N3DSAudio_ChnSetRate( channel, (float)current->SampleRate() * pitchScale );
	}
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
	N3DS_SoundLock();
	memcpy( levels, newLevels, sizeof( levels ) );
	pitchScale = idMath::ClampFloat( 0.1f, 8.0f, pitch );
	ApplyLevels();
	N3DS_SoundUnlock();
	return true;
}

/*
========================
idSoundVoice_3DS::SourceRun

Mixer lock held. Points at the data from 'frame' on and cuts 'frames' to what is
contiguous there: the rest of a decoded ADPCM block, or of a PCM buffer.
========================
*/
const short * idSoundVoice_3DS::SourceRun( int & frames ) {
	idSoundSample_3DS * s = current;
	const int channels = s->format.basic.numChannels;
	const int absFrame = frame + s->playBegin;
	if ( s->format.basic.formatTag == idWaveFile::FORMAT_ADPCM ) {
		const int spb = s->format.extra.adpcm.samplesPerBlock;
		const int blockBytes = s->format.basic.blockSize;
		if ( spb <= 0 || blockBytes <= 0 || spb * channels > adpcmSize ) {
			return NULL;
		}
		const int block = absFrame / spb;
		const int inBlock = absFrame - block * spb;
		if ( adpcmSample != s || adpcmBlock != block ) {
			// find the block in the buffers (each holds whole blocks)
			const int first = block * spb;
			int previous = 0;
			bool found = false;
			for ( int b = 0; b < s->buffers.Num() && !found; b++ ) {
				const idSoundSample_3DS::sampleBuffer_t & buf = s->buffers[b];
				if ( first < buf.numSamples ) {
					if ( buf.buffer == NULL ) {
						return NULL;
					}
					const int offset = ( ( first - previous ) / spb ) * blockBytes;
					if ( offset >= buf.bufferSize ) {
						return NULL;
					}
					DecodeAdpcmBlock( s->format, (const byte *)buf.buffer + offset, Min( blockBytes, buf.bufferSize - offset ), adpcmData );
					found = true;
				}
				previous = buf.numSamples;
			}
			if ( !found ) {
				return NULL;
			}
			adpcmSample = s;
			adpcmBlock = block;
		}
		frames = Min( frames, spb - inBlock );
		return adpcmData + inBlock * channels;
	}
	// 16-bit PCM
	int previous = 0;
	for ( int b = 0; b < s->buffers.Num(); b++ ) {
		const idSoundSample_3DS::sampleBuffer_t & buf = s->buffers[b];
		if ( absFrame < buf.numSamples ) {
			if ( buf.buffer == NULL ) {
				return NULL;
			}
			const int index = absFrame - previous;
			const int inBuffer = buf.bufferSize / ( channels * (int)sizeof( short ) ) - index;
			if ( inBuffer <= 0 ) {
				return NULL;
			}
			frames = Min( frames, Min( buf.numSamples - absFrame, inBuffer ) );
			return (const short *)buf.buffer + index * channels;
		}
		previous = buf.numSamples;
	}
	return NULL;
}

/*
========================
idSoundVoice_3DS::Produce

Mixer lock held. Copies (stereo: through the level matrix) up to 'frames' frames from
the play position on into dst, moving on to the looping sample at the end of the leadin.
========================
*/
int idSoundVoice_3DS::Produce( short * dst, int frames, int & peak ) {
	int done = 0;
	while ( done < frames && !sourceDone ) {
		if ( frame >= current->playLength ) {
			if ( loopingSample != NULL && loopingSample->playLength > 0 ) {
				frame = ( current == loopingSample ) ? frame - current->playLength : 0;
				current = loopingSample;
				if ( frame >= current->playLength ) {
					frame = 0;
				}
				continue;
			}
			sourceDone = true;
			break;
		}
		int run = Min( frames - done, current->playLength - frame );
		const short * src = SourceRun( run );
		if ( src == NULL || run <= 0 ) {
			// the data is gone (should not happen while the voice holds the sample)
			sourceDone = true;
			break;
		}
		if ( dst != NULL ) {
			if ( !stereo ) {
				short * d = dst + done;
				for ( int i = 0; i < run; i++ ) {
					const int v = src[i];
					d[i] = (short)v;
					const int a = v < 0 ? -v : v;
					if ( a > peak ) {
						peak = a;
					}
				}
			} else {
				short * d = dst + done * 2;
				const int ll = levels[0][0], lr = levels[0][1], rl = levels[1][0], rr = levels[1][1];
				const bool mono = current->format.basic.numChannels < 2;	// cannot happen (same format as the leadin)
				for ( int i = 0; i < run; i++ ) {
					const int l = src[mono ? i : i * 2];
					const int r = src[mono ? i : i * 2 + 1];
					int outL = (int)( ( (int64)l * ll + (int64)r * rl ) >> 16 );
					int outR = (int)( ( (int64)l * lr + (int64)r * rr ) >> 16 );
					outL = outL > 32767 ? 32767 : ( outL < -32768 ? -32768 : outL );
					outR = outR > 32767 ? 32767 : ( outR < -32768 ? -32768 : outR );
					d[i * 2 + 0] = (short)outL;
					d[i * 2 + 1] = (short)outR;
					const int a = abs( l ) > abs( r ) ? abs( l ) : abs( r );
					if ( a > peak ) {
						peak = a;
					}
				}
			}
		}
		done += run;
		frame += run;
	}
	return done;
}

/*
========================
idSoundVoice_3DS::Feed

Feeder thread (and StartNow), mixer lock held.
========================
*/
void idSoundVoice_3DS::Feed() {
	if ( !playing || channel < 0 || current == NULL ) {
		return;
	}
	if ( !sourceDone ) {
		bool busy = N3DSAudio_ChnBusy( channel ) != 0;
		bool queuedAny = false;
		for ( int b = 0; b < N3DS_AUDIO_BUFFERS && !sourceDone; b++ ) {
			if ( !N3DSAudio_BufFree( channel, b ) ) {
				continue;
			}
			int peak = 0;
			const int n = Produce( N3DSAudio_BufData( channel, b ), N3DS_AUDIO_FRAMES, peak );
			if ( n > 0 ) {
				bufferPeak[b] = peak;
				N3DSAudio_BufQueue( channel, b, n, stereo ? 1 : 0 );
				queuedAny = true;
			}
		}
		// all played out while there was more to play: the feeder came too late
		if ( !busy && queuedAny && soundSystemLocal.hardware.ChannelStarted( channel ) ) {
			soundSystemLocal.hardware.CountUnderrun();
		}
		soundSystemLocal.hardware.SetChannelStarted( channel );
		return;
	}
	if ( !N3DSAudio_ChnBusy( channel ) ) {
		// everything played: the channel is free again
		StopLocked();
	}
}

/*
========================
idSoundVoice_3DS::RunSilent

No DSP: the voice advances by the clock so that sounds still end.
========================
*/
void idSoundVoice_3DS::RunSilent( int ms ) {
	if ( !playing || paused || current == NULL ) {
		return;
	}
	const int frames = (int)( (int64)ms * (int)( current->SampleRate() * pitchScale ) / 1000 );
	int peak = 0;
	if ( frames > 0 ) {
		Produce( NULL, frames, peak );
	}
	if ( sourceDone ) {
		StopLocked();
	}
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
	N3DS_SoundLock();
	paused = true;
	if ( channel >= 0 ) {
		N3DSAudio_ChnSetPaused( channel, 1 );
	}
	N3DS_SoundUnlock();
}

/*
========================
idSoundVoice_3DS::UnPause
========================
*/
void idSoundVoice_3DS::UnPause() {
	N3DS_SoundLock();
	if ( playing ) {
		paused = false;
		if ( channel >= 0 ) {
			N3DSAudio_ChnSetPaused( channel, 0 );
		}
	}
	N3DS_SoundUnlock();
}

/*
========================
idSoundVoice_3DS::Stop
========================
*/
void idSoundVoice_3DS::Stop() {
	pendingStart = false;
	N3DS_SoundLock();
	StopLocked();
	N3DS_SoundUnlock();
}

/*
========================
idSoundVoice_3DS::GetAmplitude

Peak of the wave buffer the DSP is playing now.
========================
*/
float idSoundVoice_3DS::GetAmplitude() {
	if ( !hasVUMeter ) {
		return 1.0f;
	}
	int peak = 0;
	N3DS_SoundLock();
	if ( channel >= 0 ) {
		for ( int b = 0; b < N3DS_AUDIO_BUFFERS; b++ ) {
			if ( N3DSAudio_BufPlaying( channel, b ) ) {
				peak = bufferPeak[b];
				break;
			}
		}
	}
	N3DS_SoundUnlock();
	return peak * ( 1.0f / 32768.0f );
}
