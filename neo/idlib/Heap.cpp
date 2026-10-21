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
#include "../idlib/precompiled.h"

//===============================================================
//
//	memory allocation all in one place
//
//===============================================================

#undef new

#if defined( ID_3DS )

extern "C" {
#include <3ds/types.h>
#include <3ds/synchronization.h>
}

/*
================================================================================================

	New 3DS temp arena

	Transient TAG_TEMP blocks (whole-file reads of resources, decompression
	buffers) are what fragments the 96 MB heap: a 1 MB buffer allocated between
	two long-lived allocations leaves a 1 MB hole that later only fits smaller
	blocks. After a map load the heap had ~8 MB free but not one 1 MB piece.
	Large TEMP blocks therefore come from a separate block reserved at the
	first allocation, with a first-fit, coalescing free list. When the arena
	cannot satisfy a request it falls back to the normal heap.

================================================================================================
*/

static const int	MEM_TEMP_ARENA_SIZE = 4 * 1024 * 1024;
static const int	MEM_TEMP_ARENA_MIN_BLOCK = 32 * 1024;	// smaller TEMP blocks stay in the heap

struct arenaBlock_t {
	uint32			size;		// payload bytes (multiple of 16)
	uint32			isFree;
	arenaBlock_t *	prev;		// physically previous block, NULL for the first
	uint32			pad;
};
static_assert( sizeof( arenaBlock_t ) == 16, "arena header must preserve 16-byte alignment" );

static byte *		memArena = NULL;
static byte *		memArenaEnd = NULL;
static LightLock	memArenaLock;
static int			memArenaUsed = 0;
static int			memArenaPeak = 0;

static arenaBlock_t * Arena_Next( arenaBlock_t * b ) {
	byte * n = (byte *)b + sizeof( arenaBlock_t ) + b->size;
	return ( n < memArenaEnd ) ? (arenaBlock_t *)n : NULL;
}

static void Arena_Init() {
	memArena = (byte *)memalign( 16, MEM_TEMP_ARENA_SIZE );
	if ( memArena == NULL ) {
		return;
	}
	memArenaEnd = memArena + MEM_TEMP_ARENA_SIZE;
	LightLock_Init( &memArenaLock );
	arenaBlock_t * b = (arenaBlock_t *)memArena;
	b->size = MEM_TEMP_ARENA_SIZE - sizeof( arenaBlock_t );
	b->isFree = 1;
	b->prev = NULL;
	b->pad = 0;
}

static bool Arena_Owns( const void * p ) {
	return memArena != NULL && (const byte *)p >= memArena && (const byte *)p < memArenaEnd;
}

static void * Arena_Alloc( int size ) {
	if ( memArena == NULL ) {
		return NULL;
	}
	const uint32 need = ( size + 15 ) & ~15;
	void * result = NULL;
	LightLock_Lock( &memArenaLock );
	for ( arenaBlock_t * b = (arenaBlock_t *)memArena; b != NULL; b = Arena_Next( b ) ) {
		if ( !b->isFree || b->size < need ) {
			continue;
		}
		if ( b->size >= need + sizeof( arenaBlock_t ) + 256 ) {
			// split, the rest stays free
			arenaBlock_t * rest = (arenaBlock_t *)( (byte *)b + sizeof( arenaBlock_t ) + need );
			rest->size = b->size - need - sizeof( arenaBlock_t );
			rest->isFree = 1;
			rest->prev = b;
			rest->pad = 0;
			arenaBlock_t * after = Arena_Next( rest );
			if ( after != NULL ) {
				after->prev = rest;
			}
			b->size = need;
		}
		b->isFree = 0;
		memArenaUsed += b->size + sizeof( arenaBlock_t );
		if ( memArenaUsed > memArenaPeak ) {
			memArenaPeak = memArenaUsed;
		}
		result = (byte *)b + sizeof( arenaBlock_t );
		break;
	}
	LightLock_Unlock( &memArenaLock );
	return result;
}

static void Arena_Free( void * p ) {
	LightLock_Lock( &memArenaLock );
	arenaBlock_t * b = (arenaBlock_t *)( (byte *)p - sizeof( arenaBlock_t ) );
	assert( !b->isFree );
	b->isFree = 1;
	memArenaUsed -= b->size + sizeof( arenaBlock_t );
	// merge with the next block
	arenaBlock_t * n = Arena_Next( b );
	if ( n != NULL && n->isFree ) {
		b->size += sizeof( arenaBlock_t ) + n->size;
		arenaBlock_t * after = Arena_Next( b );
		if ( after != NULL ) {
			after->prev = b;
		}
	}
	// merge into the previous block
	arenaBlock_t * pr = b->prev;
	if ( pr != NULL && pr->isFree ) {
		pr->size += sizeof( arenaBlock_t ) + b->size;
		arenaBlock_t * after = Arena_Next( pr );
		if ( after != NULL ) {
			after->prev = pr;
		}
	}
	LightLock_Unlock( &memArenaLock );
}

void Mem_ArenaStats( int & used, int & peak, int & size ) {
	used = memArenaUsed;
	peak = memArenaPeak;
	size = ( memArena != NULL ) ? MEM_TEMP_ARENA_SIZE : 0;
}

/*
==================
Mem_LargestFreeBlock

Binary search with real allocations; only for diagnostics at checkpoints.
==================
*/
int Mem_LargestFreeBlock() {
	int lo = 0;
	int hi = 64 * 1024 * 1024;
	while ( hi - lo > 16 * 1024 ) {
		const int mid = lo + ( hi - lo ) / 2;
		void * p = malloc( mid );
		if ( p != NULL ) {
			free( p );
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return lo;
}

/*
================================================================================================

	New 3DS heap accounting

	Per-tag accounting is optional because it costs a 16-byte header per block
	(~4 MB with the ~250k blocks of a loaded map). It is enabled when the file
	sdmc:/doom3/memtags exists; the choice is made once, at the first allocation,
	and never changes, so every block is freed the way it was allocated.
	With tracking on, each block carries its size and memTag_t and Mem_LogTags
	prints a breakdown by subsystem at checkpoints and on out-of-memory.

================================================================================================
*/

struct memHeader_t {
	uint32	size;
	uint16	tag;
	uint16	magic;
	uint32	pad[2];
};
static_assert( sizeof( memHeader_t ) == 16, "header must preserve 16-byte alignment" );

static const uint16 MEM_HEADER_MAGIC = 0xD35D;

static const char * memTagNames[] = {
#define MEM_TAG( x )	#x,
#include "sys/sys_alloc_tags.h"
};

static int		memTrackingState = -1;		// -1 undecided, 0 off, 1 on

static bool Mem_TrackingEnabled() {
	if ( memTrackingState < 0 ) {
		FILE * f = fopen( "sdmc:/doom3/memtags", "rb" );
		memTrackingState = ( f != NULL ) ? 1 : 0;
		if ( f != NULL ) {
			fclose( f );
		}
	}
	return memTrackingState == 1;
}

static int32	memTagBytes[TAG_NUM_TAGS];
static int32	memTagBlocks[TAG_NUM_TAGS];
static int32	memTotalBytes = 0;
static int32	memPeakBytes = 0;

static void Mem_Account( int tag, int bytes, int blocks ) {
	if ( tag < 0 || tag >= TAG_NUM_TAGS ) {
		tag = TAG_UNSET;
	}
	__atomic_add_fetch( &memTagBytes[tag], bytes, __ATOMIC_RELAXED );
	__atomic_add_fetch( &memTagBlocks[tag], blocks, __ATOMIC_RELAXED );
	const int32 total = __atomic_add_fetch( &memTotalBytes, bytes, __ATOMIC_RELAXED );
	if ( total > memPeakBytes ) {
		memPeakBytes = total;
	}
}

/*
==================
Mem_LogTags

Prints the biggest users of the engine heap.
==================
*/
void Mem_LogTags( int maxLines ) {
	if ( memTrackingState != 1 ) {
		return;		// create sdmc:/doom3/memtags for the per-subsystem breakdown
	}
	int order[TAG_NUM_TAGS];
	int n = 0;
	for ( int i = 0; i < TAG_NUM_TAGS; i++ ) {
		if ( memTagBytes[i] > 0 ) {
			order[n++] = i;
		}
	}
	// small n: insertion sort by bytes, descending
	for ( int i = 1; i < n; i++ ) {
		int t = order[i];
		int j = i - 1;
		while ( j >= 0 && memTagBytes[order[j]] < memTagBytes[t] ) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = t;
	}
	idLib::Printf( "Engine heap: %d KB in use, peak %d KB\n", memTotalBytes / 1024, memPeakBytes / 1024 );
	for ( int i = 0; i < n && i < maxLines; i++ ) {
		const int t = order[i];
		idLib::Printf( "  %-22s %7d KB %7d blocks\n", memTagNames[t], memTagBytes[t] / 1024, memTagBlocks[t] );
	}
}

void * operator new( size_t s ) {
	// operator new must return a unique pointer even for 0 bytes
	return Mem_Alloc( s ? (int)s : 1, TAG_NEW );
}
void operator delete( void *p ) noexcept {
	Mem_Free( p );
}
void * operator new[]( size_t s ) {
	return Mem_Alloc( s ? (int)s : 1, TAG_NEW );
}
void operator delete[]( void *p ) noexcept {
	Mem_Free( p );
}

/*
==================
Mem_Alloc16
==================
*/
static void * memEmergencyReserve = NULL;
static const int MEM_EMERGENCY_RESERVE = 512 * 1024;

void * Mem_Alloc16( const int size, const memTag_t tag ) {
	if ( !size ) {
		return NULL;
	}
	if ( memEmergencyReserve == NULL ) {
		static bool reserved = false;
		if ( !reserved ) {
			reserved = true;
			memEmergencyReserve = malloc( MEM_EMERGENCY_RESERVE );
			Arena_Init();
		}
	}
	const int paddedSize = ( size + 15 ) & ~15;
	const bool tracking = Mem_TrackingEnabled();
	const int rawSize = paddedSize + ( tracking ? sizeof( memHeader_t ) : 0 );
	byte * raw = NULL;
	if ( tag == TAG_TEMP && size >= MEM_TEMP_ARENA_MIN_BLOCK ) {
		raw = (byte *)Arena_Alloc( rawSize );
	}
	if ( raw == NULL ) {
		raw = (byte *)memalign( 16, rawSize );
	}
	if ( raw == NULL ) {
		// give the error path (printing, log file, shutdown) room to work
		if ( memEmergencyReserve != NULL ) {
			free( memEmergencyReserve );
			memEmergencyReserve = NULL;
		} else {
			// already failed once: no more reports, just stop
			idLib::FatalError( "Mem_Alloc16: out of memory (again) allocating %d bytes", paddedSize );
		}
		// Running out of memory is the expected failure on 3DS: say so, with the
		// breakdown, instead of letting the caller crash on a NULL pointer later.
		struct mallinfo mi = mallinfo();
		idLib::Printf( "Mem_Alloc16: out of memory allocating %d KB for %s, malloc %d KB in use, largest free block %d KB, temp arena %d / %d KB\n",
			paddedSize / 1024, ( tag >= 0 && tag < TAG_NUM_TAGS ) ? memTagNames[tag] : "?", (int)( mi.uordblks / 1024 ),
			Mem_LargestFreeBlock() / 1024, memArenaUsed / 1024, ( memArena != NULL ) ? MEM_TEMP_ARENA_SIZE / 1024 : 0 );
		Mem_LogTags( 24 );
		idLib::FatalError( "Mem_Alloc16: out of memory allocating %d KB for %s",
			paddedSize / 1024, ( tag >= 0 && tag < TAG_NUM_TAGS ) ? memTagNames[tag] : "?" );
	}
	if ( !tracking ) {
		return raw;
	}
	memHeader_t * hdr = (memHeader_t *)raw;
	hdr->size = paddedSize;
	hdr->tag = (uint16)tag;
	hdr->magic = MEM_HEADER_MAGIC;
	Mem_Account( tag, paddedSize, 1 );
	return raw + sizeof( memHeader_t );
}

/*
==================
Mem_Free16
==================
*/
void Mem_Free16( void *ptr ) {
	if ( ptr == NULL ) {
		return;
	}
	if ( memTrackingState != 1 ) {
		if ( Arena_Owns( ptr ) ) {
			Arena_Free( ptr );
		} else {
			free( ptr );
		}
		return;
	}
	memHeader_t * hdr = (memHeader_t *)( (byte *)ptr - sizeof( memHeader_t ) );
	if ( Arena_Owns( hdr ) ) {
		hdr->magic = 0;
		Mem_Account( hdr->tag, -(int)hdr->size, -1 );
		Arena_Free( hdr );
		return;
	}
	if ( hdr->magic != MEM_HEADER_MAGIC ) {
		// not ours: plain malloc block
		free( ptr );
		return;
	}
	hdr->magic = 0;
	Mem_Account( hdr->tag, -(int)hdr->size, -1 );
	free( hdr );
}

#else // !ID_3DS

/*
==================
Mem_Alloc16
==================
*/
void * Mem_Alloc16( const int size, const memTag_t tag ) {
	if ( !size ) {
		return NULL;
	}
	const int paddedSize = ( size + 15 ) & ~15;
	return _aligned_malloc( paddedSize, 16 );
}

/*
==================
Mem_Free16
==================
*/
void Mem_Free16( void *ptr ) {
	if ( ptr == NULL ) {
		return;
	}
	_aligned_free( ptr );
}

#endif // ID_3DS

/*
==================
Mem_ClearedAlloc
==================
*/
void * Mem_ClearedAlloc( const int size, const memTag_t tag ) {
	void * mem = Mem_Alloc( size, tag );
	SIMDProcessor->Memset( mem, 0, size );
	return mem;
}

/*
==================
Mem_CopyString
==================
*/
char *Mem_CopyString( const char *in ) {
	char * out = (char *)Mem_Alloc( strlen(in) + 1, TAG_STRING );
	strcpy( out, in );
	return out;
}

