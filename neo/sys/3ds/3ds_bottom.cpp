/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

===========================================================================
*/

/*
================================================================================================

	New 3DS bottom screen: a touch panel for the console's own features above the log.

	The bottom screen is two libctru text consoles (8x8 cells, 40 x 30):
		rows 0-5	panel: title, the GYRO AIM button (touch toggles n3ds_gyro), a status
					line, a separator
		rows 6-29	the log, as before (stdout goes there)
	The panel is redrawn under the log lock (Sys_N3DSConsoleLock) so it never cuts into a
	log line. Touch is read once per frame on the main thread (IN_Frame).

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "3ds_local.h"

extern "C" {
#include <3ds/types.h>
#include <3ds/gfx.h>
#include <3ds/console.h>
#include <3ds/services/hid.h>
#include <3ds/os.h>
}
#include <stdio.h>

extern idCVar n3ds_gyro;

static PrintConsole	panelConsole;
static PrintConsole	logConsole;
static bool			bottomReady = false;
static int			drawnState = -1;		// gyro state the panel shows
static int			lastStatusMs = 0;
static bool			touchWasDown = false;

static const int	PANEL_ROWS = 6;
// the button: rows 1-3, columns 2-25 (pixels: 8 per cell)
static const int	BUTTON_X0 = 2 * 8, BUTTON_X1 = 26 * 8;
static const int	BUTTON_Y0 = 1 * 8, BUTTON_Y1 = 4 * 8;

void N3DS_BottomInit() {
	consoleInit( GFX_BOTTOM, &panelConsole );
	consoleInit( GFX_BOTTOM, &logConsole );
	consoleSetWindow( &panelConsole, 0, 0, 40, PANEL_ROWS );
	consoleSetWindow( &logConsole, 0, PANEL_ROWS, 40, 30 - PANEL_ROWS );
	consoleSelect( &logConsole );
	bottomReady = true;
	drawnState = -1;
}

// panel console selected, log lock held
static void N3DS_PanelDraw( int state, float yawDps, float pitchDps ) {
	// title bar
	printf( "\x1b[1;1H\x1b[37;44m DOOM 3 BFG  New 3DS                    \x1b[0m" );
	// button
	const bool on = ( state != 0 );
	const char * colour = on ? "\x1b[30;42m" : "\x1b[37;41m";
	printf( "\x1b[2;3H%s                        \x1b[0m", colour );
	printf( "\x1b[3;3H%s   GYRO AIM:  %-3s        \x1b[0m", colour, on ? "ON" : "OFF" );
	printf( "\x1b[4;3H%s                        \x1b[0m", colour );
	printf( "\x1b[2;29H\x1b[37mtouch to\x1b[0m" );
	printf( "\x1b[3;29H\x1b[37mswitch  \x1b[0m" );
	// status
	if ( state == 0 ) {
		printf( "\x1b[5;1H\x1b[37m gyro off                               \x1b[0m" );
	} else if ( state == 1 ) {
		printf( "\x1b[5;1H\x1b[33m calibrating: hold the console still    \x1b[0m" );
	} else {
		char line[48];
		idStr::snPrintf( line, sizeof( line ), " yaw %+6.1f  pitch %+6.1f deg/s", yawDps, pitchDps );
		printf( "\x1b[5;1H\x1b[32m%-40s\x1b[0m", line );
	}
	printf( "\x1b[6;1H\x1b[34m----------------------------------------\x1b[0m" );
}

static void N3DS_PanelRefresh( bool force ) {
	float yawDps = 0.0f, pitchDps = 0.0f;
	const int state = Sys_N3DSGyroState( yawDps, pitchDps );
	const int now = Sys_Milliseconds();
	// the rates line is refreshed twice a second while the gyro is on
	if ( !force && state == drawnState && ( state != 2 || now - lastStatusMs < 500 ) ) {
		return;
	}
	drawnState = state;
	lastStatusMs = now;
	Sys_N3DSConsoleLock();
	consoleSelect( &panelConsole );
	N3DS_PanelDraw( state, yawDps, pitchDps );
	consoleSelect( &logConsole );
	Sys_N3DSConsoleUnlock();
}

/*
========================
N3DS_BottomFrame

Main thread, after hidScanInput.
========================
*/
void N3DS_BottomFrame() {
	if ( !bottomReady ) {
		return;
	}
	const bool down = ( hidKeysHeld() & KEY_TOUCH ) != 0;
	if ( down && !touchWasDown ) {
		touchPosition t;
		hidTouchRead( &t );
		if ( t.px >= BUTTON_X0 && t.px < BUTTON_X1 && t.py >= BUTTON_Y0 && t.py < BUTTON_Y1 ) {
			n3ds_gyro.SetBool( !n3ds_gyro.GetBool() );
			common->Printf( "gyro aim %s (touch)\n", n3ds_gyro.GetBool() ? "on" : "off" );
		}
	}
	touchWasDown = down;
	N3DS_PanelRefresh( false );
}
