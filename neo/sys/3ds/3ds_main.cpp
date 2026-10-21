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
	fputs( clean, stdout );

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
void Sys_LogMemory( const char * when ) {
	const int appFree = (int)osGetMemRegionFree( MEMREGION_APPLICATION );
	const int appSize = (int)osGetMemRegionSize( MEMREGION_APPLICATION );
	const int linFree = (int)linearSpaceFree();
	struct mallinfo mi = mallinfo();
	Sys_Printf( "MEM [%s]: malloc used %d KB, region free %d / %d KB, linear free %d KB\n",
		when, (int)( mi.uordblks / 1024 ), appFree / 1024, appSize / 1024, linFree / 1024 );
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
==============
Sys_Error

Show the early console as an error dialog
==============
*/
void Sys_Error( const char *error, ... ) {
	char text[4096];
	va_list argptr;
	va_start( argptr, error );
	idStr::vsnPrintf( text, sizeof( text ), error, argptr );
	va_end( argptr );

	Sys_LogWrite( "\n********************\nERROR: " );
	Sys_LogWrite( text );
	Sys_LogWrite( "\n********************\n" );
	Sys_LogMemory( "error" );

	Sys_WaitForStart();

	if ( logFile != NULL ) {
		fclose( logFile );
		logFile = NULL;
	}
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
	Sys_LogWrite( "Sys_Quit\n" );
	if ( logFile != NULL ) {
		fclose( logFile );
		logFile = NULL;
	}
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
void Sys_Init() {
	Sys_Printf( "%s\n", Sys_GetProcessorString() );
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
		"+set s_noSound 1 ";

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
	consoleInit( GFX_BOTTOM, NULL );

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
	Sys_Printf( "heap %d KB, linear heap %d KB\n", (int)( __ctru_heap_size / 1024 ), (int)( __ctru_linear_heap_size / 1024 ) );
	Sys_LogMemory( "start" );

	Sys_BuildCommandLine();
	Sys_Printf( "cmdline: %s\n", sys_cmdline );

	common->Init( 0, NULL, sys_cmdline );

	while ( aptMainLoop() && !n3ds.quitRequested ) {
		common->Frame();
	}

	common->Quit();
	return 0;
}
