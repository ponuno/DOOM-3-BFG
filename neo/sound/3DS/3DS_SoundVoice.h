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
#ifndef __3DS_SOUNDVOICE_H__
#define __3DS_SOUNDVOICE_H__

/*
================================================
idSoundVoice_3DS

A voice of the New 3DS sound output (3DS_SoundHardware.cpp). While it plays it owns
one of the DSP's 24 channels (n3ds_audio.c): the DSP resamples it (channel rate =
sample rate * pitch), applies the left/right levels and mixes it with the others.
The voice only keeps the channel's wave buffers filled with PCM16: 16-bit PCM is
copied, MS ADPCM decoded block by block; the leadin sample plays once and then the
looping sample (if any) forever, like the XAudio2 voice. Stereo samples get their
2x2 level matrix (CalculateSurround) applied while filling, mono ones use the DSP's
levels directly.

Samples from the sound pack (N3DS_FORMAT_DSPADPCM) are not decoded at all: the voice
copies their DSP-ADPCM frames into the channel's wave buffers and the DSP decodes them.
DSP-ADPCM channels are mono, so a stereo sample takes two DSP channels, and its 2x2
level matrix becomes the two channels' left/right levels.

The main thread sets it up (Create/Start/Update/Stop); the feeder thread refills it
(Feed). Both under the mixer lock.
================================================
*/

class idSoundVoice_3DS : public idSoundVoice_Base {
public:
							idSoundVoice_3DS();
							~idSoundVoice_3DS();

	void					Create( const idSoundSample * leadinSample, const idSoundSample * loopingSample );

	// Start playing at a particular point in the buffer.  Does an Update() too
	void					Start( int offsetMS, int ssFlags );

	// Stop playing.
	void					Stop();

	// Stop consuming buffers
	void					Pause();
	// Start consuming buffers again
	void					UnPause();

	// Sends new position/volume/pitch information to the hardware
	bool					Update();

	// returns the RMS levels of the most recently processed block of audio, SSF_FLICKER must have been passed to Start
	float					GetAmplitude();

	// returns true if we can re-use this voice
	bool					CompatibleFormat( idSoundSample_3DS * s );

	uint32					GetSampleRate() const { return sampleRate; }

	// feeder thread, mixer lock held: refill the channel's finished wave buffers
	void					Feed();
	// no DSP: consume what 'ms' of playback would have played
	void					RunSilent( int ms );

private:
	friend class idSoundHardware_3DS;

	// Returns true when all the buffers are finished processing
	bool					IsPlaying();

	// Called after the voice has been stopped
	void					FlushSourceBuffers();

	// Destroy the internal hardware resource
	void					DestroyInternal();

	void					SetSamples( idSoundSample_3DS * leadin, idSoundSample_3DS * looping );
	void					StopLocked();
	// up to 'frames' frames of the sample data from the play position on (PCM16, the
	// sample's channels); NULL if the data is not there
	const short *			SourceRun( int & frames );
	// sound pack samples: queue DSP-ADPCM frames, advance without output
	void					FeedDsp();
	void					AdvanceSilent( int frames );
	// fills dst (NULL: just skips) with up to 'frames' output frames, returns how many
	int						Produce( short * dst, int frames, int & peak );
	void					ApplyLevels();

	idSoundSample_3DS *		leadinSample;
	idSoundSample_3DS *		loopingSample;

	// These are the fields from the sample format that matter to us for voice reuse
	uint16					formatTag;
	uint16					numChannels;
	uint32					sampleRate;

	bool					hasVUMeter;
	volatile bool			paused;
	volatile bool			playing;

	// sample data still being read from the SD card when Start() was called: the start
	// happens in Update() once it is in memory
	bool					pendingStart;
	int						pendingOffsetMS;
	int						pendingFlags;
	int						pendingSince;
	void					StartNow( int offsetMS, int ssFlags );
	void					StartDsp( int offsetMS );

	// playback state (under the mixer lock)
	int						channel;			// DSP channel, -1 if none
	int						channel2;			// DSP-ADPCM stereo: the right source channel's DSP channel
	bool					dsp;				// playing sound pack (DSP-ADPCM) data
	bool					dspNewStream;		// the next buffer starts a stream (decoder state reset)
	const idSoundSample_3DS * bufSample[N3DS_AUDIO_BUFFERS];	// DSP-ADPCM: what each wave buffer holds
	int						bufFrame[N3DS_AUDIO_BUFFERS];
	bool					stereo;				// output (and source) is stereo
	bool					sourceDone;			// everything is queued
	idSoundSample_3DS *		current;			// leadin or looping
	int						frame;				// play position in 'current', frames from playBegin
	int						levels[2][2];		// [source channel][left, right], 1.0 = 65536
	float					pitchScale;			// pitch multiplier
	int						bufferPeak[N3DS_AUDIO_BUFFERS];	// peak of each wave buffer's data

	// MS ADPCM: the decoded block
	const idSoundSample_3DS * adpcmSample;
	int						adpcmBlock;
	short *					adpcmData;			// samplesPerBlock * channels
	int						adpcmSize;			// shorts allocated
};

/*
================================================
idSoundVoice
================================================
*/
class idSoundVoice : public idSoundVoice_3DS {
};

#endif
