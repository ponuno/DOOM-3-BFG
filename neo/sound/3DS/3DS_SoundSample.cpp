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
#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../snd_local.h"

extern idCVar s_useCompression;
extern idCVar s_noSound;

#define GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( x ) x

const uint32 SOUND_MAGIC_IDMSA = 0x6D7A7274;

extern idCVar sys_lang;

/*
========================
AllocBuffer
========================
*/
static void * AllocBuffer( int size, const char * name ) {
	return Mem_Alloc( size, TAG_AUDIO );
}

/*
========================
FreeBuffer
========================
*/
static void FreeBuffer( void * p ) {
	return Mem_Free( p );
}

/*
================================================================================================

	New 3DS: sample data on demand

	LoadResource() reads only the header of a generated .idwav (format, length,
	amplitude) and remembers where each buffer's data sits in the file. Small
	samples are read right away; the rest are read the first time a voice plays
	them (EnsureData, main thread). All sample data shares n3ds_soundMemoryKB:
	when it is full, the least recently played samples that no voice uses give
	their data back.

================================================================================================
*/
idCVar n3ds_soundMemoryKB( "n3ds_soundMemoryKB", "8192", CVAR_SOUND | CVAR_INTEGER, "New 3DS: memory for sound sample data; least recently played samples are dropped when it is full" );
idCVar n3ds_soundPreloadKB( "n3ds_soundPreloadKB", "48", CVAR_SOUND | CVAR_INTEGER, "New 3DS: samples up to this size are read at level load, bigger ones when first played" );

static idList< idSoundSample_3DS *, TAG_AUDIO >	allSamples;
static int										sampleDataBytes = 0;
static int										sampleLoadsOnDemand = 0;
static int										sampleEvictions = 0;
static int										sampleUseClock = 0;

int N3DS_SoundDataKB() { return sampleDataBytes / 1024; }
int N3DS_SoundLoadsOnDemand() { const int n = sampleLoadsOnDemand; sampleLoadsOnDemand = 0; return n; }
int N3DS_SoundEvictions() { const int n = sampleEvictions; sampleEvictions = 0; return n; }

/*
========================
idSoundSample_3DS::idSoundSample_3DS
========================
*/
idSoundSample_3DS::idSoundSample_3DS() {
	dataLoaded = false;
	activeVoices = 0;
	lastUse = 0;
	allSamples.Append( this );
	timestamp = FILE_NOT_FOUND_TIMESTAMP;
	loaded = false;
	neverPurge = false;
	levelLoadReferenced = false;

	memset( &format, 0, sizeof( format ) );

	totalBufferSize = 0;

	playBegin = 0;
	playLength = 0;

	lastPlayedTime = 0;
}

/*
========================
idSoundSample_3DS::~idSoundSample_3DS
========================
*/
idSoundSample_3DS::~idSoundSample_3DS() {
	FreeData();
	allSamples.Remove( this );
}

/*
========================
idSoundSample_3DS::FreeBuffers

Only the sample data; the header stays.
========================
*/
void idSoundSample_3DS::FreeBuffers() {
	N3DS_SoundLock();
	for ( int i = 0; i < buffers.Num(); i++ ) {
		if ( buffers[i].buffer != NULL ) {
			FreeBuffer( buffers[i].buffer );
			sampleDataBytes -= buffers[i].bufferSize;
			buffers[i].buffer = NULL;
		}
	}
	dataLoaded = false;
	N3DS_SoundUnlock();
}

/*
========================
idSoundSample_3DS::ReadBuffers
========================
*/
bool idSoundSample_3DS::ReadBuffers( idFile * f ) {
	int total = 0;
	for ( int i = 0; i < buffers.Num(); i++ ) {
		total += buffers[i].bufferSize;
	}
	MakeRoom( total );
	for ( int i = 0; i < buffers.Num(); i++ ) {
		if ( f->Seek( buffers[i].fileOffset, FS_SEEK_SET ) != 0 ) {
			return false;
		}
		void * data = AllocBuffer( buffers[i].bufferSize, GetName() );
		if ( f->Read( data, buffers[i].bufferSize ) != buffers[i].bufferSize ) {
			FreeBuffer( data );
			return false;
		}
		N3DS_SoundLock();
		buffers[i].buffer = data;
		sampleDataBytes += buffers[i].bufferSize;
		N3DS_SoundUnlock();
	}
	dataLoaded = true;
	return true;
}

/*
========================
idSoundSample_3DS::MakeRoom

Drops the data of least recently played samples no voice is using until 'bytes'
more fit in n3ds_soundMemoryKB.
========================
*/
void idSoundSample_3DS::MakeRoom( int bytes ) {
	const int budget = n3ds_soundMemoryKB.GetInteger() * 1024;
	while ( sampleDataBytes + bytes > budget ) {
		idSoundSample_3DS * oldest = NULL;
		for ( int i = 0; i < allSamples.Num(); i++ ) {
			idSoundSample_3DS * s = allSamples[i];
			if ( s == this || !s->dataLoaded || s->activeVoices > 0 || s->generatedName.IsEmpty() ) {
				continue;
			}
			if ( oldest == NULL || s->lastUse < oldest->lastUse ) {
				oldest = s;
			}
		}
		if ( oldest == NULL ) {
			break;		// everything resident is playing: go over the budget
		}
		oldest->FreeBuffers();
		sampleEvictions++;
	}
}

/*
========================
idSoundSample_3DS::EnsureData

Called when a voice is about to play the sample. False if the data cannot be read.
========================
*/
bool idSoundSample_3DS::EnsureData() {
	lastUse = ++sampleUseClock;
	if ( dataLoaded ) {
		return true;
	}
	if ( generatedName.IsEmpty() ) {
		return false;
	}
	idFileLocal f( fileSystem->OpenFileRead( generatedName ) );
	if ( f == NULL ) {
		return false;
	}
	sampleLoadsOnDemand++;
	if ( !ReadBuffers( f ) ) {
		FreeBuffers();
		return false;
	}
	return true;
}

/*
========================
idSoundSample_3DS::WriteGeneratedSample
========================
*/
void idSoundSample_3DS::WriteGeneratedSample( idFile *fileOut ) {
	fileOut->WriteBig( SOUND_MAGIC_IDMSA );
	fileOut->WriteBig( timestamp );
	fileOut->WriteBig( loaded );
	fileOut->WriteBig( playBegin );
	fileOut->WriteBig( playLength );
	idWaveFile::WriteWaveFormatDirect( format, fileOut );
	fileOut->WriteBig( ( int )amplitude.Num() );
	fileOut->Write( amplitude.Ptr(), amplitude.Num() );
	fileOut->WriteBig( totalBufferSize );
	fileOut->WriteBig( ( int )buffers.Num() );
	for ( int i = 0; i < buffers.Num(); i++ ) {
		fileOut->WriteBig( buffers[ i ].numSamples );
		fileOut->WriteBig( buffers[ i ].bufferSize );
		fileOut->Write( buffers[ i ].buffer, buffers[ i ].bufferSize );
	};
}
/*
========================
idSoundSample_3DS::WriteAllSamples
========================
*/
void idSoundSample_3DS::WriteAllSamples( const idStr &sampleName ) {
	idSoundSample_3DS * samplePC = new idSoundSample_3DS();
	{
		idStrStatic< MAX_OSPATH > inName = sampleName;
		inName.Append( ".msadpcm" );
		idStrStatic< MAX_OSPATH > inName2 = sampleName;
		inName2.Append( ".wav" );

		idStrStatic< MAX_OSPATH > outName = "generated/";
		outName.Append( sampleName );
		outName.Append( ".idwav" );

		if ( samplePC->LoadWav( inName ) || samplePC->LoadWav( inName2 ) ) {
			idFile *fileOut = fileSystem->OpenFileWrite( outName, "fs_basepath" );
			samplePC->WriteGeneratedSample( fileOut );
			delete fileOut;
		} 
	}
	delete samplePC;
}

/*
========================
idSoundSample_3DS::LoadGeneratedSound
========================
*/
bool idSoundSample_3DS::LoadGeneratedSample( const idStr &filename ) {
	// header only; the data is read now if the sample is small, else when first played
	idFileLocal fileIn( fileSystem->OpenFileRead( filename ) );
	if ( fileIn == NULL ) {
		return false;
	}
	uint32 magic;
	fileIn->ReadBig( magic );
	fileIn->ReadBig( timestamp );
	fileIn->ReadBig( loaded );
	fileIn->ReadBig( playBegin );
	fileIn->ReadBig( playLength );
	idWaveFile::ReadWaveFormatDirect( format, fileIn );
	int num;
	fileIn->ReadBig( num );
	amplitude.Clear();
	amplitude.SetNum( num );
	fileIn->Read( amplitude.Ptr(), amplitude.Num() );
	fileIn->ReadBig( totalBufferSize );
	fileIn->ReadBig( num );
	buffers.SetNum( num );
	int total = 0;
	for ( int i = 0; i < num; i++ ) {
		fileIn->ReadBig( buffers[ i ].numSamples );
		fileIn->ReadBig( buffers[ i ].bufferSize );
		buffers[ i ].buffer = NULL;
		buffers[ i ].fileOffset = fileIn->Tell();
		total += buffers[ i ].bufferSize;
		if ( i + 1 < num ) {
			fileIn->Seek( buffers[ i ].bufferSize, FS_SEEK_CUR );
		}
	}
	generatedName = filename;
	dataLoaded = false;
	if ( total <= n3ds_soundPreloadKB.GetInteger() * 1024 ) {
		if ( !ReadBuffers( fileIn ) ) {
			FreeBuffers();
		}
	}
	return true;
}
/*
========================
idSoundSample_3DS::Load
========================
*/
void idSoundSample_3DS::LoadResource() {
	FreeData();

	if ( idStr::Icmpn( GetName(), "_default", 8 ) == 0 ) {
		MakeDefault();
		return;
	}

	if ( s_noSound.GetBool() ) {
		MakeDefault();
		return;
	}

	loaded = false;

	for ( int i = 0; i < 2; i++ ) {
		idStrStatic< MAX_OSPATH > sampleName = GetName();
		if ( ( i == 0 ) && !sampleName.Replace( "/vo/", va( "/vo/%s/", sys_lang.GetString() ) ) ) {
			i++;
		}
		idStrStatic< MAX_OSPATH > generatedName = "generated/";
		generatedName.Append( sampleName );

		{
			if ( s_useCompression.GetBool() ) {
				sampleName.Append( ".msadpcm" );
			} else {
				sampleName.Append( ".wav" );
			}
			generatedName.Append( ".idwav" );
		}
		loaded = LoadGeneratedSample( generatedName ) || LoadWav( sampleName );
		if ( loaded && generatedName != this->generatedName ) {
			N3DS_MarkResident();		// read whole from a .wav
		}

		if ( !loaded && s_useCompression.GetBool() ) {
			sampleName.SetFileExtension( "wav" );
			loaded = LoadWav( sampleName );
			if ( loaded ) {
				N3DS_MarkResident();
			}
		}

		if ( loaded ) {
			if ( cvarSystem->GetCVarBool( "fs_buildresources" ) ) {
				fileSystem->AddSamplePreload( GetName() );
				WriteAllSamples( GetName() );

				if ( sampleName.Find( "/vo/" ) >= 0 ) {
					for ( int i = 0; i < Sys_NumLangs(); i++ ) {
						const char * lang = Sys_Lang( i );
						if ( idStr::Icmp( lang, ID_LANG_ENGLISH ) == 0 ) {
							continue;
						}
						idStrStatic< MAX_OSPATH > locName = GetName();
						locName.Replace( "/vo/", va( "/vo/%s/", Sys_Lang( i ) ) );
						WriteAllSamples( locName );
					}
				}
			}
			return;
		}
	}

	if ( !loaded ) {
		// make it default if everything else fails
		MakeDefault();
	}
	return;
}

/*
========================
idSoundSample_3DS::LoadWav
========================
*/
bool idSoundSample_3DS::LoadWav( const idStr & filename ) {

	// load the wave
	idWaveFile wave;
	if ( !wave.Open( filename ) ) {
		return false;
	}

	idStrStatic< MAX_OSPATH > sampleName = filename;
	sampleName.SetFileExtension( "amp" );
	LoadAmplitude( sampleName );

	const char * formatError = wave.ReadWaveFormat( format );
	if ( formatError != NULL ) {
		idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), formatError );
		MakeDefault();
		return false;
	}
	timestamp = wave.Timestamp();

	totalBufferSize = wave.SeekToChunk( 'data' );

	if ( format.basic.formatTag == idWaveFile::FORMAT_PCM || format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE ) {

		if ( format.basic.bitsPerSample != 16 ) {
			idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), "Not a 16 bit PCM wav file" );
			MakeDefault();
			return false;
		}

		playBegin = 0;
		playLength = ( totalBufferSize ) / format.basic.blockSize;

		buffers.SetNum( 1 );
		buffers[0].bufferSize = totalBufferSize;
		buffers[0].numSamples = playLength;
		buffers[0].buffer = AllocBuffer( totalBufferSize, GetName() );
		

		wave.Read( buffers[0].buffer, totalBufferSize );

		if ( format.basic.bitsPerSample == 16 ) {
			idSwap::LittleArray( (short *)buffers[0].buffer, totalBufferSize / sizeof( short ) );
		}

		buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );

	} else if ( format.basic.formatTag == idWaveFile::FORMAT_ADPCM ) {

		playBegin = 0;
		playLength = ( ( totalBufferSize / format.basic.blockSize ) * format.extra.adpcm.samplesPerBlock );

		buffers.SetNum( 1 );
		buffers[0].bufferSize = totalBufferSize;
		buffers[0].numSamples = playLength;
		buffers[0].buffer  = AllocBuffer( totalBufferSize, GetName() );
		
		wave.Read( buffers[0].buffer, totalBufferSize );

		buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );

	} else if ( format.basic.formatTag == idWaveFile::FORMAT_XMA2 ) {

		if ( format.extra.xma2.blockCount == 0 ) {
			idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), "No data blocks in file" );
			MakeDefault();
			return false;
		}

		int bytesPerBlock = format.extra.xma2.bytesPerBlock;
		assert( format.extra.xma2.blockCount == ALIGN( totalBufferSize, bytesPerBlock ) / bytesPerBlock );
		assert( format.extra.xma2.blockCount * bytesPerBlock >= totalBufferSize );
		assert( format.extra.xma2.blockCount * bytesPerBlock < totalBufferSize + bytesPerBlock );

		buffers.SetNum( format.extra.xma2.blockCount );
		for ( int i = 0; i < buffers.Num(); i++ ) {
			if ( i == buffers.Num() - 1 ) {
				buffers[i].bufferSize = totalBufferSize - ( i * bytesPerBlock );
			} else {
				buffers[i].bufferSize = bytesPerBlock;
			}

			buffers[i].buffer = AllocBuffer( buffers[i].bufferSize, GetName() );
			wave.Read( buffers[i].buffer, buffers[i].bufferSize );
			buffers[i].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[i].buffer );
		}

		int seekTableSize = wave.SeekToChunk( 'seek' );
		if ( seekTableSize != 4 * buffers.Num() ) {
			idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), "Wrong number of entries in seek table" );
			MakeDefault();
			return false;
		}

		for ( int i = 0; i < buffers.Num(); i++ ) {
			wave.Read( &buffers[i].numSamples, sizeof( buffers[i].numSamples ) );
			idSwap::Big( buffers[i].numSamples );
		}

		playBegin = format.extra.xma2.loopBegin;
		playLength = format.extra.xma2.loopLength;

		if ( buffers[buffers.Num()-1].numSamples < playBegin + playLength ) {
			// This shouldn't happen, but it's not fatal if it does
			playLength = buffers[buffers.Num()-1].numSamples - playBegin;
		} else {
			// Discard samples beyond playLength
			for ( int i = 0; i < buffers.Num(); i++ ) {
				if ( buffers[i].numSamples > playBegin + playLength ) {
					buffers[i].numSamples = playBegin + playLength;
					// Ideally, the following loop should always have 0 iterations because playBegin + playLength ends in the last block already
					// But there is no guarantee for that, so to be safe, discard all buffers beyond this one
					for ( int j = i + 1; j < buffers.Num(); j++ ) {
						FreeBuffer( buffers[j].buffer );
					}
					buffers.SetNum( i + 1 );
					break;
				}
			}
		}

	} else {
		idLib::Warning( "LoadWav( %s ) : Unsupported wave format %d", filename.c_str(), format.basic.formatTag );
		MakeDefault();
		return false;
	}

	wave.Close();

	if ( format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE ) {
		// HACK: XAudio2 doesn't really support FORMAT_EXTENSIBLE so we convert it to a basic format after extracting the channel mask
		format.basic.formatTag = format.extra.extensible.subFormat.data1;
	}

	// sanity check...
	assert( buffers[buffers.Num()-1].numSamples == playBegin + playLength );

	return true;
}


/*
========================
idSoundSample_3DS::MakeDefault
========================
*/
void idSoundSample_3DS::MakeDefault() {
	FreeData();

	static const int DEFAULT_NUM_SAMPLES = 256;

	timestamp = FILE_NOT_FOUND_TIMESTAMP;
	loaded = true;

	memset( &format, 0, sizeof( format ) );
	format.basic.formatTag = idWaveFile::FORMAT_PCM;
	format.basic.numChannels = 1;
	format.basic.bitsPerSample = 16;
	format.basic.samplesPerSec = SND_3DS_MIN_SAMPLE_RATE;
	format.basic.blockSize = format.basic.numChannels * format.basic.bitsPerSample / 8;
	format.basic.avgBytesPerSec = format.basic.samplesPerSec * format.basic.blockSize;

	assert( format.basic.blockSize == 2 );

	totalBufferSize = DEFAULT_NUM_SAMPLES * 2;

	short * defaultBuffer = (short *)AllocBuffer( totalBufferSize, GetName() );
	for ( int i = 0; i < DEFAULT_NUM_SAMPLES; i += 2 ) {
		defaultBuffer[i + 0] = SHRT_MIN;
		defaultBuffer[i + 1] = SHRT_MAX;
	}

	buffers.SetNum( 1 );
	buffers[0].buffer = defaultBuffer;
	buffers[0].bufferSize = totalBufferSize;
	buffers[0].numSamples = DEFAULT_NUM_SAMPLES;
	buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );

	playBegin = 0;
	playLength = DEFAULT_NUM_SAMPLES;
	N3DS_MarkResident();
}

/*
========================
idSoundSample_3DS::N3DS_MarkResident

Data read whole (a .wav or the default beep): resident, never dropped.
========================
*/
void idSoundSample_3DS::N3DS_MarkResident() {
	generatedName.Clear();
	dataLoaded = true;
	for ( int i = 0; i < buffers.Num(); i++ ) {
		buffers[i].fileOffset = 0;
		sampleDataBytes += buffers[i].bufferSize;
	}
}

/*
========================
idSoundSample_3DS::FreeData

Called before deleting the object and at the start of LoadResource()
========================
*/
void idSoundSample_3DS::FreeData() {
	if ( buffers.Num() > 0 ) {
		soundSystemLocal.StopVoicesWithSample( (idSoundSample *)this );
		FreeBuffers();
		N3DS_SoundLock();
		buffers.Clear();
		N3DS_SoundUnlock();
	}
	generatedName.Clear();
	dataLoaded = false;
	amplitude.Clear();

	timestamp = FILE_NOT_FOUND_TIMESTAMP;
	memset( &format, 0, sizeof( format ) );
	loaded = false;
	totalBufferSize = 0;
	playBegin = 0;
	playLength = 0;
}

/*
========================
idSoundSample_3DS::LoadAmplitude
========================
*/
bool idSoundSample_3DS::LoadAmplitude( const idStr & name ) {
	amplitude.Clear();
	idFileLocal f( fileSystem->OpenFileRead( name ) );
	if ( f == NULL ) {
		return false;
	}
	amplitude.SetNum( f->Length() );
	f->Read( amplitude.Ptr(), amplitude.Num() );
	return true;
}

/*
========================
idSoundSample_3DS::GetAmplitude
========================
*/
float idSoundSample_3DS::GetAmplitude( int timeMS ) const {
	if ( timeMS < 0 || timeMS > LengthInMsec() ) {
		return 0.0f;
	}
	if ( IsDefault() ) {
		return 1.0f;
	}
	int index = timeMS * 60 / 1000;
	if ( index < 0 || index >= amplitude.Num() ) {
		return 0.0f;
	}
	return (float)amplitude[index] / 255.0f;
}
