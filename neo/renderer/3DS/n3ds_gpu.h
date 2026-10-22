/*
================================================================================================

	New 3DS GPU layer (citro3d), plain C interface.

	n3ds_gpu.c is the only file that includes <3ds.h> / <citro3d.h>; engine code
	talks to the GPU through these functions so the libctru names never meet the
	engine headers. n3ds_gpu.c is also compiled with the toolchain's default short
	enums, because citro3d/libctru structs (DVLE_s, C3D_TexEnv, ...) contain enums.

================================================================================================
*/
#ifndef __N3DS_GPU_H__
#define __N3DS_GPU_H__

#ifdef __cplusplus
extern "C" {
#endif

int				N3DSGPU_Init( void );						// 1 on success
void			N3DSGPU_Shutdown( void );
const char *	N3DSGPU_LastError( void );

// waits for the GPU to finish the previous frame; if that takes over 2 s the hang
// callback is called once (then the wait goes on)
void			N3DSGPU_BeginFrame( unsigned int clearRGBA );
void			N3DSGPU_SetHangCallback( void ( *callback )( unsigned int waitedMs ) );
void			N3DSGPU_EndFrame( void );

// rows[16]: row-major 4x4, already converted to PICA clip space
void			N3DSGPU_SetMatrix( const float * rows );
void			N3DSGPU_SetColor( float r, float g, float b, float a );

// verts: idDrawVert layout (xyz float3 @0, normal ubyte4 @16), linear memory
// indexes: 16-bit, linear memory
void			N3DSGPU_DrawIndexed( const void * verts, int stride, const unsigned short * indexes, int numIndexes );

// diagnostics: a triangle from a private buffer (16-byte vertices, exact layout),
// drawn with an identity matrix; isolates the engine's buffers from the GPU setup
void			N3DSGPU_DrawTestTriangle( void );
// copy a draw into a private linear buffer first (tests the engine buffer addresses)
void			N3DSGPU_DrawIndexedCopied( const void * verts, int stride, int numVerts, const unsigned short * indexes, int numIndexes );

// diagnostics: command buffer use 0..1, GPU time of the last frame (ms)
float			N3DSGPU_CmdBufUsage( void );
float			N3DSGPU_DrawingTimeMs( void );

// make CPU writes in linear memory visible to the GPU
void			N3DSGPU_FlushRange( const void * p, unsigned int size );

// textures: 16-bit formats in VRAM, PICA tiled layout, mip levels contiguous
enum {
	N3DS_TEX_RGB565		= 0,
	N3DS_TEX_RGBA5551	= 1,
	N3DS_TEX_RGBA4		= 2,
	N3DS_TEX_A8			= 3
};
unsigned int	N3DSGPU_VRAMFree( void );
unsigned int	N3DSGPU_LinearFree( void );
void *			N3DSGPU_LinearAlloc( unsigned int size );
// NULL when VRAM has no room; maxLevel = number of mip levels - 1; wrap: N3DS_WRAP_*
void *			N3DSGPU_TexCreate( int width, int height, int format, int maxLevel, int repeat );
// a texture in linear memory (FCRAM) instead of VRAM: the GPU samples it from there
// (slower); its data is written directly (N3DSGPU_TexData, then N3DSGPU_TexFlush)
void *			N3DSGPU_TexCreateLinear( int width, int height, int format, int maxLevel, int repeat );
void *			N3DSGPU_TexData( void * tex );
void			N3DSGPU_TexFlush( void * tex, unsigned int size );
// room for another texture copy in this frame (the GX queue is small)
int				N3DSGPU_CanQueueTexture( void );
// copies the whole chain from staging (linear memory, takes ownership) to VRAM
int				N3DSGPU_TexUpload( void * tex, void * staging, unsigned int size );
void			N3DSGPU_TexFree( void * tex );
// wrap modes for N3DSGPU_TexCreate
enum {
	N3DS_WRAP_CLAMP		= 0,
	N3DS_WRAP_REPEAT	= 1,
	N3DS_WRAP_ZERO		= 2		// clamp to a black, transparent border
};

// fill pass: diffuse (or NULL = vertex colour only) modulated by the vertex colour,
// depth written; alphaSource 0 = no alpha test, 1 = diffuse alpha, 2 = mask alpha
void			N3DSGPU_SetFillPass( void * diffuse, void * mask, int alphaSource );
// light pass: projection x falloff x diffuse x vertex colour x 4, added where the
// depth equals the fill pass; NULL textures count as white; projective: unit 0
// divides by q per pixel
void			N3DSGPU_SetLightPass( void * projection, void * falloff, void * diffuse, int projective );
// model-space light origin (w = 1: N.L shading, 0: none) and the S, T, Q and
// falloff planes of the light projection
void			N3DSGPU_SetLightUniforms( const float * origin, const float * s, const float * t, const float * q, const float * f );
void			N3DSGPU_SetPassParams( int fill, int projective );
// hardware fragment lighting (n3ds_lighting 2): program 1 = n3ds_vshader_lit
int				N3DSGPU_UseProgram( int lit );		// 0 if the lit program is not available
// rows[12]: model -> eye 3x4, row-major (lit program)
void			N3DSGPU_SetModelView( const float * rows );
// slot 0..7: eye-space position, colour (already / scale), 1 / radius for the
// distance attenuation; spotDir (eye space, pointing away from the light) and the
// cone's outer cosine, or NULL for a point light
void			N3DSGPU_HWSetLight( int slot, const float * eyePos, const float * color, float invRadius,
								const float * spotDir, float cosOuter );
void			N3DSGPU_HWEnableLights( int mask );
void			N3DSGPU_HWAmbient( float r, float g, float b );
// one pass: diffuse (unit 0, or NULL) x (lit ? fragment lighting x scale : 1) x vertex
// colour; alphaSource as N3DSGPU_SetFillPass (mask on unit 1); depth written
void			N3DSGPU_SetHWPass( void * diffuse, void * mask, int alphaSource, int lit, int scale );

// ambient / GUI stages (program 2 = n3ds_vshader_amb)
// colour = ( vertex colour * vcModulate + vcAdd ) * color; st through texS / texT
void			N3DSGPU_SetStageUniforms( const float * color, float vcModulate, float vcAdd, const float * texS, const float * texT );
// blend factors in GL order (0 zero, 1 one, 2 src colour, 3 1-src colour, 4 dst colour,
// 5 1-dst colour, 6 src alpha, 7 1-src alpha, 8 dst alpha, 9 1-dst alpha); blendOp 0 add,
// 1 subtract, 2 min, 3 max; depthFunc 0 GL lequal, 1 always, 2 GL greater, 3 equal;
// writeMask PICA bits (1 r, 2 g, 4 b, 8 a, 0x10 depth); alphaRef 0..255 or -1; depth bias
// in 1/65536 units towards the viewer
void			N3DSGPU_SetStageState( int srcBlend, int dstBlend, int blendOp, int depthFunc, int writeMask,
									int alphaRef, int depthBiasUnits );
// texture (or NULL) x vertex colour; swf: + swfAdd[4] (signed) and premultiplied alpha;
// alphaTex: an A8 texture, only its alpha is used
void			N3DSGPU_SetStageTexEnv( void * tex, int swf, const float * swfAdd, int alphaTex );
// stencil (SWF masks): func -1 = off, else GL order (0 always, 1 less, 2 lequal, 3 greater,
// 4 gequal, 5 equal, 6 notequal, 7 never); ops GL order (0 keep, 1 zero, 2 replace, 3 incr,
// 4 decr, 5 invert, 6 incr wrap, 7 decr wrap). The stencil starts every frame at 128.
void			N3DSGPU_SetStencil( int func, int ref, int mask, int opFail, int opZFail, int opPass );
void			N3DSGPU_EndStages( void );
// hang hunting: submit everything drawn so far, wait up to timeoutMs; 0 = the GPU did not finish
int				N3DSGPU_DebugSync( unsigned int timeoutMs );
// 1: clean the data cache over all linear memory at every frame end (citro3d default)
void			N3DSGPU_SetFullFlush( int enable );

// scissor in render target pixels (240 x 400, the 3DS framebuffer orientation),
// right/bottom exclusive
void			N3DSGPU_SetScissor( int enable, int left, int top, int right, int bottom );

#ifdef __cplusplus
}
#endif

#endif
