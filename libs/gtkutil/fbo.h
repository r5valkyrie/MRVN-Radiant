/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

/// Stub FBO class for the Vulkan migration.
/// In Vulkan, framebuffer objects are replaced by render passes + VkFramebuffer.
/// This stub maintains the same interface so call-sites compile while Phase 6
/// wires up the real Vulkan render-pass framebuffer.
class FBO
{
public:
	const int m_samples;

	FBO( int /*w*/, int /*h*/, bool /*hasDepth*/, int samples ) : m_samples( samples )
	{
		// Phase 6: create VkFramebuffer with matching dimensions and sample count.
	}
	FBO( FBO&& ) noexcept = delete;
	~FBO(){
		// Phase 6: destroy VkFramebuffer.
	}
	bool bind(){
		// Phase 6: begin render pass.
		return true;
	}
	bool release(){
		// Phase 6: end render pass.
		return true;
	}
	void blit(){
		// Phase 6: resolve MSAA via vkCmdResolveImage or a blit subpass.
	}
};
