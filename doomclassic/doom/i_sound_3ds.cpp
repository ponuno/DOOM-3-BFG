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

// Classic DOOM sound for New 3DS: null implementation for now.
// The XAudio2 version is i_sound_win32.cpp.

#include "Precompiled.h"
#include "globaldata.h"

#include "i_sound.h"
#include "w_wad.h"
#include "sounds.h"

void I_InitSound() {}
void I_InitSoundHardware( int numOutputChannels_, int channelMask ) {}
void I_UpdateSound( void ) {}
void I_SubmitSound( void ) {}
void I_ShutdownSound( void ) {}
void I_ShutdownSoundHardware() {}
void I_SetChannels() {}
void I_ProcessSoundEvents( void ) {}

int I_GetSfxLumpNum( sfxinfo_t * sfx ) {
	char namebuf[9];
	sprintf( namebuf, "ds%s", sfx->name );
	return W_GetNumForName( namebuf );
}

int I_StartSound( int id, mobj_t *origin, mobj_t *listener_origin, int vol, int pitch, int priority ) {
	return id;
}

void I_StopSound( int handle, int player ) {}
int I_SoundIsPlaying( int handle ) { return 0; }
void I_UpdateSoundParams( int handle, int vol, int sep, int pitch ) {}
void I_SetSfxVolume( int volume ) {}

void I_InitMusic( void ) {}
void I_ShutdownMusic( void ) {}
void I_SetMusicVolume( int volume ) {}
void I_PauseSong( int handle ) {}
void I_ResumeSong( int handle ) {}
int I_RegisterSong( void *data, int length ) { return 0; }
void I_PlaySong( const char *songname, int looping ) {}
void I_StopSong( int handle ) {}
void I_UnRegisterSong( int handle ) {}
void I_UpdateMusic( void ) {}
