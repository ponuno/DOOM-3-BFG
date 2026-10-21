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

/*
================================================================================================

	New 3DS heap accounting

	Every Mem_Alloc16 block carries a 16-byte header (keeps 16-byte alignment) with
	its size and memTag_t, so usage can be broken down by subsystem when memory runs
	out or at checkpoints (Mem_LogTags). Blocks without a valid header (freed through
	Mem_Free16 but not allocated by Mem_Alloc16) fall back to a plain free().

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
void * Mem_Alloc16( const int size, const memTag_t tag ) {
	if ( !size ) {
		return NULL;
	}
	const int paddedSize = ( size + 15 ) & ~15;
	byte * raw = (byte *)memalign( 16, paddedSize + sizeof( memHeader_t ) );
	if ( raw == NULL ) {
		// Running out of memory is the expected failure on 3DS: say so, with the
		// breakdown, instead of letting the caller crash on a NULL pointer later.
		struct mallinfo mi = mallinfo();
		idLib::Printf( "Mem_Alloc16: out of memory allocating %d KB for %s, malloc %d KB in use\n",
			paddedSize / 1024, ( tag >= 0 && tag < TAG_NUM_TAGS ) ? memTagNames[tag] : "?", (int)( mi.uordblks / 1024 ) );
		Mem_LogTags( 24 );
		idLib::FatalError( "Mem_Alloc16: out of memory allocating %d KB for %s",
			paddedSize / 1024, ( tag >= 0 && tag < TAG_NUM_TAGS ) ? memTagNames[tag] : "?" );
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
	memHeader_t * hdr = (memHeader_t *)( (byte *)ptr - sizeof( memHeader_t ) );
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

