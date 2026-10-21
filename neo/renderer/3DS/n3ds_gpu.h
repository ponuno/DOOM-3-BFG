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

void			N3DSGPU_BeginFrame( unsigned int clearRGBA );
void			N3DSGPU_EndFrame( void );

// rows[16]: row-major 4x4, already converted to PICA clip space
void			N3DSGPU_SetMatrix( const float * rows );
void			N3DSGPU_SetColor( float r, float g, float b, float a );

// verts: idDrawVert layout (xyz float3 @0, normal ubyte4 @16), linear memory
// indexes: 16-bit, linear memory
void			N3DSGPU_DrawIndexed( const void * verts, int stride, const unsigned short * indexes, int numIndexes );

// make CPU writes in linear memory visible to the GPU
void			N3DSGPU_FlushRange( const void * p, unsigned int size );

#ifdef __cplusplus
}
#endif

#endif
