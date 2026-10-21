/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

===========================================================================
*/

/*
================================================================================================

	New 3DS backend, stage 1: the world without textures.

	Replaces RB_ExecuteBackEndCommands when n3ds_renderer is 1. The frontend runs
	unchanged; here every opaque surface of the main 3D view is drawn with one
	flat colour per material, shaded by its normal (see n3ds_vshader.v.pica).
	Vertex and index data come straight from the vertex cache buffers, which the
	null GL driver keeps in linear (GPU-visible) memory.

	Not yet: textures, lighting, translucent surfaces, GUIs/HUD, skinning
	(GPU-skinned models are drawn in their bind pose), subviews.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../tr_local.h"
#include "n3ds_gpu.h"

void * NullGL_BufferStorage( GLuint name, bool * linear );	// gl_null.cpp

bool r_n3dsC3D = false;

idCVar n3ds_c3dMaxDraws( "n3ds_c3dMaxDraws", "0", CVAR_RENDERER | CVAR_INTEGER, "citro3d: draw at most this many surfaces per frame (0 = all), for bisecting GPU hangs" );
idCVar n3ds_c3dValidate( "n3ds_c3dValidate", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: check every draw's index range against its vertex/index buffers" );
idCVar n3ds_c3dTest( "n3ds_c3dTest", "0", CVAR_RENDERER | CVAR_INTEGER, "citro3d bring-up test: 1 = only a test triangle, 2 = engine surfaces copied to a private buffer first (first one per frame)" );
idCVar n3ds_c3dTrace( "n3ds_c3dTrace", "3", CVAR_RENDERER | CVAR_INTEGER, "citro3d: log begin/end of the first N 3D frames" );

static int		c3dSkippedNotLinear = 0;
static bool		c3dWarnedNotLinear = false;
static int		c3dFrame3D = 0;			// 3D frames drawn so far
static int		c3dBadDraws = 0;		// rejected by validation (total)
static int		c3dBadLogged = 0;
static int		c3dStatFrames = 0;
static int		c3dStatDraws = 0;
static int		c3dStatTris = 0;
static int		c3dStatBad = 0;
static float	c3dStatCmdMax = 0.0f;
static float	c3dStatGpuMs = 0.0f;
static int		c3dStatStart = 0;

/*
========================
RB_N3DS_Init
========================
*/
bool RB_N3DS_Init() {
	if ( !N3DSGPU_Init() ) {
		common->Printf( "citro3d: init failed: %s -- staying on the null renderer\n", N3DSGPU_LastError() );
		r_n3dsC3D = false;
		return false;
	}
	common->Printf( "citro3d: initialized (stage 1: untextured world)\n" );
	r_n3dsC3D = true;
	return true;
}

/*
========================
RB_N3DS_Shutdown
========================
*/
void RB_N3DS_Shutdown() {
	if ( r_n3dsC3D ) {
		N3DSGPU_Shutdown();
		r_n3dsC3D = false;
	}
}

/*
========================
RB_N3DS_FlushForGPU
========================
*/
void RB_N3DS_FlushForGPU( const void * p, int size ) {
	if ( r_n3dsC3D ) {
		N3DSGPU_FlushRange( p, (unsigned int)size );
	}
}

/*
========================
N3DS_SetMVP

GL clip space (z in -w..w) -> PICA: rotated a quarter turn for the 3DS screen
orientation and z remapped to -w..0, the same as citro3d's *Tilt projections:
	{{0,1,0,0},{-1,0,0,0},{0,0,0.5,-0.5},{0,0,0,1}} * mvp
========================
*/
static void N3DS_SetMVP( const idRenderMatrix & mvp ) {
	float rows[16];
	const float * r0 = mvp[0];
	const float * r1 = mvp[1];
	const float * r2 = mvp[2];
	const float * r3 = mvp[3];
	for ( int c = 0; c < 4; c++ ) {
		rows[0 * 4 + c] = r1[c];
		rows[1 * 4 + c] = -r0[c];
		rows[2 * 4 + c] = 0.5f * r2[c] - 0.5f * r3[c];
		rows[3 * 4 + c] = r3[c];
	}
	N3DSGPU_SetMatrix( rows );
}

/*
========================
N3DS_MaterialColor

Stable pastel colour per material name.
========================
*/
static void N3DS_MaterialColor( const idMaterial * material ) {
	const int h = idStr::Hash( material->GetName() );
	const float r = 0.35f + 0.65f * ( ( h >> 0 ) & 255 ) / 255.0f;
	const float g = 0.35f + 0.65f * ( ( h >> 8 ) & 255 ) / 255.0f;
	const float b = 0.35f + 0.65f * ( ( h >> 16 ) & 255 ) / 255.0f;
	N3DSGPU_SetColor( r, g, b, 1.0f );
}

/*
========================
N3DS_DrawView
========================
*/
static bool N3DS_ValidateDraw( const drawSurf_t * surf, const idMaterial * material, const idVertexBuffer & vb,
		const idIndexBuffer & ib, const byte * vbBase, const byte * ibBase ) {
	const int numVerts = vb.GetSize() / (int)sizeof( idDrawVert );
	const char * why = NULL;
	int maxIndex = -1;
	if ( surf->numIndexes * (int)sizeof( triIndex_t ) > ib.GetSize() ) {
		why = "index count beyond index allocation";
	} else if ( ( surf->numIndexes % 3 ) != 0 ) {
		why = "index count not a multiple of 3";
	} else {
		const triIndex_t * idx = (const triIndex_t *)( ibBase + ib.GetOffset() );
		for ( int k = 0; k < surf->numIndexes; k++ ) {
			if ( idx[k] > maxIndex ) {
				maxIndex = idx[k];
			}
		}
		if ( maxIndex >= numVerts ) {
			why = "index beyond vertex allocation";
		}
	}
	if ( why == NULL ) {
		return true;
	}
	c3dBadDraws++;
	c3dStatBad++;
	if ( c3dBadLogged < 8 ) {
		c3dBadLogged++;
		common->Printf( "citro3d: bad draw (%s): %s, %d indexes (max %d), %d verts, vb %p+%d size %d, ib %p+%d size %d\n",
			why, material->GetName(), surf->numIndexes, maxIndex, numVerts,
			vbBase, vb.GetOffset(), vb.GetSize(), ibBase, ib.GetOffset(), ib.GetSize() );
	}
	return false;
}

static void N3DS_DrawView( const viewDef_t * viewDef ) {
	const int test = n3ds_c3dTest.GetInteger();
	if ( test == 1 ) {
		N3DSGPU_DrawTestTriangle();
		c3dStatDraws++;
		return;
	}
	const int maxDraws = ( test == 2 ) ? 1 : n3ds_c3dMaxDraws.GetInteger();
	const bool validate = n3ds_c3dValidate.GetBool();
	int draws = 0;
	const viewEntity_t * lastSpace = NULL;
	GLuint lastVBName = 0, lastIBName = 0;
	byte * vbBase = NULL;
	byte * ibBase = NULL;
	bool vbLinear = false, ibLinear = false;

	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t * surf = viewDef->drawSurfs[i];
		const idMaterial * material = surf->material;
		if ( material == NULL || surf->numIndexes == 0 ) {
			continue;
		}
		if ( material->Coverage() == MC_TRANSLUCENT || material->GetSort() >= SS_POST_PROCESS ) {
			continue;
		}

		idVertexBuffer vb;
		idIndexBuffer ib;
		if ( !vertexCache.GetVertexBuffer( surf->ambientCache, &vb ) ) {
			continue;
		}
		if ( !vertexCache.GetIndexBuffer( surf->indexCache, &ib ) ) {
			continue;
		}

		const GLuint vbName = (GLuint)(UINT_PTR)vb.GetAPIObject();
		const GLuint ibName = (GLuint)(UINT_PTR)ib.GetAPIObject();
		if ( vbName != lastVBName ) {
			vbBase = (byte *)NullGL_BufferStorage( vbName, &vbLinear );
			lastVBName = vbName;
		}
		if ( ibName != lastIBName ) {
			ibBase = (byte *)NullGL_BufferStorage( ibName, &ibLinear );
			lastIBName = ibName;
		}
		if ( vbBase == NULL || ibBase == NULL || !vbLinear || !ibLinear ) {
			c3dSkippedNotLinear++;
			if ( !c3dWarnedNotLinear ) {
				c3dWarnedNotLinear = true;
				common->Printf( "citro3d: vertex cache buffer %u/%u is not in linear memory, its surfaces are skipped\n", vbName, ibName );
			}
			continue;
		}

		if ( validate && !N3DS_ValidateDraw( surf, material, vb, ib, vbBase, ibBase ) ) {
			continue;
		}
		if ( maxDraws > 0 && draws >= maxDraws ) {
			break;
		}
		// never let the command buffer overflow: a truncated draw sequence hangs the GPU
		if ( N3DSGPU_CmdBufUsage() > 0.9f ) {
			if ( c3dBadLogged < 8 ) {
				c3dBadLogged++;
				common->Printf( "citro3d: command buffer 90%% full after %d draws, rest of the frame dropped\n", draws );
			}
			break;
		}
		draws++;

		if ( surf->space != lastSpace ) {
			N3DS_SetMVP( surf->space->mvp );
			lastSpace = surf->space;
		}
		N3DS_MaterialColor( material );
		if ( test == 2 ) {
			N3DSGPU_DrawIndexedCopied( vbBase + vb.GetOffset(), sizeof( idDrawVert ), vb.GetSize() / (int)sizeof( idDrawVert ),
				(const unsigned short *)( ibBase + ib.GetOffset() ), surf->numIndexes );
		} else {
			N3DSGPU_DrawIndexed( vbBase + vb.GetOffset(), sizeof( idDrawVert ),
				(const unsigned short *)( ibBase + ib.GetOffset() ), surf->numIndexes );
		}
		if ( c3dFrame3D == 0 && draws <= 3 ) {
			common->Printf( "citro3d: draw %d: %s, %d indexes, vb %p+%d, ib %p+%d\n", draws, material->GetName(),
				surf->numIndexes, vbBase, vb.GetOffset(), ibBase, ib.GetOffset() );
		}

		backEnd.pc.c_drawElements++;
		backEnd.pc.c_drawIndexes += surf->numIndexes;
		c3dStatDraws++;
		c3dStatTris += surf->numIndexes / 3;
	}
}

/*
========================
RB_N3DS_ExecuteBackEndCommands
========================
*/
void RB_N3DS_ExecuteBackEndCommands( const emptyCommand_t * cmds ) {
	if ( cmds->commandId == RC_NOP && !cmds->next ) {
		return;
	}
	const uint64 start = Sys_Microseconds();

	// is there a 3D view this frame? (for tracing the first ones)
	bool has3D = false;
	for ( const emptyCommand_t * c = cmds; c != NULL; c = (const emptyCommand_t *)c->next ) {
		if ( c->commandId == RC_DRAW_VIEW_3D ) {
			const viewDef_t * v = ( (const drawSurfsCommand_t *)c )->viewDef;
			if ( v != NULL && !v->isSubview && v->viewEntitys != NULL ) {
				has3D = true;
			}
		}
	}
	const bool trace = has3D && c3dFrame3D < n3ds_c3dTrace.GetInteger();
	if ( trace ) {
		common->Printf( "citro3d: 3D frame %d: begin\n", c3dFrame3D );
	}

	N3DSGPU_BeginFrame( 0x182030FF );
	if ( trace ) {
		common->Printf( "citro3d: 3D frame %d: GPU ready, previous frame %.2f ms\n", c3dFrame3D, N3DSGPU_DrawingTimeMs() );
	}
	const int drawsBefore = c3dStatDraws;
	for ( ; cmds != NULL; cmds = (const emptyCommand_t *)cmds->next ) {
		if ( cmds->commandId != RC_DRAW_VIEW_3D ) {
			continue;
		}
		const viewDef_t * viewDef = ( (const drawSurfsCommand_t *)cmds )->viewDef;
		if ( viewDef == NULL || viewDef->isSubview || viewDef->viewEntitys == NULL ) {
			continue;
		}
		N3DS_DrawView( viewDef );
	}
	const float cmdUse = N3DSGPU_CmdBufUsage();
	if ( trace ) {
		common->Printf( "citro3d: 3D frame %d: %d draws, command buffer %.0f%%, submitting\n",
			c3dFrame3D, c3dStatDraws - drawsBefore, cmdUse * 100.0f );
	}
	N3DSGPU_EndFrame();
	if ( trace ) {
		common->Printf( "citro3d: 3D frame %d: submitted\n", c3dFrame3D );
	}
	if ( has3D ) {
		c3dFrame3D++;
	}

	// a line every 2 seconds next to the PERF lines
	c3dStatFrames++;
	c3dStatCmdMax = Max( c3dStatCmdMax, cmdUse );
	c3dStatGpuMs += N3DSGPU_DrawingTimeMs();
	const int now = Sys_Milliseconds();
	if ( c3dStatStart == 0 ) {
		c3dStatStart = now;
	}
	if ( now - c3dStatStart >= 2000 && c3dFrame3D > 0 ) {
		const float f = (float)Max( c3dStatFrames, 1 );
		common->Printf( "PERF   citro3d: %.0f draws %.0f tris per frame, GPU %.1f ms, cmdbuf peak %.0f%%, bad draws %d\n",
			c3dStatDraws / f, c3dStatTris / f, c3dStatGpuMs / f, c3dStatCmdMax * 100.0f, c3dStatBad );
		c3dStatStart = now;
		c3dStatFrames = c3dStatDraws = c3dStatTris = c3dStatBad = 0;
		c3dStatCmdMax = c3dStatGpuMs = 0.0f;
	}

	backEnd.pc.totalMicroSec = Sys_Microseconds() - start;
}
