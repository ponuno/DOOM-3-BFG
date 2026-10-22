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
		rows 0-5	panel: title, the GYRO AIM button (touch toggles n3ds_gyro), the GPU TEST
					button (touch starts n3ds_gpuBench, results in the log), a status
					line, a separator
		rows 6-29	where the log is written; the log itself only after a fatal error
					(Sys_LogWrite prints to stdout, which goes there, only then)
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
// GPU TEST: rows 1-3, columns 28-38
static const int	BENCH_X0 = 28 * 8, BENCH_X1 = 39 * 8;
static int			drawnBench = -1;

bool RB_N3DS_BenchRunning();

void N3DS_BottomInit() {
	consoleInit( GFX_BOTTOM, &panelConsole );
	consoleInit( GFX_BOTTOM, &logConsole );
	consoleSetWindow( &panelConsole, 0, 0, 40, PANEL_ROWS );
	consoleSetWindow( &logConsole, 0, PANEL_ROWS, 40, 30 - PANEL_ROWS );
	consoleSelect( &logConsole );
	printf( "\x1b[2;2H\x1b[37mThe log is written to\x1b[0m" );
	printf( "\x1b[3;2H\x1b[36m%s\x1b[0m", N3DS_LOG_FILE );
	bottomReady = true;
	drawnState = -1;
}

// a fatal error is about to be printed: the log area is cleared for it
void N3DS_BottomShowLog() {
	if ( !bottomReady ) {
		return;
	}
	consoleSelect( &logConsole );
	consoleClear();
}

// panel console selected, log lock held
static void N3DS_PanelDraw( int state, float yawDps, float pitchDps, bool bench ) {
	// title bar
	printf( "\x1b[1;1H\x1b[37;44m DOOM 3 BFG  New 3DS                    \x1b[0m" );
	// button
	const bool on = ( state != 0 );
	const char * colour = on ? "\x1b[30;42m" : "\x1b[37;41m";
	printf( "\x1b[2;3H%s                        \x1b[0m", colour );
	printf( "\x1b[3;3H%s   GYRO AIM:  %-3s        \x1b[0m", colour, on ? "ON" : "OFF" );
	printf( "\x1b[4;3H%s                        \x1b[0m", colour );
	// GPU TEST button
	const char * benchColour = bench ? "\x1b[30;43m" : "\x1b[37;45m";
	printf( "\x1b[2;29H%s           \x1b[0m", benchColour );
	printf( "\x1b[3;29H%s%s\x1b[0m", benchColour, bench ? "  RUNNING  " : " GPU TEST  " );
	printf( "\x1b[4;29H%s           \x1b[0m", benchColour );
	// status
	if ( bench ) {
		printf( "\x1b[5;1H\x1b[33m GPU test: stand still ~15 s            \x1b[0m" );
	} else if ( state == 0 ) {
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
	const int bench = RB_N3DS_BenchRunning() ? 1 : 0;
	if ( !force && state == drawnState && bench == drawnBench && ( state != 2 || now - lastStatusMs < 500 ) ) {
		return;
	}
	drawnState = state;
	drawnBench = bench;
	lastStatusMs = now;
	Sys_N3DSConsoleLock();
	consoleSelect( &panelConsole );
	N3DS_PanelDraw( state, yawDps, pitchDps, bench != 0 );
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
		} else if ( t.px >= BENCH_X0 && t.px < BENCH_X1 && t.py >= BUTTON_Y0 && t.py < BUTTON_Y1 && !RB_N3DS_BenchRunning() ) {
			cvarSystem->SetCVarBool( "n3ds_gpuBench", true );
			common->Printf( "GPU TEST: asked for (touch)\n" );
		}
	}
	touchWasDown = down;
	N3DS_PanelRefresh( false );
}
