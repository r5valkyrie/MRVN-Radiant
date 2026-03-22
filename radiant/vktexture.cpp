/*
   vktexture.cpp — Vulkan texture upload, mipmap generation, sampler creation.

   Licensed under the GNU General Public License v2 or later.
 */

#include "vktexture.h"
#include "ivk.h"
#include "ivkcontext.h"
#include "vkutil.h"
#include "debugging/debugging.h"
#include "stream/textstream.h"

#include <cstring>
#include <cmath>
#include <algorithm>

// ── Global texture table ──────────────────────────────────────────────────────

std::vector<VkTexture> g_vkTextures;

static void ensureReservedSlot()
{
	if ( g_vkTextures.empty() )
	{
		g_vkTextures.emplace_back(); // slot 0 = invalid
		g_vkTextures[0].valid = false;
	}
}

// ── Error check ───────────────────────────────────────────────────────────────

static inline void vkCheck( VkResult r, const char* call )
{
	if ( r != VK_SUCCESS )
		ERROR_MESSAGE( "VkTexture error " << static_cast<int>( r ) << " in " << call );
}
#define VK_CHECK( call )  vkCheck( (call), #call )

// ── Sampler creation ─────────────────────────────────────────────────────────

static VkSampler createSampler( VkTexFilter filter, bool anisotropy, bool cubemap, uint32_t mipLevels )
{
	VulkanBinding& vk = GlobalVulkan();

	VkFilter          magFilter = VK_FILTER_LINEAR;
	VkFilter          minFilter = VK_FILTER_LINEAR;
	VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	float             maxLod   = static_cast<float>( mipLevels );

	switch ( filter )
	{
	case VK_TEX_NEAREST:
		magFilter  = VK_FILTER_NEAREST;
		minFilter  = VK_FILTER_NEAREST;
		mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		maxLod     = 0.0f;
		break;
	case VK_TEX_NEAREST_MIPMAP_NEAREST:
		magFilter  = VK_FILTER_NEAREST;
		minFilter  = VK_FILTER_NEAREST;
		mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		break;
	case VK_TEX_NEAREST_MIPMAP_LINEAR:
		magFilter  = VK_FILTER_NEAREST;
		minFilter  = VK_FILTER_NEAREST;
		mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		break;
	case VK_TEX_LINEAR:
		maxLod = 0.0f;
		break;
	case VK_TEX_LINEAR_MIPMAP_NEAREST:
		mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		break;
	case VK_TEX_LINEAR_MIPMAP_LINEAR:
	default:
		break;
	}

	VkSamplerCreateInfo si = {};
	si.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	si.magFilter        = magFilter;
	si.minFilter        = minFilter;
	si.mipmapMode       = mipmapMode;
	si.addressModeU     = cubemap ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	si.addressModeV     = si.addressModeU;
	si.addressModeW     = si.addressModeU;
	si.mipLodBias       = 0.0f;
	si.minLod           = 0.0f;
	si.maxLod           = maxLod;
	si.anisotropyEnable = ( anisotropy && vk.maxAnisotropy > 1.0f ) ? VK_TRUE : VK_FALSE;
	si.maxAnisotropy    = si.anisotropyEnable ? vk.maxAnisotropy : 1.0f;
	si.compareEnable    = VK_FALSE;
	si.unnormalizedCoordinates = VK_FALSE;

	VkSampler sampler;
	VK_CHECK( vkCreateSampler( vk.device, &si, nullptr, &sampler ) );
	return sampler;
}

// ── Staging-buffer helper ─────────────────────────────────────────────────────

struct StagingBuffer
{
	VkBuffer      buffer = VK_NULL_HANDLE;
	VmaAllocation alloc  = VK_NULL_HANDLE;
};

static StagingBuffer createStaging( const void* data, VkDeviceSize size )
{
	VulkanBinding& vk = GlobalVulkan();
	StagingBuffer  sb;

	VkBufferCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	ci.size  = size;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
	           VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo info;
	VK_CHECK( vmaCreateBuffer( vk.allocator, &ci, &ai, &sb.buffer, &sb.alloc, &info ) );
	std::memcpy( info.pMappedData, data, static_cast<std::size_t>( size ) );
	return sb;
}

static void destroyStaging( StagingBuffer& sb )
{
	vmaDestroyBuffer( GlobalVulkan().allocator, sb.buffer, sb.alloc );
	sb = {};
}

// ── Mipmap count ─────────────────────────────────────────────────────────────

static uint32_t calcMipLevels( uint32_t w, uint32_t h )
{
	return static_cast<uint32_t>( std::floor( std::log2( std::max( w, h ) ) ) ) + 1u;
}

// ── 2-D texture upload ────────────────────────────────────────────────────────

uint32_t VKTexture_create2D( const unsigned char* rgba,
                             uint32_t             width,
                             uint32_t             height,
                             VkTexFilter          filter,
                             bool                 anisotropy )
{
	ASSERT_MESSAGE( GlobalVulkan().contextValid, "VKTexture_create2D: no Vulkan context" );

	VulkanBinding& vk       = GlobalVulkan();
	uint32_t       mipLevels = calcMipLevels( width, height );
	const VkDeviceSize dataSize = static_cast<VkDeviceSize>( width ) * height * 4;

	// ── Create GPU image ──────────────────────────────────────────────────
	VkImageCreateInfo ici = {};
	ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType     = VK_IMAGE_TYPE_2D;
	ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent        = { width, height, 1 };
	ici.mipLevels     = mipLevels;
	ici.arrayLayers   = 1;
	ici.samples       = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
	ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
	                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocCI = {};
	allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	VkTexture tex;
	tex.width     = width;
	tex.height    = height;
	tex.mipLevels = mipLevels;
	tex.isCubeMap = false;

	VK_CHECK( vmaCreateImage( vk.allocator, &ici, &allocCI, &tex.image, &tex.alloc, nullptr ) );

	// ── Upload base mip via staging ───────────────────────────────────────
	StagingBuffer staging = createStaging( rgba, dataSize );

	VkCommandBuffer cmd = VKContext_beginTransferCmd();

	vkutil_transitionImageLayout( cmd, tex.image,
	                              VK_IMAGE_LAYOUT_UNDEFINED,
	                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                              VK_IMAGE_ASPECT_COLOR_BIT, mipLevels );

	VkBufferImageCopy copy = {};
	copy.bufferOffset      = 0;
	copy.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copy.imageExtent       = { width, height, 1 };
	vkCmdCopyBufferToImage( cmd, staging.buffer, tex.image,
	                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );

	// ── Generate mipmaps via vkCmdBlitImage ───────────────────────────────
	int32_t mipW = static_cast<int32_t>( width );
	int32_t mipH = static_cast<int32_t>( height );

	for ( uint32_t i = 1; i < mipLevels; ++i )
	{
		// Transition mip i-1 from TRANSFER_DST to TRANSFER_SRC
		VkImageMemoryBarrier bar = {};
		bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		bar.image               = tex.image;
		bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1 };
		bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		bar.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier( cmd,
		                       VK_PIPELINE_STAGE_TRANSFER_BIT,
		                       VK_PIPELINE_STAGE_TRANSFER_BIT,
		                       0, 0, nullptr, 0, nullptr, 1, &bar );

		// Blit mip i-1 → mip i
		int32_t nextW = std::max( mipW / 2, 1 );
		int32_t nextH = std::max( mipH / 2, 1 );

		VkImageBlit blit = {};
		blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
		blit.srcOffsets[0]  = { 0, 0, 0 };
		blit.srcOffsets[1]  = { mipW, mipH, 1 };
		blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
		blit.dstOffsets[0]  = { 0, 0, 0 };
		blit.dstOffsets[1]  = { nextW, nextH, 1 };

		vkCmdBlitImage( cmd,
		                tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                1, &blit, VK_FILTER_LINEAR );

		// Transition mip i-1 to SHADER_READ_ONLY
		bar.oldLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		bar.newLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier( cmd,
		                       VK_PIPELINE_STAGE_TRANSFER_BIT,
		                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                       0, 0, nullptr, 0, nullptr, 1, &bar );

		mipW = nextW;
		mipH = nextH;
	}

	// Transition last mip to SHADER_READ_ONLY
	VkImageMemoryBarrier lastBar = {};
	lastBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	lastBar.image               = tex.image;
	lastBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	lastBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	lastBar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevels - 1, 1, 0, 1 };
	lastBar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	lastBar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	lastBar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	lastBar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier( cmd,
	                       VK_PIPELINE_STAGE_TRANSFER_BIT,
	                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                       0, 0, nullptr, 0, nullptr, 1, &lastBar );

	VKContext_endTransferCmd( cmd );
	destroyStaging( staging );

	// ── Image view ────────────────────────────────────────────────────────
	VkImageViewCreateInfo vci = {};
	vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image    = tex.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format   = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 };
	VK_CHECK( vkCreateImageView( vk.device, &vci, nullptr, &tex.view ) );

	// ── Sampler ───────────────────────────────────────────────────────────
	tex.sampler = createSampler( filter, anisotropy, false, mipLevels );
	tex.valid   = true;

	// ── Register in table ─────────────────────────────────────────────────
	ensureReservedSlot();
	uint32_t idx = static_cast<uint32_t>( g_vkTextures.size() );
	g_vkTextures.push_back( tex );
	return idx;
}

// ── Cube-map texture upload ───────────────────────────────────────────────────

uint32_t VKTexture_createCubeMap( const unsigned char* const faces[6],
                                  uint32_t                   faceSize )
{
	ASSERT_MESSAGE( GlobalVulkan().contextValid, "VKTexture_createCubeMap: no Vulkan context" );

	VulkanBinding& vk       = GlobalVulkan();
	const VkDeviceSize faceBytes = static_cast<VkDeviceSize>( faceSize ) * faceSize * 4;

	// ── Concatenate all face data into one staging buffer ─────────────────
	std::vector<unsigned char> combined( 6 * faceBytes );
	for ( int i = 0; i < 6; ++i )
		std::memcpy( combined.data() + i * faceBytes, faces[i], static_cast<std::size_t>( faceBytes ) );

	StagingBuffer staging = createStaging( combined.data(),
	                                       static_cast<VkDeviceSize>( combined.size() ) );

	// ── Create cube-map image ─────────────────────────────────────────────
	VkImageCreateInfo ici = {};
	ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	ici.imageType     = VK_IMAGE_TYPE_2D;
	ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent        = { faceSize, faceSize, 1 };
	ici.mipLevels     = 1;
	ici.arrayLayers   = 6;
	ici.samples       = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
	ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocCI = {};
	allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	VkTexture tex;
	tex.width     = faceSize;
	tex.height    = faceSize;
	tex.mipLevels = 1;
	tex.isCubeMap = true;

	VK_CHECK( vmaCreateImage( vk.allocator, &ici, &allocCI, &tex.image, &tex.alloc, nullptr ) );

	// ── Upload via staging ────────────────────────────────────────────────
	VkCommandBuffer cmd = VKContext_beginTransferCmd();

	vkutil_transitionImageLayout( cmd, tex.image,
	                              VK_IMAGE_LAYOUT_UNDEFINED,
	                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                              VK_IMAGE_ASPECT_COLOR_BIT, 1, 6 );

	VkBufferImageCopy copies[6] = {};
	for ( int i = 0; i < 6; ++i )
	{
		copies[i].bufferOffset      = static_cast<VkDeviceSize>( i ) * faceBytes;
		copies[i].imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0,
		                               static_cast<uint32_t>( i ), 1 };
		copies[i].imageExtent       = { faceSize, faceSize, 1 };
	}
	vkCmdCopyBufferToImage( cmd, staging.buffer, tex.image,
	                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies );

	vkutil_transitionImageLayout( cmd, tex.image,
	                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                              VK_IMAGE_ASPECT_COLOR_BIT, 1, 6 );

	VKContext_endTransferCmd( cmd );
	destroyStaging( staging );

	// ── Image view ────────────────────────────────────────────────────────
	VkImageViewCreateInfo vci = {};
	vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image    = tex.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	vci.format   = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
	VK_CHECK( vkCreateImageView( vk.device, &vci, nullptr, &tex.view ) );

	tex.sampler = createSampler( VK_TEX_LINEAR, false, true, 1 );
	tex.valid   = true;

	ensureReservedSlot();
	uint32_t idx = static_cast<uint32_t>( g_vkTextures.size() );
	g_vkTextures.push_back( tex );
	return idx;
}

// ── Destroy ───────────────────────────────────────────────────────────────────

static void destroyTexture( VkTexture& tex )
{
	if ( !tex.valid ) return;
	VkDevice     dev   = GlobalVulkan().device;
	VmaAllocator alloc = GlobalVulkan().allocator;

	if ( tex.sampler ) vkDestroySampler  ( dev, tex.sampler, nullptr );
	if ( tex.view    ) vkDestroyImageView( dev, tex.view,    nullptr );
	if ( tex.image   ) vmaDestroyImage   ( alloc, tex.image, tex.alloc );

	tex = VkTexture{};
}

void VKTexture_destroy( uint32_t index )
{
	if ( index == 0 || index >= g_vkTextures.size() ) return;
	destroyTexture( g_vkTextures[index] );
}

void VKTexture_destroyAll()
{
	for ( auto& tex : g_vkTextures )
		destroyTexture( tex );
	g_vkTextures.clear();
}

// ── Get ───────────────────────────────────────────────────────────────────────

const VkTexture& VKTexture_get( uint32_t index )
{
	ASSERT_MESSAGE( index < g_vkTextures.size(), "VKTexture_get: invalid index " << index );
	return g_vkTextures[index];
}
