/*
   MRVN-Radiant — Vulkan context: device, swapchain, render pass, command buffers.

   Copyright (C) 2001-2006, William Joseph — original GL context (glwidget.cpp).
   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#include "ivkcontext.h"
#include "ivk.h"
#include "vkutil.h"
#include "debugging/debugging.h"
#include "stream/textstream.h"

#include <vector>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t    FRAMES_IN_FLIGHT = 2;
static constexpr VkDeviceSize STREAM_VB_SIZE  = 16u * 1024u * 1024u;   // 16 MB
static constexpr VkDeviceSize STREAM_IB_SIZE  =  8u * 1024u * 1024u;   //  8 MB

// ── Internal context state ────────────────────────────────────────────────────

struct VkCtx
{
	VkSurfaceKHR surface = VK_NULL_HANDLE;

	// Swapchain resources
	std::vector<VkImage>       swapImages;
	std::vector<VkImageView>   swapImageViews;

	// Depth buffer
	VkImage       depthImage = VK_NULL_HANDLE;
	VmaAllocation depthAlloc = VK_NULL_HANDLE;
	VkImageView   depthView  = VK_NULL_HANDLE;

	// Framebuffers (one per swapchain image)
	std::vector<VkFramebuffer> framebuffers;

	// Per-frame command buffers
	std::vector<VkCommandBuffer> cmdBuffers;

	// Double-buffered synchronisation objects
	VkSemaphore imageAvail[FRAMES_IN_FLIGHT] = {};
	VkSemaphore renderDone[FRAMES_IN_FLIGHT] = {};
	VkFence     inFlight  [FRAMES_IN_FLIGHT] = {};
	uint32_t    currentFrame      = 0;
	uint32_t    currentImageIndex = 0;
};

static VkCtx g_ctx;

// ── Error helper ──────────────────────────────────────────────────────────────

static inline void vkCheck( VkResult result, const char* call )
{
	if ( result != VK_SUCCESS )
	{
		ERROR_MESSAGE( "Vulkan error " << static_cast<int>( result ) << " in " << call );
	}
}

#define VK_CHECK( call )  vkCheck( (call), #call )

// ── Queue-family discovery ────────────────────────────────────────────────────

struct QueueFamilyIndices
{
	uint32_t graphics = UINT32_MAX;
	uint32_t present  = UINT32_MAX;

	bool complete() const { return graphics != UINT32_MAX && present != UINT32_MAX; }
};

static QueueFamilyIndices findQueueFamilies( VkPhysicalDevice pd, VkSurfaceKHR surface )
{
	QueueFamilyIndices idx;

	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties( pd, &count, nullptr );
	std::vector<VkQueueFamilyProperties> families( count );
	vkGetPhysicalDeviceQueueFamilyProperties( pd, &count, families.data() );

	for ( uint32_t i = 0; i < count; ++i )
	{
		if ( families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT )
			idx.graphics = i;

		VkBool32 presentSupport = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR( pd, i, surface, &presentSupport );
		if ( presentSupport )
			idx.present = i;

		if ( idx.complete() ) break;
	}

	return idx;
}

// ── Physical device selection ─────────────────────────────────────────────────

static bool deviceSuitable( VkPhysicalDevice pd, VkSurfaceKHR surface )
{
	if ( !findQueueFamilies( pd, surface ).complete() ) return false;

	// Must support VK_KHR_swapchain
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties( pd, nullptr, &extCount, nullptr );
	std::vector<VkExtensionProperties> exts( extCount );
	vkEnumerateDeviceExtensionProperties( pd, nullptr, &extCount, exts.data() );

	bool hasSwapchain = false;
	for ( auto& e : exts )
		if ( strcmp( e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME ) == 0 )
		{ hasSwapchain = true; break; }

	if ( !hasSwapchain ) return false;

	// Must have at least one surface format and present mode
	uint32_t fmtCount = 0, modeCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR( pd, surface, &fmtCount, nullptr );
	vkGetPhysicalDeviceSurfacePresentModesKHR( pd, surface, &modeCount, nullptr );

	return fmtCount > 0 && modeCount > 0;
}

static VkPhysicalDevice pickPhysicalDevice( VkInstance inst, VkSurfaceKHR surface )
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices( inst, &count, nullptr );
	ASSERT_MESSAGE( count > 0, "No Vulkan-capable GPU found" );

	std::vector<VkPhysicalDevice> devices( count );
	vkEnumeratePhysicalDevices( inst, &count, devices.data() );

	VkPhysicalDevice fallback = VK_NULL_HANDLE;
	for ( auto pd : devices )
	{
		if ( !deviceSuitable( pd, surface ) ) continue;
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties( pd, &props );
		if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) return pd;
		fallback = pd;
	}

	ASSERT_MESSAGE( fallback != VK_NULL_HANDLE, "No suitable GPU found" );
	return fallback;
}

// ── Swapchain helpers ─────────────────────────────────────────────────────────

static VkSurfaceFormatKHR chooseSwapFormat( VkPhysicalDevice pd, VkSurfaceKHR surface )
{
	uint32_t count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR( pd, surface, &count, nullptr );
	std::vector<VkSurfaceFormatKHR> formats( count );
	vkGetPhysicalDeviceSurfaceFormatsKHR( pd, surface, &count, formats.data() );

	for ( auto& f : formats )
		if ( f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR )
			return f;

	return formats[0];
}

static VkPresentModeKHR chooseSwapPresentMode( VkPhysicalDevice pd, VkSurfaceKHR surface )
{
	uint32_t count = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR( pd, surface, &count, nullptr );
	std::vector<VkPresentModeKHR> modes( count );
	vkGetPhysicalDeviceSurfacePresentModesKHR( pd, surface, &count, modes.data() );

	// Prefer immediate (no vsync); fall back to FIFO (guaranteed available)
	for ( auto m : modes )
		if ( m == VK_PRESENT_MODE_IMMEDIATE_KHR ) return m;

	return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D chooseSwapExtent( VkPhysicalDevice pd, VkSurfaceKHR surface,
                                    uint32_t w, uint32_t h )
{
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR( pd, surface, &caps );

	if ( caps.currentExtent.width != UINT32_MAX )
		return caps.currentExtent;

	VkExtent2D extent = { w, h };
	extent.width  = std::clamp( extent.width,  caps.minImageExtent.width,  caps.maxImageExtent.width  );
	extent.height = std::clamp( extent.height, caps.minImageExtent.height, caps.maxImageExtent.height );
	return extent;
}

// ── Depth-format selection ────────────────────────────────────────────────────

static VkFormat findDepthFormat( VkPhysicalDevice pd )
{
	static const VkFormat candidates[] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D24_UNORM_S8_UINT,
	};

	for ( auto fmt : candidates )
	{
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties( pd, fmt, &props );
		if ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT )
			return fmt;
	}

	ASSERT_MESSAGE( false, "No suitable depth format available" );
	return VK_FORMAT_UNDEFINED;
}

// ── Image-layout transition (vkutil) ─────────────────────────────────────────

void vkutil_transitionImageLayout( VkCommandBuffer    cmd,
                                   VkImage            image,
                                   VkImageLayout      oldLayout,
                                   VkImageLayout      newLayout,
                                   VkImageAspectFlags aspectMask,
                                   uint32_t           mipLevels,
                                   uint32_t           arrayLayers )
{
	VkImageMemoryBarrier barrier = {};
	barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout           = oldLayout;
	barrier.newLayout           = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = { aspectMask, 0, mipLevels, 0, arrayLayers };

	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

	if ( oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
	     newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL )
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if ( oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
	          newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL )
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else if ( oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
	          newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL )
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else
	{
		ASSERT_MESSAGE( false, "vkutil_transitionImageLayout: unsupported layout transition" );
	}

	vkCmdPipelineBarrier( cmd, srcStage, dstStage, 0,
	                       0, nullptr, 0, nullptr, 1, &barrier );
}

// ── Single-shot transfer command buffer ──────────────────────────────────────

VkCommandBuffer VKContext_beginTransferCmd()
{
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool        = GlobalVulkan().commandPool;
	allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer cmd;
	vkAllocateCommandBuffers( GlobalVulkan().device, &allocInfo, &cmd );

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( cmd, &beginInfo );

	return cmd;
}

void VKContext_endTransferCmd( VkCommandBuffer cmd )
{
	vkEndCommandBuffer( cmd );

	VkSubmitInfo submitInfo = {};
	submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers    = &cmd;

	vkQueueSubmit( GlobalVulkan().graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE );
	vkQueueWaitIdle( GlobalVulkan().graphicsQueue );
	vkFreeCommandBuffers( GlobalVulkan().device, GlobalVulkan().commandPool, 1, &cmd );
}

// ── Swapchain creation ────────────────────────────────────────────────────────

static void createSwapchain( uint32_t w, uint32_t h )
{
	VulkanBinding& vk = GlobalVulkan();

	VkSurfaceFormatKHR surfFmt  = chooseSwapFormat( vk.physDevice, g_ctx.surface );
	VkPresentModeKHR   presMode = chooseSwapPresentMode( vk.physDevice, g_ctx.surface );
	VkExtent2D         extent   = chooseSwapExtent( vk.physDevice, g_ctx.surface, w, h );

	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physDevice, g_ctx.surface, &caps );

	uint32_t imageCount = caps.minImageCount + 1;
	if ( caps.maxImageCount > 0 )
		imageCount = std::min( imageCount, caps.maxImageCount );

	VkSwapchainCreateInfoKHR swapInfo = {};
	swapInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapInfo.surface          = g_ctx.surface;
	swapInfo.minImageCount    = imageCount;
	swapInfo.imageFormat      = surfFmt.format;
	swapInfo.imageColorSpace  = surfFmt.colorSpace;
	swapInfo.imageExtent      = extent;
	swapInfo.imageArrayLayers = 1;
	swapInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	uint32_t queueIndices[] = { vk.graphicsFamily, vk.presentFamily };
	if ( vk.graphicsFamily != vk.presentFamily )
	{
		swapInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
		swapInfo.queueFamilyIndexCount = 2;
		swapInfo.pQueueFamilyIndices   = queueIndices;
	}
	else
	{
		swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	swapInfo.preTransform   = caps.currentTransform;
	swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapInfo.presentMode    = presMode;
	swapInfo.clipped        = VK_TRUE;

	VK_CHECK( vkCreateSwapchainKHR( vk.device, &swapInfo, nullptr, &vk.swapchain ) );
	vk.swapFormat = surfFmt.format;
	vk.swapExtent = extent;

	// Retrieve swapchain images
	vkGetSwapchainImagesKHR( vk.device, vk.swapchain, &imageCount, nullptr );
	g_ctx.swapImages.resize( imageCount );
	vkGetSwapchainImagesKHR( vk.device, vk.swapchain, &imageCount, g_ctx.swapImages.data() );

	// Create image views
	g_ctx.swapImageViews.resize( imageCount );
	for ( uint32_t i = 0; i < imageCount; ++i )
	{
		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image    = g_ctx.swapImages[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format   = vk.swapFormat;
		viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK( vkCreateImageView( vk.device, &viewInfo, nullptr, &g_ctx.swapImageViews[i] ) );
	}
}

// ── Depth buffer ──────────────────────────────────────────────────────────────

static VkImageAspectFlags depthAspectForFormat( VkFormat fmt )
{
	VkImageAspectFlags flags = VK_IMAGE_ASPECT_DEPTH_BIT;
	if ( fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT )
		flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
	return flags;
}

static void createDepthBuffer()
{
	VulkanBinding& vk = GlobalVulkan();
	vk.depthFormat = findDepthFormat( vk.physDevice );

	VkImageCreateInfo imgInfo = {};
	imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imgInfo.imageType     = VK_IMAGE_TYPE_2D;
	imgInfo.format        = vk.depthFormat;
	imgInfo.extent        = { vk.swapExtent.width, vk.swapExtent.height, 1 };
	imgInfo.mipLevels     = 1;
	imgInfo.arrayLayers   = 1;
	imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
	imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocCI = {};
	allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	VK_CHECK( vmaCreateImage( vk.allocator, &imgInfo, &allocCI,
	                          &g_ctx.depthImage, &g_ctx.depthAlloc, nullptr ) );

	VkImageAspectFlags aspect = depthAspectForFormat( vk.depthFormat );
	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image    = g_ctx.depthImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format   = vk.depthFormat;
	viewInfo.subresourceRange = { aspect, 0, 1, 0, 1 };
	VK_CHECK( vkCreateImageView( vk.device, &viewInfo, nullptr, &g_ctx.depthView ) );

	// Transition to depth-stencil attachment layout before first use
	VkCommandBuffer cmd = VKContext_beginTransferCmd();
	vkutil_transitionImageLayout( cmd, g_ctx.depthImage,
	                              VK_IMAGE_LAYOUT_UNDEFINED,
	                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	                              aspect );
	VKContext_endTransferCmd( cmd );
}

// ── Render pass ───────────────────────────────────────────────────────────────

static void createRenderPass()
{
	VulkanBinding& vk = GlobalVulkan();

	VkAttachmentDescription colorAttach = {};
	colorAttach.format         = vk.swapFormat;
	colorAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
	colorAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttach.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentDescription depthAttach = {};
	depthAttach.format         = vk.depthFormat;
	depthAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
	depthAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttach.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttach.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttach.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount    = 1;
	subpass.pColorAttachments       = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	VkSubpassDependency dep = {};
	dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass    = 0;
	dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dep.srcAccessMask = 0;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkAttachmentDescription attachments[] = { colorAttach, depthAttach };
	VkRenderPassCreateInfo rpInfo = {};
	rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpInfo.attachmentCount = 2;
	rpInfo.pAttachments    = attachments;
	rpInfo.subpassCount    = 1;
	rpInfo.pSubpasses      = &subpass;
	rpInfo.dependencyCount = 1;
	rpInfo.pDependencies   = &dep;

	VK_CHECK( vkCreateRenderPass( vk.device, &rpInfo, nullptr, &vk.renderPass ) );
}

// ── Framebuffers ──────────────────────────────────────────────────────────────

static void createFramebuffers()
{
	VulkanBinding& vk = GlobalVulkan();
	g_ctx.framebuffers.resize( g_ctx.swapImageViews.size() );

	for ( size_t i = 0; i < g_ctx.swapImageViews.size(); ++i )
	{
		VkImageView attachments[] = { g_ctx.swapImageViews[i], g_ctx.depthView };

		VkFramebufferCreateInfo fbInfo = {};
		fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbInfo.renderPass      = vk.renderPass;
		fbInfo.attachmentCount = 2;
		fbInfo.pAttachments    = attachments;
		fbInfo.width           = vk.swapExtent.width;
		fbInfo.height          = vk.swapExtent.height;
		fbInfo.layers          = 1;

		VK_CHECK( vkCreateFramebuffer( vk.device, &fbInfo, nullptr, &g_ctx.framebuffers[i] ) );
	}
}

// ── Command pool & buffers ────────────────────────────────────────────────────

static void createCommandPool()
{
	VulkanBinding& vk = GlobalVulkan();

	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = vk.graphicsFamily;
	poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	VK_CHECK( vkCreateCommandPool( vk.device, &poolInfo, nullptr, &vk.commandPool ) );
}

static void allocateCommandBuffers()
{
	VulkanBinding& vk = GlobalVulkan();
	g_ctx.cmdBuffers.resize( g_ctx.swapImages.size() );

	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool        = vk.commandPool;
	allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = static_cast<uint32_t>( g_ctx.cmdBuffers.size() );

	VK_CHECK( vkAllocateCommandBuffers( vk.device, &allocInfo, g_ctx.cmdBuffers.data() ) );
}

// ── Synchronisation objects ───────────────────────────────────────────────────

static void createSyncObjects()
{
	VkDevice device = GlobalVulkan().device;

	VkSemaphoreCreateInfo semInfo   = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo     fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for ( uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i )
	{
		VK_CHECK( vkCreateSemaphore( device, &semInfo,   nullptr, &g_ctx.imageAvail[i] ) );
		VK_CHECK( vkCreateSemaphore( device, &semInfo,   nullptr, &g_ctx.renderDone[i] ) );
		VK_CHECK( vkCreateFence   ( device, &fenceInfo, nullptr, &g_ctx.inFlight[i]   ) );
	}
}

// ── Streaming buffers ─────────────────────────────────────────────────────────

static void createStreamBuffers()
{
	VulkanBinding& vk = GlobalVulkan();

	auto alloc = [&]( VkStreamBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage )
	{
		VkBufferCreateInfo bufInfo = {};
		bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufInfo.size  = size;
		bufInfo.usage = usage;

		VmaAllocationCreateInfo allocCI = {};
		allocCI.usage = VMA_MEMORY_USAGE_AUTO;
		allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		                VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VmaAllocationInfo info = {};
		VK_CHECK( vmaCreateBuffer( vk.allocator, &bufInfo, &allocCI,
		                           &buf.buffer, &buf.alloc, &info ) );
		buf.mapped   = info.pMappedData;
		buf.capacity = size;
	};

	alloc( vk.m_streamVB, STREAM_VB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT );
	alloc( vk.m_streamIB, STREAM_IB_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT  );
}

// ── Public API ────────────────────────────────────────────────────────────────

void VKContext_create( VkSurfaceKHR surface, uint32_t width, uint32_t height )
{
	VulkanBinding& vk = GlobalVulkan();
	g_ctx.surface = surface;

	// ── Physical device ────────────────────────────────────────────────────
	vk.physDevice = pickPhysicalDevice( vk.instance, surface );
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties( vk.physDevice, &props );
		globalOutputStream() << "Vulkan GPU: " << props.deviceName << '\n';
		vk.major_version = VK_VERSION_MAJOR( props.apiVersion );
		vk.minor_version = VK_VERSION_MINOR( props.apiVersion );
	}

	// ── Queue family indices ───────────────────────────────────────────────
	QueueFamilyIndices qi = findQueueFamilies( vk.physDevice, surface );
	vk.graphicsFamily = qi.graphics;
	vk.presentFamily  = qi.present;

	// ── Physical features ──────────────────────────────────────────────────
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties( vk.physDevice, &props );
	vk.maxAnisotropy = props.limits.maxSamplerAnisotropy;

	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures( vk.physDevice, &features );
	vk.support_texture_compression_bc = features.textureCompressionBC == VK_TRUE;

	// ── Logical device ─────────────────────────────────────────────────────
	std::vector<VkDeviceQueueCreateInfo> queueCIs;
	std::vector<uint32_t> uniqueFamilies = { qi.graphics };
	if ( qi.present != qi.graphics ) uniqueFamilies.push_back( qi.present );

	float priority = 1.0f;
	for ( uint32_t family : uniqueFamilies )
	{
		VkDeviceQueueCreateInfo qci = {};
		qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = family;
		qci.queueCount       = 1;
		qci.pQueuePriorities = &priority;
		queueCIs.push_back( qci );
	}

	const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

	VkPhysicalDeviceFeatures enabledFeatures = {};
	enabledFeatures.samplerAnisotropy    = features.samplerAnisotropy;
	enabledFeatures.textureCompressionBC = features.textureCompressionBC;
	enabledFeatures.fillModeNonSolid     = features.fillModeNonSolid;
	enabledFeatures.wideLines            = features.wideLines;

	VkDeviceCreateInfo devInfo = {};
	devInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	devInfo.queueCreateInfoCount    = static_cast<uint32_t>( queueCIs.size() );
	devInfo.pQueueCreateInfos       = queueCIs.data();
	devInfo.enabledExtensionCount   = 1;
	devInfo.ppEnabledExtensionNames = devExts;
	devInfo.pEnabledFeatures        = &enabledFeatures;

	VK_CHECK( vkCreateDevice( vk.physDevice, &devInfo, nullptr, &vk.device ) );
	vkGetDeviceQueue( vk.device, qi.graphics, 0, &vk.graphicsQueue );
	vkGetDeviceQueue( vk.device, qi.present,  0, &vk.presentQueue  );

	// ── VMA allocator ──────────────────────────────────────────────────────
	VmaAllocatorCreateInfo vmaInfo = {};
	vmaInfo.physicalDevice   = vk.physDevice;
	vmaInfo.device           = vk.device;
	vmaInfo.instance         = vk.instance;
	vmaInfo.vulkanApiVersion = VK_API_VERSION_1_2;
	VK_CHECK( vmaCreateAllocator( &vmaInfo, &vk.allocator ) );

	// ── Command pool ───────────────────────────────────────────────────────
	createCommandPool();

	// ── Swapchain + depth + render-pass + framebuffers ─────────────────────
	createSwapchain( width, height );
	createDepthBuffer();
	createRenderPass();
	createFramebuffers();
	allocateCommandBuffers();
	createSyncObjects();
	createStreamBuffers();

	globalOutputStream() << "Vulkan context ready ("
	                     << width << 'x' << height << ")\n";
}

void VKContext_destroy()
{
	VulkanBinding& vk = GlobalVulkan();
	if ( vk.device == VK_NULL_HANDLE ) return;

	vkDeviceWaitIdle( vk.device );

	// Stream buffers
	if ( vk.m_streamVB.buffer ) vmaDestroyBuffer( vk.allocator, vk.m_streamVB.buffer, vk.m_streamVB.alloc );
	if ( vk.m_streamIB.buffer ) vmaDestroyBuffer( vk.allocator, vk.m_streamIB.buffer, vk.m_streamIB.alloc );
	vk.m_streamVB = {};
	vk.m_streamIB = {};

	// Sync objects
	for ( uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i )
	{
		vkDestroySemaphore( vk.device, g_ctx.imageAvail[i], nullptr );
		vkDestroySemaphore( vk.device, g_ctx.renderDone[i], nullptr );
		vkDestroyFence    ( vk.device, g_ctx.inFlight[i],   nullptr );
	}

	vkDestroyCommandPool( vk.device, vk.commandPool, nullptr );

	for ( auto fb : g_ctx.framebuffers    ) vkDestroyFramebuffer( vk.device, fb, nullptr );
	for ( auto iv : g_ctx.swapImageViews  ) vkDestroyImageView  ( vk.device, iv, nullptr );

	vkDestroyImageView( vk.device, g_ctx.depthView, nullptr );
	vmaDestroyImage( vk.allocator, g_ctx.depthImage, g_ctx.depthAlloc );

	vkDestroyRenderPass  ( vk.device, vk.renderPass, nullptr );
	vkDestroySwapchainKHR( vk.device, vk.swapchain,  nullptr );
	vmaDestroyAllocator  ( vk.allocator );
	vkDestroyDevice      ( vk.device,   nullptr );
	vkDestroySurfaceKHR  ( vk.instance, g_ctx.surface, nullptr );

	g_ctx = VkCtx{};
	vk    = VulkanBinding{};
}

void VKContext_recreateSwapchain( uint32_t newWidth, uint32_t newHeight )
{
	VulkanBinding& vk = GlobalVulkan();
	vkDeviceWaitIdle( vk.device );

	for ( auto fb : g_ctx.framebuffers    ) vkDestroyFramebuffer( vk.device, fb, nullptr );
	for ( auto iv : g_ctx.swapImageViews  ) vkDestroyImageView  ( vk.device, iv, nullptr );
	g_ctx.framebuffers.clear();
	g_ctx.swapImageViews.clear();
	g_ctx.swapImages.clear();

	vkDestroyImageView   ( vk.device, g_ctx.depthView,  nullptr );
	vmaDestroyImage      ( vk.allocator, g_ctx.depthImage, g_ctx.depthAlloc );
	vkDestroyRenderPass  ( vk.device, vk.renderPass, nullptr );
	vkDestroySwapchainKHR( vk.device, vk.swapchain,  nullptr );

	createSwapchain( newWidth, newHeight );
	createDepthBuffer();
	createRenderPass();
	createFramebuffers();
}

bool VKContext_beginFrame( VkCommandBuffer* outCmd, uint32_t* outImageIndex )
{
	VulkanBinding& vk    = GlobalVulkan();
	uint32_t       frame = g_ctx.currentFrame;

	vkWaitForFences( vk.device, 1, &g_ctx.inFlight[frame], VK_TRUE, UINT64_MAX );

	VkResult res = vkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX,
	                                       g_ctx.imageAvail[frame],
	                                       VK_NULL_HANDLE,
	                                       &g_ctx.currentImageIndex );

	if ( res == VK_ERROR_OUT_OF_DATE_KHR ) return false;

	vkResetFences( vk.device, 1, &g_ctx.inFlight[frame] );

	VkCommandBuffer cmd = g_ctx.cmdBuffers[g_ctx.currentImageIndex];
	vkResetCommandBuffer( cmd, 0 );

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer( cmd, &beginInfo );

	*outCmd      = cmd;
	*outImageIndex = g_ctx.currentImageIndex;
	return true;
}

void VKContext_endFrame( VkCommandBuffer cmd, uint32_t imageIndex )
{
	VulkanBinding& vk    = GlobalVulkan();
	uint32_t       frame = g_ctx.currentFrame;

	vkEndCommandBuffer( cmd );

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo = {};
	submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount   = 1;
	submitInfo.pWaitSemaphores      = &g_ctx.imageAvail[frame];
	submitInfo.pWaitDstStageMask    = &waitStage;
	submitInfo.commandBufferCount   = 1;
	submitInfo.pCommandBuffers      = &cmd;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores    = &g_ctx.renderDone[frame];

	VK_CHECK( vkQueueSubmit( vk.graphicsQueue, 1, &submitInfo, g_ctx.inFlight[frame] ) );

	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores    = &g_ctx.renderDone[frame];
	presentInfo.swapchainCount     = 1;
	presentInfo.pSwapchains        = &vk.swapchain;
	presentInfo.pImageIndices      = &imageIndex;

	vkQueuePresentKHR( vk.presentQueue, &presentInfo );  // SUBOPTIMAL is non-fatal
	g_ctx.currentFrame = ( frame + 1 ) % FRAMES_IN_FLIGHT;
}
