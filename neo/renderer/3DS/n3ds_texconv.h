/*
================================================================================================

	New 3DS texture conversion: one level of a .bimage -> PICA200 texture data.

	No GPU calls here (host-testable): decode to RGBA8, fit to a power of two size,
	build the mip chain and write it in the PICA tiled layout (8x8 tiles, Morton
	order inside a tile), levels one after another as citro3d expects them.

================================================================================================
*/
#ifndef __N3DS_TEXCONV_H__
#define __N3DS_TEXCONV_H__

// GPU formats (same values as N3DS_TEX_* in n3ds_gpu.h)
enum n3dsTexFormat_t {
	N3DS_TEXCONV_RGB565		= 0,
	N3DS_TEXCONV_RGBA5551	= 1,
	N3DS_TEXCONV_RGBA4		= 2,
	N3DS_TEXCONV_A8			= 3
};

// decodes one stored level (bimage format / colorFormat values) to RGBA8,
// rgba must hold width * height * 4 bytes; false for formats that are not handled
bool	N3DS_DecodeLevel( int format, int colorFormat, int width, int height,
						  const byte * data, int dataSize, byte * rgba );

struct n3dsTexLayout_t {
	int		width;			// power of two, 8..1024
	int		height;
	int		maxLevel;		// mip levels - 1
	int		format;			// n3dsTexFormat_t
	int		bytes;			// whole chain
};

// picks size, mip count and format for an RGBA8 image; alphaOnly: the texture is
// only used for its alpha (alpha-test mask)
void	N3DS_ChooseLayout( const byte * rgba, int width, int height, int maxSize, bool mips,
						   bool alphaOnly, n3dsTexLayout_t & layout );

// writes the whole chain for 'layout' into dst (layout.bytes); flip: image row 0
// goes to the last texture row (the PICA samples t = 0 there, GL at row 0)
void	N3DS_BuildTexture( const byte * rgba, int width, int height, const n3dsTexLayout_t & layout,
						   bool flip, byte * dst );

// diagnostics (n3ds_texPackDecode): level 0 of a texture pack chain in ETC1 / ETC1A4 /
// A4 (N3DS_TEX_*, tiled and flipped as the GPU takes it) back to RGBA8, rows top-down
// as N3DS_BuildTexture expects; false for other formats
bool	N3DS_DecodePackLevel( int gpuFormat, int width, int height, const byte * data, byte * rgba );

// size in bytes of one level (level 0 = width x height)
int		N3DS_LevelBytes( const n3dsTexLayout_t & layout, int level );

#endif
