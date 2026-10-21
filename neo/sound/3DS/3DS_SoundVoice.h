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

A voice of the New 3DS software mixer (3DS_SoundHardware.cpp). The main thread
sets it up (Create/Start/Update/Stop, under the mixer lock); the mixer thread
reads the sample data, resamples it (linear) to the output rate, applies the
stereo levels from CalculateSurround and the gain, and plays the leadin sample
once and then the looping sample (if any) forever, like the XAudio2 voice.
Sample formats: 16-bit PCM and MS ADPCM, mono or stereo.
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

	// mixer thread: adds this voice into out[frames * 2] (stereo)
	void					Mix( int * out, int frames, int outputRate );

private:
	friend class idSoundHardware_3DS;

	// Returns true when all the buffers are finished processing
	bool					IsPlaying();

	// Called after the voice has been stopped
	void					FlushSourceBuffers();

	// Destroy the internal hardware resource
	void					DestroyInternal();

	void					SetSamples( idSoundSample_3DS * leadin, idSoundSample_3DS * looping );
	// one frame of a sample (both channels of a stereo one), false past the end
	bool					FetchFrame( idSoundSample_3DS * s, int frame, int & left, int & right );
	const short *			AdpcmBlock( idSoundSample_3DS * s, int block );

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

	// mixer state (under the mixer lock)
	idSoundSample_3DS *		current;			// leadin or looping
	int64					position;			// in frames of 'current', 32.32 fixed point
	int						levels[2][2];		// [source channel][left, right], 1.0 = 65536
	float					pitchScale;			// pitch multiplier (the sample rate is applied when mixing)
	float					amplitude;			// peak of the last mixed block, 0..1

	// MS ADPCM: two decoded blocks
	static const int		ADPCM_CACHE = 2;
	const idSoundSample_3DS * cacheSample[ADPCM_CACHE];
	int						cacheBlock[ADPCM_CACHE];
	int						cacheNext;
	short *					cacheData[ADPCM_CACHE];		// samplesPerBlock * channels
	int						cacheSize;					// shorts per cache slot
};

/*
================================================
idSoundVoice
================================================
*/
class idSoundVoice : public idSoundVoice_3DS {
};

#endif
