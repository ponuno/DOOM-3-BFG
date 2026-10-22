/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

===========================================================================
*/

/*
================================================================================================

	New 3DS textures for the citro3d backend.

	At image load time (idImage::ActuallyLoadImage) one small level of every colour
	image (diffuse, default, coverage) is chosen, e.g. 128x128 DXT5 = 16 KB. All other
	pixel data is skipped.

	Streaming (n3ds_texStream 1): for images read from a file, only where that level
	lives on disk is remembered (container OS path + offset). The first time the image
	is drawn, a loader thread reads the level with its own unbuffered FILE (the engine's
	container handles are not thread safe) while the surface is drawn untextured for a
	few frames. After the upload the data stays in a small RAM LRU cache
	(n3ds_texRAMCacheKB), so a texture evicted from VRAM usually comes back without a
	read. Images without a file behind them (generated, tiny memory-loaded files)
	keep their level in RAM as before. mars_city1 kept 13.7 MB of levels this way, of
	which a room draws a few hundred KB.

	When a surface using the image is drawn, that level is decoded, given a mip
	chain, tiled and copied to VRAM (a few per frame). VRAM is managed as an LRU
	cache: textures not used in the current frame are evicted when space runs out,
	and come back from the kept level when they are seen again.

	Texture pack (_n3ds_textures.pak, made on the PC by tools/n3ds/n3dstex): an image the
	pack holds is not read from its .bimage at all. Its GPU data - ETC1 / ETC1A4 world
	textures, HILO8 normal maps, A4 / A8 masks and fonts, 16-bit GUI art, whole mip
	chains already tiled - is streamed from the pack like the kept levels above and
	copied to VRAM as it is: no decoding, no resampling, a quarter of the VRAM of RGB565
	for world textures. The n3ds_tex*MaxSize cvars cut the top levels off the chain.
	Images the pack lacks take the old path.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../tr_local.h"
#include "n3ds_gpu.h"
#include "n3ds_texconv.h"
#include "n3ds_textures.h"
#include "n3ds_pack.h"

idCVar n3ds_textures( "n3ds_textures", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: draw textures (0 = flat colours)" );
idCVar n3ds_texMaxSize( "n3ds_texMaxSize", "128", CVAR_RENDERER | CVAR_INTEGER, "citro3d: largest texture size; the level kept at load is the largest one not over this", 8, 512 );
idCVar n3ds_texFontA8( "n3ds_texFontA8", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT, "citro3d: fonts as alpha-only A8 textures (0 = colour textures, for bisecting)" );
idCVar n3ds_texFontMaxSize( "n3ds_texFontMaxSize", "1024", CVAR_RENDERER | CVAR_INTEGER, "citro3d: largest font atlas size (fonts are alpha-only A8 textures)", 8, 1024 );
idCVar n3ds_texGuiMaxSize( "n3ds_texGuiMaxSize", "512", CVAR_RENDERER | CVAR_INTEGER, "citro3d: largest texture size for GUI / HUD / font images (guis/, generated/swf/, newfonts/, fonts/)", 8, 1024 );
idCVar n3ds_texMips( "n3ds_texMips", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: build mip maps" );
idCVar n3ds_texUploadsPerFrame( "n3ds_texUploadsPerFrame", "4", CVAR_RENDERER | CVAR_INTEGER, "citro3d: textures decoded and copied to VRAM per frame at most", 1, 64 );
idCVar n3ds_texUploadsAfterLoad( "n3ds_texUploadsAfterLoad", "256", CVAR_RENDERER | CVAR_INTEGER, "citro3d: texture upload limit for the first 20 3D frames after a map load (the GX queue caps it at 24)" );
idCVar n3ds_texFlip( "n3ds_texFlip", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: store texture rows bottom-up (the PICA samples t = 0 at the last row); toggle if textures are upside down" );
idCVar n3ds_texStream( "n3ds_texStream", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: read texture levels from disk when first drawn instead of keeping them all in RAM (set at startup; applies to images loaded afterwards)" );
idCVar n3ds_texRAMCacheKB( "n3ds_texRAMCacheKB", "1024", CVAR_RENDERER | CVAR_INTEGER, "citro3d: RAM kept for streamed texture levels already uploaded, so that textures evicted from VRAM come back without a read" );
idCVar n3ds_texLinearKB( "n3ds_texLinearKB", "3072", CVAR_RENDERER | CVAR_INTEGER, "citro3d: linear memory (FCRAM) for textures that do not fit in VRAM; the GPU samples them from there" );
idCVar n3ds_texLinearReserveKB( "n3ds_texLinearReserveKB", "2048", CVAR_RENDERER | CVAR_INTEGER, "citro3d: linear memory the texture cache always leaves free (citro3d, staging copies)" );
idCVar n3ds_texVRAMReserveKB( "n3ds_texVRAMReserveKB", "64", CVAR_RENDERER | CVAR_INTEGER, "citro3d: VRAM left free by the texture cache" );
idCVar n3ds_texPack( "n3ds_texPack", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT, "citro3d: take textures from base/_n3ds_textures.pak (converted on the PC by n3dstex) when it is there" );
idCVar n3ds_texWorldMaxSize( "n3ds_texWorldMaxSize", "256", CVAR_RENDERER | CVAR_INTEGER, "citro3d texture pack: largest world texture (larger pack levels are skipped)", 8, 1024 );
idCVar n3ds_texBumpMaxSize( "n3ds_texBumpMaxSize", "128", CVAR_RENDERER | CVAR_INTEGER, "citro3d texture pack: largest normal map", 8, 1024 );
idCVar n3ds_texSpecMaxSize( "n3ds_texSpecMaxSize", "128", CVAR_RENDERER | CVAR_INTEGER, "citro3d texture pack: largest specular map", 8, 1024 );

struct n3dsTexture_t {
	idImage *			image;
	// the kept level, as stored
	int					format;
	int					colorFormat;
	int					width;
	int					height;
	int					dataSize;
	byte *				data;
	int					wrap;			// N3DS_WRAP_*
	bool				alphaOnly;		// coverage images: only the alpha is used
	int					maxSize;		// largest side on the GPU
	bool				unsupported;	// the level cannot be decoded
	// from the texture pack: data is the GPU chain itself (width / height / packMaxLevel,
	// gpuFormat N3DS_TEX_*), copied as it is
	bool				packed;
	int					gpuFormat;
	int					packMaxLevel;
	bool				packHasAlpha;
	int					usage;			// textureUsage_t (packed images)
	// on the GPU
	void *				gpu;
	int					gpuBytes;
	bool				inLinear;		// gpu lives in linear memory (VRAM was full)
	bool				gpuHasAlpha;
	int					lastFrame;
	int					retryFrame;		// after a failed upload: not before this frame
	bool				everUploaded;	// diagnostics: drawn at least once since load
	// streaming: the level is read from streamPath at streamOffset (streamPath < 0: data is
	// owned for the image's lifetime). loadState and handing over data are under texLoadLock.
	int					streamPath;
	int					streamOffset;
	int					loadState;		// TEX_*
	bool				orphan;			// purged while being read: the loader deletes it
	n3dsTexture_t *		ramPrev;		// RAM cache of streamed levels (main thread only)
	n3dsTexture_t *		ramNext;
	bool				inRamCache;
	n3dsTexture_t *		lruPrev;
	n3dsTexture_t *		lruNext;
};

static n3dsTexture_t *	lruHead = NULL;			// most recently used
static n3dsTexture_t *	lruTail = NULL;
static int				texFrame = 1;
static int				uploadsThisFrame = 0;
static int				uploadLimit = 4;
static bool				lastFlip = true;
static int				lastMaxSize = 128;
static bool				lastMips = true;
static int				boostFrames = 0;		// 3D frames with a raised upload limit after a level load

// stats (reset by N3DS_TexStatsLine)
static int				statUploads = 0;
static int				statEvictions = 0;
static int				statDeferred = 0;
static int				statFailed = 0;
static uint64			statUploadUs = 0;
static int				keptCount = 0;
static int				keptBytes = 0;
static int				residentCount = 0;
static int				linearTexBytes = 0;		// of residentBytes, in linear memory
static int				statLinearUploads = 0;
static int				residentBytes = 0;

static void LRU_Unlink( n3dsTexture_t * t ) {
	if ( t->lruPrev != NULL ) {
		t->lruPrev->lruNext = t->lruNext;
	} else if ( lruHead == t ) {
		lruHead = t->lruNext;
	}
	if ( t->lruNext != NULL ) {
		t->lruNext->lruPrev = t->lruPrev;
	} else if ( lruTail == t ) {
		lruTail = t->lruPrev;
	}
	t->lruPrev = t->lruNext = NULL;
}

static void LRU_PushFront( n3dsTexture_t * t ) {
	t->lruPrev = NULL;
	t->lruNext = lruHead;
	if ( lruHead != NULL ) {
		lruHead->lruPrev = t;
	}
	lruHead = t;
	if ( lruTail == NULL ) {
		lruTail = t;
	}
}


/*
================================================================================================

	Streaming

================================================================================================
*/
enum {
	TEX_IDLE,		// nothing pending; data is either owned (RAM cache / pinned) or NULL
	TEX_QUEUED,
	TEX_LOADING,	// the loader thread is reading it
	TEX_READY,		// read: data belongs to the main thread from now on
	TEX_FAILED
};

static const int TEX_QUEUE_SIZE = 512;
static const int TEX_MAX_PATHS = 32;

struct texStreamPath_t {
	idStrStatic< MAX_OSPATH >	path;
	int							refs;		// textures reading from it (under texLoadLock)
	FILE *						file;		// loader thread's own handle
};

static idSysMutex			texLoadLock;
static n3dsTexture_t *		texQueue[TEX_QUEUE_SIZE];
static int					texQueueHead = 0;		// next to read
static int					texQueueCount = 0;
static texStreamPath_t		texPaths[TEX_MAX_PATHS];
static int					streamedCount = 0;		// levels left on disk
static int					streamedBytes = 0;
static int					statReads = 0;			// under texLoadLock
static int					statReadBytes = 0;
static uint64				statReadUs = 0;
static int					statReadFailures = 0;
static int					statStreamWaits = 0;	// main thread
static n3dsTexture_t *		ramHead = NULL;			// RAM cache, most recently uploaded first
static n3dsTexture_t *		ramTail = NULL;
static int					ramBytes = 0;

// called with texLoadLock held
static void N3DS_TexDelete( n3dsTexture_t * t ) {
	if ( t->streamPath >= 0 ) {
		texPaths[t->streamPath].refs--;
		streamedCount--;
		streamedBytes -= t->dataSize;
	} else {
		keptCount--;
		keptBytes -= t->dataSize;
	}
	delete t;
}

// called with texLoadLock held; -1 when the table is full
static int N3DS_TexPathIndex( const char * osPath ) {
	int freeSlot = -1;
	for ( int i = 0; i < TEX_MAX_PATHS; i++ ) {
		texStreamPath_t & p = texPaths[i];
		if ( ( p.refs > 0 || p.file != NULL ) && p.path.Icmp( osPath ) == 0 ) {
			return i;
		}
		if ( freeSlot < 0 && p.refs == 0 && p.file == NULL ) {
			freeSlot = i;
		}
	}
	if ( freeSlot >= 0 ) {
		texPaths[freeSlot].path = osPath;
	}
	return freeSlot;
}

class idN3DSTexLoader : public idSysThread {
public:
	virtual int Run() {
		for ( ;; ) {
			texLoadLock.Lock();
			n3dsTexture_t * t = NULL;
			while ( texQueueCount > 0 && t == NULL ) {
				t = texQueue[texQueueHead];
				texQueue[texQueueHead] = NULL;
				texQueueHead = ( texQueueHead + 1 ) % TEX_QUEUE_SIZE;
				texQueueCount--;
			}
			if ( t == NULL ) {
				// idle: give back the handles of containers no texture reads from any more
				for ( int i = 0; i < TEX_MAX_PATHS; i++ ) {
					if ( texPaths[i].refs == 0 && texPaths[i].file != NULL ) {
						fclose( texPaths[i].file );
						texPaths[i].file = NULL;
					}
				}
				texLoadLock.Unlock();
				return 0;
			}
			t->loadState = TEX_LOADING;
			const int pathIndex = t->streamPath;
			const int offset = t->streamOffset;
			const int size = t->dataSize;
			FILE * f = texPaths[pathIndex].file;
			idStrStatic< MAX_OSPATH > path = texPaths[pathIndex].path;
			texLoadLock.Unlock();

			const uint64 start = Sys_Microseconds();
			if ( f == NULL ) {
				f = fopen( path.c_str(), "rb" );
				if ( f != NULL ) {
					setvbuf( f, NULL, _IONBF, 0 );		// exactly the bytes asked for, no 64 KB refills
					texLoadLock.Lock();
					texPaths[pathIndex].file = f;
					texLoadLock.Unlock();
				}
			}
			byte * buf = ( f != NULL ) ? (byte *)Mem_Alloc( size, TAG_IMAGE ) : NULL;
			const bool ok = buf != NULL && fseek( f, offset, SEEK_SET ) == 0 && (int)fread( buf, 1, size, f ) == size;
			const uint64 us = Sys_Microseconds() - start;

			texLoadLock.Lock();
			if ( t->orphan ) {
				Mem_Free( buf );
				N3DS_TexDelete( t );
			} else if ( ok ) {
				t->data = buf;
				t->loadState = TEX_READY;
				statReads++;
				statReadBytes += size;
				statReadUs += us;
			} else {
				Mem_Free( buf );
				t->loadState = TEX_FAILED;
				statReadFailures++;
			}
			texLoadLock.Unlock();
		}
	}
};

static idN3DSTexLoader *	texLoader = NULL;

static void N3DS_TexWakeLoader() {
	if ( texLoader == NULL ) {
		texLoader = new (TAG_IMAGE) idN3DSTexLoader;
		// core 0 above the main thread: it mostly waits for the SD card and must
		// start the next read as soon as one completes
		texLoader->StartWorkerThread( "N3DS_TexLoader", CORE_0A, THREAD_ABOVE_NORMAL, 32 * 1024 );
	}
	// safe while it runs too: the worker then runs Run() once more, so no request is
	// left behind between its last look at the queue and going idle
	texLoader->SignalWork();
}

// main thread; false when the queue is full (asked again next frame)
static bool N3DS_TexRequest( n3dsTexture_t * t ) {
	texLoadLock.Lock();
	bool queued = false;
	const bool wasEmpty = ( texQueueCount == 0 );
	if ( t->loadState == TEX_IDLE && texQueueCount < TEX_QUEUE_SIZE ) {
		texQueue[( texQueueHead + texQueueCount ) % TEX_QUEUE_SIZE] = t;
		texQueueCount++;
		t->loadState = TEX_QUEUED;
		queued = true;
	}
	texLoadLock.Unlock();
	// a non-empty queue has a wake-up pending already (the loader has not popped it yet)
	if ( queued && wasEmpty ) {
		N3DS_TexWakeLoader();
	}
	return queued;
}

static void RAM_Unlink( n3dsTexture_t * t ) {
	if ( !t->inRamCache ) {
		return;
	}
	if ( t->ramPrev != NULL ) {
		t->ramPrev->ramNext = t->ramNext;
	} else {
		ramHead = t->ramNext;
	}
	if ( t->ramNext != NULL ) {
		t->ramNext->ramPrev = t->ramPrev;
	} else {
		ramTail = t->ramPrev;
	}
	t->ramPrev = t->ramNext = NULL;
	t->inRamCache = false;
	ramBytes -= t->dataSize;
}

// a streamed level was just uploaded: keep its data while the RAM cache has room
static void N3DS_TexRamKeep( n3dsTexture_t * t ) {
	RAM_Unlink( t );
	const int budget = Max( 0, n3ds_texRAMCacheKB.GetInteger() ) * 1024;
	while ( ramTail != NULL && ramBytes + t->dataSize > budget ) {
		n3dsTexture_t * old = ramTail;
		RAM_Unlink( old );
		Mem_Free( old->data );
		old->data = NULL;
	}
	if ( ramBytes + t->dataSize > budget ) {
		Mem_Free( t->data );
		t->data = NULL;
		return;
	}
	t->ramPrev = NULL;
	t->ramNext = ramHead;
	if ( ramHead != NULL ) {
		ramHead->ramPrev = t;
	} else {
		ramTail = t;
	}
	ramHead = t;
	t->inRamCache = true;
	ramBytes += t->dataSize;
}

bool RB_N3DS_TexStreaming() {
	return r_n3dsC3D && n3ds_texStream.GetBool();
}

static void N3DS_TexDropGPU( n3dsTexture_t * t ) {
	if ( t->gpu == NULL ) {
		return;
	}
	LRU_Unlink( t );
	N3DSGPU_TexFree( t->gpu );
	t->gpu = NULL;
	residentCount--;
	residentBytes -= t->gpuBytes;
	if ( t->inLinear ) {
		linearTexBytes -= t->gpuBytes;
		t->inLinear = false;
	}
	t->gpuBytes = 0;
}

/*
========================
RB_N3DS_TexKeepSize

Largest level to keep for this image at load time, 0 = keep nothing.
========================
*/
static bool N3DS_IsGuiImage( const char * name ) {
	return name != NULL && ( idStr::Icmpn( name, "guis/", 5 ) == 0 || idStr::Icmpn( name, "generated/swf/", 14 ) == 0 ||
		idStr::Icmpn( name, "newfonts/", 9 ) == 0 || idStr::Icmpn( name, "fonts/", 6 ) == 0 );
}

int RB_N3DS_TexKeepSize( textureUsage_t usage, const char * name ) {
	if ( !r_n3dsC3D ) {
		return 0;
	}
	switch ( usage ) {
		case TD_DIFFUSE:
		case TD_DEFAULT:
		case TD_COVERAGE:
		case TD_LIGHT:
		case TD_LOOKUP_TABLE_MONO:
		case TD_LOOKUP_TABLE_RGB1:
		case TD_LOOKUP_TABLE_RGBA:
		case TD_FONT:
			break;
		default:
			return 0;
	}
	// a 1024x1280 font atlas keeps 512x640 (24-pixel glyphs, A8 512x512 on the GPU):
	// glyphs are drawn 8-14 pixels tall, so the mip chain below has something to filter
	if ( usage == TD_FONT ) {
		return n3ds_texFontMaxSize.GetInteger();
	}
	// GUIs are drawn at about screen size: their atlases need far more than world textures
	if ( N3DS_IsGuiImage( name ) ) {
		return n3ds_texGuiMaxSize.GetInteger();
	}
	return n3ds_texMaxSize.GetInteger();
}

/*
========================
RB_N3DS_ImageLevel

Called at the end of idImage::ActuallyLoadImage with the kept level.
========================
*/
void RB_N3DS_ImageLevel( idImage * image, int format, int colorFormat, int width, int height,
						 const byte * data, int dataSize, int wrap, bool alphaOnly, int maxSize ) {
	RB_N3DS_ImagePurged( image );
	if ( data == NULL || dataSize <= 0 ) {
		return;
	}
	n3dsTexture_t * t = new (TAG_IMAGE) n3dsTexture_t;
	memset( t, 0, sizeof( *t ) );
	t->image = image;
	t->format = format;
	t->colorFormat = colorFormat;
	t->width = width;
	t->height = height;
	t->dataSize = dataSize;
	t->data = (byte *)Mem_Alloc( dataSize, TAG_IMAGE );
	memcpy( t->data, data, dataSize );
	t->wrap = wrap;
	t->alphaOnly = alphaOnly;
	t->maxSize = maxSize;
	t->streamPath = -1;
	image->n3dsTex = t;
	keptCount++;
	keptBytes += dataSize;
}

/*
========================
RB_N3DS_ImageLevelStreamed

Like RB_N3DS_ImageLevel, but the level stays on disk until the image is drawn.
========================
*/
void RB_N3DS_ImageLevelStreamed( idImage * image, int format, int colorFormat, int width, int height,
								 int dataSize, const char * osPath, int osOffset, int wrap, bool alphaOnly, int maxSize ) {
	RB_N3DS_ImagePurged( image );
	if ( dataSize <= 0 || osPath == NULL || osPath[0] == '\0' ) {
		return;
	}
	texLoadLock.Lock();
	const int pathIndex = N3DS_TexPathIndex( osPath );
	if ( pathIndex >= 0 ) {
		texPaths[pathIndex].refs++;
		streamedCount++;
		streamedBytes += dataSize;
	}
	texLoadLock.Unlock();
	if ( pathIndex < 0 ) {
		common->Warning( "citro3d: more than %d texture source files, %s is not drawn", TEX_MAX_PATHS, image->GetName() );
		return;
	}
	n3dsTexture_t * t = new (TAG_IMAGE) n3dsTexture_t;
	memset( t, 0, sizeof( *t ) );
	t->image = image;
	t->format = format;
	t->colorFormat = colorFormat;
	t->width = width;
	t->height = height;
	t->dataSize = dataSize;
	t->data = NULL;
	t->wrap = wrap;
	t->alphaOnly = alphaOnly;
	t->maxSize = maxSize;
	t->streamPath = pathIndex;
	t->streamOffset = osOffset;
	t->loadState = TEX_IDLE;
	image->n3dsTex = t;
}

/*
================================================================================================

	Texture pack

================================================================================================
*/
static bool					packTried = false;
static n3dsPackEntry_t *	packEntries = NULL;
static int					packNum = 0;
static idStrStatic< MAX_OSPATH >	packOSPath;
static int					statPackedUploads = 0;
static int					packedCount = 0;		// textures from the pack (loaded images)

static void N3DS_PackInit() {
	packTried = true;
	if ( !r_n3dsC3D || !n3ds_texPack.GetBool() ) {
		return;
	}
	const char * osPath = fileSystem->RelativePathToOSPath( "_n3ds_textures.pak", "fs_basepath" );
	FILE * f = fopen( osPath, "rb" );
	if ( f == NULL ) {
		common->Printf( "citro3d: no texture pack (%s): textures are converted on the console\n", osPath );
		return;
	}
	n3dsPackHeader_t header;
	bool ok = fread( &header, sizeof( header ), 1, f ) == 1 && header.magic == N3DS_PACK_MAGIC && header.version == N3DS_PACK_VERSION;
	long fileLength = 0;
	if ( ok && fseek( f, 0, SEEK_END ) == 0 ) {
		fileLength = ftell( f );
	}
	ok = ok && header.numEntries > 0 && header.numEntries < 1000000 &&
		(uint64)header.entryOffset + (uint64)header.numEntries * sizeof( n3dsPackEntry_t ) <= (uint64)fileLength;
	if ( ok ) {
		packEntries = (n3dsPackEntry_t *)Mem_Alloc( header.numEntries * sizeof( n3dsPackEntry_t ), TAG_IMAGE );
		ok = fseek( f, header.entryOffset, SEEK_SET ) == 0 &&
			fread( packEntries, sizeof( n3dsPackEntry_t ), header.numEntries, f ) == header.numEntries;
		if ( !ok ) {
			Mem_Free( packEntries );
			packEntries = NULL;
		}
	}
	fclose( f );
	if ( !ok ) {
		common->Warning( "citro3d: %s is not a texture pack of version %d or is damaged (convert again with n3dstex)", osPath, N3DS_PACK_VERSION );
		return;
	}
	packNum = header.numEntries;
	packOSPath = osPath;
	common->Printf( "citro3d: texture pack %s: %d textures, %d MB (converted with world %d, bump %d, specular %d, gui %d)\n",
		osPath, packNum, (int)( fileLength >> 20 ), header.maxWorld, header.maxBump, header.maxSpecular, header.maxGui );
}

const n3dsPackEntry_t * RB_N3DS_PackFind( const char * generatedFileName ) {
	if ( !packTried ) {
		N3DS_PackInit();
	}
	if ( packEntries == NULL || generatedFileName == NULL ) {
		return NULL;
	}
	const uint64 h = N3DS_PackHash( generatedFileName );
	int lo = 0, hi = packNum - 1;
	while ( lo <= hi ) {
		const int mid = ( lo + hi ) >> 1;
		const uint64 m = packEntries[mid].nameHash;
		if ( m == h ) {
			return &packEntries[mid];
		}
		if ( m < h ) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return NULL;
}

static int N3DS_PackFormat( int packFormat ) {
	switch ( packFormat ) {
		case N3DS_PACKFMT_RGB565:	return N3DS_TEX_RGB565;
		case N3DS_PACKFMT_RGBA5551:	return N3DS_TEX_RGBA5551;
		case N3DS_PACKFMT_RGBA4:	return N3DS_TEX_RGBA4;
		case N3DS_PACKFMT_A8:		return N3DS_TEX_A8;
		case N3DS_PACKFMT_ETC1:		return N3DS_TEX_ETC1;
		case N3DS_PACKFMT_ETC1A4:	return N3DS_TEX_ETC1A4;
		case N3DS_PACKFMT_HILO8:	return N3DS_TEX_HILO8;
		case N3DS_PACKFMT_A4:		return N3DS_TEX_A4;
		default:					return -1;
	}
}

/*
========================
RB_N3DS_ImagePacked

The image is in the texture pack: its chain is read from there when it is first drawn.
========================
*/
void RB_N3DS_ImagePacked( idImage * image, const n3dsPackEntry_t * e, int wrap ) {
	RB_N3DS_ImagePurged( image );
	const int gpuFormat = N3DS_PackFormat( e->gpuFormat );
	if ( gpuFormat < 0 || e->width < 8 || e->height < 8 ) {
		return;
	}
	int cap = n3ds_texWorldMaxSize.GetInteger();
	if ( e->usage == TD_BUMP ) {
		cap = n3ds_texBumpMaxSize.GetInteger();
	} else if ( e->usage == TD_SPECULAR ) {
		cap = n3ds_texSpecMaxSize.GetInteger();
	} else if ( e->usage == TD_FONT ) {
		cap = n3ds_texFontMaxSize.GetInteger();
	} else if ( N3DS_IsGuiImage( image->GetName() ) || ( e->usage >= TD_LOOKUP_TABLE_MONO && e->usage <= TD_LOOKUP_TABLE_RGBA ) ) {
		cap = n3ds_texGuiMaxSize.GetInteger();
	}
	// the chain from the first level that fits (each level is half the one before)
	int level = 0;
	while ( level < e->maxLevel && ( ( e->width >> level ) > cap || ( e->height >> level ) > cap ) ) {
		level++;
	}
	const uint32 skip = N3DS_PackLevelOffset( e->gpuFormat, e->width, e->height, level );
	const uint64 offset = (uint64)e->dataOffset * N3DS_PACK_ALIGN + skip;
	if ( skip >= e->dataSize || offset > 0x7FFFFFFF ) {
		return;
	}
	texLoadLock.Lock();
	const int pathIndex = N3DS_TexPathIndex( packOSPath.c_str() );
	if ( pathIndex >= 0 ) {
		texPaths[pathIndex].refs++;
		streamedCount++;
		streamedBytes += e->dataSize - skip;
	}
	texLoadLock.Unlock();
	if ( pathIndex < 0 ) {
		common->Warning( "citro3d: more than %d texture source files, %s is not drawn", TEX_MAX_PATHS, image->GetName() );
		return;
	}
	n3dsTexture_t * t = new (TAG_IMAGE) n3dsTexture_t;
	memset( t, 0, sizeof( *t ) );
	t->image = image;
	t->format = e->srcFormat;
	t->colorFormat = e->srcColorFormat;
	t->width = e->width >> level;
	t->height = e->height >> level;
	t->dataSize = e->dataSize - skip;
	t->data = NULL;
	t->wrap = wrap;
	t->alphaOnly = ( e->flags & N3DS_PACKFLAG_ALPHAONLY ) != 0;
	t->maxSize = cap;
	t->packed = true;
	t->gpuFormat = gpuFormat;
	t->packMaxLevel = e->maxLevel - level;
	t->packHasAlpha = ( e->flags & N3DS_PACKFLAG_ALPHA ) != 0;
	t->usage = e->usage;
	t->streamPath = pathIndex;
	t->streamOffset = (int)offset;
	t->loadState = TEX_IDLE;
	image->n3dsTex = t;
	packedCount++;
}

/*
========================
RB_N3DS_ImagePurged
========================
*/
void RB_N3DS_ImagePurged( idImage * image ) {
	n3dsTexture_t * t = (n3dsTexture_t *)image->n3dsTex;
	if ( t == NULL ) {
		return;
	}
	N3DS_TexDropGPU( t );
	image->n3dsTex = NULL;
	RAM_Unlink( t );
	if ( t->packed ) {
		packedCount--;
	}
	texLoadLock.Lock();
	if ( t->loadState == TEX_LOADING ) {
		t->orphan = true;		// the loader thread is writing into it: it deletes it when done
		texLoadLock.Unlock();
		return;
	}
	if ( t->loadState == TEX_QUEUED ) {
		for ( int i = 0; i < texQueueCount; i++ ) {
			n3dsTexture_t *& q = texQueue[( texQueueHead + i ) % TEX_QUEUE_SIZE];
			if ( q == t ) {
				q = NULL;		// the loader skips it
			}
		}
	}
	Mem_Free( t->data );
	N3DS_TexDelete( t );
	texLoadLock.Unlock();
}

/*
========================
N3DS_TexBeginFrame
========================
*/
void RB_N3DS_TexLevelLoaded() {
	boostFrames = 20;		// 3D frames; at most 24 copies fit in one frame
}

void N3DS_TexBeginFrame( bool has3D ) {
	texFrame++;
	uploadsThisFrame = 0;
	uploadLimit = n3ds_texUploadsPerFrame.GetInteger();
	if ( has3D && boostFrames > 0 ) {
		// first frames of a map: get everything in view onto the GPU at once
		boostFrames--;
		uploadLimit = Max( uploadLimit, n3ds_texUploadsAfterLoad.GetInteger() );
	}
	// conversion settings changed: everything is rebuilt as it is seen again
	const bool flip = n3ds_texFlip.GetBool();
	const int maxSize = n3ds_texMaxSize.GetInteger();
	const bool mips = n3ds_texMips.GetBool();
	if ( flip != lastFlip || maxSize != lastMaxSize || mips != lastMips ) {
		lastFlip = flip;
		lastMaxSize = maxSize;
		lastMips = mips;
		while ( lruHead != NULL ) {
			N3DS_TexDropGPU( lruHead );
		}
		common->Printf( "citro3d: texture settings changed, textures are rebuilt (flip %d, max %d, mips %d)\n", flip, maxSize, mips );
	}
}

/*
========================
N3DS_TexFailed

A texture that could not be uploaded waits a second before it is tried again, so
that it does not take the per-frame upload slots from the others.
========================
*/
static int failuresLogged = 0;

static void N3DS_TexFailed( n3dsTexture_t * t, const n3dsTexLayout_t & layout, const char * why ) {
	statDeferred++;
	t->retryFrame = texFrame + 60;
	if ( failuresLogged < 12 ) {
		failuresLogged++;
		common->Printf( "citro3d: texture %s (%dx%d stored fmt %d/%d -> %dx%d fmt %d, %d levels, %d bytes) not uploaded: %s; VRAM free %u KB, linear free %u KB\n",
			t->image->GetName(), t->width, t->height, t->format, t->colorFormat, layout.width, layout.height,
			layout.format, layout.maxLevel + 1, layout.bytes, why, N3DSGPU_VRAMFree() / 1024, N3DSGPU_LinearFree() / 1024 );
	}
}

/*
========================
N3DS_TexUpload
========================
*/
// the GPU chain: from the pack as it is, else built from the decoded level
static void N3DS_TexFill( const n3dsTexture_t * t, const n3dsTexLayout_t & layout, const byte * rgba, byte * dst ) {
	if ( t->packed ) {
		memcpy( dst, t->data, layout.bytes );
	} else {
		N3DS_BuildTexture( rgba, t->width, t->height, layout, n3ds_texFlip.GetBool(), dst );
	}
}

static bool N3DS_TexHasAlpha( const n3dsTexture_t * t, const n3dsTexLayout_t & layout ) {
	return t->packed ? t->packHasAlpha : ( layout.format != N3DS_TEXCONV_RGB565 );
}

// VRAM full: the texture goes to linear memory, within n3ds_texLinearKB, evicting linear
// textures this frame does not use. Written in place, no copy.
static bool N3DS_TexUploadLinear( n3dsTexture_t * t, const n3dsTexLayout_t & layout, const byte * rgba ) {
	const int budget = Max( 0, n3ds_texLinearKB.GetInteger() ) * 1024;
	const unsigned int reserve = (unsigned int)Max( 0, n3ds_texLinearReserveKB.GetInteger() ) * 1024;
	void * gpu = NULL;
	for ( ;; ) {
		if ( linearTexBytes + layout.bytes <= budget && N3DSGPU_LinearFree() >= (unsigned int)layout.bytes + reserve ) {
			gpu = N3DSGPU_TexCreateLinear( layout.width, layout.height, layout.format, layout.maxLevel, t->wrap );
			if ( gpu != NULL ) {
				break;
			}
		}
		// the least recently used linear texture this frame does not draw
		n3dsTexture_t * victim = NULL;
		for ( n3dsTexture_t * v = lruTail; v != NULL && v->lastFrame < texFrame; v = v->lruPrev ) {
			if ( v->inLinear ) {
				victim = v;
				break;
			}
		}
		if ( victim == NULL ) {
			return false;
		}
		N3DS_TexDropGPU( victim );
		statEvictions++;
	}
	N3DS_TexFill( t, layout, rgba, (byte *)N3DSGPU_TexData( gpu ) );
	N3DSGPU_TexFlush( gpu, layout.bytes );
	t->gpu = gpu;
	t->gpuBytes = layout.bytes;
	t->inLinear = true;
	t->everUploaded = true;
	t->gpuHasAlpha = N3DS_TexHasAlpha( t, layout );
	residentCount++;
	residentBytes += layout.bytes;
	linearTexBytes += layout.bytes;
	LRU_PushFront( t );
	statLinearUploads++;
	return true;
}

static bool N3DS_TexUpload( n3dsTexture_t * t ) {
	const uint64 start = Sys_Microseconds();

	if ( t->data == NULL ) {
		return false;		// nothing to decode (a streamed level not in memory): not "unsupported"
	}
	n3dsTexLayout_t layout;
	idTempArray<byte> rgba( t->packed ? 4 : t->width * t->height * 4 );
	if ( t->packed ) {
		// converted on the PC: the data is the chain
		layout.width = t->width;
		layout.height = t->height;
		layout.maxLevel = t->packMaxLevel;
		layout.format = t->gpuFormat;
		layout.bytes = t->dataSize;
	} else if ( !N3DS_DecodeLevel( t->format, t->colorFormat, t->width, t->height, t->data, t->dataSize, rgba.Ptr() ) ) {
		t->unsupported = true;
		statFailed++;
		static int undecodableLogged = 0;
		if ( undecodableLogged < 32 ) {
			undecodableLogged++;
			common->Printf( "citro3d: texture %s cannot be decoded (%dx%d, format %d, colour format %d, %d bytes): not drawn\n",
				t->image->GetName(), t->width, t->height, t->format, t->colorFormat, t->dataSize );
		}
		return false;
	} else {
		N3DS_ChooseLayout( rgba.Ptr(), t->width, t->height, t->maxSize, n3ds_texMips.GetBool(), t->alphaOnly, layout );
	}

	// room in VRAM: evict textures that the current frame does not use
	const unsigned int reserve = (unsigned int)Max( 0, n3ds_texVRAMReserveKB.GetInteger() ) * 1024;
	void * gpu = NULL;
	for ( ;; ) {
		if ( N3DSGPU_VRAMFree() >= (unsigned int)layout.bytes + reserve ) {
			gpu = N3DSGPU_TexCreate( layout.width, layout.height, layout.format, layout.maxLevel, t->wrap );
			if ( gpu != NULL ) {
				break;
			}
		}
		// the least recently used VRAM texture this frame does not draw
		n3dsTexture_t * victim = NULL;
		for ( n3dsTexture_t * v = lruTail; v != NULL && v->lastFrame < texFrame; v = v->lruPrev ) {
			if ( !v->inLinear ) {
				victim = v;
				break;
			}
		}
		if ( victim == NULL ) {
			break;		// every VRAM texture is in use this frame
		}
		N3DS_TexDropGPU( victim );
		statEvictions++;
	}
	if ( gpu == NULL ) {
		// VRAM is full of textures this frame draws (a world in view behind a menu, a
		// cutscene with many screens): the GPU can sample from linear memory too
		if ( N3DS_TexUploadLinear( t, layout, rgba.Ptr() ) ) {
			statUploads++;
			statPackedUploads += t->packed ? 1 : 0;
			statUploadUs += Sys_Microseconds() - start;
			return true;
		}
		N3DS_TexFailed( t, layout, "no VRAM block and no room in the linear texture pool (all resident textures in use this frame)" );
		return false;
	}

	byte * staging = (byte *)N3DSGPU_LinearAlloc( layout.bytes );
	if ( staging == NULL ) {
		N3DSGPU_TexFree( gpu );
		N3DS_TexFailed( t, layout, "no linear memory for the staging copy" );
		return false;
	}
	N3DS_TexFill( t, layout, rgba.Ptr(), staging );
	if ( !N3DSGPU_TexUpload( gpu, staging, layout.bytes ) ) {		// takes the staging buffer
		N3DSGPU_TexFree( gpu );
		N3DS_TexFailed( t, layout, "copy refused (size mismatch or GX queue full)" );
		return false;
	}

	t->gpu = gpu;
	t->gpuBytes = layout.bytes;
	t->everUploaded = true;
	t->gpuHasAlpha = N3DS_TexHasAlpha( t, layout );
	residentCount++;
	residentBytes += layout.bytes;
	LRU_PushFront( t );
	statUploads++;
	statPackedUploads += t->packed ? 1 : 0;
	statUploadUs += Sys_Microseconds() - start;
	return true;
}

/*
========================
N3DS_TexForImage

The GPU texture of an image for this frame, uploading it if needed; NULL when the
image has no kept level, cannot be decoded, or has to wait for a later frame.
========================
*/
bool N3DS_FontsAlphaOnly() {
	return n3ds_texFontA8.GetBool();
}

bool N3DS_TexIsAlphaOnly( idImage * image ) {
	const n3dsTexture_t * t = ( image != NULL ) ? (const n3dsTexture_t *)image->n3dsTex : NULL;
	return t != NULL && t->alphaOnly;
}

bool N3DS_TexIsNormalMap( idImage * image ) {
	const n3dsTexture_t * t = ( image != NULL ) ? (const n3dsTexture_t *)image->n3dsTex : NULL;
	return t != NULL && t->packed && t->gpuFormat == N3DS_TEX_HILO8;
}

void * N3DS_TexForImage( idImage * image ) {
	if ( image == NULL || !n3ds_textures.GetBool() ) {
		return NULL;
	}
	n3dsTexture_t * t = (n3dsTexture_t *)image->n3dsTex;
	if ( t == NULL || t->unsupported ) {
		return NULL;
	}
	if ( t->gpu == NULL ) {
		if ( texFrame < t->retryFrame ) {
			return NULL;
		}
		if ( t->streamPath >= 0 ) {
			// streamed: take a finished read - always, also when the data turned up before
			// this frame looked. A READY left behind once made a later re-upload (after the
			// RAM cache had freed the data) decode NULL and mark the texture unsupported.
			texLoadLock.Lock();
			const int state = t->loadState;
			if ( state == TEX_READY || state == TEX_FAILED ) {
				t->loadState = TEX_IDLE;
			}
			texLoadLock.Unlock();
			if ( t->data == NULL ) {
				if ( state == TEX_FAILED ) {
					t->retryFrame = texFrame + 120;
					if ( failuresLogged < 12 ) {
						failuresLogged++;
						common->Printf( "citro3d: texture %s: reading %d bytes at %d of %s failed\n", t->image->GetName(),
							t->dataSize, t->streamOffset, texPaths[t->streamPath].path.c_str() );
					}
					return NULL;
				}
				// not read yet, being read, or read and dropped by the RAM cache since
				if ( state != TEX_QUEUED && state != TEX_LOADING ) {
					N3DS_TexRequest( t );
				}
				statStreamWaits++;
				return NULL;
			}
		}
		if ( uploadsThisFrame >= uploadLimit || !N3DSGPU_CanQueueTexture() ) {
			statDeferred++;
			return NULL;
		}
		uploadsThisFrame++;
		if ( !N3DS_TexUpload( t ) ) {
			return NULL;
		}
		if ( t->streamPath >= 0 ) {
			N3DS_TexRamKeep( t );
		}
	} else if ( lruHead != t ) {
		LRU_Unlink( t );
		LRU_PushFront( t );
	}
	t->lastFrame = texFrame;
	return t->gpu;
}

/*
========================
N3DS_TexStatsLine
========================
*/
void N3DS_TexStatsLine( idStr & line ) {
	texLoadLock.Lock();
	const int reads = statReads, readBytes = statReadBytes, readFailures = statReadFailures;
	const uint64 readUs = statReadUs;
	statReads = statReadBytes = statReadFailures = 0;
	statReadUs = 0;
	const int queued = texQueueCount;
	texLoadLock.Unlock();
	line.Format( "tex %d resident %d KB (%d KB in linear memory, %d put there), VRAM free %u KB, %d uploads (%d from the pack, %.1f ms each), %d evictions, %d waiting, %d undecodable | kept %d levels %d KB | streamed %d levels %d KB (%d packed), %d reads %d KB (%.1f ms each), %d failed, %d waits, %d queued, RAM cache %d KB",
		residentCount, residentBytes / 1024, linearTexBytes / 1024, statLinearUploads, N3DSGPU_VRAMFree() / 1024, statUploads, statPackedUploads,
		statUploads > 0 ? statUploadUs * 0.001f / statUploads : 0.0f, statEvictions, statDeferred, statFailed,
		keptCount, keptBytes / 1024, streamedCount, streamedBytes / 1024, packedCount, reads, readBytes / 1024,
		reads > 0 ? readUs * 0.001f / reads : 0.0f, readFailures, statStreamWaits, queued, ramBytes / 1024 );
	statUploads = statEvictions = statDeferred = statFailed = statStreamWaits = statLinearUploads = statPackedUploads = 0;
	statUploadUs = 0;
}

/*
========================
N3DS_TexLogMemory

Memory diagnostics (MEM checkpoints): what the kept levels are made of, how many of
them were ever drawn, and the biggest ones.
========================
*/
static const char * N3DS_FormatName( int format ) {
	switch ( format ) {
		case FMT_DXT1:		return "DXT1";
		case FMT_DXT5:		return "DXT5";
		case FMT_RGBA8:		return "RGBA8";
		case FMT_XRGB8:		return "XRGB8";
		case FMT_ALPHA:		return "ALPHA";
		case FMT_L8A8:		return "L8A8";
		case FMT_LUM8:		return "LUM8";
		case FMT_INT8:		return "INT8";
		case FMT_RGB565:	return "RGB565";
		default:			return "other";
	}
}

void N3DS_TexLogMemory() {
	if ( globalImages == NULL || globalImages->images.Num() == 0 ) {
		return;
	}
	enum { CLS_WORLD, CLS_GUI, CLS_FONT, CLS_SWF, CLS_LIGHT, CLS_NUM };
	static const char * clsNames[CLS_NUM] = { "world", "guis/", "fonts", "swf", "lights/" };
	int clsCount[CLS_NUM] = {}, clsBytes[CLS_NUM] = {};
	int fmtCount[3] = {}, fmtBytes[3] = {};		// DXT1, DXT5, other
	int usedCount = 0, usedBytes = 0, total = 0, totalBytes = 0, inRAM = 0, inRAMBytes = 0;
	const int TOP = 8;
	const n3dsTexture_t * top[TOP] = {};

	for ( int i = 0; i < globalImages->images.Num(); i++ ) {
		const idImage * image = globalImages->images[i];
		const n3dsTexture_t * t = ( image != NULL ) ? (const n3dsTexture_t *)image->n3dsTex : NULL;
		if ( t == NULL ) {
			continue;
		}
		const char * name = image->GetName();
		int cls = CLS_WORLD;
		if ( idStr::Icmpn( name, "newfonts/", 9 ) == 0 || idStr::Icmpn( name, "fonts/", 6 ) == 0 ) {
			cls = CLS_FONT;
		} else if ( idStr::Icmpn( name, "generated/swf/", 14 ) == 0 ) {
			cls = CLS_SWF;
		} else if ( idStr::Icmpn( name, "guis/", 5 ) == 0 ) {
			cls = CLS_GUI;
		} else if ( idStr::Icmpn( name, "lights/", 7 ) == 0 ) {
			cls = CLS_LIGHT;
		}
		clsCount[cls]++;
		clsBytes[cls] += t->dataSize;
		const int f = ( t->format == FMT_DXT1 ) ? 0 : ( t->format == FMT_DXT5 ) ? 1 : 2;
		fmtCount[f]++;
		fmtBytes[f] += t->dataSize;
		total++;
		totalBytes += t->dataSize;
		if ( t->data != NULL ) {
			inRAM++;
			inRAMBytes += t->dataSize;
		}
		if ( t->everUploaded ) {
			usedCount++;
			usedBytes += t->dataSize;
		}
		for ( int k = 0; k < TOP; k++ ) {
			if ( top[k] == NULL || t->dataSize > top[k]->dataSize ) {
				for ( int m = TOP - 1; m > k; m-- ) {
					top[m] = top[m - 1];
				}
				top[k] = t;
				break;
			}
		}
	}
	idLib::Printf( "  textures: %d idImages (%d B each), %d levels %d KB (in RAM %d / %d KB, %d streamed from disk), drawn since load %d / %d KB\n",
		globalImages->images.Num(), (int)sizeof( idImage ), total, totalBytes / 1024, inRAM, inRAMBytes / 1024, streamedCount,
		usedCount, usedBytes / 1024 );
	idLib::Printf( "    by kind: %s %d/%d KB, %s %d/%d KB, %s %d/%d KB, %s %d/%d KB, %s %d/%d KB\n",
		clsNames[0], clsCount[0], clsBytes[0] / 1024, clsNames[1], clsCount[1], clsBytes[1] / 1024,
		clsNames[2], clsCount[2], clsBytes[2] / 1024, clsNames[3], clsCount[3], clsBytes[3] / 1024,
		clsNames[4], clsCount[4], clsBytes[4] / 1024 );
	idLib::Printf( "    by format: DXT1 %d/%d KB, DXT5 %d/%d KB, other %d/%d KB\n",
		fmtCount[0], fmtBytes[0] / 1024, fmtCount[1], fmtBytes[1] / 1024, fmtCount[2], fmtBytes[2] / 1024 );
	for ( int k = 0; k < TOP && top[k] != NULL; k++ ) {
		idLib::Printf( "    %4d KB %dx%d %s%s%s %s\n", top[k]->dataSize / 1024, top[k]->width, top[k]->height,
			N3DS_FormatName( top[k]->format ), top[k]->everUploaded ? "" : " (not drawn)",
			top[k]->data != NULL ? "" : " (on disk)", top[k]->image->GetName() );
	}
}
