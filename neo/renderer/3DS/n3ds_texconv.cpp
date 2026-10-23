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
#include "n3ds_gpu.h"

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
			// a side that is not a multiple of 4 (a 512x651 SWF atlas level) is stored as whole
			// blocks: decode the padded size, then keep the top-left width x height
			const int pw = ( width + 3 ) & ~3;
			const int ph = ( height + 3 ) & ~3;
			const int blockBytes = ( format == FMT_DXT1 ) ? 8 : 16;
			if ( dataSize < ( pw / 4 ) * ( ph / 4 ) * blockBytes ) {
				return false;
			}
			if ( pw != width || ph != height ) {
				if ( colorFormat == CFM_NORMAL_DXT5 ) {
					return false;
				}
				idTempArray<byte> padded( pw * ph * 4 );
				if ( !N3DS_DecodeLevel( format, colorFormat, pw, ph, data, dataSize, padded.Ptr() ) ) {
					return false;
				}
				for ( int y = 0; y < height; y++ ) {
					memcpy( rgba + y * width * 4, padded.Ptr() + y * pw * 4, width * 4 );
				}
				return true;
			}
			idDxtDecoder dxt;
			if ( format == FMT_DXT1 ) {
				dxt.DecompressImageDXT1( data, rgba, width, height );
			} else if ( colorFormat == CFM_YCOCG_DXT5 ) {
				// CoCg_Y with the per-block scale in blue. Not DecompressYCoCgDXT5: that decodes
				// the colour block the GeForce 7 way (c0 <= c1: three colours and black), and a
				// "black" there is Co = Cg = -128 - magenta / red pixels. A DXT5 colour block is
				// four colours, as every D3D10+ GPU decodes it for the PC game.
				dxt.DecompressImageDXT5( data, rgba, width, height );
				for ( int i = 0; i < numPixels; i++ ) {
					byte * p = rgba + i * 4;
					const int scale = ( p[2] >> 3 ) + 1;
					const int co = ( p[0] - 128 ) / scale;
					const int cg = ( p[1] - 128 ) / scale;
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

// RGBA8 w x h -> tw x th, each target pixel the coverage-weighted average of the source
// pixels under it. Separable (rows, then columns) with the weights of each target
// position worked out once, in 1/65536: no division per pixel (ARM11 has no divider).
struct n3dsTap_t {
	int		first;		// first source index
	int		count;
	int		weight[8];	// sums to 65536 (ratios below 2:1 cover at most 3 sources; up to 8 allowed)
};

static void N3DS_AreaTaps( int src, int dst, n3dsTap_t * taps ) {
	for ( int t = 0; t < dst; t++ ) {
		// target t covers [t * src / dst, (t + 1) * src / dst) in 1/256 source units
		const int a = ( t * src * 256 ) / dst;
		const int b = ( ( t + 1 ) * src * 256 ) / dst;
		n3dsTap_t & tap = taps[t];
		tap.first = a >> 8;
		tap.count = 0;
		int total = 0;
		for ( int s = a >> 8; s < src && ( s << 8 ) < b && tap.count < 8; s++ ) {
			const int w = Min( b, ( s + 1 ) << 8 ) - Max( a, s << 8 );
			tap.weight[tap.count++] = Max( w, 0 );
			total += Max( w, 0 );
		}
		// normalise to 65536, the rounding remainder on the last tap
		int sum = 0;
		for ( int k = 0; k < tap.count; k++ ) {
			tap.weight[k] = ( total > 0 ) ? ( tap.weight[k] * 65536 ) / total : 0;
			sum += tap.weight[k];
		}
		if ( tap.count > 0 ) {
			tap.weight[tap.count - 1] += 65536 - sum;
		}
	}
}

static void N3DS_AreaResample( const byte * src, int w, int h, int tw, int th, byte * dst ) {
	idTempArray<n3dsTap_t> xt( tw );
	idTempArray<n3dsTap_t> yt( th );
	N3DS_AreaTaps( w, tw, xt.Ptr() );
	N3DS_AreaTaps( h, th, yt.Ptr() );
	// rows: w x h -> tw x h
	idTempArray<byte> mid( tw * h * 4 );
	for ( int y = 0; y < h; y++ ) {
		const byte * row = src + y * w * 4;
		byte * out = mid.Ptr() + y * tw * 4;
		for ( int x = 0; x < tw; x++ ) {
			const n3dsTap_t & tap = xt[x];
			int c[4] = { 32768, 32768, 32768, 32768 };
			for ( int k = 0; k < tap.count; k++ ) {
				const byte * p = row + ( tap.first + k ) * 4;
				const int wgt = tap.weight[k];
				c[0] += p[0] * wgt;
				c[1] += p[1] * wgt;
				c[2] += p[2] * wgt;
				c[3] += p[3] * wgt;
			}
			for ( int k = 0; k < 4; k++ ) {
				out[x * 4 + k] = (byte)Min( c[k] >> 16, 255 );
			}
		}
	}
	// columns: tw x h -> tw x th
	for ( int y = 0; y < th; y++ ) {
		const n3dsTap_t & tap = yt[y];
		byte * out = dst + y * tw * 4;
		for ( int x = 0; x < tw * 4; x++ ) {
			int c = 32768;
			for ( int k = 0; k < tap.count; k++ ) {
				c += mid[( tap.first + k ) * tw * 4 + x] * tap.weight[k];
			}
			out[x] = (byte)Min( c >> 16, 255 );
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
	// whatever is left (a non power of two side, less than twice the target): area
	// resample. Nearest sampling here dropped whole rows: a 256x320 font atlas made
	// 256x256 lost every fifth row of every glyph.
	if ( w != tw || h != th ) {
		N3DS_AreaResample( bufA, w, h, tw, th, bufB );
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


/*
================================================================================================

	N3DS_DecodePackLevel

	For bisecting GPU trouble with the pack's formats: the PICA ETC1 layout is 8x8 tiles in
	row order, each four 4x4 blocks (top-left, top-right, bottom-left, bottom-right), each
	block a little-endian 64-bit ETC1 block (ETC1A4: 8 bytes of 4-bit alpha first, x and
	y swapped, even rows in the low nibble). A4 is tiled texel by texel (Morton order in a
	tile), two texels per byte, the even one in the low nibble. Row 0 of the data is the
	bottom row of the image (n3dstex flips before tiling).

================================================================================================
*/
static const int etc1Modifiers[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };

static inline int Etc1Clamp( int v ) {
	return v < 0 ? 0 : ( v > 255 ? 255 : v );
}

// a standard (big-endian bit numbering) ETC1 block into 16 RGB pixels, pixel (x, y) at y * 4 + x
static void N3DS_DecodeEtc1Block( uint64 b, byte out[16][3] ) {
	const bool diff = ( b >> 33 ) & 1;
	const bool flip = ( b >> 32 ) & 1;
	int base[2][3];
	for ( int c = 0; c < 3; c++ ) {
		const int shift = 59 - c * 8;		// R at 63.., G at 55.., B at 47..
		if ( diff ) {
			const int v5 = (int)( ( b >> shift ) & 31 );
			int d = (int)( ( b >> ( shift - 3 ) ) & 7 );
			if ( d >= 4 ) {
				d -= 8;
			}
			const int v5b = ( v5 + d ) & 31;
			base[0][c] = ( v5 << 3 ) | ( v5 >> 2 );
			base[1][c] = ( v5b << 3 ) | ( v5b >> 2 );
		} else {
			const int a = (int)( ( b >> ( shift + 1 ) ) & 15 );
			const int c2 = (int)( ( b >> ( shift - 3 ) ) & 15 );
			base[0][c] = a * 17;
			base[1][c] = c2 * 17;
		}
	}
	const int table[2] = { (int)( ( b >> 37 ) & 7 ), (int)( ( b >> 34 ) & 7 ) };
	for ( int x = 0; x < 4; x++ ) {
		for ( int y = 0; y < 4; y++ ) {
			const int i = x * 4 + y;
			const int msb = (int)( ( b >> ( 16 + i ) ) & 1 );
			const int lsb = (int)( ( b >> i ) & 1 );
			const int sub = flip ? ( y >= 2 ) : ( x >= 2 );
			const int mod = etc1Modifiers[table[sub]][lsb];
			const int delta = msb ? -mod : mod;
			for ( int c = 0; c < 3; c++ ) {
				out[y * 4 + x][c] = (byte)Etc1Clamp( base[sub][c] + delta );
			}
		}
	}
}

static const byte decMortonX[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
static const byte decMortonY[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };

bool N3DS_DecodePackLevel( int gpuFormat, int width, int height, const byte * data, byte * rgba ) {
	if ( width < 8 || height < 8 || data == NULL ) {
		return false;
	}
	// into the stored (bottom-up) orientation first, flipped at the end
	if ( gpuFormat == N3DS_TEX_ETC1 || gpuFormat == N3DS_TEX_ETC1A4 ) {
		const bool alpha = ( gpuFormat == N3DS_TEX_ETC1A4 );
		const byte * p = data;
		for ( int ty = 0; ty < height; ty += 8 ) {
			for ( int tx = 0; tx < width; tx += 8 ) {
				for ( int by = 0; by < 8; by += 4 ) {
					for ( int bx = 0; bx < 8; bx += 4 ) {
						const byte * alphaBlock = NULL;
						if ( alpha ) {
							alphaBlock = p;
							p += 8;
						}
						uint64 b = 0;
						for ( int k = 7; k >= 0; k-- ) {
							b = ( b << 8 ) | p[k];		// little-endian in memory
						}
						p += 8;
						byte px[16][3];
						N3DS_DecodeEtc1Block( b, px );
						for ( int y = 0; y < 4; y++ ) {
							for ( int x = 0; x < 4; x++ ) {
								byte * o = rgba + ( ( ty + by + y ) * width + tx + bx + x ) * 4;
								o[0] = px[y * 4 + x][0];
								o[1] = px[y * 4 + x][1];
								o[2] = px[y * 4 + x][2];
								int a = 255;
								if ( alphaBlock != NULL ) {
									const byte v = alphaBlock[2 * x + y / 2];
									a = ( ( y & 1 ) ? ( v >> 4 ) : ( v & 15 ) ) * 17;
								}
								o[3] = (byte)a;
							}
						}
					}
				}
			}
		}
	} else if ( gpuFormat == N3DS_TEX_A4 ) {
		for ( int y = 0; y < height; y++ ) {
			for ( int x = 0; x < width; x++ ) {
				const int t = ( y & ~7 ) * width + ( x & ~7 ) * 8 + decMortonX[x & 7] + decMortonY[y & 7];
				const byte v = data[t >> 1];
				byte * o = rgba + ( y * width + x ) * 4;
				o[0] = o[1] = o[2] = 255;
				o[3] = (byte)( ( ( t & 1 ) ? ( v >> 4 ) : ( v & 15 ) ) * 17 );
			}
		}
	} else {
		return false;
	}
	// stored bottom-up: rows back to top-down
	for ( int y = 0; y < height / 2; y++ ) {
		byte * a = rgba + y * width * 4;
		byte * b = rgba + ( height - 1 - y ) * width * 4;
		for ( int i = 0; i < width * 4; i++ ) {
			const byte t = a[i];
			a[i] = b[i];
			b[i] = t;
		}
	}
	return true;
}
