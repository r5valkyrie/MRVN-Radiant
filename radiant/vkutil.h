/*
   MRVN-Radiant — Vulkan layout-transition helper (internal header).

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

/// Record an image-layout transition barrier into @p cmd.
/// Supported transitions:
///   UNDEFINED → TRANSFER_DST_OPTIMAL
///   TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
///   UNDEFINED → DEPTH_STENCIL_ATTACHMENT_OPTIMAL
void vkutil_transitionImageLayout( VkCommandBuffer cmd,
                                   VkImage         image,
                                   VkImageLayout   oldLayout,
                                   VkImageLayout   newLayout,
                                   VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   uint32_t           mipLevels  = 1,
                                   uint32_t           arrayLayers = 1 );
