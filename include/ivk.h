/*
   MRVN-Radiant — Vulkan binding header.
   Replaces the old OpenGL binding (include/igl.h).

   Copyright (C) 2001-2006, William Joseph — original GL binding.
   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "generic/constant.h"
#include "gtkutil/glfont.h"

// ── Streaming buffer ─────────────────────────────────────────────────────────

/// Persistently-mapped streaming buffer for per-frame vertex / index uploads.
/// Mirrors the old GL buffer-orphaning pattern (GL_STREAM_DRAW).
struct VkStreamBuffer
{
	VkBuffer      buffer   = VK_NULL_HANDLE;
	VmaAllocation alloc    = VK_NULL_HANDLE;
	void*         mapped   = nullptr;       ///< CPU-accessible persistent mapping
	VkDeviceSize  capacity = 0;             ///< Allocated size in bytes
};

// ── VulkanBinding ────────────────────────────────────────────────────────────

/// \brief Global Vulkan binding — owns all per-process Vulkan state.
/// Accessed via GlobalVulkan().  Replaces the old OpenGLBinding / gl().
struct VulkanBinding
{
	INTEGER_CONSTANT( Version, 2 );
	STRING_CONSTANT( Name, "qgl" );         ///< Module-registry key (kept for compat)

	int  major_version = 1;
	int  minor_version = 2;

	/// True once the Vulkan context has been fully initialised.
	bool contextValid = false;

	// ── Core Vulkan handles ─────────────────────────────────────────────────
	VkInstance       instance       = VK_NULL_HANDLE;
	VkPhysicalDevice physDevice     = VK_NULL_HANDLE;
	VkDevice         device         = VK_NULL_HANDLE;
	VkQueue          graphicsQueue  = VK_NULL_HANDLE;
	VkQueue          presentQueue   = VK_NULL_HANDLE;
	uint32_t         graphicsFamily = UINT32_MAX;
	uint32_t         presentFamily  = UINT32_MAX;

	// ── Memory allocator ────────────────────────────────────────────────────
	VmaAllocator allocator = VK_NULL_HANDLE;

	// ── Swapchain ───────────────────────────────────────────────────────────
	VkSwapchainKHR swapchain  = VK_NULL_HANDLE;
	VkFormat       swapFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D     swapExtent = { 0, 0 };

	// ── Main render pass ───────────────────────────────────────────────────
	VkRenderPass renderPass  = VK_NULL_HANDLE;
	VkFormat     depthFormat = VK_FORMAT_UNDEFINED;

	// ── Command pool ───────────────────────────────────────────────────────
	VkCommandPool commandPool = VK_NULL_HANDLE;

	// ── Streaming per-frame buffers ────────────────────────────────────────
	VkStreamBuffer m_streamVB;      ///< Streaming vertex buffer  (CPU-write / GPU-read)
	VkStreamBuffer m_streamIB;      ///< Streaming index buffer   (CPU-write / GPU-read)

	// ── Text rendering ─────────────────────────────────────────────────────
	GLFont* m_font = nullptr;

	/// Render a string at the current raster position (unchanged from GL era).
	void drawString( const char* s ) const
	{
		if ( m_font ) m_font->printString( s );
	}
	void drawChar( char c ) const
	{
		char buf[2] = { c, '\0' };
		drawString( buf );
	}

	// ── Feature support ────────────────────────────────────────────────────
	bool  support_texture_compression_bc = false;   ///< BC1-BC7 (equivalent of S3TC)
	float maxAnisotropy                  = 1.0f;

	// ── Debug / error checking ─────────────────────────────────────────────
	void ( *assertNoErrors )( const char* file, int line ) = nullptr;
};

// ── Module-system integration ─────────────────────────────────────────────────

#include "modulesystem.h"

template<typename Type> class GlobalModule;
typedef GlobalModule<VulkanBinding> GlobalVulkanModule;

template<typename Type> class GlobalModuleRef;
typedef GlobalModuleRef<VulkanBinding> GlobalVulkanModuleRef;

inline VulkanBinding& GlobalVulkan()
{
	return GlobalVulkanModule::getTable();
}
