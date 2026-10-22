/*
================================================================================================

	New 3DS video (.n3v), written by tools/n3ds/n3dstex from the game's Bink videos
	(base/video/*.bik -> base/video/*.n3v).

	Each frame is an ETC1 texture (power-of-two size, PICA tiled and flipped like every
	texture of the port, one mip level), so the console only copies it to the GPU. Frames
	are stored as changes to the previous one at the granularity of the 8-byte ETC1
	blocks: identical source blocks give identical blocks, so still parts of a video cost
	nothing. Every keyInterval frames a whole frame is stored (looping, seeking back).

	Layout (little-endian):
		n3dsVideoHeader_t
		frame records, each:
			byte type	N3DS_VIDEO_KEY:    frameBytes of ETC1 data follow
						N3DS_VIDEO_DELTA:  a bit per block ( frameBytes / 8 bits, block i
						                   in bit i & 7 of byte i >> 3 ), then the 8-byte
						                   blocks whose bits are set, in order
						N3DS_VIDEO_REPEAT: nothing, the frame is the previous one
		n3dsVideoFrame_t[numFrames] at indexOffset

	No audio: the game's videos play their sound through sound shaders.

================================================================================================
*/
#ifndef __N3DS_VIDEO_H__
#define __N3DS_VIDEO_H__

#include <stdint.h>

#define N3DS_VIDEO_MAGIC	0x4456334Eu		// "N3VD"
#define N3DS_VIDEO_VERSION	1

enum {
	N3DS_VIDEO_KEY		= 0,
	N3DS_VIDEO_DELTA	= 1,
	N3DS_VIDEO_REPEAT	= 2
};

#pragma pack( push, 1 )
struct n3dsVideoHeader_t {
	uint32_t	magic;
	uint32_t	version;
	uint16_t	width;			// texture, powers of two >= 8
	uint16_t	height;
	uint16_t	srcWidth;		// the Bink video
	uint16_t	srcHeight;
	uint32_t	fpsMilli;		// frames per second x 1000
	uint32_t	numFrames;
	uint32_t	keyInterval;
	uint32_t	indexOffset;
	uint32_t	frameBytes;		// width * height / 2
	uint32_t	flags;
	uint32_t	reserved[2];
};

struct n3dsVideoFrame_t {
	uint32_t	offset;
	uint32_t	size;
};
#pragma pack( pop )

typedef char n3dsVideoHeaderSizeCheck[ sizeof( n3dsVideoHeader_t ) == 48 ? 1 : -1 ];

#endif
