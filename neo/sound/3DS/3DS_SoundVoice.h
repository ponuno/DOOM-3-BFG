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

Null voice: keeps the same contract as the XAudio2 voice (start/stop/update,
reuse by format) so the sound world can allocate and release voices.
A voice "plays" for the length of its samples on the sound clock, so code that
waits for a sound to finish still progresses.
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

private:
	friend class idSoundHardware_3DS;

	// Returns true when all the buffers are finished processing
	bool					IsPlaying();

	// Called after the voice has been stopped
	void					FlushSourceBuffers();

	// Destroy the internal hardware resource
	void					DestroyInternal();

	idSoundSample_3DS *		leadinSample;
	idSoundSample_3DS *		loopingSample;

	// These are the fields from the sample format that matter to us for voice reuse
	uint16					formatTag;
	uint16					numChannels;
	uint32					sampleRate;

	bool					hasVUMeter;
	bool					paused;
	bool					playing;
	int						startTime;			// Sys_Milliseconds() when started
	int						endTime;			// 0 = looping (never ends on its own)
};

/*
================================================
idSoundVoice
================================================
*/
class idSoundVoice : public idSoundVoice_3DS {
};

#endif
