/*
================================================================================================

	New 3DS texture pack (_n3ds_textures.pak), written by tools/n3ds/n3dstex.

	Every 2D image of the game's .resources files, converted on the PC into what the
	PICA200 samples directly: ETC1 / ETC1A4 for world textures (4 / 8 bits per pixel),
	HILO8 for normal maps, A4 / A8 for masks and fonts, 16-bit formats for GUI art.
	Each texture is its whole mip chain, tiled, largest level first, levels back to
	back exactly as citro3d lays them out: the engine copies it to VRAM as it is.
	A smaller top level is the same data from a later offset (N3DS_PackLevelOffset).

	Layout (little-endian):
		n3dsPackHeader_t
		... texture data, each texture at a multiple of N3DS_PACK_ALIGN ...
		n3dsPackEntry_t[numEntries], sorted by nameHash

	nameHash: FNV-1a 64 of the generated file name as the resource table stores it
	(lower case, forward slashes), e.g. "generated/images/textures/base_wall/foo#__0100.bimage".

	No engine headers here: the converter includes this file too.

================================================================================================
*/
#ifndef __N3DS_PACK_H__
#define __N3DS_PACK_H__

#include <stdint.h>

#define N3DS_PACK_MAGIC		0x5854334Eu		// "N3TX"
#define N3DS_PACK_VERSION	1
#define N3DS_PACK_ALIGN		128

// GPU texture formats in the pack: the PICA's own numbers (GPU_TEXCOLOR)
enum {
	N3DS_PACKFMT_RGBA8		= 0x0,
	N3DS_PACKFMT_RGB8		= 0x1,
	N3DS_PACKFMT_RGBA5551	= 0x2,
	N3DS_PACKFMT_RGB565		= 0x3,
	N3DS_PACKFMT_RGBA4		= 0x4,
	N3DS_PACKFMT_LA8		= 0x5,
	N3DS_PACKFMT_HILO8		= 0x6,
	N3DS_PACKFMT_L8			= 0x7,
	N3DS_PACKFMT_A8			= 0x8,
	N3DS_PACKFMT_LA4		= 0x9,
	N3DS_PACKFMT_L4			= 0xA,
	N3DS_PACKFMT_A4			= 0xB,
	N3DS_PACKFMT_ETC1		= 0xC,
	N3DS_PACKFMT_ETC1A4		= 0xD
};

// n3dsPackEntry_t::flags
enum {
	N3DS_PACKFLAG_ALPHA		= 1,	// the texture has a meaningful alpha channel
	N3DS_PACKFLAG_ALPHAONLY	= 2,	// only the alpha is used (A4 / A8: masks, fonts)
	N3DS_PACKFLAG_NORMALMAP	= 4		// HILO8: x in hi (red), y in lo (green), z rebuilt by the GPU
};

#pragma pack( push, 1 )
struct n3dsPackHeader_t {
	uint32_t	magic;
	uint32_t	version;
	uint32_t	numEntries;
	uint32_t	entryOffset;		// file offset of the entry table
	uint32_t	converterFlags;
	uint16_t	maxWorld;			// sizes the converter was run with (for the log)
	uint16_t	maxBump;
	uint16_t	maxSpecular;
	uint16_t	maxGui;
	uint32_t	reserved;
};

struct n3dsPackEntry_t {
	uint64_t	nameHash;
	uint32_t	dataOffset;			// in units of N3DS_PACK_ALIGN
	uint32_t	dataSize;			// whole chain
	int64_t		sourceFileTime;		// as the .bimage header
	uint16_t	width;				// GPU level 0, powers of two >= 8
	uint16_t	height;
	uint16_t	srcWidth;			// .bimage header: what the engine's idImage reports
	uint16_t	srcHeight;
	uint8_t		gpuFormat;			// N3DS_PACKFMT_*
	uint8_t		maxLevel;			// mip levels - 1
	uint8_t		srcFormat;			// textureFormat_t
	uint8_t		srcColorFormat;		// textureColor_t
	uint8_t		srcNumLevels;
	uint8_t		flags;				// N3DS_PACKFLAG_*
	uint8_t		usage;				// textureUsage_t from the name ("#__UUCC")
	uint8_t		pad;
};
#pragma pack( pop )

typedef char n3dsPackHeaderSizeCheck[ sizeof( n3dsPackHeader_t ) == 32 ? 1 : -1 ];
typedef char n3dsPackEntrySizeCheck[ sizeof( n3dsPackEntry_t ) == 40 ? 1 : -1 ];

static inline int N3DS_PackBitsPerPixel( int fmt ) {
	switch ( fmt ) {
		case N3DS_PACKFMT_RGBA8:	return 32;
		case N3DS_PACKFMT_RGB8:		return 24;
		case N3DS_PACKFMT_RGBA5551:
		case N3DS_PACKFMT_RGB565:
		case N3DS_PACKFMT_RGBA4:
		case N3DS_PACKFMT_LA8:
		case N3DS_PACKFMT_HILO8:	return 16;
		case N3DS_PACKFMT_L8:
		case N3DS_PACKFMT_A8:
		case N3DS_PACKFMT_LA4:
		case N3DS_PACKFMT_ETC1A4:	return 8;
		case N3DS_PACKFMT_L4:
		case N3DS_PACKFMT_A4:
		case N3DS_PACKFMT_ETC1:		return 4;
		default:					return 0;
	}
}

static inline uint32_t N3DS_PackLevelBytes( int fmt, int width, int height, int level ) {
	return (uint32_t)( ( width >> level ) * ( height >> level ) * N3DS_PackBitsPerPixel( fmt ) / 8 );
}

// bytes before 'level' in the chain
static inline uint32_t N3DS_PackLevelOffset( int fmt, int width, int height, int level ) {
	uint32_t offset = 0;
	for ( int l = 0; l < level; l++ ) {
		offset += N3DS_PackLevelBytes( fmt, width, height, l );
	}
	return offset;
}

static inline uint64_t N3DS_PackHash( const char * s ) {
	uint64_t h = 14695981039346656037ull;
	for ( ; *s != '\0'; s++ ) {
		unsigned char c = (unsigned char)*s;
		if ( c >= 'A' && c <= 'Z' ) {
			c = (unsigned char)( c - 'A' + 'a' );
		} else if ( c == '\\' ) {
			c = '/';
		}
		h ^= c;
		h *= 1099511628211ull;
	}
	return h;
}

#endif
