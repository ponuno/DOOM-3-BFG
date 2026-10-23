/*
================================================================================================

	n3dstex video part: Bink videos (base/.../NAME.bik) -> <out>/doom3/base/.../NAME.n3v

	Each .bik is decoded with libbinkdec (LGPL, Barry Duncan, from FFmpeg's Bink decoder),
	converted from BT.601 YCbCr like the game's bink.pixel, resampled to a power-of-two
	texture (-videosize, the long side; aspect kept to the nearest power of two, the
	material stretches it back), taken at -videofps, flipped and ETC1-encoded like every
	texture, and stored as block changes to the previous frame (format:
	neo/renderer/3DS/n3ds_video.h). The console only copies frames to the GPU.

	The decoder runs one video at a time (libbinkdec keeps global state); frames are
	ETC1-encoded on all threads in batches.

================================================================================================
*/
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#include "n3dstex.h"
#include "rg_etc1.h"
#include "libbinkdec/include/BinkDecoder.h"
#include "../../../neo/renderer/3DS/n3ds_pack.h"
#include "../../../neo/renderer/3DS/n3ds_video.h"

namespace fs = std::filesystem;

static int PowerOfTwoNearest( double v ) {
	int p = 8;
	while ( p < 1024 && p * 1.41421356 < v ) {
		p *= 2;
	}
	return p;
}

static int PowerOfTwoBelowV( int v ) {
	int p = 1;
	while ( p * 2 <= v ) {
		p *= 2;
	}
	return p;
}

static inline float Clamp01( float v ) {
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

// YCbCr planes (Y, Cb, Cr, as libbinkdec gives them) -> float RGBA, BT.601 as bink.pixel
static void YuvToRgb( YUVbuffer yuv, int width, int height, bool gray, fimage_t & out ) {
	out.resize( (size_t)width * height * 4 );
	for ( int y = 0; y < height; y++ ) {
		const uint8_t * py = yuv[0].data + (size_t)y * yuv[0].pitch;
		const uint8_t * pb = gray ? NULL : yuv[1].data + (size_t)( y >> 1 ) * yuv[1].pitch;
		const uint8_t * pr = gray ? NULL : yuv[2].data + (size_t)( y >> 1 ) * yuv[2].pitch;
		for ( int x = 0; x < width; x++ ) {
			const float Y = py[x] / 255.0f;
			const float Cb = gray ? 0.5f : pb[x >> 1] / 255.0f;
			const float Cr = gray ? 0.5f : pr[x >> 1] / 255.0f;
			float * o = &out[( (size_t)y * width + x ) * 4];
			o[0] = Clamp01( 1.164123535f * Y + 1.595794678f * Cr - 0.87065506f );
			o[1] = Clamp01( 1.164123535f * Y - 0.813476563f * Cr - 0.391448975f * Cb + 0.529705048f );
			o[2] = Clamp01( 1.164123535f * Y + 2.017822266f * Cb - 1.081668854f );
			o[3] = 1.0f;
		}
	}
}

static void PutLE32( std::vector<byte> & v, uint32_t x ) {
	for ( int i = 0; i < 4; i++ ) {
		v.push_back( (byte)( x >> ( 8 * i ) ) );
	}
}

static bool ConvertVideo( const fs::path & bik, const fs::path & outPath, uint64_t & bytesOut, double & seconds, std::string & why,
		int videoIndex, int numVideos ) {
	BinkDecoder dec;
	if ( !dec.Open( bik.string() ) ) {
		why = "not a Bink 1 video libbinkdec can read";
		return false;
	}
	const int srcW = (int)dec.frameWidth;
	const int srcH = (int)dec.frameHeight;
	const uint32_t srcFrames = dec.GetNumFrames();
	const double srcFps = dec.GetFrameRate();
	if ( srcW <= 0 || srcH <= 0 || srcFrames == 0 || srcFps <= 0.0 ) {
		why = "empty video";
		return false;
	}
	// texture size: the long side at most -videosize, the other side the nearest power of two
	int tw, th;
	if ( srcW >= srcH ) {
		tw = std::min( opt.videoSize, PowerOfTwoBelowV( srcW ) );
		th = std::min( tw, PowerOfTwoNearest( (double)tw * srcH / srcW ) );
	} else {
		th = std::min( opt.videoSize, PowerOfTwoBelowV( srcH ) );
		tw = std::min( th, PowerOfTwoNearest( (double)th * srcW / srcH ) );
	}
	tw = std::max( tw, 8 );
	th = std::max( th, 8 );
	const double fps = std::min( (double)opt.videoFps, srcFps );
	const uint32_t numFrames = std::max<uint32_t>( 1, (uint32_t)floor( srcFrames / srcFps * fps ) );
	const uint32_t frameBytes = (uint32_t)tw * th / 2;
	const uint32_t numBlocks = frameBytes / 8;
	const uint32_t keyInterval = std::max<uint32_t>( 1, (uint32_t)lround( fps * 2.0 ) );

	FILE * out = fopen( outPath.string().c_str(), "wb" );
	if ( out == NULL ) {
		why = "cannot write " + outPath.string();
		return false;
	}
	n3dsVideoHeader_t header = {};
	fwrite( &header, sizeof( header ), 1, out );
	uint64_t pos = sizeof( header );
	std::vector<n3dsVideoFrame_t> index( numFrames );
	std::vector<byte> previous;

	int numThreads = opt.threads > 0 ? opt.threads : (int)std::thread::hardware_concurrency();
	numThreads = std::max( 1, std::min( numThreads, 64 ) );
	const uint32_t batch = (uint32_t)numThreads * 2;
	uint32_t srcDecoded = 0;		// source frames decoded so far
	fimage_t lastRgb;
	bool gray = false;
	for ( uint32_t first = 0; first < numFrames; first += batch ) {
		if ( cancelRequested ) {
			fclose( out );
			fs::remove( outPath );
			why = "cancelled";
			return false;
		}
		Progress( STAGE_VIDEOS, ( videoIndex + (double)first / numFrames ) / numVideos, "%s (%d / %d)",
			bik.filename().string().c_str(), videoIndex + 1, numVideos );
		const uint32_t count = std::min( batch, numFrames - first );
		std::vector<fimage_t> images( count );
		// decode up to the source frame each output frame shows (Bink is predictive:
		// every frame has to be decoded)
		for ( uint32_t k = 0; k < count; k++ ) {
			const uint32_t want = std::min( srcFrames - 1, (uint32_t)floor( ( first + k ) / fps * srcFps ) );
			while ( srcDecoded <= want ) {
				YUVbuffer yuv = {};
				dec.GetNextFrame( yuv );
				gray = ( yuv[1].data == NULL || yuv[2].data == NULL );
				YuvToRgb( yuv, srcW, srcH, gray, lastRgb );
				srcDecoded++;
			}
			images[k] = lastRgb;
		}
		// resample, flip, ETC1: in parallel
		std::vector<std::vector<byte>> encoded( count );
		std::atomic<uint32_t> next( 0 );
		auto worker = [&]() {
			for ( ;; ) {
				const uint32_t k = next++;
				if ( k >= count ) {
					break;
				}
				fimage_t & img = images[k];
				int w = srcW, h = srcH;
				FitTo( img, w, h, tw, th );
				fimage_t flipped( img.size() );
				for ( int y = 0; y < h; y++ ) {
					memcpy( &flipped[(size_t)y * w * 4], &img[(size_t)( h - 1 - y ) * w * 4], (size_t)w * 4 * sizeof( float ) );
				}
				EncodeLevel( flipped, w, h, N3DS_PACKFMT_ETC1, encoded[k], opt.videoQuality );
			}
		};
		std::vector<std::thread> threads;
		for ( int t = 0; t < numThreads; t++ ) {
			threads.emplace_back( worker );
		}
		for ( std::thread & t : threads ) {
			t.join();
		}
		// changes against the previous frame, in order
		for ( uint32_t k = 0; k < count; k++ ) {
			const uint32_t f = first + k;
			const std::vector<byte> & cur = encoded[k];
			std::vector<byte> rec;
			if ( previous.empty() || f % keyInterval == 0 ) {
				rec.push_back( N3DS_VIDEO_KEY );
				rec.insert( rec.end(), cur.begin(), cur.end() );
			} else {
				std::vector<byte> mask( ( numBlocks + 7 ) / 8, 0 );
				std::vector<byte> blocks;
				for ( uint32_t b = 0; b < numBlocks; b++ ) {
					if ( memcmp( &cur[b * 8], &previous[b * 8], 8 ) != 0 ) {
						mask[b >> 3] |= (byte)( 1 << ( b & 7 ) );
						blocks.insert( blocks.end(), cur.begin() + b * 8, cur.begin() + b * 8 + 8 );
					}
				}
				if ( blocks.empty() ) {
					rec.push_back( N3DS_VIDEO_REPEAT );
				} else if ( mask.size() + blocks.size() >= frameBytes ) {
					rec.push_back( N3DS_VIDEO_KEY );		// almost everything changed
					rec.insert( rec.end(), cur.begin(), cur.end() );
				} else {
					rec.push_back( N3DS_VIDEO_DELTA );
					rec.insert( rec.end(), mask.begin(), mask.end() );
					rec.insert( rec.end(), blocks.begin(), blocks.end() );
				}
			}
			index[f].offset = (uint32_t)pos;
			index[f].size = (uint32_t)rec.size();
			fwrite( rec.data(), 1, rec.size(), out );
			pos += rec.size();
			previous = cur;
		}
	}
	header.magic = N3DS_VIDEO_MAGIC;
	header.version = N3DS_VIDEO_VERSION;
	header.width = (uint16_t)tw;
	header.height = (uint16_t)th;
	header.srcWidth = (uint16_t)srcW;
	header.srcHeight = (uint16_t)srcH;
	header.fpsMilli = (uint32_t)lround( fps * 1000.0 );
	header.numFrames = numFrames;
	header.keyInterval = keyInterval;
	header.indexOffset = (uint32_t)pos;
	header.frameBytes = frameBytes;
	fwrite( index.data(), sizeof( n3dsVideoFrame_t ), index.size(), out );
	pos += index.size() * sizeof( n3dsVideoFrame_t );
	fseek( out, 0, SEEK_SET );
	fwrite( &header, sizeof( header ), 1, out );
	if ( fclose( out ) != 0 || pos > 0x7FFFFFFFull ) {
		why = "write failed";
		return false;
	}
	bytesOut = pos;
	seconds = srcFrames / srcFps;
	Log( "  %s: %dx%d %.0f fps %.1f s -> %dx%d %.0f fps, %.1f MB\n", bik.filename().string().c_str(), srcW, srcH, srcFps,
		seconds, tw, th, fps, pos / 1048576.0 );
	return true;
}

int ConvertVideos() {
	const auto startTime = std::chrono::steady_clock::now();
	std::vector<fs::path> biks;
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
		if ( ext != ".bik" ) {
			continue;
		}
		std::string name = it->path().string();
		std::transform( name.begin(), name.end(), name.begin(), ::tolower );
		if ( !opt.only.empty() && name.find( opt.only ) == std::string::npos ) {
			continue;
		}
		if ( IsInOutput( it->path().string() ) ) {
			continue;
		}
		biks.push_back( it->path() );
	}
	std::sort( biks.begin(), biks.end() );
	Log( "n3dstex: videos in %s: %zu .bik files (texture %d, %d fps, ETC1 quality %d)\n", opt.baseDir.c_str(), biks.size(),
		opt.videoSize, opt.videoFps, opt.videoQuality );
	if ( biks.empty() ) {
		Log( "no videos (the PC game keeps them in base/video): nothing to do\n" );
		return 0;
	}
	rg_etc1::pack_etc1_block_init();
	uint64_t total = 0;
	double totalSeconds = 0.0;
	int failed = 0;
	for ( size_t v = 0; v < biks.size(); v++ ) {
		const fs::path & bik = biks[v];
		if ( cancelRequested ) {
			return 2;
		}
		// same place under the output base folder as under the game's: video/NAME.n3v
		fs::path outPath = fs::path( OutBaseDir() ) / fs::relative( bik, fs::path( opt.baseDir ), ec );
		outPath.replace_extension( ".n3v" );
		fs::create_directories( outPath.parent_path(), ec );
		uint64_t bytes = 0;
		double seconds = 0.0;
		std::string why;
		if ( !ConvertVideo( bik, outPath, bytes, seconds, why, (int)v, (int)biks.size() ) ) {
			if ( cancelRequested ) {
				Log( "cancelled: %s not written\n", outPath.string().c_str() );
				return 2;
			}
			Log( "  %s: skipped (%s)\n", bik.filename().string().c_str(), why.c_str() );
			failed++;
			continue;
		}
		total += bytes;
		totalSeconds += seconds;
	}
	const double secs = std::chrono::duration<double>( std::chrono::steady_clock::now() - startTime ).count();
	Log( "\n%zu videos (%.1f min), %d skipped, %.1f MB of .n3v, %.0f s\n", biks.size() - failed, totalSeconds / 60.0, failed,
		total / 1048576.0, secs );
	Log( "videos: %s\n", ( fs::path( OutBaseDir() ) / "video" ).string().c_str() );
	return 0;
}
