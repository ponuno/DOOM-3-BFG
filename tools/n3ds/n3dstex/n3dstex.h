/*
	n3dstex: shared helpers of the texture (n3dstex.cpp) and sound (n3dssnd.cpp) converters.
*/
#ifndef __N3DSTEX_H__
#define __N3DSTEX_H__

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

typedef uint8_t byte;

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
	bool		doTextures = true;
	bool		doSounds = true;
	std::string	soundOutput;
};
extern options_t opt;

void Log( const char * fmt, ... );

// 64-bit file positions (a long is 32 bits on Windows)
#if defined( _WIN32 )
#define FSeek64( f, o, w )	_fseeki64( f, o, w )
#define FTell64( f )		_ftelli64( f )
#else
#define FSeek64( f, o, w )	fseeko( f, (off_t)( o ), w )
#define FTell64( f )		( (int64_t)ftello( f ) )
#endif

static inline uint32_t BE32( const byte * p ) {
	return ( (uint32_t)p[0] << 24 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 8 ) | p[3];
}
static inline int64_t BE64( const byte * p ) {
	return (int64_t)( ( (uint64_t)BE32( p ) << 32 ) | BE32( p + 4 ) );
}
static inline uint32_t LE32( const byte * p ) {
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}
static inline uint16_t LE16( const byte * p ) {
	return (uint16_t)( p[0] | ( p[1] << 8 ) );
}

bool ReadAt( FILE * f, uint32_t offset, void * dst, uint32_t size );

// every entry of a resource container's table (name lower case, forward slashes)
struct resourceEntry_t {
	std::string	name;
	uint32_t	offset;
	uint32_t	length;
};
bool ReadResourceTable( const std::string & path, std::vector<resourceEntry_t> & entries );

// sound pack (n3dssnd.cpp): returns the process exit code
int ConvertSounds( const std::vector<std::string> & containers );

#endif
