/*
================================================================================================

	New 3DS sound pack (_n3ds_sounds.pak), written by tools/n3ds/n3dstex.

	Every sample of the game's _sound_pc*.resources (generated/...idwav, 16-bit PCM or
	MS ADPCM) re-encoded as DSP-ADPCM, the format the 3DS DSP decodes itself: the CPU
	no longer decodes or mixes anything, it hands the DSP the encoded frames.

	DSP-ADPCM (GameCube format): frames of 8 bytes = 14 samples. Byte 0 is the frame
	header: predictor index (high nibble) and scale shift (low nibble); then 14 signed
	4-bit residuals, high nibble first. A sample decodes as
		s = clamp16( ( ( residual << scale ) << 11 ) + 1024 + c1 * h1 + c2 * h2 ) >> 11
	with h1 / h2 the last two decoded samples and (c1, c2) the predictor's coefficient
	pair (2048 = 1.0). All samples share the pack's 8 pairs (the header), so a voice that
	goes on from its leadin to its looping sample keeps its DSP channel's coefficients.
	Every channel starts with h1 = h2 = 0.

	Layout (little-endian):
		n3dsSoundPackHeader_t
		... sample data: per sample, channel 0 then channel 1, each channelBytes long,
		    the sample at a multiple of N3DS_SOUNDPACK_ALIGN ...
		n3dsSoundPackEntry_t[numEntries], sorted by nameHash (N3DS_PackHash of the
		generated name as the resource table has it, "generated/sound/...idwav")

================================================================================================
*/
#ifndef __N3DS_SOUNDPACK_H__
#define __N3DS_SOUNDPACK_H__

#include <stdint.h>

#define N3DS_SOUNDPACK_MAGIC	0x4E53334Eu		// "N3SN"
#define N3DS_SOUNDPACK_VERSION	1
#define N3DS_SOUNDPACK_ALIGN	32
#define N3DS_DSPADPCM_FRAME_SAMPLES	14
#define N3DS_DSPADPCM_FRAME_BYTES	8

#pragma pack( push, 1 )
struct n3dsSoundPackHeader_t {
	uint32_t	magic;
	uint32_t	version;
	uint32_t	numEntries;
	uint32_t	entryOffset;
	int16_t		coefs[16];			// 8 predictor pairs (c1, c2), 2048 = 1.0
	uint32_t	reserved[4];
};

struct n3dsSoundPackEntry_t {
	uint64_t	nameHash;
	uint32_t	dataOffset;			// in units of N3DS_SOUNDPACK_ALIGN
	uint32_t	numSamples;			// per channel (the sample's playLength)
	uint32_t	sampleRate;
	uint32_t	channelBytes;		// ceil( numSamples / 14 ) * 8
	uint8_t		channels;			// 1 or 2
	uint8_t		flags;
	uint16_t	pad;
	uint32_t	reserved;
};
#pragma pack( pop )

typedef char n3dsSoundPackHeaderSizeCheck[ sizeof( n3dsSoundPackHeader_t ) == 64 ? 1 : -1 ];
typedef char n3dsSoundPackEntrySizeCheck[ sizeof( n3dsSoundPackEntry_t ) == 32 ? 1 : -1 ];

static inline uint32_t N3DS_DspAdpcmBytes( uint32_t samples ) {
	return ( ( samples + N3DS_DSPADPCM_FRAME_SAMPLES - 1 ) / N3DS_DSPADPCM_FRAME_SAMPLES ) * N3DS_DSPADPCM_FRAME_BYTES;
}

#endif
