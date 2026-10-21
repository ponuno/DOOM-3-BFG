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
	N3DS_TEX_RGBA4		= 2
};
unsigned int	N3DSGPU_VRAMFree( void );
unsigned int	N3DSGPU_LinearFree( void );
void *			N3DSGPU_LinearAlloc( unsigned int size );
// NULL when VRAM has no room; maxLevel = number of mip levels - 1; wrap: N3DS_WRAP_*
void *			N3DSGPU_TexCreate( int width, int height, int format, int maxLevel, int repeat );
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
// scissor in render target pixels (240 x 400, the 3DS framebuffer orientation),
// right/bottom exclusive
void			N3DSGPU_SetScissor( int enable, int left, int top, int right, int bottom );

#ifdef __cplusplus
}
#endif

#endif
