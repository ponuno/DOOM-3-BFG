/*
================================================================================================

	n3dstex: DOOM 3 BFG textures -> New 3DS texture pack (_n3ds_textures.pak)

	Reads every 2D image (generated/images/...bimage) from the game's .resources
	files and writes it in a format the PICA200 samples directly, with its whole mip
	chain, tiled and flipped as the engine's citro3d backend expects. The engine then
	copies a texture to VRAM as it is: no DXT decoding, resampling or tiling on the
	console, a quarter of the VRAM for world textures (ETC1, 4 bpp, instead of RGB565),
	and no .bimage reads at map load. Pack format: neo/renderer/3DS/n3ds_pack.h.

	What becomes what (usage from the "#__UUCC" in the name):
		diffuse, specular, default, light images	ETC1, ETC1A4 when the alpha is used
		bump (normal maps)							HILO8 (x, y; z rebuilt by the GPU)
		coverage (alpha-test masks)					A4
		fonts										A8
		GUI art (guis/, generated/swf/), lookup		RGB565 / RGBA5551 / RGBA4
	Cube maps are left out (the citro3d backend does not draw them yet).

	Usage: n3dstex <doom3 base folder> [options]   (n3dstex -h for the options)

	Part of the DOOM 3 BFG New 3DS port (GPL v3, as the game's source). rg_etc1.cpp is
	Rich Geldreich's ETC1 packer (zlib licence).

================================================================================================
*/
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rg_etc1.h"
#include "../../../neo/renderer/3DS/n3ds_pack.h"

namespace fs = std::filesystem;
typedef uint8_t byte;

/*
================================================================================================
	Engine enums (neo/renderer/Image.h, ImageOpts.h)
================================================================================================
*/
enum { TD_SPECULAR, TD_DIFFUSE, TD_DEFAULT, TD_BUMP, TD_FONT, TD_LIGHT, TD_LOOKUP_TABLE_MONO,
	TD_LOOKUP_TABLE_ALPHA, TD_LOOKUP_TABLE_RGB1, TD_LOOKUP_TABLE_RGBA, TD_COVERAGE, TD_DEPTH };
enum { FMT_NONE, FMT_RGBA8, FMT_XRGB8, FMT_ALPHA, FMT_L8A8, FMT_LUM8, FMT_INT8, FMT_DXT1, FMT_DXT5,
	FMT_DEPTH, FMT_X16, FMT_Y16_X16, FMT_RGB565 };
enum { CFM_DEFAULT, CFM_NORMAL_DXT5, CFM_YCOCG_DXT5, CFM_GREEN_ALPHA };
enum { TT_DISABLED, TT_2D, TT_CUBIC };

static const uint32_t RESOURCE_FILE_MAGIC = 0xD000000D;
static const uint32_t BIMAGE_MAGIC = ( 'B' << 0 ) | ( 'I' << 8 ) | ( 'M' << 16 ) | ( 10 << 24 );

/*
================================================================================================
	Options
================================================================================================
*/
struct options_t {
	std::string	baseDir;
	std::string	output;
	int			maxWorld = 256;
	int			maxBump = 128;
	int			maxSpecular = 128;
	int			maxGui = 512;
	int			maxFont = 1024;
	int			maxLight = 128;
	int			threads = 0;
	int			quality = 1;		// rg_etc1: 0 low, 1 medium, 2 high
	std::string	only;				// convert names containing this only (testing)
	std::string	previewDir;			// write the converted level 0 as .tga here (with -only)
	bool		list = false;
	bool		verbose = false;
};
static options_t opt;

static std::mutex logLock;
static void Log( const char * fmt, ... ) {
	char buf[2048];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	std::lock_guard<std::mutex> lock( logLock );
	fputs( buf, stdout );
	fflush( stdout );
}

/*
================================================================================================
	Big-endian readers
================================================================================================
*/
static inline uint32_t BE32( const byte * p ) {
	return ( (uint32_t)p[0] << 24 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 8 ) | p[3];
}
static inline int64_t BE64( const byte * p ) {
	return (int64_t)( ( (uint64_t)BE32( p ) << 32 ) | BE32( p + 4 ) );
}
static inline uint32_t LE32( const byte * p ) {
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

// 64-bit file positions (a long is 32 bits on Windows)
#if defined( _WIN32 )
#define FSeek64( f, o, w )	_fseeki64( f, o, w )
#define FTell64( f )		_ftelli64( f )
#else
#define FSeek64( f, o, w )	fseeko( f, (off_t)( o ), w )
#define FTell64( f )		( (int64_t)ftello( f ) )
#endif

static bool ReadAt( FILE * f, uint32_t offset, void * dst, uint32_t size ) {
	if ( FSeek64( f, (int64_t)offset, SEEK_SET ) != 0 ) {
		return false;
	}
	return fread( dst, 1, size, f ) == size;
}

/*
================================================================================================
	Resource containers
================================================================================================
*/
struct imageSource_t {
	std::string	name;			// lower case, as in the table
	int			container;
	uint32_t	offset;
	uint32_t	length;
};

static std::vector<std::string>		containers;
static std::vector<imageSource_t>	sources;

static bool ReadContainerTable( const std::string & path, int index, std::map<std::string, int> & seen, int & added, int & total ) {
	FILE * f = fopen( path.c_str(), "rb" );
	if ( f == NULL ) {
		Log( "  cannot open %s\n", path.c_str() );
		return false;
	}
	byte head[12];
	if ( fread( head, 1, 12, f ) != 12 || BE32( head ) != RESOURCE_FILE_MAGIC ) {
		Log( "  %s: not a resource file\n", path.c_str() );
		fclose( f );
		return false;
	}
	const uint32_t tableOffset = BE32( head + 4 );
	const uint32_t tableLength = BE32( head + 8 );
	FSeek64( f, 0, SEEK_END );
	const int64_t fileLength = FTell64( f );
	if ( tableOffset < 12 || tableLength <= 4 || (int64_t)tableOffset > fileLength || (int64_t)tableLength > fileLength - (int64_t)tableOffset ) {
		Log( "  %s is damaged: table at %u, %u bytes, file %lld bytes\n", path.c_str(), tableOffset, tableLength, (long long)fileLength );
		fclose( f );
		return false;
	}
	std::vector<byte> table( tableLength );
	if ( !ReadAt( f, tableOffset, table.data(), tableLength ) ) {
		Log( "  %s: cannot read the table\n", path.c_str() );
		fclose( f );
		return false;
	}
	fclose( f );

	const byte * p = table.data();
	const byte * end = p + tableLength;
	const uint32_t num = BE32( p );
	p += 4;
	added = 0;
	total = 0;
	for ( uint32_t i = 0; i < num; i++ ) {
		if ( end - p < 4 ) {
			Log( "  %s: table ends early (%u of %u entries)\n", path.c_str(), i, num );
			return false;
		}
		const uint32_t len = LE32( p );		// idFile::ReadString: little-endian length
		p += 4;
		if ( (uint32_t)( end - p ) < len + 8 ) {
			Log( "  %s: table ends early (%u of %u entries)\n", path.c_str(), i, num );
			return false;
		}
		std::string name( (const char *)p, len );
		p += len;
		const uint32_t offset = BE32( p );
		const uint32_t length = BE32( p + 4 );
		p += 8;
		for ( char & c : name ) {
			c = ( c == '\\' ) ? '/' : (char)tolower( (unsigned char)c );
		}
		if ( name.compare( 0, 17, "generated/images/" ) != 0 || name.size() < 7 || name.compare( name.size() - 7, 7, ".bimage" ) != 0 ) {
			continue;
		}
		total++;
		if ( seen.count( name ) ) {
			continue;
		}
		seen[name] = (int)sources.size();
		sources.push_back( { name, index, offset, length } );
		added++;
	}
	return true;
}

static void FindContainers() {
	std::vector<std::string> found;
	std::error_code ec;
	for ( fs::recursive_directory_iterator it( opt.baseDir, ec ), endIt; it != endIt; it.increment( ec ) ) {
		if ( ec ) {
			break;
		}
		if ( !it->is_regular_file() ) {
			continue;
		}
		std::string ext = it->path().extension().string();
		std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
		if ( ext == ".resources" ) {
			found.push_back( it->path().string() );
		}
	}
	// the common ones first, then the maps, each in name order (stable output)
	std::sort( found.begin(), found.end(), []( const std::string & a, const std::string & b ) {
		const bool ma = a.find( "maps" ) != std::string::npos;
		const bool mb = b.find( "maps" ) != std::string::npos;
		if ( ma != mb ) {
			return !ma;
		}
		return a < b;
	} );
	containers = found;
}

/*
================================================================================================
	Decoding (as the engine's n3ds_texconv.cpp / idDxtDecoder)
================================================================================================
*/
static inline byte ClampByte( int v ) {
	return (byte)( v < 0 ? 0 : ( v > 255 ? 255 : v ) );
}

static void ColorFrom565( uint16_t c, byte * color ) {
	color[0] = (byte)( ( ( c >> 8 ) & 0xF8 ) | ( ( c >> 13 ) & 7 ) );
	color[1] = (byte)( ( ( c >> 3 ) & 0xFC ) | ( ( c >> 9 ) & 3 ) );
	color[2] = (byte)( ( ( c << 3 ) & 0xF8 ) | ( ( c >> 2 ) & 7 ) );
}

// one 8-byte colour block -> 16 RGBA pixels (alpha written only if writeAlpha)
static void DecodeColorBlock( const byte * in, bool noBlack, bool writeAlpha, byte * block ) {
	byte colors[4][4];
	const uint16_t c0 = (uint16_t)( in[0] | ( in[1] << 8 ) );
	const uint16_t c1 = (uint16_t)( in[2] | ( in[3] << 8 ) );
	ColorFrom565( c0, colors[0] );
	ColorFrom565( c1, colors[1] );
	colors[0][3] = colors[1][3] = 255;
	if ( noBlack || c0 > c1 ) {
		for ( int k = 0; k < 3; k++ ) {
			colors[2][k] = (byte)( ( 2 * colors[0][k] + colors[1][k] ) / 3 );
			colors[3][k] = (byte)( ( colors[0][k] + 2 * colors[1][k] ) / 3 );
		}
		colors[2][3] = colors[3][3] = 255;
	} else {
		for ( int k = 0; k < 3; k++ ) {
			colors[2][k] = (byte)( ( colors[0][k] + colors[1][k] ) / 2 );
			colors[3][k] = 0;
		}
		colors[2][3] = 255;
		colors[3][3] = 0;
	}
	uint32_t indexes = LE32( in + 4 );
	for ( int i = 0; i < 16; i++ ) {
		const byte * c = colors[indexes & 3];
		block[i * 4 + 0] = c[0];
		block[i * 4 + 1] = c[1];
		block[i * 4 + 2] = c[2];
		if ( writeAlpha ) {
			block[i * 4 + 3] = c[3];
		}
		indexes >>= 2;
	}
}

static void DecodeAlphaBlock( const byte * in, byte * block ) {
	byte a[8];
	a[0] = in[0];
	a[1] = in[1];
	if ( a[0] > a[1] ) {
		for ( int k = 1; k <= 6; k++ ) {
			a[1 + k] = (byte)( ( ( 7 - k ) * a[0] + k * a[1] ) / 7 );
		}
	} else {
		for ( int k = 1; k <= 4; k++ ) {
			a[1 + k] = (byte)( ( ( 5 - k ) * a[0] + k * a[1] ) / 5 );
		}
		a[6] = 0;
		a[7] = 255;
	}
	for ( int half = 0; half < 2; half++ ) {
		const byte * p = in + 2 + half * 3;
		uint32_t indexes = p[0] | ( p[1] << 8 ) | ( p[2] << 16 );
		for ( int i = 0; i < 8; i++ ) {
			block[( half * 8 + i ) * 4 + 3] = a[indexes & 7];
			indexes >>= 3;
		}
	}
}

// formats: 1 = DXT1, 5 = DXT5, 7 = DXT5 with the colour block always 4-colour off (nVidia7x)
static void DecodeDXT( int kind, const byte * data, int width, int height, byte * rgba ) {
	byte block[64];
	const byte * in = data;
	for ( int y = 0; y < height; y += 4 ) {
		for ( int x = 0; x < width; x += 4 ) {
			if ( kind == 1 ) {
				DecodeColorBlock( in, false, true, block );
				in += 8;
			} else {
				DecodeAlphaBlock( in, block );
				DecodeColorBlock( in + 8, kind == 5, false, block );
				in += 16;
			}
			for ( int j = 0; j < 4; j++ ) {
				memcpy( rgba + ( ( y + j ) * width + x ) * 4, block + j * 16, 16 );
			}
		}
	}
}

static bool DecodeLevel( int format, int colorFormat, int width, int height, const byte * data, uint32_t dataSize, byte * rgba ) {
	const int numPixels = width * height;
	switch ( format ) {
		case FMT_DXT1:
		case FMT_DXT5: {
			const int pw = ( width + 3 ) & ~3;
			const int ph = ( height + 3 ) & ~3;
			const int blockBytes = ( format == FMT_DXT1 ) ? 8 : 16;
			if ( dataSize < (uint32_t)( ( pw / 4 ) * ( ph / 4 ) * blockBytes ) ) {
				return false;
			}
			std::vector<byte> padded( (size_t)pw * ph * 4 );
			if ( format == FMT_DXT1 ) {
				DecodeDXT( 1, data, pw, ph, padded.data() );
			} else if ( colorFormat == CFM_YCOCG_DXT5 ) {
				DecodeDXT( 7, data, pw, ph, padded.data() );
				for ( int i = 0; i < pw * ph; i++ ) {
					byte * p = &padded[i * 4];
					const int scale = ( p[2] >> 3 ) + 1;
					const int co = ( p[0] - 128 ) / scale;
					const int cg = ( p[1] - 128 ) / scale;
					const int yv = p[3];
					p[0] = ClampByte( yv + co - cg );
					p[1] = ClampByte( yv + cg );
					p[2] = ClampByte( yv - co - cg );
					p[3] = 255;
				}
			} else {
				DecodeDXT( 5, data, pw, ph, padded.data() );
			}
			for ( int y = 0; y < height; y++ ) {
				memcpy( rgba + (size_t)y * width * 4, &padded[(size_t)y * pw * 4], (size_t)width * 4 );
			}
			break;
		}
		case FMT_RGBA8:
		case FMT_XRGB8:
			if ( dataSize < (uint32_t)numPixels * 4 ) {
				return false;
			}
			memcpy( rgba, data, (size_t)numPixels * 4 );
			if ( format == FMT_XRGB8 ) {
				for ( int i = 0; i < numPixels; i++ ) {
					rgba[i * 4 + 3] = 255;
				}
			}
			break;
		case FMT_RGB565:
			if ( dataSize < (uint32_t)numPixels * 2 ) {
				return false;
			}
			for ( int i = 0; i < numPixels; i++ ) {
				ColorFrom565( (uint16_t)( ( data[i * 2] << 8 ) | data[i * 2 + 1] ), rgba + i * 4 );	// big-endian
				rgba[i * 4 + 3] = 255;
			}
			break;
		case FMT_LUM8:
		case FMT_INT8:
		case FMT_ALPHA:
			if ( dataSize < (uint32_t)numPixels ) {
				return false;
			}
			for ( int i = 0; i < numPixels; i++ ) {
				byte * p = rgba + i * 4;
				const byte v = data[i];
				if ( format == FMT_LUM8 ) {
					p[0] = p[1] = p[2] = v;
					p[3] = 255;
				} else if ( format == FMT_INT8 ) {
					p[0] = p[1] = p[2] = p[3] = v;
				} else {
					p[0] = p[1] = p[2] = 255;
					p[3] = v;
				}
			}
			break;
		case FMT_L8A8:
			if ( dataSize < (uint32_t)numPixels * 2 ) {
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
		for ( int i = 0; i < numPixels; i++ ) {
			byte * p = rgba + i * 4;
			p[3] = p[1];
			p[0] = p[1] = p[2] = 255;
		}
	}
	return true;
}

/*
================================================================================================
	Resampling (as the engine: box halving, then an area filter for the rest)
================================================================================================
*/
static int PowerOfTwoBelow( int v ) {
	int p = 1;
	while ( p * 2 <= v ) {
		p *= 2;
	}
	return p;
}

// float images: 4 channels per pixel
typedef std::vector<float> fimage_t;

static void Halve( const fimage_t & src, int w, int h, bool hx, bool hy, fimage_t & dst ) {
	const int nw = hx ? w / 2 : w;
	const int nh = hy ? h / 2 : h;
	dst.assign( (size_t)nw * nh * 4, 0.0f );
	for ( int y = 0; y < nh; y++ ) {
		const int y0 = hy ? y * 2 : y, y1 = hy ? y0 + 1 : y0;
		for ( int x = 0; x < nw; x++ ) {
			const int x0 = hx ? x * 2 : x, x1 = hx ? x0 + 1 : x0;
			for ( int k = 0; k < 4; k++ ) {
				dst[( (size_t)y * nw + x ) * 4 + k] = 0.25f * ( src[( (size_t)y0 * w + x0 ) * 4 + k] + src[( (size_t)y0 * w + x1 ) * 4 + k] +
					src[( (size_t)y1 * w + x0 ) * 4 + k] + src[( (size_t)y1 * w + x1 ) * 4 + k] );
			}
		}
	}
}

// each target pixel: coverage-weighted average of the source pixels under it, one axis at a time
static void AreaResample1D( const fimage_t & src, int w, int h, int tw, int th, bool alongX, fimage_t & dst ) {
	const int srcLen = alongX ? w : h;
	const int dstLen = alongX ? tw : th;
	const int ow = alongX ? tw : w;
	const int oh = alongX ? h : th;
	dst.assign( (size_t)ow * oh * 4, 0.0f );
	for ( int t = 0; t < dstLen; t++ ) {
		const double a = (double)t * srcLen / dstLen;
		const double b = (double)( t + 1 ) * srcLen / dstLen;
		for ( int s = (int)a; s < srcLen && s < b; s++ ) {
			const double wgt = ( std::min( b, (double)s + 1.0 ) - std::max( a, (double)s ) ) / ( b - a );
			if ( wgt <= 0.0 ) {
				continue;
			}
			if ( alongX ) {
				for ( int y = 0; y < oh; y++ ) {
					for ( int k = 0; k < 4; k++ ) {
						dst[( (size_t)y * ow + t ) * 4 + k] += (float)wgt * src[( (size_t)y * w + s ) * 4 + k];
					}
				}
			} else {
				for ( int x = 0; x < ow; x++ ) {
					for ( int k = 0; k < 4; k++ ) {
						dst[( (size_t)t * ow + x ) * 4 + k] += (float)wgt * src[( (size_t)s * w + x ) * 4 + k];
					}
				}
			}
		}
	}
}

static void FitTo( fimage_t & img, int & w, int & h, int tw, int th ) {
	fimage_t tmp;
	while ( w >= tw * 2 || h >= th * 2 ) {
		const bool hx = w >= tw * 2, hy = h >= th * 2;
		Halve( img, w, h, hx, hy, tmp );
		w = hx ? w / 2 : w;
		h = hy ? h / 2 : h;
		img.swap( tmp );
	}
	if ( w != tw ) {
		AreaResample1D( img, w, h, tw, h, true, tmp );
		img.swap( tmp );
		w = tw;
	}
	if ( h != th ) {
		AreaResample1D( img, w, h, w, th, false, tmp );
		img.swap( tmp );
		h = th;
	}
}

/*
================================================================================================
	PICA200 tiling and encoders
================================================================================================
*/
static const byte mortonX[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
static const byte mortonY[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };

// texel index of image pixel (x, y) in the tiled level; the image is already flipped
static inline int TiledIndex( int x, int y, int w ) {
	return ( y & ~7 ) * w + ( x & ~7 ) * 8 + mortonX[x & 7] + mortonY[y & 7];
}

static inline byte Q( float v ) {		// 0..1 -> 0..255
	const int i = (int)std::lround( v * 255.0f );
	return (byte)( i < 0 ? 0 : ( i > 255 ? 255 : i ) );
}

static inline int Bits( float v, int bits ) {	// 0..1 -> 0..2^bits-1
	const int maxv = ( 1 << bits ) - 1;
	const int i = (int)std::lround( v * maxv );
	return i < 0 ? 0 : ( i > maxv ? maxv : i );
}

static void EncodeLevel( const fimage_t & img, int w, int h, int fmt, std::vector<byte> & out ) {
	const size_t base = out.size();
	out.resize( base + N3DS_PackLevelBytes( fmt, w, h, 0 ), 0 );
	byte * dst = &out[base];

	if ( fmt == N3DS_PACKFMT_ETC1 || fmt == N3DS_PACKFMT_ETC1A4 ) {
		rg_etc1::etc1_pack_params params;
		params.clear();
		params.m_quality = (rg_etc1::etc1_quality)opt.quality;
		const bool alpha = ( fmt == N3DS_PACKFMT_ETC1A4 );
		byte * o = dst;
		for ( int ty = 0; ty < h; ty += 8 ) {
			for ( int tx = 0; tx < w; tx += 8 ) {
				for ( int by = 0; by < 8; by += 4 ) {
					for ( int bx = 0; bx < 8; bx += 4 ) {
						uint32_t pixels[16];
						byte alphaBlock[8] = {};
						for ( int y = 0; y < 4; y++ ) {
							for ( int x = 0; x < 4; x++ ) {
								const float * p = &img[( (size_t)( ty + by + y ) * w + tx + bx + x ) * 4];
								pixels[y * 4 + x] = (uint32_t)Q( p[0] ) | ( (uint32_t)Q( p[1] ) << 8 ) | ( (uint32_t)Q( p[2] ) << 16 ) | 0xFF000000u;
								if ( alpha ) {
									// 4 bits per pixel, x and y swapped, even rows in the low nibble
									alphaBlock[2 * x + y / 2] |= (byte)( Bits( p[3], 4 ) << ( ( y & 1 ) ? 4 : 0 ) );
								}
							}
						}
						byte etc[8];
						rg_etc1::pack_etc1_block( etc, pixels, params );
						if ( alpha ) {
							memcpy( o, alphaBlock, 8 );
							o += 8;
						}
						for ( int i = 0; i < 8; i++ ) {		// big-endian block -> little-endian
							o[i] = etc[7 - i];
						}
						o += 8;
					}
				}
			}
		}
		return;
	}

	for ( int y = 0; y < h; y++ ) {
		for ( int x = 0; x < w; x++ ) {
			const float * p = &img[( (size_t)y * w + x ) * 4];
			const int t = TiledIndex( x, y, w );
			switch ( fmt ) {
				case N3DS_PACKFMT_RGB565: {
					const uint16_t v = (uint16_t)( ( Bits( p[0], 5 ) << 11 ) | ( Bits( p[1], 6 ) << 5 ) | Bits( p[2], 5 ) );
					dst[t * 2] = (byte)v;
					dst[t * 2 + 1] = (byte)( v >> 8 );
					break;
				}
				case N3DS_PACKFMT_RGBA5551: {
					const uint16_t v = (uint16_t)( ( Bits( p[0], 5 ) << 11 ) | ( Bits( p[1], 5 ) << 6 ) | ( Bits( p[2], 5 ) << 1 ) | ( p[3] >= 0.5f ? 1 : 0 ) );
					dst[t * 2] = (byte)v;
					dst[t * 2 + 1] = (byte)( v >> 8 );
					break;
				}
				case N3DS_PACKFMT_RGBA4: {
					const uint16_t v = (uint16_t)( ( Bits( p[0], 4 ) << 12 ) | ( Bits( p[1], 4 ) << 8 ) | ( Bits( p[2], 4 ) << 4 ) | Bits( p[3], 4 ) );
					dst[t * 2] = (byte)v;
					dst[t * 2 + 1] = (byte)( v >> 8 );
					break;
				}
				case N3DS_PACKFMT_HILO8:
					// the normal's x / y were put in red / green: lo byte green, hi byte red
					dst[t * 2] = Q( p[1] );
					dst[t * 2 + 1] = Q( p[0] );
					break;
				case N3DS_PACKFMT_A8:
					dst[t] = Q( p[3] );
					break;
				case N3DS_PACKFMT_A4:
					dst[t / 2] |= (byte)( Bits( p[3], 4 ) << ( ( t & 1 ) ? 4 : 0 ) );
					break;
				default:
					break;
			}
		}
	}
}

/*
================================================================================================
	Decoding the output again (preview images and the self test)
================================================================================================
*/
static void DecodeOutputLevel( const byte * src, int w, int h, int fmt, std::vector<byte> & rgba ) {
	rgba.assign( (size_t)w * h * 4, 255 );
	if ( fmt == N3DS_PACKFMT_ETC1 || fmt == N3DS_PACKFMT_ETC1A4 ) {
		const bool alpha = ( fmt == N3DS_PACKFMT_ETC1A4 );
		const byte * s = src;
		for ( int ty = 0; ty < h; ty += 8 ) {
			for ( int tx = 0; tx < w; tx += 8 ) {
				for ( int by = 0; by < 8; by += 4 ) {
					for ( int bx = 0; bx < 8; bx += 4 ) {
						byte alphaBlock[8] = {};
						if ( alpha ) {
							memcpy( alphaBlock, s, 8 );
							s += 8;
						}
						byte etc[8];
						for ( int i = 0; i < 8; i++ ) {
							etc[i] = s[7 - i];
						}
						s += 8;
						uint32_t pixels[16];
						rg_etc1::unpack_etc1_block( etc, pixels );
						for ( int y = 0; y < 4; y++ ) {
							for ( int x = 0; x < 4; x++ ) {
								byte * d = &rgba[( (size_t)( ty + by + y ) * w + tx + bx + x ) * 4];
								memcpy( d, &pixels[y * 4 + x], 3 );
								if ( alpha ) {
									const int a = ( alphaBlock[2 * x + y / 2] >> ( ( y & 1 ) ? 4 : 0 ) ) & 15;
									d[3] = (byte)( a * 17 );
								}
							}
						}
					}
				}
			}
		}
		return;
	}
	for ( int y = 0; y < h; y++ ) {
		for ( int x = 0; x < w; x++ ) {
			const int t = TiledIndex( x, y, w );
			byte * d = &rgba[( (size_t)y * w + x ) * 4];
			const uint16_t v = ( N3DS_PackBitsPerPixel( fmt ) == 16 ) ? (uint16_t)( src[t * 2] | ( src[t * 2 + 1] << 8 ) ) : 0;
			switch ( fmt ) {
				case N3DS_PACKFMT_RGB565:
					d[0] = (byte)( ( ( v >> 11 ) & 31 ) * 255 / 31 );
					d[1] = (byte)( ( ( v >> 5 ) & 63 ) * 255 / 63 );
					d[2] = (byte)( ( v & 31 ) * 255 / 31 );
					break;
				case N3DS_PACKFMT_RGBA5551:
					d[0] = (byte)( ( ( v >> 11 ) & 31 ) * 255 / 31 );
					d[1] = (byte)( ( ( v >> 6 ) & 31 ) * 255 / 31 );
					d[2] = (byte)( ( ( v >> 1 ) & 31 ) * 255 / 31 );
					d[3] = ( v & 1 ) ? 255 : 0;
					break;
				case N3DS_PACKFMT_RGBA4:
					d[0] = (byte)( ( ( v >> 12 ) & 15 ) * 17 );
					d[1] = (byte)( ( ( v >> 8 ) & 15 ) * 17 );
					d[2] = (byte)( ( ( v >> 4 ) & 15 ) * 17 );
					d[3] = (byte)( ( v & 15 ) * 17 );
					break;
				case N3DS_PACKFMT_HILO8:
					d[0] = (byte)( v >> 8 );
					d[1] = (byte)( v & 255 );
					d[2] = 0;
					break;
				case N3DS_PACKFMT_A8:
					d[0] = d[1] = d[2] = 255;
					d[3] = src[t];
					break;
				case N3DS_PACKFMT_A4:
					d[0] = d[1] = d[2] = 255;
					d[3] = (byte)( ( ( src[t / 2] >> ( ( t & 1 ) ? 4 : 0 ) ) & 15 ) * 17 );
					break;
				default:
					break;
			}
		}
	}
}

static bool WriteTGA( const std::string & path, const std::vector<byte> & rgba, int w, int h, bool flipBack ) {
	FILE * f = fopen( path.c_str(), "wb" );
	if ( f == NULL ) {
		return false;
	}
	byte head[18] = {};
	head[2] = 2;
	head[12] = (byte)w;
	head[13] = (byte)( w >> 8 );
	head[14] = (byte)h;
	head[15] = (byte)( h >> 8 );
	head[16] = 32;
	head[17] = 0x28;		// top-left origin, 8 alpha bits
	fwrite( head, 1, 18, f );
	for ( int y = 0; y < h; y++ ) {
		const int sy = flipBack ? ( h - 1 - y ) : y;
		for ( int x = 0; x < w; x++ ) {
			const byte * p = &rgba[( (size_t)sy * w + x ) * 4];
			const byte bgra[4] = { p[2], p[1], p[0], p[3] };
			fwrite( bgra, 1, 4, f );
		}
	}
	fclose( f );
	return true;
}

/*
================================================================================================
	Converting one image
================================================================================================
*/
enum kind_t { KIND_COLOR, KIND_GUI, KIND_NORMAL, KIND_MASK, KIND_FONT, KIND_SKIP };

struct result_t {
	bool				ok = false;
	std::string			skipReason;
	n3dsPackEntry_t		entry = {};
	std::vector<byte>	data;
	int					srcLevelW = 0;
	int					srcLevelH = 0;
};

static int UsageFromName( const std::string & name, int & cube ) {
	cube = 0;
	const size_t p = name.rfind( "#__" );
	if ( p == std::string::npos || p + 7 > name.size() ) {
		return TD_DEFAULT;
	}
	const char * s = name.c_str() + p + 3;
	for ( int i = 0; i < 4; i++ ) {
		if ( s[i] < '0' || s[i] > '9' ) {
			return TD_DEFAULT;
		}
	}
	cube = ( s[2] - '0' ) * 10 + ( s[3] - '0' );
	return ( s[0] - '0' ) * 10 + ( s[1] - '0' );
}

static bool IsGuiName( const std::string & name ) {
	const std::string n = name.substr( 17 );		// after "generated/images/"
	return n.compare( 0, 5, "guis/" ) == 0 || n.compare( 0, 14, "generated/swf/" ) == 0;
}

static void ConvertImage( const imageSource_t & src, FILE * f, result_t & res ) {
	std::vector<byte> file( src.length );
	if ( src.length < 40 || !ReadAt( f, src.offset, file.data(), src.length ) ) {
		res.skipReason = "cannot read";
		return;
	}
	const byte * p = file.data();
	const int64_t sourceFileTime = BE64( p );
	const uint32_t magic = BE32( p + 8 );
	const int textureType = (int)BE32( p + 12 );
	const int format = (int)BE32( p + 16 );
	const int colorFormat = (int)BE32( p + 20 );
	const int width = (int)BE32( p + 24 );
	const int height = (int)BE32( p + 28 );
	const int numLevels = (int)BE32( p + 32 );
	if ( magic != BIMAGE_MAGIC ) {
		res.skipReason = "not a bimage";
		return;
	}
	if ( textureType != TT_2D ) {
		res.skipReason = "cube map";
		return;
	}
	int cube = 0;
	const int usage = UsageFromName( src.name, cube );

	kind_t kind = KIND_COLOR;
	int maxSize = opt.maxWorld;
	if ( usage == TD_BUMP ) {
		kind = KIND_NORMAL;
		maxSize = opt.maxBump;
	} else if ( usage == TD_COVERAGE ) {
		kind = KIND_MASK;
	} else if ( usage == TD_FONT ) {
		kind = KIND_FONT;
		maxSize = opt.maxFont;
	} else if ( usage == TD_DEPTH ) {
		kind = KIND_SKIP;
	} else if ( usage >= TD_LOOKUP_TABLE_MONO && usage <= TD_LOOKUP_TABLE_RGBA ) {
		kind = KIND_GUI;
		maxSize = 256;
	} else if ( IsGuiName( src.name ) ) {
		kind = KIND_GUI;
		maxSize = opt.maxGui;
	} else if ( usage == TD_SPECULAR ) {
		maxSize = opt.maxSpecular;
	} else if ( usage == TD_LIGHT ) {
		maxSize = opt.maxLight;
	}
	if ( kind == KIND_SKIP ) {
		res.skipReason = "depth image";
		return;
	}

	// target size: powers of two, at most maxSize, at least 8
	int tw = PowerOfTwoBelow( std::max( width, 1 ) );
	int th = PowerOfTwoBelow( std::max( height, 1 ) );
	while ( tw > maxSize || th > maxSize ) {
		tw = std::max( tw >> 1, 1 );
		th = std::max( th >> 1, 1 );
	}
	tw = std::max( tw, 8 );
	th = std::max( th, 8 );

	// the smallest stored level that still covers the target (its own mips are id's)
	const byte * levelData = NULL;
	uint32_t levelSize = 0;
	int lw = 0, lh = 0;
	const byte * q = p + 36;
	const byte * end = p + src.length;
	for ( int i = 0; i < numLevels; i++ ) {
		if ( end - q < 20 ) {
			break;
		}
		const int w = (int)BE32( q + 8 );
		const int h = (int)BE32( q + 12 );
		const uint32_t size = BE32( q + 16 );
		q += 20;
		if ( (uint32_t)( end - q ) < size ) {
			break;
		}
		if ( levelData == NULL || ( w >= std::min( tw, width ) && h >= std::min( th, height ) ) ) {
			levelData = q;
			levelSize = size;
			lw = w;
			lh = h;
		}
		q += size;
	}
	if ( levelData == NULL ) {
		res.skipReason = "no levels";
		return;
	}
	std::vector<byte> rgba( (size_t)lw * lh * 4 );
	if ( !DecodeLevel( format, colorFormat, lw, lh, levelData, levelSize, rgba.data() ) ) {
		char why[96];
		snprintf( why, sizeof( why ), "format %d / colour format %d not handled", format, colorFormat );
		res.skipReason = why;
		return;
	}
	res.srcLevelW = lw;
	res.srcLevelH = lh;

	// to float, flipped vertically (the PICA samples t = 0 at the last row)
	fimage_t img( (size_t)lw * lh * 4 );
	for ( int y = 0; y < lh; y++ ) {
		const byte * row = &rgba[(size_t)( lh - 1 - y ) * lw * 4];
		for ( int i = 0; i < lw * 4; i++ ) {
			img[(size_t)y * lw * 4 + i] = row[i] * ( 1.0f / 255.0f );
		}
	}
	if ( kind == KIND_NORMAL ) {
		// normal in red / green as -1..1 vector components (DXT5 normal maps keep x in alpha)
		for ( size_t i = 0; i < img.size(); i += 4 ) {
			const float x = ( colorFormat == CFM_NORMAL_DXT5 ) ? img[i + 3] : img[i + 0];
			const float y = img[i + 1];
			img[i + 0] = x * 2.0f - 1.0f;
			img[i + 1] = y * 2.0f - 1.0f;
			img[i + 2] = sqrtf( std::max( 0.0f, 1.0f - img[i + 0] * img[i + 0] - img[i + 1] * img[i + 1] ) );
			img[i + 3] = 1.0f;
		}
	}

	int w = lw, h = lh;
	FitTo( img, w, h, tw, th );

	// GPU format
	bool hasAlpha = false, binaryAlpha = true;
	for ( size_t i = 3; i < img.size(); i += 4 ) {
		if ( img[i] < 0.98f ) {
			hasAlpha = true;
		}
		if ( img[i] > 0.035f && img[i] < 0.965f ) {
			binaryAlpha = false;
		}
	}
	int fmt;
	uint8_t flags = 0;
	switch ( kind ) {
		case KIND_NORMAL:	fmt = N3DS_PACKFMT_HILO8; flags = N3DS_PACKFLAG_NORMALMAP; break;
		case KIND_MASK:		fmt = N3DS_PACKFMT_A4; flags = N3DS_PACKFLAG_ALPHA | N3DS_PACKFLAG_ALPHAONLY; break;
		case KIND_FONT:		fmt = N3DS_PACKFMT_A8; flags = N3DS_PACKFLAG_ALPHA | N3DS_PACKFLAG_ALPHAONLY; break;
		case KIND_GUI:
			fmt = !hasAlpha ? N3DS_PACKFMT_RGB565 : ( binaryAlpha ? N3DS_PACKFMT_RGBA5551 : N3DS_PACKFMT_RGBA4 );
			flags = hasAlpha ? N3DS_PACKFLAG_ALPHA : 0;
			break;
		default:
			fmt = hasAlpha ? N3DS_PACKFMT_ETC1A4 : N3DS_PACKFMT_ETC1;
			flags = hasAlpha ? N3DS_PACKFLAG_ALPHA : 0;
			break;
	}

	// mip chain down to 8 on the short side
	int maxLevel = 0;
	for ( int m = std::min( w, h ); m > 8; m >>= 1 ) {
		maxLevel++;
	}
	for ( int level = 0; level <= maxLevel; level++ ) {
		if ( kind == KIND_NORMAL ) {
			// stored as 0..1 in red / green; the average of normals is renormalized per level
			fimage_t enc = img;
			for ( size_t i = 0; i < enc.size(); i += 4 ) {
				float x = img[i], y = img[i + 1], z = img[i + 2];
				const float len = sqrtf( x * x + y * y + z * z );
				if ( len > 1e-6f ) {
					x /= len;
					y /= len;
				}
				enc[i + 0] = x * 0.5f + 0.5f;
				enc[i + 1] = y * 0.5f + 0.5f;
			}
			EncodeLevel( enc, w, h, fmt, res.data );
		} else {
			EncodeLevel( img, w, h, fmt, res.data );
		}
		if ( level < maxLevel ) {
			fimage_t half;
			Halve( img, w, h, true, true, half );
			img.swap( half );
			w /= 2;
			h /= 2;
		}
	}

	n3dsPackEntry_t & e = res.entry;
	e.nameHash = N3DS_PackHash( src.name.c_str() );
	e.dataSize = (uint32_t)res.data.size();
	e.sourceFileTime = sourceFileTime;
	e.width = (uint16_t)tw;
	e.height = (uint16_t)th;
	e.srcWidth = (uint16_t)std::min( width, 65535 );
	e.srcHeight = (uint16_t)std::min( height, 65535 );
	e.gpuFormat = (uint8_t)fmt;
	e.maxLevel = (uint8_t)maxLevel;
	e.srcFormat = (uint8_t)format;
	e.srcColorFormat = (uint8_t)colorFormat;
	e.srcNumLevels = (uint8_t)std::min( numLevels, 255 );
	e.flags = flags;
	e.usage = (uint8_t)usage;
	res.ok = true;
}

/*
================================================================================================
	Main
================================================================================================
*/
static const char * FormatName( int fmt ) {
	switch ( fmt ) {
		case N3DS_PACKFMT_ETC1:		return "ETC1";
		case N3DS_PACKFMT_ETC1A4:	return "ETC1A4";
		case N3DS_PACKFMT_HILO8:	return "HILO8";
		case N3DS_PACKFMT_A4:		return "A4";
		case N3DS_PACKFMT_A8:		return "A8";
		case N3DS_PACKFMT_RGB565:	return "RGB565";
		case N3DS_PACKFMT_RGBA5551:	return "RGBA5551";
		case N3DS_PACKFMT_RGBA4:	return "RGBA4";
		default:					return "?";
	}
}

static void Usage() {
	printf(
		"n3dstex - DOOM 3 BFG textures -> New 3DS texture pack\n\n"
		"usage: n3dstex <doom3 base folder> [options]\n"
		"  the folder holding _common.resources and maps/ (the PC game's base, or the SD card's doom3/base)\n\n"
		"options:\n"
		"  -o <file>      output (default: <base>/_n3ds_textures.pak)\n"
		"  -world <n>     largest world / diffuse texture (default 256)\n"
		"  -bump <n>      largest normal map (default 128)\n"
		"  -spec <n>      largest specular map (default 128)\n"
		"  -light <n>     largest light image (default 128)\n"
		"  -gui <n>       largest GUI image (default 512)\n"
		"  -font <n>      largest font atlas (default 1024)\n"
		"  -q <0|1|2>     ETC1 quality: low, medium (default), high\n"
		"  -j <n>         threads (default: all cores)\n"
		"  -only <text>   convert only images whose name contains <text>\n"
		"  -preview <dir> with -only: also write each result's level 0 as .tga\n"
		"  -list          list the images found, convert nothing\n"
		"  -v             log every image\n" );
}

static bool ParseArgs( int argc, char ** argv ) {
	if ( argc < 2 ) {
		return false;
	}
	for ( int i = 1; i < argc; i++ ) {
		const std::string a = argv[i];
		auto next = [&]( int & v ) {
			if ( i + 1 >= argc ) {
				return false;
			}
			v = atoi( argv[++i] );
			return true;
		};
		if ( a == "-h" || a == "--help" ) {
			return false;
		} else if ( a == "-o" && i + 1 < argc ) {
			opt.output = argv[++i];
		} else if ( a == "-world" ) {
			if ( !next( opt.maxWorld ) ) return false;
		} else if ( a == "-bump" ) {
			if ( !next( opt.maxBump ) ) return false;
		} else if ( a == "-spec" ) {
			if ( !next( opt.maxSpecular ) ) return false;
		} else if ( a == "-light" ) {
			if ( !next( opt.maxLight ) ) return false;
		} else if ( a == "-gui" ) {
			if ( !next( opt.maxGui ) ) return false;
		} else if ( a == "-font" ) {
			if ( !next( opt.maxFont ) ) return false;
		} else if ( a == "-q" ) {
			if ( !next( opt.quality ) ) return false;
		} else if ( a == "-j" ) {
			if ( !next( opt.threads ) ) return false;
		} else if ( a == "-only" && i + 1 < argc ) {
			opt.only = argv[++i];
			std::transform( opt.only.begin(), opt.only.end(), opt.only.begin(), ::tolower );
		} else if ( a == "-preview" && i + 1 < argc ) {
			opt.previewDir = argv[++i];
		} else if ( a == "-list" ) {
			opt.list = true;
		} else if ( a == "-v" ) {
			opt.verbose = true;
		} else if ( a[0] == '-' ) {
			printf( "unknown option %s\n\n", a.c_str() );
			return false;
		} else {
			opt.baseDir = a;
		}
	}
	auto pow2 = []( int & v, int lo, int hi ) {
		v = std::max( lo, std::min( hi, PowerOfTwoBelow( std::max( v, 1 ) ) ) );
	};
	pow2( opt.maxWorld, 8, 1024 );
	pow2( opt.maxBump, 8, 1024 );
	pow2( opt.maxSpecular, 8, 1024 );
	pow2( opt.maxLight, 8, 1024 );
	pow2( opt.maxGui, 8, 1024 );
	pow2( opt.maxFont, 8, 1024 );
	opt.quality = std::max( 0, std::min( 2, opt.quality ) );
	if ( opt.output.empty() ) {
		opt.output = ( fs::path( opt.baseDir ) / "_n3ds_textures.pak" ).string();
	}
	return !opt.baseDir.empty();
}

int main( int argc, char ** argv ) {
	if ( !ParseArgs( argc, argv ) ) {
		Usage();
		return 1;
	}
	const auto startTime = std::chrono::steady_clock::now();
	Log( "n3dstex: %s -> %s\n", opt.baseDir.c_str(), opt.output.c_str() );
	Log( "sizes: world %d, bump %d, specular %d, light %d, gui %d, font %d; ETC1 quality %d\n",
		opt.maxWorld, opt.maxBump, opt.maxSpecular, opt.maxLight, opt.maxGui, opt.maxFont, opt.quality );

	FindContainers();
	if ( containers.empty() ) {
		Log( "no .resources files under %s\n", opt.baseDir.c_str() );
		return 1;
	}
	std::map<std::string, int> seen;
	for ( size_t i = 0; i < containers.size(); i++ ) {
		int added = 0, total = 0;
		if ( ReadContainerTable( containers[i], (int)i, seen, added, total ) && ( added > 0 || opt.verbose ) ) {
			Log( "  %s: %d images, %d new\n", containers[i].c_str(), total, added );
		}
	}
	if ( !opt.only.empty() ) {
		std::vector<imageSource_t> filtered;
		for ( const imageSource_t & s : sources ) {
			if ( s.name.find( opt.only ) != std::string::npos ) {
				filtered.push_back( s );
			}
		}
		sources.swap( filtered );
	}
	Log( "%zu resource files, %zu images to convert\n", containers.size(), sources.size() );
	if ( opt.list ) {
		for ( const imageSource_t & s : sources ) {
			Log( "%s (%u bytes in %s)\n", s.name.c_str(), s.length, containers[s.container].c_str() );
		}
		return 0;
	}
	if ( sources.empty() ) {
		return 1;
	}

	FILE * out = fopen( opt.output.c_str(), "wb" );
	if ( out == NULL ) {
		Log( "cannot write %s\n", opt.output.c_str() );
		return 1;
	}
	n3dsPackHeader_t header = {};
	fwrite( &header, sizeof( header ), 1, out );
	uint64_t writePos = sizeof( header );
	auto padTo = [&]( uint64_t align ) {
		static const byte zeros[N3DS_PACK_ALIGN] = {};
		const uint64_t pad = ( align - writePos % align ) % align;
		fwrite( zeros, 1, (size_t)pad, out );
		writePos += pad;
	};

	rg_etc1::pack_etc1_block_init();

	std::vector<n3dsPackEntry_t> entries;
	std::mutex outLock;
	std::atomic<size_t> next( 0 );
	std::atomic<int> done( 0 ), skipped( 0 );
	std::map<int, std::pair<int, uint64_t>> perFormat;		// count, bytes
	std::map<std::string, int> skipReasons;
	bool writeFailed = false;

	int numThreads = opt.threads > 0 ? opt.threads : (int)std::thread::hardware_concurrency();
	numThreads = std::max( 1, std::min( numThreads, 64 ) );
	Log( "converting with %d threads...\n", numThreads );

	auto worker = [&]() {
		std::vector<FILE *> files( containers.size(), (FILE *)NULL );
		for ( ;; ) {
			const size_t i = next++;
			if ( i >= sources.size() ) {
				break;
			}
			const imageSource_t & src = sources[i];
			FILE *& f = files[src.container];
			if ( f == NULL ) {
				f = fopen( containers[src.container].c_str(), "rb" );
			}
			result_t res;
			if ( f != NULL ) {
				ConvertImage( src, f, res );
			} else {
				res.skipReason = "cannot open container";
			}
			std::lock_guard<std::mutex> lock( outLock );
			if ( !res.ok ) {
				skipped++;
				skipReasons[res.skipReason]++;
				if ( opt.verbose ) {
					Log( "  skip %s: %s\n", src.name.c_str(), res.skipReason.c_str() );
				}
			} else {
				padTo( N3DS_PACK_ALIGN );
				res.entry.dataOffset = (uint32_t)( writePos / N3DS_PACK_ALIGN );
				if ( fwrite( res.data.data(), 1, res.data.size(), out ) != res.data.size() ) {
					writeFailed = true;
				}
				writePos += res.data.size();
				entries.push_back( res.entry );
				perFormat[res.entry.gpuFormat].first++;
				perFormat[res.entry.gpuFormat].second += res.data.size();
				if ( opt.verbose ) {
					Log( "  %s: %dx%d (from %dx%d of %dx%d) %s, %d levels, %zu bytes\n", src.name.c_str(), res.entry.width, res.entry.height,
						res.srcLevelW, res.srcLevelH, res.entry.srcWidth, res.entry.srcHeight, FormatName( res.entry.gpuFormat ),
						res.entry.maxLevel + 1, res.data.size() );
				}
				if ( !opt.previewDir.empty() ) {
					std::vector<byte> rgba;
					DecodeOutputLevel( res.data.data(), res.entry.width, res.entry.height, res.entry.gpuFormat, rgba );
					std::string fileName = src.name.substr( 17 );
					std::replace( fileName.begin(), fileName.end(), '/', '_' );
					std::replace( fileName.begin(), fileName.end(), '#', '_' );
					fs::create_directories( opt.previewDir );
					WriteTGA( ( fs::path( opt.previewDir ) / ( fileName + ".tga" ) ).string(), rgba, res.entry.width, res.entry.height, true );
				}
			}
			const int d = ++done;
			if ( d % 500 == 0 ) {
				Log( "  %d / %zu\n", d, sources.size() );
			}
		}
		for ( FILE * f : files ) {
			if ( f != NULL ) {
				fclose( f );
			}
		}
	};
	std::vector<std::thread> threads;
	for ( int t = 0; t < numThreads; t++ ) {
		threads.emplace_back( worker );
	}
	for ( std::thread & t : threads ) {
		t.join();
	}

	// entry table, sorted for the binary search on the console
	std::sort( entries.begin(), entries.end(), []( const n3dsPackEntry_t & a, const n3dsPackEntry_t & b ) {
		return a.nameHash < b.nameHash;
	} );
	for ( size_t i = 1; i < entries.size(); i++ ) {
		if ( entries[i].nameHash == entries[i - 1].nameHash ) {
			Log( "WARNING: name hash collision (%016llx): one of two images will be drawn with the other's texture\n",
				(unsigned long long)entries[i].nameHash );
		}
	}
	padTo( 16 );
	header.magic = N3DS_PACK_MAGIC;
	header.version = N3DS_PACK_VERSION;
	header.numEntries = (uint32_t)entries.size();
	header.entryOffset = (uint32_t)writePos;
	header.maxWorld = (uint16_t)opt.maxWorld;
	header.maxBump = (uint16_t)opt.maxBump;
	header.maxSpecular = (uint16_t)opt.maxSpecular;
	header.maxGui = (uint16_t)opt.maxGui;
	if ( !entries.empty() && fwrite( entries.data(), sizeof( n3dsPackEntry_t ), entries.size(), out ) != entries.size() ) {
		writeFailed = true;
	}
	writePos += entries.size() * sizeof( n3dsPackEntry_t );
	fseek( out, 0, SEEK_SET );
	fwrite( &header, sizeof( header ), 1, out );
	// the console seeks with a 32-bit long
	if ( fclose( out ) != 0 || writeFailed || writePos > 0x7FFFFFFFull ) {
		Log( "ERROR: writing %s failed%s\n", opt.output.c_str(), writePos > 0x7FFFFFFFull ? ": the pack is over 2 GB, use smaller sizes" : " (disk full?)" );
		return 1;
	}

	const double secs = std::chrono::duration<double>( std::chrono::steady_clock::now() - startTime ).count();
	Log( "\n%zu textures, %d skipped, %.1f MB, %.0f s\n", entries.size(), skipped.load(), writePos / ( 1024.0 * 1024.0 ), secs );
	for ( const auto & f : perFormat ) {
		Log( "  %-8s %6d textures %8.1f MB\n", FormatName( f.first ), f.second.first, f.second.second / ( 1024.0 * 1024.0 ) );
	}
	for ( const auto & s : skipReasons ) {
		Log( "  skipped (%s): %d\n", s.first.c_str(), s.second );
	}
	Log( "copy %s to sdmc:/doom3/base/ on the SD card\n", fs::path( opt.output ).filename().string().c_str() );
	return 0;
}
