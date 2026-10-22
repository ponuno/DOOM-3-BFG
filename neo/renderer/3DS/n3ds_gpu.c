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
#include <stdio.h>

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

// textures released while a frame that may sample them is still being built or drawn:
// deleted once that frame is finished (with the staging buffers)
#define MAX_PENDING_FREE	512
static C3D_Tex *			pendingFree[2][MAX_PENDING_FREE];
static int					numPendingFree[2];
// texture copies asked for while a frame is being built: done at the next BeginFrame,
// alone on the GPU, before that frame's command list. Queued into the frame itself
// they were submitted together with its command list (libctru runs up to three GX
// commands at once), so the GPU could draw with a texture still being copied into
// VRAM. A texture with its copy pending is not handed out (N3DSGPU_TexPending).
#define MAX_DEFERRED_COPIES	24
static C3D_Tex *			deferredTex[MAX_DEFERRED_COPIES];
static void *				deferredStaging[MAX_DEFERRED_COPIES];
static u32					deferredSize[MAX_DEFERRED_COPIES];
static int					numDeferred = 0;
// what each texture unit has bound (citro3d keeps the pointer and reads the C3D_Tex
// again whenever it re-emits the unit's registers, e.g. after a home menu visit)
static C3D_Tex *			boundTex[3];
// what a unit gets when its texture is released: citro3d cannot bind NULL to units 1
// and 2 (C3D_TexBind reads the texture's type), and a unit must never hold a freed one
static C3D_Tex				placeholderTex;
static int					placeholderOk = 0;

// C3D_FrameEnd( 0 ) cleans the CPU data cache over the whole linear heap every frame:
// 69 MB in the 178 MB mode. Everything the GPU reads is flushed where it is written
// (frame vertex cache at BeginBackEnd, static cache and textures on upload, sound
// buffers), so only the command list needs it. fullFlush: the old behaviour.
static int					fullFlush = 0;

void N3DSGPU_SetFullFlush( int enable ) {
	fullFlush = enable;
}

static u8 N3DS_FrameEndFlags( void ) {
	return fullFlush ? 0 : GX_CMDLIST_FLUSH;
}

static void N3DS_BindTex( int unit, C3D_Tex * tex ) {
	boundTex[unit] = tex;
	C3D_TexBind( unit, tex );
}

static void N3DS_DeleteTex( C3D_Tex * tex ) {
	C3D_TexDelete( tex );
	free( tex );
}
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
	AttrInfo_AddLoader( attrInfo, 4, GPU_UNSIGNED_BYTE, 4 );	// v4 = tangent (lit program: normal maps)

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

	// 8x8 black placeholder (linear memory)
	if ( C3D_TexInit( &placeholderTex, 8, 8, GPU_RGB565 ) ) {
		memset( placeholderTex.data, 0, placeholderTex.size );
		GSPGPU_FlushDataCache( placeholderTex.data, placeholderTex.size );
		placeholderOk = 1;
	}

	initialized = 1;
	return 1;
}

void N3DSGPU_Shutdown( void ) {
	if ( !initialized ) {
		return;
	}
	if ( inFrame ) {
		C3D_FrameEnd( N3DS_FrameEndFlags() );
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
	if ( placeholderOk ) {
		C3D_TexDelete( &placeholderTex );
		placeholderOk = 0;
	}
	C3D_RenderTargetDelete( topTarget );
	C3D_Fini();
	initialized = 0;
}

static void N3DS_RunDeferredCopies( void );
static int N3DS_LostCompletion( u64 stuckSince );
static const char * N3DS_GxCommandName( u32 id );

static void ( *noticeCallback )( const char * text ) = NULL;

void N3DSGPU_SetNoticeCallback( void ( *callback )( const char * text ) ) {
	noticeCallback = callback;
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
		if ( reported ) {
			N3DS_LostCompletion( start );
		}
		svcSleepThread( 1000LL * 1000 );
	}
	// the previous frame is finished: the copies asked for while it was built, on their own
	N3DS_RunDeferredCopies();
	// staging buffers and textures released while the previous frame was built
	pendingSet ^= 1;
	for ( int i = 0; i < numPendingFree[pendingSet]; i++ ) {
		N3DS_DeleteTex( pendingFree[pendingSet][i] );
	}
	numPendingFree[pendingSet] = 0;
	// depth 0 (far, the depth buffer is reversed), stencil 128: SWF masks count from there
	C3D_RenderTargetClear( topTarget, C3D_CLEAR_ALL, clearRGBA, 128u << 24 );
	C3D_FrameDrawOn( topTarget );
	inFrame = 1;
}

// hang hunting: submit what was drawn so far and wait for the GPU to finish it
static int N3DS_LostCompletion( u64 stuckSince );

int N3DSGPU_DebugSync( unsigned int timeoutMs ) {
	if ( !inFrame ) {
		return 1;
	}
	C3D_FrameEnd( N3DS_FrameEndFlags() );
	const u64 start = osGetTime();
	int result = 1;
	while ( !C3D_FrameBegin( C3D_FRAME_NONBLOCK ) ) {
		const u64 waited = osGetTime() - start;
		if ( waited > timeoutMs && result == 1 ) {
			// the GPU idle: count the completion interrupt by hand and see whether the
			// queue then finishes (an interrupt was lost) or not (the GPU is stuck)
			if ( !N3DS_LostCompletion( start ) ) {
				continue;		// not idle for 0.5 s yet
			}
			result = 2;
		}
		if ( waited > timeoutMs + 2000 ) {
			inFrame = 0;		// the GPU is stuck on it; BeginFrame keeps reporting
			return 0;
		}
		svcSleepThread( 200LL * 1000 );
	}
	C3D_FrameDrawOn( topTarget );	// no clear: the frame goes on
	return result;
}

/*
================================================================================================

	GPU hang report

	citro3d runs a frame as entries of its GX command queue: the clear (memory fill),
	texture copies queued during the frame, the command list(s), the display transfer.
	libctru submits up to three at a time and counts the completion interrupts
	(lastEntry), so the first entry not counted is the one the GPU is stuck on: a
	transfer (the PPF engine) or a command list (P3D, the 3D pipeline).

	The queue sits at the start of citro3d's context (citro3d/source/internal.h,
	C3D_Context: gxQueue, cmdBuf, cmdBufSize); it is not public, the layout below is
	that of the citro3d this port is built with.

================================================================================================
*/
typedef struct {
	gxCmdQueue_s	gxQueue;
	u32 *			cmdBuf;
	size_t			cmdBufSize;
} n3dsC3DContextHead_t;
extern n3dsC3DContextHead_t __C3D_Context;

// libctru counts the GX queue's completion interrupts (gxqueue.c); the queue waits for the
// next one forever if it never comes
extern void gxCmdQueueInterrupt( GSPGPU_Event irq );

/*
========================
N3DS_LostCompletion

Diagnostics for the Mars City stops (0088-0098): the GX queue waits for an entry while the
GPU busy register (0x10400034: bit 31 P3D, bit 30 PPF, bits 26/27 PSC0/1) says no engine is
working - the work is done or never started, and its completion interrupt was not
counted. After 2 s of waiting and 0.5 s of an idle GPU this logs the queue entry and
counts its interrupt (gxCmdQueueInterrupt), so the queue moves on. If the frame then
appears and the game goes on, the GPU had not hung: an interrupt was lost. If the GPU
was stuck after all, the next frame stops the same way.
========================
*/
static int N3DS_LostCompletion( u64 stuckSince ) {
	static u64 idleSince = 0;
	static int logged = 0;
	u32 busy = 0xFFFFFFFF;
	if ( R_FAILED( GSPGPU_ReadHWRegs( 0x400034, &busy, 4 ) ) ) {
		return 0;
	}
	const u64 now = osGetTime();
	if ( ( busy & 0xCC000000u ) != 0 ) {
		idleSince = 0;
		return 0;
	}
	if ( idleSince < stuckSince ) {
		idleSince = now;
		return 0;
	}
	if ( now - idleSince < 500 ) {
		return 0;
	}
	idleSince = 0;
	const gxCmdQueue_s * q = &__C3D_Context.gxQueue;
	if ( q->lastEntry >= q->numEntries ) {
		return 0;
	}
	const u32 id = q->entries[q->lastEntry].data[0] & 0xFF;
	GSPGPU_Event irq = GSPGPU_EVENT_P3D;
	if ( id == 0x02 ) {
		irq = GSPGPU_EVENT_PSC0;
	} else if ( id == 0x03 || id == 0x04 ) {
		irq = GSPGPU_EVENT_PPF;
	} else if ( id == 0x00 ) {
		irq = GSPGPU_EVENT_DMA;
	}
	if ( noticeCallback != NULL && logged < 40 ) {
		logged++;
		char text[256];
		snprintf( text, sizeof( text ), "citro3d: GPU idle (busy %08lx) %llu ms into the wait, queue on entry %u of %u (%s): "
			"its completion interrupt never came - counted now (lost interrupt #%d)\n", busy, now - stuckSince,
			q->lastEntry, q->numEntries, N3DS_GxCommandName( id ), logged );
		noticeCallback( text );
	}
	gxCmdQueueInterrupt( irq );
	return 1;
}

// the GX queue is idle (C3D_FrameBegin cleared it, nothing of this frame is queued yet):
// the deferred texture copies run alone, and the frame starts once they are done
static void N3DS_RunDeferredCopies( void ) {
	if ( numDeferred == 0 ) {
		return;
	}
	gxCmdQueue_s * q = &__C3D_Context.gxQueue;
	for ( int i = 0; i < numDeferred; i++ ) {
		GX_TextureCopy( (u32 *)deferredStaging[i], 0, (u32 *)deferredTex[i]->data, 0, deferredSize[i], 8 );
	}
	gxCmdQueueRun( q );
	const u64 start = osGetTime();
	int reported = 0;
	while ( !gxCmdQueueWait( q, 0 ) ) {
		if ( !reported && osGetTime() - start > 2000 ) {
			reported = 1;
			if ( hangCallback != NULL ) {
				hangCallback( (unsigned int)( osGetTime() - start ) );
			}
		}
		if ( reported ) {
			N3DS_LostCompletion( start );
		}
		svcSleepThread( 200LL * 1000 );
	}
	gxCmdQueueStop( q );
	gxCmdQueueClear( q );
	for ( int i = 0; i < numDeferred; i++ ) {
		linearFree( deferredStaging[i] );
	}
	numDeferred = 0;
}
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;

static const char * N3DS_GxCommandName( u32 id ) {
	switch ( id & 0xFF ) {
		case 0x00:	return "DMA";
		case 0x01:	return "command list";
		case 0x02:	return "memory fill";
		case 0x03:	return "display transfer";
		case 0x04:	return "texture copy";
		case 0x05:	return "cache flush";
		default:	return "?";
	}
}

static const char * ( *hangTexNamer )( unsigned int address ) = NULL;

void N3DSGPU_SetHangTexNamer( const char * ( *namer )( unsigned int address ) ) {
	hangTexNamer = namer;
}

void N3DSGPU_TexDescribe( void * handle, char * out, int outSize ) {
	static const char * const formats[] = { "RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4", "LA8", "HILO8", "L8",
		"A8", "LA4", "L4", "A4", "ETC1", "ETC1A4" };
	const C3D_Tex * tex = (const C3D_Tex *)handle;
	if ( tex == NULL ) {
		snprintf( out, outSize, "none" );
		return;
	}
	const unsigned int fmt = (unsigned int)tex->fmt;
	snprintf( out, outSize, "%s %ux%u %u lv @%08lx%s", fmt < 14 ? formats[fmt] : "?", tex->width, tex->height,
		tex->maxLevel + 1u, (u32)tex->data, tex == &placeholderTex ? " (placeholder)" : "" );
}

int N3DSGPU_HangQueueReport( char * out, int outSize ) {
	if ( out == NULL || outSize <= 0 ) {
		return 0;
	}
	out[0] = 0;
	const gxCmdQueue_s * q = &__C3D_Context.gxQueue;
	int len = 0, lines = 0;
#define HANG_LINE( ... ) do { if ( len < outSize ) { len += snprintf( out + len, outSize - len, __VA_ARGS__ ); lines++; } } while ( 0 )
	HANG_LINE( "GX queue: %u entries, %u submitted, %u finished (the GPU is on entry %u)\n",
		q->numEntries, q->curEntry, q->lastEntry, q->lastEntry );
	for ( int i = 0; i < q->numEntries && i < q->maxEntries; i++ ) {
		const u32 * d = q->entries[i].data;
		const char * state = ( i < q->lastEntry ) ? "done   " : ( ( i < q->curEntry ) ? "STUCK  " : "waiting" );
		if ( i >= q->lastEntry && i < q->curEntry && i != q->lastEntry ) {
			state = "running";		// submitted behind the stuck one
		}
		switch ( d[0] & 0xFF ) {
			case 0x01:
				HANG_LINE( "  %2d %s command list %08lx, %lu bytes\n", i, state, d[1], d[2] );
				break;
			case 0x02:
				HANG_LINE( "  %2d %s memory fill %08lx-%08lx, %08lx-%08lx\n", i, state, d[1], d[3], d[4], d[6] );
				break;
			case 0x03:
				HANG_LINE( "  %2d %s display transfer %08lx -> %08lx, in %08lx out %08lx, flags %08lx\n", i, state,
					d[1], d[2], d[3], d[4], d[5] );
				break;
			case 0x04:
				HANG_LINE( "  %2d %s texture copy %08lx -> %08lx, %lu bytes, in %08lx out %08lx, flags %08lx (%s)\n", i, state,
					d[1], d[2], d[3], d[4], d[5], d[6], ( hangTexNamer != NULL && hangTexNamer( d[2] ) != NULL ) ? hangTexNamer( d[2] ) : "?" );
				break;
			default:
				HANG_LINE( "  %2d %s %s %08lx %08lx %08lx %08lx\n", i, state, N3DS_GxCommandName( d[0] ), d[1], d[2], d[3], d[4] );
				break;
		}
	}
	// GPU status register (0x10400034): which engines are busy
	u32 stat = 0;
	if ( R_SUCCEEDED( GSPGPU_ReadHWRegs( 0x400034, &stat, 4 ) ) ) {
		HANG_LINE( "GPU status %08lx\n", stat );
	}
#undef HANG_LINE
	return lines;
}

static void N3DS_DumpPut32( FILE * f, u32 x ) {
	fwrite( &x, 4, 1, f );
}

// GPU registers worth reading: no data ports (LUT, uniform, shader code uploads), no triggers
static const u16 hangRegRanges[][2] = {
	{ 0x000, 0x0A7 },		// misc, texture units
	{ 0x0B8, 0x0E7 },		// combiners 0-3, combiner buffer, fog setup
	{ 0x0F0, 0x10F },		// combiners 4-5, fragment operations
	{ 0x112, 0x11F },		// framebuffer (0x110 / 0x111 are invalidate / flush triggers)
	{ 0x140, 0x1C7 },		// lights, lighting config
	{ 0x1D0, 0x22D },		// lighting LUT selection, vertex attribute buffers, index buffer
	{ 0x238, 0x23B },		// command buffer sizes / addresses
	{ 0x240, 0x25E },		// geometry pipeline
	{ 0x2B0, 0x2BE },		// vertex shader setup
	{ 0x2C0, 0x2C0 },
	{ 0x2CB, 0x2CB },
	{ 0x2D5, 0x2D5 },
};

int N3DSGPU_HangDump( const char * path, int withRegs ) {
	FILE * f = fopen( path, "wb" );
	if ( f == NULL ) {
		return 0;
	}
	const gxCmdQueue_s * q = &__C3D_Context.gxQueue;
	const int num = ( q->numEntries < q->maxEntries ) ? q->numEntries : q->maxEntries;
	N3DS_DumpPut32( f, 0x4847334E );		// "N3GH"
	N3DS_DumpPut32( f, 1 );
	N3DS_DumpPut32( f, __ctru_linear_heap );
	N3DS_DumpPut32( f, osConvertVirtToPhys( (const void *)__ctru_linear_heap ) );
	N3DS_DumpPut32( f, __ctru_linear_heap_size );
	N3DS_DumpPut32( f, (u32)num );
	N3DS_DumpPut32( f, q->curEntry );
	N3DS_DumpPut32( f, q->lastEntry );
	for ( int i = 0; i < num; i++ ) {
		fwrite( q->entries[i].data, 4, 8, f );
	}
	// the command lists, as the CPU wrote them
	for ( int i = 0; i < num; i++ ) {
		const u32 * d = q->entries[i].data;
		if ( ( d[0] & 0xFF ) != 0x01 ) {
			continue;
		}
		const u32 addr = d[1];
		u32 size = d[2];
		if ( addr < __ctru_linear_heap || addr + size > __ctru_linear_heap + __ctru_linear_heap_size ) {
			size = 0;		// not in linear memory: not ours to read
		}
		N3DS_DumpPut32( f, 0x4C444D43 );	// "CMDL"
		N3DS_DumpPut32( f, (u32)i );
		N3DS_DumpPut32( f, size );
		fwrite( (const void *)addr, 1, size, f );
	}
	N3DS_DumpPut32( f, 0x444E4521 );		// "!END"
	const int ok = ( fclose( f ) == 0 );
	if ( !ok || !withRegs ) {
		return ok;
	}
	// the registers, appended once the rest is safely on the card
	f = fopen( path, "ab" );
	if ( f == NULL ) {
		return ok;
	}
	N3DS_DumpPut32( f, 0x53474552 );		// "REGS": PICA register, value pairs
	for ( unsigned int r = 0; r < sizeof( hangRegRanges ) / sizeof( hangRegRanges[0] ); r++ ) {
		for ( u32 reg = hangRegRanges[r][0]; reg <= hangRegRanges[r][1]; reg++ ) {
			u32 value = 0;
			if ( R_SUCCEEDED( GSPGPU_ReadHWRegs( 0x401000 + reg * 4, &value, 4 ) ) ) {
				N3DS_DumpPut32( f, reg );
				N3DS_DumpPut32( f, value );
			}
		}
	}
	// external registers: memory fill, transfer engine, status
	for ( u32 off = 0; off < 0x100; off += 4 ) {
		u32 value = 0;
		if ( R_SUCCEEDED( GSPGPU_ReadHWRegs( 0x400000 + off, &value, 4 ) ) ) {
			N3DS_DumpPut32( f, 0x10000 | off );
			N3DS_DumpPut32( f, value );
		}
	}
	for ( u32 off = 0xC00; off < 0xC40; off += 4 ) {
		u32 value = 0;
		if ( R_SUCCEEDED( GSPGPU_ReadHWRegs( 0x400000 + off, &value, 4 ) ) ) {
			N3DS_DumpPut32( f, 0x10000 | off );
			N3DS_DumpPut32( f, value );
		}
	}
	N3DS_DumpPut32( f, 0xFFFFFFFF );
	fclose( f );
	return ok;
}

void N3DSGPU_EndFrame( void ) {
	if ( !inFrame ) {
		return;
	}
	C3D_FrameEnd( N3DS_FrameEndFlags() );
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
	// position (attr 0, 12) | st (attr 2, 4) | normal (attr 1, 4) | tangent (attr 4, 4) |
	// colour (attr 3, 4) | colour2 (pad 4 = 0xC)
	BufInfo_Add( bufInfo, verts, stride, 6, 0xC34120 );
	C3D_DrawElements( GPU_TRIANGLES, numIndexes, C3D_UNSIGNED_SHORT, indexes );
}

typedef struct {
	float			xyz[3];
	short			st[2];
	unsigned char	normal[4];
	unsigned char	color[4];
	unsigned char	tangent[4];		// every attribute needs a buffer (v4 since normal maps)
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
			testVerts[i].tangent[0] = 255;
			testVerts[i].tangent[1] = testVerts[i].tangent[2] = 128;
			testVerts[i].tangent[3] = 255;
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
	BufInfo_Add( bufInfo, testVerts, sizeof( testVert_t ), 5, 0x43120 );
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
	return !inFrame || numDeferred < MAX_DEFERRED_COPIES;
}

int N3DSGPU_TexPending( void * handle ) {
	for ( int i = 0; i < numDeferred; i++ ) {
		if ( deferredTex[i] == (C3D_Tex *)handle ) {
			return 1;
		}
	}
	return 0;
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

static void * N3DS_TexCreateIn( int width, int height, int format, int maxLevel, int repeat, int onVram );

void * N3DSGPU_TexCreate( int width, int height, int format, int maxLevel, int repeat ) {
	return N3DS_TexCreateIn( width, height, format, maxLevel, repeat, 1 );
}

void * N3DSGPU_TexCreateLinear( int width, int height, int format, int maxLevel, int repeat ) {
	return N3DS_TexCreateIn( width, height, format, maxLevel, repeat, 0 );
}

void N3DSGPU_InvalidateRange( void * data, unsigned int size ) {
	GSPGPU_InvalidateDataCache( data, size );
}

void * N3DSGPU_TexData( void * tex ) {
	return ( tex != NULL ) ? ( (C3D_Tex *)tex )->data : NULL;
}

void N3DSGPU_TexFlush( void * tex, unsigned int size ) {
	if ( tex != NULL ) {
		GSPGPU_FlushDataCache( ( (C3D_Tex *)tex )->data, size );
	}
}

static void * N3DS_TexCreateIn( int width, int height, int format, int maxLevel, int repeat, int onVram ) {
	if ( !initialized ) {
		return NULL;
	}
	C3D_Tex * tex = (C3D_Tex *)malloc( sizeof( C3D_Tex ) );
	if ( tex == NULL ) {
		return NULL;
	}
	GPU_TEXCOLOR fmt = GPU_RGB565;
	switch ( format ) {
		case N3DS_TEX_RGBA5551:	fmt = GPU_RGBA5551; break;
		case N3DS_TEX_RGBA4:	fmt = GPU_RGBA4; break;
		case N3DS_TEX_A8:		fmt = GPU_A8; break;		// alpha-test masks, fonts
		case N3DS_TEX_ETC1:		fmt = GPU_ETC1; break;
		case N3DS_TEX_ETC1A4:	fmt = GPU_ETC1A4; break;
		case N3DS_TEX_HILO8:	fmt = GPU_HILO8; break;
		case N3DS_TEX_A4:		fmt = GPU_A4; break;
		default:				break;
	}
	C3D_TexInitParams params;
	memset( &params, 0, sizeof( params ) );
	params.width = (u16)width;
	params.height = (u16)height;
	params.maxLevel = (u8)maxLevel;
	params.format = fmt;
	params.type = GPU_TEX_2D;
	params.onVram = onVram != 0;
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
	GSPGPU_FlushDataCache( staging, size );
	if ( inFrame ) {
		if ( numDeferred >= MAX_DEFERRED_COPIES ) {
			linearFree( staging );		// too many copies this frame: tried again later
			return 0;
		}
		deferredTex[numDeferred] = tex;
		deferredStaging[numDeferred] = staging;
		deferredSize[numDeferred] = size;
		numDeferred++;
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
	// a copy not done yet: dropped (the texture was never handed out)
	for ( int i = 0; i < numDeferred; i++ ) {
		if ( deferredTex[i] == tex ) {
			linearFree( deferredStaging[i] );
			numDeferred--;
			deferredTex[i] = deferredTex[numDeferred];
			deferredStaging[i] = deferredStaging[numDeferred];
			deferredSize[i] = deferredSize[numDeferred];
			break;
		}
	}
	// no unit may keep pointing at it
	for ( int unit = 0; unit < 3; unit++ ) {
		if ( boundTex[unit] == tex ) {
			N3DS_BindTex( unit, placeholderOk ? &placeholderTex : NULL );
		}
	}
	// the frame being built, or the one the GPU is drawing, may still sample it: its VRAM
	// is given back once those are finished, so no other texture is copied over it meanwhile
	if ( inFrame && numPendingFree[pendingSet] < MAX_PENDING_FREE ) {
		pendingFree[pendingSet][numPendingFree[pendingSet]++] = tex;
		return;
	}
	N3DS_DeleteTex( tex );
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
static void N3DS_ResetSpecStages( void );

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
		N3DS_BindTex( 2, (C3D_Tex *)diffuse );
	}
	if ( alphaSource == 2 ) {
		N3DS_BindTex( 1, (C3D_Tex *)mask );
	}
	const int key = 0x100 | ( diffuse != NULL ? 1 : 0 ) | ( alphaSource << 1 );
	if ( key != texEnvMode ) {
		N3DS_ResetSpecStages();
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
			N3DS_BindTex( 0, &projectionTex );
		} else {
			N3DS_BindTex( 0, src );
		}
	}
	if ( falloff != NULL ) {
		N3DS_BindTex( 1, (C3D_Tex *)falloff );
	}
	if ( diffuse != NULL ) {
		N3DS_BindTex( 2, (C3D_Tex *)diffuse );
	}
	const int key = 0x200 | ( projection != NULL ? 1 : 0 ) | ( falloff != NULL ? 2 : 0 ) | ( diffuse != NULL ? 4 : 0 );
	if ( key != texEnvMode ) {
		N3DS_ResetSpecStages();
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
static C3D_LightLut		hwSpecLut;				// D0: specular highlight over N.H
static float			hwSpecPower = -1.0f;
static int				hwBumpOn = -1;			// light env bump mode as set
// combiner stage 3 and the combiner buffer, used by the specular pass: put back by the others
static int				specStagesActive = 0;
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

static void N3DS_ResetSpecStages( void ) {
	if ( !specStagesActive ) {
		return;
	}
	specStagesActive = 0;
	C3D_TexEnvInit( C3D_GetTexEnv( 3 ) );
	C3D_TexEnvBufUpdate( C3D_Both, 0 );
}

void N3DSGPU_HWSpecularPower( float power ) {
	if ( power == hwSpecPower ) {
		return;
	}
	// BFG interaction.pixel: pow( N.H, 10 ) x specular map; uploaded only when it changes
	hwSpecPower = power;
	LightLut_FromFunc( &hwSpecLut, powf, power, false );
	C3D_LightEnvLut( &hwEnv, GPU_LUT_D0, GPU_LUTINPUT_NH, false, &hwSpecLut );
}

static void N3DS_HWLightInit( void ) {
	C3D_LightEnvInit( &hwEnv );
	C3D_Material mtl;
	memset( &mtl, 0, sizeof( mtl ) );
	mtl.ambient[0] = mtl.ambient[1] = mtl.ambient[2] = 1.0f;
	mtl.diffuse[0] = mtl.diffuse[1] = mtl.diffuse[2] = 1.0f;
	// specular: the light's colour x the D0 table; the combiner multiplies it by the
	// specular map, or leaves it out
	mtl.specular0[0] = mtl.specular0[1] = mtl.specular0[2] = 1.0f;
	C3D_LightEnvMaterial( &hwEnv, &mtl );
	C3D_LightEnvAmbient( &hwEnv, 0.0f, 0.0f, 0.0f );
	LightLutDA_Create( &hwDistLut, N3DS_DistAttn, 0.0f, 1.0f, 0.0f, 0.0f );
	hwSpecPower = -1.0f;
	N3DSGPU_HWSpecularPower( 10.0f );
	// no highlight where N.L < 0 (BFG multiplies the specular term by N.L too)
	C3D_LightEnvClampHighlights( &hwEnv, true );
	// normal maps on texture unit 1, HILO8: x and y, z rebuilt by the GPU
	C3D_LightEnvBumpSel( &hwEnv, 1 );
	C3D_LightEnvBumpNormalZ( &hwEnv, false );
	C3D_LightEnvBumpMode( &hwEnv, GPU_BUMP_NOT_USED );
	hwBumpOn = 0;
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
	N3DS_ResetSpecStages();
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
	C3D_LightSpecular0( light, color[0], color[1], color[2] );
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

void N3DSGPU_SetHWPass( void * diffuse, void * mask, int alphaSource, int lit, int scale, void * normalMap, void * specMap ) {
	// lit: diffuse x fragment lighting x scale x vertex colour (+ specular map x highlight x
	// scale); unlit: diffuse x vertex colour
	if ( alphaSource == 2 && mask == NULL ) {
		alphaSource = 0;
	}
	if ( alphaSource != 0 && diffuse == NULL ) {
		alphaSource = 0;
	}
	// unit 1 holds either the alpha mask or the normal map
	const int bump = ( lit && normalMap != NULL && alphaSource != 2 ) ? 1 : 0;
	const int spec = ( lit && specMap != NULL ) ? 1 : 0;
	if ( diffuse != NULL ) {
		N3DS_BindTex( 0, (C3D_Tex *)diffuse );
	}
	if ( alphaSource == 2 ) {
		N3DS_BindTex( 1, (C3D_Tex *)mask );
	} else if ( bump ) {
		N3DS_BindTex( 1, (C3D_Tex *)normalMap );
	}
	if ( spec ) {
		N3DS_BindTex( 2, (C3D_Tex *)specMap );
	}
	if ( lit != hwEnvBound ) {
		hwEnvBound = lit;
		C3D_LightEnvBind( lit ? &hwEnv : NULL );
	}
	if ( lit && bump != hwBumpOn ) {
		hwBumpOn = bump;
		C3D_LightEnvBumpMode( &hwEnv, bump ? GPU_BUMP_AS_BUMP : GPU_BUMP_NOT_USED );
	}
	const int key = 0x400 | ( diffuse != NULL ? 1 : 0 ) | ( alphaSource << 1 ) | ( lit ? 8 : 0 ) | ( scale << 4 ) | ( spec << 8 );
	if ( key != texEnvMode ) {
		const int first = ( texEnvMode & 0x400 ) == 0 || texEnvMode < 0;
		texEnvMode = key;
		const GPU_TEVSCALE tevScale = scale >= 4 ? GPU_TEVSCALE_4 : ( scale >= 2 ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1 );
		const GPU_TEVSRC light = lit ? GPU_FRAGMENT_PRIMARY_COLOR : GPU_PRIMARY_COLOR;
		// with a specular map, stage 0 is the highlight (kept in the combiner buffer) and
		// the diffuse chain moves one stage down; stage 3 adds the two
		const int d = spec ? 1 : 0;
		C3D_TexEnv * env;
		if ( spec ) {
			env = C3D_GetTexEnv( 0 );
			C3D_TexEnvInit( env );
			C3D_TexEnvSrc( env, C3D_RGB, GPU_TEXTURE2, GPU_FRAGMENT_SECONDARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
			// C3D_TexEnvInit leaves the alpha on GPU_PREVIOUS, which stage 0 does not have
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_Alpha, GPU_REPLACE );
			C3D_TexEnvScale( env, C3D_RGB, tevScale );
			// buffer update bit 0: stage 0's output goes into the combiner buffer, which
			// stages 2 and 3 read (the buffer a stage sees lags one stage behind)
			C3D_TexEnvBufUpdate( C3D_RGB, 1 );
			C3D_TexEnvBufUpdate( C3D_Alpha, 0 );
			specStagesActive = 1;
		}
		env = C3D_GetTexEnv( d );
		C3D_TexEnvInit( env );
		if ( diffuse != NULL ) {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_TEXTURE0, light, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		} else {
			C3D_TexEnvSrc( env, C3D_RGB, light, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_REPLACE );
		}
		if ( lit ) {
			C3D_TexEnvScale( env, C3D_RGB, tevScale );
		}
		if ( alphaSource == 1 ) {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		} else if ( alphaSource == 2 ) {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		} else {
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
		}
		C3D_TexEnvFunc( env, C3D_Alpha, GPU_REPLACE );

		// x vertex colour (lit: the diffuse stage colour; unlit: already in)
		env = C3D_GetTexEnv( d + 1 );
		C3D_TexEnvInit( env );
		if ( lit ) {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_MODULATE );
		}
		if ( spec ) {
			// + the highlight
			env = C3D_GetTexEnv( 3 );
			C3D_TexEnvInit( env );
			C3D_TexEnvSrc( env, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS_BUFFER, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_ADD );
		} else {
			// stage 2 may hold the specular chain's vertex colour stage
			N3DS_StageInit( 2 );
			N3DS_ResetSpecStages();
		}
		if ( first ) {
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

void N3DSGPU_SetStageTexEnv( void * tex, int swf, const float * swfAdd, int alphaTex ) {
	// normal stage: texture x vertex colour. SWF GUI (as gui.pixel): texture x vertex
	// colour + add colour, then the colour premultiplied by its alpha. An A8 texture
	// (font, coverage) has no colour: white x vertex colour, alpha x vertex alpha
	if ( tex != NULL ) {
		N3DS_BindTex( 0, (C3D_Tex *)tex );
	}
	const int key = 0x800 | ( tex != NULL ? 1 : 0 ) | ( swf ? 2 : 0 ) | ( ( tex != NULL && alphaTex ) ? 4 : 0 );
	if ( key != texEnvMode || swf ) {
		texEnvMode = key;
		N3DS_ResetSpecStages();
		C3D_TexEnv * env = C3D_GetTexEnv( 0 );
		C3D_TexEnvInit( env );
		if ( tex != NULL && alphaTex ) {
			C3D_TexEnvSrc( env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_RGB, GPU_REPLACE );
			C3D_TexEnvSrc( env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR );
			C3D_TexEnvFunc( env, C3D_Alpha, GPU_MODULATE );
		} else if ( tex != NULL ) {
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

static int stencilKey = -1;

void N3DSGPU_SetStencil( int func, int ref, int mask, int opFail, int opZFail, int opPass ) {
	const int key = ( func < 0 ) ? 0 : ( 1 | ( ( func & 7 ) << 1 ) | ( ( ref & 0xFF ) << 4 ) | ( ( mask & 0xFF ) << 12 ) |
		( ( opFail & 7 ) << 20 ) | ( ( opZFail & 7 ) << 23 ) | ( ( opPass & 7 ) << 26 ) );
	if ( key == stencilKey ) {
		return;
	}
	stencilKey = key;
	if ( func < 0 ) {
		C3D_StencilTest( false, GPU_ALWAYS, 0, 0xFF, 0 );
		C3D_StencilOp( GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP );
		return;
	}
	static const GPU_TESTFUNC funcs[8] = { GPU_ALWAYS, GPU_LESS, GPU_LEQUAL, GPU_GREATER, GPU_GEQUAL, GPU_EQUAL, GPU_NOTEQUAL, GPU_NEVER };
	// the GL op order is the PICA's
	C3D_StencilTest( true, funcs[func & 7], ref & 0xFF, mask & 0xFF, 0xFF );
	C3D_StencilOp( (GPU_STENCILOP)( opFail & 7 ), (GPU_STENCILOP)( opZFail & 7 ), (GPU_STENCILOP)( opPass & 7 ) );
}

void N3DSGPU_EndStages( void ) {
	N3DSGPU_SetStencil( -1, 0, 0, 0, 0, 0 );
	N3DS_ResetSpecStages();
	// back to the opaque setup
	C3D_DepthMap( true, -1.0f, 0.0f );
	C3D_DepthTest( true, GPU_GREATER, GPU_WRITE_ALL );
	C3D_AlphaBlend( GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO );
	C3D_AlphaTest( false, GPU_ALWAYS, 0 );
	alphaTestOn = -1;
	stageStateKey = -1;
	texEnvMode = -1;
}
