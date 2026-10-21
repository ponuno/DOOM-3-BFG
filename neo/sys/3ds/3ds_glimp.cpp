/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

/*
================================================================================================

	New 3DS "window" layer (counterpart of win_glimp.cpp).

	Bring-up stage: the renderer runs against the null GL driver
	(renderer/OpenGL/gl_null), so nothing is drawn. The top screen is
	kept black; the bottom screen shows the log console.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../../renderer/tr_local.h"
#include "3ds_local.h"

// libctru headers are C; only <3ds.h> adds extern "C" itself
extern "C" {
#include <3ds/types.h>
#include <3ds/gfx.h>
}

static const int N3DS_TOP_WIDTH		= 400;
static const int N3DS_TOP_HEIGHT	= 240;

/*
========================
ClearTopScreen
========================
*/
static void ClearTopScreen() {
	u16 w, h;
	for ( int i = 0; i < 2; i++ ) {
		u8 * fb = gfxGetFramebuffer( GFX_TOP, GFX_LEFT, &w, &h );
		memset( fb, 0, w * h * gspGetBytesPerPixel( gfxGetScreenFormat( GFX_TOP ) ) );
		gfxFlushBuffers();
		gfxSwapBuffers();
	}
}

/*
========================
GLimp_Init
========================
*/
bool GLimp_Init( glimpParms_t parms ) {
	ClearTopScreen();

	glConfig.isFullscreen = 1;
	glConfig.isStereoPixelFormat = false;
	glConfig.stereoPixelFormatAvailable = false;
	glConfig.nativeScreenWidth = N3DS_TOP_WIDTH;
	glConfig.nativeScreenHeight = N3DS_TOP_HEIGHT;
	glConfig.displayFrequency = 60;
	glConfig.multisamples = 0;
	glConfig.pixelAspect = 1.0f;
	glConfig.physicalScreenWidthInCentimeters = 7.7f;	// New 3DS top screen is ~3.88" diagonal
	glConfig.colorBits = 24;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;

	if ( cvarSystem->GetCVarInteger( "n3ds_renderer" ) == 1 ) {
		RB_N3DS_Init();
	}
	idLib::Printf( "GLimp_Init: %dx%d (%s)\n", glConfig.nativeScreenWidth, glConfig.nativeScreenHeight,
		r_n3dsC3D ? "citro3d" : "null GL" );
	return true;
}

/*
========================
GLimp_SetScreenParms
========================
*/
bool GLimp_SetScreenParms( glimpParms_t parms ) {
	glConfig.nativeScreenWidth = N3DS_TOP_WIDTH;
	glConfig.nativeScreenHeight = N3DS_TOP_HEIGHT;
	return true;
}

/*
========================
GLimp_Shutdown
========================
*/
void GLimp_Shutdown() {
	RB_N3DS_Shutdown();
}

/*
========================
GLimp_SwapBuffers
========================
*/
void GLimp_SwapBuffers() {
}

/*
========================
GLimp_SetGamma
========================
*/
void GLimp_SetGamma( unsigned short red[256], unsigned short green[256], unsigned short blue[256] ) {
}

/*
========================
GLimp_ActivateContext / GLimp_DeactivateContext
========================
*/
void GLimp_ActivateContext() {
}

void GLimp_DeactivateContext() {
}

/*
========================
GLimp_EnableLogging
========================
*/
void GLimp_EnableLogging( bool enable ) {
}

/*
========================
R_GetModeListForDisplay
========================
*/
bool R_GetModeListForDisplay( const int requestedDisplayNum, idList<vidMode_t> & modeList ) {
	modeList.Clear();
	if ( requestedDisplayNum != 0 ) {
		return false;
	}
	vidMode_t mode;
	mode.width = N3DS_TOP_WIDTH;
	mode.height = N3DS_TOP_HEIGHT;
	mode.displayHz = 60;
	modeList.AddUnique( mode );
	return true;
}

/*
========================
DumpAllDisplayDevices
========================
*/
void DumpAllDisplayDevices() {
	common->Printf( "Display: New 3DS top screen %dx%d\n", N3DS_TOP_WIDTH, N3DS_TOP_HEIGHT );
}
