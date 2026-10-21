/*
================================================================================================

	New 3DS GPU layer: citro3d setup, frame begin/end and indexed draws.
	See n3ds_gpu.h.

================================================================================================
*/
#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include <stdlib.h>

#include "n3ds_gpu.h"

// generated at build time from n3ds_vshader*.v.pica (picasso + cmake/bin2c.cmake)
extern const u8		n3ds_vshader_shbin[];
extern const u32	n3ds_vshader_shbin_size;
extern const u8		n3ds_vshader_lit_shbin[];
extern const u32	n3ds_vshader_lit_shbin_size;
extern const u8		n3ds_vshader_amb_shbin[];
extern const u32	n3ds_vshader_amb_shbin_size;

#define DISPLAY_TRANSFER_FLAGS \
	( GX_TRANSFER_FLIP_VERT( 0 ) | GX_TRANSFER_OUT_TILED( 0 ) | GX_TRANSFER_RAW_COPY( 0 ) | \
	  GX_TRANSFER_IN_FORMAT( GX_TRANSFER_FMT_RGBA8 ) | GX_TRANSFER_OUT_FORMAT( GX_TRANSFER_FMT_RGB8 ) | \
	  GX_TRANSFER_SCALING( GX_TRANSFER_SCALE_NO ) )

static const char *			lastError = "";
static int					initialized = 0;
static int					inFrame = 0;
static DVLB_s *				vshDvlb = NULL;
static shaderProgram_s		program;
static int					uLocMvp = -1;
static int					uLocColor = -1;
static int					uLocPow2 = -1;
static int					uLocLightOrigin = -1;
static int					uLocLightS = -1;
static int					uLocLightT = -1;
static int					uLocLightQ = -1;
static int					uLocLightF = -1;
static int					uLocPassParams = -1;
// hardware-lit program (n3ds_vshader_lit.v.pica)
static DVLB_s *				vshDvlbLit = NULL;
static shaderProgram_s		programLit;
static int					litOk = 0;
static int					litMvp = -1;
static int					litMv = -1;
static int					litColor = -1;
static int					litPow2 = -1;
static int					activeLit = 0;			// program in use: 0 base, 1 lit, 2 ambient/gui
// ambient / GUI program (n3ds_vshader_amb.v.pica)
static DVLB_s *				vshDvlbAmb = NULL;
static shaderProgram_s		programAmb;
static int					ambOk = 0;
static int					ambMvp = -1;
static int					ambColor = -1;
static int					ambVc = -1;
static int					ambTexS = -1;
static int					ambTexT = -1;
static int					ambPow2 = -1;
static int					stageStateKey = -1;
static int					texEnvMode = -1;		// texture combiner setup of the current pass
static int					alphaTestOn = -1;

// staging buffers of texture copies queued in this frame: freed once the GPU is done.
// citro3d's GX command queue holds 32 entries per frame; the frame itself needs a
// few (clear, command list, display transfer), so at most this many copies per frame
#define MAX_PENDING_STAGING	24
static void *				pendingStaging[2][MAX_PENDING_STAGING];
static int					numPendingStaging[2];
static int					pendingSet = 0;
static C3D_RenderTarget *	topTarget = NULL;
static void					( *hangCallback )( unsigned int ) = NULL;

void N3DSGPU_SetHangCallback( void ( *callback )( unsigned int waitedMs ) ) {
	hangCallback = callback;
}

const char * N3DSGPU_LastError( void ) {
	return lastError;
}

// half float exponent table for the st decode (see n3ds_vshader.v.pica)
static void N3DS_SetPow2( int loc ) {
	for ( int e = 0; e < 32; e++ ) {
		float x, y;
		if ( e == 0 ) {
			x = 0.0f;
			y = 1.0f / 16777216.0f;			// 2^-24: denormals
		} else if ( e == 31 ) {
			x = 65504.0f;					// inf / nan: largest half
			y = 0.0f;
		} else {
			x = ( e >= 15 ) ? (float)( 1 << ( e - 15 ) ) : 1.0f / (float)( 1 << ( 15 - e ) );
			y = x / 1024.0f;
		}
		C3D_FVUnifSet( GPU_VERTEX_SHADER, loc + e, x, y, 0.0f, 0.0f );
	}
}

static void N3DS_HWLightInit( void );

int N3DSGPU_Init( void ) {
	if ( initialized ) {
		return 1;
	}
	// one frame of draw calls lives here; each draw is ~100 bytes of commands
	if ( !C3D_Init( C3D_DEFAULT_CMDBUF_SIZE * 4 ) ) {
		lastError = "C3D_Init failed";
		return 0;
	}
	C3D_DEPTHTYPE depthType;		// transparent union in C; built explicitly for clang
	depthType.__e = GPU_RB_DEPTH24_STENCIL8;
	topTarget = C3D_RenderTargetCreate( 240, 400, GPU_RB_RGBA8, depthType );
	if ( topTarget == NULL ) {
		lastError = "C3D_RenderTargetCreate failed (VRAM)";
		C3D_Fini();
		return 0;
	}
	C3D_RenderTargetSetOutput( topTarget, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS );

	vshDvlb = DVLB_ParseFile( (u32 *)n3ds_vshader_shbin, n3ds_vshader_shbin_size );
	if ( vshDvlb == NULL || vshDvlb->numDVLE < 1 ) {
		lastError = "DVLB_ParseFile failed";
		return 0;
	}
	shaderProgramInit( &program );
	shaderProgramSetVsh( &program, &vshDvlb->DVLE[0] );
	C3D_BindProgram( &program );
	uLocMvp = shaderInstanceGetUniformLocation( program.vertexShader, "mvp" );
	uLocColor = shaderInstanceGetUniformLocation( program.vertexShader, "surfColor" );
	uLocPow2 = shaderInstanceGetUniformLocation( program.vertexShader, "pow2" );
	uLocLightOrigin = shaderInstanceGetUniformLocation( program.vertexShader, "lightOrigin" );
	uLocLightS = shaderInstanceGetUniformLocation( program.vertexShader, "lightS" );
	uLocLightT = shaderInstanceGetUniformLocation( program.vertexShader, "lightT" );
	uLocLightQ = shaderInstanceGetUniformLocation( program.vertexShader, "lightQ" );
	uLocLightF = shaderInstanceGetUniformLocation( program.vertexShader, "lightF" );
	uLocPassParams = shaderInstanceGetUniformLocation( program.vertexShader, "passParams" );
	if ( uLocMvp < 0 || uLocColor < 0 || uLocPow2 < 0 || uLocLightOrigin < 0 || uLocLightS < 0 ||
			uLocLightT < 0 || uLocLightQ < 0 || uLocLightF < 0 || uLocPassParams < 0 ) {
		lastError = "shader uniforms not found";
		return 0;
	}
	N3DS_SetPow2( uLocPow2 );

	// the hardware-lit program; the renderer falls back to the other one without it
	vshDvlbLit = DVLB_ParseFile( (u32 *)n3ds_vshader_lit_shbin, n3ds_vshader_lit_shbin_size );
	if ( vshDvlbLit != NULL && vshDvlbLit->numDVLE >= 1 ) {
		shaderProgramInit( &programLit );
		shaderProgramSetVsh( &programLit, &vshDvlbLit->DVLE[0] );
		litMvp = shaderInstanceGetUniformLocation( programLit.vertexShader, "mvp" );
		litMv = shaderInstanceGetUniformLocation( programLit.vertexShader, "mv" );
		litColor = shaderInstanceGetUniformLocation( programLit.vertexShader, "surfColor" );
		litPow2 = shaderInstanceGetUniformLocation( programLit.vertexShader, "pow2" );
		litOk = ( litMvp >= 0 && litMv >= 0 && litColor >= 0 && litPow2 >= 0 );
	}
	vshDvlbAmb = DVLB_ParseFile( (u32 *)n3ds_vshader_amb_shbin, n3ds_vshader_amb_shbin_size );
	if ( vshDvlbAmb != NULL && vshDvlbAmb->numDVLE >= 1 ) {
		shaderProgramInit( &programAmb );
		shaderProgramSetVsh( &programAmb, &vshDvlbAmb->DVLE[0] );
		ambMvp = shaderInstanceGetUniformLocation( programAmb.vertexShader, "mvp" );
		ambColor = shaderInstanceGetUniformLocation( programAmb.vertexShader, "stageColor" );
		ambVc = shaderInstanceGetUniformLocation( programAmb.vertexShader, "vcParams" );
		ambTexS = shaderInstanceGetUniformLocation( programAmb.vertexShader, "texS" );
		ambTexT = shaderInstanceGetUniformLocation( programAmb.vertexShader, "texT" );
		ambPow2 = shaderInstanceGetUniformLocation( programAmb.vertexShader, "pow2" );
		ambOk = ( ambMvp >= 0 && ambColor >= 0 && ambVc >= 0 && ambTexS >= 0 && ambTexT >= 0 && ambPow2 >= 0 );
	}
	N3DS_HWLightInit();

	C3D_AttrInfo * attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init( attrInfo );
	AttrInfo_AddLoader( attrInfo, 0, GPU_FLOAT, 3 );			// v0 = position
	AttrInfo_AddLoader( attrInfo, 1, GPU_UNSIGNED_BYTE, 4 );	// v1 = normal
	AttrInfo_AddLoader( attrInfo, 2, GPU_SHORT, 2 );			// v2 = st (raw half floats)
	AttrInfo_AddLoader( attrInfo, 3, GPU_UNSIGNED_BYTE, 4 );	// v3 = vertex colour

	texEnvMode = -1;
	alphaTestOn = -1;
	N3DSGPU_SetFillPass( NULL, NULL, 0 );
	{
		const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		N3DSGPU_SetLightUniforms( zero, zero, zero, zero, zero );
		N3DSGPU_SetPassParams( 1, 0 );
	}

	// depth: near = 1, far = 0 (cleared to 0), as citro3d's own projections
	C3D_DepthMap( true, -1.0f, 0.0f );
	C3D_DepthTest( true, GPU_GREATER, GPU_WRITE_ALL );
	C3D_CullFace( GPU_CULL_NONE );
	C3D_AlphaBlend( GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO );

	initialized = 1;
	return 1;
}

void N3DSGPU_Shutdown( void ) {
	if ( !initialized ) {
		return;
	}
	if ( inFrame ) {
		C3D_FrameEnd( 0 );
		inFrame = 0;
	}
	C3D_LightEnvBind( NULL );
	shaderProgramFree( &program );
	DVLB_Free( vshDvlb );
	if ( vshDvlbAmb != NULL ) {
		shaderProgramFree( &programAmb );
		DVLB_Free( vshDvlbAmb );
		vshDvlbAmb = NULL;
	}
	ambOk = 0;
	if ( vshDvlbLit != NULL ) {
		shaderProgramFree( &programLit );
		DVLB_Free( vshDvlbLit );
		vshDvlbLit = NULL;
	}
	litOk = 0;
	activeLit = 0;
	C3D_RenderTargetDelete( topTarget );
	C3D_Fini();
	initialized = 0;
}

void N3DSGPU_BeginFrame( unsigned int clearRGBA ) {
	if ( !initialized || inFrame ) {
		return;
	}
	// non-blocking polls so a hung GPU is reported instead of freezing silently
	const u64 start = osGetTime();
	int reported = 0;
	while ( !C3D_FrameBegin( C3D_FRAME_SYNCDRAW | C3D_FRAME_NONBLOCK ) ) {
		const u64 waited = osGetTime() - start;
		if ( !reported && waited > 2000 ) {
			reported = 1;
			if ( hangCallback != NULL ) {
				hangCallback( (unsigned int)waited );
			}
		}
		svcSleepThread( 1000LL * 1000 );
	}
	// the previous frame (and its texture copies) is finished: free its staging buffers
	pendingSet ^= 1;
	for ( int i = 0; i < numPendingStaging[pendingSet]; i++ ) {
		linearFree( pendingStaging[pendingSet][i] );
	}
	numPendingStaging[pendingSet] = 0;
	C3D_RenderTargetClear( topTarget, C3D_CLEAR_ALL, clearRGBA, 0 );
	C3D_FrameDrawOn( topTarget );
	inFrame = 1;
}

void N3DSGPU_EndFrame( void ) {
	if ( !inFrame ) {
		return;
	}
	C3D_FrameEnd( 0 );
	inFrame = 0;
}

void N3DSGPU_SetMatrix( const float * rows ) {
	C3D_Mtx mtx;
	for ( int i = 0; i < 4; i++ ) {
		mtx.r[i].x = rows[i * 4 + 0];
		mtx.r[i].y = rows[i * 4 + 1];
		mtx.r[i].z = rows[i * 4 + 2];
		mtx.r[i].w = rows[i * 4 + 3];
	}
	C3D_FVUnifMtx4x4( GPU_VERTEX_SHADER, activeLit == 2 ? ambMvp : ( activeLit ? litMvp : uLocMvp ), &mtx );
}

void N3DSGPU_SetColor( float r, float g, float b, float a ) {
	C3D_FVUnifSet( GPU_VERTEX_SHADER, activeLit == 2 ? ambColor : ( activeLit ? litColor : uLocColor ), r, g, b, a );
}

void N3DSGPU_DrawIndexed( const void * verts, int stride, const unsigned short * indexes, int numIndexes ) {
	if ( !inFrame || numIndexes <= 0 ) {
		return;
	}
	C3D_BufInfo * bufInfo = C3D_GetBufInfo();
	BufInfo_Init( bufInfo );
	// idDrawVert, 32 bytes, described completely so the components add up to the stride:
	// position (attr 0, 12) | st (attr 2, 4) | normal (attr 1, 4) | tangent (pad 4 = 0xC) |
	// colour (attr 3, 4) | colour2 (pad 4)
	BufInfo_Add( bufInfo, verts, stride, 6, 0xC3C120 );
	C3D_DrawElements( GPU_TRIANGLES, numIndexes, C3D_UNSIGNED_SHORT, indexes );
}

typedef struct {
	float			xyz[3];
	short			st[2];
	unsigned char	normal[4];
	unsigned char	color[4];
} testVert_t;

static testVert_t *			testVerts = NULL;
static unsigned short *		testIndexes = NULL;
static unsigned char *		copyVerts = NULL;
static unsigned short *		copyIndexes = NULL;
static const int			COPY_VERTS_BYTES = 256 * 1024;
static const int			COPY_INDEX_BYTES = 64 * 1024;

void N3DSGPU_DrawTestTriangle( void ) {
	if ( !inFrame ) {
		return;
	}
	if ( testVerts == NULL ) {
		testVerts = (testVert_t *)linearAlloc( 3 * sizeof( testVert_t ) );
		testIndexes = (unsigned short *)linearAlloc( 16 );
		if ( testVerts == NULL || testIndexes == NULL ) {
			return;
		}
		// already in PICA clip space (z in -1..0); normals face +z
		const float p[3][3] = { { -0.6f, -0.6f, -0.5f }, { 0.6f, -0.6f, -0.5f }, { 0.0f, 0.6f, -0.5f } };
		for ( int i = 0; i < 3; i++ ) {
			testVerts[i].xyz[0] = p[i][0];
			testVerts[i].xyz[1] = p[i][1];
			testVerts[i].xyz[2] = p[i][2];
			testVerts[i].st[0] = 0;
			testVerts[i].st[1] = 0;
			testVerts[i].normal[0] = 128;
			testVerts[i].normal[1] = 128;
			testVerts[i].normal[2] = 255;
			testVerts[i].normal[3] = 0;
			testVerts[i].color[0] = testVerts[i].color[1] = testVerts[i].color[2] = testVerts[i].color[3] = 255;
			testIndexes[i] = (unsigned short)i;
		}
		GSPGPU_FlushDataCache( testVerts, 3 * sizeof( testVert_t ) );
		GSPGPU_FlushDataCache( testIndexes, 16 );
	}
	const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	N3DSGPU_SetMatrix( identity );
	N3DSGPU_SetColor( 1.0f, 0.2f, 0.2f, 1.0f );
	N3DSGPU_SetFillPass( NULL, NULL, 0 );
	N3DSGPU_SetPassParams( 1, 0 );
	C3D_BufInfo * bufInfo = C3D_GetBufInfo();
	BufInfo_Init( bufInfo );
	BufInfo_Add( bufInfo, testVerts, sizeof( testVert_t ), 4, 0x3120 );
	C3D_DrawElements( GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, testIndexes );
}

void N3DSGPU_DrawIndexedCopied( const void * verts, int stride, int numVerts, const unsigned short * indexes, int numIndexes ) {
	if ( !inFrame || numIndexes <= 0 ) {
		return;
	}
	if ( copyVerts == NULL ) {
		copyVerts = (unsigned char *)linearAlloc( COPY_VERTS_BYTES );
		copyIndexes = (unsigned short *)linearAlloc( COPY_INDEX_BYTES );
		if ( copyVerts == NULL || copyIndexes == NULL ) {
			return;
		}
	}
	if ( numVerts * stride > COPY_VERTS_BYTES || numIndexes * 2 > COPY_INDEX_BYTES ) {
		return;
	}
	memcpy( copyVerts, verts, numVerts * stride );
	memcpy( copyIndexes, indexes, numIndexes * 2 );
	GSPGPU_FlushDataCache( copyVerts, numVerts * stride );
	GSPGPU_FlushDataCache( copyIndexes, numIndexes * 2 );
	N3DSGPU_DrawIndexed( copyVerts, stride, copyIndexes, numIndexes );
}

float N3DSGPU_CmdBufUsage( void ) {
	return initialized ? C3D_GetCmdBufUsage() : 0.0f;
}

float N3DSGPU_DrawingTimeMs( void ) {
	return initialized ? C3D_GetDrawingTime() : 0.0f;
}

void N3DSGPU_FlushRange( const void * p, unsigned int size ) {
	if ( p != NULL && size > 0 ) {
		GSPGPU_FlushDataCache( p, size );
	}
}

/*
================================================================================================

	Textures

================================================================================================
*/

int N3DSGPU_CanQueueTexture( void ) {
	return !inFrame || numPendingStaging[pendingSet] < MAX_PENDING_STAGING;
}

unsigned int N3DSGPU_VRAMFree( void ) {
	return vramSpaceFree();
}

unsigned int N3DSGPU_LinearFree( void ) {
	return linearSpaceFree();
}

void * N3DSGPU_LinearAlloc( unsigned int size ) {
	return linearAlloc( size );
}

void * N3DSGPU_TexCreate( int width, int height, int format, int maxLevel, int repeat ) {
	if ( !initialized ) {
		return NULL;
	}
	C3D_Tex * tex = (C3D_Tex *)malloc( sizeof( C3D_Tex ) );
	if ( tex == NULL ) {
		return NULL;
	}
	GPU_TEXCOLOR fmt = GPU_RGB565;
	if ( format == N3DS_TEX_RGBA5551 ) {
		fmt = GPU_RGBA5551;
	} else if ( format == N3DS_TEX_RGBA4 ) {
		fmt = GPU_RGBA4;
	} else if ( format == N3DS_TEX_A8 ) {
		fmt = GPU_A8;		// alpha-test masks (coverage images)
	}
	C3D_TexInitParams params;
	memset( &params, 0, sizeof( params ) );
	params.width = (u16)width;
	params.height = (u16)height;
	params.maxLevel = (u8)maxLevel;
	params.format = fmt;
	params.type = GPU_TEX_2D;
	params.onVram = true;
	if ( !C3D_TexInitWithParams( tex, NULL, params ) ) {
		free( tex );
		return NULL;
	}
	C3D_TexSetFilter( tex, GPU_LINEAR, GPU_LINEAR );
	C3D_TexSetFilterMipmap( tex, GPU_LINEAR );
	if ( repeat == N3DS_WRAP_REPEAT ) {
		C3D_TexSetWrap( tex, GPU_REPEAT, GPU_REPEAT );
	} else if ( repeat == N3DS_WRAP_ZERO ) {
		C3D_TexSetWrap( tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER );
		tex->border = 0;		// black, alpha 0
	} else {
		C3D_TexSetWrap( tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE );
	}
	return tex;
}

int N3DSGPU_TexUpload( void * handle, void * staging, unsigned int size ) {
	C3D_Tex * tex = (C3D_Tex *)handle;
	if ( tex == NULL || staging == NULL ) {
		return 0;
	}
	const u32 total = C3D_TexCalcTotalSize( tex->size, tex->maxLevel );
	if ( size != total ) {
		linearFree( staging );
		return 0;
	}
	if ( inFrame && numPendingStaging[pendingSet] >= MAX_PENDING_STAGING ) {
		linearFree( staging );		// too many copies in one frame: try again next frame
		return 0;
	}
	GSPGPU_FlushDataCache( staging, size );
	if ( inFrame ) {
		// Queued without splitting the frame: this frame's draws are only submitted
		// at C3D_FrameEnd, behind the copy, and no draw recorded so far uses this
		// (new) texture. One queue entry per copy (a split would take another).
		// The staging buffer must live until the frame is done.
		GX_TextureCopy( (u32 *)staging, 0, (u32 *)tex->data, 0, size, 8 );
		pendingStaging[pendingSet][numPendingStaging[pendingSet]++] = staging;
	} else {
		// whole mip chain in one copy (the levels are contiguous in both buffers)
		C3D_SyncTextureCopy( (u32 *)staging, 0, (u32 *)tex->data, 0, size, 8 );
		linearFree( staging );	// synchronous outside a frame
	}
	return 1;
}

void N3DSGPU_TexFree( void * handle ) {
	C3D_Tex * tex = (C3D_Tex *)handle;
	if ( tex == NULL ) {
		return;
	}
	C3D_TexDelete( tex );
	free( tex );
}

/*
================================================================================================

	Passes

	fill:  unit 2 = diffuse (st), unit 1 = alpha mask (st), colour = vertex colour
	light: unit 0 = light projection, unit 1 = light falloff, unit 2 = diffuse,
	       x vertex colour x 4 (the vertex colour carries light colour / 4 so that
	       overbright lights survive the 0..1 clamp), added to the framebuffer where
	       the depth is equal to the fill pass

================================================================================================
*/

static C3D_Tex	projectionTex;		// unit 0 copy of a light image with the projection mode set

static void N3DS_StageInit( int id ) {
	C3D_TexEnv * env = C3D_GetTexEnv( id );
	C3D_TexEnvInit( env );
}

void N3DSGPU_SetFillPass( void * diffuse, void * mask, int alphaSource ) {
	// alphaSource: 0 = no alpha test, 1 = diffuse alpha, 2 = mask alpha
	if ( alphaSource == 2 && mask == NULL ) {
		alphaSource = 0;
	}
	if ( alphaSource != 0 && diffuse == NULL ) {
		alphaSource = 0;
	}
	if ( diffuse != NULL ) {
		C3D_TexBind( 2, (C3D_Tex *)diffuse );
	}
	if ( alphaSource == 2 ) {
		C3D_TexBind( 1, (C3D_Tex *)mask );
	}
	const int key = 0x100 | ( diffuse != NULL ? 1 : 0 ) | ( alphaSource << 1 );
	if ( key != texEnvMode ) {
		const int wasLight = ( texEnvMode & 0x200 ) != 0 || texEnvMode < 0;
		texEnvMode = key;
		C3D_TexEnv * env = C3D_GetTexEnv( 0 );
		C3D_TexEnvInit( env );
		if ( diffuse != NULL ) {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_TEXTURE2, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		} else {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_REPLACE );
		}
		if ( alphaSource == 1 ) {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE2, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		} else if ( alphaSource == 2 ) {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		} else {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		}
		C3D_TexEnvFunc( env, C3D_Alpha, GPU_REPLACE );
		if ( wasLight ) {
			N3DS_StageInit( 1 );
			N3DS_StageInit( 2 );
			C3D_DepthTest( true, GPU_GREATER, GPU_WRITE_ALL );
			C3D_AlphaBlend( GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO );
		}
	}
	const int at = ( alphaSource != 0 ) ? 1 : 0;
	if ( at != alphaTestOn ) {
		alphaTestOn = at;
		C3D_AlphaTest( at != 0, GPU_GREATER, 127 );
	}
}

void N3DSGPU_SetLightPass( void * projection, void * falloff, void * diffuse, int projective ) {
	if ( projection != NULL ) {
		C3D_Tex * src = (C3D_Tex *)projection;
		if ( projective ) {
			projectionTex = *src;
			projectionTex.param = ( projectionTex.param & ~GPU_TEXTURE_MODE( 7 ) ) | GPU_TEXTURE_MODE( GPU_TEX_PROJECTION );
			C3D_TexBind( 0, &projectionTex );
		} else {
			C3D_TexBind( 0, src );
		}
	}
	if ( falloff != NULL ) {
		C3D_TexBind( 1, (C3D_Tex *)falloff );
	}
	if ( diffuse != NULL ) {
		C3D_TexBind( 2, (C3D_Tex *)diffuse );
	}
	const int key = 0x200 | ( projection != NULL ? 1 : 0 ) | ( falloff != NULL ? 2 : 0 ) | ( diffuse != NULL ? 4 : 0 );
	if ( key != texEnvMode ) {
		const int wasFill = ( texEnvMode & 0x200 ) == 0;
		texEnvMode = key;
		C3D_TexEnv * env = C3D_GetTexEnv( 0 );
		C3D_TexEnvInit( env );
		C3D_TexEnvSrc( env, C3D_RGB, projection != NULL ? GPU_TEXTURE0 : GPU_CONSTANT,
			falloff != NULL ? GPU_TEXTURE1 : GPU_CONSTANT, GPU_PRIMARY_COLOR );
		C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		C3D_TexEnvSrc( env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT );
		C3D_TexEnvFunc( env, C3D_Alpha, GPU_REPLACE );

		env = C3D_GetTexEnv( 1 );
		C3D_TexEnvInit( env );
		C3D_TexEnvSrc( env, C3D_RGB, GPU_PREVIOUS, diffuse != NULL ? GPU_TEXTURE2 : GPU_CONSTANT, GPU_PRIMARY_COLOR );
		C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );

		env = C3D_GetTexEnv( 2 );
		C3D_TexEnvInit( env );
		C3D_TexEnvSrc( env, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		C3D_TexEnvScale( env, C3D_RGB, GPU_TEVSCALE_4 );

		if ( wasFill ) {
			C3D_DepthTest( true, GPU_EQUAL, GPU_WRITE_COLOR );
			// colour += src, destination alpha kept
			C3D_AlphaBlend( GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE );
		}
	}
	if ( alphaTestOn != 0 ) {
		alphaTestOn = 0;
		C3D_AlphaTest( false, GPU_GREATER, 127 );
	}
}

void N3DSGPU_SetLightUniforms( const float * origin, const float * s, const float * t, const float * q, const float * f ) {
	C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocLightOrigin, origin[0], origin[1], origin[2], origin[3] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocLightS, s[0], s[1], s[2], s[3] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocLightT, t[0], t[1], t[2], t[3] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocLightQ, q[0], q[1], q[2], q[3] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocLightF, f[0], f[1], f[2], f[3] );
}

void N3DSGPU_SetScissor( int enable, int left, int top, int right, int bottom ) {
	if ( !enable ) {
		C3D_SetScissor( GPU_SCISSOR_DISABLE, 0, 0, 0, 0 );
		return;
	}
	C3D_SetScissor( GPU_SCISSOR_NORMAL, (u32)left, (u32)top, (u32)right, (u32)bottom );
}

void N3DSGPU_SetPassParams( int fill, int projective ) {
	C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocPassParams, fill ? 1.0f : 0.0f, projective ? 1.0f : 0.0f, 0.0f, 0.0f );
}

/*
================================================================================================

	Hardware fragment lighting (n3ds_lighting 2)

	Eight PICA light slots. All slots share one distance attenuation table over
	0..1; each light scales its distance into it (distAttnScale = 1 / radius),
	written straight into the slot so the table is uploaded only once. Spot
	lights get their own cone table per slot, rebuilt when the cone changes.
	Light colours carry colour / scale; the combiner multiplies by scale.

================================================================================================
*/

static C3D_LightEnv		hwEnv;
static C3D_Light		hwLights[8];
static C3D_LightLutDA	hwDistLut;
static C3D_LightLut		hwSpotLut[8];
static float			hwSpotCos[8];			// cone the slot's table was built for (2 = none)
static int				hwEnabledMask = -1;
static int				hwEnvBound = 0;

static float N3DS_DistAttn( float x, float unused0, float unused1 ) {
	// 1 at the light, 0 at its radius, soft towards both ends
	(void)unused0;
	(void)unused1;
	if ( x >= 1.0f ) {
		return 0.0f;
	}
	const float a = 1.0f - x * x;
	return a * a;
}

static float hwSpotParam;
static float N3DS_SpotCone( float c, float cosOuter ) {
	// cosine of the angle to the spot axis -> 0 outside the cone, 1 well inside
	const float width = ( 1.0f - cosOuter ) * 0.35f + 0.0001f;
	float v = ( c - cosOuter ) / width;
	if ( v < 0.0f ) {
		v = 0.0f;
	} else if ( v > 1.0f ) {
		v = 1.0f;
	}
	return v * v * ( 3.0f - 2.0f * v );
}

static void N3DS_HWLightInit( void ) {
	C3D_LightEnvInit( &hwEnv );
	C3D_Material mtl;
	memset( &mtl, 0, sizeof( mtl ) );
	mtl.ambient[0] = mtl.ambient[1] = mtl.ambient[2] = 1.0f;
	mtl.diffuse[0] = mtl.diffuse[1] = mtl.diffuse[2] = 1.0f;
	C3D_LightEnvMaterial( &hwEnv, &mtl );
	C3D_LightEnvAmbient( &hwEnv, 0.0f, 0.0f, 0.0f );
	LightLutDA_Create( &hwDistLut, N3DS_DistAttn, 0.0f, 1.0f, 0.0f, 0.0f );
	for ( int i = 0; i < 8; i++ ) {
		C3D_LightInit( &hwLights[i], &hwEnv );
		C3D_LightSpecular0( &hwLights[i], 0.0f, 0.0f, 0.0f );
		C3D_LightSpecular1( &hwLights[i], 0.0f, 0.0f, 0.0f );
		C3D_LightDistAttn( &hwLights[i], &hwDistLut );
		C3D_LightEnable( &hwLights[i], false );
		hwSpotCos[i] = 2.0f;
	}
	hwEnabledMask = 0;
	hwEnvBound = 0;
	(void)hwSpotParam;
}

int N3DSGPU_UseProgram( int lit ) {
	if ( lit == 1 && !litOk ) {
		return 0;
	}
	if ( lit == 2 && !ambOk ) {
		return 0;
	}
	if ( lit == activeLit ) {
		return 1;
	}
	activeLit = lit;
	C3D_BindProgram( lit == 2 ? &programAmb : ( lit ? &programLit : &program ) );
	N3DS_SetPow2( lit == 2 ? ambPow2 : ( lit ? litPow2 : uLocPow2 ) );	// the programs' uniforms share registers
	texEnvMode = -1;
	alphaTestOn = -1;
	stageStateKey = -1;
	if ( lit != 1 ) {
		C3D_LightEnvBind( NULL );
		hwEnvBound = 0;
	}
	if ( lit == 0 ) {
		N3DSGPU_SetFillPass( NULL, NULL, 0 );
	}
	return 1;
}

void N3DSGPU_SetModelView( const float * rows ) {
	if ( activeLit != 1 ) {
		return;
	}
	C3D_FVUnifSet( GPU_VERTEX_SHADER, litMv + 0, rows[0], rows[1], rows[2], rows[3] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, litMv + 1, rows[4], rows[5], rows[6], rows[7] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, litMv + 2, rows[8], rows[9], rows[10], rows[11] );
}

void N3DSGPU_HWSetLight( int slot, const float * eyePos, const float * color, float invRadius,
		const float * spotDir, float cosOuter ) {
	if ( slot < 0 || slot >= 8 ) {
		return;
	}
	C3D_Light * light = &hwLights[slot];
	C3D_FVec pos = FVec4_New( eyePos[0], eyePos[1], eyePos[2], 1.0f );
	C3D_LightPosition( light, &pos );
	C3D_LightDiffuse( light, color[0], color[1], color[2] );
	// shared table, per-light scale (no table upload)
	light->conf.distAttnBias = f32tof20( 0.0f );
	light->conf.distAttnScale = f32tof20( invRadius );
	light->flags |= C3DF_Light_Dirty;
	if ( spotDir != NULL ) {
		C3D_LightSpotDir( light, spotDir[0], spotDir[1], spotDir[2] );
		if ( hwSpotCos[slot] != cosOuter ) {
			hwSpotCos[slot] = cosOuter;
			LightLut_FromFunc( &hwSpotLut[slot], N3DS_SpotCone, cosOuter, true );
			C3D_LightSpotLut( light, &hwSpotLut[slot] );
		} else {
			C3D_LightSpotEnable( light, true );
		}
	} else {
		C3D_LightSpotEnable( light, false );
	}
}

void N3DSGPU_HWEnableLights( int mask ) {
	if ( mask == hwEnabledMask ) {
		return;
	}
	for ( int i = 0; i < 8; i++ ) {
		const int on = ( mask >> i ) & 1;
		if ( on != ( ( hwEnabledMask >> i ) & 1 ) || hwEnabledMask < 0 ) {
			C3D_LightEnable( &hwLights[i], on != 0 );
		}
	}
	hwEnabledMask = mask;
}

void N3DSGPU_HWAmbient( float r, float g, float b ) {
	C3D_LightEnvAmbient( &hwEnv, r, g, b );
}

void N3DSGPU_SetHWPass( void * diffuse, void * mask, int alphaSource, int lit, int scale ) {
	// lit: diffuse x fragment lighting x scale x vertex colour; unlit: diffuse x vertex colour
	if ( alphaSource == 2 && mask == NULL ) {
		alphaSource = 0;
	}
	if ( alphaSource != 0 && diffuse == NULL ) {
		alphaSource = 0;
	}
	if ( diffuse != NULL ) {
		C3D_TexBind( 0, (C3D_Tex *)diffuse );
	}
	if ( alphaSource == 2 ) {
		C3D_TexBind( 1, (C3D_Tex *)mask );
	}
	if ( lit != hwEnvBound ) {
		hwEnvBound = lit;
		C3D_LightEnvBind( lit ? &hwEnv : NULL );
	}
	const int key = 0x400 | ( diffuse != NULL ? 1 : 0 ) | ( alphaSource << 1 ) | ( lit ? 8 : 0 ) | ( scale << 4 );
	if ( key != texEnvMode ) {
		const int first = ( texEnvMode & 0x400 ) == 0 || texEnvMode < 0;
		texEnvMode = key;
		C3D_TexEnv * env = C3D_GetTexEnv( 0 );
		C3D_TexEnvInit( env );
		const GPU_TEVSRC light = lit ? GPU_FRAGMENT_PRIMARY_COLOR : GPU_PRIMARY_COLOR;
		if ( diffuse != NULL ) {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_TEXTURE0, light, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		} else {
			C3D_TexEnvSrc( env, C3D_RGB, light, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_REPLACE );
		}
		if ( lit ) {
			C3D_TexEnvScale( env, C3D_RGB, scale >= 4 ? GPU_TEVSCALE_4 : ( scale >= 2 ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1 ) );
		}
		if ( alphaSource == 1 ) {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		} else if ( alphaSource == 2 ) {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		} else {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		}
		C3D_TexEnvFunc( env, C3D_Alpha, GPU_REPLACE );

		// stage 1: x vertex colour (lit: the diffuse stage colour; unlit: already in)
		env = C3D_GetTexEnv( 1 );
		C3D_TexEnvInit( env );
		if ( lit ) {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		}
		if ( diffuse != NULL && !lit ) {
			// unlit with texture: stage 0 was texture x light (= vertex colour) already
		}
		if ( first ) {
			N3DS_StageInit( 2 );
			C3D_DepthTest( true, GPU_GREATER, GPU_WRITE_ALL );
			C3D_AlphaBlend( GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO );
		}
	}
	const int at = ( alphaSource != 0 ) ? 1 : 0;
	if ( at != alphaTestOn ) {
		alphaTestOn = at;
		C3D_AlphaTest( at != 0, GPU_GREATER, 127 );
	}
}

/*
================================================================================================

	Ambient / GUI stages (program 2)

================================================================================================
*/

void N3DSGPU_SetStageUniforms( const float * color, float vcModulate, float vcAdd, const float * texS, const float * texT ) {
	if ( activeLit != 2 ) {
		return;
	}
	C3D_FVUnifSet( GPU_VERTEX_SHADER, ambColor, color[0], color[1], color[2], color[3] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, ambVc, vcModulate, vcAdd, 0.0f, 0.0f );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, ambTexS, texS[0], texS[1], texS[2], texS[3] );
	C3D_FVUnifSet( GPU_VERTEX_SHADER, ambTexT, texT[0], texT[1], texT[2], texT[3] );
}

static GPU_BLENDFACTOR N3DS_BlendFactor( int f ) {
	// N3DS_BLEND_* have the PICA (= GL) order
	return (GPU_BLENDFACTOR)f;
}

void N3DSGPU_SetStageState( int srcBlend, int dstBlend, int blendOp, int depthFunc, int writeMask,
		int alphaRef, int depthBiasUnits ) {
	const int key = srcBlend | ( dstBlend << 4 ) | ( blendOp << 8 ) | ( depthFunc << 10 ) | ( writeMask << 13 ) |
		( ( alphaRef + 1 ) << 18 ) | ( ( depthBiasUnits & 0xFF ) << 27 );
	if ( key == stageStateKey ) {
		return;
	}
	stageStateKey = key;
	static const GPU_BLENDEQUATION ops[4] = { GPU_BLEND_ADD, GPU_BLEND_SUBTRACT, GPU_BLEND_MIN, GPU_BLEND_MAX };
	C3D_AlphaBlend( ops[blendOp & 3], ops[blendOp & 3], N3DS_BlendFactor( srcBlend ), N3DS_BlendFactor( dstBlend ),
		N3DS_BlendFactor( srcBlend ), N3DS_BlendFactor( dstBlend ) );
	// the depth buffer is reversed (near = 1): GL's LEQUAL becomes GEQUAL
	static const GPU_TESTFUNC funcs[5] = { GPU_GEQUAL, GPU_ALWAYS, GPU_LESS, GPU_EQUAL, GPU_ALWAYS };
	const int f = ( depthFunc >= 0 && depthFunc < 5 ) ? depthFunc : 1;
	C3D_DepthTest( true, funcs[f], (GPU_WRITEMASK)( writeMask & 0x1F ) );
	if ( alphaRef >= 0 ) {
		C3D_AlphaTest( true, GPU_GREATER, alphaRef );
	} else {
		C3D_AlphaTest( false, GPU_ALWAYS, 0 );
	}
	alphaTestOn = -1;
	// polygon offset: pull the depth towards the viewer
	C3D_DepthMap( true, -1.0f, (float)depthBiasUnits * ( 1.0f / 65536.0f ) );
}

void N3DSGPU_SetStageTexEnv( void * tex, int swf, const float * swfAdd ) {
	// normal stage: texture x vertex colour. SWF GUI (as gui.pixel): texture x vertex
	// colour + add colour, then the colour premultiplied by its alpha
	if ( tex != NULL ) {
		C3D_TexBind( 0, (C3D_Tex *)tex );
	}
	const int key = 0x800 | ( tex != NULL ? 1 : 0 ) | ( swf ? 2 : 0 );
	if ( key != texEnvMode || swf ) {
		texEnvMode = key;
		C3D_TexEnv * env = C3D_GetTexEnv( 0 );
		C3D_TexEnvInit( env );
		if ( tex != NULL ) {
			C3D_TexEnvSrc( env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_Both, GPU_MODULATE );
		} else {
			C3D_TexEnvSrc( env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_Both, GPU_REPLACE );
		}
		env = C3D_GetTexEnv( 1 );
		C3D_TexEnvInit( env );
		env = C3D_GetTexEnv( 2 );
		C3D_TexEnvInit( env );
		if ( swf ) {
			// + add colour (signed, stored as 0.5 + add / 2)
			env = C3D_GetTexEnv( 1 );
			C3D_TexEnvSrc( env, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_Both, GPU_ADD_SIGNED );
			u32 c = 0;
			for ( int k = 0; k < 4; k++ ) {
				float v = 0.5f + 0.5f * swfAdd[k];
				v = v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
				c |= ( (u32)( v * 255.0f + 0.5f ) ) << ( 8 * k );
			}
			C3D_TexEnvColor( env, c );
			// premultiply
			env = C3D_GetTexEnv( 2 );
			C3D_TexEnvSrc( env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PRIMARY_COLOR );
			C3D_TexEnvOpRgb( env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		}
	}
}

void N3DSGPU_EndStages( void ) {
	// back to the opaque setup
	C3D_DepthMap( true, -1.0f, 0.0f );
	C3D_DepthTest( true, GPU_GREATER, GPU_WRITE_ALL );
	C3D_AlphaBlend( GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO );
	C3D_AlphaTest( false, GPU_ALWAYS, 0 );
	alphaTestOn = -1;
	stageStateKey = -1;
	texEnvMode = -1;
}
