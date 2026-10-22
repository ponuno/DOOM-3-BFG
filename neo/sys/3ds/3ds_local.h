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

#ifndef __3DS_LOCAL_H__
#define __3DS_LOCAL_H__

/*
================================================================================================

	New Nintendo 3DS platform internals (the counterpart of win32/win_local.h).
	Engine-facing code only; libctru is included in the .cpp files.

================================================================================================
*/

// where the game data and saves live on the SD card
#define N3DS_BASE_PATH		"sdmc:/doom3"
#define N3DS_SAVE_PATH		"sdmc:/doom3/save"
#define N3DS_LOG_FILE		"sdmc:/doom3/log.txt"

struct n3dsVars_t {
	bool			activeApp;			// false while the HOME menu / sleep has us suspended
	bool			quitRequested;
	bool			isNew3DS;
	int				appMemoryAtStart;	// bytes free in the APPLICATION region at startup
	int				linearAtStart;		// bytes free in the linear heap at startup
};

extern n3dsVars_t	n3ds;

void	Sys_QueEvent( sysEventType_t type, int value, int value2, int ptrLength, void *ptr, int inputDeviceNum );

// 3ds_input.cpp
void	IN_Init();
void	IN_Shutdown();
void	IN_Frame();			// samples HID once per frame on the main thread

// 3ds_main.cpp
void	Sys_LogMemory( const char * when );	// prints heap / linear heap usage to the console and log

// 3ds_bottom.cpp: bottom screen = touch panel (gyro aim button) above the log
void	N3DS_BottomInit();
void	N3DS_BottomShowLog();			// before a fatal error is printed
void	N3DS_BottomFrame();				// main thread, once per frame after hidScanInput

#endif /* !__3DS_LOCAL_H__ */
