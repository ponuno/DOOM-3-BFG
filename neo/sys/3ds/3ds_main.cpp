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

	New Nintendo 3DS entry point and system services (counterpart of win_main.cpp +
	win_shared.cpp).

	Output goes to the bottom screen console and to sdmc:/doom3/log.txt.
	Extra command line arguments can be put in sdmc:/doom3/cmdline.txt.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../sys_local.h"
#include "3ds_local.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>

// only the parts of libctru we need: the full <3ds.h> clashes with engine names
// libctru headers are C; only <3ds.h> adds extern "C" itself
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/os.h>
#include <3ds/synchronization.h>
#include <3ds/allocator/linear.h>
#include <3ds/allocator/mappable.h>
#include <3ds/result.h>
#include <3ds/gfx.h>
#include <3ds/console.h>
#include <3ds/services/apt.h>
#include <3ds/services/hid.h>
#include <3ds/gpu/gx.h>
#include <3ds/thread.h>
}

n3dsVars_t	n3ds;

/*
	libctru startup parameters (weak symbols in libctru).

	__stacksize__: the main thread stack comes out of the heap; the 32 KB default is
	far too small for idTech code (16-32 KB string buffers on the stack).

	Heaps: libctru's default splits memory 50/50 between the application heap and the
	linear (GPU-visible) heap. Asking for a small linear heap instead makes libctru put
	everything else in the application heap, but that heap lives in a fixed 96 MB
	virtual window (OS_HEAP_AREA_BEGIN..END), so with ~112 MB available the allocation
	fails and libctru panics before main(). __system_allocateHeaps below gives the
	application heap as much as fits in that window and the rest to the linear heap,
	which the citro3d renderer will use for vertex buffers and textures.

	Extended memory (the CIA, SystemModeExt 178MB): the heap window stays 96 MB, so
	the heap takes all of it and everything else goes to the linear heap. The renderer
	sizes its static vertex/index caches for that (Sys_N3DSExtendedMemory), and once
	common->Init has allocated every long-lived GPU buffer, main() keeps
	N3DS_GPU_HEADROOM_MB of linear memory for citro3d's run-time needs and hands the rest
	to malloc as a second region (Mem_AddHeapRegion, idlib/Heap.cpp): linear memory is
	ordinary cached RAM, only its physical layout is special. In the 124 MB mode (.3dsx)
	nothing changes: 86 MB heap, the rest linear, no second heap.
*/
extern "C" {

u32 __stacksize__ = 2 * 1024 * 1024;

extern char *	fake_heap_start;
extern char *	fake_heap_end;
extern u32		__ctru_heap;
extern u32		__ctru_heap_size;
extern u32		__ctru_linear_heap;
extern u32		__ctru_linear_heap_size;

static const u32 N3DS_MIN_LINEAR_HEAP = 8 * 1024 * 1024;
#if !defined( N3DS_APP_HEAP_MB )
#define N3DS_APP_HEAP_MB 86
#endif
static const u32 N3DS_APP_HEAP_MAX = (u32)N3DS_APP_HEAP_MB * 1024 * 1024;
// extended memory mode: the linear heap must hold the GPU buffers (frame caches 7.5 MB,
// static caches 26 MB, citro3d ~1.5 MB) plus a second heap worth having
static const u32 N3DS_EXT_MIN_LINEAR = 48 * 1024 * 1024;
// linear memory left to citro3d after common->Init: every long-lived GPU buffer is
// allocated by then, and in game the 124 MB mode never took more than ~1.5 MB on top
#if !defined( N3DS_GPU_HEADROOM_MB )
#define N3DS_GPU_HEADROOM_MB 6
#endif
static const u32 N3DS_GPU_HEADROOM = (u32)N3DS_GPU_HEADROOM_MB * 1024 * 1024;
// less than this beyond the GPU share is not worth a second heap
static const u32 N3DS_MIN_SECOND_HEAP = 4 * 1024 * 1024;

void __system_allocateHeaps( void ) {
	Result rc;

	// Remaining commit for our process (the 3dsx is already loaded)
	Handle reslimit = 0;
	rc = svcGetResourceLimit( &reslimit, CUR_PROCESS_HANDLE );
	if ( R_FAILED( rc ) ) {
		svcBreak( USERBREAK_PANIC );
	}
	s64 maxCommit = 0, currentCommit = 0;
	ResourceLimitType reslimitType = RESLIMIT_COMMIT;
	svcGetResourceLimitLimitValues( &maxCommit, reslimit, &reslimitType, 1 );
	svcGetResourceLimitCurrentValues( &currentCommit, reslimit, &reslimitType, 1 );
	svcCloseHandle( reslimit );

	const u32 remaining = (u32)( maxCommit - currentCommit ) & ~0xFFF;
	const u32 heapWindow = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN;

	u32 heapSize = ( remaining > N3DS_MIN_LINEAR_HEAP ) ? remaining - N3DS_MIN_LINEAR_HEAP : remaining / 2;
	if ( heapSize > heapWindow ) {
		heapSize = heapWindow;
	}
	// build option DOOM3DS_APP_HEAP_MB: what is not given to the heap goes to the
	// linear heap (citro3d: vertex cache, command buffer, later textures)
	if ( heapSize > N3DS_APP_HEAP_MAX ) {
		heapSize = N3DS_APP_HEAP_MAX;
	}
	// extended memory: the whole heap window, the rest is linear (GPU + second heap)
	if ( remaining >= heapWindow + N3DS_EXT_MIN_LINEAR ) {
		heapSize = heapWindow;
	}
	heapSize &= ~0xFFF;
	__ctru_heap_size = heapSize;
	__ctru_linear_heap_size = ( remaining - heapSize ) & ~0xFFF;

	rc = svcControlMemory( &__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC, (MemPerm)( MEMPERM_READ | MEMPERM_WRITE ) );
	if ( R_FAILED( rc ) ) {
		svcBreak( USERBREAK_PANIC );
	}

	rc = svcControlMemory( &__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, (MemPerm)( MEMPERM_READ | MEMPERM_WRITE ) );
	if ( R_FAILED( rc ) ) {
		svcBreak( USERBREAK_PANIC );
	}

	mappableInit( OS_MAP_AREA_BEGIN, OS_MAP_AREA_END );

	// newlib's malloc lives in the application heap
	fake_heap_start = (char *)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}

}

/*
	New 3DS memory/CPU switches. All default to on; each can be turned off from
	sdmc:/doom3/cmdline.txt (e.g. "+set n3ds_animQuant 0") to isolate a problem.
*/
idCVar n3ds_animQuant( "n3ds_animQuant", "1", CVAR_SYSTEM | CVAR_BOOL | CVAR_INIT, "store MD5 anim frames as 16-bit (halves anim memory)" );
idCVar n3ds_noShadows( "n3ds_noShadows", "1", CVAR_SYSTEM | CVAR_BOOL | CVAR_INIT, "drop stencil shadow data at load and disable shadow casting" );
idCVar n3ds_mapEntitiesOnly( "n3ds_mapEntitiesOnly", "1", CVAR_SYSTEM | CVAR_BOOL | CVAR_INIT, "discard .map brushes/patches while parsing (collision comes from .bcmodel)" );
idCVar n3ds_lazySaveBuffer( "n3ds_lazySaveBuffer", "1", CVAR_SYSTEM | CVAR_BOOL | CVAR_INIT, "do not preallocate the 4 MB savegame buffer at startup" );
idCVar n3ds_skipImageData( "n3ds_skipImageData", "1", CVAR_SYSTEM | CVAR_BOOL | CVAR_INIT, "read only image headers, not pixels (the null GL drops them anyway)" );
idCVar n3ds_lazyInteractions( "n3ds_lazyInteractions", "0", CVAR_SYSTEM | CVAR_BOOL | CVAR_INIT, "do not pre-generate every light/entity interaction at map start; create them when first seen" );
idCVar n3ds_discardStaticGeo( "n3ds_discardStaticGeo", "1", CVAR_SYSTEM | CVAR_BOOL | CVAR_INIT, "null GL: drop static vertex/index cache uploads instead of keeping an 11 MB copy nobody draws" );
idCVar n3ds_renderer( "n3ds_renderer", "1", CVAR_SYSTEM | CVAR_INTEGER | CVAR_INIT, "0 = null GL (nothing drawn), 1 = citro3d" );
idCVar n3ds_staticVertexKB( "n3ds_staticVertexKB", "0", CVAR_SYSTEM | CVAR_INTEGER | CVAR_INIT, "static vertex cache size, 0 = default for the renderer" );
idCVar n3ds_debugPhysics( "n3ds_debugPhysics", "0", CVAR_SYSTEM | CVAR_BOOL, "heavy physics diagnostics: NaN checks after every AF step, LCP dumps, collision trace counters, SLOW AF lines" );
idCVar n3ds_staticIndexKB( "n3ds_staticIndexKB", "0", CVAR_SYSTEM | CVAR_INTEGER | CVAR_INIT, "static index cache size, 0 = default for the renderer" );
idCVar n3ds_core1Percent( "n3ds_core1Percent", "30", CVAR_SYSTEM | CVAR_INTEGER | CVAR_INIT, "share of the system core (core 1) given to the game's threads, 0-80" );

idCVar sys_logFlush( "sys_logFlush", "1", CVAR_SYSTEM | CVAR_BOOL, "flush sdmc:/doom3/log.txt after every line (slower, but survives crashes)" );

static FILE *		logFile = NULL;
static LightLock	logLock;
static bool			logLockInit = false;
static u64			baseTick = 0;

static char			sys_cmdline[MAX_STRING_CHARS];
static char			sys_exePath[MAX_OSPATH] = "sdmc:/3ds/doom3ds.3dsx";

/*
================================================================================================

	Output

================================================================================================
*/

/*
==============
Sys_LogWrite
==============
*/
static bool screenLog = false;	// true once a fatal error is being shown

static void Sys_LogWrite( const char * text ) {
	if ( logLockInit ) {
		LightLock_Lock( &logLock );
	}

	// bottom screen console (strip the engine's ^color codes)
	char clean[1024];
	int j = 0;
	for ( int i = 0; text[i] != '\0' && j < (int)sizeof( clean ) - 1; i++ ) {
		if ( text[i] == '^' && text[i + 1] != '\0' && text[i + 1] != '^' ) {
			i++;
			continue;
		}
		clean[j++] = text[i];
	}
	clean[j] = '\0';
	// the bottom screen shows the log only for a fatal error (the full log is in the file)
	if ( screenLog ) {
		fputs( clean, stdout );
	}

	if ( logFile != NULL ) {
		fputs( clean, logFile );
		if ( sys_logFlush.GetBool() ) {
			fflush( logFile );
		}
	}

	if ( logLockInit ) {
		LightLock_Unlock( &logLock );
	}
}

void Sys_N3DSConsoleLock() {
	if ( logLockInit ) {
		LightLock_Lock( &logLock );
	}
}

void Sys_N3DSConsoleUnlock() {
	if ( logLockInit ) {
		LightLock_Unlock( &logLock );
	}
}

/*
==============
Sys_Printf
==============
*/
void Sys_Printf( const char *fmt, ... ) {
	char msg[MAX_PRINT_MSG];
	va_list argptr;
	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
	Sys_LogWrite( msg );
}

/*
==============
Sys_DebugPrintf
==============
*/
void Sys_DebugPrintf( const char *fmt, ... ) {
	char msg[MAX_PRINT_MSG];
	va_list argptr;
	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
	Sys_LogWrite( msg );
}

/*
==============
Sys_DebugVPrintf
==============
*/
void Sys_DebugVPrintf( const char *fmt, va_list arg ) {
	char msg[MAX_PRINT_MSG];
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, arg );
	Sys_LogWrite( msg );
}

/*
==============
Sys_LogMemory
==============
*/
void N3DS_TexLogMemory();								// renderer/3DS/n3ds_textures.cpp
void R_FrameMemoryStats( int & highWater, int & size );	// renderer/tr_frontend_main.cpp
void N3DS_SoundLogMemory();								// sound/3DS/3DS_SoundSample.cpp
void N3DS_LogDeclMemory();								// framework/DeclManager.cpp
void Mem_SlabStats( int & chunkBytes, int & usedBytes, int & emptyBytes );	// idlib/Heap.cpp
void Mem_AddHeapRegion( void * mem, int bytes );						// idlib/Heap.cpp
void Mem_HeapRegionStats( int & size, int & taken );					// idlib/Heap.cpp
struct mallinfo Mem_MallInfo();											// idlib/Heap.cpp

bool Sys_N3DSExtendedMemory() {
	return __ctru_linear_heap_size >= N3DS_EXT_MIN_LINEAR;
}

/*
==============
Sys_N3DSInitSecondHeap

Extended memory mode: linear memory beyond what citro3d needs becomes a second region
of malloc itself (Mem_AddHeapRegion), so everything that allocates - the engine, libctru
(thread stacks), newlib (FILE buffers) - can use it. Runs after common->Init, when the
long-lived GPU buffers (frame and static vertex caches, citro3d) are allocated; until
then the heap window alone has plenty.
==============
*/
static void Sys_N3DSInitSecondHeap() {
	const u32 linearFree = linearSpaceFree();
	if ( !Sys_N3DSExtendedMemory() || linearFree < N3DS_GPU_HEADROOM + N3DS_MIN_SECOND_HEAP ) {
		return;
	}
	const u32 bytes = ( linearFree - N3DS_GPU_HEADROOM ) & ~0xFFF;
	void * mem = linearAlloc( bytes );
	if ( mem == NULL ) {
		Sys_Printf( "second heap: linearAlloc( %d KB ) failed\n", (int)( bytes / 1024 ) );
		return;
	}
	Mem_AddHeapRegion( mem, (int)bytes );
	Sys_Printf( "second heap: %d KB of linear memory added to malloc, %d KB left for the GPU\n", (int)( bytes / 1024 ), (int)( linearSpaceFree() / 1024 ) );
}

/*
==============
Sys_N3DSMemSample

Cheap numbers for the map tour (n3ds_mapTour): KB in malloc blocks, KB malloc has
taken from the heap (its high-water footprint), the heap size, free linear memory.
==============
*/
/*
==============
Sys_N3DSMallocAvailableKB

What malloc can still hand out: free memory inside its arena plus the heap it has not
taken yet (the heap window and the second region). Fragmentation not counted.
==============
*/
int Sys_N3DSMallocAvailableKB() {
	struct mallinfo mi = Mem_MallInfo();
	int h2Size, h2Taken;
	Mem_HeapRegionStats( h2Size, h2Taken );
	const int64 total = (int64)__ctru_heap_size + h2Size;
	const int64 avail = total - (int64)mi.arena + (int64)mi.fordblks;
	return (int)( Max( avail, (int64)0 ) / 1024 );
}

void Sys_N3DSMemSample( int & mallocUsedKB, int & mallocArenaKB, int & heapKB, int & linearFreeKB ) {
	struct mallinfo mi = Mem_MallInfo();
	int h2Size, h2Taken;
	Mem_HeapRegionStats( h2Size, h2Taken );
	// malloc spans the heap window and the second region
	mallocUsedKB = (int)( mi.uordblks / 1024 );
	mallocArenaKB = (int)( mi.arena / 1024 );
	heapKB = (int)( ( __ctru_heap_size + h2Size ) / 1024 );
	linearFreeKB = (int)( linearSpaceFree() / 1024 );
}

void Sys_LogMemory( const char * when ) {
	const int appFree = (int)osGetMemRegionFree( MEMREGION_APPLICATION );
	const int appSize = (int)osGetMemRegionSize( MEMREGION_APPLICATION );
	const int linFree = (int)linearSpaceFree();
	struct mallinfo mi = Mem_MallInfo();
	int arenaUsed, arenaPeak, arenaSize;
	Mem_ArenaStats( arenaUsed, arenaPeak, arenaSize );
	Sys_Printf( "MEM [%s]: malloc used %d KB, free in malloc %d KB, largest free %d KB, region free %d / %d KB, linear free %d KB, temp arena %d (peak %d) / %d KB\n",
		when, (int)( mi.uordblks / 1024 ), (int)( mi.fordblks / 1024 ), Mem_LargestFreeBlock() / 1024, appFree / 1024, appSize / 1024, linFree / 1024,
		arenaUsed / 1024, arenaPeak / 1024, arenaSize / 1024 );
	Mem_LogTags( 16 );
	int slabChunk, slabUsed, slabEmpty;
	Mem_SlabStats( slabChunk, slabUsed, slabEmpty );
	Sys_Printf( "  small blocks: %d KB in pages, %d KB in use, %d KB in empty pages\n", slabChunk / 1024, slabUsed / 1024, slabEmpty / 1024 );
	int h2Size, h2Taken;
	Mem_HeapRegionStats( h2Size, h2Taken );
	if ( h2Size > 0 ) {
		Sys_Printf( "  second heap: malloc has taken %d KB of %d KB (window %d KB)\n", h2Taken / 1024, h2Size / 1024, (int)( __ctru_heap_size / 1024 ) );
	}

	// what is inside the big tags (no allocations: safe on the out-of-memory path)
	N3DS_TexLogMemory();
	int frameHigh, frameSize;
	R_FrameMemoryStats( frameHigh, frameSize );
	if ( frameHigh > 0 ) {
		Sys_Printf( "  frame memory: peak %d KB of %d KB (x2)\n", frameHigh / 1024, frameSize / 1024 );
	}
	N3DS_SoundLogMemory();
	N3DS_LogDeclMemory();
}

/*
================================================================================================

	Fatal errors and shutdown

================================================================================================
*/

/*
==============
Sys_WaitForStart

Keeps the message on screen until the user presses START.
==============
*/
static void Sys_WaitForStart() {
	Sys_LogWrite( "\nPress START to exit.\n" );
	while ( aptMainLoop() ) {
		hidScanInput();
		if ( hidKeysDown() & KEY_START ) {
			break;
		}
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
}

/*
================================================================================================

	One fatal error at a time

	The thread that reaches Sys_Error first owns the screens until the user presses START.
	Out of memory, other threads usually fail right after it; they went through
	idCommonLocal::FatalError's recursive branch to Sys_Quit, whose gfxExit unmapped GSP's
	shared memory under the thread showing the error (a data abort at 0x10002280 in
	gspPresentBuffer instead of the message). Any other thread that gets to Sys_Error or
	Sys_Quit now, and the main thread before it touches the GPU again
	(Sys_N3DSFatalCheck), stops where it is.

================================================================================================
*/
static volatile u32		fatalThreadId = 0;		// 0: no fatal error is being shown

static u32 Sys_N3DSThreadId() {
	u32 id = 0;
	svcGetThreadId( &id, CUR_THREAD_HANDLE );
	return id;
}

// another thread shows a fatal error: never come back
static void Sys_N3DSStopForFatal() {
	for ( ;; ) {
		svcSleepThread( 1000LL * 1000 * 1000 );
	}
}

// true if this thread is now the one showing the fatal error (or already was)
static bool Sys_N3DSClaimFatal() {
	const u32 me = Sys_N3DSThreadId();
	if ( __sync_bool_compare_and_swap( &fatalThreadId, 0u, me ) ) {
		return true;
	}
	return fatalThreadId == me;
}

void Sys_N3DSFatalCheck() {
	const u32 owner = fatalThreadId;
	if ( owner != 0 && owner != Sys_N3DSThreadId() ) {
		Sys_N3DSStopForFatal();
	}
}

/*
==============
Sys_Error

Show the early console as an error dialog
==============
*/
void Sys_Error( const char *error, ... ) {
	if ( !Sys_N3DSClaimFatal() ) {
		Sys_N3DSStopForFatal();
	}
	char text[4096];
	va_list argptr;
	va_start( argptr, error );
	idStr::vsnPrintf( text, sizeof( text ), error, argptr );
	va_end( argptr );

	screenLog = true;
	N3DS_BottomShowLog();
	Sys_LogWrite( "\n********************\nERROR: " );
	Sys_LogWrite( text );
	Sys_LogWrite( "\n********************\n" );
	Sys_LogMemory( "error" );

	// close the log now: the user may power off instead of pressing START
	if ( logFile != NULL ) {
		fclose( logFile );
		logFile = NULL;
	}

	Sys_WaitForStart();
	N3DS_BottomShutdown();
	gfxExit();
	exit( 1 );
}

/*
==============
Sys_SetFatalError
==============
*/
void Sys_SetFatalError( const char *error ) {
}

/*
==============
Sys_Quit
==============
*/
void Sys_Quit() {
	// a fatal error shown by another thread keeps the screens until START
	Sys_N3DSFatalCheck();
	Sys_LogWrite( "Sys_Quit\n" );
	if ( logFile != NULL ) {
		fclose( logFile );
		logFile = NULL;
	}
	N3DS_BottomShutdown();
	gfxExit();
	exit( 0 );
}

/*
==============
Sys_AlreadyRunning
==============
*/
bool Sys_AlreadyRunning() {
	return false;
}

/*
==============
Sys_GetCmdLine
==============
*/
const char * Sys_GetCmdLine() {
	return sys_cmdline;
}

/*
==============
Sys_ReLaunch / Sys_Launch
==============
*/
void Sys_ReLaunch( void * data, const unsigned int dataSize ) {
	idLib::Warning( "Sys_ReLaunch is not supported on 3DS" );
}

void Sys_Launch( const char * path, idCmdArgs & args, void * data, unsigned int dataSize ) {
	idLib::Warning( "Sys_Launch is not supported on 3DS" );
}

/*
==============
idSysLocal::OpenURL / StartProcess
==============
*/
void idSysLocal::OpenURL( const char *url, bool doexit ) {
	idLib::Printf( "OpenURL( %s ): not supported on 3DS\n", url );
}

void idSysLocal::StartProcess( const char *exePath, bool doexit ) {
	idLib::Printf( "StartProcess( %s ): not supported on 3DS\n", exePath );
}

/*
================================================================================================

	Time

================================================================================================
*/

/*
==============
Sys_Milliseconds
==============
*/
int Sys_Milliseconds() {
	return (int)( ( svcGetSystemTick() - baseTick ) / ( SYSCLOCK_ARM11 / 1000 ) );
}

/*
==============
Sys_Microseconds
==============
*/
uint64 Sys_Microseconds() {
	const u64 ticks = svcGetSystemTick() - baseTick;
	const u64 hz = (u64)SYSCLOCK_ARM11;
	return ( ticks / hz ) * 1000000ULL + ( ( ticks % hz ) * 1000000ULL ) / hz;
}

/*
==============
Sys_GetClockTicks / Sys_ClockTicksPerSecond
==============
*/
double Sys_GetClockTicks() {
	return (double)svcGetSystemTick();
}

double Sys_ClockTicksPerSecond() {
	return (double)SYSCLOCK_ARM11;
}

/*
==============
Sys_Sleep
==============
*/
void Sys_Sleep( int msec ) {
	svcSleepThread( (s64)msec * 1000000LL );
}

/*
================================================================================================

	CPU / FPU / memory

================================================================================================
*/

cpuid_t Sys_GetProcessorId() {
	return CPUID_GENERIC;
}

const char * Sys_GetProcessorString() {
	return n3ds.isNew3DS ? "ARM11 MPCore 804MHz (New 3DS)" : "ARM11 MPCore 268MHz";
}

/*
==============
Sys_CPUCount

The job system sizes its worker pool from this. On New 3DS the application
owns core 0 and core 2 (core 1 is time-sliced with the system).
==============
*/
void Sys_CPUCount( int & numLogicalCPUCores, int & numPhysicalCPUCores, int & numCPUPackages ) {
	numPhysicalCPUCores = n3ds.isNew3DS ? 2 : 1;
	numLogicalCPUCores = numPhysicalCPUCores;
	numCPUPackages = 1;
}

bool Sys_FPU_StackIsEmpty() { return true; }
const char * Sys_FPU_GetState() { return ""; }
void Sys_FPU_EnableExceptions( int exceptions ) {}
void Sys_FPU_SetFTZ( bool enable ) {}
void Sys_FPU_SetDAZ( bool enable ) {}

bool Sys_LockMemory( void *ptr, int bytes ) { return true; }
bool Sys_UnlockMemory( void *ptr, int bytes ) { return true; }
void Sys_SetPhysicalWorkMemory( int minBytes, int maxBytes ) {}

void Sys_GetCallStack( address_t *callStack, const int callStackSize ) {
	for ( int i = 0; i < callStackSize; i++ ) {
		callStack[i] = 0;
	}
}
const char * Sys_GetCallStackStr( const address_t *callStack, const int callStackSize ) { return ""; }
const char * Sys_GetCallStackCurStr( int depth ) { return ""; }
void Sys_ShutdownSymbols() {}

/*
================================================================================================

	DLLs (none: the game is linked statically) and clipboard

================================================================================================
*/

int Sys_DLL_Load( const char *dllName ) { return 0; }
void * Sys_DLL_GetProcAddress( int dllHandle, const char *procName ) { return NULL; }
void Sys_DLL_Unload( int dllHandle ) {}

char * Sys_GetClipboardData() { return NULL; }
void Sys_SetClipboardData( const char *string ) {}

/*
================================================================================================

	Files and paths

================================================================================================
*/

const char * Sys_DefaultBasePath() {
	return N3DS_BASE_PATH;
}

const char * Sys_DefaultSavePath() {
	return N3DS_SAVE_PATH;
}

const char * Sys_EXEPath() {
	return sys_exePath;
}

void Sys_Mkdir( const char *path ) {
	mkdir( path, 0777 );
}

bool Sys_Rmdir( const char *path ) {
	return rmdir( path ) == 0;
}

bool Sys_IsFileWritable( const char *path ) {
	struct stat st;
	if ( stat( path, &st ) == -1 ) {
		return true;
	}
	return ( st.st_mode & S_IWUSR ) != 0;
}

sysFolder_t Sys_IsFolder( const char *path ) {
	struct stat st;
	if ( stat( path, &st ) < 0 ) {
		return FOLDER_ERROR;
	}
	return S_ISDIR( st.st_mode ) ? FOLDER_YES : FOLDER_NO;
}

ID_TIME_T Sys_FileTimeStamp( idFileHandle fp ) {
	struct stat st;
	if ( fp == NULL || fstat( fileno( fp ), &st ) != 0 ) {
		return 0;
	}
	return (ID_TIME_T)st.st_mtime;
}

int64 Sys_GetDriveFreeSpaceInBytes( const char * path ) {
	struct statvfs st;
	if ( statvfs( "sdmc:/", &st ) != 0 ) {
		return 0;
	}
	return (int64)st.f_bavail * (int64)st.f_bsize;
}

/*
==============
Sys_ListFiles

Passing "/" as extension lists directories, anything else lists files ending in it.
==============
*/
int Sys_ListFiles( const char *directory, const char *extension, idStrList &list ) {
	if ( !extension ) {
		extension = "";
	}

	bool wantDirs = false;
	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		wantDirs = true;
	}
	const int extLen = idStr::Length( extension );

	list.Clear();

	DIR * dir = opendir( directory );
	if ( dir == NULL ) {
		return -1;
	}

	struct dirent * d;
	while ( ( d = readdir( dir ) ) != NULL ) {
		if ( !idStr::Cmp( d->d_name, "." ) || !idStr::Cmp( d->d_name, ".." ) ) {
			continue;
		}

		bool isDir;
#ifdef DT_DIR
		if ( d->d_type != DT_UNKNOWN ) {
			isDir = ( d->d_type == DT_DIR );
		} else
#endif
		{
			idStr full = directory;
			full.AppendPath( d->d_name );
			isDir = ( Sys_IsFolder( full.c_str() ) == FOLDER_YES );
		}

		if ( isDir != wantDirs ) {
			continue;
		}
		if ( extLen > 0 ) {
			const int nameLen = idStr::Length( d->d_name );
			if ( nameLen < extLen || idStr::Icmp( d->d_name + nameLen - extLen, extension ) != 0 ) {
				continue;
			}
		}
		list.Append( d->d_name );
	}
	closedir( dir );

	return list.Num();
}

/*
================================================================================================

	Events

================================================================================================
*/

#define	MAX_QUED_EVENTS		256
#define	MASK_QUED_EVENTS	( MAX_QUED_EVENTS - 1 )

static sysEvent_t	eventQue[MAX_QUED_EVENTS];
static int			eventHead = 0;
static int			eventTail = 0;

/*
================
Sys_QueEvent

Ptr should either be null, or point to a block of data that can
be freed by the game later.
================
*/
void Sys_QueEvent( sysEventType_t type, int value, int value2, int ptrLength, void *ptr, int inputDeviceNum ) {
	sysEvent_t * ev = &eventQue[ eventHead & MASK_QUED_EVENTS ];

	if ( eventHead - eventTail >= MAX_QUED_EVENTS ) {
		common->Printf( "Sys_QueEvent: overflow\n" );
		// we are discarding an event, but don't leak memory
		if ( ev->evPtr ) {
			Mem_Free( ev->evPtr );
		}
		eventTail++;
	}

	eventHead++;

	ev->evType = type;
	ev->evValue = value;
	ev->evValue2 = value2;
	ev->evPtrLength = ptrLength;
	ev->evPtr = ptr;
	ev->inputDevice = inputDeviceNum;
}

/*
================
Sys_GenerateEvents
================
*/
void Sys_GenerateEvents() {
	static bool entered = false;
	if ( entered ) {
		return;
	}
	entered = true;

	IN_Frame();

	entered = false;
}

/*
================
Sys_ClearEvents
================
*/
void Sys_ClearEvents() {
	eventHead = eventTail = 0;
}

/*
================
Sys_GetEvent
================
*/
sysEvent_t Sys_GetEvent() {
	sysEvent_t	ev;

	// return if we have data
	if ( eventHead > eventTail ) {
		eventTail++;
		return eventQue[ ( eventTail - 1 ) & MASK_QUED_EVENTS ];
	}

	// return the empty event
	memset( &ev, 0, sizeof( ev ) );

	return ev;
}

/*
================================================================================================

	Init / shutdown

================================================================================================
*/

extern idCVar sys_lang;

/*
================
Sys_SetLanguageFromSystem
================
*/
void Sys_SetLanguageFromSystem() {
	sys_lang.SetString( Sys_DefaultLanguage() );
}

/*
================
Sys_Init
================
*/
/*
================================================================================================

	Hang watchdog

================================================================================================
*/
static const char * volatile	n3dsStages[N3DS_STAGE_COUNT] = { "start", "-", "-" };
static volatile uint32			n3dsFramesDone = 0;
static Thread					n3dsWatchdogThread = NULL;

void Sys_N3DSStage( int slot, const char * what ) {
	if ( slot >= 0 && slot < N3DS_STAGE_COUNT ) {
		n3dsStages[slot] = what;
	}
}

void Sys_N3DSFrameDone() {
	n3dsFramesDone++;
}

idN3DSSlowScope::idN3DSSlowScope( const char * label_, int limitMs ) :
	label( label_ ), limitUs( limitMs * 1000 ), start( Sys_Microseconds() ) {
}

idN3DSSlowScope::~idN3DSSlowScope() {
	const uint64 us = Sys_Microseconds() - start;
	if ( us > (uint64)limitUs ) {
		Sys_Printf( "SLOW %s: %.1f ms\n", label, us * 0.001f );
	}
}

static void N3DS_WatchdogMain( void * ) {
	uint32 lastFrames = n3dsFramesDone;
	int stalledMs = 0;
	bool reported = false;
	for ( ;; ) {
		svcSleepThread( 500LL * 1000 * 1000 );
		const uint32 frames = n3dsFramesDone;
		if ( frames != lastFrames ) {
			if ( reported ) {
				Sys_Printf( "WATCHDOG: frames resumed after %.1f s\n", stalledMs * 0.001f );
			}
			lastFrames = frames;
			stalledMs = 0;
			reported = false;
			continue;
		}
		stalledMs += 500;
		// frames only count once a map runs; loading has its own long pauses
		if ( frames > 0 && !reported && stalledMs >= 5000 ) {
			reported = true;
			Sys_Printf( "WATCHDOG: no frame for %.1f s (frame %u): main '%s', game thread '%s', render '%s'\n",
				stalledMs * 0.001f, frames, n3dsStages[N3DS_STAGE_MAIN], n3dsStages[N3DS_STAGE_GAME], n3dsStages[N3DS_STAGE_RENDER] );
		} else if ( reported && ( stalledMs % 10000 ) == 0 ) {
			Sys_Printf( "WATCHDOG: still stuck %.0f s: main '%s', game thread '%s', render '%s'\n",
				stalledMs * 0.001f, n3dsStages[N3DS_STAGE_MAIN], n3dsStages[N3DS_STAGE_GAME], n3dsStages[N3DS_STAGE_RENDER] );
		}
	}
}

void Sys_Init() {
	Sys_Printf( "%s\n", Sys_GetProcessorString() );

	// higher priority than the engine threads so it runs even if one of them spins
	// 64 KB stack: Sys_Printf alone puts a MAX_PRINT_MSG (16 KB) buffer on it
	n3dsWatchdogThread = threadCreate( N3DS_WatchdogMain, NULL, 64 * 1024, 0x19, -2, true );

	// let threads placed on core 1 actually run (0 = none of its time is ours)
	const int core1 = idMath::ClampInt( 0, 80, n3ds_core1Percent.GetInteger() );
	if ( core1 > 0 ) {
		Result rc = APT_SetAppCpuTimeLimit( core1 );
		Sys_Printf( "core 1 time limit %d%%: %s\n", core1, R_SUCCEEDED( rc ) ? "ok" : "failed" );
	}
	Sys_Printf( "switches: animQuant %d noShadows %d mapEntitiesOnly %d lazySaveBuffer %d skipImageData %d lazyInteractions %d discardStaticGeo %d\n",
		n3ds_animQuant.GetBool(), n3ds_noShadows.GetBool(), n3ds_mapEntitiesOnly.GetBool(), n3ds_lazySaveBuffer.GetBool(), n3ds_skipImageData.GetBool(), n3ds_lazyInteractions.GetBool(), n3ds_discardStaticGeo.GetBool() );
	Sys_LogMemory( "Sys_Init" );
}

/*
================
Sys_Shutdown
================
*/
void Sys_Shutdown() {
	IN_Shutdown();
}

/*
================
Sys_BuildCommandLine

Built-in defaults for the bring-up builds, followed by the contents of
sdmc:/doom3/cmdline.txt (if present), so tests can be changed without rebuilding.
================
*/
static void Sys_BuildCommandLine() {
	idStr cmd =
		"+set com_skipIntroVideos 1 "
		"+set com_disableAutoSaves 1 "
		"+set r_cullDynamicLightTriangles 0 ";	// per-frame light-tri culling of moving models: CPU + frame index cache	// map-start autosave needs ~2 MB of buffers; no room yet

	FILE * f = fopen( N3DS_BASE_PATH "/cmdline.txt", "rb" );
	if ( f != NULL ) {
		char buf[MAX_STRING_CHARS];
		int n = (int)fread( buf, 1, sizeof( buf ) - 1, f );
		fclose( f );
		if ( n > 0 ) {
			buf[n] = '\0';
			for ( int i = 0; i < n; i++ ) {
				if ( buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\t' ) {
					buf[i] = ' ';
				}
			}
			cmd += buf;
		}
	}
	idStr::Copynz( sys_cmdline, cmd.c_str(), sizeof( sys_cmdline ) );
}

/*
==================
N3DS_Main

Called from main() in 3ds_exe.cpp (the executable's own translation unit).
==================
*/
int N3DS_Main( int argc, char ** argv ) {
	// New 3DS: 804 MHz + L2 cache
	osSetSpeedupEnable( true );

	bool isNew = false;
	APT_CheckNew3DS( &isNew );
	n3ds.isNew3DS = isNew;
	n3ds.activeApp = true;
	n3ds.quitRequested = false;

	gfxInitDefault();
	N3DS_BottomInit();		// touch panel above the log on the bottom screen

	LightLock_Init( &logLock );
	logLockInit = true;
	baseTick = svcGetSystemTick();

	mkdir( N3DS_BASE_PATH, 0777 );
	mkdir( N3DS_SAVE_PATH, 0777 );
	logFile = fopen( N3DS_LOG_FILE, "w" );

	if ( argc > 0 && argv != NULL && argv[0] != NULL ) {
		idStr::Copynz( sys_exePath, argv[0], sizeof( sys_exePath ) );
	}

	n3ds.appMemoryAtStart = (int)osGetMemRegionFree( MEMREGION_APPLICATION );
	n3ds.linearAtStart = (int)linearSpaceFree();

	Sys_Printf( "DOOM 3 BFG for New 3DS (%s %s)\n", __DATE__, __TIME__ );
	Sys_Printf( "%s, application region %d KB\n", n3ds.isNew3DS ? "New 3DS" : "Old 3DS",
		(int)( osGetMemRegionSize( MEMREGION_APPLICATION ) / 1024 ) );
	Sys_Printf( "heap %d KB, linear heap %d KB%s\n", (int)( __ctru_heap_size / 1024 ), (int)( __ctru_linear_heap_size / 1024 ),
		Sys_N3DSExtendedMemory() ? " (extended memory mode)" : "" );
	Sys_LogMemory( "start" );

	Sys_BuildCommandLine();
	Sys_Printf( "cmdline: %s\n", sys_cmdline );

	common->Init( 0, NULL, sys_cmdline );
	Sys_N3DSInitSecondHeap();
	Sys_LogMemory( "common->Init done" );

	while ( aptMainLoop() && !n3ds.quitRequested ) {
		common->Frame();
	}

	common->Quit();
	return 0;
}
