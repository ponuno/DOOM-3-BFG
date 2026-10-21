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

static int		c3dSkippedNotLinear = 0;
static bool		c3dWarnedNotLinear = false;

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
static void N3DS_DrawView( const viewDef_t * viewDef ) {
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

		if ( surf->space != lastSpace ) {
			N3DS_SetMVP( surf->space->mvp );
			lastSpace = surf->space;
		}
		N3DS_MaterialColor( material );
		N3DSGPU_DrawIndexed( vbBase + vb.GetOffset(), sizeof( idDrawVert ),
			(const unsigned short *)( ibBase + ib.GetOffset() ), surf->numIndexes );

		backEnd.pc.c_drawElements++;
		backEnd.pc.c_drawIndexes += surf->numIndexes;
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

	N3DSGPU_BeginFrame( 0x182030FF );
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
	N3DSGPU_EndFrame();

	backEnd.pc.totalMicroSec = Sys_Microseconds() - start;
}
