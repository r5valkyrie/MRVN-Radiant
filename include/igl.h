/*
   igl.h — backward-compatibility shim for the OpenGL → Vulkan migration.

   The QOpenGLFunctions_2_0 binding has been replaced by VulkanBinding (ivk.h).
   All code that previously called   gl().glXxx(…)   must be migrated to the
   Vulkan API or to the higher-level helpers in vkcontext / vktexture.

   Copyright (C) 2001-2006, William Joseph — original file.
   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

#include "ivk.h"

// ── Type aliases kept for code still referencing the old names ──────────────────

using OpenGLBinding         = VulkanBinding;
using GlobalOpenGLModule    = GlobalVulkanModule;
using GlobalOpenGLModuleRef = GlobalVulkanModuleRef;

/// Returns the global Vulkan binding (alias for GlobalVulkan()).
inline VulkanBinding& GlobalOpenGL()
{
	return GlobalVulkan();
}

// NOTE: gl() has been intentionally removed.
// Replace every  gl().glXxx(…)  call with the corresponding Vulkan API.
// Files that still call gl() will fail to compile until migrated (Phase 5–7).

#if defined( _DEBUG )
#  define GlobalOpenGL_debugAssertNoErrors() \
     do { if ( GlobalVulkan().assertNoErrors ) \
              GlobalVulkan().assertNoErrors( __FILE__, __LINE__ ); } while ( 0 )
#else
#  define GlobalOpenGL_debugAssertNoErrors()
#endif

// ── GL type aliases for code still using GL type names during migration ───────
// These are the standard OpenGL typedefs (from <GL/gl.h>) reproduced here so
// that files which include igl.h but no longer include <GL/gl.h> continue to
// compile.  They are plain integer/float aliases — no OpenGL library needed.
#ifndef GL_TYPES_DEFINED
#define GL_TYPES_DEFINED
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;
#endif // GL_TYPES_DEFINED

// ── GL constant aliases used during migration ─────────────────────────────────
// Only the constants actually referenced from non-GL-aware compilation units.
#ifndef GL_ZERO
#  define GL_ZERO                    0
#  define GL_ONE                     1
#  define GL_SRC_COLOR               0x0300
#  define GL_ONE_MINUS_SRC_COLOR     0x0301
#  define GL_SRC_ALPHA               0x0302
#  define GL_ONE_MINUS_SRC_ALPHA     0x0303
#  define GL_DST_COLOR               0x0306
#  define GL_ONE_MINUS_DST_COLOR     0x0307
#  define GL_DST_ALPHA               0x0304
#  define GL_ONE_MINUS_DST_ALPHA     0x0305

#  define GL_NEVER                   0x0200
#  define GL_LESS                    0x0201
#  define GL_EQUAL                   0x0202
#  define GL_LEQUAL                  0x0203
#  define GL_GREATER                 0x0204
#  define GL_NOTEQUAL                0x0205
#  define GL_GEQUAL                  0x0206
#  define GL_ALWAYS                  0x0207

#  define GL_POINTS                  0x0000
#  define GL_LINES                   0x0001
#  define GL_LINE_LOOP               0x0002
#  define GL_LINE_STRIP              0x0003
#  define GL_TRIANGLES               0x0004
#  define GL_TRIANGLE_STRIP          0x0005
#  define GL_TRIANGLE_FAN            0x0006
#  define GL_QUADS                   0x0007
#  define GL_QUAD_STRIP              0x0008
#  define GL_POLYGON                 0x0009

#  define GL_FLOAT                   0x1406
#  define GL_UNSIGNED_BYTE           0x1401
#  define GL_UNSIGNED_INT            0x1405
#  define GL_DOUBLE                  0x140A
#  define GL_ARRAY_BUFFER            0x8892

#  define GL_TEXTURE0                0x84C0
#  define GL_TEXTURE1                0x84C1
#  define GL_TEXTURE2                0x84C2
#  define GL_TEXTURE3                0x84C3
#  define GL_TEXTURE4                0x84C4
#  define GL_TEXTURE5                0x84C5
#  define GL_TEXTURE6                0x84C6
#  define GL_TEXTURE7                0x84C7

#  define GL_TEXTURE_2D              0x0DE1
#  define GL_TEXTURE_CUBE_MAP        0x8513
#  define GL_TEXTURE_WRAP_S          0x2802
#  define GL_TEXTURE_WRAP_T          0x2803
#  define GL_CLAMP_TO_BORDER         0x812D
#  define GL_CLAMP_TO_EDGE           0x812F

#  define GL_DEPTH_TEST              0x0B71
#  define GL_BLEND                   0x0BE2
#  define GL_CULL_FACE               0x0B44
#  define GL_LIGHTING                0x0B50
#  define GL_ALPHA_TEST              0x0BC0
#  define GL_FOG                     0x0B60
#  define GL_LINE_STIPPLE            0x0B24
#  define GL_POLYGON_STIPPLE         0x0B42
#  define GL_POLYGON_OFFSET_LINE     0x2A02
#  define GL_NORMALIZE               0x0BA1

#  define GL_PROJECTION              0x1701
#  define GL_MODELVIEW               0x1700

#  define GL_VIEWPORT                0x0BA2

#  define GL_CW                      0x0900
#  define GL_CCW                     0x0901

#  define GL_FRONT_AND_BACK          0x0408
#  define GL_FILL                    0x1B02
#  define GL_LINE                    0x1B01

#  define GL_FLAT                    0x1D00
#  define GL_SMOOTH                  0x1D01

#  define GL_COLOR_MATERIAL          0x0B57
#  define GL_RESCALE_NORMAL          0x803A
#  define GL_NORMAL_ARRAY            0x8075
#  define GL_COLOR_ARRAY             0x8076

#  define GL_DEPTH_WRITEMASK         0x0B72
#  define GL_SAMPLES_PASSED          0x8914
#  define GL_QUERY_RESULT            0x8866

#  define GL_FOG_MODE                0x0B65
#  define GL_LINEAR                  0x2601
#  define GL_EXP                     0x0800
#  define GL_EXP2                    0x0801
#  define GL_FOG_START               0x0B63
#  define GL_FOG_END                 0x0B64
#  define GL_FOG_DENSITY             0x0B62
#  define GL_FOG_COLOR               0x0B66
#endif // GL_ZERO

