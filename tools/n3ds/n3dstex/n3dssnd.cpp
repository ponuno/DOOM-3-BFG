/*
================================================================================================

	n3dstex sound part: _sound_pc*.resources -> _n3ds_sounds.pak (DSP-ADPCM)

	Every generated sample (generated/...idwav: 16-bit PCM or MS ADPCM, mono or stereo)
	is decoded and re-encoded per channel as DSP-ADPCM, the format the 3DS DSP decodes in
	hardware. Pack format and the DSP-ADPCM rules: neo/sound/3DS/n3ds_soundpack.h.

	Encoder: for every 14-sample frame each of the 8 predictor pairs and every scale
	shift is tried, simulating the DSP's decoder exactly (rounding, clamping), and the
	one with the least squared error is kept. All samples use one fixed set of pairs:
	the seven MS ADPCM pairs the game's samples were encoded with (x 8, MS ADPCM uses
	256 = 1.0) and a gentle second-order one; a voice can then go from its leadin to its
	looping sample without changing its DSP channel's coefficients.

	-v logs every sample with its signal-to-noise ratio against the decoded source.

================================================================================================
*/
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>

#include "n3dstex.h"
#include "../../../neo/renderer/3DS/n3ds_pack.h"
#include "../../../neo/sound/3DS/n3ds_soundpack.h"

namespace fs = std::filesystem;

static const uint32_t SOUND_MAGIC_IDMSA = 0x6D7A7274;
enum { FORMAT_PCM = 1, FORMAT_ADPCM = 2, FORMAT_EXTENSIBLE = 0xFFFF };

// predictor pairs, 2048 = 1.0: the MS ADPCM standard pairs (256 = 1.0) x 8, and (1.0, -0.5)
static const int16_t dspCoefs[16] = {
	2048, 0,
	4096, -2048,
	0, 0,
	1536, 512,
	1920, 0,
	3680, -1664,
	3136, -1856,
	2048, -1024
};

/*
================================================================================================
	Source decoding (as the engine's 3DS_SoundVoice.cpp)
================================================================================================
*/
static const int msAdapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
static const int msDefaultCoef[7][2] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 } };

struct msFormat_t {
	int		channels;
	int		blockBytes;
	int		samplesPerBlock;
	int		numCoef;
	int		coef[7][2];
};

static inline int MsNibble( int nibble, int & s1, int & s2, int & delta, int c1, int c2 ) {
	const int signedNibble = ( nibble & 8 ) ? nibble - 16 : nibble;
	int pred = ( s1 * c1 + s2 * c2 ) >> 8;
	pred += signedNibble * delta;
	pred = std::max( -32768, std::min( 32767, pred ) );
	s2 = s1;
	s1 = pred;
	delta = ( msAdapt[nibble] * delta ) >> 8;
	if ( delta < 16 ) {
		delta = 16;
	}
	return pred;
}

// one block -> out[channels][samplesPerBlock] appended
static void DecodeMsBlock( const msFormat_t & fmt, const byte * block, int blockBytes, std::vector<int16_t> * out ) {
	const int channels = fmt.channels;
	const int spb = fmt.samplesPerBlock;
	if ( blockBytes < 7 * channels ) {
		return;
	}
	int coef[2][2], delta[2], s1[2], s2[2];
	const byte * p = block;
	for ( int c = 0; c < channels; c++ ) {
		const int pred = std::min( (int)p[c], 6 );
		coef[c][0] = ( fmt.numCoef == 7 ) ? fmt.coef[pred][0] : msDefaultCoef[pred][0];
		coef[c][1] = ( fmt.numCoef == 7 ) ? fmt.coef[pred][1] : msDefaultCoef[pred][1];
	}
	p += channels;
	for ( int c = 0; c < channels; c++ ) {
		delta[c] = (int16_t)LE16( p + c * 2 );
	}
	p += channels * 2;
	for ( int c = 0; c < channels; c++ ) {
		s1[c] = (int16_t)LE16( p + c * 2 );
	}
	p += channels * 2;
	for ( int c = 0; c < channels; c++ ) {
		s2[c] = (int16_t)LE16( p + c * 2 );
	}
	p += channels * 2;
	std::vector<int16_t> inter( (size_t)spb * channels, 0 );
	for ( int c = 0; c < channels; c++ ) {
		inter[c] = (int16_t)s2[c];
		inter[channels + c] = (int16_t)s1[c];
	}
	const byte * end = block + blockBytes;
	int n = 2 * channels;
	const int total = spb * channels;
	int c = 0;
	for ( ; p < end && n < total; p++ ) {
		inter[n++] = (int16_t)MsNibble( *p >> 4, s1[c], s2[c], delta[c], coef[c][0], coef[c][1] );
		c = ( c + 1 ) % channels;
		if ( n >= total ) {
			break;
		}
		inter[n++] = (int16_t)MsNibble( *p & 15, s1[c], s2[c], delta[c], coef[c][0], coef[c][1] );
		c = ( c + 1 ) % channels;
	}
	for ( int i = 0; i < spb; i++ ) {
		for ( int ch = 0; ch < channels; ch++ ) {
			out[ch].push_back( inter[i * channels + ch] );
		}
	}
}

struct soundSource_t {
	std::string	name;
	int			container;
	uint32_t	offset;
	uint32_t	length;
};

struct soundResult_t {
	bool					ok = false;
	std::string				skipReason;
	n3dsSoundPackEntry_t	entry = {};
	std::vector<byte>		data;
	double					snr = 0.0;		// dB, worst channel
	double					seconds = 0.0;
};

// parses a generated .idwav and decodes its play range to 16-bit PCM per channel
static bool DecodeIdWav( const byte * p, uint32_t length, int & rate, std::vector<int16_t> pcm[2], std::string & why ) {
	const byte * end = p + length;
	if ( length < 4 + 8 + 1 + 8 + 16 || BE32( p ) != SOUND_MAGIC_IDMSA ) {
		why = "not an idwav";
		return false;
	}
	const byte * q = p + 4 + 8 + 1;
	const int playBegin = (int)BE32( q );
	const int playLength = (int)BE32( q + 4 );
	q += 8;
	int tag = LE16( q );
	const int channels = LE16( q + 2 );
	rate = (int)LE32( q + 4 );
	const int blockBytes = LE16( q + 12 );
	const int bits = LE16( q + 14 );
	q += 16;
	msFormat_t ms = {};
	if ( tag == FORMAT_ADPCM ) {
		if ( end - q < 2 + 32 ) {
			why = "short header";
			return false;
		}
		const int extraSize = LE16( q );
		q += 2;
		if ( extraSize != 32 ) {
			why = "unexpected ADPCM header size";
			return false;
		}
		ms.samplesPerBlock = LE16( q );
		ms.numCoef = LE16( q + 2 );
		for ( int i = 0; i < 7; i++ ) {
			ms.coef[i][0] = (int16_t)LE16( q + 4 + i * 4 );
			ms.coef[i][1] = (int16_t)LE16( q + 6 + i * 4 );
		}
		q += 32;
	} else if ( tag == FORMAT_EXTENSIBLE ) {
		if ( end - q < 2 + 22 ) {
			why = "short header";
			return false;
		}
		const int sub = (int)LE32( q + 2 + 6 );
		q += 2 + 22;
		tag = sub;
	}
	if ( tag != FORMAT_PCM && tag != FORMAT_ADPCM ) {
		why = "format " + std::to_string( tag ) + " not handled";
		return false;
	}
	if ( channels < 1 || channels > 2 || rate < 1000 || rate > 96000 || playLength <= 0 || playBegin < 0 ) {
		why = "bad header";
		return false;
	}
	if ( tag == FORMAT_PCM && bits != 16 ) {
		why = "PCM not 16-bit";
		return false;
	}
	ms.channels = channels;
	ms.blockBytes = blockBytes;
	if ( tag == FORMAT_ADPCM && ( ms.samplesPerBlock <= 2 || blockBytes < 7 * channels ) ) {
		why = "bad ADPCM header";
		return false;
	}
	// amplitude table, buffer table
	if ( end - q < 4 ) {
		why = "short file";
		return false;
	}
	const uint32_t numAmp = BE32( q );
	q += 4;
	if ( (uint32_t)( end - q ) < numAmp + 8 ) {
		why = "short file";
		return false;
	}
	q += numAmp;
	q += 4;		// totalBufferSize
	const uint32_t numBuffers = BE32( q );
	q += 4;
	std::vector<int16_t> all[2];
	int previous = 0;
	for ( uint32_t b = 0; b < numBuffers; b++ ) {
		if ( end - q < 8 ) {
			why = "short buffer table";
			return false;
		}
		const int numSamples = (int)BE32( q );		// cumulative
		const uint32_t size = BE32( q + 4 );
		q += 8;
		if ( (uint32_t)( end - q ) < size ) {
			why = "short buffer";
			return false;
		}
		const int want = numSamples - previous;
		const size_t before = all[0].size();
		if ( tag == FORMAT_PCM ) {
			const int frames = (int)( size / ( 2 * channels ) );
			for ( int i = 0; i < frames; i++ ) {
				for ( int c = 0; c < channels; c++ ) {
					all[c].push_back( (int16_t)LE16( q + ( i * channels + c ) * 2 ) );
				}
			}
		} else {
			for ( uint32_t off = 0; off < size; off += blockBytes ) {
				DecodeMsBlock( ms, q + off, (int)std::min<uint32_t>( blockBytes, size - off ), all );
			}
		}
		// each buffer holds exactly its samples (the last ADPCM block may be padded)
		for ( int c = 0; c < channels; c++ ) {
			all[c].resize( before + std::max( want, 0 ), 0 );
		}
		previous = numSamples;
		q += size;
	}
	for ( int c = 0; c < channels; c++ ) {
		if ( (int)all[c].size() < playBegin + playLength ) {
			all[c].resize( playBegin + playLength, 0 );
		}
		pcm[c].assign( all[c].begin() + playBegin, all[c].begin() + playBegin + playLength );
	}
	return true;
}

/*
================================================================================================
	DSP-ADPCM
================================================================================================
*/
static inline int DspDecodeSample( int nibble, int scale, int c1, int c2, int h1, int h2 ) {
	int v = ( ( nibble * ( 1 << scale ) ) * 2048 ) + 1024 + c1 * h1 + c2 * h2;
	v >>= 11;
	return std::max( -32768, std::min( 32767, v ) );
}

// encodes one channel; out gets N3DS_DspAdpcmBytes( n ) bytes
static void DspEncode( const int16_t * pcm, int n, std::vector<byte> & out ) {
	int h1 = 0, h2 = 0;
	for ( int f = 0; f < n; f += 14 ) {
		int x[14];
		const int count = std::min( 14, n - f );
		for ( int i = 0; i < 14; i++ ) {
			x[i] = ( i < count ) ? pcm[f + i] : 0;
		}
		int64_t bestErr = INT64_MAX;
		int bestP = 0, bestS = 0, bestH1 = 0, bestH2 = 0;
		int bestNib[14] = {};
		for ( int pr = 0; pr < 8; pr++ ) {
			const int c1 = dspCoefs[pr * 2], c2 = dspCoefs[pr * 2 + 1];
			for ( int sc = 0; sc <= 12; sc++ ) {
				int a1 = h1, a2 = h2;
				int64_t err = 0;
				int nib[14];
				const int step = 2048 << sc;
				for ( int i = 0; i < 14 && err < bestErr; i++ ) {
					const int pred = c1 * a1 + c2 * a2 + 1024;
					const int64_t want = (int64_t)x[i] * 2048 - pred;
					// nearest residual, then the neighbours (rounding of >> 11 and clamping)
					int r = (int)std::floor( (double)want / step + 0.5 );
					r = std::max( -8, std::min( 7, r ) );
					int bestR = r, bestY = DspDecodeSample( r, sc, c1, c2, a1, a2 );
					for ( int d = -1; d <= 1; d += 2 ) {
						const int rr = r + d;
						if ( rr < -8 || rr > 7 ) {
							continue;
						}
						const int y = DspDecodeSample( rr, sc, c1, c2, a1, a2 );
						if ( std::abs( y - x[i] ) < std::abs( bestY - x[i] ) ) {
							bestR = rr;
							bestY = y;
						}
					}
					// count only the real samples of a short last frame
					if ( i < count ) {
						err += (int64_t)( bestY - x[i] ) * ( bestY - x[i] );
					}
					nib[i] = bestR;
					a2 = a1;
					a1 = bestY;
				}
				if ( err < bestErr ) {
					bestErr = err;
					bestP = pr;
					bestS = sc;
					bestH1 = a1;
					bestH2 = a2;
					memcpy( bestNib, nib, sizeof( nib ) );
				}
			}
		}
		out.push_back( (byte)( ( bestP << 4 ) | bestS ) );
		for ( int i = 0; i < 14; i += 2 ) {
			out.push_back( (byte)( ( ( bestNib[i] & 15 ) << 4 ) | ( bestNib[i + 1] & 15 ) ) );
		}
		// the state the DSP has after this frame (all 14 decoded samples, also padding)
		h1 = bestH1;
		h2 = bestH2;
	}
}

static void DspDecode( const byte * data, int n, std::vector<int16_t> & out ) {
	out.clear();
	int h1 = 0, h2 = 0;
	for ( int f = 0; f < n; f += 14 ) {
		const byte * fr = data + ( f / 14 ) * 8;
		const int pr = fr[0] >> 4, sc = fr[0] & 15;
		const int c1 = dspCoefs[pr * 2], c2 = dspCoefs[pr * 2 + 1];
		for ( int i = 0; i < 14; i++ ) {
			int nib = ( i & 1 ) ? ( fr[1 + i / 2] & 15 ) : ( fr[1 + i / 2] >> 4 );
			if ( nib >= 8 ) {
				nib -= 16;
			}
			const int y = DspDecodeSample( nib, sc, c1, c2, h1, h2 );
			h2 = h1;
			h1 = y;
			if ( f + i < n ) {
				out.push_back( (int16_t)y );
			}
		}
	}
}

static double Snr( const std::vector<int16_t> & a, const std::vector<int16_t> & b ) {
	double sig = 0.0, noise = 0.0;
	for ( size_t i = 0; i < a.size() && i < b.size(); i++ ) {
		sig += (double)a[i] * a[i];
		noise += (double)( a[i] - b[i] ) * ( a[i] - b[i] );
	}
	if ( noise <= 0.0 ) {
		return 99.0;
	}
	if ( sig <= 0.0 ) {
		return 0.0;
	}
	return 10.0 * log10( sig / noise );
}

static void ConvertSound( const soundSource_t & src, FILE * f, soundResult_t & res ) {
	std::vector<byte> file( src.length );
	if ( !ReadAt( f, src.offset, file.data(), src.length ) ) {
		res.skipReason = "cannot read";
		return;
	}
	int rate = 0;
	std::vector<int16_t> pcm[2];
	if ( !DecodeIdWav( file.data(), src.length, rate, pcm, res.skipReason ) ) {
		return;
	}
	const int channels = pcm[1].empty() ? 1 : 2;
	const int n = (int)pcm[0].size();
	const uint32_t channelBytes = N3DS_DspAdpcmBytes( n );
	double worst = 99.0;
	for ( int c = 0; c < channels; c++ ) {
		std::vector<byte> enc;
		DspEncode( pcm[c].data(), n, enc );
		std::vector<int16_t> dec;
		DspDecode( enc.data(), n, dec );
		worst = std::min( worst, Snr( pcm[c], dec ) );
		res.data.insert( res.data.end(), enc.begin(), enc.end() );
	}
	res.snr = worst;
	res.seconds = (double)n / rate;
	n3dsSoundPackEntry_t & e = res.entry;
	e.nameHash = N3DS_PackHash( src.name.c_str() );
	e.numSamples = (uint32_t)n;
	e.sampleRate = (uint32_t)rate;
	e.channelBytes = channelBytes;
	e.channels = (uint8_t)channels;
	res.ok = true;
}

/*
================================================================================================
	ConvertSounds
================================================================================================
*/
int ConvertSounds( const std::vector<std::string> & containers ) {
	const auto startTime = std::chrono::steady_clock::now();
	Log( "n3dstex: sounds %s -> %s\n", opt.baseDir.c_str(), opt.soundOutput.c_str() );
	std::vector<soundSource_t> sources;
	std::map<std::string, bool> seen;
	for ( size_t i = 0; i < containers.size(); i++ ) {
		std::vector<resourceEntry_t> entries;
		if ( !ReadResourceTable( containers[i], entries ) ) {
			continue;
		}
		int found = 0;
		for ( const resourceEntry_t & e : entries ) {
			if ( e.name.size() < 6 || e.name.compare( e.name.size() - 6, 6, ".idwav" ) != 0 || seen.count( e.name ) ) {
				continue;
			}
			if ( !opt.only.empty() && e.name.find( opt.only ) == std::string::npos ) {
				continue;
			}
			seen[e.name] = true;
			sources.push_back( { e.name, (int)i, e.offset, e.length } );
			found++;
		}
		if ( found > 0 ) {
			Log( "  %s: %d sounds\n", containers[i].c_str(), found );
		}
	}
	Log( "%zu sounds to convert\n", sources.size() );
	if ( opt.list ) {
		for ( const soundSource_t & s : sources ) {
			Log( "%s (%u bytes)\n", s.name.c_str(), s.length );
		}
		return 0;
	}
	if ( sources.empty() ) {
		Log( "no sounds found (the _sound_pc*.resources files belong in the base folder): no sound pack\n" );
		return 0;
	}

	FILE * out = fopen( opt.soundOutput.c_str(), "wb" );
	if ( out == NULL ) {
		Log( "cannot write %s\n", opt.soundOutput.c_str() );
		return 1;
	}
	n3dsSoundPackHeader_t header = {};
	fwrite( &header, sizeof( header ), 1, out );
	uint64_t writePos = sizeof( header );
	auto padTo = [&]( uint64_t align ) {
		static const byte zeros[64] = {};
		const uint64_t pad = ( align - writePos % align ) % align;
		fwrite( zeros, 1, (size_t)pad, out );
		writePos += pad;
	};

	std::vector<n3dsSoundPackEntry_t> entries;
	std::mutex outLock;
	std::atomic<size_t> next( 0 );
	std::atomic<int> done( 0 );
	int skipped = 0;
	std::map<std::string, int> skipReasons;
	double totalSeconds = 0.0, snrSum = 0.0;
	int snrCount = 0;
	std::vector<std::pair<double, std::string>> worst;
	bool writeFailed = false;

	int numThreads = opt.threads > 0 ? opt.threads : (int)std::thread::hardware_concurrency();
	numThreads = std::max( 1, std::min( numThreads, 64 ) );
	auto worker = [&]() {
		std::vector<FILE *> files( containers.size(), (FILE *)NULL );
		for ( ;; ) {
			const size_t i = next++;
			if ( i >= sources.size() ) {
				break;
			}
			const soundSource_t & src = sources[i];
			FILE *& f = files[src.container];
			if ( f == NULL ) {
				f = fopen( containers[src.container].c_str(), "rb" );
			}
			soundResult_t res;
			if ( f != NULL ) {
				ConvertSound( src, f, res );
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
				padTo( N3DS_SOUNDPACK_ALIGN );
				res.entry.dataOffset = (uint32_t)( writePos / N3DS_SOUNDPACK_ALIGN );
				if ( fwrite( res.data.data(), 1, res.data.size(), out ) != res.data.size() ) {
					writeFailed = true;
				}
				writePos += res.data.size();
				entries.push_back( res.entry );
				totalSeconds += res.seconds;
				if ( res.seconds > 0.05 ) {
					snrSum += res.snr;
					snrCount++;
					worst.push_back( { res.snr, src.name } );
				}
				if ( opt.verbose ) {
					Log( "  %s: %u Hz %d ch %.2f s, SNR %.1f dB\n", src.name.c_str(), res.entry.sampleRate, res.entry.channels, res.seconds, res.snr );
				}
			}
			const int d = ++done;
			if ( d % 1000 == 0 ) {
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

	std::sort( entries.begin(), entries.end(), []( const n3dsSoundPackEntry_t & a, const n3dsSoundPackEntry_t & b ) {
		return a.nameHash < b.nameHash;
	} );
	for ( size_t i = 1; i < entries.size(); i++ ) {
		if ( entries[i].nameHash == entries[i - 1].nameHash ) {
			Log( "WARNING: name hash collision (%016llx)\n", (unsigned long long)entries[i].nameHash );
		}
	}
	padTo( 16 );
	header.magic = N3DS_SOUNDPACK_MAGIC;
	header.version = N3DS_SOUNDPACK_VERSION;
	header.numEntries = (uint32_t)entries.size();
	header.entryOffset = (uint32_t)writePos;
	memcpy( header.coefs, dspCoefs, sizeof( dspCoefs ) );
	if ( !entries.empty() && fwrite( entries.data(), sizeof( n3dsSoundPackEntry_t ), entries.size(), out ) != entries.size() ) {
		writeFailed = true;
	}
	writePos += entries.size() * sizeof( n3dsSoundPackEntry_t );
	fseek( out, 0, SEEK_SET );
	fwrite( &header, sizeof( header ), 1, out );
	if ( fclose( out ) != 0 || writeFailed || writePos > 0x7FFFFFFFull ) {
		Log( "ERROR: writing %s failed%s\n", opt.soundOutput.c_str(), writePos > 0x7FFFFFFFull ? ": over 2 GB" : " (disk full?)" );
		return 1;
	}
	const double secs = std::chrono::duration<double>( std::chrono::steady_clock::now() - startTime ).count();
	Log( "\n%zu sounds (%.0f min of audio), %d skipped, %.1f MB, %.0f s\n", entries.size(), totalSeconds / 60.0, skipped,
		writePos / ( 1024.0 * 1024.0 ), secs );
	if ( snrCount > 0 ) {
		std::sort( worst.begin(), worst.end() );
		Log( "  SNR against the decoded source: mean %.1f dB; lowest:", snrSum / snrCount );
		for ( size_t i = 0; i < worst.size() && i < 3; i++ ) {
			Log( " %.1f (%s)", worst[i].first, worst[i].second.c_str() );
		}
		Log( "\n" );
	}
	for ( const auto & s : skipReasons ) {
		Log( "  skipped (%s): %d\n", s.first.c_str(), s.second );
	}
	Log( "copy %s to sdmc:/doom3/base/ on the SD card\n", fs::path( opt.soundOutput ).filename().string().c_str() );
	return 0;
}
