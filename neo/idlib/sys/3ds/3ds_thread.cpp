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

// New Nintendo 3DS threading backend on top of libctru.

#include "../../precompiled.h"

// only the parts of libctru we need: the full <3ds.h> clashes with engine names
// libctru headers are C; only <3ds.h> adds extern "C" itself
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
#include <3ds/thread.h>
}

static_assert( sizeof( mutexHandle_t ) == sizeof( RecursiveLock ), "mutexHandle_t must mirror RecursiveLock" );
static_assert( sizeof( signalHandle_t ) == sizeof( LightEvent ), "signalHandle_t must mirror LightEvent" );
static_assert( offsetof( signalHandle_t, lock ) == offsetof( LightEvent, lock ), "signalHandle_t layout" );

/*
================================================================================================

	Thread priorities and cores

	libctru: lower number = higher priority. The main thread starts at 0x30 and is
	lowered to N3DS_PRIORITY_MAIN (see MapPriority).
	Valid application range is 0x18..0x3F.

	Cores on New 3DS (application view):
		0  - main application core
		1  - system core, only usable after APT_SetAppCpuTimeLimit(), heavily time-sliced
		2  - New 3DS extra core, fully available once osSetSpeedupEnable(true) is called
		3  - New 3DS, reserved by the system
		-2 - "default" = the core of the creating process (core 0)

	BFG's job system asks for CORE_0x/1x/2x (360 hardware threads). Workers
	are steered to core 2 first, then core 1.

================================================================================================
*/

#define N3DS_PRIORITY_MAIN		0x32
#define N3DS_YIELD_SITES		5

/*
	libctru's APT event thread (HOME button, sleep, power) runs on core 0 at 0x31.
	svcSleepThread( 0 ) - what Sys_Yield does - only yields to threads of equal or
	higher priority, so a core-0 thread at 0x30 that spins on Sys_Yield (a job
	thread stalled on a job list the suspended main thread will never finish, the
	main thread waiting for shadow volumes) starves the APT thread forever and the
	game never comes back from the HOME menu or sleep mode.

	So every thread that can spin runs below the APT thread: the main thread is
	lowered to N3DS_PRIORITY_MAIN at startup (Sys_N3DSSetMainPriority) and
	THREAD_NORMAL maps to the same value, keeping the main thread and the job
	threads equal so their Sys_Yield still hands the core to each other.
	THREAD_ABOVE_NORMAL (the texture / sound loaders: they block on I/O and never
	spin) stays above the main thread.
*/
static int MapPriority( xthreadPriority priority ) {
	switch ( priority ) {
		case THREAD_LOWEST:			return 0x3F;
		case THREAD_BELOW_NORMAL:	return N3DS_PRIORITY_MAIN + 1;
		case THREAD_NORMAL:			return N3DS_PRIORITY_MAIN;
		case THREAD_ABOVE_NORMAL:	return 0x30;
		case THREAD_HIGHEST:		return 0x2C;
	}
	return N3DS_PRIORITY_MAIN;
}

/*
==================
Sys_N3DSSetMainPriority

Lowers the calling (main) thread below libctru's APT thread, see MapPriority.
==================
*/
void Sys_N3DSSetMainPriority() {
	svcSetThreadPriority( CUR_THREAD_HANDLE, N3DS_PRIORITY_MAIN );
}

static int MapCore( core_t core ) {
	switch ( core ) {
		case CORE_0A:
		case CORE_0B:	return 0;
		case CORE_1A:
		case CORE_1B:	return 2;
		case CORE_2A:
		case CORE_2B:	return 1;
		default:		return -2;
	}
}

struct threadStart_t {
	xthread_t	function;
	void *		parms;
	char		name[32];
	bool		suspended;
	LightEvent	resume;
};

struct threadRecord_t {
	Thread			thread;
	threadStart_t *	start;
};

/*
	Which core each engine thread asked for and which it runs on: a thread whose core could
	not be had falls back to core 0, where it competes with the main thread. Read by
	Sys_N3DSThreadReport once the game runs.
*/
struct threadCoreRecord_t {
	char	name[32];
	int		asked;		// core asked for (libctru numbering), -2 = default
	int		fallback;	// threadCreate on that core failed
	int		ran;		// core the thread found itself on, -1 until it started
	volatile uint32	yields;	// Sys_Yield calls (Sys_N3DSYieldReport)
};
static const int			MAX_THREAD_RECORDS = 16;
static threadCoreRecord_t	threadRecords[MAX_THREAD_RECORDS];
static int					numThreadRecords = 0;
static __thread threadCoreRecord_t * myThreadRecord;	// NULL: main thread or not ours
static volatile uint32		mainYields;

static void ThreadEntry( void * arg ) {
	threadStart_t * start = (threadStart_t *)arg;
	if ( start->suspended ) {
		LightEvent_Wait( &start->resume );
	}
	for ( int i = 0; i < numThreadRecords; i++ ) {
		if ( threadRecords[i].ran < 0 && idStr::Cmp( threadRecords[i].name, start->name ) == 0 ) {
			threadRecords[i].ran = svcGetProcessorID();
			myThreadRecord = &threadRecords[i];
			break;
		}
	}
	start->function( start->parms );
}

/*
========================
Sys_N3DSThreadReport
========================
*/
void Sys_N3DSThreadReport() {
	s32 mainCore = svcGetProcessorID();
	idLib::Printf( "threads: main thread on core %d\n", (int)mainCore );
	for ( int i = 0; i < numThreadRecords; i++ ) {
		const threadCoreRecord_t & r = threadRecords[i];
		idLib::Printf( "threads: %-22s asked core %2d, runs on core %2d%s\n", r.name, r.asked, r.ran,
			r.fallback != 0 ? " (creating it there failed, fell back to the default core)" : "" );
	}
}

/*
========================
Sys_GetCurrentThreadID
========================
*/
uintptr_t Sys_GetCurrentThreadID() {
	u32 id = 0;
	svcGetThreadId( &id, CUR_THREAD_HANDLE );
	return id;
}

/*
========================
Sys_CreateThread
========================
*/
uintptr_t Sys_CreateThread( xthread_t function, void *parms, xthreadPriority priority, const char *name, core_t core, int stackSize, bool suspended ) {
	threadStart_t * start = new threadStart_t;
	start->function = function;
	start->parms = parms;
	start->suspended = suspended;
	idStr::Copynz( start->name, name ? name : "", sizeof( start->name ) );
	LightEvent_Init( &start->resume, RESET_ONESHOT );

	int coreId = MapCore( core );
	threadCoreRecord_t * record = NULL;
	if ( numThreadRecords < MAX_THREAD_RECORDS ) {
		record = &threadRecords[numThreadRecords++];
		idStr::Copynz( record->name, start->name, sizeof( record->name ) );
		record->asked = coreId;
		record->fallback = 0;
		record->ran = -1;
	}
	Thread t = threadCreate( ThreadEntry, start, stackSize, MapPriority( priority ), coreId, false );
	if ( t == NULL && coreId != -2 ) {
		if ( record != NULL ) {
			record->fallback = 1;
		}
		// requested core not available (e.g. old 3DS or speedup not enabled): fall back to core 0
		t = threadCreate( ThreadEntry, start, stackSize, MapPriority( priority ), -2, false );
	}
	if ( t == NULL ) {
		idLib::common->FatalError( "Sys_CreateThread: failed to create thread '%s'", start->name );
		delete start;
		return 0;
	}

	threadRecord_t * rec = new threadRecord_t;
	rec->thread = t;
	rec->start = start;
	return (uintptr_t)rec;
}

/*
========================
Sys_ResumeThread (used for threads created suspended)
========================
*/
void Sys_ResumeThread( uintptr_t threadHandle ) {
	threadRecord_t * rec = (threadRecord_t *)threadHandle;
	if ( rec != NULL ) {
		LightEvent_Signal( &rec->start->resume );
	}
}

/*
========================
Sys_WaitForThread
========================
*/
void Sys_WaitForThread( uintptr_t threadHandle ) {
	threadRecord_t * rec = (threadRecord_t *)threadHandle;
	if ( rec != NULL ) {
		threadJoin( rec->thread, U64_MAX );
	}
}

/*
========================
Sys_DestroyThread
========================
*/
void Sys_DestroyThread( uintptr_t threadHandle ) {
	threadRecord_t * rec = (threadRecord_t *)threadHandle;
	if ( rec == NULL ) {
		return;
	}
	threadJoin( rec->thread, U64_MAX );
	threadFree( rec->thread );
	delete rec->start;
	delete rec;
}

/*
========================
Sys_SetCurrentThreadName
========================
*/
void Sys_SetCurrentThreadName( const char * name ) {
	// Horizon has no thread names; nothing to do.
}

/*
========================
Sys_Yield
========================
*/
// nanoseconds Sys_Yield sleeps (n3ds_yieldNs, set by the main loop). 0: svcSleepThread( 0 ),
// which only yields to threads of the same or a higher priority. 0140 made it 1 us so that
// libctru's APT event thread got the core (the game hung after sleep / HOME), but that froze
// the whole console on a shot on le_hell. The APT thread is now kept running by the thread
// priorities instead (see MapPriority); this stays a test switch.
int n3dsYieldNs = 0;

// spin sites calling Sys_Yield (ParallelJobList.cpp, tr_backend_draw.cpp), see
// Sys_N3DSYieldReport
volatile uint32 n3dsYieldSite[N3DS_YIELD_SITES];

void Sys_Yield() {
	threadCoreRecord_t * r = myThreadRecord;
	if ( r != NULL ) {
		r->yields++;
	} else {
		mainYields++;
	}
	svcSleepThread( n3dsYieldNs );
}

/*
========================
Sys_N3DSYieldReport

Who spun on Sys_Yield since the last call - for the watchdog while no frames come (the
game suspended in the HOME menu: a thread spinning then starves the system).
========================
*/
void Sys_N3DSYieldReport( char * out, int size ) {
	static uint32 lastMain, lastThread[MAX_THREAD_RECORDS], lastSite[N3DS_YIELD_SITES];
	static const char * siteNames[N3DS_YIELD_SITES] = { "Wait jobs", "Wait threads", "AddJobList", "job thread stalled", "backend shadows" };
	int len = idStr::snPrintf( out, size, "main %u", mainYields - lastMain );
	lastMain = mainYields;
	for ( int i = 0; i < numThreadRecords && len < size; i++ ) {
		const uint32 y = threadRecords[i].yields;
		if ( y != lastThread[i] ) {
			len += idStr::snPrintf( out + len, size - len, ", %s %u", threadRecords[i].name, y - lastThread[i] );
		}
		lastThread[i] = y;
	}
	for ( int i = 0; i < N3DS_YIELD_SITES && len < size; i++ ) {
		const uint32 y = n3dsYieldSite[i];
		if ( y != lastSite[i] ) {
			len += idStr::snPrintf( out + len, size - len, " | %s %u", siteNames[i], y - lastSite[i] );
		}
		lastSite[i] = y;
	}
}

/*
================================================================================================

	Signal

================================================================================================
*/

void Sys_SignalCreate( signalHandle_t & handle, bool manualReset ) {
	LightEvent_Init( (LightEvent *)&handle, manualReset ? RESET_STICKY : RESET_ONESHOT );
}

void Sys_SignalDestroy( signalHandle_t & handle ) {
	// LightEvent owns no kernel objects
}

void Sys_SignalRaise( signalHandle_t & handle ) {
	LightEvent_Signal( (LightEvent *)&handle );
}

void Sys_SignalClear( signalHandle_t & handle ) {
	LightEvent_Clear( (LightEvent *)&handle );
}

bool Sys_SignalWait( signalHandle_t & handle, int timeout ) {
	LightEvent * ev = (LightEvent *)&handle;
	if ( timeout == idSysSignal::WAIT_INFINITE ) {
		LightEvent_Wait( ev );
		return true;
	}
	if ( timeout == 0 ) {
		return LightEvent_TryWait( ev ) != 0;
	}
	return LightEvent_WaitTimeout( ev, (s64)timeout * 1000000LL ) == 0;
}

/*
================================================================================================

	Mutex (recursive, like a Win32 CRITICAL_SECTION)

================================================================================================
*/

void Sys_MutexCreate( mutexHandle_t & handle ) {
	RecursiveLock_Init( (RecursiveLock *)&handle );
}

void Sys_MutexDestroy( mutexHandle_t & handle ) {
}

bool Sys_MutexLock( mutexHandle_t & handle, bool blocking ) {
	if ( blocking ) {
		RecursiveLock_Lock( (RecursiveLock *)&handle );
		return true;
	}
	return RecursiveLock_TryLock( (RecursiveLock *)&handle ) == 0;
}

void Sys_MutexUnlock( mutexHandle_t & handle ) {
	RecursiveLock_Unlock( (RecursiveLock *)&handle );
}

/*
================================================================================================

	Interlocked integer / pointer (ARMv6K ldrex/strex via GCC builtins)

	Win32 semantics: Increment/Decrement/Add/Sub return the NEW value,
	Exchange/CompareExchange return the OLD value.

================================================================================================
*/

interlockedInt_t Sys_InterlockedIncrement( interlockedInt_t & value ) {
	return __atomic_add_fetch( &value, 1, __ATOMIC_SEQ_CST );
}

interlockedInt_t Sys_InterlockedDecrement( interlockedInt_t & value ) {
	return __atomic_sub_fetch( &value, 1, __ATOMIC_SEQ_CST );
}

interlockedInt_t Sys_InterlockedAdd( interlockedInt_t & value, interlockedInt_t i ) {
	return __atomic_add_fetch( &value, i, __ATOMIC_SEQ_CST );
}

interlockedInt_t Sys_InterlockedSub( interlockedInt_t & value, interlockedInt_t i ) {
	return __atomic_sub_fetch( &value, i, __ATOMIC_SEQ_CST );
}

interlockedInt_t Sys_InterlockedExchange( interlockedInt_t & value, interlockedInt_t exchange ) {
	return __atomic_exchange_n( &value, exchange, __ATOMIC_SEQ_CST );
}

interlockedInt_t Sys_InterlockedCompareExchange( interlockedInt_t & value, interlockedInt_t comparand, interlockedInt_t exchange ) {
	__atomic_compare_exchange_n( &value, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
	return comparand;	// holds the old value whether or not the swap happened
}

void * Sys_InterlockedExchangePointer( void *& ptr, void * exchange ) {
	return __atomic_exchange_n( &ptr, exchange, __ATOMIC_SEQ_CST );
}

void * Sys_InterlockedCompareExchangePointer( void * & ptr, void * comparand, void * exchange ) {
	__atomic_compare_exchange_n( &ptr, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
	return comparand;
}

/*
================================================================================================

	Thread local storage slots

================================================================================================
*/

static const int MAX_TLS_SLOTS = 32;
static __thread ptrdiff_t	tlsSlots[MAX_TLS_SLOTS];
static uint32				tlsUsedMask = 0;

int Sys_TLSAllocSlot() {
	for ( ;; ) {
		uint32 used = __atomic_load_n( &tlsUsedMask, __ATOMIC_SEQ_CST );
		if ( used == 0xFFFFFFFFu ) {
			return -1;
		}
		int slot = __builtin_ctz( ~used );
		if ( __atomic_compare_exchange_n( &tlsUsedMask, &used, used | ( 1u << slot ), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ) ) {
			return slot;
		}
	}
}

void Sys_TLSFreeSlot( int slot ) {
	if ( slot >= 0 && slot < MAX_TLS_SLOTS ) {
		__atomic_and_fetch( &tlsUsedMask, ~( 1u << slot ), __ATOMIC_SEQ_CST );
	}
}

ptrdiff_t & Sys_TLSSlot( int slot ) {
	static ptrdiff_t dummy;
	if ( slot < 0 || slot >= MAX_TLS_SLOTS ) {
		dummy = 0;
		return dummy;
	}
	return tlsSlots[slot];
}
