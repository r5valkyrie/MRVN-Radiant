/*
   MRVN-Radiant — Vulkan context API declarations.
   Include this wherever VKContext_* functions are needed.

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

/// Initialise the Vulkan rendering context from a pre-created surface.
/// Must be called after GlobalVulkan().instance is set (done by glwidget_setDefaultFormat).
/// @param surface  Pre-created VkSurfaceKHR obtained from the Qt window.
/// @param width    Initial framebuffer width  (pixels).
/// @param height   Initial framebuffer height (pixels).
void VKContext_create( VkSurfaceKHR surface, uint32_t width, uint32_t height );

/// Destroy all Vulkan context resources (swapchain, device, allocator, …).
/// Calls vkDeviceWaitIdle internally.
void VKContext_destroy();

/// Recreate the swapchain and associated resources after a window resize.
void VKContext_recreateSwapchain( uint32_t newWidth, uint32_t newHeight );

/// Begin recording a single-use command buffer for data upload / layout transitions.
/// Must be paired with VKContext_endTransferCmd().
VkCommandBuffer VKContext_beginTransferCmd();

/// Submit and free the command buffer returned by VKContext_beginTransferCmd().
void VKContext_endTransferCmd( VkCommandBuffer cmd );

/// Acquire the next swapchain image and begin recording a frame command buffer.
/// Returns false if the swapchain is out-of-date (call VKContext_recreateSwapchain).
/// On success *outCmd is ready to record; begin a render pass inside it.
bool VKContext_beginFrame( VkCommandBuffer* outCmd, uint32_t* outImageIndex );

/// Submit the recorded frame and present it.  Advances the internal frame counter.
void VKContext_endFrame( VkCommandBuffer cmd, uint32_t imageIndex );
