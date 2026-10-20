/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

/*
	New 3DS has no OpenGL. Engine headers still use GL scalar types and a few
	core enum values as plain identifiers (index types, texture handles), so this
	header provides exactly those. The actual GPU backend is citro3d
	(renderer/Citro3D); no GL function is ever called on 3DS.
*/

#ifndef __GL_TYPES_3DS_H__
#define __GL_TYPES_3DS_H__

#include <stddef.h>
#include <stdint.h>

typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef void			GLvoid;
typedef signed char		GLbyte;
typedef short			GLshort;
typedef int				GLint;
typedef int				GLsizei;
typedef unsigned char	GLubyte;
typedef unsigned short	GLushort;
typedef unsigned int	GLuint;
typedef float			GLfloat;
typedef float			GLclampf;
typedef double			GLdouble;
typedef double			GLclampd;
typedef char			GLchar;
typedef ptrdiff_t		GLintptr;
typedef ptrdiff_t		GLsizeiptr;
typedef int64_t			GLint64;
typedef uint64_t		GLuint64;
typedef struct __GLsync *	GLsync;

#define GL_FALSE						0
#define GL_TRUE							1

#define GL_BYTE							0x1400
#define GL_UNSIGNED_BYTE				0x1401
#define GL_SHORT						0x1402
#define GL_UNSIGNED_SHORT				0x1403
#define GL_INT							0x1404
#define GL_UNSIGNED_INT					0x1405
#define GL_FLOAT						0x1406
#define GL_HALF_FLOAT					0x140B

#define GL_TEXTURE_2D					0x0DE1
#define GL_TEXTURE_CUBE_MAP				0x8513

#endif // !__GL_TYPES_3DS_H__
