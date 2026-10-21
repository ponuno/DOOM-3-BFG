/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

===========================================================================
*/

/*
================================================================================================

	New 3DS backend, stage 3: textured world lit per vertex.

	Replaces RB_ExecuteBackEndCommands when n3ds_renderer is 1. The frontend runs
	unchanged. Every opaque or alpha-tested surface of the main 3D view is drawn
	once (fill pass) with its diffuse map at the n3ds_ambient level, or its first
	ambient stage at full brightness for unlit materials; perforated materials use
	their coverage image for the alpha test. Then every interaction of every light
	is added on top (light pass, depth equal): light projection x falloff x diffuse
	x light colour x max(0, N.L) per vertex, the same light model as the PC
	interaction shader without normal and specular maps. No shadows (n3ds_noShadows).
	Textures come from n3ds_textures.cpp; surfaces whose texture is not on the GPU
	(yet) get one flat colour per material.
	Vertex and index data come straight from the vertex cache buffers, which the
	null GL driver keeps in linear (GPU-visible) memory.

	Not yet: translucent surfaces, GUIs/HUD, surface texture matrices, skinning
	(GPU-skinned models are drawn in their bind pose), subviews.

================================================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../tr_local.h"
#include "n3ds_gpu.h"
#include "n3ds_textures.h"

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
static int		c3dBadMatrix = 0;
static int		c3dStatTextured = 0;
static int		c3dStatLights = 0;
static int		c3dStatLightDraws = 0;

idCVar n3ds_lighting( "n3ds_lighting", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: per-vertex light passes (0 = everything at full brightness)" );
idCVar n3ds_ambient( "n3ds_ambient", "0.06", CVAR_RENDERER | CVAR_FLOAT, "citro3d: brightness of lit surfaces where no light reaches (Doom 3 itself: 0)" );
idCVar n3ds_lightProjective( "n3ds_lightProjective", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: divide the light projection per pixel (texture unit 0 projection mode); 0 = per vertex" );

// the previous frame's draws, dumped if the GPU does not finish it
struct c3dDrawRecord_t {
	const idMaterial *	material;
	int					numIndexes;
	int					vbOffset;
	int					ibOffset;
	unsigned int		vbName;
	unsigned int		ibName;
};
static const int		C3D_MAX_RECORDS = 512;
static c3dDrawRecord_t	c3dRecords[2][C3D_MAX_RECORDS];
static int				c3dNumRecords[2];
static int				c3dRecordSet = 0;		// set being filled this frame

static void N3DS_GPUHang( unsigned int waitedMs ) {
	const int prev = c3dRecordSet;		// not flipped yet: the frame the GPU is still on
	common->Printf( "citro3d: GPU has not finished the previous frame after %u ms (3D frame %d); its %d draws:\n",
		waitedMs, c3dFrame3D, c3dNumRecords[prev] );
	for ( int i = 0; i < c3dNumRecords[prev]; i++ ) {
		const c3dDrawRecord_t & r = c3dRecords[prev][i];
		common->Printf( "  %3d %s: %d indexes, vb %u+%d, ib %u+%d\n", i,
			r.material != NULL ? r.material->GetName() : "?", r.numIndexes, r.vbName, r.vbOffset, r.ibName, r.ibOffset );
	}
}

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
	N3DSGPU_SetHangCallback( N3DS_GPUHang );
	common->Printf( "citro3d: initialized (stage 3: lit textured world, VRAM free %u KB, linear free %u KB)\n",
		N3DSGPU_VRAMFree() / 1024, N3DSGPU_LinearFree() / 1024 );
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
static bool N3DS_SetMVP( const idRenderMatrix & mvp ) {
	for ( int i = 0; i < 4; i++ ) {
		for ( int j = 0; j < 4; j++ ) {
			const float v = mvp[i][j];
			if ( IEEE_FLT_IS_NAN( v ) || IEEE_FLT_IS_INF( v ) || idMath::Fabs( v ) > 1e12f ) {
				return false;
			}
		}
	}
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
	return true;
}

/*
========================
N3DS_MaterialColor

Stable pastel colour per material name (surfaces whose texture is missing).
========================
*/
static void N3DS_MaterialColor( const idMaterial * material, float scale ) {
	const int h = idStr::Hash( material->GetName() );
	const float r = 0.35f + 0.65f * ( ( h >> 0 ) & 255 ) / 255.0f;
	const float g = 0.35f + 0.65f * ( ( h >> 8 ) & 255 ) / 255.0f;
	const float b = 0.35f + 0.65f * ( ( h >> 16 ) & 255 ) / 255.0f;
	N3DSGPU_SetColor( r * scale, g * scale, b * scale, 1.0f );
}

/*
========================
N3DS_FillImages

What the fill pass draws for a material: its diffuse map (lit by the light
passes, so only the ambient level here) or, for unlit materials, its first
ambient stage at full brightness; and the alpha-test mask of perforated ones.
========================
*/
struct n3dsFillImages_t {
	idImage *	color;
	idImage *	mask;			// coverage stage, or == color for an alpha-tested ambient stage
	bool		lit;			// color is a diffuse map
};

static void N3DS_FillImages( const idMaterial * material, const float * regs, n3dsFillImages_t & out ) {
	out.color = NULL;
	out.mask = NULL;
	out.lit = false;
	idImage * ambient = NULL;
	bool ambientAlphaTest = false;
	for ( int i = 0; i < material->GetNumStages(); i++ ) {
		const shaderStage_t * stage = material->GetStage( i );
		if ( regs != NULL && !regs[ stage->conditionRegister ] ) {
			continue;
		}
		idImage * image = stage->texture.image;
		if ( image == NULL || stage->texture.cinematic != NULL || stage->texture.texgen != TG_EXPLICIT ) {
			continue;
		}
		if ( stage->lighting == SL_DIFFUSE ) {
			if ( out.color == NULL ) {
				out.color = image;
				out.lit = true;
			}
		} else if ( stage->lighting == SL_COVERAGE ) {
			if ( out.mask == NULL ) {
				out.mask = image;
			}
		} else if ( stage->lighting == SL_AMBIENT && ambient == NULL ) {
			ambient = image;
			ambientAlphaTest = stage->hasAlphaTest;
		}
	}
	if ( out.color == NULL ) {
		out.color = ambient;
		if ( out.mask == NULL && ambientAlphaTest ) {
			out.mask = ambient;
		}
	}
}

/*
========================
N3DS_DiffuseStage

The diffuse stage of an interaction surface (first one that passes its condition).
========================
*/
static const shaderStage_t * N3DS_DiffuseStage( const idMaterial * material, const float * regs ) {
	for ( int i = 0; i < material->GetNumStages(); i++ ) {
		const shaderStage_t * stage = material->GetStage( i );
		if ( stage->lighting != SL_DIFFUSE ) {
			continue;
		}
		if ( regs != NULL && !regs[ stage->conditionRegister ] ) {
			continue;
		}
		return stage;
	}
	return NULL;
}

/*
========================
Light projection helpers (as tr_backend_draw.cpp)
========================
*/
static void N3DS_GetShaderTextureMatrix( const float * shaderRegisters, const textureStage_t * texture, float matrix[16] ) {
	matrix[0*4+0] = shaderRegisters[ texture->matrix[0][0] ];
	matrix[1*4+0] = shaderRegisters[ texture->matrix[0][1] ];
	matrix[2*4+0] = 0.0f;
	matrix[3*4+0] = shaderRegisters[ texture->matrix[0][2] ];

	matrix[0*4+1] = shaderRegisters[ texture->matrix[1][0] ];
	matrix[1*4+1] = shaderRegisters[ texture->matrix[1][1] ];
	matrix[2*4+1] = 0.0f;
	matrix[3*4+1] = shaderRegisters[ texture->matrix[1][2] ];

	if ( matrix[3*4+0] < -40.0f || matrix[12] > 40.0f ) {
		matrix[3*4+0] -= (int)matrix[3*4+0];
	}
	if ( matrix[13] < -40.0f || matrix[13] > 40.0f ) {
		matrix[13] -= (int)matrix[13];
	}

	matrix[0*4+2] = 0.0f;
	matrix[1*4+2] = 0.0f;
	matrix[2*4+2] = 1.0f;
	matrix[3*4+2] = 0.0f;

	matrix[0*4+3] = 0.0f;
	matrix[1*4+3] = 0.0f;
	matrix[2*4+3] = 0.0f;
	matrix[3*4+3] = 1.0f;
}

static void N3DS_BakeTextureMatrixIntoTexgen( idPlane lightProject[3], const float * textureMatrix ) {
	float genMatrix[16];
	float final[16];

	genMatrix[0*4+0] = lightProject[0][0];
	genMatrix[1*4+0] = lightProject[0][1];
	genMatrix[2*4+0] = lightProject[0][2];
	genMatrix[3*4+0] = lightProject[0][3];

	genMatrix[0*4+1] = lightProject[1][0];
	genMatrix[1*4+1] = lightProject[1][1];
	genMatrix[2*4+1] = lightProject[1][2];
	genMatrix[3*4+1] = lightProject[1][3];

	genMatrix[0*4+2] = 0.0f;
	genMatrix[1*4+2] = 0.0f;
	genMatrix[2*4+2] = 0.0f;
	genMatrix[3*4+2] = 0.0f;

	genMatrix[0*4+3] = lightProject[2][0];
	genMatrix[1*4+3] = lightProject[2][1];
	genMatrix[2*4+3] = lightProject[2][2];
	genMatrix[3*4+3] = lightProject[2][3];

	R_MatrixMultiply( genMatrix, textureMatrix, final );

	lightProject[0][0] = final[0*4+0];
	lightProject[0][1] = final[1*4+0];
	lightProject[0][2] = final[2*4+0];
	lightProject[0][3] = final[3*4+0];

	lightProject[1][0] = final[0*4+1];
	lightProject[1][1] = final[1*4+1];
	lightProject[1][2] = final[2*4+1];
	lightProject[1][3] = final[3*4+1];
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

// vertex/index buffer lookup for one view, remembering the last buffers
struct n3dsBuffers_t {
	GLuint		lastVBName;
	GLuint		lastIBName;
	byte *		vbBase;
	byte *		ibBase;
	bool		vbLinear;
	bool		ibLinear;
	bool		validate;
};

// resolves a surface's buffers; NULL vertex pointer when it cannot be drawn
static const byte * N3DS_SurfaceBuffers( const drawSurf_t * surf, n3dsBuffers_t & b, const unsigned short ** indexes,
		c3dDrawRecord_t * record ) {
	idVertexBuffer vb;
	idIndexBuffer ib;
	if ( !vertexCache.GetVertexBuffer( surf->ambientCache, &vb ) ) {
		return NULL;
	}
	if ( !vertexCache.GetIndexBuffer( surf->indexCache, &ib ) ) {
		return NULL;
	}
	const GLuint vbName = (GLuint)(UINT_PTR)vb.GetAPIObject();
	const GLuint ibName = (GLuint)(UINT_PTR)ib.GetAPIObject();
	if ( vbName != b.lastVBName ) {
		b.vbBase = (byte *)NullGL_BufferStorage( vbName, &b.vbLinear );
		b.lastVBName = vbName;
	}
	if ( ibName != b.lastIBName ) {
		b.ibBase = (byte *)NullGL_BufferStorage( ibName, &b.ibLinear );
		b.lastIBName = ibName;
	}
	if ( b.vbBase == NULL || b.ibBase == NULL || !b.vbLinear || !b.ibLinear ) {
		c3dSkippedNotLinear++;
		if ( !c3dWarnedNotLinear ) {
			c3dWarnedNotLinear = true;
			common->Printf( "citro3d: vertex cache buffer %u/%u is not in linear memory, its surfaces are skipped\n", vbName, ibName );
		}
		return NULL;
	}
	if ( b.validate && !N3DS_ValidateDraw( surf, surf->material, vb, ib, b.vbBase, b.ibBase ) ) {
		return NULL;
	}
	record->material = surf->material;
	record->numIndexes = surf->numIndexes;
	record->vbOffset = vb.GetOffset();
	record->ibOffset = ib.GetOffset();
	record->vbName = vbName;
	record->ibName = ibName;
	*indexes = (const unsigned short *)( b.ibBase + ib.GetOffset() );
	return b.vbBase + vb.GetOffset();
}

// false when the command buffer is nearly full: the rest of the frame is dropped
// (a truncated draw sequence hangs the GPU)
static bool N3DS_CommandRoom( int draws ) {
	if ( N3DSGPU_CmdBufUsage() > 0.9f ) {
		if ( c3dBadLogged < 8 ) {
			c3dBadLogged++;
			common->Printf( "citro3d: command buffer 90%% full after %d draws, rest of the frame dropped\n", draws );
		}
		return false;
	}
	return true;
}

static void N3DS_Draw( const drawSurf_t * surf, const byte * verts, const unsigned short * indexes,
		const c3dDrawRecord_t & record, int test ) {
	if ( c3dNumRecords[c3dRecordSet] < C3D_MAX_RECORDS ) {
		c3dRecords[c3dRecordSet][c3dNumRecords[c3dRecordSet]++] = record;
	}
	if ( test == 2 ) {
		idVertexBuffer vb;
		vertexCache.GetVertexBuffer( surf->ambientCache, &vb );
		N3DSGPU_DrawIndexedCopied( verts, sizeof( idDrawVert ), vb.GetSize() / (int)sizeof( idDrawVert ), indexes, surf->numIndexes );
	} else {
		N3DSGPU_DrawIndexed( verts, sizeof( idDrawVert ), indexes, surf->numIndexes );
	}
	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += surf->numIndexes;
	c3dStatDraws++;
	c3dStatTris += surf->numIndexes / 3;
}

static bool N3DS_SkipSurface( const drawSurf_t * surf ) {
	const idMaterial * material = surf->material;
	if ( material == NULL || surf->numIndexes == 0 ) {
		return true;
	}
	return ( material->Coverage() == MC_TRANSLUCENT || material->GetSort() >= SS_POST_PROCESS );
}

static void N3DS_DrawView( const viewDef_t * viewDef ) {
	const int test = n3ds_c3dTest.GetInteger();
	if ( test == 1 ) {
		N3DSGPU_DrawTestTriangle();
		c3dStatDraws++;
		return;
	}
	const int maxDraws = ( test == 2 ) ? 1 : n3ds_c3dMaxDraws.GetInteger();
	n3dsBuffers_t buffers;
	memset( &buffers, 0, sizeof( buffers ) );
	buffers.validate = n3ds_c3dValidate.GetBool();
	int draws = 0;
	const bool lighting = n3ds_lighting.GetBool();
	const float ambient = lighting ? n3ds_ambient.GetFloat() : 1.0f;
	c3dDrawRecord_t record;

	//----------------------------------
	// fill pass: depth, ambient level of lit surfaces, unlit surfaces at full brightness
	//----------------------------------
	const viewEntity_t * lastSpace = NULL;
	bool spaceOk = true;
	N3DSGPU_SetPassParams( 1, 0 );
	{
		const float noLight[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		N3DSGPU_SetLightUniforms( noLight, noLight, noLight, noLight, noLight );
	}
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t * surf = viewDef->drawSurfs[i];
		if ( N3DS_SkipSurface( surf ) ) {
			continue;
		}
		const idMaterial * material = surf->material;
		const unsigned short * indexes;
		const byte * verts = N3DS_SurfaceBuffers( surf, buffers, &indexes, &record );
		if ( verts == NULL ) {
			continue;
		}
		if ( maxDraws > 0 && draws >= maxDraws ) {
			break;
		}
		if ( !N3DS_CommandRoom( draws ) ) {
			return;
		}
		if ( surf->space != lastSpace ) {
			spaceOk = N3DS_SetMVP( surf->space->mvp );
			lastSpace = surf->space;
			if ( !spaceOk && c3dBadMatrix++ < 8 ) {
				common->Printf( "citro3d: non-finite matrix for %s, its surfaces are skipped\n", material->GetName() );
			}
		}
		if ( !spaceOk ) {
			continue;
		}
		draws++;

		n3dsFillImages_t images;
		N3DS_FillImages( material, surf->shaderRegisters, images );
		void * colorTex = N3DS_TexForImage( images.color );
		void * maskTex = NULL;
		int alphaSource = 0;
		if ( colorTex != NULL && images.mask != NULL ) {
			if ( images.mask == images.color ) {
				alphaSource = 1;
			} else {
				maskTex = N3DS_TexForImage( images.mask );
				alphaSource = ( maskTex != NULL ) ? 2 : 0;
			}
		}
		const float level = images.lit ? ambient : 1.0f;
		if ( colorTex != NULL ) {
			N3DSGPU_SetColor( level, level, level, 1.0f );
			c3dStatTextured++;
		} else {
			N3DS_MaterialColor( material, level );
		}
		N3DSGPU_SetFillPass( colorTex, maskTex, alphaSource );
		N3DS_Draw( surf, verts, indexes, record, test );
		if ( c3dFrame3D == 0 && draws <= 3 ) {
			common->Printf( "citro3d: draw %d: %s, %d indexes, vb %p, ib %p\n", draws, material->GetName(),
				surf->numIndexes, verts, indexes );
		}
	}

	if ( !lighting || test != 0 ) {
		return;
	}

	//----------------------------------
	// light passes: every interaction of every light, added on top
	//----------------------------------
	const bool projective = n3ds_lightProjective.GetBool();
	const float lightScale = r_lightScale.GetFloat() * 0.25f;		// x 4 in the texture combiner
	N3DSGPU_SetPassParams( 0, projective ? 1 : 0 );
	for ( const viewLight_t * vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next ) {
		const idMaterial * lightShader = vLight->lightShader;
		if ( lightShader == NULL || lightShader->IsFogLight() || lightShader->IsBlendLight() ) {
			continue;
		}
		if ( vLight->localInteractions == NULL && vLight->globalInteractions == NULL ) {
			continue;
		}
		c3dStatLights++;
		const float * lightRegs = vLight->shaderRegisters;
		void * falloffTex = N3DS_TexForImage( vLight->falloffImage );

		for ( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ ) {
			const shaderStage_t * lightStage = lightShader->GetStage( lightStageNum );
			if ( !lightRegs[ lightStage->conditionRegister ] ) {
				continue;
			}
			const idVec4 lightColor(
				lightScale * lightRegs[ lightStage->color.registers[0] ],
				lightScale * lightRegs[ lightStage->color.registers[1] ],
				lightScale * lightRegs[ lightStage->color.registers[2] ],
				lightRegs[ lightStage->color.registers[3] ] );
			if ( lightColor.x <= 0.0f && lightColor.y <= 0.0f && lightColor.z <= 0.0f ) {
				continue;
			}
			float lightTextureMatrix[16];
			if ( lightStage->texture.hasMatrix ) {
				N3DS_GetShaderTextureMatrix( lightRegs, &lightStage->texture, lightTextureMatrix );
			}
			void * projectionTex = N3DS_TexForImage( lightStage->texture.image );

			lastSpace = NULL;
			for ( int list = 0; list < 2; list++ ) {
				for ( const drawSurf_t * surf = ( list == 0 ) ? vLight->localInteractions : vLight->globalInteractions;
						surf != NULL; surf = surf->nextOnLight ) {
					if ( N3DS_SkipSurface( surf ) ) {
						continue;
					}
					const shaderStage_t * diffuseStage = N3DS_DiffuseStage( surf->material, surf->shaderRegisters );
					if ( diffuseStage == NULL ) {
						continue;
					}
					const unsigned short * indexes;
					const byte * verts = N3DS_SurfaceBuffers( surf, buffers, &indexes, &record );
					if ( verts == NULL ) {
						continue;
					}
					if ( !N3DS_CommandRoom( draws ) ) {
						return;
					}
					if ( surf->space != lastSpace ) {
						lastSpace = surf->space;
						spaceOk = N3DS_SetMVP( surf->space->mvp );
						if ( spaceOk ) {
							// light origin and projection in model space
							idVec4 localLightOrigin( 0.0f, 0.0f, 0.0f, lightShader->IsAmbientLight() ? 0.0f : 1.0f );
							R_GlobalPointToLocal( surf->space->modelMatrix, vLight->globalLightOrigin, localLightOrigin.ToVec3() );
							idPlane lightProjection[4];
							for ( int k = 0; k < 4; k++ ) {
								R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[k], lightProjection[k] );
							}
							if ( lightStage->texture.hasMatrix ) {
								N3DS_BakeTextureMatrixIntoTexgen( lightProjection, lightTextureMatrix );
							}
							N3DSGPU_SetLightUniforms( localLightOrigin.ToFloatPtr(), lightProjection[0].ToFloatPtr(),
								lightProjection[1].ToFloatPtr(), lightProjection[2].ToFloatPtr(), lightProjection[3].ToFloatPtr() );
						}
					}
					if ( !spaceOk ) {
						continue;
					}
					draws++;

					// light colour x the diffuse stage colour
					const float * surfRegs = surf->shaderRegisters;
					float r = lightColor.x, g = lightColor.y, b = lightColor.z;
					if ( surfRegs != NULL ) {
						r *= surfRegs[ diffuseStage->color.registers[0] ];
						g *= surfRegs[ diffuseStage->color.registers[1] ];
						b *= surfRegs[ diffuseStage->color.registers[2] ];
					}
					N3DSGPU_SetColor( idMath::ClampFloat( 0.0f, 1.0f, r ), idMath::ClampFloat( 0.0f, 1.0f, g ),
						idMath::ClampFloat( 0.0f, 1.0f, b ), 1.0f );
					void * diffuseTex = N3DS_TexForImage( diffuseStage->texture.image );
					N3DSGPU_SetLightPass( projectionTex, falloffTex, diffuseTex, projective ? 1 : 0 );
					N3DS_Draw( surf, verts, indexes, record, test );
					c3dStatLightDraws++;
				}
			}
		}
	}
	// back to the fill pass setup for the next view
	N3DSGPU_SetPassParams( 1, 0 );
	N3DSGPU_SetFillPass( NULL, NULL, 0 );
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

	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: waiting for GPU" );
	N3DSGPU_BeginFrame( 0x182030FF );
	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: drawing" );
	N3DS_TexBeginFrame( has3D );
	c3dRecordSet ^= 1;
	c3dNumRecords[c3dRecordSet] = 0;
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
	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: submitting" );
	N3DSGPU_EndFrame();
	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: idle" );
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
		common->Printf( "PERF   citro3d: %.0f draws (%.0f textured, %.0f light draws for %.1f lights) %.0f tris per frame, GPU %.1f ms, cmdbuf peak %.0f%%, bad draws %d\n",
			c3dStatDraws / f, c3dStatTextured / f, c3dStatLightDraws / f, c3dStatLights / f, c3dStatTris / f, c3dStatGpuMs / f,
			c3dStatCmdMax * 100.0f, c3dStatBad );
		idStr texLine;
		N3DS_TexStatsLine( texLine );
		common->Printf( "PERF   citro3d: %s\n", texLine.c_str() );
		c3dStatTextured = c3dStatLights = c3dStatLightDraws = 0;
		c3dStatStart = now;
		c3dStatFrames = c3dStatDraws = c3dStatTris = c3dStatBad = 0;
		c3dStatCmdMax = c3dStatGpuMs = 0.0f;
	}

	backEnd.pc.totalMicroSec = Sys_Microseconds() - start;
}
