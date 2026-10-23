/*
================================================================================================

	New 3DS texture cache for the citro3d backend (see n3ds_textures.cpp).
	The load-time hooks (RB_N3DS_TexKeepSize, RB_N3DS_ImageLevel,
	RB_N3DS_ImagePurged) are declared in tr_local.h.

================================================================================================
*/
#ifndef __N3DS_TEXTURES_H__
#define __N3DS_TEXTURES_H__

void		N3DS_TexBeginFrame( bool has3D );
void *		N3DS_TexForImage( idImage * image );		// GPU texture or NULL
void		N3DS_TexPrefetch( idImage ** images, int count );	// read ahead (backend, once per main view)
bool		N3DS_TexIsAlphaOnly( idImage * image );	// A8 on the GPU (fonts, coverage): no colour
bool		N3DS_TexIsNormalMap( idImage * image );	// HILO8 from the texture pack: usable by the bump unit
void		N3DS_TexStatsLine( idStr & line );
void		N3DS_TexReleaseRAM( const char * why );	// empty the RAM cache (owning thread only)
// the running counters behind the stats line (reset with it): taken before and after a frame
struct n3dsTexCounters_t {
	int		uploads;
	uint64	uploadUs;
	int		evictions;
	int		reads;
	int		waits;
};
void		N3DS_TexCounters( n3dsTexCounters_t & c );
// diagnostics: "name: ETC1 128x128 6 lv @18000000, pack, VRAM" (or why there is no GPU texture)
void		N3DS_TexDescribe( idImage * image, idStr & out );
const char *N3DS_TexNameForAddress( unsigned int address );	// resident texture whose data is there
void		N3DS_TexLogMemory();						// memory diagnostics at MEM checkpoints

#endif
