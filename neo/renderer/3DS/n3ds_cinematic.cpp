/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

===========================================================================
*/

/*
================================================================================================

	New 3DS videos: the game's Bink videos converted on the PC by n3dstex into .n3v
	(renderer/3DS/n3ds_video.h): ETC1 frames stored as block changes to the previous one.

	A material's videoMap (the main menu's Mars, loadvideo, screens in the levels) opens
	video/NAME.n3v instead of video/NAME.bik. The backend asks for the texture of the
	frame at its time (N3DS_GpuTexture, from the ambient / GUI stage code); the frame is
	built in RAM by applying the records since the last frame shown (or since the key
	frame before it, when going back or looping), then copied into one of two textures
	in linear memory - the other one may still be sampled by the frame the GPU draws.
	Nothing is decoded: a frame costs a short SD read and a 32 KB copy.

	The PC source of BFG has no video player at all (Bink is not open source); without
	the .n3v files the stages stay undrawn as before.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../tr_local.h"
#include "n3ds_gpu.h"
#include "n3ds_video.h"

class idCinematic_3DS : public idCinematic {
public:
						idCinematic_3DS();
						~idCinematic_3DS();

	virtual bool		InitFromFile( const char * qpath, bool looping );
	virtual int			AnimationLength();
	virtual cinData_t	ImageForTime( int milliseconds );
	virtual void		Close();
	virtual void		ResetTime( int time );
	virtual int			GetStartTime();
	virtual float		GetFrameRate() const;
	virtual void *		N3DS_GpuTexture( int milliseconds );

private:
	bool				ApplyRecord( int frame );
	bool				GoToFrame( int frame );
	int					FrameForTime( int milliseconds );

	idStr				name;
	FILE *				file;
	n3dsVideoHeader_t	header;
	n3dsVideoFrame_t *	index;
	byte *				frameData;		// the current frame, as the GPU wants it
	byte *				record;			// one frame record
	int					recordSize;
	int					currentFrame;	// in frameData, -1 none
	bool				looping;
	int					startTime;		// -1: starts at the first frame asked for
	bool				ended;
	void *				textures[2];
	int					shown;			// texture holding currentFrame, -1 none
	int					shownFrame;
};

static int videoOpenLogs = 0;

idCinematic_3DS::idCinematic_3DS() {
	file = NULL;
	memset( &header, 0, sizeof( header ) );
	index = NULL;
	frameData = NULL;
	record = NULL;
	recordSize = 0;
	currentFrame = -1;
	looping = false;
	startTime = -1;
	ended = false;
	textures[0] = textures[1] = NULL;
	shown = -1;
	shownFrame = -1;
}

idCinematic_3DS::~idCinematic_3DS() {
	Close();
}

void idCinematic_3DS::Close() {
	if ( file != NULL ) {
		fclose( file );
		file = NULL;
	}
	Mem_Free( index );
	index = NULL;
	Mem_Free( frameData );
	frameData = NULL;
	Mem_Free( record );
	record = NULL;
	recordSize = 0;
	for ( int i = 0; i < 2; i++ ) {
		if ( textures[i] != NULL ) {
			N3DSGPU_TexFree( textures[i] );
			textures[i] = NULL;
		}
	}
	currentFrame = -1;
	shown = -1;
	shownFrame = -1;
}

bool idCinematic_3DS::InitFromFile( const char * qpath, bool looping_ ) {
	Close();
	looping = looping_;
	startTime = -1;
	ended = false;
	// video/NAME.bik (either slash) -> video/NAME.n3v in the base folder
	idStr path = qpath;
	path.BackSlashesToSlashes();
	path.SetFileExtension( "n3v" );
	name = path;
	const char * osPath = fileSystem->RelativePathToOSPath( path.c_str(), "fs_basepath" );
	file = fopen( osPath, "rb" );
	if ( file == NULL ) {
		if ( videoOpenLogs++ < 16 ) {
			common->Printf( "video: no %s (convert the .bik with n3dstex): not drawn\n", osPath );
		}
		return false;
	}
	bool ok = fread( &header, sizeof( header ), 1, file ) == 1 && header.magic == N3DS_VIDEO_MAGIC &&
		header.version == N3DS_VIDEO_VERSION && header.numFrames > 0 && header.numFrames < 1000000 &&
		header.width >= 8 && header.height >= 8 && header.width <= 1024 && header.height <= 1024 &&
		header.frameBytes == (uint32)header.width * header.height / 2 && header.fpsMilli > 0 && header.keyInterval > 0;
	if ( ok ) {
		index = (n3dsVideoFrame_t *)Mem_Alloc( header.numFrames * sizeof( n3dsVideoFrame_t ), TAG_CINEMATIC );
		ok = fseek( file, header.indexOffset, SEEK_SET ) == 0 &&
			fread( index, sizeof( n3dsVideoFrame_t ), header.numFrames, file ) == header.numFrames;
	}
	if ( !ok ) {
		common->Warning( "video: %s is not a .n3v of version %d (convert again with n3dstex)", osPath, N3DS_VIDEO_VERSION );
		Close();
		return false;
	}
	frameData = (byte *)Mem_Alloc( header.frameBytes, TAG_CINEMATIC );
	memset( frameData, 0, header.frameBytes );
	if ( videoOpenLogs++ < 16 ) {
		common->Printf( "video: %s %dx%d (from %dx%d), %d frames at %.1f fps%s\n", path.c_str(), header.width, header.height,
			header.srcWidth, header.srcHeight, header.numFrames, header.fpsMilli * 0.001f, looping ? ", looping" : "" );
	}
	return true;
}

int idCinematic_3DS::AnimationLength() {
	if ( file == NULL ) {
		return 0;
	}
	return (int)( (int64)header.numFrames * 1000000 / header.fpsMilli );
}

float idCinematic_3DS::GetFrameRate() const {
	return ( file != NULL ) ? header.fpsMilli * 0.001f : 30.0f;
}

void idCinematic_3DS::ResetTime( int time ) {
	startTime = time;
	ended = false;
}

int idCinematic_3DS::GetStartTime() {
	return startTime;
}

int idCinematic_3DS::FrameForTime( int milliseconds ) {
	if ( startTime < 0 ) {
		startTime = milliseconds;
	}
	int64 frame = (int64)( milliseconds - startTime ) * header.fpsMilli / 1000000;
	if ( frame < 0 ) {
		frame = 0;
	}
	if ( looping ) {
		frame %= header.numFrames;
	} else if ( frame >= header.numFrames ) {
		frame = header.numFrames - 1;
		ended = true;
	}
	return (int)frame;
}

// applies frame 'frame''s record to frameData
bool idCinematic_3DS::ApplyRecord( int frame ) {
	const n3dsVideoFrame_t & fr = index[frame];
	if ( fr.size == 0 ) {
		return false;
	}
	if ( (int)fr.size > recordSize ) {
		Mem_Free( record );
		recordSize = fr.size;
		record = (byte *)Mem_Alloc( recordSize, TAG_CINEMATIC );
	}
	if ( fseek( file, fr.offset, SEEK_SET ) != 0 || fread( record, 1, fr.size, file ) != fr.size ) {
		return false;
	}
	const int blocks = header.frameBytes / 8;
	const int maskBytes = ( blocks + 7 ) / 8;
	switch ( record[0] ) {
		case N3DS_VIDEO_KEY:
			if ( fr.size < 1 + header.frameBytes ) {
				return false;
			}
			memcpy( frameData, record + 1, header.frameBytes );
			return true;
		case N3DS_VIDEO_DELTA: {
			if ( (int)fr.size < 1 + maskBytes ) {
				return false;
			}
			const byte * mask = record + 1;
			const byte * src = mask + maskBytes;
			const byte * end = record + fr.size;
			for ( int b = 0; b < blocks; b++ ) {
				if ( mask[b >> 3] & ( 1 << ( b & 7 ) ) ) {
					if ( src + 8 > end ) {
						return false;
					}
					memcpy( frameData + b * 8, src, 8 );
					src += 8;
				}
			}
			return true;
		}
		case N3DS_VIDEO_REPEAT:
			return true;
		default:
			return false;
	}
}

bool idCinematic_3DS::GoToFrame( int frame ) {
	if ( frame == currentFrame ) {
		return true;
	}
	int from = currentFrame + 1;
	if ( currentFrame < 0 || frame < currentFrame || frame - currentFrame > (int)header.keyInterval ) {
		from = frame - frame % header.keyInterval;		// a key frame
	}
	for ( int f = from; f <= frame; f++ ) {
		if ( !ApplyRecord( f ) ) {
			currentFrame = -1;
			return false;
		}
	}
	currentFrame = frame;
	return true;
}

/*
========================
idCinematic_3DS::N3DS_GpuTexture

Backend: the GPU texture showing the frame at 'milliseconds', NULL if there is none.
========================
*/
void * idCinematic_3DS::N3DS_GpuTexture( int milliseconds ) {
	if ( file == NULL ) {
		return NULL;
	}
	const int frame = FrameForTime( milliseconds );
	if ( shown >= 0 && shownFrame == frame ) {
		return textures[shown];
	}
	if ( !GoToFrame( frame ) ) {
		return ( shown >= 0 ) ? textures[shown] : NULL;
	}
	const int target = ( shown == 0 ) ? 1 : 0;
	if ( textures[target] == NULL ) {
		textures[target] = N3DSGPU_TexCreateLinear( header.width, header.height, N3DS_TEX_ETC1, 0, N3DS_WRAP_CLAMP );
		if ( textures[target] == NULL ) {
			return ( shown >= 0 ) ? textures[shown] : NULL;
		}
	}
	memcpy( N3DSGPU_TexData( textures[target] ), frameData, header.frameBytes );
	N3DSGPU_TexFlush( textures[target], header.frameBytes );
	shown = target;
	shownFrame = frame;
	return textures[shown];
}

cinData_t idCinematic_3DS::ImageForTime( int milliseconds ) {
	cinData_t c;
	memset( &c, 0, sizeof( c ) );
	if ( file != NULL ) {
		FrameForTime( milliseconds );
		c.imageWidth = header.width;
		c.imageHeight = header.height;
		c.status = ended ? FMV_EOF : FMV_PLAY;
	} else {
		c.status = FMV_EOF;
	}
	return c;
}

idCinematic * N3DS_AllocCinematic() {
	return new (TAG_CINEMATIC) idCinematic_3DS;
}
