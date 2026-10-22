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
#ifndef __3DS_SOUNDHARDWARE_H__
#define __3DS_SOUNDHARDWARE_H__

/*
================================================
idSoundHardware_3DS

New 3DS sound output: every playing voice gets one of the DSP's 24 channels, the
DSP resamples and mixes them (n3ds_audio.c); a feeder thread keeps the channels'
wave buffers filled. Without the DSP firmware the voices still run on the clock,
so the sound world behaves the same, silently.
================================================
*/

static const int SND_3DS_MIN_SAMPLE_RATE = 1000;

class idSoundSample_3DS;
class idSoundVoice_3DS;

class idSoundHardware_3DS {
public:
					idSoundHardware_3DS();

	void			Init();
	void			Shutdown();

	void 			Update();

	idSoundVoice *	AllocateVoice( const idSoundSample * leadinSample, const idSoundSample * loopingSample );
	void			FreeVoice( idSoundVoice * voice );

	// video playback asks for the XAudio2 device; there is none here
	void *			GetIXAudio2() const { return NULL; }

	int				GetNumZombieVoices() const { return zombieVoices.Num(); }
	int				GetNumFreeVoices() const { return freeVoices.Num(); }

	// feeder thread (n3ds_audio.c callback)
	void			FeedVoices();

	// DSP channels, mixer lock held
	bool			AudioRunning() const { return audioOk; }
	int				AllocChannel();
	void			FreeChannel( int ch );
	bool			ChannelStarted( int ch ) const { return channelStarted[ch]; }
	void			SetChannelStarted( int ch ) { channelStarted[ch] = true; }
	void			CountUnderrun() { underruns++; }
	void			CountDropped() { droppedVoices++; }

protected:
	friend class idSoundSample_3DS;
	friend class idSoundVoice_3DS;

private:
	int				outputChannels;
	int				channelMask;
	bool			audioOk;			// NDSP running
	int				lastUpdateMs;		// without NDSP: voices advanced by the clock
	int				lastReportMs;
	float			lastMasterVolume;

	bool			channelUsed[N3DS_AUDIO_CHANNELS];
	bool			channelStarted[N3DS_AUDIO_CHANNELS];	// had buffers queued before
	int				underruns;
	int				droppedVoices;

	idStaticList<idSoundVoice_3DS, MAX_HARDWARE_VOICES * 2 > voices;
	idStaticList<idSoundVoice_3DS *, MAX_HARDWARE_VOICES * 2 > zombieVoices;
	idStaticList<idSoundVoice_3DS *, MAX_HARDWARE_VOICES * 2 > freeVoices;
};

/*
================================================
idSoundHardware
================================================
*/

class idSoundHardware : public idSoundHardware_3DS {
};

#endif
