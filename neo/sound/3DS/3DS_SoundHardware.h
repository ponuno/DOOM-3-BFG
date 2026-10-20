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

New 3DS sound output. Currently a null device: voices are tracked so the
sound world behaves normally, but nothing is mixed. The ndsp backend will
replace the bodies in 3DS_SoundHardware.cpp / 3DS_SoundVoice.cpp.
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

protected:
	friend class idSoundSample_3DS;
	friend class idSoundVoice_3DS;

private:
	int				outputChannels;
	int				channelMask;

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
