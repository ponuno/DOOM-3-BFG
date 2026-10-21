/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

===========================================================================
*/

/*
================================================================================================

	New 3DS texture conversion, see n3ds_texconv.h.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../ImageOpts.h"
#include "../DXT/DXTCodec.h"
#include "n3ds_texconv.h"

static inline byte N3DS_ClampByte( int v ) {
	return (byte)( v < 0 ? 0 : ( v > 255 ? 255 : v ) );
}

/*
========================
N3DS_DecodeLevel
========================
*/
bool N3DS_DecodeLevel( int format, int colorFormat, int width, int height,
					   const byte * data, int dataSize, byte * rgba ) {
	if ( data == NULL || width <= 0 || height <= 0 ) {
		return false;
	}
	const int numPixels = width * height;

	switch ( format ) {
		case FMT_DXT1:
		case FMT_DXT5: {
			if ( ( width & 3 ) != 0 || ( height & 3 ) != 0 ) {
				return false;		// block-padded levels are not produced at the sizes kept here
			}
			const int blockBytes = ( format == FMT_DXT1 ) ? 8 : 16;
			if ( dataSize < ( width / 4 ) * ( height / 4 ) * blockBytes ) {
				return false;
			}
			idDxtDecoder dxt;
			if ( format == FMT_DXT1 ) {
				dxt.DecompressImageDXT1( data, rgba, width, height );
			} else if ( colorFormat == CFM_YCOCG_DXT5 ) {
				// output is CoCg_Y with the per-block scale already removed
				dxt.DecompressYCoCgDXT5( data, rgba, width, height );
				for ( int i = 0; i < numPixels; i++ ) {
					byte * p = rgba + i * 4;
					const int co = p[0] - 128;
					const int cg = p[1] - 128;
					const int y = p[3];
					p[0] = N3DS_ClampByte( y + co - cg );
					p[1] = N3DS_ClampByte( y + cg );
					p[2] = N3DS_ClampByte( y - co - cg );
					p[3] = 255;
				}
			} else if ( colorFormat == CFM_NORMAL_DXT5 ) {
				return false;		// normal maps: not drawn yet
			} else {
				dxt.DecompressImageDXT5( data, rgba, width, height );
			}
			break;
		}
		case FMT_RGBA8:
		case FMT_XRGB8:
			if ( dataSize < numPixels * 4 ) {
				return false;
			}
			memcpy( rgba, data, numPixels * 4 );
			if ( format == FMT_XRGB8 ) {
				for ( int i = 0; i < numPixels; i++ ) {
					rgba[i * 4 + 3] = 255;
				}
			}
			break;
		case FMT_RGB565:
			// stored big-endian (see idBinaryImage::Load2DFromMemory)
			if ( dataSize < numPixels * 2 ) {
				return false;
			}
			for ( int i = 0; i < numPixels; i++ ) {
				const int c = ( data[i * 2 + 0] << 8 ) | data[i * 2 + 1];
				const int r = ( c >> 11 ) & 31;
				const int g = ( c >> 5 ) & 63;
				const int b = c & 31;
				rgba[i * 4 + 0] = (byte)( ( r << 3 ) | ( r >> 2 ) );
				rgba[i * 4 + 1] = (byte)( ( g << 2 ) | ( g >> 4 ) );
				rgba[i * 4 + 2] = (byte)( ( b << 3 ) | ( b >> 2 ) );
				rgba[i * 4 + 3] = 255;
			}
			break;
		case FMT_LUM8:
		case FMT_INT8:
		case FMT_ALPHA:
			if ( dataSize < numPixels ) {
				return false;
			}
			for ( int i = 0; i < numPixels; i++ ) {
				const byte v = data[i];
				byte * p = rgba + i * 4;
				if ( format == FMT_LUM8 ) {
					p[0] = p[1] = p[2] = v;
					p[3] = 255;
				} else if ( format == FMT_INT8 ) {
					p[0] = p[1] = p[2] = p[3] = v;
				} else {
					p[0] = p[1] = p[2] = 255;		// alpha images read as white with alpha
					p[3] = v;
				}
			}
			break;
		case FMT_L8A8:
			if ( dataSize < numPixels * 2 ) {
				return false;
			}
			for ( int i = 0; i < numPixels; i++ ) {
				byte * p = rgba + i * 4;
				p[0] = p[1] = p[2] = data[i * 2 + 0];
				p[3] = data[i * 2 + 1];
			}
			break;
		default:
			return false;
	}

	if ( colorFormat == CFM_GREEN_ALPHA ) {
		// coverage / font images keep their alpha in green
		for ( int i = 0; i < numPixels; i++ ) {
			byte * p = rgba + i * 4;
			p[3] = p[1];
			p[0] = p[1] = p[2] = 255;
		}
	}
	return true;
}

/*
========================
N3DS_ChooseLayout
========================
*/
static int N3DS_PowerOfTwoBelow( int v ) {
	int p = 1;
	while ( p * 2 <= v ) {
		p *= 2;
	}
	return p;
}

static int N3DS_BytesPerPixel( int format ) {
	return ( format == N3DS_TEXCONV_A8 ) ? 1 : 2;
}

int N3DS_LevelBytes( const n3dsTexLayout_t & layout, int level ) {
	return ( layout.width >> level ) * ( layout.height >> level ) * N3DS_BytesPerPixel( layout.format );
}

void N3DS_ChooseLayout( const byte * rgba, int width, int height, int maxSize, bool mips,
						bool alphaOnly, n3dsTexLayout_t & layout ) {
	maxSize = idMath::ClampInt( 8, 1024, N3DS_PowerOfTwoBelow( Max( maxSize, 8 ) ) );
	int tw = N3DS_PowerOfTwoBelow( Max( width, 1 ) );
	int th = N3DS_PowerOfTwoBelow( Max( height, 1 ) );
	while ( tw > maxSize || th > maxSize ) {
		tw = Max( tw >> 1, 1 );
		th = Max( th >> 1, 1 );
	}
	tw = Max( tw, 8 );
	th = Max( th, 8 );
	layout.width = tw;
	layout.height = th;

	layout.maxLevel = 0;
	if ( mips ) {
		int m = Min( tw, th );
		while ( m > 8 ) {
			m >>= 1;
			layout.maxLevel++;
		}
	}

	if ( alphaOnly ) {
		layout.format = N3DS_TEXCONV_A8;
	} else {
		bool opaque = true;
		bool binary = true;
		for ( int i = 0; i < width * height; i++ ) {
			const byte a = rgba[i * 4 + 3];
			if ( a < 250 ) {
				opaque = false;
			}
			if ( a > 8 && a < 247 ) {
				binary = false;
				break;
			}
		}
		layout.format = opaque ? N3DS_TEXCONV_RGB565 : ( binary ? N3DS_TEXCONV_RGBA5551 : N3DS_TEXCONV_RGBA4 );
	}

	layout.bytes = 0;
	for ( int l = 0; l <= layout.maxLevel; l++ ) {
		layout.bytes += N3DS_LevelBytes( layout, l );
	}
}

/*
========================
N3DS_BuildTexture
========================
*/

// 2x2 (or 2x1 / 1x2) box filter of an RGBA8 image, in place into dst
static void N3DS_Halve( const byte * src, int w, int h, bool halveX, bool halveY, byte * dst ) {
	const int nw = halveX ? w / 2 : w;
	const int nh = halveY ? h / 2 : h;
	for ( int y = 0; y < nh; y++ ) {
		const int y0 = halveY ? y * 2 : y;
		const int y1 = halveY ? y0 + 1 : y0;
		for ( int x = 0; x < nw; x++ ) {
			const int x0 = halveX ? x * 2 : x;
			const int x1 = halveX ? x0 + 1 : x0;
			const byte * a = src + ( y0 * w + x0 ) * 4;
			const byte * b = src + ( y0 * w + x1 ) * 4;
			const byte * c = src + ( y1 * w + x0 ) * 4;
			const byte * d = src + ( y1 * w + x1 ) * 4;
			byte * o = dst + ( y * nw + x ) * 4;
			for ( int k = 0; k < 4; k++ ) {
				o[k] = (byte)( ( a[k] + b[k] + c[k] + d[k] + 2 ) >> 2 );
			}
		}
	}
}

static const byte mortonX[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
static const byte mortonY[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };

static inline unsigned short N3DS_Pack16( const byte * p, int format ) {
	switch ( format ) {
		case N3DS_TEXCONV_RGBA5551:
			return (unsigned short)( ( ( p[0] >> 3 ) << 11 ) | ( ( p[1] >> 3 ) << 6 ) | ( ( p[2] >> 3 ) << 1 ) | ( p[3] >= 128 ? 1 : 0 ) );
		case N3DS_TEXCONV_RGBA4:
			return (unsigned short)( ( ( p[0] >> 4 ) << 12 ) | ( ( p[1] >> 4 ) << 8 ) | ( ( p[2] >> 4 ) << 4 ) | ( p[3] >> 4 ) );
		default:
			return (unsigned short)( ( ( p[0] >> 3 ) << 11 ) | ( ( p[1] >> 2 ) << 5 ) | ( p[2] >> 3 ) );
	}
}

// one level, RGBA8 w x h (both multiples of 8) -> tiled
static void N3DS_TileLevel( const byte * rgba, int w, int h, int format, bool flip, byte * dst ) {
	for ( int y = 0; y < h; y++ ) {
		const int ty = flip ? ( h - 1 - y ) : y;
		const int rowBase = ( ty & ~7 ) * w;
		const int my = mortonY[ty & 7];
		const byte * src = rgba + y * w * 4;
		for ( int x = 0; x < w; x++ ) {
			const int texel = rowBase + ( x & ~7 ) * 8 + mortonX[x & 7] + my;
			if ( format == N3DS_TEXCONV_A8 ) {
				dst[texel] = src[x * 4 + 3];
			} else {
				( (unsigned short *)dst )[texel] = N3DS_Pack16( src + x * 4, format );
			}
		}
	}
}

void N3DS_BuildTexture( const byte * rgba, int width, int height, const n3dsTexLayout_t & layout,
						bool flip, byte * dst ) {
	const int tw = layout.width;
	const int th = layout.height;

	// two scratch images, large enough for the source and the target
	const int maxPixels = Max( width * height, tw * th );
	byte * bufA = (byte *)Mem_Alloc( maxPixels * 4, TAG_TEMP );
	byte * bufB = (byte *)Mem_Alloc( maxPixels * 4, TAG_TEMP );
	memcpy( bufA, rgba, width * height * 4 );
	int w = width;
	int h = height;

	// box-filter down while a dimension is at least twice the target
	while ( w >= tw * 2 || h >= th * 2 ) {
		const bool hx = ( w >= tw * 2 );
		const bool hy = ( h >= th * 2 );
		N3DS_Halve( bufA, w, h, hx, hy, bufB );
		if ( hx ) {
			w /= 2;
		}
		if ( hy ) {
			h /= 2;
		}
		SwapValues( bufA, bufB );
	}
	// whatever is left (non power of two sizes, tiny images): nearest sample
	if ( w != tw || h != th ) {
		for ( int y = 0; y < th; y++ ) {
			const int sy = ( y * h ) / th;
			for ( int x = 0; x < tw; x++ ) {
				const int sx = ( x * w ) / tw;
				memcpy( bufB + ( y * tw + x ) * 4, bufA + ( sy * w + sx ) * 4, 4 );
			}
		}
		SwapValues( bufA, bufB );
		w = tw;
		h = th;
	}

	byte * out = dst;
	for ( int level = 0; level <= layout.maxLevel; level++ ) {
		N3DS_TileLevel( bufA, w, h, layout.format, flip, out );
		out += N3DS_LevelBytes( layout, level );
		if ( level < layout.maxLevel ) {
			N3DS_Halve( bufA, w, h, true, true, bufB );
			w /= 2;
			h /= 2;
			SwapValues( bufA, bufB );
		}
	}

	Mem_Free( bufA );
	Mem_Free( bufB );
}
