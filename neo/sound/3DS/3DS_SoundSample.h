/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#ifndef __3DS_SOUNDSAMPLE_H__
#define __3DS_SOUNDSAMPLE_H__

// mixer lock (3DS_SoundHardware.cpp) and sample data statistics
void	N3DS_SoundLock();
void	N3DS_SoundUnlock();
int		N3DS_SoundDataKB();
int		N3DS_SoundLoadsOnDemand();
int		N3DS_SoundEvictions();

/*
================================================
idSoundSample_3DS
================================================
*/
class idSampleInfo;
class idSoundSample_3DS {
public:
					idSoundSample_3DS();

	// Loads and initializes the resource based on the name.
	virtual void	 LoadResource();

	void			SetName( const char * n ) { name = n; }
	const char *	GetName() const { return name; }
	ID_TIME_T		GetTimestamp() const { return timestamp; }

	// turns it into a beep
	void			MakeDefault();

	// frees all data
	void			FreeData();

	int				LengthInMsec() const { return SamplesToMsec( NumSamples(), SampleRate() ); }
	int				SampleRate() const { return format.basic.samplesPerSec; }
	int				NumSamples() const { return playLength; }
	int				NumChannels() const { return format.basic.numChannels; }
	int				BufferSize() const { return totalBufferSize; }

	bool			IsCompressed() const { return ( format.basic.formatTag != idWaveFile::FORMAT_PCM ); }

	bool			IsDefault() const { return timestamp == FILE_NOT_FOUND_TIMESTAMP; }
	bool			IsLoaded() const { return loaded; }

	void			SetNeverPurge() { neverPurge = true; }
	bool			GetNeverPurge() const { return neverPurge; }

	void			SetLevelLoadReferenced() { levelLoadReferenced = true; }
	void			ResetLevelLoadReferenced() { levelLoadReferenced = false; }
	bool			GetLevelLoadReferenced() const { return levelLoadReferenced; }

	int				GetLastPlayedTime() const { return lastPlayedTime; }
	void			SetLastPlayedTime( int t ) { lastPlayedTime = t; }

	float			GetAmplitude( int timeMS ) const;

	static void		N3DS_LogMemory();	// memory diagnostics at MEM checkpoints

protected:
	friend class idSoundHardware_3DS;
	friend class idSoundVoice_3DS;

					~idSoundSample_3DS();

	bool			LoadWav( const idStr & name );
	bool			LoadAmplitude( const idStr & name );
	void			WriteAllSamples( const idStr &sampleName );
	bool			LoadGeneratedSample( const idStr &name );
	void			WriteGeneratedSample( idFile *fileOut );

	struct sampleBuffer_t {
		void * buffer;			// NULL while the data is not loaded (New 3DS, see EnsureData)
		int bufferSize;
		int numSamples;
		int fileOffset;			// where the data is in generatedName
	};

	// New 3DS: data on demand (3DS_SoundSample.cpp)
	bool			EnsureData();
	void			FreeBuffers();
	bool			ReadBuffers( idFile * f );
	void			MakeRoom( int bytes );
	void			N3DS_MarkResident();
	idStr			generatedName;		// empty: data read whole, never dropped
	bool			dataLoaded;
	int				activeVoices;		// voices created with this sample (data must stay)
	int				lastUse;

	idStr			name;

	ID_TIME_T		timestamp;
	bool			loaded;

	bool			neverPurge;
	bool			levelLoadReferenced;
	bool			usesMapHeap;

	uint32			lastPlayedTime;

	int				totalBufferSize;	// total size of all the buffers
	idList<sampleBuffer_t, TAG_AUDIO> buffers;

	int				playBegin;
	int				playLength;

	idWaveFile::waveFmt_t	format;

	idList<byte, TAG_AMPLITUDE> amplitude;
};

/*
================================================
idSoundSample

This reverse-inheritance purportedly makes working on
multiple platforms easier.
================================================
*/
class idSoundSample : public idSoundSample_3DS {
public:
};

#endif
