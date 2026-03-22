/*
   iglrender.h — Render-state types.

   GL-specific types (GLenum, GLint, GLfloat, …) have been replaced with plain
   C++ types so that this header no longer depends on any OpenGL or Vulkan
   headers directly.

   Copyright (C) 2001-2006, William Joseph — original file.
   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

#include <cstdint>
#include "generic/vector.h"

class AABB;
class Matrix4;

// ── Fog-mode identifiers (no longer GL enums) ──────────────────────────────

enum RsFogMode : uint32_t
{
	RS_FOG_NONE = 0,
	RS_FOG_EXP  = 1,
	RS_FOG_EXP2 = 2,
	RS_FOG_LIN  = 3,
};

// ── Blend-factor identifiers (VkBlendFactor values used directly) ────────────
// Values must match VkBlendFactor enum (vulkan/vulkan_core.h).
static constexpr uint32_t RS_BLEND_ZERO                  = 1u;  // VK_BLEND_FACTOR_ZERO
static constexpr uint32_t RS_BLEND_ONE                   = 2u;  // VK_BLEND_FACTOR_ONE
static constexpr uint32_t RS_BLEND_SRC_ALPHA             = 6u;  // VK_BLEND_FACTOR_SRC_ALPHA
static constexpr uint32_t RS_BLEND_ONE_MINUS_SRC_ALPHA   = 7u;  // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
static constexpr uint32_t RS_BLEND_DST_COLOR             = 4u;  // VK_BLEND_FACTOR_DST_COLOR
static constexpr uint32_t RS_BLEND_ONE_MINUS_DST_COLOR   = 5u;  // VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
static constexpr uint32_t RS_BLEND_SRC_COLOR             = 2u;  // VK_BLEND_FACTOR_SRC_COLOR (same as ONE — use with care)

// ── Compare-op identifiers (VkCompareOp values used directly) ───────────────
// Values must match VkCompareOp enum (vulkan/vulkan_core.h).
static constexpr uint32_t RS_COMPARE_NEVER               = 0u;  // VK_COMPARE_OP_NEVER
static constexpr uint32_t RS_COMPARE_LESS                = 1u;  // VK_COMPARE_OP_LESS
static constexpr uint32_t RS_COMPARE_EQUAL               = 2u;  // VK_COMPARE_OP_EQUAL
static constexpr uint32_t RS_COMPARE_LESS_OR_EQUAL       = 3u;  // VK_COMPARE_OP_LESS_OR_EQUAL
static constexpr uint32_t RS_COMPARE_GREATER             = 4u;  // VK_COMPARE_OP_GREATER
static constexpr uint32_t RS_COMPARE_ALWAYS              = 7u;  // VK_COMPARE_OP_ALWAYS

// ── GLProgram — abstract Vulkan "program" interface ─────────────────────────

class GLProgram
{
public:
	virtual void enable() = 0;
	virtual void disable() = 0;
	virtual void setParameters( const Vector3& viewer,
	                            const Matrix4& localToWorld,
	                            const Vector3& origin,
	                            const Vector3& colour,
	                            const Matrix4& world2light ) = 0;
	virtual ~GLProgram() = default;
};

// ── OpenGLFogState ────────────────────────────────────────────────────────────

class OpenGLFogState
{
public:
	OpenGLFogState()
		: mode( RS_FOG_EXP ), density( 0.0f ), start( 0.0f ), end( 0.0f ),
		  index( 0 ), colour( 1, 1, 1, 1 ){}

	uint32_t mode;              ///< RsFogMode value
	float    density;
	float    start;
	float    end;
	int32_t  index;
	Vector4  colour;
};

// ── OpenGLState — render-state key / pipeline descriptor ───────────────────

class OpenGLState
{
public:
	enum ESort
	{
		eSortFirst        = 0,
		eSortOpaque       = 1,
		eSortMultiFirst   = 2,
		eSortMultiLast    = 1023,
		eSortOverbrighten = 1024,
		eSortFullbright   = 1025,
		eSortTranslucent  = 1026,
		eSortHighlight    = 1027,
		eSortOverlayFirst = 1028,
		eSortOverlayLast  = 2047,
		eSortText         = 2048,
		eSortControlFirst = 2050,
		eSortControlLast  = 3071,
		eSortGUI0         = 3072,
		eSortGUI1         = 3073,
		eSortLast         = 4096,
	};

	unsigned int m_state;                               ///< RENDER_* bitmask
	std::size_t  m_sort;                                ///< ESort value

	/// Texture table indices (0 = not bound)
	uint32_t m_texture;
	uint32_t m_texture1, m_texture2, m_texture3, m_texture4;
	uint32_t m_texture5, m_texture6, m_texture7;
	uint32_t m_textureSkyBox;

	Vector4  m_colour;

	uint32_t m_blend_src, m_blend_dst;  ///< RS_BLEND_* (== VkBlendFactor)
	uint32_t m_depthfunc;               ///< RS_COMPARE_* (== VkCompareOp)
	uint32_t m_alphafunc;               ///< RS_COMPARE_* for alpha test
	float    m_alpharef;

	float    m_linewidth;
	float    m_pointsize;
	int32_t  m_linestipple_factor;
	uint16_t m_linestipple_pattern;

	OpenGLFogState m_fog;
	GLProgram*     m_program;

	OpenGLState() : m_program( nullptr ){}
};

// ── OpenGLStateLibrary ────────────────────────────────────────────────────────

class OpenGLStateLibrary
{
public:
	INTEGER_CONSTANT( Version, 1 );
	STRING_CONSTANT( Name, "openglshaderlibrary" );

	virtual void getDefaultState( OpenGLState& state ) const = 0;
	virtual void insert( const char* name, const OpenGLState& state ) = 0;
	virtual void erase( const char* name ) = 0;
};

#include "modulesystem.h"

template<typename Type> class GlobalModule;
typedef GlobalModule<OpenGLStateLibrary> GlobalOpenGLStateLibraryModule;

template<typename Type> class GlobalModuleRef;
typedef GlobalModuleRef<OpenGLStateLibrary> GlobalOpenGLStateLibraryModuleRef;

inline OpenGLStateLibrary& GlobalOpenGLStateLibrary()
{
	return GlobalOpenGLStateLibraryModule::getTable();
}
