/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "../idlib/precompiled.h"
#pragma hdrstop

#include "Common_local.h"
#include "../renderer/Image.h"
#include "../renderer/ImageOpts.h"
#include "../../doomclassic/doom/doomlib.h"
#include "../../doomclassic/doom/globaldata.h"

/*

New for tech4x:

Unlike previous SMP work, the actual GPU command drawing is done in the main thread, which avoids the
OpenGL problems with needing windows to be created by the same thread that creates the context, as well
as the issues with passing context ownership back and forth on the 360.

The game tic and the generation of the draw command list is now run in a separate thread, and overlapped
with the interpretation of the previous draw command list.

While the game tic should be nicely contained, the draw command generation winds through the user interface
code, and is potentially hazardous.  For now, the overlap will be restricted to the renderer back end,
which should also be nicely contained.

*/
#define DEFAULT_FIXED_TIC "0"
#define DEFAULT_NO_SLEEP "0"

idCVar com_deltaTimeClamp( "com_deltaTimeClamp", "50", CVAR_INTEGER, "don't process more than this time in a single frame" );

idCVar com_fixedTic( "com_fixedTic", DEFAULT_FIXED_TIC, CVAR_BOOL, "run a single game frame per render frame" );
idCVar com_noSleep( "com_noSleep", DEFAULT_NO_SLEEP, CVAR_BOOL, "don't sleep if the game is running too fast" );
idCVar com_smp( "com_smp", "1", CVAR_BOOL|CVAR_SYSTEM|CVAR_NOCHEAT, "run the game and draw code in a separate thread" );
idCVar com_aviDemoSamples( "com_aviDemoSamples", "16", CVAR_SYSTEM, "" );
idCVar com_aviDemoWidth( "com_aviDemoWidth", "256", CVAR_SYSTEM, "" );
idCVar com_aviDemoHeight( "com_aviDemoHeight", "256", CVAR_SYSTEM, "" );
idCVar com_skipGameDraw( "com_skipGameDraw", "0", CVAR_SYSTEM | CVAR_BOOL, "" );

idCVar com_sleepGame( "com_sleepGame", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the game time" );
idCVar com_sleepDraw( "com_sleepDraw", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the draw time" );
idCVar com_sleepRender( "com_sleepRender", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the render time" );

idCVar net_drawDebugHud( "net_drawDebugHud", "0", CVAR_SYSTEM | CVAR_INTEGER, "0 = None, 1 = Hud 1, 2 = Hud 2, 3 = Snapshots" );

idCVar timescale( "timescale", "1", CVAR_SYSTEM | CVAR_FLOAT, "Number of game frames to run per render frame", 0.001f, 100.0f );

extern idCVar in_useJoystick;
extern idCVar in_joystickRumble;

#if defined( ID_3DS )
// game thread -> main thread perf counters (approximate, no locking needed)
static int		n3dsPerfTicks = 0;
static uint64	n3dsPerfGameUs = 0;
static uint64	n3dsPerfGameMaxUs = 0;
static uint64	n3dsPerfDrawUs = 0;
static uint64	n3dsWaitGameUs = 0;		// main thread waiting for the game / draw thread, this frame

// main thread time per part of idCommonLocal::Frame, this frame (N3DS_MARK adds the time since
// the previous mark to a part)
enum {
	N3DS_MAIN_OUTSIDE,		// between two Frame() calls (the platform's loop)
	N3DS_MAIN_EVENTS,		// events, input
	N3DS_MAIN_SWAP,			// SwapCommandBuffers
	N3DS_MAIN_PACE,			// waiting for the next game tick
	N3DS_MAIN_SESSION,		// session, GUI events, usercmds, starting the game thread
	N3DS_MAIN_BACKEND,		// RenderCommandBuffers
	N3DS_MAIN_WAITGAME,		// waiting for the game / draw thread
	N3DS_MAIN_SEND,			// usercmds, snapshots
	N3DS_MAIN_SOUND,		// soundSystem->Render
	N3DS_MAIN_REST,			// game return, lobby, frame done
	N3DS_MAIN_PARTS
};
static uint64	n3dsMainUs[N3DS_MAIN_PARTS];
static uint64	n3dsMainMark = 0;
static void N3DS_MARK( int part ) {
	const uint64 now = Sys_Microseconds();
	if ( n3dsMainMark != 0 ) {
		n3dsMainUs[part] += now - n3dsMainMark;
	}
	n3dsMainMark = now;
}
idCVar n3ds_spikeMs( "n3ds_spikeMs", "36", CVAR_SYSTEM | CVAR_FLOAT, "New 3DS: log a SPIKE line with the frame's breakdown for every frame longer than this (ms; 0 = off). At 30 fps a frame over 33.3 ms is shown for 50 ms", 0.0f, 1000.0f );

/*
========================
N3DS_SpikeFrame

Once per frame on the main thread. The PERF lines average over 2 s, which hides the single
long frames that make moving feel uneven; a frame over n3ds_spikeMs gets its own line: what
the game / draw thread did since the last frame (ticks, game, draw), how long the main thread
waited for it, and the back end's frame (its own CPU time, the waits for the GPU and vblank,
the GPU time, draws, textures uploaded / evicted / read).
========================
*/
static void N3DS_SpikeFrame( bool mapSpawned ) {
	static uint64 lastUs = 0;
	static uint64 lastGameUs = 0, lastDrawUs = 0;
	static int lastTicks = 0;
	static int windowStartMs = 0, windowLines = 0;
	const uint64 now = Sys_Microseconds();
	const uint64 gameUs = n3dsPerfGameUs, drawUs = n3dsPerfDrawUs;
	const int ticks = n3dsPerfTicks;
	const float frameMs = lastUs != 0 ? ( now - lastUs ) * 0.001f : 0.0f;
	// the PERF block resets the counters every 2 s: a smaller value means a new window
	const float gameMs = ( gameUs >= lastGameUs ? gameUs - lastGameUs : gameUs ) * 0.001f;
	const float drawMs = ( drawUs >= lastDrawUs ? drawUs - lastDrawUs : drawUs ) * 0.001f;
	const int frameTicks = ticks >= lastTicks ? ticks - lastTicks : ticks;
	lastUs = now;
	lastGameUs = gameUs;
	lastDrawUs = drawUs;
	lastTicks = ticks;
	extern float N3DS_SoundLockWaitMs();		// main thread, since the last call
	const float soundLockMs = N3DS_SoundLockWaitMs();
	const float limit = n3ds_spikeMs.GetFloat();
	if ( !mapSpawned || limit <= 0.0f || frameMs <= limit || !Sys_N3DSDebug() ) {
		return;
	}
	const int nowMs = Sys_Milliseconds();
	if ( nowMs - windowStartMs >= 1000 ) {
		windowStartMs = nowMs;
		windowLines = 0;
	}
	if ( ++windowLines > 6 ) {
		return;		// at most 6 lines a second
	}
	n3dsBackendFrame_t b;
	RB_N3DS_LastFrame( b );
	const float backCpu = b.totalMs - b.waitGpuMs - b.waitVBlankMs;
	common->Printf( "SPIKE %.1f ms: game thread %d ticks %.1f ms + draw %.1f ms, main waited for it %.1f ms | back end %.1f ms (CPU %.1f, waited GPU %.1f, vblank %.1f), GPU %.1f ms, %d draws (%d light) %d tris | tex %d uploads %.1f ms, %d evicted, %d reads, %d waits\n",
		frameMs, frameTicks, gameMs, drawMs, n3dsWaitGameUs * 0.001f, b.totalMs, backCpu, b.waitGpuMs, b.waitVBlankMs, b.gpuMs,
		b.draws, b.lightDraws, b.tris, b.texUploads, b.texUploadMs, b.texEvictions, b.texReads, b.texWaits );
	const float * m = NULL;
	float mainMs[N3DS_MAIN_PARTS];
	for ( int i = 0; i < N3DS_MAIN_PARTS; i++ ) {
		mainMs[i] = n3dsMainUs[i] * 0.001f;
	}
	m = mainMs;
	common->Printf( "SPIKE   main thread: outside Frame %.1f, events %.1f, swap %.1f, waiting for the tick %.1f, session/usercmds %.1f, back end %.1f, waiting for the game thread %.1f, send %.1f, sound %.1f (lock waits %.1f), rest %.1f ms\n",
		m[N3DS_MAIN_OUTSIDE], m[N3DS_MAIN_EVENTS], m[N3DS_MAIN_SWAP], m[N3DS_MAIN_PACE], m[N3DS_MAIN_SESSION], m[N3DS_MAIN_BACKEND],
		m[N3DS_MAIN_WAITGAME], m[N3DS_MAIN_SEND], m[N3DS_MAIN_SOUND], soundLockMs, m[N3DS_MAIN_REST] );
	// the lines just written are not counted in the next frame
	lastUs = Sys_Microseconds();
}
#endif

/*
===============
idGameThread::Run

Run in a background thread for performance, but can also
be called directly in the foreground thread for comparison.
===============
*/
int idGameThread::Run() {
	commonLocal.frameTiming.startGameTime = Sys_Microseconds();

	// debugging tool to test frame dropping behavior
	if ( com_sleepGame.GetInteger() ) {
		Sys_Sleep( com_sleepGame.GetInteger() );
	}

	if ( numGameFrames == 0 ) {
		// Ensure there's no stale gameReturn data from a paused game
		ret = gameReturn_t();
	}

	if ( isClient ) {
		// run the game logic
		for ( int i = 0; i < numGameFrames; i++ ) {
			SCOPED_PROFILE_EVENT( "Client Prediction" );
			if ( userCmdMgr ) {
				game->ClientRunFrame( *userCmdMgr, ( i == numGameFrames - 1 ), ret );
			}
			if ( ret.syncNextGameFrame || ret.sessionCommand[0] != 0 ) {
				break;
			}
		}
	} else {
		// run the game logic
		for ( int i = 0; i < numGameFrames; i++ ) {
			SCOPED_PROFILE_EVENT( "GameTic" );
			N3DS_STAGE( N3DS_STAGE_GAME, "game tic" );
			if ( userCmdMgr ) {
				game->RunFrame( *userCmdMgr, ret );
			}
			if ( ret.syncNextGameFrame || ret.sessionCommand[0] != 0 ) {
				break;
			}
		}
	}

	// we should have consumed all of our usercmds
	if ( userCmdMgr ) {
		if ( userCmdMgr->HasUserCmdForPlayer( game->GetLocalClientNum() ) && common->GetCurrentGame() == DOOM3_BFG ) {
			idLib::Printf( "idGameThread::Run: didn't consume all usercmds\n" );
		}
	}

	commonLocal.frameTiming.finishGameTime = Sys_Microseconds();
	N3DS_STAGE( N3DS_STAGE_GAME, "game draw" );

#if defined( ID_3DS )
	{
		const uint64 gameUs = commonLocal.frameTiming.finishGameTime - commonLocal.frameTiming.startGameTime;
		n3dsPerfTicks += numGameFrames;
		n3dsPerfGameUs += gameUs;
		if ( gameUs > n3dsPerfGameMaxUs ) {
			n3dsPerfGameMaxUs = gameUs;
		}
	}
#endif

	SetThreadGameTime( ( commonLocal.frameTiming.finishGameTime - commonLocal.frameTiming.startGameTime ) / 1000 );

	// build render commands and geometry
	{
		SCOPED_PROFILE_EVENT( "Draw" );
		commonLocal.Draw();
	}

	commonLocal.frameTiming.finishDrawTime = Sys_Microseconds();
	N3DS_STAGE( N3DS_STAGE_GAME, "idle" );
#if defined( ID_3DS )
	n3dsPerfDrawUs += commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.finishGameTime;
#endif

	SetThreadRenderTime( ( commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.finishGameTime ) / 1000 );

	SetThreadTotalTime( ( commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.startGameTime ) / 1000 );

	return 0;
}

/*
===============
idGameThread::RunGameAndDraw

===============
*/
gameReturn_t idGameThread::RunGameAndDraw( int numGameFrames_, idUserCmdMgr & userCmdMgr_, bool isClient_, int startGameFrame ) {
	// this should always immediately return
	this->WaitForThread();

	// save the usercmds for the background thread to pick up
	userCmdMgr = &userCmdMgr_;

	isClient = isClient_;

	// grab the return value created by the last thread execution
	gameReturn_t latchedRet = ret;

	numGameFrames = numGameFrames_;

	// start the thread going
	if ( com_smp.GetBool() == false ) {
		// run it in the main thread so PIX profiling catches everything
		Run();
	} else {
		this->SignalWork();
	}

	// return the latched result while the thread runs in the background
	return latchedRet;
}


/*
===============
idCommonLocal::DrawWipeModel

Draw the fade material over everything that has been drawn
===============
*/
void idCommonLocal::DrawWipeModel() {

	if ( wipeStartTime >= wipeStopTime ) {
		return;
	}

	int currentTime = Sys_Milliseconds();

	if ( !wipeHold && currentTime > wipeStopTime ) {
		return;
	}

	float fade = ( float )( currentTime - wipeStartTime ) / ( wipeStopTime - wipeStartTime );
	renderSystem->SetColor4( 1, 1, 1, fade );
	renderSystem->DrawStretchPic( 0, 0, 640, 480, 0, 0, 1, 1, wipeMaterial );
}

/*
===============
idCommonLocal::Draw
===============
*/
void idCommonLocal::Draw() {
	// debugging tool to test frame dropping behavior
	if ( com_sleepDraw.GetInteger() ) {
		Sys_Sleep( com_sleepDraw.GetInteger() );
	}

	if ( loadGUI != NULL ) {
		loadGUI->Render( renderSystem, Sys_Milliseconds() );
	} else if ( currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC ) {
		const float sysWidth = renderSystem->GetWidth() * renderSystem->GetPixelAspect();
		const float sysHeight = renderSystem->GetHeight();
		const float sysAspect = sysWidth / sysHeight;
		const float doomAspect = 4.0f / 3.0f;
		const float adjustment = sysAspect / doomAspect;
		const float barHeight = ( adjustment >= 1.0f ) ? 0.0f : ( 1.0f - adjustment ) * (float)SCREEN_HEIGHT * 0.25f;
		const float barWidth = ( adjustment <= 1.0f ) ? 0.0f : ( adjustment - 1.0f ) * (float)SCREEN_WIDTH * 0.25f;
		if ( barHeight > 0.0f ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, SCREEN_WIDTH, barHeight, 0, 0, 1, 1, whiteMaterial );
			renderSystem->DrawStretchPic( 0, SCREEN_HEIGHT - barHeight, SCREEN_WIDTH, barHeight, 0, 0, 1, 1, whiteMaterial );
		}
		if ( barWidth > 0.0f ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, barWidth, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
			renderSystem->DrawStretchPic( SCREEN_WIDTH - barWidth, 0, barWidth, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
		}
		renderSystem->SetColor4( 1, 1, 1, 1 );
		renderSystem->DrawStretchPic( barWidth, barHeight, SCREEN_WIDTH - barWidth * 2.0f, SCREEN_HEIGHT - barHeight * 2.0f, 0, 0, 1, 1, doomClassicMaterial );
	} else if ( game && game->Shell_IsActive() ) {
		bool gameDraw = game->Draw( game->GetLocalClientNum() );
		if ( !gameDraw ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
		}
		game->Shell_Render();
	} else if ( readDemo ) {
		renderWorld->RenderScene( &currentDemoRenderView );
		renderSystem->DrawDemoPics();
	} else if ( mapSpawned ) {
		bool gameDraw = false;
		// normal drawing for both single and multi player
		if ( !com_skipGameDraw.GetBool() && Game()->GetLocalClientNum() >= 0 ) {
			// draw the game view
			int	start = Sys_Milliseconds();
			if ( game ) {
				gameDraw = game->Draw( Game()->GetLocalClientNum() );
			}
			int end = Sys_Milliseconds();
			time_gameDraw += ( end - start );	// note time used for com_speeds
		}
		if ( !gameDraw ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, 640, 480, 0, 0, 1, 1, whiteMaterial );
		}

		// save off the 2D drawing from the game
		if ( writeDemo ) {
			renderSystem->WriteDemoPics();
		}
	} else {
		renderSystem->SetColor4( 0, 0, 0, 1 );
		renderSystem->DrawStretchPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
	}

	{
		SCOPED_PROFILE_EVENT( "Post-Draw" );

		// draw the wipe material on top of this if it hasn't completed yet
		DrawWipeModel();

		Dialog().Render( loadGUI != NULL );

		// draw the half console / notify console on top of everything
		console->Draw( false );
	}
}

/*
===============
idCommonLocal::UpdateScreen

This is an out-of-sequence screen update, not the normal game rendering
===============
*/
void idCommonLocal::UpdateScreen( bool captureToImage ) {
	if ( insideUpdateScreen ) {
		return;
	}
	insideUpdateScreen = true;

	// make sure the game / draw thread has completed
	gameThread.WaitForThread();

	// release the mouse capture back to the desktop
	Sys_GrabMouseCursor( false );

	// build all the draw commands without running a new game tic
	Draw();

	if ( captureToImage ) {
		renderSystem->CaptureRenderToImage( "_currentRender", false );
	}

	// this should exit right after vsync, with the GPU idle and ready to draw
	const emptyCommand_t * cmd = renderSystem->SwapCommandBuffers( &time_frontend, &time_backend, &time_shadows, &time_gpu );

	// get the GPU busy with new commands
	renderSystem->RenderCommandBuffers( cmd );

	insideUpdateScreen = false;
}
/*
================
idCommonLocal::ProcessGameReturn
================
*/
void idCommonLocal::ProcessGameReturn( const gameReturn_t & ret ) {
	// set joystick rumble
	if ( in_useJoystick.GetBool() && in_joystickRumble.GetBool() && !game->Shell_IsActive() && session->GetSignInManager().GetMasterInputDevice() >= 0 ) {
		Sys_SetRumble( session->GetSignInManager().GetMasterInputDevice(), ret.vibrationLow, ret.vibrationHigh );		// Only set the rumble on the active controller
	} else {
		for ( int i = 0; i < MAX_INPUT_DEVICES; i++ ) {
			Sys_SetRumble( i, 0, 0 );
		}
	}

	syncNextGameFrame = ret.syncNextGameFrame;

	if ( ret.sessionCommand[0] ) {
		idCmdArgs args;

		args.TokenizeString( ret.sessionCommand, false );

		if ( !idStr::Icmp( args.Argv(0), "map" ) ) {
			MoveToNewMap( args.Argv( 1 ), false );
		} else if ( !idStr::Icmp( args.Argv(0), "devmap" ) ) {
			MoveToNewMap( args.Argv( 1 ), true );
		} else if ( !idStr::Icmp( args.Argv(0), "died" ) ) {
			if ( !IsMultiplayer() ) {
				game->Shell_Show( true );
			}
		} else if ( !idStr::Icmp( args.Argv(0), "disconnect" ) ) {
			cmdSystem->BufferCommandText( CMD_EXEC_INSERT, "stoprecording ; disconnect" );
		} else if ( !idStr::Icmp( args.Argv(0), "endOfDemo" ) ) {
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, "endOfDemo" );
		}
	}
}

extern idCVar com_forceGenericSIMD;

#if defined( ID_3DS )
/*
================================================================================================

	New 3DS map tour: n3ds_mapTour [map ...]

	Loads the maps one after another (default: the biggest .resources archives), lets
	each run for a while, and logs one TOUR line per map: malloc peak and heap
	footprint, least free linear memory, static vertex/index cache use, load time.
	The summary at the end gives the worst map for each. Running out of memory ends
	the tour like any other error; the log shows where.

================================================================================================
*/
void Sys_N3DSMemSample( int & mallocUsedKB, int & mallocArenaKB, int & heapKB, int & linearFreeKB );
extern int n3dsMapLoads;
extern int n3dsLastLoadMsec;

idCVar n3ds_tourFrames( "n3ds_tourFrames", "600", CVAR_INTEGER, "3DS map tour: frames each map runs before the next one is loaded" );

enum { TOUR_IDLE, TOUR_NEXT, TOUR_LOADING, TOUR_RUNNING };

struct n3dsTourMap_t {
	idStr	name;
	bool	loaded;
	int		loadMsec;
	int		mallocPeakKB;
	int		arenaPeakKB;
	int		linearMinKB;
	int		staticVertexKB;
	int		staticIndexKB;
};

static idList< n3dsTourMap_t >	tourMaps;
static int						tourIndex = 0;
static int						tourState = TOUR_IDLE;
static int						tourFrames = 0;
static int						tourLoadsAtStart = 0;
static int						tourHeapKB = 0;

static const char * n3dsTourDefault[] = {
	// biggest maps/*.resources first (base game, Resurrection of Evil, The Lost Mission)
	"game/phobos3", "game/phobos1", "game/le_exis2", "game/phobos2", "game/erebus3", "game/erebus4",
	"game/le_enpro1", "game/deltax", "game/le_underground2", "game/mc_underground", "game/erebus5",
	"game/le_enpro2", "game/enpro", "game/comm1", "game/erebus1", "game/mars_city1", "game/erebus2",
	"game/le_exis1", "game/alphalabs2", "game/erebus6", "game/delta2b", "game/mars_city2", "game/alphalabs1",
};

/*
========================
N3DS_TourSample

Called at the load checkpoints and every frame while a tour runs.
========================
*/
void N3DS_TourSample() {
	if ( tourState != TOUR_LOADING && tourState != TOUR_RUNNING ) {
		return;
	}
	if ( tourIndex < 0 || tourIndex >= tourMaps.Num() ) {
		return;
	}
	n3dsTourMap_t & m = tourMaps[tourIndex];
	int used, arena, heap, linear;
	Sys_N3DSMemSample( used, arena, heap, linear );
	tourHeapKB = heap;
	m.mallocPeakKB = Max( m.mallocPeakKB, used );
	m.arenaPeakKB = Max( m.arenaPeakKB, arena );
	m.linearMinKB = ( m.linearMinKB < 0 ) ? linear : Min( m.linearMinKB, linear );
	m.staticVertexKB = Max( m.staticVertexKB, vertexCache.staticData.vertexMemUsed.GetValue() / 1024 );
	m.staticIndexKB = Max( m.staticIndexKB, vertexCache.staticData.indexMemUsed.GetValue() / 1024 );
}

static void N3DS_TourPrintMap( const n3dsTourMap_t & m ) {
	if ( !m.loaded ) {
		common->Printf( "TOUR %-22s did not load\n", m.name.c_str() );
		return;
	}
	common->Printf( "TOUR %-22s load %3d s | malloc peak %6d KB, heap footprint %6d / %d KB (%d KB left) | linear min free %5d KB | static vertex %5d / %d KB, index %5d / %d KB\n",
		m.name.c_str(), m.loadMsec / 1000, m.mallocPeakKB, m.arenaPeakKB, tourHeapKB, tourHeapKB - m.arenaPeakKB, m.linearMinKB,
		m.staticVertexKB, STATIC_VERTEX_MEMORY / 1024, m.staticIndexKB, STATIC_INDEX_MEMORY / 1024 );
}

static void N3DS_TourSummary() {
	common->Printf( "TOUR ---- summary (%d maps) ----\n", tourMaps.Num() );
	int worstArena = -1, worstLinear = -1, worstVertex = -1, worstIndex = -1;
	for ( int i = 0; i < tourMaps.Num(); i++ ) {
		const n3dsTourMap_t & m = tourMaps[i];
		N3DS_TourPrintMap( m );
		if ( !m.loaded ) {
			continue;
		}
		if ( worstArena < 0 || m.arenaPeakKB > tourMaps[worstArena].arenaPeakKB ) { worstArena = i; }
		if ( worstLinear < 0 || m.linearMinKB < tourMaps[worstLinear].linearMinKB ) { worstLinear = i; }
		if ( worstVertex < 0 || m.staticVertexKB > tourMaps[worstVertex].staticVertexKB ) { worstVertex = i; }
		if ( worstIndex < 0 || m.staticIndexKB > tourMaps[worstIndex].staticIndexKB ) { worstIndex = i; }
	}
	if ( worstArena >= 0 ) {
		common->Printf( "TOUR worst heap: %s (%d KB of %d), worst linear: %s (%d KB free), worst static vertex: %s (%d KB), worst static index: %s (%d KB)\n",
			tourMaps[worstArena].name.c_str(), tourMaps[worstArena].arenaPeakKB, tourHeapKB,
			tourMaps[worstLinear].name.c_str(), tourMaps[worstLinear].linearMinKB,
			tourMaps[worstVertex].name.c_str(), tourMaps[worstVertex].staticVertexKB,
			tourMaps[worstIndex].name.c_str(), tourMaps[worstIndex].staticIndexKB );
	}
}

CONSOLE_COMMAND_SHIP( n3ds_mapTour, "3DS: load the given maps (default: the heaviest ones) one after another and log their memory use", NULL ) {
	tourMaps.Clear();
	if ( args.Argc() > 1 ) {
		for ( int i = 1; i < args.Argc(); i++ ) {
			n3dsTourMap_t & m = tourMaps.Alloc();
			m.name = args.Argv( i );
		}
	} else {
		for ( int i = 0; i < (int)( sizeof( n3dsTourDefault ) / sizeof( n3dsTourDefault[0] ) ); i++ ) {
			n3dsTourMap_t & m = tourMaps.Alloc();
			m.name = n3dsTourDefault[i];
		}
	}
	for ( int i = 0; i < tourMaps.Num(); i++ ) {
		n3dsTourMap_t & m = tourMaps[i];
		m.loaded = false;
		m.loadMsec = m.mallocPeakKB = m.arenaPeakKB = m.staticVertexKB = m.staticIndexKB = 0;
		m.linearMinKB = -1;
	}
	tourIndex = 0;
	tourState = TOUR_NEXT;
	common->Printf( "TOUR: %d maps, %d frames each\n", tourMaps.Num(), n3ds_tourFrames.GetInteger() );
}

/*
========================
N3DS_TourFrame

Once per frame from idCommonLocal::Frame.
========================
*/
static void N3DS_TourFrame( bool mapSpawned ) {
	switch ( tourState ) {
		case TOUR_NEXT:
			if ( tourIndex >= tourMaps.Num() ) {
				N3DS_TourSummary();
				tourState = TOUR_IDLE;
				return;
			}
			common->Printf( "TOUR: loading %s (%d of %d)\n", tourMaps[tourIndex].name.c_str(), tourIndex + 1, tourMaps.Num() );
			tourLoadsAtStart = n3dsMapLoads;
			tourFrames = 0;
			tourState = TOUR_LOADING;
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "map %s\n", tourMaps[tourIndex].name.c_str() ) );
			return;
		case TOUR_LOADING:
			N3DS_TourSample();
			if ( n3dsMapLoads != tourLoadsAtStart && mapSpawned ) {
				tourMaps[tourIndex].loaded = true;
				tourMaps[tourIndex].loadMsec = n3dsLastLoadMsec;
				tourFrames = 0;
				tourState = TOUR_RUNNING;
			} else if ( ++tourFrames > 1800 ) {
				// the map never came up (wrong name, or an error dropped back to the menu)
				N3DS_TourPrintMap( tourMaps[tourIndex] );
				tourIndex++;
				tourState = TOUR_NEXT;
			}
			return;
		case TOUR_RUNNING:
			N3DS_TourSample();
			if ( ++tourFrames >= n3ds_tourFrames.GetInteger() ) {
				N3DS_TourPrintMap( tourMaps[tourIndex] );
				tourIndex++;
				tourState = TOUR_NEXT;
			}
			return;
		default:
			return;
	}
}
#endif

/*
=================
idCommonLocal::Frame
=================
*/
void idCommonLocal::Frame() {
	try {
		SCOPED_PROFILE_EVENT( "Common::Frame" );

		// This is the only place this is incremented
		idLib::frameNumber++;

		// allow changing SIMD usage on the fly
		if ( com_forceGenericSIMD.IsModified() ) {
			idSIMD::InitProcessor( "doom", com_forceGenericSIMD.GetBool() );
			com_forceGenericSIMD.ClearModified();
		}

		// Do the actual switch between Doom 3 and the classics here so
		// that things don't get confused in the middle of the frame.
		PerformGameSwitch();

		// pump all the events
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_OUTSIDE );
#endif
		N3DS_STAGE( N3DS_STAGE_MAIN, "events" );
		Sys_GenerateEvents();

		// write config file if anything changed
		WriteConfiguration(); 

		eventLoop->RunEventLoop();

		// Activate the shell if it's been requested
		if ( showShellRequested && game ) {
			game->Shell_Show( true );
			showShellRequested = false;
		}

		// if the console or another gui is down, we don't need to hold the mouse cursor
		bool chatting = false;
		if ( console->Active() || Dialog().IsDialogActive() || session->IsSystemUIShowing() || ( game && game->InhibitControls() && !IsPlayingDoomClassic() ) ) {
			Sys_GrabMouseCursor( false );
			usercmdGen->InhibitUsercmd( INHIBIT_SESSION, true );
			chatting = true;
		} else {
			Sys_GrabMouseCursor( true );
			usercmdGen->InhibitUsercmd( INHIBIT_SESSION, false );
		}

		const bool pauseGame = ( !mapSpawned || ( !IsMultiplayer() && ( Dialog().IsDialogPausing() || session->IsSystemUIShowing() || ( game && game->Shell_IsActive() ) ) ) ) && !IsPlayingDoomClassic();

		// save the screenshot and audio from the last draw if needed
		if ( aviCaptureMode ) {
			idStr name = va("demos/%s/%s_%05i.tga", aviDemoShortName.c_str(), aviDemoShortName.c_str(), aviDemoFrameCount++ );
			renderSystem->TakeScreenshot( com_aviDemoWidth.GetInteger(), com_aviDemoHeight.GetInteger(), name, com_aviDemoSamples.GetInteger(), NULL );

			// remove any printed lines at the top before taking the screenshot
			console->ClearNotifyLines();

			// this will call Draw, possibly multiple times if com_aviDemoSamples is > 1
			renderSystem->TakeScreenshot( com_aviDemoWidth.GetInteger(), com_aviDemoHeight.GetInteger(), name, com_aviDemoSamples.GetInteger(), NULL );
		}

		//--------------------------------------------
		// wait for the GPU to finish drawing
		//
		// It is imporant to minimize the time spent between this
		// section and the call to renderSystem->RenderCommandBuffers(),
		// because the GPU is completely idle.
		//--------------------------------------------
		// this should exit right after vsync, with the GPU idle and ready to draw
		// This may block if the GPU isn't finished renderng the previous frame.
		frameTiming.startSyncTime = Sys_Microseconds();
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_EVENTS );
#endif
		const emptyCommand_t * renderCommands = NULL;
		N3DS_STAGE( N3DS_STAGE_MAIN, "swap command buffers" );
		if ( com_smp.GetBool() ) {
			renderCommands = renderSystem->SwapCommandBuffers( &time_frontend, &time_backend, &time_shadows, &time_gpu );
		} else {
			// the GPU will stay idle through command generation for minimal
			// input latency
			renderSystem->SwapCommandBuffers_FinishRendering( &time_frontend, &time_backend, &time_shadows, &time_gpu );
		}
		frameTiming.finishSyncTime = Sys_Microseconds();
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_SWAP );
#endif

		//--------------------------------------------
		// Determine how many game tics we are going to run,
		// now that the previous frame is completely finished.
		//
		// It is important that any waiting on the GPU be done
		// before this, or there will be a bad stuttering when
		// dropping frames for performance management.
		//--------------------------------------------

		// input:
		// thisFrameTime
		// com_noSleep
		// com_engineHz
		// com_fixedTic
		// com_deltaTimeClamp
		// IsMultiplayer
		//
		// in/out state:
		// gameFrame
		// gameTimeResidual
		// lastFrameTime
		// syncNextFrame
		//
		// Output:
		// numGameFrames

		// How many game frames to run
		int numGameFrames = 0;

		for(;;) {
			const int thisFrameTime = Sys_Milliseconds();
			static int lastFrameTime = thisFrameTime;	// initialized only the first time
			const int deltaMilliseconds = thisFrameTime - lastFrameTime;
			lastFrameTime = thisFrameTime;

			// if there was a large gap in time since the last frame, or the frame
			// rate is very very low, limit the number of frames we will run
			const int clampedDeltaMilliseconds = Min( deltaMilliseconds, com_deltaTimeClamp.GetInteger() );

			gameTimeResidual += clampedDeltaMilliseconds * timescale.GetFloat();

			// don't run any frames when paused
			if ( pauseGame ) {
				gameFrame++;
				gameTimeResidual = 0;
				break;
			}

			// debug cvar to force multiple game tics
			if ( com_fixedTic.GetInteger() > 0 ) {
				numGameFrames = com_fixedTic.GetInteger();
				gameFrame += numGameFrames;
				gameTimeResidual = 0;
				break;
			}

			if ( syncNextGameFrame ) {
				// don't sleep at all
				syncNextGameFrame = false;
				gameFrame++;
				numGameFrames++;
				gameTimeResidual = 0;
				break;
			}

			for ( ;; ) {
				// How much time to wait before running the next frame,
				// based on com_engineHz
				const int frameDelay = FRAME_TO_MSEC( gameFrame + 1 ) - FRAME_TO_MSEC( gameFrame );
				if ( gameTimeResidual < frameDelay ) {
					break;
				}
				gameTimeResidual -= frameDelay;
				gameFrame++;
				numGameFrames++;
				// if there is enough residual left, we may run additional frames
			}

			if ( numGameFrames > 0 ) {
				// ready to actually run them
				break;
			}

			// if we are vsyncing, we always want to run at least one game
			// frame and never sleep, which might happen due to scheduling issues
			// if we were just looking at real time.
			if ( com_noSleep.GetBool() ) {
				numGameFrames = 1;
				gameFrame += numGameFrames;
				gameTimeResidual = 0;
				break;
			}

			// not enough time has passed to run a frame, as might happen if
			// we don't have vsync on, or the monitor is running at 120hz while
			// com_engineHz is 60, so sleep a bit and check again
			Sys_Sleep( 0 );
		}

		//--------------------------------------------
		// It would be better to push as much of this as possible
		// either before or after the renderSystem->SwapCommandBuffers(),
		// because the GPU is completely idle.
		//--------------------------------------------

		// Update session and syncronize to the new session state after sleeping
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_PACE );
#endif
		session->UpdateSignInManager();
		session->Pump();
		session->ProcessSnapAckQueue();

		if ( session->GetState() == idSession::LOADING ) {
			// If the session reports we should be loading a map, load it!
			ExecuteMapChange();
			mapSpawnData.savegameFile = NULL;
			mapSpawnData.persistentPlayerInfo.Clear();
			return;
		} else if ( session->GetState() != idSession::INGAME && mapSpawned ) {
			// If the game is running, but the session reports we are not in a game, disconnect
			// This happens when a server disconnects us or we sign out
			LeaveGame();
			return;
		}

		if ( mapSpawned && !pauseGame ) {
			if ( IsClient() ) {
				RunNetworkSnapshotFrame();
			}
		}

		ExecuteReliableMessages();

		// send frame and mouse events to active guis
		GuiFrameEvents();

		//--------------------------------------------
		// Prepare usercmds and kick off the game processing
		// in a background thread
		//--------------------------------------------

		// get the previous usercmd for bypassed head tracking transform
		const usercmd_t	previousCmd = usercmdGen->GetCurrentUsercmd();

		// build a new usercmd
		int deviceNum = session->GetSignInManager().GetMasterInputDevice();
#if defined( ID_3DS )
		{
			// the pad feeds the usercmd (C-Stick look, analog move) only through the master
			// input device; -1 leaves only its key events (bindings)
			static int lastDeviceNum = -2;
			if ( deviceNum != lastDeviceNum ) {
				Printf( "controls: master input device %d (was %d)\n", deviceNum, lastDeviceNum );
				lastDeviceNum = deviceNum;
			}
		}
#endif
		usercmdGen->BuildCurrentUsercmd( deviceNum );
		if ( deviceNum == -1 ) {
			for ( int i = 0; i < MAX_INPUT_DEVICES; i++ ) {
				Sys_PollJoystickInputEvents( i );
				Sys_EndJoystickInputEvents();
			}
		}
		if ( pauseGame ) {
			usercmdGen->Clear();
		}

		usercmd_t newCmd = usercmdGen->GetCurrentUsercmd();

		// Store server game time - don't let time go past last SS time in case we are extrapolating
		if ( IsClient() ) {
			newCmd.serverGameMilliseconds = std::min( Game()->GetServerGameTimeMs(), Game()->GetSSEndTime() );
		} else {
			newCmd.serverGameMilliseconds = Game()->GetServerGameTimeMs();
		}

		userCmdMgr.MakeReadPtrCurrentForPlayer( Game()->GetLocalClientNum() );

		// Stuff a copy of this userCmd for each game frame we are going to run.
		// Ideally, the usercmds would be built in another thread so you could
		// still get 60hz control accuracy when the game is running slower.
		for ( int i = 0 ; i < numGameFrames ; i++ ) {
			newCmd.clientGameMilliseconds = FRAME_TO_MSEC( gameFrame-numGameFrames+i+1 );
			userCmdMgr.PutUserCmdForPlayer( game->GetLocalClientNum(), newCmd );
		}

		// If we're in Doom or Doom 2, run tics and upload the new texture.
		if ( ( GetCurrentGame() == DOOM_CLASSIC || GetCurrentGame() == DOOM2_CLASSIC ) && !( Dialog().IsDialogPausing() || session->IsSystemUIShowing() ) ) {
			RunDoomClassicFrame();
		}
		
		// start the game / draw command generation thread going in the background
		N3DS_STAGE( N3DS_STAGE_MAIN, "run game and draw" );
		gameReturn_t ret = gameThread.RunGameAndDraw( numGameFrames, userCmdMgr, IsClient(), gameFrame - numGameFrames );

		if ( !com_smp.GetBool() ) {
			// in non-smp mode, run the commands we just generated, instead of
			// frame-delayed ones from a background thread
			renderCommands = renderSystem->SwapCommandBuffers_FinishCommandBuffers();
		}

		//----------------------------------------
		// Run the render back end, getting the GPU busy with new commands
		// ASAP to minimize the pipeline bubble.
		//----------------------------------------
		frameTiming.startRenderTime = Sys_Microseconds();
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_SESSION );
#endif
		N3DS_STAGE( N3DS_STAGE_MAIN, "render command buffers" );
		renderSystem->RenderCommandBuffers( renderCommands );
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_BACKEND );
#endif
		N3DS_STAGE( N3DS_STAGE_MAIN, "after render" );
		if ( com_sleepRender.GetInteger() > 0 ) {
			// debug tool to test frame adaption
			Sys_Sleep( com_sleepRender.GetInteger() );
		}
		frameTiming.finishRenderTime = Sys_Microseconds();

		// make sure the game / draw thread has completed
		// This may block if the game is taking longer than the render back end
#if defined( ID_3DS )
		const uint64 n3dsWaitGameStart = Sys_Microseconds();
		gameThread.WaitForThread();
		n3dsWaitGameUs = Sys_Microseconds() - n3dsWaitGameStart;
		N3DS_MARK( N3DS_MAIN_WAITGAME );
#else
		gameThread.WaitForThread();
#endif

		// Send local usermds to the server.
		// This happens after the game frame has run so that prediction data is up to date.
		SendUsercmds( Game()->GetLocalClientNum() );

		// Now that we have an updated game frame, we can send out new snapshots to our clients
		session->Pump(); // Pump to get updated usercmds to relay
		SendSnapshots();

		// Render the sound system using the latest commands from the game thread
		if ( pauseGame ) {
			soundWorld->Pause();
			soundSystem->SetPlayingSoundWorld( menuSoundWorld );
		} else {
			soundWorld->UnPause();
			soundSystem->SetPlayingSoundWorld( soundWorld );
		}
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_SEND );
#endif
		soundSystem->Render();
#if defined( ID_3DS )
		N3DS_MARK( N3DS_MAIN_SOUND );
#endif

		// process the game return for map changes, etc
		ProcessGameReturn( ret );

		idLobbyBase & lobby = session->GetActivePlatformLobbyBase();
		if ( lobby.HasActivePeers() ) {
			if ( net_drawDebugHud.GetInteger() == 1 ) {
				lobby.DrawDebugNetworkHUD();
			}
			if ( net_drawDebugHud.GetInteger() == 2 ) {
				lobby.DrawDebugNetworkHUD2();
			}
			lobby.DrawDebugNetworkHUD_ServerSnapshotMetrics( net_drawDebugHud.GetInteger() == 3 );
		}

#if defined( ID_3DS )
		if ( mapSpawned ) {
			Sys_N3DSFrameDone();
		}
		N3DS_TourFrame( mapSpawned );
		N3DS_MARK( N3DS_MAIN_REST );
		N3DS_SpikeFrame( mapSpawned );
		memset( n3dsMainUs, 0, sizeof( n3dsMainUs ) );
		n3dsMainMark = Sys_Microseconds();		// SPIKE lines written are not counted
		// New 3DS: one line every 2 seconds once a map is running
		{
			static int perfStart = 0;
			static int perfFrames = 0;
			static uint64 perfFrontendUs = 0;
			static uint64 perfBackendUs = 0;
			const int now = Sys_Milliseconds();
			if ( !mapSpawned ) {
				perfStart = now;
				perfFrames = 0;
				perfFrontendUs = perfBackendUs = 0;
				n3dsPerfTicks = 0;
				n3dsPerfGameUs = n3dsPerfGameMaxUs = n3dsPerfDrawUs = 0;
			} else {
				perfFrames++;
				perfFrontendUs += time_frontend;
				perfBackendUs += time_backend;
				const int elapsed = now - perfStart;
				if ( elapsed >= 2000 && perfFrames > 0 && !Sys_N3DSDebug() ) {
					// counted, not printed (n3ds_debug 0)
					perfStart = now;
					perfFrames = 0;
					perfFrontendUs = perfBackendUs = 0;
					n3dsPerfTicks = 0;
					n3dsPerfGameUs = n3dsPerfGameMaxUs = n3dsPerfDrawUs = 0;
					extern int n3dsRenderStats[8];
					memset( n3dsRenderStats, 0, sizeof( n3dsRenderStats ) );
				} else if ( elapsed >= 2000 && perfFrames > 0 ) {
					static bool threadsReported = false;
					if ( !threadsReported ) {
						// once: which core each engine thread got (all threads have started by now)
						extern void Sys_N3DSThreadReport();
						Sys_N3DSThreadReport();
						threadsReported = true;
					}
					const float ticks = (float)Max( n3dsPerfTicks, 1 );
					Printf( "PERF %.1fs: %d frames %.1f fps | %d ticks, game %.1f ms/tick, worst frame %.1f ms | draw %.1f frontend %.1f backend %.1f ms/frame\n",
						elapsed * 0.001f, perfFrames, perfFrames * 1000.0f / elapsed,
						n3dsPerfTicks, n3dsPerfGameUs * 0.001f / ticks, n3dsPerfGameMaxUs * 0.001f,
						n3dsPerfDrawUs * 0.001f / perfFrames, perfFrontendUs * 0.001f / perfFrames, perfBackendUs * 0.001f / perfFrames );
					extern int n3dsRenderStats[8];
					extern int n3dsFramePeakIndex, n3dsFramePeakVertex, n3dsCacheOverflows, n3dsFrameIndexBytes, n3dsFrameVertexBytes;
					const float rf = (float)Max( n3dsRenderStats[0], 1 );
					extern void R_FrameMemoryStats( int & highWater, int & size );
					int frameMemHigh, frameMemSize;
					R_FrameMemoryStats( frameMemHigh, frameMemSize );
					Printf( "PERF   scene/frame: ents %.0f lights %.0f guiSurfs %.0f draws %.0f tris %.0f newInteractions %.1f deformVerts %.0f | frame cache peak idx %d / %d KB, vtx %d / %d KB, overflows %d | frame memory peak %d / %d KB\n",
						n3dsRenderStats[1] / rf, n3dsRenderStats[2] / rf, n3dsRenderStats[3] / rf, n3dsRenderStats[4] / rf,
						n3dsRenderStats[5] / rf, n3dsRenderStats[6] / rf, n3dsRenderStats[7] / rf,
						n3dsFramePeakIndex / 1024, n3dsFrameIndexBytes / 1024,
						n3dsFramePeakVertex / 1024, n3dsFrameVertexBytes / 1024, n3dsCacheOverflows,
						frameMemHigh / 1024, frameMemSize / 1024 );
					memset( n3dsRenderStats, 0, sizeof( n3dsRenderStats ) );
					n3dsFramePeakIndex = n3dsFramePeakVertex = n3dsCacheOverflows = 0;
					perfStart = now;
					perfFrames = 0;
					perfFrontendUs = perfBackendUs = 0;
					n3dsPerfTicks = 0;
					n3dsPerfGameUs = n3dsPerfGameMaxUs = n3dsPerfDrawUs = 0;
				}
			}
		}
#endif

		// report timing information
		if ( com_speeds.GetBool() ) {
			static int lastTime = Sys_Milliseconds();
			int	nowTime = Sys_Milliseconds();
			int	com_frameMsec = nowTime - lastTime;
			lastTime = nowTime;
			Printf( "frame:%d all:%3d gfr:%3d rf:%3lld bk:%3lld\n", idLib::frameNumber, com_frameMsec, time_gameFrame, time_frontend / 1000, time_backend / 1000 );
			time_gameFrame = 0;
			time_gameDraw = 0;
		}

		// the FPU stack better be empty at this point or some bad code or compiler bug left values on the stack
		if ( !Sys_FPU_StackIsEmpty() ) {
			Printf( Sys_FPU_GetState() );
			FatalError( "idCommon::Frame: the FPU stack is not empty at the end of the frame\n" );
		}

		mainFrameTiming = frameTiming;

		session->GetSaveGameManager().Pump();
	} catch( idException & ) {
		return;			// an ERP_DROP was thrown
	}
}

/*
=================
idCommonLocal::RunDoomClassicFrame
=================
*/
void idCommonLocal::RunDoomClassicFrame() {
	static int doomTics = 0;

	if( DoomLib::expansionDirty ) {

		// re-Initialize the Doom Engine.
		DoomLib::Interface.Shutdown();
		DoomLib::Interface.Startup( 1, false );
		DoomLib::expansionDirty = false;
	}


	if ( DoomLib::Interface.Frame( doomTics, &userCmdMgr ) ) {
		Globals *data = (Globals*)DoomLib::GetGlobalData( 0 );

		idArray< unsigned int, 256 > palette;
		std::copy( data->XColorMap, data->XColorMap + palette.Num(), palette.Ptr() );

		// Do the palette lookup.
		for ( int row = 0; row < DOOMCLASSIC_RENDERHEIGHT; ++row ) {
			for ( int column = 0; column < DOOMCLASSIC_RENDERWIDTH; ++column ) {
				const int doomScreenPixelIndex = row * DOOMCLASSIC_RENDERWIDTH + column;
				const byte paletteIndex = data->screens[0][doomScreenPixelIndex];
				const unsigned int paletteColor = palette[paletteIndex];
				const byte red = (paletteColor & 0xFF000000) >> 24;
				const byte green = (paletteColor & 0x00FF0000) >> 16;
				const byte blue = (paletteColor & 0x0000FF00) >> 8;

				const int imageDataPixelIndex = row * DOOMCLASSIC_RENDERWIDTH * DOOMCLASSIC_BYTES_PER_PIXEL + column * DOOMCLASSIC_BYTES_PER_PIXEL;
				doomClassicImageData[imageDataPixelIndex]		= red;
				doomClassicImageData[imageDataPixelIndex + 1]	= green;
				doomClassicImageData[imageDataPixelIndex + 2]	= blue;
				doomClassicImageData[imageDataPixelIndex + 3]	= 255;
			}
		}
	}

	renderSystem->UploadImage( "_doomClassic", doomClassicImageData.Ptr(), DOOMCLASSIC_RENDERWIDTH, DOOMCLASSIC_RENDERHEIGHT );
	doomTics++;
}
