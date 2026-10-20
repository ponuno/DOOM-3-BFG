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

	New 3DS input: the console's buttons are presented to the engine as one Xbox 360
	style pad (device 0), so the stock joy_360 bindings and the gamepad menus work.

		A / B / X / Y		-> A / B / X / Y			(J_ACTION1..4)
		L / R				-> left / right shoulder	(J_ACTION5..6)
		ZL / ZR				-> left / right trigger		(axes)
		START / SELECT		-> Start / Back				(J_ACTION9..10)
		D-pad				-> D-pad
		Circle Pad			-> left stick
		C-Stick (New 3DS)	-> right stick

	START + SELECT together quits.

	HID is sampled once per frame on the main thread (IN_Frame); the usercmd
	code consumes the cached state through Sys_PollJoystickInputEvents.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "3ds_local.h"

// libctru headers are C; only <3ds.h> adds extern "C" itself
extern "C" {
#include <3ds/types.h>
#include <3ds/synchronization.h>
#include <3ds/services/hid.h>
#include <3ds/services/irrst.h>	// C-Stick (merged into hid on New 3DS)
}

static const int CIRCLE_PAD_RANGE	= 156;	// approximate max deflection of the Circle Pad / C-Stick
static const int AXIS_RANGE			= 32767;
static const int STICK_BUTTON_RANGE	= AXIS_RANGE / 2;	// threshold for stick-as-button GUI events
static const int TRIGGER_BUTTON_RANGE = 128;

struct padState_t {
	u32		buttons;
	int		lx, ly;		// -32767..32767, y positive = down (Xbox convention after the inversion below)
	int		rx, ry;
	int		lt, rt;		// 0..32767
};

static LightLock	padLock;
static padState_t	padCurrent;
static padState_t	padPrevious;

static const int	MAX_JOY_EVENTS = 64;
struct joyEvent_t {
	int		event;
	int		value;
};
static joyEvent_t	joyEvents[MAX_JOY_EVENTS];
static int			numJoyEvents = 0;

static bool			buttonStates[K_LAST_KEY];
static int			joyAxis[MAX_JOYSTICK_AXIS];

/*
========================
ScaleStick
========================
*/
static int ScaleStick( int v ) {
	int s = v * AXIS_RANGE / CIRCLE_PAD_RANGE;
	if ( s > AXIS_RANGE ) {
		s = AXIS_RANGE;
	}
	if ( s < -AXIS_RANGE ) {
		s = -AXIS_RANGE;
	}
	return s;
}

/*
========================
IN_Init
========================
*/
void IN_Init() {
	LightLock_Init( &padLock );
	memset( &padCurrent, 0, sizeof( padCurrent ) );
	memset( &padPrevious, 0, sizeof( padPrevious ) );
	memset( buttonStates, 0, sizeof( buttonStates ) );
	memset( joyAxis, 0, sizeof( joyAxis ) );
}

/*
========================
IN_Shutdown
========================
*/
void IN_Shutdown() {
}

/*
========================
IN_Frame
========================
*/
void IN_Frame() {
	hidScanInput();

	const u32 held = hidKeysHeld();
	circlePosition cp, cs;
	hidCircleRead( &cp );
	hidCstickRead( &cs );

	if ( ( held & ( KEY_START | KEY_SELECT ) ) == ( KEY_START | KEY_SELECT ) ) {
		if ( !n3ds.quitRequested ) {
			n3ds.quitRequested = true;
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
		}
	}

	padState_t s;
	s.buttons = held;
	s.lx = ScaleStick( cp.dx );
	s.ly = -ScaleStick( cp.dy );	// 3DS: up is positive; engine: up is negative
	s.rx = ScaleStick( cs.dx );
	s.ry = -ScaleStick( cs.dy );
	s.lt = ( held & KEY_ZL ) ? AXIS_RANGE : 0;
	s.rt = ( held & KEY_ZR ) ? AXIS_RANGE : 0;

	LightLock_Lock( &padLock );
	padCurrent = s;
	LightLock_Unlock( &padLock );
}

/*
========================
PushButton

GUI events: only send an SE_KEY when the state changes
========================
*/
static void PushButton( int key, bool value ) {
	if ( buttonStates[key] != value ) {
		buttonStates[key] = value;
		Sys_QueEvent( SE_KEY, key, value, 0, NULL, 0 );
	}
}

/*
========================
PostInputEvent
========================
*/
static void PostInputEvent( int event, int value, int range ) {
	// These events are used for GUI button presses
	if ( ( event >= J_ACTION1 ) && ( event <= J_ACTION_MAX ) ) {
		PushButton( K_JOY1 + ( event - J_ACTION1 ), value != 0 );
	} else if ( event == J_AXIS_LEFT_X ) {
		PushButton( K_JOY_STICK1_LEFT, ( value < -range ) );
		PushButton( K_JOY_STICK1_RIGHT, ( value > range ) );
	} else if ( event == J_AXIS_LEFT_Y ) {
		PushButton( K_JOY_STICK1_UP, ( value < -range ) );
		PushButton( K_JOY_STICK1_DOWN, ( value > range ) );
	} else if ( event == J_AXIS_RIGHT_X ) {
		PushButton( K_JOY_STICK2_LEFT, ( value < -range ) );
		PushButton( K_JOY_STICK2_RIGHT, ( value > range ) );
	} else if ( event == J_AXIS_RIGHT_Y ) {
		PushButton( K_JOY_STICK2_UP, ( value < -range ) );
		PushButton( K_JOY_STICK2_DOWN, ( value > range ) );
	} else if ( ( event >= J_DPAD_UP ) && ( event <= J_DPAD_RIGHT ) ) {
		PushButton( K_JOY_DPAD_UP + ( event - J_DPAD_UP ), value != 0 );
	} else if ( event == J_AXIS_LEFT_TRIG ) {
		PushButton( K_JOY_TRIGGER1, ( value > range ) );
	} else if ( event == J_AXIS_RIGHT_TRIG ) {
		PushButton( K_JOY_TRIGGER2, ( value > range ) );
	}
	if ( event >= J_AXIS_MIN && event <= J_AXIS_MAX ) {
		int axis = event - J_AXIS_MIN;
		int percent = ( value * 16 ) / AXIS_RANGE;
		if ( joyAxis[axis] != percent ) {
			joyAxis[axis] = percent;
			Sys_QueEvent( SE_JOYSTICK, axis, percent, 0, NULL, 0 );
		}
	}

	// These events are used for actual game input
	if ( numJoyEvents < MAX_JOY_EVENTS ) {
		joyEvents[numJoyEvents].event = event;
		joyEvents[numJoyEvents].value = value;
		numJoyEvents++;
	}
}

/*
================================================================================================

	Sys_ joystick interface

================================================================================================
*/

void Sys_InitInput() {
	IN_Init();
}

int Sys_PollJoystickInputEvents( int deviceNum ) {
	numJoyEvents = 0;
	if ( deviceNum != 0 ) {
		return 0;
	}

	padState_t cur, old;
	LightLock_Lock( &padLock );
	cur = padCurrent;
	old = padPrevious;
	padPrevious = padCurrent;
	LightLock_Unlock( &padLock );

	static const struct { u32 key; int event; } buttonMap[] = {
		{ KEY_A,		J_ACTION1 },
		{ KEY_B,		J_ACTION2 },
		{ KEY_X,		J_ACTION3 },
		{ KEY_Y,		J_ACTION4 },
		{ KEY_L,		J_ACTION5 },
		{ KEY_R,		J_ACTION6 },
		{ KEY_START,	J_ACTION9 },
		{ KEY_SELECT,	J_ACTION10 },
		{ KEY_DUP,		J_DPAD_UP },
		{ KEY_DDOWN,	J_DPAD_DOWN },
		{ KEY_DLEFT,	J_DPAD_LEFT },
		{ KEY_DRIGHT,	J_DPAD_RIGHT },
	};
	for ( int i = 0; i < (int)( sizeof( buttonMap ) / sizeof( buttonMap[0] ) ); i++ ) {
		const u32 mask = buttonMap[i].key;
		if ( ( cur.buttons & mask ) != ( old.buttons & mask ) ) {
			PostInputEvent( buttonMap[i].event, ( cur.buttons & mask ) != 0, 0 );
		}
	}

	if ( cur.lt != old.lt ) {
		PostInputEvent( J_AXIS_LEFT_TRIG, cur.lt, TRIGGER_BUTTON_RANGE );
	}
	if ( cur.rt != old.rt ) {
		PostInputEvent( J_AXIS_RIGHT_TRIG, cur.rt, TRIGGER_BUTTON_RANGE );
	}
	if ( cur.lx != old.lx ) {
		PostInputEvent( J_AXIS_LEFT_X, cur.lx, STICK_BUTTON_RANGE );
	}
	if ( cur.ly != old.ly ) {
		PostInputEvent( J_AXIS_LEFT_Y, cur.ly, STICK_BUTTON_RANGE );
	}
	if ( cur.rx != old.rx ) {
		PostInputEvent( J_AXIS_RIGHT_X, cur.rx, STICK_BUTTON_RANGE );
	}
	if ( cur.ry != old.ry ) {
		PostInputEvent( J_AXIS_RIGHT_Y, cur.ry, STICK_BUTTON_RANGE );
	}

	return numJoyEvents;
}

int Sys_ReturnJoystickInputEvent( const int n, int &action, int &value ) {
	if ( n < 0 || n >= numJoyEvents ) {
		return 0;
	}
	action = joyEvents[n].event;
	value = joyEvents[n].value;
	return 1;
}

void Sys_EndJoystickInputEvents() {
}

void Sys_SetRumble( int device, int low, int hi ) {
}

/*
================================================================================================

	Keyboard / mouse: none on 3DS (the touch screen may become the mouse later)

================================================================================================
*/

int Sys_PollKeyboardInputEvents() {
	return 0;
}

int Sys_ReturnKeyboardInputEvent( const int n, int &ch, bool &state ) {
	return 0;
}

void Sys_EndKeyboardInputEvents() {
}

int Sys_PollMouseInputEvents( int mouseEvents[MAX_MOUSE_EVENTS][2] ) {
	return 0;
}

void Sys_GrabMouseCursor( bool grabIt ) {
}
