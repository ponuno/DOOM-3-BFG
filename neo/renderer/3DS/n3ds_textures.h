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
void		N3DS_TexStatsLine( idStr & line );
void		N3DS_TexLogMemory();						// memory diagnostics at MEM checkpoints

#endif
