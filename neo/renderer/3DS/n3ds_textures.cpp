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
idCVar n3ds_texGuiMaxSize( "n3ds_texGuiMaxSize", "512", CVAR_RENDERER | CVAR_INTEGER, "citro3d: largest texture size for GUI / HUD / font images (guis/, generated/swf/, newfonts/, fonts/)", 8, 1024 );
idCVar n3ds_texMips( "n3ds_texMips", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: build mip maps" );
idCVar n3ds_texUploadsPerFrame( "n3ds_texUploadsPerFrame", "4", CVAR_RENDERER | CVAR_INTEGER, "citro3d: textures decoded and copied to VRAM per frame at most", 1, 64 );
idCVar n3ds_texUploadsAfterLoad( "n3ds_texUploadsAfterLoad", "256", CVAR_RENDERER | CVAR_INTEGER, "citro3d: texture upload limit for the first 20 3D frames after a map load (the GX queue caps it at 24)" );
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
	int					maxSize;		// largest side on the GPU
	bool				unsupported;	// the level cannot be decoded
	// on the GPU
	void *				gpu;
	int					gpuBytes;
	bool				gpuHasAlpha;
	int					lastFrame;
	int					retryFrame;		// after a failed upload: not before this frame
	bool				everUploaded;	// diagnostics: drawn at least once since load
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
	// GUIs are drawn at about screen size: their atlases need far more than world textures
	if ( usage == TD_FONT || N3DS_IsGuiImage( name ) ) {
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
static bool N3DS_TexUpload( n3dsTexture_t * t ) {
	const uint64 start = Sys_Microseconds();

	idTempArray<byte> rgba( t->width * t->height * 4 );
	if ( !N3DS_DecodeLevel( t->format, t->colorFormat, t->width, t->height, t->data, t->dataSize, rgba.Ptr() ) ) {
		t->unsupported = true;
		statFailed++;
		return false;
	}
	n3dsTexLayout_t layout;
	N3DS_ChooseLayout( rgba.Ptr(), t->width, t->height, t->maxSize, n3ds_texMips.GetBool(), t->alphaOnly, layout );

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
		N3DS_TexFailed( t, layout, "no VRAM block (all resident textures in use this frame, or TexCreate failed)" );
		return false;
	}

	byte * staging = (byte *)N3DSGPU_LinearAlloc( layout.bytes );
	if ( staging == NULL ) {
		N3DSGPU_TexFree( gpu );
		N3DS_TexFailed( t, layout, "no linear memory for the staging copy" );
		return false;
	}
	N3DS_BuildTexture( rgba.Ptr(), t->width, t->height, layout, n3ds_texFlip.GetBool(), staging );
	if ( !N3DSGPU_TexUpload( gpu, staging, layout.bytes ) ) {		// takes the staging buffer
		N3DSGPU_TexFree( gpu );
		N3DS_TexFailed( t, layout, "copy refused (size mismatch or GX queue full)" );
		return false;
	}

	t->gpu = gpu;
	t->gpuBytes = layout.bytes;
	t->everUploaded = true;
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
		if ( texFrame < t->retryFrame ) {
			return NULL;
		}
		if ( uploadsThisFrame >= uploadLimit || !N3DSGPU_CanQueueTexture() ) {
			statDeferred++;
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
	int usedCount = 0, usedBytes = 0, total = 0, totalBytes = 0;
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
	idLib::Printf( "  textures: %d idImages (%d B each), %d kept levels %d KB, drawn since load %d / %d KB\n",
		globalImages->images.Num(), (int)sizeof( idImage ), total, totalBytes / 1024, usedCount, usedBytes / 1024 );
	idLib::Printf( "    by kind: %s %d/%d KB, %s %d/%d KB, %s %d/%d KB, %s %d/%d KB, %s %d/%d KB\n",
		clsNames[0], clsCount[0], clsBytes[0] / 1024, clsNames[1], clsCount[1], clsBytes[1] / 1024,
		clsNames[2], clsCount[2], clsBytes[2] / 1024, clsNames[3], clsCount[3], clsBytes[3] / 1024,
		clsNames[4], clsCount[4], clsBytes[4] / 1024 );
	idLib::Printf( "    by format: DXT1 %d/%d KB, DXT5 %d/%d KB, other %d/%d KB\n",
		fmtCount[0], fmtBytes[0] / 1024, fmtCount[1], fmtBytes[1] / 1024, fmtCount[2], fmtBytes[2] / 1024 );
	for ( int k = 0; k < TOP && top[k] != NULL; k++ ) {
		idLib::Printf( "    %4d KB %dx%d %s%s %s\n", top[k]->dataSize / 1024, top[k]->width, top[k]->height,
			N3DS_FormatName( top[k]->format ), top[k]->everUploaded ? "" : " (not drawn)", top[k]->image->GetName() );
	}
}
