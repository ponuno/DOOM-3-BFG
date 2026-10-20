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

	New 3DS networking: not supported (single player only). The session code
	still creates a local "lobby" for single player, which only needs loopback
	addresses, so idUDP is a closed socket that drops everything.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"

/*
========================
Sys_InitNetworking
========================
*/
void Sys_InitNetworking() {
	idLib::Printf( "Networking: disabled on 3DS\n" );
}

/*
========================
Sys_StringToNetAdr
========================
*/
bool Sys_StringToNetAdr( const char *s, netadr_t *a, bool doDNSResolve ) {
	memset( a, 0, sizeof( *a ) );

	idStr host = s;
	int port = 0;
	const int colon = host.Find( ':' );
	if ( colon >= 0 ) {
		port = atoi( host.c_str() + colon + 1 );
		host.CapLength( colon );
	}

	if ( host.Icmp( "localhost" ) == 0 ) {
		a->type = NA_LOOPBACK;
		a->port = (unsigned short)port;
		return true;
	}

	int b[4];
	if ( sscanf( host.c_str(), "%d.%d.%d.%d", &b[0], &b[1], &b[2], &b[3] ) == 4 ) {
		a->type = NA_IP;
		for ( int i = 0; i < 4; i++ ) {
			a->ip[i] = (unsigned char)b[i];
		}
		a->port = (unsigned short)port;
		return true;
	}

	a->type = NA_BAD;
	return false;
}

/*
========================
Sys_NetAdrToString
========================
*/
const char * Sys_NetAdrToString( const netadr_t a ) {
	static int index = 0;
	static char buf[ 4 ][ 64 ];	// flip/flop
	char *s;

	s = buf[index];
	index = ( index + 1 ) & 3;

	s[0] = '\0';
	if ( a.type == NA_LOOPBACK ) {
		if ( a.port ) {
			idStr::snPrintf( s, 64, "localhost:%i", a.port );
		} else {
			idStr::snPrintf( s, 64, "localhost" );
		}
	} else if ( a.type == NA_IP ) {
		idStr::snPrintf( s, 64, "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3], a.port );
	}
	return s;
}

/*
========================
Sys_CompareNetAdrBase
========================
*/
bool Sys_CompareNetAdrBase( const netadr_t a, const netadr_t b ) {
	if ( a.type != b.type ) {
		return false;
	}
	if ( a.type == NA_LOOPBACK ) {
		return a.port == b.port;
	}
	if ( a.type == NA_IP ) {
		return a.ip[0] == b.ip[0] && a.ip[1] == b.ip[1] && a.ip[2] == b.ip[2] && a.ip[3] == b.ip[3];
	}
	return false;
}

/*
========================
Sys_GetLocalIP
========================
*/
const char * Sys_GetLocalIP( int i ) {
	return "127.0.0.1";
}

/*
================================================================================================

	idUDP (closed)

================================================================================================
*/

idUDP::idUDP() {
	netSocket = 0;
	memset( &bound_to, 0, sizeof( bound_to ) );
	silent = false;
	packetsRead = 0;
	bytesRead = 0;
	packetsWritten = 0;
	bytesWritten = 0;
}

idUDP::~idUDP() {
	Close();
}

bool idUDP::InitForPort( int portNumber ) {
	bound_to.type = NA_LOOPBACK;
	bound_to.port = (unsigned short)portNumber;
	return false;
}

void idUDP::Close() {
	netSocket = 0;
	memset( &bound_to, 0, sizeof( bound_to ) );
}

bool idUDP::GetPacket( netadr_t &from, void *data, int &size, int maxSize ) {
	return false;
}

bool idUDP::GetPacketBlocking( netadr_t &from, void *data, int &size, int maxSize, int timeout ) {
	return false;
}

void idUDP::SendPacket( const netadr_t to, const void *data, int size ) {
}
