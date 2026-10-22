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
#ifndef __VERTEXCACHE2_H__
#define __VERTEXCACHE2_H__

#if defined( ID_3DS )
// New 3DS has ~178 MB for the whole application; PC sizes would need ~186 MB here alone.
// First-pass budgets, to be tuned from measurements (r_showVertexCache / map load log).
// index: 1 MB overflowed in mars_city1 only while interactions were built per frame
// (n3ds_lazyInteractions 1); with static interactions the busiest delta4 fights peak at
// 255 KB (PERF "frame cache peak idx"), and an overflow only drops a surface for a frame
// The GUI model takes a fixed block of each (idGuiModel::MAX_VERTS / MAX_INDEXES: 1 MB of
// vertexes, 96 KB of indexes); the rest of a frame's dynamic geometry was under 300 KB.
const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 512 * 1024;
const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 2 * 1024 * 1024;
#else
const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
#endif
const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 256 * 1024;

#if defined( ID_3DS )
// Three sets, not two. The frontend of frame N+2 (game thread) runs while the backend of
// N+1 is being built and the GPU may still be drawing frame N: with two sets it overwrote
// the vertexes and indexes frame N was drawn from (the PC waits for the GPU in
// GL_BlockingSwapBuffers first; the null GL does not). In the Mars City intro this hit the
// GUI blocks at the start of the set: garbage on the terminal screens, then a GPU that
// never finished the frame. With three, set (N+2) % 3 was last used by frame N-1, which
// the backend of N waited for (C3D_FrameBegin) before it started.
const int VERTCACHE_NUM_FRAMES = 3;
#else
const int VERTCACHE_NUM_FRAMES = 2;
#endif

// there are a lot more static indexes than vertexes, because interactions are just new
// index lists that reference existing vertexes
#if defined( ID_3DS )
// mars_city1's whole .bproc is 8.6 MB; map load prints how much of these was used
// Set at renderer init (n3ds_staticVertexKB / n3ds_staticIndexKB, or by renderer):
// null GL with n3ds_discardStaticGeo: budgets only (10 / 6 MB, mars_city1 used 7.9 / 2.1);
// citro3d: real linear memory (8.5 / 3.5 MB; delta4 uses 3.7 / 0.9, mars_city1 needs more than
// 6 MB of vertexes, and static interactions put their index lists here too).
extern int n3dsStaticIndexMemory;
extern int n3dsStaticVertexMemory;
// bytes that did not fit since the last static cache reset: a map that overflows still
// loads to the end, then N3DS_CheckStaticCache stops it with the size it really needs
extern int n3dsStaticVertexOverflow;
extern int n3dsStaticIndexOverflow;
#define STATIC_INDEX_MEMORY		n3dsStaticIndexMemory
#define STATIC_VERTEX_MEMORY	n3dsStaticVertexMemory
#else
const int STATIC_INDEX_MEMORY = 31 * 1024 * 1024;
const int STATIC_VERTEX_MEMORY = 31 * 1024 * 1024;	// make sure it fits in VERTCACHE_OFFSET_MASK!
#endif

// vertCacheHandle_t packs size, offset, and frame number into 64 bits
typedef uint64 vertCacheHandle_t;
const int VERTCACHE_STATIC = 1;					// in the static set, not the per-frame set
const int VERTCACHE_SIZE_SHIFT = 1;
const int VERTCACHE_SIZE_MASK = 0x7fffff;		// 8 megs 
const int VERTCACHE_OFFSET_SHIFT = 24;
const int VERTCACHE_OFFSET_MASK = 0x1ffffff;	// 32 megs 
const int VERTCACHE_FRAME_SHIFT = 49;
const int VERTCACHE_FRAME_MASK = 0x7fff;		// 15 bits = 32k frames to wrap around

const int VERTEX_CACHE_ALIGN		= 32;
const int INDEX_CACHE_ALIGN			= 16;
const int JOINT_CACHE_ALIGN			= 16;

enum cacheType_t {
	CACHE_VERTEX,
	CACHE_INDEX,
	CACHE_JOINT
};

struct geoBufferSet_t {
	idIndexBuffer			indexBuffer;
	idVertexBuffer			vertexBuffer;
	idJointBuffer			jointBuffer;
	byte *					mappedVertexBase;
	byte *					mappedIndexBase;
	byte *					mappedJointBase;
	idSysInterlockedInteger	indexMemUsed;
	idSysInterlockedInteger	vertexMemUsed;
	idSysInterlockedInteger	jointMemUsed;
	int						allocations;	// number of index and vertex allocations combined
};

class idVertexCache {
public:
	void			Init( bool restart = false );
	void			Shutdown();
	void			PurgeAll();

	// call on loading a new map
	void			FreeStaticData();

	// this data is only valid for one frame of rendering
	vertCacheHandle_t	AllocVertex( const void * data, int bytes ) {
		return ActuallyAlloc( frameData[listNum], data, bytes, CACHE_VERTEX );
	}
	vertCacheHandle_t	AllocIndex( const void * data, int bytes ) {
		return ActuallyAlloc( frameData[listNum], data, bytes, CACHE_INDEX );
	}
	vertCacheHandle_t	AllocJoint( const void * data, int bytes ) {
		return ActuallyAlloc( frameData[listNum], data, bytes, CACHE_JOINT );
	}

	// this data is valid until the next map load
	vertCacheHandle_t	AllocStaticVertex( const void * data, int bytes ) {
		if ( staticData.vertexMemUsed.GetValue() + bytes > STATIC_VERTEX_MEMORY ) {
#if defined( ID_3DS )
			n3dsStaticVertexOverflow += bytes;	// no handle: the surface would use frame memory
			return (vertCacheHandle_t)0;
#endif
			idLib::FatalError( "AllocStaticVertex failed, increase STATIC_VERTEX_MEMORY" );
		}
		return ActuallyAlloc( staticData, data, bytes, CACHE_VERTEX );
	}
	vertCacheHandle_t	AllocStaticIndex( const void * data, int bytes ) {
		if ( staticData.indexMemUsed.GetValue() + bytes > STATIC_INDEX_MEMORY ) {
#if defined( ID_3DS )
			n3dsStaticIndexOverflow += bytes;
			return (vertCacheHandle_t)0;
#endif
			idLib::FatalError( "AllocStaticIndex failed, increase STATIC_INDEX_MEMORY" );
		}
		return ActuallyAlloc( staticData, data, bytes, CACHE_INDEX );
	}

	byte *			MappedVertexBuffer( vertCacheHandle_t handle ) {
		release_assert( !CacheIsStatic( handle ) );
		const uint64 offset = (int)( handle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
		const uint64 frameNum = (int)( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		release_assert( frameNum == ( currentFrame & VERTCACHE_FRAME_MASK ) );
		return frameData[ listNum ].mappedVertexBase + offset;
	}

	byte *			MappedIndexBuffer( vertCacheHandle_t handle ) {
		release_assert( !CacheIsStatic( handle ) );
		const uint64 offset = (int)( handle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
		const uint64 frameNum = (int)( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		release_assert( frameNum == ( currentFrame & VERTCACHE_FRAME_MASK ) );
		return frameData[ listNum ].mappedIndexBase + offset;
	}

	// Returns false if it's been purged
	// This can only be called by the front end, the back end should only be looking at
	// vertCacheHandle_t that are already validated.
	bool			CacheIsCurrent( const vertCacheHandle_t handle ) {
		const int isStatic = handle & VERTCACHE_STATIC;
		if ( isStatic ) {
			return true;
		}
		const uint64 frameNum = (int)( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if ( frameNum != ( currentFrame & VERTCACHE_FRAME_MASK ) ) {
			return false;
		}
		return true;
	}

	static bool		CacheIsStatic( const vertCacheHandle_t handle ) {
		return ( handle & VERTCACHE_STATIC ) != 0;
	}

	// vb/ib is a temporary reference -- don't store it
	bool			GetVertexBuffer( vertCacheHandle_t handle, idVertexBuffer * vb );
	bool			GetIndexBuffer( vertCacheHandle_t handle, idIndexBuffer * ib );
	bool			GetJointBuffer( vertCacheHandle_t handle, idJointBuffer * jb );

	void			BeginBackEnd();

public:
	int				currentFrame;	// for determining the active buffers
	int				listNum;		// currentFrame % VERTCACHE_NUM_FRAMES
	int				drawListNum;	// (currentFrame-1) % VERTCACHE_NUM_FRAMES

	geoBufferSet_t	staticData;
	geoBufferSet_t	frameData[VERTCACHE_NUM_FRAMES];

	// High water marks for the per-frame buffers
	int				mostUsedVertex;
	int				mostUsedIndex;
	int				mostUsedJoint;

	// Try to make room for <bytes> bytes
	vertCacheHandle_t	ActuallyAlloc( geoBufferSet_t & vcs, const void * data, int bytes, cacheType_t type );
};

// platform specific code to memcpy into vertex buffers efficiently
// 16 byte alignment is guaranteed
void CopyBuffer( byte * dst, const byte * src, int numBytes );

extern	idVertexCache	vertexCache;

#endif // __VERTEXCACHE2_H__
