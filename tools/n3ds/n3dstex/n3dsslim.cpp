/*
================================================================================================

	n3dstex -slim <dir>: an SD card base folder without what the packs replace

	Writes every .resources file of the base folder again into <dir> (same relative
	paths), and copies every other file of the base folder (default.cfg, strings, the two
	packs, ...) as it is, so <dir> can go to sdmc:/doom3/base as a whole.

	In the new .resources files:
	- images the texture pack has (generated/images/...bimage) are left out: the engine
	  looks every 2D image up in the pack before it opens a .bimage
	  (idBinaryImage::LoadFromGeneratedFile), and never opens these;
	- samples the sound pack has (generated/...idwav) keep their header - format, length,
	  amplitude table (light flicker, lip sync) - with no buffers: the engine reads the
	  header and takes the data from the pack (idSoundSample_3DS::N3DS_UsePack);
	- everything else is copied byte for byte.
	Without the packs on the card such images are missing and such samples are silent.

================================================================================================
*/
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

#include "n3dstex.h"
#include "../../../neo/renderer/3DS/n3ds_pack.h"
#include "../../../neo/sound/3DS/n3ds_soundpack.h"

namespace fs = std::filesystem;

static const uint32_t SLIM_RESOURCE_MAGIC = 0xD000000D;
static const uint32_t SLIM_SOUND_MAGIC = 0x6D7A7274;

static void PutBE32( std::vector<byte> & v, uint32_t x ) {
	v.push_back( (byte)( x >> 24 ) );
	v.push_back( (byte)( x >> 16 ) );
	v.push_back( (byte)( x >> 8 ) );
	v.push_back( (byte)x );
}

// hashes of the entries of a pack; false if the file is not a pack of that kind
static bool ReadPackHashes( const std::string & path, bool sound, std::set<uint64_t> & hashes ) {
	FILE * f = fopen( path.c_str(), "rb" );
	if ( f == NULL ) {
		return false;
	}
	uint32_t head[4] = {};
	bool ok = fread( head, 4, 4, f ) == 4;
	const uint32_t magic = head[0], version = head[1], num = head[2], entryOffset = head[3];
	ok = ok && magic == ( sound ? N3DS_SOUNDPACK_MAGIC : N3DS_PACK_MAGIC ) &&
		version == ( sound ? (uint32_t)N3DS_SOUNDPACK_VERSION : (uint32_t)N3DS_PACK_VERSION );
	const size_t entrySize = sound ? sizeof( n3dsSoundPackEntry_t ) : sizeof( n3dsPackEntry_t );
	std::vector<byte> table;
	if ( ok ) {
		table.resize( (size_t)num * entrySize );
		ok = FSeek64( f, (int64_t)entryOffset, SEEK_SET ) == 0 && fread( table.data(), 1, table.size(), f ) == table.size();
	}
	fclose( f );
	if ( !ok ) {
		return false;
	}
	for ( uint32_t i = 0; i < num; i++ ) {
		uint64_t h;
		memcpy( &h, &table[i * entrySize], 8 );		// nameHash is the first field of both
		hashes.insert( h );
	}
	return true;
}

// the .idwav up to its buffer table, with no buffers; empty if it cannot be parsed
static std::vector<byte> IdWavStub( const byte * p, uint32_t length ) {
	std::vector<byte> stub;
	const byte * end = p + length;
	if ( length < 4 + 8 + 1 + 8 + 16 || BE32( p ) != SLIM_SOUND_MAGIC ) {
		return stub;
	}
	const byte * q = p + 4 + 8 + 1 + 8;
	const int tag = LE16( q );
	q += 16;
	if ( tag == 2 ) {			// MS ADPCM: extra size + 32 bytes
		q += 2 + 32;
	} else if ( tag == 0xFFFF ) {	// extensible: extra size + 22 bytes
		q += 2 + 22;
	} else if ( tag != 1 ) {
		return stub;
	}
	if ( end - q < 4 ) {
		return stub;
	}
	const uint32_t numAmp = BE32( q );
	q += 4;
	if ( (uint32_t)( end - q ) < numAmp + 8 ) {
		return stub;
	}
	q += numAmp;
	stub.assign( p, q );		// up to totalBufferSize
	PutBE32( stub, 0 );			// totalBufferSize
	PutBE32( stub, 0 );			// no buffers
	return stub;
}

static bool CopyRange( FILE * src, uint64_t offset, uint64_t length, FILE * dst ) {
	static thread_local std::vector<byte> buf( 1 << 20 );
	if ( FSeek64( src, (int64_t)offset, SEEK_SET ) != 0 ) {
		return false;
	}
	while ( length > 0 ) {
		const size_t n = (size_t)std::min<uint64_t>( length, buf.size() );
		if ( fread( buf.data(), 1, n, src ) != n || fwrite( buf.data(), 1, n, dst ) != n ) {
			return false;
		}
		length -= n;
	}
	return true;
}

struct slimStats_t {
	uint64_t	before = 0;
	uint64_t	after = 0;
	int			imagesDropped = 0;
	int			soundsStubbed = 0;
	int			copied = 0;
};

static bool SlimContainer( const std::string & srcPath, const std::string & dstPath, const std::set<uint64_t> & images,
		const std::set<uint64_t> & sounds, slimStats_t & st ) {
	std::vector<resourceEntry_t> entries;
	if ( !ReadResourceTable( srcPath, entries ) ) {
		return false;
	}
	FILE * src = fopen( srcPath.c_str(), "rb" );
	FILE * dst = fopen( dstPath.c_str(), "wb" );
	if ( src == NULL || dst == NULL ) {
		Log( "  cannot open %s or write %s\n", srcPath.c_str(), dstPath.c_str() );
		if ( src ) fclose( src );
		if ( dst ) fclose( dst );
		return false;
	}
	FSeek64( src, 0, SEEK_END );
	st.before += (uint64_t)FTell64( src );

	const byte zeros[12] = {};
	fwrite( zeros, 1, 12, dst );
	uint64_t pos = 12;
	std::vector<resourceEntry_t> kept;
	bool ok = true;
	for ( const resourceEntry_t & e : entries ) {
		const bool isImage = e.name.size() > 7 && e.name.compare( e.name.size() - 7, 7, ".bimage" ) == 0;
		const bool isSound = e.name.size() > 6 && e.name.compare( e.name.size() - 6, 6, ".idwav" ) == 0;
		const uint64_t h = N3DS_PackHash( e.name.c_str() );
		if ( isImage && images.count( h ) ) {
			st.imagesDropped++;
			continue;
		}
		resourceEntry_t out = e;
		out.offset = (uint32_t)pos;
		if ( isSound && sounds.count( h ) ) {
			std::vector<byte> data( e.length );
			std::vector<byte> stub;
			if ( ReadAt( src, e.offset, data.data(), e.length ) ) {
				stub = IdWavStub( data.data(), e.length );
			}
			if ( !stub.empty() ) {
				fwrite( stub.data(), 1, stub.size(), dst );
				out.length = (uint32_t)stub.size();
				pos += stub.size();
				kept.push_back( out );
				st.soundsStubbed++;
				continue;
			}
		}
		if ( !CopyRange( src, e.offset, e.length, dst ) ) {
			ok = false;
			break;
		}
		pos += e.length;
		kept.push_back( out );
		st.copied++;
	}
	// table: count, then name (little-endian length), offset, length (big-endian)
	std::vector<byte> table;
	PutBE32( table, (uint32_t)kept.size() );
	for ( const resourceEntry_t & e : kept ) {
		const uint32_t len = (uint32_t)e.name.size();
		table.push_back( (byte)len );
		table.push_back( (byte)( len >> 8 ) );
		table.push_back( (byte)( len >> 16 ) );
		table.push_back( (byte)( len >> 24 ) );
		table.insert( table.end(), e.name.begin(), e.name.end() );
		PutBE32( table, e.offset );
		PutBE32( table, e.length );
	}
	const uint64_t tableOffset = pos;
	ok = ok && fwrite( table.data(), 1, table.size(), dst ) == table.size();
	pos += table.size();
	std::vector<byte> head;
	PutBE32( head, SLIM_RESOURCE_MAGIC );
	PutBE32( head, (uint32_t)tableOffset );
	PutBE32( head, (uint32_t)table.size() );
	FSeek64( dst, 0, SEEK_SET );
	ok = ok && fwrite( head.data(), 1, 12, dst ) == 12;
	fclose( src );
	ok = ( fclose( dst ) == 0 ) && ok && pos <= 0x7FFFFFFFull;
	st.after += pos;
	return ok;
}

int SlimResources( const std::vector<std::string> & containers ) {
	std::error_code ec;
	const fs::path base = fs::weakly_canonical( fs::path( opt.baseDir ), ec );
	const fs::path outDir = fs::weakly_canonical( fs::path( opt.slimDir ), ec );
	if ( outDir == base ) {
		Log( "-slim needs another folder than the base folder (the originals are read from there)\n" );
		return 1;
	}
	std::set<uint64_t> images, sounds;
	const bool haveTex = ReadPackHashes( opt.output, false, images );
	const bool haveSnd = ReadPackHashes( opt.soundOutput, true, sounds );
	if ( !haveTex && !haveSnd ) {
		Log( "-slim: no packs (%s, %s): convert first\n", opt.output.c_str(), opt.soundOutput.c_str() );
		return 1;
	}
	Log( "n3dstex: slim copy of %s -> %s\n", opt.baseDir.c_str(), opt.slimDir.c_str() );
	Log( "  texture pack: %s (%zu images), sound pack: %s (%zu samples)\n", haveTex ? "yes" : "no", images.size(),
		haveSnd ? "yes" : "no", sounds.size() );

	slimStats_t st;
	int failed = 0;
	for ( const std::string & c : containers ) {
		const fs::path rel = fs::relative( fs::path( c ), fs::path( opt.baseDir ), ec );
		const fs::path dst = outDir / rel;
		fs::create_directories( dst.parent_path(), ec );
		const uint64_t before = st.before, after = st.after;
		if ( !SlimContainer( c, dst.string(), images, sounds, st ) ) {
			Log( "  FAILED: %s\n", c.c_str() );
			failed++;
			continue;
		}
		Log( "  %s: %.1f -> %.1f MB\n", rel.string().c_str(), ( st.before - before ) / 1048576.0, ( st.after - after ) / 1048576.0 );
	}
	// everything else of the base folder as it is
	uint64_t otherBytes = 0;
	int otherFiles = 0;
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
			continue;
		}
		const fs::path abs = fs::weakly_canonical( it->path(), ec );
		if ( abs.string().compare( 0, outDir.string().size(), outDir.string() ) == 0 ) {
			continue;		// the output folder inside the base folder
		}
		const fs::path dst = outDir / fs::relative( it->path(), fs::path( opt.baseDir ), ec );
		fs::create_directories( dst.parent_path(), ec );
		fs::copy_file( it->path(), dst, fs::copy_options::overwrite_existing, ec );
		if ( ec ) {
			Log( "  cannot copy %s\n", it->path().string().c_str() );
			failed++;
			ec.clear();
			continue;
		}
		otherBytes += it->file_size();
		otherFiles++;
	}
	Log( "\n.resources: %.0f MB -> %.0f MB (%d images left out, %d samples reduced to their header, %d entries copied)\n",
		st.before / 1048576.0, st.after / 1048576.0, st.imagesDropped, st.soundsStubbed, st.copied );
	Log( "other files copied: %d, %.0f MB (the packs among them)\n", otherFiles, otherBytes / 1048576.0 );
	if ( failed > 0 ) {
		Log( "ERROR: %d files failed\n", failed );
		return 1;
	}
	Log( "copy the contents of %s to sdmc:/doom3/base/ on the SD card (instead of the originals)\n", opt.slimDir.c_str() );
	return 0;
}
