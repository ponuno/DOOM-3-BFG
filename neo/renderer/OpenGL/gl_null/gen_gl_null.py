#!/usr/bin/env python3
"""
Generates gl_null.cpp: a null OpenGL driver for the New 3DS bring-up build.

Every GL entry point the engine uses gets a stub with the exact prototype from
gl.h / glext.h. Core functions are real extern "C" symbols (qgl_linked.h maps
qglX -> glX); extension functions are handed out by GLimp_ExtensionPointer().

Stateful pieces that the engine depends on are emulated for real:
  - buffer objects (gen/bind/data/subdata/map/unmap) live in RAM, because the
    vertex cache writes geometry through mapped pointers;
  - object names are handed out from counters;
  - shaders/programs always compile and link;
  - glGetString reports GL 2.1 with exactly the extensions BFG requires.

Usage: gen_gl_null.py <gl.h> <glext.h> <names.txt> <out.cpp>
  names.txt: one GL function name per line (core and extension).
"""
import re, sys

gl_h, glext_h, names_txt, out_cpp = sys.argv[1:5]

proto_re = re.compile(r'GLAPI\s+(.+?)\s*(?:GLAPIENTRY|APIENTRY)\s+(gl\w+)\s*\(([^;]*?)\)\s*;', re.S)
protos = {}
for path in (gl_h, glext_h):
    for m in proto_re.finditer(open(path, encoding='latin-1').read()):
        ret, name, args = m.group(1).strip(), m.group(2), ' '.join(m.group(3).split())
        protos.setdefault(name, (ret, args))

names = [l.strip() for l in open(names_txt) if l.strip()]
core = []
ext = []
for n in names:
    if n not in protos:
        # some engine lookups use a suffix that glext.h does not declare
        # (e.g. glGetQueryObjectui64vARB); borrow the prototype of a sibling
        stem = re.sub(r'(ARB|EXT)$', '', n)
        alt = [c for c in (stem, stem + 'ARB', stem + 'EXT') if c in protos]
        if not alt:
            sys.exit('no prototype for ' + n)
        protos[n] = protos[alt[0]]
# core = declared in gl.h (Mesa), ext = only in glext.h
core_names = set(m.group(2) for m in proto_re.finditer(open(gl_h, encoding='latin-1').read()))

def argnames(args):
    if args.strip() in ('', 'void'):
        return []
    out = []
    for i, a in enumerate(args.split(',')):
        a = a.strip()
        m = re.search(r'(\w+)\s*(\[\s*\w*\s*\])?$', a)
        out.append(m.group(1) if m else 'a%d' % i)
    return out

# ---------------------------------------------------------------------------
# hand-written behaviour, keyed by the base name (ARB/EXT suffix stripped)
# ---------------------------------------------------------------------------
def base(n):
    return re.sub(r'(ARB|EXT)$', '', n)

special = {
'glGetError':        'return GL_NO_ERROR;',
'glGetString':       'return (const GLubyte *)NullGL_GetString( name );',
'glGetStringi':      'return (const GLubyte *)"";',
'glGetIntegerv':     'NullGL_GetIntegerv( pname, params );',
'glGetFloatv':       'GLint v[16] = { 0 }; NullGL_GetIntegerv( pname, v ); for ( int i = 0; i < 16; i++ ) { params[i] = (GLfloat)v[i]; }',
'glGenTextures':     'NullGL_GenNames( n, textures );',
'glGenBuffers':      'NullGL_GenBuffers( n, buffers );',
'glDeleteBuffers':   'NullGL_DeleteBuffers( n, buffers );',
'glBindBuffer':      'NullGL_BindBuffer( target, buffer );',
'glBindBufferRange': 'NullGL_BindBuffer( target, buffer );',
'glBufferData':      'NullGL_BufferData( target, size, data );',
'glBufferSubData':   'NullGL_BufferSubData( target, offset, size, data );',
'glGetBufferSubData':'NullGL_GetBufferSubData( target, offset, size, data );',
'glMapBuffer':       'return NullGL_MapBuffer( target, 0 );',
'glMapBufferRange':  'return NullGL_MapBuffer( target, offset );',
'glUnmapBuffer':     'return GL_TRUE;',
'glIsBuffer':        'return NullGL_IsBuffer( buffer );',
'glGetBufferParameteriv': 'NullGL_GetBufferParameteriv( target, pname, params );',
'glGetBufferPointerv':    '*params = NullGL_MapBuffer( target, 0 );',
'glGenQueries':      'NullGL_GenNames( n, ids );',
'glGetQueryObjectiv':'*params = ( pname == GL_QUERY_RESULT_AVAILABLE ) ? 1 : 0;',
'glGetQueryObjectuiv':'*params = ( pname == GL_QUERY_RESULT_AVAILABLE ) ? 1 : 0;',
'glGetQueryObjectui64v':'*params = 0;',
'glGetQueryiv':      '*params = 0;',
'glIsQuery':         'return GL_TRUE;',
'glGenVertexArrays': 'NullGL_GenNames( n, arrays );',
'glGenPrograms':     'NullGL_GenNames( n, programs );',
'glCreateShader':    'return NullGL_NewName();',
'glCreateProgram':   'return NullGL_NewName();',
'glGetShaderiv':     '*params = ( pname == GL_COMPILE_STATUS ) ? GL_TRUE : 0;',
'glGetProgramiv':    '*params = ( pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS ) ? GL_TRUE : 0;',
'glGetShaderInfoLog':'if ( length ) { *length = 0; } if ( bufSize > 0 && infoLog ) { infoLog[0] = 0; }',
'glGetProgramInfoLog':'if ( length ) { *length = 0; } if ( bufSize > 0 && infoLog ) { infoLog[0] = 0; }',
'glGetUniformLocation':'return 0;',
'glGetUniformBlockIndex':'return 0;',
'glFenceSync':       'return (GLsync)1;',
'glClientWaitSync':  'return GL_ALREADY_SIGNALED;',
'glIsSync':          'return GL_TRUE;',
'glGetDebugMessageLog':'return 0;',
'glReadPixels':      'NullGL_ReadPixels( width, height, format, type, pixels );',
'glGetCompressedTexImage':'',
}

def body_for(name, ret, args):
    b = base(name)
    if b in special:
        return special[b]
    if name in special:
        return special[name]
    if ret == 'void':
        return ''
    return 'return 0;'

core_out, ext_out, table = [], [], []
for n in names:
    ret, args = protos[n]
    body = body_for(n, ret, args)
    sig_args = args if args.strip() else 'void'
    if n in core_names:
        core_out.append('GLAPI %s GLAPIENTRY %s( %s ) { %s }' % (ret, n, sig_args, body))
        # the engine fetches some gl.h functions (e.g. glActiveTextureARB) by name too
        table.append('\t{ "%s", (GLExtension_t)%s },' % (n, n))
    else:
        ext_out.append('static %s APIENTRY null_%s( %s ) { %s }' % (ret, n, sig_args, body))
        table.append('\t{ "%s", (GLExtension_t)null_%s },' % (n, n))

src = open(out_cpp + '.in').read()
src = src.replace('@CORE@', '\n'.join(core_out))
src = src.replace('@EXT@', '\n'.join(ext_out))
src = src.replace('@TABLE@', '\n'.join(table))
open(out_cpp, 'w').write(src)
print('core %d, extensions %d' % (len(core_out), len(ext_out)))
