/*
================================================================================================

	New 3DS GPU layer: citro3d setup, frame begin/end and indexed draws.
	See n3ds_gpu.h.

================================================================================================
*/
#include <3ds.h>
#include <citro3d.h>
#include <string.h>

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
static C3D_RenderTarget *	topTarget = NULL;

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
	if ( uLocMvp < 0 || uLocColor < 0 ) {
		lastError = "shader uniforms not found";
		return 0;
	}

	C3D_AttrInfo * attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init( attrInfo );
	AttrInfo_AddLoader( attrInfo, 0, GPU_FLOAT, 3 );			// v0 = position
	AttrInfo_AddLoader( attrInfo, 1, GPU_UNSIGNED_BYTE, 4 );	// v1 = normal

	C3D_TexEnv * env = C3D_GetTexEnv( 0 );
	C3D_TexEnvInit( env );
	C3D_TexEnvSrc( env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
	C3D_TexEnvFunc( env, C3D_Both, GPU_REPLACE );

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
	C3D_FrameBegin( C3D_FRAME_SYNCDRAW );
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
	// position (attr 0, 12 bytes), skip st (4 bytes padding = 0xC), normal (attr 1)
	BufInfo_Add( bufInfo, verts, stride, 3, 0x1C0 );
	C3D_DrawElements( GPU_TRIANGLES, numIndexes, C3D_UNSIGNED_SHORT, indexes );
}

void N3DSGPU_FlushRange( const void * p, unsigned int size ) {
	if ( p != NULL && size > 0 ) {
		GSPGPU_FlushDataCache( p, size );
	}
}
