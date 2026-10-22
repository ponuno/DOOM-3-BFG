/*
	n3dstex: shared helpers of the texture (n3dstex.cpp) and sound (n3dssnd.cpp) converters.
*/
#ifndef __N3DSTEX_H__
#define __N3DSTEX_H__

#include <atomic>
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
	bool		doVideos = true;
	int			videoSize = 512;
	int			videoFps = 20;
	int			videoQuality = 0;
	std::string	slimDir;			// -slim: write slimmed .resources there, convert nothing
	// where results go: <outDir>/doom3/base/... (the SD card layout); default
	// "outputDoom3" next to the base folder
	std::string	outDir;
	bool		sdFull = false;		// -sd: also the whole SD base folder (slim .resources + the rest)
};
extern options_t opt;

void Log( const char * fmt, ... );

// front ends (command line, Windows window): log lines and progress go there too
enum { STAGE_TEXTURES, STAGE_SOUNDS, STAGE_VIDEOS, STAGE_SDCOPY, NUM_STAGES };
extern void ( *logSink )( const char * text );
extern void ( *progressSink )( int stage, double fraction, const char * text );
void Progress( int stage, double fraction, const char * fmt, ... );
// set by a front end: converters stop at the next item and delete what they were writing
extern std::atomic<bool> cancelRequested;

// <base folder>, or <dir>/base when <dir> holds the game (both checked for _common.resources)
std::string DetectBaseDir( const std::string & dir );
// fills the output paths from baseDir / outDir; false without a base folder
bool ResolveOptions();
// <outDir>/doom3/base
std::string OutBaseDir();
// true if p is inside the output folder (skipped when the base folder is scanned)
bool IsInOutput( const std::string & p );
// the whole conversion as the options say: 0 ok, 1 errors, 2 cancelled
int RunConverter();

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

// float RGBA images (n3dstex.cpp): box / area resampling, PICA encoders
typedef std::vector<float> fimage_t;
void FitTo( fimage_t & img, int & w, int & h, int tw, int th );
void EncodeLevel( const fimage_t & img, int w, int h, int fmt, std::vector<byte> & out, int etc1Quality );

// every entry of a resource container's table (name lower case, forward slashes)
struct resourceEntry_t {
	std::string	name;
	uint32_t	offset;
	uint32_t	length;
};
bool ReadResourceTable( const std::string & path, std::vector<resourceEntry_t> & entries );

// sound pack (n3dssnd.cpp): returns the process exit code
int ConvertSounds( const std::vector<std::string> & containers );
// slim copy of the base folder (n3dsslim.cpp)
int SlimResources( const std::vector<std::string> & containers );
// videos (n3dsvid.cpp): base/.../*.bik -> <out base>/.../*.n3v
int ConvertVideos();

#endif
