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
#include "tlsf/tlsf.h"

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

static const int	MEM_TEMP_ARENA_SIZE = 1536 * 1024;	// mars_city1 peak: 873 KB
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
================================================================================================

	New 3DS second heap (extended memory mode)

	The application heap window is 96 MB in every memory mode. With the 178 MB New 3DS
	mode (the CIA build) the rest of the application's memory can only be linear
	memory. N3DS_Main (after common->Init) gives the part the GPU does not need to Mem_InitSecondHeap; when
	malloc cannot satisfy a big block or a slab chunk, it comes from there. A TLSF
	allocator (O(1), immediate coalescing) manages it under its own lock: libctru's
	linear allocator is not thread safe and is only used by the main thread.

================================================================================================
*/

static tlsf_t			heap2 = NULL;
static uintptr_t		heap2Begin = 0;
static uintptr_t		heap2End = 0;
static LightLock		heap2Lock;
static int				heap2Size = 0;
static int				heap2Used = 0;
static int				heap2Peak = 0;

void Mem_InitSecondHeap( void * mem, int bytes ) {
	if ( heap2 != NULL || mem == NULL || bytes < 1024 * 1024 ) {
		return;
	}
	// TLSF wants its pool aligned to its own alignment (8 bytes covers 32 and 64 bits)
	const uintptr_t start = ( (uintptr_t)mem + 7 ) & ~(uintptr_t)7;
	bytes -= (int)( start - (uintptr_t)mem );
	bytes &= ~7;
	mem = (void *)start;
	// other threads are running by now: publish the heap only once it is complete
	LightLock_Init( &heap2Lock );
	tlsf_t h = tlsf_create_with_pool( mem, bytes );
	heap2Begin = (uintptr_t)mem;
	heap2End = heap2Begin + bytes;
	heap2Size = bytes;
	__sync_synchronize();
	heap2 = h;
}

static bool Heap2_Owns( const void * p ) {
	return heap2 != NULL && (uintptr_t)p >= heap2Begin && (uintptr_t)p < heap2End;
}

static void * Heap2_Malloc( int bytes ) {
	if ( heap2 == NULL ) {
		return NULL;
	}
	LightLock_Lock( &heap2Lock );
	void * p = tlsf_malloc( heap2, bytes );
	if ( p != NULL ) {
		heap2Used += (int)tlsf_block_size( p );
		if ( heap2Used > heap2Peak ) {
			heap2Peak = heap2Used;
		}
	}
	LightLock_Unlock( &heap2Lock );
	return p;
}

static void Heap2_Free( void * p ) {
	LightLock_Lock( &heap2Lock );
	heap2Used -= (int)tlsf_block_size( p );
	tlsf_free( heap2, p );
	LightLock_Unlock( &heap2Lock );
}

void Mem_SecondHeapStats( int & size, int & used, int & peak ) {
	size = heap2Size;
	used = heap2Used;
	peak = heap2Peak;
}

// malloc, else the second heap
static void * Raw_Malloc( int bytes ) {
	void * p = malloc( bytes );
	return ( p != NULL ) ? p : Heap2_Malloc( bytes );
}

static void Raw_Free( void * p ) {
	if ( Heap2_Owns( p ) ) {
		Heap2_Free( p );
	} else {
		free( p );
	}
}

/*
================================================================================================

	New 3DS block allocator

	Mem_Alloc16 used memalign( 16, n ). newlib's malloc aligns to 8 on ARM, so memalign
	asks malloc for n + 32 bytes and gives the unaligned leader and the spare tail
	back as free chunks of 16-32 bytes. No later memalign can use them (each asks for
	at least 48 bytes), so with ~250k engine blocks per map the heap filled with
	dead holes: mars_city1 failed a 4 KB allocation with 5.9 MB "free" in malloc.

	Now:
	- blocks up to SLAB_MAX bytes come from 16 KB pages, one size class (multiple of
	  16) per page, free slots chained inside the page. A page that empties goes to a
	  shared pool for any class; a 512 KB chunk whose pages are all empty goes back to
	  malloc, so level changes do not strand memory in the wrong size class.
	- bigger blocks are malloc( n + 23 ), aligned by hand inside; the 8 bytes before
	  the block hold a magic and the malloc pointer. One malloc chunk, nothing given
	  back in pieces.
	Both are 16-byte aligned like before and need no per-block malloc header for the
	small blocks. Pointers from anywhere else (plain malloc) are still freed with free().

================================================================================================
*/

static const int		SLAB_PAGE = 16 * 1024;
static const int		SLAB_MAX = 512;						// largest block from pages
static const int		SLAB_CLASSES = SLAB_MAX / 16;		// class c holds ( c + 1 ) * 16 byte blocks
static const int		SLAB_CHUNK = 512 * 1024;			// pages are taken from malloc this many at a time
static const int		SLAB_PAGES_PER_CHUNK = SLAB_CHUNK / SLAB_PAGE;
static const int		SLAB_MAX_CHUNKS = 255;
static const uintptr_t	SLAB_ADDR_BASE = 0x08000000;		// application heap address range
static const uintptr_t	SLAB_ADDR_END = 0x10000000;
static const int		SLAB_HEAP_PAGES = ( SLAB_ADDR_END - SLAB_ADDR_BASE ) / SLAB_PAGE;
static const int		SLAB_HEAP2_PAGES = ( 128 * 1024 * 1024 ) / SLAB_PAGE;	// second heap: up to 128 MB mapped
static const int		SLAB_MAP_PAGES = SLAB_HEAP_PAGES + SLAB_HEAP2_PAGES;

struct slabPage_t {
	uint16			objSize;		// 0 while in the empty pool
	uint16			used;
	uint16			capacity;
	uint16			bumped;			// slots handed out at least once (the rest were never touched)
	void *			freeList;
	slabPage_t *	prev;			// partial list of its class, or the empty pool (next only)
	slabPage_t *	next;
};
// blocks start after the header, rounded up to keep them 16-byte aligned
static const int		SLAB_HEADER = ( sizeof( slabPage_t ) + 15 ) & ~15;

struct slabChunk_t {
	void *			raw;			// malloc pointer, NULL when the slot is unused
	int				usedPages;		// pages not in the empty pool
};

static LightLock		slabLock;
static uint8			slabPageChunk[SLAB_MAP_PAGES];		// chunk index + 1 for slab pages, 0 otherwise
static slabChunk_t		slabChunks[SLAB_MAX_CHUNKS];
static slabPage_t *		slabPartial[SLAB_CLASSES];			// pages of each class with a free slot
static slabPage_t *		slabEmpty = NULL;					// pages of no class
static int				slabChunkCount = 0;
static int				slabUsedBytes = 0;					// in blocks handed out (their class size)
static int				slabEmptyPages = 0;

static inline int Slab_MapIndex( const void * p ) {
	const uintptr_t a = (uintptr_t)p;
	if ( a >= SLAB_ADDR_BASE && a < SLAB_ADDR_END ) {
		return (int)( ( a - SLAB_ADDR_BASE ) / SLAB_PAGE );
	}
	// slab pages are aligned to SLAB_PAGE in absolute addresses: index from the page
	// boundary below the heap start, not from the (unaligned) start itself
	const uintptr_t heap2MapBase = heap2Begin & ~(uintptr_t)( SLAB_PAGE - 1 );
	if ( Heap2_Owns( p ) && a - heap2MapBase < (uintptr_t)SLAB_HEAP2_PAGES * SLAB_PAGE ) {
		return SLAB_HEAP_PAGES + (int)( ( a - heap2MapBase ) / SLAB_PAGE );
	}
	return -1;
}

static bool Slab_Owns( const void * p ) {
	const int i = Slab_MapIndex( p );
	return i >= 0 && slabPageChunk[i] != 0;
}

static void Slab_Unlink( slabPage_t ** list, slabPage_t * page ) {
	if ( page->prev != NULL ) {
		page->prev->next = page->next;
	} else {
		*list = page->next;
	}
	if ( page->next != NULL ) {
		page->next->prev = page->prev;
	}
	page->prev = page->next = NULL;
}

static void Slab_PushFront( slabPage_t ** list, slabPage_t * page ) {
	page->prev = NULL;
	page->next = *list;
	if ( *list != NULL ) {
		( *list )->prev = page;
	}
	*list = page;
}

// a new 512 KB chunk: all its pages go to the empty pool
static bool Slab_AddChunk() {
	int slot = -1;
	for ( int i = 0; i < SLAB_MAX_CHUNKS; i++ ) {
		if ( slabChunks[i].raw == NULL ) {
			slot = i;
			break;
		}
	}
	if ( slot < 0 ) {
		return false;
	}
	void * raw = Raw_Malloc( SLAB_CHUNK + SLAB_PAGE );
	if ( raw == NULL ) {
		return false;
	}
	byte * base = (byte *)( ( (uintptr_t)raw + SLAB_PAGE - 1 ) & ~(uintptr_t)( SLAB_PAGE - 1 ) );
	if ( Slab_MapIndex( base ) < 0 || Slab_MapIndex( base + SLAB_CHUNK - 1 ) < 0 ) {
		Raw_Free( raw );		// outside the mapped address ranges: not ours to map
		return false;
	}
	slabChunks[slot].raw = raw;
	slabChunks[slot].usedPages = 0;
	slabChunkCount++;
	for ( int i = 0; i < SLAB_PAGES_PER_CHUNK; i++ ) {
		slabPage_t * page = (slabPage_t *)( base + i * SLAB_PAGE );
		memset( page, 0, sizeof( *page ) );
		slabPageChunk[Slab_MapIndex( page )] = (uint8)( slot + 1 );
		Slab_PushFront( &slabEmpty, page );
		slabEmptyPages++;
	}
	return true;
}

// every page of the chunk is in the empty pool: give it back to malloc
static void Slab_ReleaseChunk( int slot ) {
	byte * base = (byte *)( ( (uintptr_t)slabChunks[slot].raw + SLAB_PAGE - 1 ) & ~(uintptr_t)( SLAB_PAGE - 1 ) );
	for ( int i = 0; i < SLAB_PAGES_PER_CHUNK; i++ ) {
		slabPage_t * page = (slabPage_t *)( base + i * SLAB_PAGE );
		Slab_Unlink( &slabEmpty, page );
		slabEmptyPages--;
		slabPageChunk[Slab_MapIndex( page )] = 0;
	}
	Raw_Free( slabChunks[slot].raw );
	slabChunks[slot].raw = NULL;
	slabChunkCount--;
}

static void * Slab_Alloc( int size ) {
	const int cls = ( size + 15 ) / 16 - 1;
	LightLock_Lock( &slabLock );
	slabPage_t * page = slabPartial[cls];
	if ( page == NULL ) {
		if ( slabEmpty == NULL && !Slab_AddChunk() ) {
			LightLock_Unlock( &slabLock );
			return NULL;
		}
		page = slabEmpty;
		Slab_Unlink( &slabEmpty, page );
		slabEmptyPages--;
		slabChunks[slabPageChunk[Slab_MapIndex( page )] - 1].usedPages++;
		page->objSize = (uint16)( ( cls + 1 ) * 16 );
		page->used = 0;
		page->capacity = (uint16)( ( SLAB_PAGE - SLAB_HEADER ) / page->objSize );
		page->bumped = 0;
		page->freeList = NULL;
		Slab_PushFront( &slabPartial[cls], page );
	}
	void * p;
	if ( page->freeList != NULL ) {
		p = page->freeList;
		page->freeList = *(void **)p;
	} else {
		p = (byte *)page + SLAB_HEADER + page->bumped * page->objSize;
		page->bumped++;
	}
	page->used++;
	slabUsedBytes += page->objSize;
	if ( page->freeList == NULL && page->bumped == page->capacity ) {
		Slab_Unlink( &slabPartial[cls], page );		// full
	}
	LightLock_Unlock( &slabLock );
	return p;
}

static void Slab_Free( void * p ) {
	LightLock_Lock( &slabLock );
	slabPage_t * page = (slabPage_t *)( (uintptr_t)p & ~(uintptr_t)( SLAB_PAGE - 1 ) );
	const int cls = page->objSize / 16 - 1;
	assert( page->objSize != 0 && page->used > 0 );
	const bool wasFull = ( page->freeList == NULL && page->bumped == page->capacity );
	*(void **)p = page->freeList;
	page->freeList = p;
	page->used--;
	slabUsedBytes -= page->objSize;
	if ( page->used == 0 ) {
		if ( !wasFull ) {
			Slab_Unlink( &slabPartial[cls], page );
		}
		page->objSize = 0;
		Slab_PushFront( &slabEmpty, page );
		slabEmptyPages++;
		const int slot = slabPageChunk[Slab_MapIndex( page )] - 1;
		if ( --slabChunks[slot].usedPages == 0 ) {
			Slab_ReleaseChunk( slot );
		}
	} else if ( wasFull ) {
		Slab_PushFront( &slabPartial[cls], page );
	}
	LightLock_Unlock( &slabLock );
}

void Mem_SlabStats( int & chunkBytes, int & usedBytes, int & emptyBytes ) {
	chunkBytes = slabChunkCount * SLAB_CHUNK;
	usedBytes = slabUsedBytes;
	emptyBytes = slabEmptyPages * SLAB_PAGE;
}

// bigger blocks: one malloc chunk, aligned inside; just before the block: magic, malloc pointer
static const uint32 BIG_BLOCK_MAGIC = 0xB16B10C5;
static const int BIG_INFO = ( sizeof( uint32 ) + sizeof( void * ) + 7 ) & ~7;	// 8 bytes on the 3DS

static void * Big_Alloc( int size ) {
	// malloc aligns to 8, the second heap (TLSF) only to 4: room for any lead up to BIG_INFO + 15
	byte * raw = (byte *)Raw_Malloc( size + BIG_INFO + 15 );
	if ( raw == NULL ) {
		return NULL;
	}
	byte * p = (byte *)( ( (uintptr_t)raw + BIG_INFO + 15 ) & ~(uintptr_t)15 );
	*(void **)( p - sizeof( void * ) ) = raw;
	*(uint32 *)( p - sizeof( void * ) - sizeof( uint32 ) ) = BIG_BLOCK_MAGIC;
	return p;
}

static bool Big_Owns( const void * p ) {
	const byte * b = (const byte *)p;
	if ( ( (uintptr_t)b & 15 ) != 0 || *(const uint32 *)( b - sizeof( void * ) - sizeof( uint32 ) ) != BIG_BLOCK_MAGIC ) {
		return false;
	}
	const byte * raw = *(const byte * const *)( b - sizeof( void * ) );
	const uintptr_t lead = (uintptr_t)b - (uintptr_t)raw;
	return lead >= (uintptr_t)BIG_INFO && lead <= (uintptr_t)BIG_INFO + 15;
}

static void Big_Free( void * p ) {
	byte * b = (byte *)p;
	void * raw = *(void **)( b - sizeof( void * ) );
	*(uint32 *)( b - sizeof( void * ) - sizeof( uint32 ) ) = 0;	// a stale pointer freed twice is not taken for a block again
	Raw_Free( raw );
}

// any engine block: slab, temp arena, big block, or foreign malloc memory
static void Block_Free( void * p ) {
	if ( Slab_Owns( p ) ) {
		Slab_Free( p );
	} else if ( Arena_Owns( p ) ) {
		Arena_Free( p );
	} else if ( Big_Owns( p ) ) {
		Big_Free( p );
	} else {
		free( p );
	}
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
		// volatile: GCC otherwise folds a malloc/free pair into "success"
		void * volatile p = malloc( mid );
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
			LightLock_Init( &slabLock );
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
		raw = (byte *)( ( rawSize <= SLAB_MAX ) ? Slab_Alloc( rawSize ) : NULL );
	}
	if ( raw == NULL ) {
		raw = (byte *)Big_Alloc( rawSize );		// also when no new slab chunk could be had
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
	// diagnostic: sdmc:/doom3/memzero makes every allocation start zeroed (to test
	// for reads of uninitialised memory that happen to be zero on the PC)
	static int memZeroState = -1;
	if ( memZeroState < 0 ) {
		FILE * f = fopen( "sdmc:/doom3/memzero", "rb" );
		memZeroState = ( f != NULL ) ? 1 : 0;
		if ( f != NULL ) {
			fclose( f );
		}
	}
	if ( memZeroState == 1 ) {
		memset( raw, 0, rawSize );
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
		Block_Free( ptr );
		return;
	}
	memHeader_t * hdr = (memHeader_t *)( (byte *)ptr - sizeof( memHeader_t ) );
	const bool ours = Slab_Owns( hdr ) || Arena_Owns( hdr ) || Big_Owns( hdr );
	if ( !ours || hdr->magic != MEM_HEADER_MAGIC ) {
		// not ours: plain malloc block
		free( ptr );
		return;
	}
	hdr->magic = 0;
	Mem_Account( hdr->tag, -(int)hdr->size, -1 );
	Block_Free( hdr );
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

