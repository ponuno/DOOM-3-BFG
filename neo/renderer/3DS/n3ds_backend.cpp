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
	n3ds_lighting 2 (default) uses the PICA fragment lighting instead, see
	N3DS_DrawViewHW below.
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
idCVar n3ds_c3dHangRegs( "n3ds_c3dHangRegs", "0", CVAR_RENDERER | CVAR_BOOL, "citro3d: when the GPU hangs, also read its registers into sdmc:/doom3/gpuhang.bin" );
idCVar n3ds_c3dHangHunt( "n3ds_c3dHangHunt", "0", CVAR_RENDERER | CVAR_INTEGER, "citro3d: from this 3D frame on, wait for the GPU after every draw and name the draw it never finishes (slow; 0 = off)" );
static bool c3dHangFound = false;
idCVar n3ds_c3dFullFlush( "n3ds_c3dFullFlush", "0", CVAR_RENDERER | CVAR_BOOL, "citro3d: clean the data cache over all linear memory at every frame end (old behaviour, ~69 MB per frame in the 178 MB mode)" );
idCVar n3ds_hwMaxLights( "n3ds_hwMaxLights", "24", CVAR_RENDERER | CVAR_INTEGER, "citro3d lighting 2: most lights per surface (the brightest are kept); beyond 8 the surface is drawn again, adding the next 8; each light costs GPU time on every pixel of the surface", 1, 24 );
idCVar n3ds_guiStencil( "n3ds_guiStencil", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: stencil for SWF masks (0 = ignore the stencil state, for bisecting)" );
idCVar n3ds_c3dValidate( "n3ds_c3dValidate", "0", CVAR_RENDERER | CVAR_BOOL, "citro3d: check every draw's index range against its vertex/index buffers" );
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
static int		c3dStatScissorPixels = 0;	// light rectangles, summed

idCVar n3ds_lighting( "n3ds_lighting", "2", CVAR_RENDERER | CVAR_INTEGER, "citro3d: 0 = everything at full brightness, 1 = a pass per light (projected light images, per-vertex N.L), 2 = PICA fragment lighting, one pass, up to 8 lights per surface" );
idCVar n3ds_hwLightRadius( "n3ds_hwLightRadius", "1", CVAR_RENDERER | CVAR_FLOAT, "citro3d n3ds_lighting 2: point light reach as a fraction of the smallest half-size of the light box" );
idCVar n3ds_ambientStages( "n3ds_ambientStages", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: draw ambient / blended material stages (glows, glass, particles, decals, screens) and fullscreen GUIs" );
idCVar n3ds_polygonOffset( "n3ds_polygonOffset", "4", CVAR_RENDERER | CVAR_FLOAT, "citro3d: depth bias per material polygonOffset unit (1/65536 of the depth range)" );
idCVar n3ds_hwSpotFlip( "n3ds_hwSpotFlip", "0", CVAR_RENDERER | CVAR_BOOL, "citro3d n3ds_lighting 2: reverse the spot light direction (if the flashlight lights everything but its cone)" );
idCVar n3ds_hwLightScale( "n3ds_hwLightScale", "4", CVAR_RENDERER | CVAR_INTEGER, "citro3d n3ds_lighting 2: overbright range, 2 or 4 (light colours are divided by it, the combiner multiplies)" );
idCVar n3ds_ambient( "n3ds_ambient", "0.1", CVAR_RENDERER | CVAR_FLOAT, "citro3d: brightness of lit surfaces where no light reaches (Doom 3 itself: 0)" );
idCVar n3ds_lightScissor( "n3ds_lightScissor", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: clip each light's passes to its screen rectangle" );
idCVar n3ds_lightProjective( "n3ds_lightProjective", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d: divide the light projection per pixel (texture unit 0 projection mode); 0 = per vertex" );
idCVar n3ds_bump( "n3ds_bump", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d lighting 2: normal maps (from the texture pack) through the PICA bump unit" );
idCVar n3ds_specular( "n3ds_specular", "1", CVAR_RENDERER | CVAR_BOOL, "citro3d lighting 2: specular maps x highlights" );
idCVar n3ds_specularPower( "n3ds_specularPower", "10", CVAR_RENDERER | CVAR_FLOAT, "citro3d lighting 2: highlight exponent (BFG interaction.pixel: 10)", 1, 128 );

// the previous frame's draws, dumped if the GPU does not finish it
struct c3dDrawRecord_t {
	const idMaterial *	material;
	int					numIndexes;
	int					vbOffset;
	int					ibOffset;
	unsigned int		vbName;
	unsigned int		ibName;
	const char *		texName;		// stage draws: the texture's image
	int					flags;			// C3D_REC_*
	// hardware-lit draws: diffuse, normal map, specular map (as drawn), lights
	idImage *			images[3];
	void *				gpuTex[3];
	short				numLights;
	short				alphaSource;
};
static const int C3D_REC_STAGE = 1, C3D_REC_A8 = 2, C3D_REC_STENCIL = 4, C3D_REC_SWF = 8, C3D_REC_ALPHATEST = 16, C3D_REC_GUIVIEW = 32,
	C3D_REC_LIT = 64;
static const int		C3D_MAX_RECORDS = 512;
static c3dDrawRecord_t	c3dRecords[2][C3D_MAX_RECORDS];
static int				c3dNumRecords[2];
static int				c3dRecordSet = 0;		// set being filled this frame

static void N3DS_PrintRecord( int i, const c3dDrawRecord_t & r ) {
	common->Printf( "  %3d %s: %d indexes, vb %u+%d, ib %u+%d%s%s%s%s%s%s%s\n", i,
		r.material != NULL ? r.material->GetName() : "?", r.numIndexes, r.vbName, r.vbOffset, r.ibName, r.ibOffset,
		( r.flags & C3D_REC_STAGE ) ? " | stage, tex " : "", ( r.flags & C3D_REC_STAGE ) ? ( r.texName != NULL ? r.texName : "none" ) : "",
		( r.flags & C3D_REC_A8 ) ? " A8" : "", ( r.flags & C3D_REC_STENCIL ) ? " stencil" : "", ( r.flags & C3D_REC_SWF ) ? " swf" : "",
		( r.flags & C3D_REC_ALPHATEST ) ? " alphatest" : "", ( r.flags & C3D_REC_GUIVIEW ) ? " guiview" : "" );
	if ( r.flags & C3D_REC_LIT ) {
		static const char * const what[3] = { "diffuse", "normal map", "specular map" };
		common->Printf( "        lit, %d lights, alpha source %d\n", r.numLights, r.alphaSource );
		for ( int k = 0; k < 3; k++ ) {
			if ( r.images[k] == NULL && r.gpuTex[k] == NULL ) {
				continue;
			}
			idStr desc;
			N3DS_TexDescribe( r.images[k], desc );
			char gpu[96];
			N3DSGPU_TexDescribe( r.gpuTex[k], gpu, sizeof( gpu ) );
			common->Printf( "        %s: %s | drawn with %s\n", what[k], desc.c_str(), gpu );
		}
	}
}

static void N3DS_GPUNotice( const char * text ) {
	common->Printf( "%s", text );
}

// n3ds_c3dFrameDump: a touch on the lower half of the bottom screen writes the GPU command lists of the next frames
idCVar n3ds_c3dFrameDump( "n3ds_c3dFrameDump", "8", CVAR_RENDERER | CVAR_INTEGER, "citro3d diagnostics: touching the lower half of the bottom screen writes the GPU command lists of this many following frames to sdmc:/doom3/frameN.bin and their draws to the log (0 = off)" );
static int	c3dDumpLeft = 0;
static int	c3dDumpIndex = 0;
static bool	c3dDumpKeysWere = false;

static void N3DS_GPUHang( unsigned int waitedMs ) {
	const int prev = c3dRecordSet;		// not flipped yet: the frame the GPU is still on
	common->Printf( "citro3d: GPU has not finished the previous frame after %u ms (3D frame %d); its %d draws:\n",
		waitedMs, c3dFrame3D, c3dNumRecords[prev] );
	for ( int i = 0; i < c3dNumRecords[prev]; i++ ) {
		N3DS_PrintRecord( i, c3dRecords[prev][i] );
	}
	// where the GPU stands: a transfer or the command list, then the whole frame on the card
	static char report[4096];
	N3DSGPU_HangQueueReport( report, sizeof( report ) );
	common->Printf( "%s", report );
	const char * dumpPath = "sdmc:/doom3/gpuhang.bin";
	common->Printf( "citro3d: writing the hung frame to %s%s...\n", dumpPath, n3ds_c3dHangRegs.GetBool() ? " (with the GPU registers)" : "" );
	if ( N3DSGPU_HangDump( dumpPath, n3ds_c3dHangRegs.GetBool() ? 1 : 0 ) ) {
		common->Printf( "citro3d: %s written: send it with log.txt\n", dumpPath );
	} else {
		common->Printf( "citro3d: %s could not be written\n", dumpPath );
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
	N3DSGPU_SetHangTexNamer( N3DS_TexNameForAddress );
	N3DSGPU_SetNoticeCallback( N3DS_GPUNotice );
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
static const shaderStage_t * N3DS_LightingStage( const idMaterial * material, const float * regs, stageLighting_t lighting ) {
	for ( int i = 0; i < material->GetNumStages(); i++ ) {
		const shaderStage_t * stage = material->GetStage( i );
		if ( stage->lighting != lighting ) {
			continue;
		}
		if ( regs != NULL && !regs[ stage->conditionRegister ] ) {
			continue;
		}
		return stage;
	}
	return NULL;
}

static const shaderStage_t * N3DS_DiffuseStage( const idMaterial * material, const float * regs ) {
	return N3DS_LightingStage( material, regs, SL_DIFFUSE );
}

// the bump / specular map of an interaction surface, if it has one (a plain 2D image)
static idImage * N3DS_StageImage( const idMaterial * material, const float * regs, stageLighting_t lighting ) {
	const shaderStage_t * stage = N3DS_LightingStage( material, regs, lighting );
	if ( stage == NULL || stage->texture.cinematic != NULL || stage->texture.texgen != TG_EXPLICIT ) {
		return NULL;
	}
	return stage->texture.image;
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
	record->texName = NULL;
	record->flags = 0;
	record->images[0] = record->images[1] = record->images[2] = NULL;
	record->gpuTex[0] = record->gpuTex[1] = record->gpuTex[2] = NULL;
	record->numLights = 0;
	record->alphaSource = 0;
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
	const int hunt = n3ds_c3dHangHunt.GetInteger();
	if ( hunt > 0 && c3dFrame3D >= hunt && !c3dHangFound ) {
		const int sync = N3DSGPU_DebugSync( 1500 );
		if ( sync == 2 ) {
			static int lostLogged = 0;
			if ( lostLogged++ < 20 ) {
				common->Printf( "HANG HUNT: draw %d of 3D frame %d: the GPU went idle without its completion interrupt; counted by hand, the queue went on:\n",
					c3dNumRecords[c3dRecordSet] - 1, c3dFrame3D );
				N3DS_PrintRecord( c3dNumRecords[c3dRecordSet] - 1, record );
			}
		}
		if ( sync == 0 ) {
			c3dHangFound = true;
			common->Printf( "HANG HUNT: the GPU never finished draw %d of 3D frame %d:\n", c3dNumRecords[c3dRecordSet] - 1, c3dFrame3D );
			N3DS_PrintRecord( c3dNumRecords[c3dRecordSet] - 1, record );
			const int n = c3dNumRecords[c3dRecordSet];
			common->Printf( "HANG HUNT: draws before it in this frame (all finished):\n" );
			for ( int i = Max( 0, n - 9 ); i < n - 1; i++ ) {
				N3DS_PrintRecord( i, c3dRecords[c3dRecordSet][i] );
			}
			// its geometry as the GPU got it (the lit vertex shader can be replayed on the PC)
			const float * mvp = (const float *)&surf->space->mvp;
			const float * mv = surf->space->modelViewMatrix;
			common->Printf( "HANG HUNT: mvp %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", mvp[0], mvp[1], mvp[2], mvp[3],
				mvp[4], mvp[5], mvp[6], mvp[7], mvp[8], mvp[9], mvp[10], mvp[11], mvp[12], mvp[13], mvp[14], mvp[15] );
			common->Printf( "HANG HUNT: modelView %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", mv[0], mv[1], mv[2], mv[3],
				mv[4], mv[5], mv[6], mv[7], mv[8], mv[9], mv[10], mv[11], mv[12], mv[13], mv[14], mv[15] );
			for ( int k = 0; k < surf->numIndexes && k < 48; k++ ) {
				const idDrawVert & v = ( (const idDrawVert *)verts )[indexes[k]];
				common->Printf( "HANG HUNT: idx %d = %d: xyz %g %g %g st %04x %04x normal %d %d %d %d tangent %d %d %d %d color %d %d %d %d\n",
					k, indexes[k], v.xyz.x, v.xyz.y, v.xyz.z, v.st[0], v.st[1], v.normal[0], v.normal[1], v.normal[2], v.normal[3],
					v.tangent[0], v.tangent[1], v.tangent[2], v.tangent[3], v.color[0], v.color[1], v.color[2], v.color[3] );
			}
			// the stuck command list holds just this draw (and the state set for it)
			static char report[4096];
			N3DSGPU_HangQueueReport( report, sizeof( report ) );
			common->Printf( "%s", report );
			if ( N3DSGPU_HangDump( "sdmc:/doom3/gpuhang_hunt.bin", 0 ) ) {
				common->Printf( "HANG HUNT: sdmc:/doom3/gpuhang_hunt.bin written: send it with log.txt\n" );
			}
		}
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
	const bool lighting = ( n3ds_lighting.GetInteger() == 1 );
	const float ambient = lighting ? n3ds_ambient.GetFloat() : 1.0f;
	c3dDrawRecord_t record;

	//----------------------------------
	// fill pass: depth, ambient level of lit surfaces, unlit surfaces at full brightness
	//----------------------------------
	const viewEntity_t * lastSpace = NULL;
	bool spaceOk = true;
	N3DSGPU_SetScissor( 0, 0, 0, 0, 0 );
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
		// unlit materials: black with depth, their ambient stages are drawn on top later
		const float level = images.lit ? ambient : ( n3ds_ambientStages.GetBool() ? 0.0f : 1.0f );
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
	const bool useScissor = n3ds_lightScissor.GetBool();
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

		// only the light's rectangle on screen is touched. GL window pixels (x right,
		// y up, viewport 400 x 240) -> render target pixels (240 x 400, rotated):
		// target x = window y, target y = 400 - window x (see N3DS_SetMVP)
		if ( useScissor ) {
			if ( vLight->scissorRect.IsEmpty() ) {
				continue;
			}
			const int wx1 = viewDef->viewport.x1 + vLight->scissorRect.x1;
			const int wx2 = viewDef->viewport.x1 + vLight->scissorRect.x2;		// inclusive
			const int wy1 = viewDef->viewport.y1 + vLight->scissorRect.y1;
			const int wy2 = viewDef->viewport.y1 + vLight->scissorRect.y2;
			const int left = idMath::ClampInt( 0, 240, wy1 );
			const int right = idMath::ClampInt( 0, 240, wy2 + 1 );
			const int top = idMath::ClampInt( 0, 400, 400 - ( wx2 + 1 ) );
			const int bottom = idMath::ClampInt( 0, 400, 400 - wx1 );
			if ( left >= right || top >= bottom ) {
				continue;
			}
			N3DSGPU_SetScissor( 1, left, top, right, bottom );
			c3dStatScissorPixels += ( right - left ) * ( bottom - top );
		}
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
	N3DSGPU_SetScissor( 0, 0, 0, 0, 0 );
	N3DSGPU_SetPassParams( 1, 0 );
	N3DSGPU_SetFillPass( NULL, NULL, 0 );
}

/*
================================================================================================

	Hardware fragment lighting path (n3ds_lighting 2)

	One pass per surface: the PICA lights it per pixel with up to 8 lights. The
	lights of a surface come from the frontend's interaction lists (each
	interaction surface is matched to the view's surface with the same vertex
	data and space). Doom 3 lights become PICA lights:
	  point light     -> positional light, distance attenuation over its radius
	  projected light -> spot light along the frustum axis, cone from the frustum,
	                     attenuated over the falloff range
	  ambient light   -> added to the surface's ambient colour
	The projection/falloff images and normal maps are not used yet.

================================================================================================
*/

static const int N3DS_MAX_FRAME_LIGHTS = 64;

struct n3dsHWLight_t {
	float	eyePos[3];
	float	color[3];		// already / combiner scale, clamped
	float	invRadius;		// distance attenuation: table input = distance * invRadius + attnBias
	float	attnBias;
	bool	spot;
	float	spotDir[3];		// eye space
	float	cosOuter;
	bool	ambient;
};

static n3dsHWLight_t	hwFrameLights[N3DS_MAX_FRAME_LIGHTS];

/*
	n3ds_hwLightLog: what happens to every light of the main view, logged when it changes
	from one frame to the next (a light flickering while the camera moves shows up as a
	light that comes and goes, or whose surfaces / colour jump).
*/
idCVar n3ds_hwQuat( "n3ds_hwQuat", "0", CVAR_RENDERER | CVAR_INTEGER, "citro3d diagnostics: normal quaternion per vertex: 0 = from the tangent frame, 1 = same but sign made consistent with the shortest Z -> N rotation, 2 = shortest Z -> N rotation only (normal maps then turn wrong)", 0, 2 );
idCVar n3ds_hwFlatUnit1( "n3ds_hwFlatUnit1", "0", CVAR_RENDERER | CVAR_BOOL, "citro3d diagnostics: lit surfaces without a normal map or mask get a flat normal texture on unit 1 (the lighting's bump unit) instead of what the previous surface left there" );
idCVar n3ds_hwKeepEnv( "n3ds_hwKeepEnv", "0", CVAR_RENDERER | CVAR_BOOL, "citro3d diagnostics: keep the fragment lighting environment bound for unlit surfaces in the HW pass" );
idCVar n3ds_hwEnvDirty( "n3ds_hwEnvDirty", "0", CVAR_RENDERER | CVAR_INTEGER, "citro3d diagnostics: send the whole lighting environment (registers, tables, lights) again: 1 = once per view, 2 = before every lit surface", 0, 2 );
idCVar n3ds_hwReloadLights( "n3ds_hwReloadLights", "0", CVAR_RENDERER | CVAR_BOOL, "citro3d diagnostics: load every light of every surface into its slot again (no slot reuse between surfaces)" );
idCVar n3ds_hwLightLog( "n3ds_hwLightLog", "0", CVAR_RENDERER | CVAR_INTEGER, "citro3d diagnostics: log lights of the view whose state, surface count or colour changes between frames (value = most lines to print)" );

struct n3dsLightLog_t {
	int		frame;			// c3dLightLogFrame it was last seen in
	char	state;			// L lit, A ambient, N no interactions, X black / not set up, F fog / blend, O over the frame limit
	short	surfaces;		// interactions matched to a drawn surface
	short	unmatched;		// interactions with no drawn surface
	float	strength;
	float	radius;
};
static const int		N3DS_LIGHTLOG_MAX = 2048;
static n3dsLightLog_t	lightLog[N3DS_LIGHTLOG_MAX];
static int				c3dLightLogFrame = 0;
static int				hwFrameLightDef[64];		// lightDef index of hwFrameLights[i]
static const char *		hwFrameLightName[64];

/*
	n3ds_hwSurfLog: per drawn surface (static geometry, keyed by its vertex cache handle), the
	set of lights it is lit with; a line when that set changes between two consecutive frames
	in which the surface is drawn. A wall that flickers while the camera turns shows which
	light it loses and gets back.
*/
idCVar n3ds_hwSurfLog( "n3ds_hwSurfLog", "0", CVAR_RENDERER | CVAR_INTEGER, "citro3d diagnostics: log surfaces whose set of lights changes between frames (value = most lines to print)" );

static const int		N3DS_SURFLOG_LIGHTS = 24;	// = N3DS_MAX_SURF_LIGHTS
struct n3dsSurfLog_t {
	uint64	key;
	int		frame;
	byte	num;
	short	lights[N3DS_SURFLOG_LIGHTS];
};
static const int		N3DS_SURFLOG_SIZE = 8192;
static n3dsSurfLog_t	surfLog[N3DS_SURFLOG_SIZE];
static int				c3dSurfLogFrame = 0;
static int				c3dSurfLogLines = 0;

static inline uint64 N3DS_SurfGeo( const drawSurf_t * surf );

static void N3DS_SurfLogEntry( const drawSurf_t * surf, const byte * lights, int num ) {
	const int limit = n3ds_hwSurfLog.GetInteger();
	if ( limit <= 0 || c3dSurfLogLines >= limit ) {
		return;
	}
	// the vertex cache handle is shared by every entity with the same model (doors...):
	// the entity pointer (a value only, not read) tells them apart
	const uint64 key = N3DS_SurfGeo( surf ) ^ ( (uint64)(uintptr_t)( surf->space != NULL ? surf->space->entityDef : NULL ) * 0x100000001B3ull );
	unsigned int h = (unsigned int)( ( key * 0x9E3779B97F4A7C15ull ) >> 51 ) & ( N3DS_SURFLOG_SIZE - 1 );
	for ( int probe = 0; probe < 32; probe++, h = ( h + 1 ) & ( N3DS_SURFLOG_SIZE - 1 ) ) {
		n3dsSurfLog_t & e = surfLog[h];
		if ( e.key != key && e.frame >= c3dSurfLogFrame - 2 ) {
			continue;		// another surface seen recently
		}
		short now[N3DS_SURFLOG_LIGHTS];
		for ( int k = 0; k < num; k++ ) {
			now[k] = (short)hwFrameLightDef[ lights[k] ];
		}
		// sorted, so that only the set counts
		for ( int a = 1; a < num; a++ ) {
			const short v = now[a];
			int b = a - 1;
			while ( b >= 0 && now[b] > v ) {
				now[b + 1] = now[b];
				b--;
			}
			now[b + 1] = v;
		}
		if ( e.key == key && e.frame == c3dSurfLogFrame - 1 && ( e.num != num || memcmp( e.lights, now, num * sizeof( short ) ) != 0 ) ) {
			idStr lost, gained;
			for ( int k = 0; k < e.num; k++ ) {
				bool still = false;
				for ( int j = 0; j < num; j++ ) {
					still |= ( now[j] == e.lights[k] );
				}
				if ( !still ) {
					// what that light is doing this frame (n3ds_hwLightLog keeps it)
					const int idx = e.lights[k];
					char state = '-';
					int surfaces = 0;
					if ( idx >= 0 && idx < N3DS_LIGHTLOG_MAX && lightLog[idx].frame == c3dLightLogFrame ) {
						state = lightLog[idx].state;
						surfaces = lightLog[idx].surfaces;
					}
					lost += va( " %d[%c %d]", idx, state, surfaces );
				}
			}
			for ( int j = 0; j < num; j++ ) {
				bool before = false;
				int light = -1;
				for ( int k = 0; k < e.num; k++ ) {
					before |= ( e.lights[k] == now[j] );
				}
				if ( !before ) {
					for ( int l = 0; l < 64; l++ ) {
						if ( hwFrameLightDef[l] == now[j] ) {
							light = l;
							break;
						}
					}
					gained += va( " %d(%s)", now[j], light >= 0 && hwFrameLightName[light] != NULL ? hwFrameLightName[light] : "?" );
				}
			}
			c3dSurfLogLines++;
			common->Printf( "SURFLOG f%d: %s (%d tris): %d -> %d lights, lost%s, gained%s\n", c3dSurfLogFrame,
				surf->material != NULL ? surf->material->GetName() : "?", surf->numIndexes / 3, e.num, num,
				lost.Length() > 0 ? lost.c_str() : " -", gained.Length() > 0 ? gained.c_str() : " -" );
		}
		e.key = key;
		e.frame = c3dSurfLogFrame;
		e.num = (byte)num;
		memcpy( e.lights, now, num * sizeof( short ) );
		return;
	}
}
static int				c3dLightLogLines = 0;

static void N3DS_LightLogEntry( const viewLight_t * vLight, char state, int surfaces, int unmatched, float strength, float radius ) {
	const int limit = n3ds_hwLightLog.GetInteger();
	if ( limit <= 0 || vLight->lightDef == NULL ) {
		return;
	}
	const int idx = vLight->lightDef->index;
	if ( idx < 0 || idx >= N3DS_LIGHTLOG_MAX ) {
		return;
	}
	n3dsLightLog_t & e = lightLog[idx];
	const bool wasThere = ( e.frame == c3dLightLogFrame - 1 );
	bool changed = !wasThere || e.state != state || ( ( e.surfaces == 0 ) != ( surfaces == 0 ) ) ||
		abs( e.surfaces - surfaces ) * 2 > Max( (int)e.surfaces, surfaces ) + 1 || unmatched != e.unmatched ||
		idMath::Fabs( e.strength - strength ) > 0.25f * Max( e.strength, strength ) + 0.01f;
	if ( changed && c3dLightLogLines < limit ) {
		c3dLightLogLines++;
		const idVec3 & o = vLight->globalLightOrigin;
		if ( !wasThere ) {
			common->Printf( "LIGHTLOG f%d: light %d (%s) at %.0f %.0f %.0f appears: %c, %d surfaces, %d unmatched, strength %.2f, radius %.0f\n",
				c3dLightLogFrame, idx, vLight->lightShader != NULL ? vLight->lightShader->GetName() : "?", o.x, o.y, o.z,
				state, surfaces, unmatched, strength, radius );
		} else {
			common->Printf( "LIGHTLOG f%d: light %d (%s): %c -> %c, surfaces %d -> %d, unmatched %d -> %d, strength %.2f -> %.2f, radius %.0f\n",
				c3dLightLogFrame, idx, vLight->lightShader != NULL ? vLight->lightShader->GetName() : "?",
				e.state, state, e.surfaces, surfaces, e.unmatched, unmatched, e.strength, strength, radius );
		}
	}
	e.frame = c3dLightLogFrame;
	e.state = state;
	e.surfaces = (short)surfaces;
	e.unmatched = (short)unmatched;
	e.strength = strength;
	e.radius = radius;
}

static float N3DS_LightStrength( const n3dsHWLight_t & l ) {
	return l.color[0] + l.color[1] + l.color[2];
}

// per view surface: its lights and the ambient light sum. The PICA has 8 light slots; a
// surface with more is drawn again, adding the next 8 (as the PC adds one pass per light)
static const int N3DS_MAX_SURF_LIGHTS = 24;
struct n3dsSurfLights_t {
	byte	lights[N3DS_MAX_SURF_LIGHTS];
	byte	numLights;
	float	ambient[3];
};
static n3dsSurfLights_t *	hwSurfLights = NULL;
static int					hwSurfLightsAlloc = 0;
static int *				hwHash = NULL;
static int					hwHashSize = 0;
static int					c3dStatHWLit = 0;
static int					c3dStatHWLightsPerSurf = 0;
static int					c3dStatHWDropped = 0;		// lights beyond N3DS_MAX_SURF_LIGHTS on a surface
static int					c3dStatHWExtraPasses = 0;	// surfaces drawn again for lights beyond 8
static int					c3dStatHWSlotLoads = 0;
static int					c3dStatBump = 0;			// lit surfaces drawn with a normal map
static int					c3dStatSpec = 0;			// ... with a specular map
static int					c3dStatBumpWaiting = 0;		// normal map not on the GPU yet

// A drawn surface and its light interactions are the same model surface: same frontEndGeo
// (the srfTriangles_t; its pointer is only compared here, never read) in the same space.
// The vertex cache handle is NOT an identity: surfaces whose geometry went into one
// vertex buffer share it, and matching by it gave one surface the lights of another -
// which one depended on the order of the drawn surfaces, so the lighting of whole walls
// changed while the camera turned.
static inline uint64 N3DS_SurfGeo( const drawSurf_t * surf ) {
	return ( surf->frontEndGeo != NULL ) ? (uint64)(UINT_PTR)surf->frontEndGeo : (uint64)surf->ambientCache;
}

static inline unsigned int N3DS_SurfKey( const drawSurf_t * surf ) {
	const uint64 a = N3DS_SurfGeo( surf );
	const uint64 b = (uint64)(UINT_PTR)surf->space;
	uint64 h = a * 0x9E3779B97F4A7C15ULL ^ ( b + ( a >> 29 ) );
	h ^= h >> 31;
	return (unsigned int)h;
}

static void N3DS_EyePoint( const float m[16], const idVec3 & p, float out[3] ) {
	for ( int i = 0; i < 3; i++ ) {
		out[i] = p.x * m[0 * 4 + i] + p.y * m[1 * 4 + i] + p.z * m[2 * 4 + i] + m[3 * 4 + i];
	}
}

static void N3DS_EyeDir( const float m[16], const idVec3 & d, float out[3] ) {
	for ( int i = 0; i < 3; i++ ) {
		out[i] = d.x * m[0 * 4 + i] + d.y * m[1 * 4 + i] + d.z * m[2 * 4 + i];
	}
}

// direction through the apex where s/q = sq and t/q = tq
static idVec3 N3DS_FrustumDir( const idPlane proj[4], float sq, float tq ) {
	const idVec3 s = proj[0].Normal() - sq * proj[2].Normal();
	const idVec3 t = proj[1].Normal() - tq * proj[2].Normal();
	idVec3 d = s.Cross( t );
	if ( d * proj[2].Normal() < 0.0f ) {
		d = -d;
	}
	d.Normalize();
	return d;
}

static bool N3DS_SetupHWLight( const viewDef_t * viewDef, const viewLight_t * vLight, float colorScale, n3dsHWLight_t & out ) {
	const idMaterial * lightShader = vLight->lightShader;
	const float * lightRegs = vLight->shaderRegisters;
	// colour of the first stage that passes its condition
	const float lightScale = r_lightScale.GetFloat();
	bool found = false;
	for ( int i = 0; i < lightShader->GetNumStages(); i++ ) {
		const shaderStage_t * stage = lightShader->GetStage( i );
		if ( !lightRegs[ stage->conditionRegister ] ) {
			continue;
		}
		for ( int c = 0; c < 3; c++ ) {
			out.color[c] = idMath::ClampFloat( 0.0f, 1.0f, lightScale * lightRegs[ stage->color.registers[c] ] / colorScale );
		}
		found = true;
		break;
	}
	if ( !found || ( out.color[0] <= 0.0f && out.color[1] <= 0.0f && out.color[2] <= 0.0f ) ) {
		return false;
	}
	out.ambient = lightShader->IsAmbientLight();
	const float * worldToEye = viewDef->worldSpace.modelViewMatrix;
	N3DS_EyePoint( worldToEye, vLight->globalLightOrigin, out.eyePos );

	const idPlane * proj = vLight->lightProject;
	const float qLen = proj[2].Normal().Length();
	if ( qLen < 1e-6f ) {
		// point light: s, t and falloff are 0.5 + 0.5 * x / radius along the light's
		// box axes. The PICA attenuation is a sphere: it has to reach 0 inside the box
		// (the frontend only gives the light to surfaces that touch the box, so light
		// left at the box faces shows as hard edges between surfaces), so its radius is
		// the smallest half-extent, and it is centred on the box.
		float r = 1e9f;
		const int planes[3] = { 0, 1, 3 };
		for ( int k = 0; k < 3; k++ ) {
			const float len = proj[ planes[k] ].Normal().Length();
			if ( len > 1e-6f ) {
				r = Min( r, 0.5f / len );
			}
		}
		if ( r > 1e8f ) {
			r = 300.0f;
		}
		r *= n3ds_hwLightRadius.GetFloat();
		out.invRadius = 1.0f / Max( r, 1.0f );
		out.attnBias = 0.0f;
		// box centre: s = t = falloff = 0.5
		idMat3 m( proj[0].Normal(), proj[1].Normal(), proj[3].Normal() );
		if ( m.InverseSelf() ) {
			const idVec3 rhs( 0.5f - proj[0][3], 0.5f - proj[1][3], 0.5f - proj[3][3] );
			const idVec3 centre( m[0] * rhs, m[1] * rhs, m[2] * rhs );		// rows of the inverse (idMat3 * vec is the transpose)
			N3DS_EyePoint( worldToEye, centre, out.eyePos );
		}
		out.spot = false;
		out.cosOuter = 2.0f;
		return true;
	}

	// projected light: axis where s/q = t/q = 0.5, cone to the frustum edges
	const idVec3 axis = N3DS_FrustumDir( proj, 0.5f, 0.5f );
	const idVec3 edgeS = N3DS_FrustumDir( proj, 1.0f, 0.5f );
	const idVec3 edgeT = N3DS_FrustumDir( proj, 0.5f, 1.0f );
	const float cosOuter = Min( axis * edgeS, axis * edgeT );
	out.spot = true;
	out.cosOuter = idMath::ClampFloat( 0.0f, 0.999f, cosOuter );
	N3DS_EyeDir( worldToEye, n3ds_hwSpotFlip.GetBool() ? -axis : axis, out.spotDir );
	// falloff plane: 0 at the start, 1 at the end of the light
	const idPlane & f = proj[3];
	const float along = f.Normal() * axis;
	// Along the axis the falloff is atOrigin + along * distance: that is the table input,
	// exactly (the old 1 / range with no bias stretched it from the origin, wrong for lights
	// whose start is far from their origin)
	if ( along > 1e-6f ) {
		out.invRadius = along;
		out.attnBias = f.Distance( vLight->globalLightOrigin );
	} else {
		out.invRadius = 1.0f / 512.0f;
		out.attnBias = 0.0f;
	}
	return true;
}

static void N3DS_ModelViewRows( const float m[16], float rows[12] ) {
	for ( int i = 0; i < 3; i++ ) {
		rows[i * 4 + 0] = m[0 * 4 + i];
		rows[i * 4 + 1] = m[1 * 4 + i];
		rows[i * 4 + 2] = m[2 * 4 + i];
		rows[i * 4 + 3] = m[3 * 4 + i];
	}
}

static void N3DS_DrawViewHW( const viewDef_t * viewDef ) {
	const int numSurfs = viewDef->numDrawSurfs;
	const int combinerScale = ( n3ds_hwLightScale.GetInteger() >= 4 ) ? 4 : 2;
	const float ambientLevel = n3ds_ambient.GetFloat();
	const bool useBump = n3ds_bump.GetBool();
	const bool useSpecular = n3ds_specular.GetBool();
	N3DSGPU_HWSpecularPower( n3ds_specularPower.GetFloat() );

	// ---- per-surface light lists ----
	if ( numSurfs > hwSurfLightsAlloc ) {
		Mem_Free( hwSurfLights );
		hwSurfLightsAlloc = numSurfs + 256;
		hwSurfLights = (n3dsSurfLights_t *)Mem_Alloc( hwSurfLightsAlloc * sizeof( n3dsSurfLights_t ), TAG_RENDER );
	}
	int hashSize = 64;
	while ( hashSize < numSurfs * 2 ) {
		hashSize <<= 1;
	}
	if ( hashSize > hwHashSize ) {
		Mem_Free( hwHash );
		hwHashSize = hashSize;
		hwHash = (int *)Mem_Alloc( hwHashSize * sizeof( int ), TAG_RENDER );
	}
	hashSize = hwHashSize;
	memset( hwHash, -1, hashSize * sizeof( int ) );
	for ( int i = 0; i < numSurfs; i++ ) {
		const drawSurf_t * surf = viewDef->drawSurfs[i];
		hwSurfLights[i].numLights = 0;
		hwSurfLights[i].ambient[0] = hwSurfLights[i].ambient[1] = hwSurfLights[i].ambient[2] = 0.0f;
		if ( N3DS_SkipSurface( surf ) ) {
			continue;
		}
		unsigned int h = N3DS_SurfKey( surf ) & ( hashSize - 1 );
		while ( hwHash[h] >= 0 ) {
			h = ( h + 1 ) & ( hashSize - 1 );
		}
		hwHash[h] = i;
	}

	const int maxSurfLights = idMath::ClampInt( 1, N3DS_MAX_SURF_LIGHTS, n3ds_hwMaxLights.GetInteger() );
	N3DSGPU_HWQuatMode( n3ds_hwQuat.GetInteger() );
	N3DSGPU_HWEnvDiag( n3ds_hwKeepEnv.GetBool() ? 1 : 0, n3ds_hwEnvDirty.GetInteger() );
	N3DSGPU_HWFlatUnit1( n3ds_hwFlatUnit1.GetBool() ? 1 : 0 );
	const bool surfLog = n3ds_hwSurfLog.GetInteger() > 0 && !viewDef->isSubview;
	if ( surfLog ) {
		c3dSurfLogFrame++;
	}
	const bool reloadLights = n3ds_hwReloadLights.GetBool();
	int numLights = 0;
	const bool lightLog = n3ds_hwLightLog.GetInteger() > 0 && !viewDef->isSubview;
	if ( lightLog ) {
		c3dLightLogFrame++;
	}
	for ( const viewLight_t * vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next ) {
		const idMaterial * lightShader = vLight->lightShader;
		if ( lightShader == NULL || lightShader->IsFogLight() || lightShader->IsBlendLight() ) {
			if ( lightLog ) {
				N3DS_LightLogEntry( vLight, 'F', 0, 0, 0.0f, 0.0f );
			}
			continue;
		}
		if ( vLight->localInteractions == NULL && vLight->globalInteractions == NULL ) {
			if ( lightLog ) {
				N3DS_LightLogEntry( vLight, 'N', 0, 0, 0.0f, 0.0f );
			}
			continue;
		}
		if ( numLights >= N3DS_MAX_FRAME_LIGHTS ) {
			if ( lightLog ) {
				N3DS_LightLogEntry( vLight, 'O', 0, 0, 0.0f, 0.0f );
			}
			continue;
		}
		n3dsHWLight_t & light = hwFrameLights[numLights];
		hwFrameLightDef[numLights] = ( vLight->lightDef != NULL ) ? vLight->lightDef->index : -1;
		hwFrameLightName[numLights] = lightShader->GetName();
		if ( !N3DS_SetupHWLight( viewDef, vLight, (float)combinerScale, light ) ) {
			if ( lightLog ) {
				N3DS_LightLogEntry( vLight, 'X', 0, 0, 0.0f, 0.0f );
			}
			continue;
		}
		c3dStatLights++;
		int logMatched = 0, logUnmatched = 0;
		for ( int list = 0; list < 2; list++ ) {
			for ( const drawSurf_t * inter = ( list == 0 ) ? vLight->localInteractions : vLight->globalInteractions;
					inter != NULL; inter = inter->nextOnLight ) {
				unsigned int h = N3DS_SurfKey( inter ) & ( hashSize - 1 );
				int found = -1;
				while ( hwHash[h] >= 0 ) {
					const drawSurf_t * s = viewDef->drawSurfs[ hwHash[h] ];
					if ( N3DS_SurfGeo( s ) == N3DS_SurfGeo( inter ) && s->space == inter->space ) {
						found = hwHash[h];
						break;
					}
					h = ( h + 1 ) & ( hashSize - 1 );
				}
				if ( found < 0 ) {
					logUnmatched++;
					continue;
				}
				logMatched++;
				n3dsSurfLights_t & sl = hwSurfLights[found];
				if ( light.ambient ) {
					for ( int c = 0; c < 3; c++ ) {
						sl.ambient[c] += light.color[c];
					}
					continue;
				}
				bool dup = false;
				for ( int k = 0; k < sl.numLights; k++ ) {
					if ( sl.lights[k] == numLights ) {
						dup = true;
						break;
					}
				}
				if ( dup ) {
					continue;
				}
				if ( sl.numLights < maxSurfLights ) {
					sl.lights[ sl.numLights++ ] = (byte)numLights;
				} else {
					// full: keep the brightest
					int weakest = 0;
					for ( int k = 1; k < sl.numLights; k++ ) {
						if ( N3DS_LightStrength( hwFrameLights[ sl.lights[k] ] ) < N3DS_LightStrength( hwFrameLights[ sl.lights[weakest] ] ) ) {
							weakest = k;
						}
					}
					if ( N3DS_LightStrength( light ) > N3DS_LightStrength( hwFrameLights[ sl.lights[weakest] ] ) ) {
						sl.lights[weakest] = (byte)numLights;
					}
					c3dStatHWDropped++;
				}
			}
		}
		if ( lightLog ) {
			N3DS_LightLogEntry( vLight, light.ambient ? 'A' : 'L', logMatched, logUnmatched, N3DS_LightStrength( light ),
				light.invRadius > 0.0f ? ( 1.0f - light.attnBias ) / light.invRadius : 0.0f );
		}
		numLights++;
	}

	// ---- draw ----
	n3dsBuffers_t buffers;
	memset( &buffers, 0, sizeof( buffers ) );
	buffers.validate = n3ds_c3dValidate.GetBool();
	c3dDrawRecord_t record;
	int slotLight[8];
	for ( int k = 0; k < 8; k++ ) {
		slotLight[k] = -1;
	}
	const viewEntity_t * lastSpace = NULL;
	bool spaceOk = true;
	int draws = 0;
	const int maxDraws = n3ds_c3dMaxDraws.GetInteger();
	const int test = n3ds_c3dTest.GetInteger();

	for ( int i = 0; i < numSurfs; i++ ) {
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
			if ( spaceOk ) {
				float rows[12];
				N3DS_ModelViewRows( surf->space->modelViewMatrix, rows );
				N3DSGPU_SetModelView( rows );
			} else if ( c3dBadMatrix++ < 8 ) {
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

		const bool lit = images.lit || images.color == NULL;
		void * normalTex = NULL;
		void * specTex = NULL;
		idImage * recBump = NULL;
		idImage * recSpec = NULL;
		if ( lit && images.lit && hwSurfLights[i].numLights > 0 ) {
			// normal and specular maps only matter where a light reaches; normal maps only
			// from the texture pack (HILO8)
			if ( useBump && alphaSource != 2 ) {
				idImage * bumpImage = N3DS_StageImage( material, surf->shaderRegisters, SL_BUMP );
				recBump = bumpImage;
				if ( bumpImage != NULL && N3DS_TexIsNormalMap( bumpImage ) ) {
					normalTex = N3DS_TexForImage( bumpImage );
					if ( normalTex != NULL ) {
						c3dStatBump++;
					} else {
						c3dStatBumpWaiting++;
					}
				}
			}
			if ( useSpecular ) {
				idImage * specImage = N3DS_StageImage( material, surf->shaderRegisters, SL_SPECULAR );
				recSpec = specImage;
				if ( specImage != NULL ) {
					specTex = N3DS_TexForImage( specImage );
					c3dStatSpec += ( specTex != NULL ) ? 1 : 0;
				}
			}
		}
		// lights in passes of 8: the brightest first, deterministic order (the set chosen for
		// a surface must not depend on which other lights happen to be in view)
		n3dsSurfLights_t & sl = hwSurfLights[i];
		if ( lit ) {
			for ( int a = 1; a < sl.numLights; a++ ) {
				const byte l = sl.lights[a];
				const float sa = N3DS_LightStrength( hwFrameLights[l] );
				int b = a - 1;
				while ( b >= 0 && ( N3DS_LightStrength( hwFrameLights[ sl.lights[b] ] ) < sa ||
						( N3DS_LightStrength( hwFrameLights[ sl.lights[b] ] ) == sa && sl.lights[b] > l ) ) ) {
					sl.lights[b + 1] = sl.lights[b];
					b--;
				}
				sl.lights[b + 1] = l;
			}
			c3dStatHWLit++;
			c3dStatHWLightsPerSurf += sl.numLights;
			if ( surfLog ) {
				N3DS_SurfLogEntry( surf, sl.lights, sl.numLights );
			}
		}
		const int numPasses = ( lit && sl.numLights > 8 ) ? ( sl.numLights + 7 ) / 8 : 1;
		for ( int pass = 0; pass < numPasses; pass++ ) {
		if ( lit ) {
			// slots: keep lights already loaded, load the rest into free slots
			if ( reloadLights ) {
				for ( int k = 0; k < 8; k++ ) {
					slotLight[k] = -1;
				}
				N3DSGPU_HWForgetLights();
			}
			const int first = pass * 8;
			const int count = Min( 8, sl.numLights - first );
			int used = 0;
			int need[8];
			int numNeed = 0;
			for ( int k = 0; k < count; k++ ) {
				const int l = sl.lights[first + k];
				int slot = -1;
				for ( int s = 0; s < 8; s++ ) {
					if ( slotLight[s] == l ) {
						slot = s;
						break;
					}
				}
				if ( slot >= 0 ) {
					used |= 1 << slot;
				} else {
					need[numNeed++] = l;
				}
			}
			for ( int k = 0; k < numNeed; k++ ) {
				const int l = need[k];
				int slot = l & 7;							// the light's usual slot, if free here
				if ( used & ( 1 << slot ) ) {
					for ( slot = 0; slot < 8 && ( used & ( 1 << slot ) ); slot++ ) {
					}
				}
				const n3dsHWLight_t & hl = hwFrameLights[l];
				N3DSGPU_HWSetLight( slot, hl.eyePos, hl.color, hl.invRadius, hl.attnBias, hl.spot ? hl.spotDir : NULL, hl.cosOuter );
				slotLight[slot] = l;
				used |= 1 << slot;
				c3dStatHWSlotLoads++;
			}
			if ( used == 0 ) {
				// No light reaches this surface (ambient only). The PICA has no "0 lights":
				// its light count register holds count - 1, so with every light disabled it
				// still lit the surface with the light in the first slot of the permutation -
				// whatever the previously drawn surface had loaded there. Draw order changes as
				// the camera turns, so such surfaces (whole walls, rocks) flickered between
				// dark and lit by a stray lamp. A black light in slot 7 is that one light now.
				const float black[3] = { 0.0f, 0.0f, 0.0f };
				const float origin[3] = { 0.0f, 0.0f, 0.0f };
				if ( slotLight[7] != -2 ) {
					N3DSGPU_HWSetLight( 7, origin, black, 1.0f, 0.0f, NULL, 0.0f );
					slotLight[7] = -2;
					c3dStatHWSlotLoads++;
				}
				used = 1 << 7;
			}
			N3DSGPU_HWEnableLights( used );
			if ( pass == 0 ) {
				const float base = ambientLevel / combinerScale;
				N3DSGPU_HWAmbient( idMath::ClampFloat( 0.0f, 1.0f, base + sl.ambient[0] ),
					idMath::ClampFloat( 0.0f, 1.0f, base + sl.ambient[1] ), idMath::ClampFloat( 0.0f, 1.0f, base + sl.ambient[2] ) );
			} else {
				// added on top: these lights only, no ambient again
				N3DSGPU_HWAmbient( 0.0f, 0.0f, 0.0f );
				c3dStatHWExtraPasses++;
			}
		}

		// diffuse stage colour (lit) / full brightness (unlit)
		float r = 1.0f, g = 1.0f, b = 1.0f;
		if ( lit && surf->shaderRegisters != NULL ) {
			const shaderStage_t * diffuseStage = N3DS_DiffuseStage( material, surf->shaderRegisters );
			if ( diffuseStage != NULL ) {
				r = idMath::ClampFloat( 0.0f, 1.0f, surf->shaderRegisters[ diffuseStage->color.registers[0] ] );
				g = idMath::ClampFloat( 0.0f, 1.0f, surf->shaderRegisters[ diffuseStage->color.registers[1] ] );
				b = idMath::ClampFloat( 0.0f, 1.0f, surf->shaderRegisters[ diffuseStage->color.registers[2] ] );
			}
		}
		if ( !lit && n3ds_ambientStages.GetBool() ) {
			r = g = b = 0.0f;		// unlit: black with depth, the ambient stages come later
		}
		if ( colorTex != NULL ) {
			c3dStatTextured++;
			N3DSGPU_SetColor( r, g, b, 1.0f );
		} else {
			N3DS_MaterialColor( material, lit ? 1.0f : r );
		}
		N3DSGPU_SetHWPass( colorTex, maskTex, alphaSource, lit ? 1 : 0, combinerScale, normalTex, specTex );
		record.flags |= lit ? C3D_REC_LIT : 0;
		record.images[0] = images.color;
		record.images[1] = recBump;
		record.images[2] = recSpec;
		record.gpuTex[0] = colorTex;
		record.gpuTex[1] = normalTex;
		record.gpuTex[2] = specTex;
		record.numLights = lit ? (short)hwSurfLights[i].numLights : 0;
		record.alphaSource = (short)alphaSource;
		if ( pass > 0 ) {
			N3DSGPU_HWAddPass( 1 );
		}
		N3DS_Draw( surf, verts, indexes, record, test );
		if ( pass > 0 && pass == numPasses - 1 ) {
			N3DSGPU_HWAddPass( 0 );
		}
		}
	}
}


/*
================================================================================================

	Ambient stages and GUIs (as RB_DrawShaderPasses)

	Every SL_AMBIENT stage of every surface, in sort order, with the stage's blend,
	depth and alpha test state, stage colour x vertex colour, the stage's texture
	matrix and polygon offset. In 3D views this runs after the fill/light passes
	(unlit opaque materials were filled black, so their stages land on black like
	on the PC); fullscreen GUI views (HUD, menus, loading screens) are only this,
	without depth. SWF GUIs (GLS_OVERRIDE) add their colour transform (color2) and
	premultiply alpha like gui.pixel. Not yet: TG_SCREEN / cube texgens,
	cinematics, new-style (program) stages, scissor.

================================================================================================
*/

static int c3dStatStages = 0;
static int c3dStatStagesSkipped = 0;
static int c3dStatVideoStages = 0;

// GLS source / destination blend bits -> GL-order factor numbers (see N3DSGPU_SetStageState)
static int N3DS_SrcBlend( uint64 bits ) {
	static const int map[8] = { 1, 0, 4, 5, 6, 7, 8, 9 };
	return map[ bits & GLS_SRCBLEND_BITS ];
}
static int N3DS_DstBlend( uint64 bits ) {
	static const int map[8] = { 0, 1, 2, 3, 6, 7, 8, 9 };
	return map[ ( bits & GLS_DSTBLEND_BITS ) >> 3 ];
}

static void N3DS_DrawAmbientStages( const viewDef_t * viewDef, bool gui ) {
	if ( !n3ds_ambientStages.GetBool() || !N3DSGPU_UseProgram( 2 ) ) {
		return;
	}
	n3dsBuffers_t buffers;
	memset( &buffers, 0, sizeof( buffers ) );
	buffers.validate = n3ds_c3dValidate.GetBool();
	c3dDrawRecord_t record;
	const viewEntity_t * lastSpace = NULL;
	bool spaceOk = true;
	int draws = 0;
	const int test = n3ds_c3dTest.GetInteger();

	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t * surf = viewDef->drawSurfs[i];
		const idMaterial * shader = surf->material;
		if ( shader == NULL || !shader->HasAmbient() || shader->IsPortalSky() || surf->numIndexes == 0 ) {
			continue;
		}
		if ( !gui && shader->GetSort() >= SS_POST_PROCESS ) {
			break;
		}
		const float * regs = surf->shaderRegisters;
		if ( regs == NULL ) {
			continue;
		}
		const unsigned short * indexes = NULL;
		const byte * verts = NULL;

		uint64 surfGLState = surf->extraGLState;
		int bias = 0;
		if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			bias = (int)( n3ds_polygonOffset.GetFloat() * shader->GetPolygonOffset() );
			surfGLState = GLS_POLYGON_OFFSET;
		}

		for ( int stage = 0; stage < shader->GetNumStages(); stage++ ) {
			const shaderStage_t * pStage = shader->GetStage( stage );
			if ( regs[ pStage->conditionRegister ] == 0 || pStage->lighting != SL_AMBIENT ) {
				continue;
			}
			uint64 stageGLState = surfGLState;
			if ( ( surfGLState & GLS_OVERRIDE ) == 0 ) {
				stageGLState |= pStage->drawStateBits;
			}
			if ( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
				continue;
			}
			if ( pStage->newStage != NULL || pStage->texture.dynamic ) {
				c3dStatStagesSkipped++;
				continue;
			}
			if ( pStage->texture.texgen != TG_EXPLICIT ) {
				c3dStatStagesSkipped++;
				continue;
			}
			float color[4];
			color[0] = regs[ pStage->color.registers[0] ];
			color[1] = regs[ pStage->color.registers[1] ];
			color[2] = regs[ pStage->color.registers[2] ];
			color[3] = regs[ pStage->color.registers[3] ];
			const uint64 blend = stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
			if ( blend == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) && color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
				continue;
			}
			if ( blend == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) && color[3] <= 0 ) {
				continue;
			}
			void * tex = NULL;
			if ( pStage->texture.cinematic != NULL ) {
				// a video (.n3v): the frame at the view's time, as the PC backend times it
				tex = pStage->texture.cinematic->N3DS_GpuTexture( viewDef->renderView.time[0] +
					idMath::Ftoi( 1000.0f * viewDef->renderView.shaderParms[11] ) );
				if ( tex == NULL ) {
					c3dStatStagesSkipped++;
					continue;
				}
				c3dStatVideoStages++;
			} else if ( pStage->texture.image != NULL ) {
				tex = N3DS_TexForImage( pStage->texture.image );
				if ( tex == NULL ) {
					c3dStatStagesSkipped++;		// not on the GPU (yet): a white quad would be worse
					continue;
				}
			}

			if ( verts == NULL ) {
				verts = N3DS_SurfaceBuffers( surf, buffers, &indexes, &record );
				if ( verts == NULL ) {
					break;
				}
				if ( surf->space != lastSpace ) {
					spaceOk = N3DS_SetMVP( surf->space->mvp );
					lastSpace = surf->space;
				}
			}
			if ( !spaceOk ) {
				break;
			}
			if ( !N3DS_CommandRoom( draws ) ) {
				N3DSGPU_EndStages();
				return;
			}

			stageVertexColor_t svc = pStage->vertexColor;
			const bool guiSurface = surf->space->isGuiSurface;
			const bool swf = guiSurface && ( stageGLState & GLS_OVERRIDE ) != 0;
			if ( guiSurface ) {
				svc = SVC_MODULATE;
			}
			float vcMod = 0.0f, vcAdd = 1.0f;
			if ( svc == SVC_MODULATE ) {
				vcMod = 1.0f;
				vcAdd = 0.0f;
			} else if ( svc == SVC_INVERSE_MODULATE ) {
				vcMod = -1.0f;
				vcAdd = 1.0f;
			}
			float swfAdd[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			if ( swf ) {
				// gui.vertex: colour transform add term = color2 * 2 - 1 (one per surface here)
				const idDrawVert * v = (const idDrawVert *)verts + indexes[0];
				for ( int k = 0; k < 4; k++ ) {
					swfAdd[k] = v->color2[k] * ( 2.0f / 255.0f ) - 1.0f;
				}
				color[0] = color[1] = color[2] = color[3] = 1.0f;		// gui.vertex ignores the stage colour
			}
			float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
			if ( pStage->texture.hasMatrix ) {
				float m[16];
				N3DS_GetShaderTextureMatrix( regs, &pStage->texture, m );
				texS[0] = m[0 * 4 + 0]; texS[1] = m[1 * 4 + 0]; texS[2] = m[2 * 4 + 0]; texS[3] = m[3 * 4 + 0];
				texT[0] = m[0 * 4 + 1]; texT[1] = m[1 * 4 + 1]; texT[2] = m[2 * 4 + 1]; texT[3] = m[3 * 4 + 1];
			}

			int depthFunc = (int)( ( stageGLState & GLS_DEPTHFUNC_BITS ) >> 13 );
			int writeMask = 0;
			if ( !( stageGLState & GLS_REDMASK ) ) {
				writeMask |= 1;
			}
			if ( !( stageGLState & GLS_GREENMASK ) ) {
				writeMask |= 2;
			}
			if ( !( stageGLState & GLS_BLUEMASK ) ) {
				writeMask |= 4;
			}
			if ( !( stageGLState & GLS_ALPHAMASK ) ) {
				writeMask |= 8;
			}
			if ( !( stageGLState & GLS_DEPTHMASK ) ) {
				writeMask |= 0x10;
			}
			if ( gui ) {
				depthFunc = 1;			// fullscreen GUIs: no depth
				writeMask &= ~0x10;
			}
			int alphaRef = -1;
			if ( pStage->hasAlphaTest ) {
				alphaRef = idMath::ClampInt( 0, 255, (int)( regs[ pStage->alphaTestRegister ] * 255.0f ) );
			}
			int stageBias = bias;
			if ( pStage->privatePolygonOffset != 0.0f ) {
				stageBias = (int)( n3ds_polygonOffset.GetFloat() * pStage->privatePolygonOffset );
			}
			const int blendOp = (int)( ( stageGLState & GLS_BLENDOP_BITS ) >> 18 );

			N3DSGPU_SetStageState( N3DS_SrcBlend( stageGLState ), N3DS_DstBlend( stageGLState ), blendOp, depthFunc, writeMask,
				alphaRef, idMath::ClampInt( 0, 255, stageBias ) );
			N3DSGPU_SetStageUniforms( color, vcMod, vcAdd, texS, texT );
			// SWF masks: stencil test / increment / decrement from the GL state
			{
				const int sFunc = (int)( ( stageGLState & GLS_STENCIL_FUNC_BITS ) >> 36 );
				const int sFail = (int)( ( stageGLState & GLS_STENCIL_OP_FAIL_BITS ) >> 39 );
				const int sZFail = (int)( ( stageGLState & GLS_STENCIL_OP_ZFAIL_BITS ) >> 42 );
				const int sPass = (int)( ( stageGLState & GLS_STENCIL_OP_PASS_BITS ) >> 45 );
				if ( ( sFunc == 0 && sFail == 0 && sZFail == 0 && sPass == 0 ) || !n3ds_guiStencil.GetBool() ) {
					N3DSGPU_SetStencil( -1, 0, 0, 0, 0, 0 );
				} else {
					N3DSGPU_SetStencil( sFunc, (int)( ( stageGLState & GLS_STENCIL_FUNC_REF_BITS ) >> GLS_STENCIL_FUNC_REF_SHIFT ),
						(int)( ( stageGLState & GLS_STENCIL_FUNC_MASK_BITS ) >> GLS_STENCIL_FUNC_MASK_SHIFT ), sFail, sZFail, sPass );
				}
			}
			const bool alphaTex = tex != NULL && N3DS_TexIsAlphaOnly( pStage->texture.image );
			N3DSGPU_SetStageTexEnv( tex, swf ? 1 : 0, swfAdd, alphaTex ? 1 : 0 );
			// a video stage has no image (texture.cinematic instead)
			record.texName = ( tex == NULL ) ? NULL : ( pStage->texture.image != NULL ? pStage->texture.image->GetName() :
				( pStage->texture.cinematic != NULL ? "(video)" : "?" ) );
			record.flags = C3D_REC_STAGE | ( alphaTex ? C3D_REC_A8 : 0 ) | ( swf ? C3D_REC_SWF : 0 ) | ( alphaRef >= 0 ? C3D_REC_ALPHATEST : 0 ) |
				( gui ? C3D_REC_GUIVIEW : 0 ) | ( ( n3ds_guiStencil.GetBool() && ( stageGLState & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_OP_FAIL_BITS | GLS_STENCIL_OP_ZFAIL_BITS | GLS_STENCIL_OP_PASS_BITS ) ) != 0 ) ? C3D_REC_STENCIL : 0 );
			N3DS_Draw( surf, verts, indexes, record, test );
			draws++;
			c3dStatStages++;
		}
	}
	N3DSGPU_EndStages();
}

/*
========================
N3DS_LogWeaponSurfaces

Diagnostics: the view weapon (weaponDepthHack spaces) and what happens to each of
its surfaces, logged whenever that set changes (weapon switch, first frame).

Runs in the backend, so it must only read backend data: drawSurfs, materials and
spaces. viewEntity_t::entityDef belongs to the frontend, which may already be
changing or freeing it (a weapon switch deletes the old view weapon entity while
the backend still draws the previous frame); reading it here crashed.
========================
*/
static int c3dWeaponSignature = 0;
static int c3dWeaponLogs = 0;

static void N3DS_LogWeaponSurfaces( const viewDef_t * viewDef ) {
	if ( c3dWeaponLogs >= 12 ) {
		return;
	}
	int signature = 0x1234;
	int numSpaces = 0;
	for ( const viewEntity_t * v = viewDef->viewEntitys; v != NULL; v = v->next ) {
		if ( v->weaponDepthHack ) {
			numSpaces++;
		}
	}
	int numSurfs = 0;
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t * surf = viewDef->drawSurfs[i];
		if ( surf->space != NULL && surf->space->weaponDepthHack ) {
			numSurfs++;
			signature = signature * 31 + (int)(UINT_PTR)surf->material + surf->numIndexes;
		}
	}
	if ( signature == c3dWeaponSignature ) {
		return;
	}
	c3dWeaponSignature = signature;
	c3dWeaponLogs++;
	common->Printf( "citro3d weapon: %d weaponDepthHack entities, %d surfaces in the view\n", numSpaces, numSurfs );
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t * surf = viewDef->drawSurfs[i];
		if ( surf->space == NULL || !surf->space->weaponDepthHack ) {
			continue;
		}
		const idMaterial * m = surf->material;
		n3dsFillImages_t images;
		if ( m != NULL ) {
			N3DS_FillImages( m, surf->shaderRegisters, images );
		}
		idVertexBuffer vb;
		const bool hasVB = vertexCache.GetVertexBuffer( surf->ambientCache, &vb );
		common->Printf( "  surf %s: %d indexes, coverage %d, sort %.1f, %s, vb %s, image %s (%s)\n",
			m != NULL ? m->GetName() : "NULL", surf->numIndexes, m != NULL ? (int)m->Coverage() : -1,
			m != NULL ? m->GetSort() : 0.0f, N3DS_SkipSurface( surf ) ? "SKIPPED" : "drawn", hasVB ? "ok" : "MISSING",
			( m != NULL && images.color != NULL ) ? images.color->GetName() : "none",
			( m != NULL && images.color != NULL && images.color->n3dsTex != NULL ) ? "kept" : "no level" );
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

	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: waiting for GPU" );
	N3DSGPU_SetFullFlush( n3ds_c3dFullFlush.GetBool() ? 1 : 0 );
	N3DSGPU_BeginFrame( 0x000000FF );
	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: drawing" );
	N3DS_TexBeginFrame( has3D );
	// lighting path for this frame (the lit program may be missing: fall back)
	const bool useHW = ( n3ds_lighting.GetInteger() == 2 ) && n3ds_c3dTest.GetInteger() == 0 && N3DSGPU_UseProgram( 1 );
	if ( !useHW ) {
		N3DSGPU_UseProgram( 0 );
	}
	c3dRecordSet ^= 1;
	c3dNumRecords[c3dRecordSet] = 0;
	{
		const bool keys = n3ds_c3dFrameDump.GetInteger() > 0 && N3DSGPU_DumpKeysHeld();
		if ( keys && !c3dDumpKeysWere && c3dDumpLeft == 0 ) {
			c3dDumpLeft = n3ds_c3dFrameDump.GetInteger();
			common->Printf( "FRAMEDUMP: touch: writing the next %d frames\n", c3dDumpLeft );
		}
		c3dDumpKeysWere = keys;
	}
	if ( trace ) {
		common->Printf( "citro3d: 3D frame %d: GPU ready, previous frame %.2f ms\n", c3dFrame3D, N3DSGPU_DrawingTimeMs() );
	}
	const int drawsBefore = c3dStatDraws;
	for ( ; cmds != NULL; cmds = (const emptyCommand_t *)cmds->next ) {
		if ( cmds->commandId == RC_DRAW_VIEW_GUI ) {
			// fullscreen GUI: HUD, menus, loading screens
			const viewDef_t * viewDef = ( (const drawSurfsCommand_t *)cmds )->viewDef;
			if ( viewDef != NULL && n3ds_c3dTest.GetInteger() == 0 ) {
				N3DS_DrawAmbientStages( viewDef, true );
			}
			continue;
		}
		if ( cmds->commandId != RC_DRAW_VIEW_3D ) {
			continue;
		}
		const viewDef_t * viewDef = ( (const drawSurfsCommand_t *)cmds )->viewDef;
		if ( viewDef == NULL || viewDef->isSubview || viewDef->viewEntitys == NULL ) {
			continue;
		}
		N3DS_LogWeaponSurfaces( viewDef );
		N3DSGPU_UseProgram( useHW ? 1 : 0 );
		if ( useHW ) {
			N3DS_DrawViewHW( viewDef );
		} else {
			N3DS_DrawView( viewDef );
		}
		// after the draws: the reads the view asked for are queued first
		N3DS_TexPrefetch( viewDef->n3dsPrefetch, viewDef->n3dsNumPrefetch );
		if ( n3ds_c3dTest.GetInteger() == 0 ) {
			N3DS_DrawAmbientStages( viewDef, false );
		}
	}
	const float cmdUse = N3DSGPU_CmdBufUsage();
	if ( trace ) {
		common->Printf( "citro3d: 3D frame %d: %d draws, command buffer %.0f%%, submitting\n",
			c3dFrame3D, c3dStatDraws - drawsBefore, cmdUse * 100.0f );
	}
	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: submitting" );
	N3DSGPU_EndFrame();
	N3DS_STAGE( N3DS_STAGE_RENDER, "c3d: idle" );
	if ( c3dDumpLeft > 0 && has3D ) {
		// the frame just submitted is still in the GX queue (cleared at the next frame start)
		const char * path = va( "sdmc:/doom3/frame%d.bin", c3dDumpIndex );
		const int ok = N3DSGPU_HangDump( path, 0 );
		common->Printf( "FRAMEDUMP %d: 3D frame %d, %d draws, %s %s\n", c3dDumpIndex, c3dFrame3D,
			c3dNumRecords[c3dRecordSet], path, ok ? "written" : "NOT written" );
		for ( int i = 0; i < c3dNumRecords[c3dRecordSet]; i++ ) {
			N3DS_PrintRecord( i, c3dRecords[c3dRecordSet][i] );
		}
		c3dDumpIndex++;
		c3dDumpLeft--;
	}
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
		common->Printf( "PERF   citro3d: %.0f draws (%.0f textured, %.0f light draws for %.1f lights, light area %.1f screens) %.0f tris per frame, GPU %.1f ms, cmdbuf peak %.0f%%, bad draws %d\n",
			c3dStatDraws / f, c3dStatTextured / f, c3dStatLightDraws / f, c3dStatLights / f, c3dStatScissorPixels / ( f * 96000.0f ),
			c3dStatTris / f, c3dStatGpuMs / f, c3dStatCmdMax * 100.0f, c3dStatBad );
		common->Printf( "PERF   citro3d stages: %.0f ambient/GUI stage draws per frame (%.0f video), %.0f skipped (texgen, video without .n3v, program, not loaded)\n",
			c3dStatStages / f, c3dStatVideoStages / f, c3dStatStagesSkipped / f );
		c3dStatStages = c3dStatStagesSkipped = c3dStatVideoStages = 0;
		if ( c3dStatHWLit > 0 ) {
			common->Printf( "PERF   citro3d HW lights: %.0f lit surfaces, %.2f lights each, %.0f slot loads, %.0f lights over the limit dropped, %.0f extra passes for lights beyond 8, %.0f with normal map (%.0f waiting for it), %.0f with specular map per frame\n",
				c3dStatHWLit / f, c3dStatHWLightsPerSurf / (float)c3dStatHWLit, c3dStatHWSlotLoads / f, c3dStatHWDropped / f, c3dStatHWExtraPasses / f,
				c3dStatBump / f, c3dStatBumpWaiting / f, c3dStatSpec / f );
		}
		c3dStatHWLit = c3dStatHWLightsPerSurf = c3dStatHWSlotLoads = c3dStatHWDropped = c3dStatHWExtraPasses = 0;
		c3dStatBump = c3dStatBumpWaiting = c3dStatSpec = 0;
		idStr texLine;
		N3DS_TexStatsLine( texLine );
		common->Printf( "PERF   citro3d: %s\n", texLine.c_str() );
		c3dStatTextured = c3dStatLights = c3dStatLightDraws = c3dStatScissorPixels = 0;
		c3dStatStart = now;
		c3dStatFrames = c3dStatDraws = c3dStatTris = c3dStatBad = 0;
		c3dStatCmdMax = c3dStatGpuMs = 0.0f;
	}

	backEnd.pc.totalMicroSec = Sys_Microseconds() - start;
}
