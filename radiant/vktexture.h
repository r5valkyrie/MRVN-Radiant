/*
   vktexture.h — Vulkan texture table: image + view + sampler per texture slot.

   qtexture_t::texture_number is now an index into g_vkTextures[].
   Index 0 is reserved as "invalid / not loaded".

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>

// ── VkTexture ─────────────────────────────────────────────────────────────────

struct VkTexture
{
	VkImage       image     = VK_NULL_HANDLE;
	VmaAllocation alloc     = VK_NULL_HANDLE;
	VkImageView   view      = VK_NULL_HANDLE;
	VkSampler     sampler   = VK_NULL_HANDLE;
	uint32_t      width     = 0;
	uint32_t      height    = 0;
	uint32_t      mipLevels = 1;
	bool          isCubeMap = false;
	bool          valid     = false;
};

// ── Global texture table ──────────────────────────────────────────────────────

/// All loaded textures. Index 0 is reserved (invalid).
extern std::vector<VkTexture> g_vkTextures;

// ── API ───────────────────────────────────────────────────────────────────────

/// Texture filter modes (match ETexturesMode in textures.cpp).
enum VkTexFilter : int
{
	VK_TEX_NEAREST                = 0,
	VK_TEX_NEAREST_MIPMAP_NEAREST = 1,
	VK_TEX_NEAREST_MIPMAP_LINEAR  = 2,
	VK_TEX_LINEAR                 = 3,
	VK_TEX_LINEAR_MIPMAP_NEAREST  = 4,
	VK_TEX_LINEAR_MIPMAP_LINEAR   = 5,
};

/// Upload an RGBA 2-D texture and return its table index (>0 on success).
uint32_t VKTexture_create2D( const unsigned char* rgba,
                             uint32_t             width,
                             uint32_t             height,
                             VkTexFilter          filter,
                             bool                 anisotropy );

/// Upload a cube-map texture from six RGBA face images (same width x height).
/// Face order: +X, -X, +Y, -Y, +Z, -Z.
uint32_t VKTexture_createCubeMap( const unsigned char* const faces[6],
                                  uint32_t                   faceSize );

/// Free a single texture slot and set its entry to invalid.
void VKTexture_destroy( uint32_t index );

/// Destroy all textures (called at context teardown).
void VKTexture_destroyAll();

/// Return the VkTexture at the given index (asserts index is valid).
const VkTexture& VKTexture_get( uint32_t index );
