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

// generated at build time from n3ds_vshader.v.pica (picasso + cmake/bin2c.cmake)
extern const u8		n3ds_vshader_shbin[];
extern const u32	n3ds_vshader_shbin_size;

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
static int					texEnvMode = -1;		// N3DSGPU_SetTextures state
static int					alphaTestOn = -1;

// staging buffers of texture copies queued in this frame: freed once the GPU is done
#define MAX_PENDING_STAGING	64
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
	if ( uLocMvp < 0 || uLocColor < 0 || uLocPow2 < 0 ) {
		lastError = "shader uniforms not found";
		return 0;
	}
	// half float exponent table for the st decode (see n3ds_vshader.v.pica)
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
		C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocPow2 + e, x, y, 0.0f, 0.0f );
	}

	C3D_AttrInfo * attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init( attrInfo );
	AttrInfo_AddLoader( attrInfo, 0, GPU_FLOAT, 3 );			// v0 = position
	AttrInfo_AddLoader( attrInfo, 1, GPU_UNSIGNED_BYTE, 4 );	// v1 = normal
	AttrInfo_AddLoader( attrInfo, 2, GPU_SHORT, 2 );			// v2 = st (raw half floats)

	texEnvMode = -1;
	alphaTestOn = -1;
	N3DSGPU_SetTextures( NULL, NULL, 0 );

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
	shaderProgramFree( &program );
	DVLB_Free( vshDvlb );
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
	C3D_FVUnifMtx4x4( GPU_VERTEX_SHADER, uLocMvp, &mtx );
}

void N3DSGPU_SetColor( float r, float g, float b, float a ) {
	C3D_FVUnifSet( GPU_VERTEX_SHADER, uLocColor, r, g, b, a );
}

void N3DSGPU_DrawIndexed( const void * verts, int stride, const unsigned short * indexes, int numIndexes ) {
	if ( !inFrame || numIndexes <= 0 ) {
		return;
	}
	C3D_BufInfo * bufInfo = C3D_GetBufInfo();
	BufInfo_Init( bufInfo );
	// idDrawVert, 32 bytes, described completely so the components add up to the stride:
	// position (attr 0, 12) | st (attr 2, 4) | normal (attr 1, 4) | tangent, color, color2 (pad 12 = 0xE)
	BufInfo_Add( bufInfo, verts, stride, 4, 0xE120 );
	C3D_DrawElements( GPU_TRIANGLES, numIndexes, C3D_UNSIGNED_SHORT, indexes );
}

typedef struct {
	float			xyz[3];
	short			st[2];
	unsigned char	normal[4];
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
			testIndexes[i] = (unsigned short)i;
		}
		GSPGPU_FlushDataCache( testVerts, 3 * sizeof( testVert_t ) );
		GSPGPU_FlushDataCache( testIndexes, 16 );
	}
	const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	N3DSGPU_SetMatrix( identity );
	N3DSGPU_SetColor( 1.0f, 0.2f, 0.2f, 1.0f );
	N3DSGPU_SetTextures( NULL, NULL, 0 );
	C3D_BufInfo * bufInfo = C3D_GetBufInfo();
	BufInfo_Init( bufInfo );
	BufInfo_Add( bufInfo, testVerts, sizeof( testVert_t ), 3, 0x120 );
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
	if ( repeat ) {
		C3D_TexSetWrap( tex, GPU_REPEAT, GPU_REPEAT );
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
	// whole mip chain in one copy (the levels are contiguous in both buffers)
	C3D_SyncTextureCopy( (u32 *)staging, 0, (u32 *)tex->data, 0, size, 8 );
	if ( inFrame ) {
		// queued behind this frame's commands: the staging buffer must live until
		// the frame is done
		pendingStaging[pendingSet][numPendingStaging[pendingSet]++] = staging;
	} else {
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

void N3DSGPU_SetTextures( void * tex0, void * tex1, int alphaTest ) {
	int mode = 0;
	if ( tex0 != NULL ) {
		mode = ( tex1 != NULL ) ? 2 : 1;
	}
	if ( mode >= 1 ) {
		C3D_TexBind( 0, (C3D_Tex *)tex0 );
	}
	if ( mode == 2 ) {
		C3D_TexBind( 1, (C3D_Tex *)tex1 );
	}
	if ( mode != texEnvMode ) {
		texEnvMode = mode;
		C3D_TexEnv * env = C3D_GetTexEnv( 0 );
		C3D_TexEnvInit( env );
		if ( mode == 0 ) {
			C3D_TexEnvSrc( env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_Both, GPU_REPLACE );
		} else {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
			if ( mode == 1 ) {
				C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
				C3D_TexEnvFunc( env, C3D_Alpha, GPU_MODULATE );
			} else {
				C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
				C3D_TexEnvFunc( env, C3D_Alpha, GPU_REPLACE );
			}
		}
	}
	const int at = ( alphaTest && mode != 0 ) ? 1 : 0;
	if ( at != alphaTestOn ) {
		alphaTestOn = at;
		C3D_AlphaTest( at != 0, GPU_GREATER, 127 );
	}
}
