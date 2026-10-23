/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

===========================================================================
*/

/*
================================================================================================

	New 3DS bottom screen: a touch panel for the console's own features.

	The bottom screen is two libctru text consoles (8x8 cells, 40 x 30, rows and columns
	counted from 0 here; the escape codes count from 1):
		row 0		title
		rows 1-3	GYRO AIM (toggles n3ds_gyro)		GPU TEST (starts n3ds_gpuBench; only with
					n3ds_debug 1, else GYRO AIM takes the whole row)
		rows 5-7	FPS COUNTER (toggles n3ds_showFPS)	HIDE SCREEN
		rows 9-12	C-STICK SENSITIVITY slider (n3ds_cstickScale, tap or drag)
		row 14		status line (gyro, GPU test)
		row 15		separator
		rows 16-29	where the log is written; the log itself only after a fatal error
					(Sys_LogWrite prints to stdout, which goes there, only then)

	HIDE SCREEN switches the bottom screen's backlight off. The touch screen still works:
	a tap anywhere switches it on again, and that tap does nothing else (it is swallowed
	until the stylus or finger is lifted). The backlight is switched on again for a fatal
	error and at exit, and off again when the game comes back from the HOME menu or sleep
	(the system may have lit it).

	The panel is redrawn under the log lock (Sys_N3DSConsoleLock) so it never cuts into a
	log line, and only when something it shows changed. Touch is read once per frame on the
	main thread (IN_Frame).

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
#include <3ds/services/gsplcd.h>
#include <3ds/os.h>
#include <3ds/result.h>
#include <3ds/services/apt.h>
}
#include <stdio.h>

extern idCVar n3ds_gyro;

static PrintConsole	panelConsole;
static PrintConsole	logConsole;
static bool			bottomReady = false;
static bool			hidden = false;			// backlight off
static bool			swallowTouch = false;	// the tap that woke the screen, until released
static bool			touchWasDown = false;
static bool			sliderDragging = false;
static int			lastStatusMs = 0;

// what the panel shows, to redraw only on a change
static int			drawnState = -1;
static int			drawnBench = -1;
static int			drawnFPS = -1;
static float		drawnScale = -1.0f;

static const int	PANEL_ROWS = 16;

// a touch area in cells: columns c0..c1-1, rows r0..r1-1
struct cellRect_t {
	int	c0, r0, c1, r1;
	bool Contains( const touchPosition & t ) const {
		return t.px >= c0 * 8 && t.px < c1 * 8 && t.py >= r0 * 8 && t.py < r1 * 8;
	}
};
static const cellRect_t	gyroButton		= { 2, 1, 20, 4 };
static const cellRect_t	gyroButtonWide	= { 2, 1, 38, 4 };		// without GPU TEST (n3ds_debug 0)
static const cellRect_t	benchButton		= { 21, 1, 38, 4 };
static const cellRect_t	fpsButton		= { 2, 5, 20, 8 };
static const cellRect_t	hideButton		= { 21, 5, 38, 8 };
static const cellRect_t	sliderArea		= { 0, 9, 40, 13 };		// generous: easy to hit and drag in
static const int		SLIDER_C0 = 2, SLIDER_C1 = 38;			// the bar: columns 2-37, rows 11-12

static const float		SCALE_MIN = 0.25f, SCALE_MAX = 3.0f, SCALE_STEP = 0.05f;

bool RB_N3DS_BenchRunning();

/*
	The gsp::Lcd session is opened only for the moment the backlight is switched. Kept open
	(0136-0151) it froze the console on the HOME button: the HOME menu needs gsp::Lcd too
	and waited for a session forever, with the game's last frame left on the screen.
*/
static void N3DS_Backlight( bool on ) {
	if ( R_FAILED( gspLcdInit() ) ) {
		return;
	}
	if ( on ) {
		GSPLCD_PowerOnBacklight( GSPLCD_SCREEN_BOTTOM );
	} else {
		GSPLCD_PowerOffBacklight( GSPLCD_SCREEN_BOTTOM );
	}
	gspLcdExit();
}

// back from the HOME menu or sleep the system may have lit the screen: keep it as chosen
static aptHookCookie	aptCookie;
static void N3DS_BottomAptHook( APT_HookType hook, void * param ) {
	if ( ( hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP ) && hidden ) {
		N3DS_Backlight( false );
	}
}

void N3DS_BottomInit() {
	consoleInit( GFX_BOTTOM, &panelConsole );
	consoleInit( GFX_BOTTOM, &logConsole );
	consoleSetWindow( &panelConsole, 0, 0, 40, PANEL_ROWS );
	consoleSetWindow( &logConsole, 0, PANEL_ROWS, 40, 30 - PANEL_ROWS );
	consoleSelect( &logConsole );
	printf( "\x1b[2;2H\x1b[37mThe log is written to\x1b[0m" );
	printf( "\x1b[3;2H\x1b[36m%s\x1b[0m", N3DS_LOG_FILE );
	aptHook( &aptCookie, N3DS_BottomAptHook, NULL );
	bottomReady = true;
}

static void N3DS_SetHidden( bool hide ) {
	if ( hide == hidden ) {
		return;
	}
	N3DS_Backlight( !hide );
	hidden = hide;
}

// a fatal error is about to be printed: the log area is cleared for it, the screen lit
void N3DS_BottomShowLog() {
	if ( !bottomReady ) {
		return;
	}
	N3DS_SetHidden( false );
	consoleSelect( &logConsole );
	consoleClear();
}

// watchdog thread, the game frozen: its state over the log area, without the log lock (the
// frozen thread may hold it); the screen is lit so it can be read
void N3DS_BottomWatchdog( const char * text ) {
	if ( !bottomReady ) {
		return;
	}
	N3DS_SetHidden( false );
	consoleSelect( &logConsole );
	consoleClear();
	fputs( text, stdout );
	fflush( stdout );
}

// exit: leave the backlight on
void N3DS_BottomShutdown() {
	if ( !bottomReady ) {
		return;
	}
	N3DS_SetHidden( false );
	aptUnhook( &aptCookie );
}

// panel console selected, log lock held
static void N3DS_DrawButton( const cellRect_t & r, const char * colour, const char * label ) {
	const int width = r.c1 - r.c0;
	char line[48];
	for ( int row = r.r0; row < r.r1; row++ ) {
		if ( row == ( r.r0 + r.r1 - 1 ) / 2 ) {
			const int len = Min( (int)strlen( label ), width );
			const int left = ( width - len ) / 2;
			idStr::snPrintf( line, sizeof( line ), "%*s%.*s%*s", left, "", len, label, width - len - left, "" );
		} else {
			idStr::snPrintf( line, sizeof( line ), "%*s", width, "" );
		}
		printf( "\x1b[%d;%dH%s%s\x1b[0m", row + 1, r.c0 + 1, colour, line );
	}
}

static void N3DS_PanelDraw( int state, float yawDps, float pitchDps, bool bench, bool fps, float scale ) {
	// title bar
	printf( "\x1b[1;1H\x1b[37;44m DOOM 3 BFG  New 3DS                    \x1b[0m" );

	const bool debug = Sys_N3DSDebug();
	N3DS_DrawButton( debug ? gyroButton : gyroButtonWide, state != 0 ? "\x1b[30;42m" : "\x1b[37;41m", state != 0 ? "GYRO AIM: ON" : "GYRO AIM: OFF" );
	if ( debug ) {
		N3DS_DrawButton( benchButton, bench ? "\x1b[30;43m" : "\x1b[37;45m", bench ? "RUNNING" : "GPU TEST" );
	}
	N3DS_DrawButton( fpsButton, fps ? "\x1b[30;42m" : "\x1b[37;41m", fps ? "FPS COUNTER: ON" : "FPS COUNTER: OFF" );
	N3DS_DrawButton( hideButton, "\x1b[37;46m", "HIDE SCREEN" );

	// slider: label with the value, then the bar (filled part and the rest)
	char line[48];
	idStr::snPrintf( line, sizeof( line ), " C-STICK SENSITIVITY  %.2fx", scale );
	printf( "\x1b[10;1H\x1b[37m%-40s\x1b[0m", line );
	const int cells = SLIDER_C1 - SLIDER_C0;
	const int filled = idMath::ClampInt( 1, cells, idMath::Ftoi( ( scale - SCALE_MIN ) / ( SCALE_MAX - SCALE_MIN ) * cells + 0.5f ) );
	for ( int row = 11; row <= 12; row++ ) {
		printf( "\x1b[%d;%dH\x1b[30;47m%*s\x1b[0m\x1b[37;40m%*s\x1b[0m", row + 1, SLIDER_C0 + 1, filled, "", cells - filled, "" );
		printf( "\x1b[%d;%dH\x1b[34m|\x1b[0m", row + 1, SLIDER_C1 + 1 );
	}
	printf( "\x1b[14;1H\x1b[37m  0.25x     tap or drag          3.00x  \x1b[0m" );

	// status
	if ( bench ) {
		printf( "\x1b[15;1H\x1b[33m GPU test: stand still ~20 s            \x1b[0m" );
	} else if ( state == 0 ) {
		printf( "\x1b[15;1H\x1b[37m gyro off                               \x1b[0m" );
	} else if ( state == 1 ) {
		printf( "\x1b[15;1H\x1b[33m calibrating: hold the console still    \x1b[0m" );
	} else {
		idStr::snPrintf( line, sizeof( line ), " yaw %+6.1f  pitch %+6.1f deg/s", yawDps, pitchDps );
		printf( "\x1b[15;1H\x1b[32m%-40s\x1b[0m", line );
	}
	printf( "\x1b[16;1H\x1b[34m----------------------------------------\x1b[0m" );
}

static void N3DS_PanelRefresh( bool force ) {
	float yawDps = 0.0f, pitchDps = 0.0f;
	const int state = Sys_N3DSGyroState( yawDps, pitchDps );
	const int now = Sys_Milliseconds();
	const int bench = RB_N3DS_BenchRunning() ? 1 : 0;
	const int fps = cvarSystem->GetCVarBool( "n3ds_showFPS" ) ? 1 : 0;
	const float scale = cvarSystem->GetCVarFloat( "n3ds_cstickScale" );
	// the rates line is refreshed twice a second while the gyro is on
	if ( !force && state == drawnState && bench == drawnBench && fps == drawnFPS && scale == drawnScale &&
			( state != 2 || now - lastStatusMs < 500 ) ) {
		return;
	}
	drawnState = state;
	drawnBench = bench;
	drawnFPS = fps;
	drawnScale = scale;
	lastStatusMs = now;
	Sys_N3DSConsoleLock();
	consoleSelect( &panelConsole );
	N3DS_PanelDraw( state, yawDps, pitchDps, bench != 0, fps != 0, scale );
	consoleSelect( &logConsole );
	Sys_N3DSConsoleUnlock();
}

static void N3DS_SliderSet( int px ) {
	const float x0 = SLIDER_C0 * 8.0f, x1 = SLIDER_C1 * 8.0f;
	const float f = idMath::ClampFloat( 0.0f, 1.0f, ( px - x0 ) / ( x1 - x0 ) );
	float scale = SCALE_MIN + f * ( SCALE_MAX - SCALE_MIN );
	scale = idMath::ClampFloat( SCALE_MIN, SCALE_MAX, idMath::Floor( scale / SCALE_STEP + 0.5f ) * SCALE_STEP );
	if ( scale != cvarSystem->GetCVarFloat( "n3ds_cstickScale" ) ) {
		cvarSystem->SetCVarFloat( "n3ds_cstickScale", scale );
	}
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
	touchPosition t;
	if ( down ) {
		hidTouchRead( &t );
	}
	const bool pressed = down && !touchWasDown;
	touchWasDown = down;

	if ( !down ) {
		swallowTouch = false;
		if ( sliderDragging ) {
			sliderDragging = false;
			common->Printf( "C-stick sensitivity %.2fx (touch)\n", cvarSystem->GetCVarFloat( "n3ds_cstickScale" ) );
		}
	}
	if ( hidden ) {
		// any tap lights the screen again and does nothing else
		if ( pressed ) {
			N3DS_SetHidden( false );
			swallowTouch = true;
			N3DS_PanelRefresh( true );
		}
		return;
	}
	if ( swallowTouch ) {
		N3DS_PanelRefresh( false );
		return;
	}

	if ( pressed ) {
		const bool debug = Sys_N3DSDebug();
		if ( ( debug ? gyroButton : gyroButtonWide ).Contains( t ) ) {
			n3ds_gyro.SetBool( !n3ds_gyro.GetBool() );
			common->Printf( "gyro aim %s (touch)\n", n3ds_gyro.GetBool() ? "on" : "off" );
		} else if ( debug && benchButton.Contains( t ) && !RB_N3DS_BenchRunning() ) {
			cvarSystem->SetCVarBool( "n3ds_gpuBench", true );
			common->Printf( "GPU TEST: asked for (touch)\n" );
		} else if ( fpsButton.Contains( t ) ) {
			const bool on = !cvarSystem->GetCVarBool( "n3ds_showFPS" );
			cvarSystem->SetCVarBool( "n3ds_showFPS", on );
			common->Printf( "FPS counter %s (touch)\n", on ? "on" : "off" );
		} else if ( hideButton.Contains( t ) ) {
			N3DS_SetHidden( true );
			swallowTouch = true;		// lifting the finger must not wake it right away
			common->Printf( "bottom screen off (touch; a tap switches it on)\n" );
			return;
		} else if ( sliderArea.Contains( t ) ) {
			sliderDragging = true;
		}
	}
	if ( down && sliderDragging ) {
		N3DS_SliderSet( t.px );
	}
	N3DS_PanelRefresh( false );
}
