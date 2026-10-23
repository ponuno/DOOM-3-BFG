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
#include "../../renderer/3DS/n3ds_pack.h"
#include "n3ds_soundpack.h"

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
	amplitude) and remembers where each buffer's data sits in the file and in the
	container's OS file. Small samples are read right away; the rest are read the
	first time a voice plays them by a loader thread with its own unbuffered FILE
	(PrepareData; the voice waits in Update() for the few ms that takes instead of
	the main thread stalling on the SD card). All sample data shares
	n3ds_soundMemoryKB: when it is full, the least recently played samples that no
	voice uses give their data back.

================================================================================================
*/
// mars_city1 ran out of heap in the sound EndLevelLoad with 7.1 MB of samples resident
// (every sample up to 48 KB was read at load); with reads in a loader thread the
// budget can be small and only short sounds need to be there beforehand
idCVar n3ds_soundMemoryKB( "n3ds_soundMemoryKB", "4096", CVAR_SOUND | CVAR_INTEGER, "New 3DS: memory for sound sample data; least recently played samples are dropped when it is full" );
idCVar n3ds_soundPreloadKB( "n3ds_soundPreloadKB", "16", CVAR_SOUND | CVAR_INTEGER, "New 3DS: samples up to this size are read at level load, bigger ones when first played" );
idCVar n3ds_soundPack( "n3ds_soundPack", "1", CVAR_SOUND | CVAR_BOOL | CVAR_INIT, "New 3DS: play samples from base/_n3ds_sounds.pak (DSP-ADPCM made by n3dstex, decoded by the DSP) when it is there" );

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
idSoundSample_3DS::N3DS_LogMemory

Memory diagnostics: resident sample data and the biggest resident samples.
========================
*/
void N3DS_SoundLogMemory() {
	idSoundSample_3DS::N3DS_LogMemory();
}



/*
================================================================================================

	Loader thread

================================================================================================
*/
enum {
	SND_LOAD_IDLE,		// nothing pending (data in memory or not)
	SND_LOAD_QUEUED,
	SND_LOAD_LOADING,
	SND_LOAD_READY,		// loadedBuffers filled; the main thread takes them
	SND_LOAD_FAILED
};

static const int SND_QUEUE_SIZE = 128;
static const int SND_MAX_PATHS = 16;

struct sndStreamPath_t {
	idStrStatic< MAX_OSPATH >	path;
	FILE *						file;		// loader thread's own handle
};

static idSysMutex			sndLoadLock;
static idSoundSample_3DS *	sndQueue[SND_QUEUE_SIZE];
static int					sndQueueHead = 0;
static int					sndQueueCount = 0;
static sndStreamPath_t		sndPaths[SND_MAX_PATHS];
static int					sndReads = 0;
static int					sndReadBytes = 0;

// under sndLoadLock; -1 when the table is full (the sample is then read the old way)
static int N3DS_SoundPathIndex( const char * osPath ) {
	for ( int i = 0; i < SND_MAX_PATHS; i++ ) {
		if ( !sndPaths[i].path.IsEmpty() && sndPaths[i].path.Icmp( osPath ) == 0 ) {
			return i;
		}
	}
	for ( int i = 0; i < SND_MAX_PATHS; i++ ) {
		if ( sndPaths[i].path.IsEmpty() ) {
			sndPaths[i].path = osPath;
			return i;
		}
	}
	return -1;
}

class idN3DSSoundLoader : public idSysThread {
public:
	virtual int Run() {
		for ( ;; ) {
			sndLoadLock.Lock();
			idSoundSample_3DS * s = NULL;
			while ( sndQueueCount > 0 && s == NULL ) {
				s = sndQueue[sndQueueHead];
				sndQueue[sndQueueHead] = NULL;
				sndQueueHead = ( sndQueueHead + 1 ) % SND_QUEUE_SIZE;
				sndQueueCount--;
			}
			if ( s == NULL ) {
				sndLoadLock.Unlock();
				return 0;
			}
			s->loadState = SND_LOAD_LOADING;
			const int pathIndex = s->streamPath;
			FILE * f = sndPaths[pathIndex].file;
			idStrStatic< MAX_OSPATH > path = sndPaths[pathIndex].path;
			sndLoadLock.Unlock();

			// buffers and loadedBuffers are not resized while the sample is LOADING (CancelLoad waits)
			if ( f == NULL ) {
				f = fopen( path.c_str(), "rb" );
				if ( f != NULL ) {
					setvbuf( f, NULL, _IONBF, 0 );
					sndLoadLock.Lock();
					sndPaths[pathIndex].file = f;
					sndLoadLock.Unlock();
				}
			}
			bool ok = ( f != NULL );
			int bytes = 0;
			for ( int i = 0; ok && i < s->buffers.Num(); i++ ) {
				const int size = s->buffers[i].bufferSize;
				void * data = Mem_Alloc( size, TAG_AUDIO );
				s->loadedBuffers[i] = data;
				ok = data != NULL && fseek( f, s->buffers[i].osOffset, SEEK_SET ) == 0 && (int)fread( data, 1, size, f ) == size;
				bytes += size;
			}
			if ( !ok ) {
				for ( int i = 0; i < s->loadedBuffers.Num(); i++ ) {
					Mem_Free( s->loadedBuffers[i] );
					s->loadedBuffers[i] = NULL;
				}
			}
			sndLoadLock.Lock();
			s->loadState = ok ? SND_LOAD_READY : SND_LOAD_FAILED;
			if ( ok ) {
				sndReads++;
				sndReadBytes += bytes;
			}
			sndLoadLock.Unlock();
		}
	}
};

static idN3DSSoundLoader *	sndLoader = NULL;

/*
================================================================================================

	Sound pack

	The pack holds every sample re-encoded as DSP-ADPCM (tools/n3ds/n3dstex). A sample
	found there keeps the header (format, length, amplitude) of its .idwav but takes its
	data from the pack: one stream per channel, read by the loader thread like the .idwav
	buffers, and played by the DSP as it is (3DS_SoundVoice.cpp).

================================================================================================
*/
static bool						sndPackTried = false;
static n3dsSoundPackEntry_t *	sndPackEntries = NULL;
static int						sndPackNum = 0;
static short					sndPackCoefs[16];
static idStrStatic< MAX_OSPATH >	sndPackOSPath;
static FILE *					sndPackMainFile = NULL;		// main thread reads (preload)
static int						sndPackSamples = 0;

const short * N3DS_SoundPackCoefs() {
	return sndPackCoefs;
}

static void N3DS_SoundPackInit() {
	sndPackTried = true;
	if ( !n3ds_soundPack.GetBool() ) {
		return;
	}
	const char * osPath = fileSystem->RelativePathToOSPath( "_n3ds_sounds.pak", "fs_basepath" );
	FILE * f = fopen( osPath, "rb" );
	if ( f == NULL ) {
		idLib::Printf( "Sound: no sound pack (%s): samples are decoded by the CPU\n", osPath );
		return;
	}
	n3dsSoundPackHeader_t header;
	bool ok = fread( &header, sizeof( header ), 1, f ) == 1 && header.magic == N3DS_SOUNDPACK_MAGIC &&
		header.version == N3DS_SOUNDPACK_VERSION && header.numEntries > 0 && header.numEntries < 1000000;
	long fileLength = 0;
	if ( ok && fseek( f, 0, SEEK_END ) == 0 ) {
		fileLength = ftell( f );
	}
	ok = ok && (uint64)header.entryOffset + (uint64)header.numEntries * sizeof( n3dsSoundPackEntry_t ) <= (uint64)fileLength;
	if ( ok ) {
		sndPackEntries = (n3dsSoundPackEntry_t *)Mem_Alloc( header.numEntries * sizeof( n3dsSoundPackEntry_t ), TAG_AUDIO );
		ok = fseek( f, header.entryOffset, SEEK_SET ) == 0 &&
			fread( sndPackEntries, sizeof( n3dsSoundPackEntry_t ), header.numEntries, f ) == header.numEntries;
		if ( !ok ) {
			Mem_Free( sndPackEntries );
			sndPackEntries = NULL;
		}
	}
	fclose( f );
	if ( !ok ) {
		idLib::Warning( "Sound: %s is not a sound pack of version %d or is damaged (convert again with n3dstex)", osPath, N3DS_SOUNDPACK_VERSION );
		return;
	}
	sndPackNum = header.numEntries;
	memcpy( sndPackCoefs, header.coefs, sizeof( sndPackCoefs ) );
	sndPackOSPath = osPath;
	idLib::Printf( "Sound: sound pack %s: %d samples, %d MB (DSP-ADPCM, decoded by the DSP)\n", osPath, sndPackNum, (int)( fileLength >> 20 ) );
}

static const n3dsSoundPackEntry_t * N3DS_SoundPackFind( const char * generatedFileName ) {
	if ( !sndPackTried ) {
		N3DS_SoundPackInit();
	}
	if ( sndPackEntries == NULL ) {
		return NULL;
	}
	const uint64 h = N3DS_PackHash( generatedFileName );
	int lo = 0, hi = sndPackNum - 1;
	while ( lo <= hi ) {
		const int mid = ( lo + hi ) >> 1;
		const uint64 m = sndPackEntries[mid].nameHash;
		if ( m == h ) {
			return &sndPackEntries[mid];
		}
		if ( m < h ) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return NULL;
}

/*
========================
idSoundSample_3DS::PrepareData

Main thread, when a voice starts or waits to start: 1 when the data is in memory,
0 while it is being read, -1 when it cannot be read.
========================
*/
int idSoundSample_3DS::PrepareData() {
	lastUse = ++sampleUseClock;
	if ( dataLoaded ) {
		return 1;
	}
	if ( generatedName.IsEmpty() ) {
		return -1;
	}
	if ( streamPath < 0 ) {
		return EnsureData() ? 1 : -1;		// no OS file to read from: through the engine
	}
	sndLoadLock.Lock();
	const int state = loadState;
	if ( state == SND_LOAD_READY ) {
		loadState = SND_LOAD_IDLE;
		sndLoadLock.Unlock();
		N3DS_SoundLock();
		for ( int i = 0; i < buffers.Num(); i++ ) {
			buffers[i].buffer = loadedBuffers[i];
			loadedBuffers[i] = NULL;
			sampleDataBytes += buffers[i].bufferSize;
		}
		dataLoaded = true;
		N3DS_SoundUnlock();
		sampleLoadsOnDemand++;
		return 1;
	}
	if ( state == SND_LOAD_FAILED ) {
		loadState = SND_LOAD_IDLE;
		sndLoadLock.Unlock();
		return -1;
	}
	if ( state != SND_LOAD_IDLE || sndQueueCount >= SND_QUEUE_SIZE ) {
		sndLoadLock.Unlock();
		return 0;		// queued or being read (or the queue is full: asked again next frame)
	}
	sndLoadLock.Unlock();

	// room for it now, so that the data fits when it arrives
	MakeRoom( totalBufferSize );
	loadedBuffers.SetNum( buffers.Num() );
	for ( int i = 0; i < loadedBuffers.Num(); i++ ) {
		loadedBuffers[i] = NULL;
	}
	sndLoadLock.Lock();
	const bool wasEmpty = ( sndQueueCount == 0 );
	sndQueue[( sndQueueHead + sndQueueCount ) % SND_QUEUE_SIZE] = this;
	sndQueueCount++;
	loadState = SND_LOAD_QUEUED;
	sndLoadLock.Unlock();
	if ( sndLoader == NULL ) {
		sndLoader = new (TAG_AUDIO) idN3DSSoundLoader;
		sndLoader->StartWorkerThread( "N3DS_SoundLoader", CORE_0A, THREAD_ABOVE_NORMAL, 32 * 1024 );
	}
	if ( wasEmpty ) {
		sndLoader->SignalWork();		// a non-empty queue has a wake-up pending already
	}
	return 0;
}

/*
========================
idSoundSample_3DS::CancelLoad

Main thread: takes the sample out of the queue, or waits for the read in progress
(a few ms), and drops what was read.
========================
*/
void idSoundSample_3DS::CancelLoad() {
	for ( ;; ) {
		sndLoadLock.Lock();
		if ( loadState != SND_LOAD_LOADING ) {
			break;
		}
		sndLoadLock.Unlock();
		Sys_Sleep( 1 );
	}
	if ( loadState == SND_LOAD_QUEUED ) {
		for ( int i = 0; i < sndQueueCount; i++ ) {
			idSoundSample_3DS *& q = sndQueue[( sndQueueHead + i ) % SND_QUEUE_SIZE];
			if ( q == this ) {
				q = NULL;		// the loader skips it
			}
		}
	}
	loadState = SND_LOAD_IDLE;
	sndLoadLock.Unlock();
	for ( int i = 0; i < loadedBuffers.Num(); i++ ) {
		Mem_Free( loadedBuffers[i] );
	}
	loadedBuffers.Clear();
}

void idSoundSample_3DS::N3DS_LogMemory() {
	if ( allSamples.Num() == 0 ) {
		return;
	}
	const int TOP = 6;
	const idSoundSample_3DS * top[TOP] = {};
	int resident = 0, residentBytes = 0, whole = 0, wholeBytes = 0, onDemand = 0;
	for ( int i = 0; i < allSamples.Num(); i++ ) {
		const idSoundSample_3DS * s = allSamples[i];
		if ( !s->generatedName.IsEmpty() && !s->dataLoaded ) {
			onDemand++;
		}
		if ( !s->dataLoaded ) {
			continue;
		}
		resident++;
		residentBytes += s->totalBufferSize;
		if ( s->generatedName.IsEmpty() ) {
			whole++;
			wholeBytes += s->totalBufferSize;
		}
		for ( int k = 0; k < TOP; k++ ) {
			if ( top[k] == NULL || s->totalBufferSize > top[k]->totalBufferSize ) {
				for ( int m = TOP - 1; m > k; m-- ) {
					top[m] = top[m - 1];
				}
				top[k] = s;
				break;
			}
		}
	}
	idLib::Printf( "  sound: %d samples, %d resident %d KB (%d never dropped %d KB), %d not loaded, budget %d KB\n",
		allSamples.Num(), resident, residentBytes / 1024, whole, wholeBytes / 1024, onDemand, n3ds_soundMemoryKB.GetInteger() );
	for ( int k = 0; k < TOP && top[k] != NULL; k++ ) {
		idLib::Printf( "    %4d KB %s%s%s\n", top[k]->totalBufferSize / 1024, top[k]->GetName(),
			top[k]->neverPurge ? " (never purge)" : "", top[k]->activeVoices > 0 ? " (playing)" : "" );
	}
	// all samples of the level by format, rate and channels (their full data size)
	struct fmtStat_t { int tag, rate, channels, count, bytes; };
	fmtStat_t fmts[8];
	int numFmts = 0;
	for ( int i = 0; i < allSamples.Num(); i++ ) {
		const idSoundSample_3DS * s = allSamples[i];
		if ( s->totalBufferSize <= 0 ) {
			continue;
		}
		int k = 0;
		for ( ; k < numFmts; k++ ) {
			if ( fmts[k].tag == s->format.basic.formatTag && fmts[k].rate == (int)s->format.basic.samplesPerSec &&
					fmts[k].channels == s->format.basic.numChannels ) {
				break;
			}
		}
		if ( k == numFmts ) {
			if ( numFmts == 8 ) {
				continue;
			}
			fmts[k].tag = s->format.basic.formatTag;
			fmts[k].rate = s->format.basic.samplesPerSec;
			fmts[k].channels = s->format.basic.numChannels;
			fmts[k].count = fmts[k].bytes = 0;
			numFmts++;
		}
		fmts[k].count++;
		fmts[k].bytes += s->totalBufferSize;
	}
	for ( int k = 0; k < numFmts; k++ ) {
		idLib::Printf( "    format %s %d Hz %d ch: %d samples, %d KB\n",
			fmts[k].tag == idWaveFile::FORMAT_PCM ? "PCM16" : fmts[k].tag == idWaveFile::FORMAT_ADPCM ? "ADPCM" :
			fmts[k].tag == N3DS_FORMAT_DSPADPCM ? "DSP-ADPCM (pack)" : "other",
			fmts[k].rate, fmts[k].channels, fmts[k].count, fmts[k].bytes / 1024 );
	}
	idLib::Printf( "    loader: %d reads %d KB, %d queued\n", sndReads, sndReadBytes / 1024, sndQueueCount );
}

/*
========================
idSoundSample_3DS::idSoundSample_3DS
========================
*/
idSoundSample_3DS::idSoundSample_3DS() {
	dataLoaded = false;
	activeVoices = 0;
	lastUse = 0;
	streamPath = -1;
	loadState = 0;
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
	CancelLoad();
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
idSoundSample_3DS::N3DS_UsePack

After the .idwav header is read: if the pack has this sample (same channels and
length), its buffers become the pack's DSP-ADPCM streams, one per channel.
========================
*/
bool idSoundSample_3DS::N3DS_UsePack( const char * generatedFileName ) {
	const n3dsSoundPackEntry_t * e = N3DS_SoundPackFind( generatedFileName );
	if ( e == NULL || e->channels < 1 || e->channels > 2 || e->channels != format.basic.numChannels ||
			(int)e->numSamples != playLength || e->channelBytes != N3DS_DspAdpcmBytes( e->numSamples ) ) {
		return false;
	}
	const uint64 base = (uint64)e->dataOffset * N3DS_SOUNDPACK_ALIGN;
	if ( base + (uint64)e->channels * e->channelBytes > 0x7FFFFFFF ) {
		return false;
	}
	sndLoadLock.Lock();
	const int pathIndex = N3DS_SoundPathIndex( sndPackOSPath.c_str() );
	sndLoadLock.Unlock();
	if ( pathIndex < 0 ) {
		return false;
	}
	format.basic.formatTag = N3DS_FORMAT_DSPADPCM;
	format.basic.samplesPerSec = e->sampleRate;
	playBegin = 0;
	buffers.SetNum( e->channels );
	totalBufferSize = 0;
	for ( int c = 0; c < e->channels; c++ ) {
		buffers[c].buffer = NULL;
		buffers[c].bufferSize = e->channelBytes;
		buffers[c].numSamples = playLength;
		buffers[c].fileOffset = 0;
		buffers[c].osOffset = (int)( base + (uint64)c * e->channelBytes );
		totalBufferSize += e->channelBytes;
	}
	streamPath = pathIndex;
	sndPackSamples++;
	return true;
}

/*
========================
idSoundSample_3DS::ReadBuffersFromPack

Main thread (small samples at level load): straight from the pack.
========================
*/
bool idSoundSample_3DS::ReadBuffersFromPack() {
	if ( sndPackMainFile == NULL ) {
		sndPackMainFile = fopen( sndPackOSPath.c_str(), "rb" );
		if ( sndPackMainFile == NULL ) {
			return false;
		}
	}
	MakeRoom( totalBufferSize );
	for ( int i = 0; i < buffers.Num(); i++ ) {
		void * data = AllocBuffer( buffers[i].bufferSize, GetName() );
		if ( fseek( sndPackMainFile, buffers[i].osOffset, SEEK_SET ) != 0 ||
				(int)fread( data, 1, buffers[i].bufferSize, sndPackMainFile ) != buffers[i].bufferSize ) {
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
	bool streamable = true;
	idStr osPath;
	for ( int i = 0; i < num; i++ ) {
		fileIn->ReadBig( buffers[ i ].numSamples );
		fileIn->ReadBig( buffers[ i ].bufferSize );
		buffers[ i ].buffer = NULL;
		buffers[ i ].fileOffset = fileIn->Tell();
		idStr bufferPath;
		int osOffset = 0;
		if ( streamable && fileIn->GetDataLocation( bufferPath, osOffset ) && ( i == 0 || bufferPath.Icmp( osPath ) == 0 ) ) {
			osPath = bufferPath;
			buffers[ i ].osOffset = osOffset;
		} else {
			streamable = false;
			buffers[ i ].osOffset = 0;
		}
		total += buffers[ i ].bufferSize;
		if ( i + 1 < num ) {
			fileIn->Seek( buffers[ i ].bufferSize, FS_SEEK_CUR );
		}
	}
	generatedName = filename;
	dataLoaded = false;
	streamPath = -1;
	// converted on the PC: the DSP plays the pack's DSP-ADPCM, nothing is decoded here
	if ( N3DS_UsePack( filename ) ) {
		if ( totalBufferSize <= n3ds_soundPreloadKB.GetInteger() * 1024 ) {
			if ( !ReadBuffersFromPack() ) {
				FreeBuffers();
			}
		}
		return true;
	}
	if ( streamable && num > 0 ) {
		sndLoadLock.Lock();
		streamPath = N3DS_SoundPathIndex( osPath );
		sndLoadLock.Unlock();
	}
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
	CancelLoad();
	streamPath = -1;
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
