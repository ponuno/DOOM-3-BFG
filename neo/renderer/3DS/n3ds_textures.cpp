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
	image (diffuse, default, coverage) is kept in main memory exactly as stored in
	the .bimage, e.g. 128x128 DXT5 = 16 KB. All other pixel data is still skipped.

	When a surface using the image is drawn, that level is decoded, given a mip
	chain, tiled and copied to VRAM (a few per frame). VRAM is managed as an LRU
	cache: textures not used in the current frame are evicted when space runs out,
	and come back from the kept level when they are seen again.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../tr_local.h"
#include "n3ds_gpu.h"
#include "n3ds_texconv.h"
#include "n3ds_textures.h"

idCVar n3ds_textures( "n3ds_textures", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: draw textures (0 = flat colours)" );
idCVar n3ds_texMaxSize( "n3ds_texMaxSize", "128", CVAR_RENDERER | CVAR_INTEGER, "citro3d: largest texture size; the level kept at load is the largest one not over this", 8, 512 );
idCVar n3ds_texMips( "n3ds_texMips", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: build mip maps" );
idCVar n3ds_texUploadsPerFrame( "n3ds_texUploadsPerFrame", "4", CVAR_RENDERER | CVAR_INTEGER, "citro3d: textures decoded and copied to VRAM per frame at most", 1, 64 );
idCVar n3ds_texUploadsAfterLoad( "n3ds_texUploadsAfterLoad", "256", CVAR_RENDERER | CVAR_INTEGER, "citro3d: texture upload limit for the first 3D frames after a map load" );
idCVar n3ds_texFlip( "n3ds_texFlip", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: store texture rows bottom-up (the PICA samples t = 0 at the last row); toggle if textures are upside down" );
idCVar n3ds_texVRAMReserveKB( "n3ds_texVRAMReserveKB", "64", CVAR_RENDERER | CVAR_INTEGER, "citro3d: VRAM left free by the texture cache" );

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
	bool				unsupported;	// the level cannot be decoded
	// on the GPU
	void *				gpu;
	int					gpuBytes;
	bool				gpuHasAlpha;
	int					lastFrame;
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

static void N3DS_TexDropGPU( n3dsTexture_t * t ) {
	if ( t->gpu == NULL ) {
		return;
	}
	LRU_Unlink( t );
	N3DSGPU_TexFree( t->gpu );
	t->gpu = NULL;
	residentCount--;
	residentBytes -= t->gpuBytes;
	t->gpuBytes = 0;
}

/*
========================
RB_N3DS_TexKeepSize

Largest level to keep for this image at load time, 0 = keep nothing.
========================
*/
int RB_N3DS_TexKeepSize( textureUsage_t usage ) {
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
			break;
		default:
			return 0;
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
						 const byte * data, int dataSize, int wrap, bool alphaOnly ) {
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
	image->n3dsTex = t;
	keptCount++;
	keptBytes += dataSize;
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
	keptCount--;
	keptBytes -= t->dataSize;
	Mem_Free( t->data );
	delete t;
	image->n3dsTex = NULL;
}

/*
========================
N3DS_TexBeginFrame
========================
*/
void RB_N3DS_TexLevelLoaded() {
	boostFrames = 3;
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
N3DS_TexUpload
========================
*/
static bool N3DS_TexUpload( n3dsTexture_t * t ) {
	const uint64 start = Sys_Microseconds();

	idTempArray<byte> rgba( t->width * t->height * 4 );
	if ( !N3DS_DecodeLevel( t->format, t->colorFormat, t->width, t->height, t->data, t->dataSize, rgba.Ptr() ) ) {
		t->unsupported = true;
		statFailed++;
		return false;
	}
	n3dsTexLayout_t layout;
	N3DS_ChooseLayout( rgba.Ptr(), t->width, t->height, n3ds_texMaxSize.GetInteger(), n3ds_texMips.GetBool(), t->alphaOnly, layout );

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
		if ( lruTail == NULL || lruTail->lastFrame >= texFrame ) {
			break;		// everything resident is in use this frame
		}
		N3DS_TexDropGPU( lruTail );
		statEvictions++;
	}
	if ( gpu == NULL ) {
		statDeferred++;
		return false;
	}

	byte * staging = (byte *)N3DSGPU_LinearAlloc( layout.bytes );
	if ( staging == NULL ) {
		N3DSGPU_TexFree( gpu );
		statDeferred++;
		return false;
	}
	N3DS_BuildTexture( rgba.Ptr(), t->width, t->height, layout, n3ds_texFlip.GetBool(), staging );
	if ( !N3DSGPU_TexUpload( gpu, staging, layout.bytes ) ) {		// takes the staging buffer
		N3DSGPU_TexFree( gpu );
		statDeferred++;
		return false;
	}

	t->gpu = gpu;
	t->gpuBytes = layout.bytes;
	t->gpuHasAlpha = ( layout.format != N3DS_TEXCONV_RGB565 );
	residentCount++;
	residentBytes += layout.bytes;
	LRU_PushFront( t );
	statUploads++;
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
void * N3DS_TexForImage( idImage * image ) {
	if ( image == NULL || !n3ds_textures.GetBool() ) {
		return NULL;
	}
	n3dsTexture_t * t = (n3dsTexture_t *)image->n3dsTex;
	if ( t == NULL || t->unsupported ) {
		return NULL;
	}
	if ( t->gpu == NULL ) {
		if ( uploadsThisFrame >= uploadLimit ) {
			return NULL;
		}
		uploadsThisFrame++;
		if ( !N3DS_TexUpload( t ) ) {
			return NULL;
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
	line.Format( "tex %d resident %d KB, VRAM free %u KB, %d uploads (%.1f ms each), %d evictions, %d waiting, %d undecodable | kept %d levels %d KB",
		residentCount, residentBytes / 1024, N3DSGPU_VRAMFree() / 1024, statUploads,
		statUploads > 0 ? statUploadUs * 0.001f / statUploads : 0.0f, statEvictions, statDeferred, statFailed,
		keptCount, keptBytes / 1024 );
	statUploads = statEvictions = statDeferred = statFailed = 0;
	statUploadUs = 0;
}
