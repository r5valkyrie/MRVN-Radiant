/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "renderstate.h"

#include "debugging/debugging.h"

#include "ishaders.h"
#include "irender.h"
#include "itextures.h"
#include "ivk.h"
#include "ivkcontext.h"
#include "iglrender.h"
#include "renderable.h"
#include "qerplugin.h"
#include "preferences.h"

#include "render.h"

#include <set>
#include <vector>
#include <list>
#include <map>
#include <algorithm>

#include "math/matrix.h"
#include "math/aabb.h"
#include "generic/callback.h"
#include "texturelib.h"
#include "string/string.h"
#include "container/hashfunc.h"
#include "container/cache.h"
#include "generic/reference.h"
#include "moduleobservers.h"
#include "stream/stringstream.h"
#include <fstream>
#include <cstring>

#include "xywindow.h"
#include "camwindow.h"

// GL blend factor aliases mapped to VkBlendFactor values.
// These are referenced throughout construct() and kept as uint32_t in OpenGLState.
#define GL_ZERO                   VK_BLEND_FACTOR_ZERO
#define GL_ONE                    VK_BLEND_FACTOR_ONE
#define GL_SRC_COLOR              VK_BLEND_FACTOR_SRC_COLOR
#define GL_ONE_MINUS_SRC_COLOR    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
#define GL_DST_COLOR              VK_BLEND_FACTOR_DST_COLOR
#define GL_ONE_MINUS_DST_COLOR    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
#define GL_SRC_ALPHA              VK_BLEND_FACTOR_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
#define GL_DST_ALPHA              VK_BLEND_FACTOR_DST_ALPHA
#define GL_ONE_MINUS_DST_ALPHA    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
#define GL_SRC_ALPHA_SATURATE     VK_BLEND_FACTOR_SRC_ALPHA_SATURATE



#define DEBUG_RENDER 0

inline void debug_string( const char* string ){
#if (DEBUG_RENDER)
	globalOutputStream() << string << '\n';
#endif
}

inline void debug_int( const char* comment, int i ){
#if (DEBUG_RENDER)
	globalOutputStream() << comment << ' ' << i << '\n';
#endif
}

inline void debug_colour( const char* /* comment */ ){
}

#include "timer.h"

StringOutputStream g_renderer_stats;
std::size_t g_count_prims;
std::size_t g_count_states;
std::size_t g_count_transforms;
Timer g_timer;
static bool g_statsEnabled = false;

inline void count_prim(){
	if( g_statsEnabled ) ++g_count_prims;
}

inline void count_state(){
	if( g_statsEnabled ) ++g_count_states;
}

inline void count_transform(){
	if( g_statsEnabled ) ++g_count_transforms;
}

void Renderer_SetStatsEnabled( bool enabled ){
	g_statsEnabled = enabled;
}

void Renderer_ResetStats(){
	g_count_prims = 0;
	g_count_states = 0;
	g_count_transforms = 0;
	g_timer.start();
}

const char* Renderer_GetStats( int frame2frame ){
	return g_renderer_stats(
		"prims: ", g_count_prims,
		" | states: ", g_count_states,
		" | transforms: ", g_count_transforms,
		" | msec: ", g_timer.elapsed_msec(),
		" | f2f: ", frame2frame
	);
}


// ── Vulkan shader program infrastructure ────────────────────────────────────

/// The command buffer currently being recorded for the active render frame.
/// Set by the render state machine (Phase 5); NULL until then.
VkCommandBuffer g_renderCmdBuffer = VK_NULL_HANDLE;

/// Shared descriptor pool for the three program types.
static VkDescriptorPool g_renderDescPool = VK_NULL_HANDLE;
static int g_renderProgRefCount = 0;

/// UBO layout supplied to every GLSL 4.5 vertex shader (set 0, binding 0).
/// The five mat4s correspond to: mvp, texMatrix0, texMatrix1, texMatrix2, localToLight.
struct VkTransformUBO {
	float mvp[16];
	float texMatrix0[16];
	float texMatrix1[16];
	float texMatrix2[16];
	float localToLight[16];
};
static const float s_mat4Identity[16] = {
	1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };

static void ensureDescriptorPool()
{
	if ( g_renderDescPool != VK_NULL_HANDLE ) return;
	VkDescriptorPoolSize sizes[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         16 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 },
	};
	VkDescriptorPoolCreateInfo ci = {};
	ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	ci.maxSets       = 16;
	ci.poolSizeCount = 2;
	ci.pPoolSizes    = sizes;
	vkCreateDescriptorPool( GlobalVulkan().device, &ci, nullptr, &g_renderDescPool );
}

/// Load a SPIR-V binary from disk and return a VkShaderModule.
static VkShaderModule loadSPIRV( const char* path )
{
	std::ifstream f( path, std::ios::binary | std::ios::ate );
	ASSERT_MESSAGE( f.is_open(), "loadSPIRV: failed to open " << path );
	const std::streamsize sz = f.tellg();
	f.seekg( 0 );
	std::vector<uint32_t> buf( static_cast<std::size_t>( sz + 3 ) / 4 );
	f.read( reinterpret_cast<char*>( buf.data() ), sz );
	VkShaderModuleCreateInfo ci = {};
	ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = static_cast<std::size_t>( sz );
	ci.pCode    = buf.data();
	VkShaderModule mod;
	if ( vkCreateShaderModule( GlobalVulkan().device, &ci, nullptr, &mod ) != VK_SUCCESS )
		ERROR_MESSAGE( "loadSPIRV: vkCreateShaderModule failed for " << path );
	return mod;
}

/// Vertex-input description for ArbitraryMeshVertex (56-byte stride).
static void fillVertexInputState(
	VkVertexInputBindingDescription&   binding,
	VkVertexInputAttributeDescription  attrs[5],
	uint32_t&                          attrCount )
{
	binding = { 0, 56, VK_VERTEX_INPUT_RATE_VERTEX };
	attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 20 }; // position
	attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,    0  }; // texcoord
	attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 8  }; // normal
	attrs[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, 32 }; // tangent
	attrs[4] = { 4, 0, VK_FORMAT_R32G32B32_SFLOAT, 44 }; // binormal
	attrCount = 5;
}

/// Create a simple graphics pipeline given vert+frag shader modules and pipeline layout.
/// Depth test / write / no transparency defaults — Phase 5 will introduce dynamic state.
static VkPipeline buildPipeline(
	VkShaderModule    vertMod,
	VkShaderModule    fragMod,
	VkPipelineLayout  layout,
	uint32_t          attrCount )
{
	VkVertexInputBindingDescription   binding;
	VkVertexInputAttributeDescription attrs[5];
	uint32_t                          nAttrs;
	fillVertexInputState( binding, attrs, nAttrs );

	const uint32_t usedAttrs = ( attrCount < nAttrs ) ? attrCount : nAttrs;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertMod;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragMod;
	stages[1].pName  = "main";

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount   = 1;
	vi.pVertexBindingDescriptions      = &binding;
	vi.vertexAttributeDescriptionCount = usedAttrs;
	vi.pVertexAttributeDescriptions    = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount  = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode             = VK_POLYGON_MODE_FILL;
	rs.cullMode                = VK_CULL_MODE_NONE;
	rs.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth               = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable  = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp   = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState ba = {};
	ba.colorWriteMask = 0xF;

	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments    = &ba;

	const VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dyn = {};
	dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates    = dynStates;

	VkGraphicsPipelineCreateInfo pci = {};
	pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pci.stageCount          = 2;
	pci.pStages             = stages;
	pci.pVertexInputState   = &vi;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState      = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState   = &ms;
	pci.pDepthStencilState  = &ds;
	pci.pColorBlendState    = &cb;
	pci.pDynamicState       = &dyn;
	pci.layout              = layout;
	pci.renderPass          = GlobalVulkan().renderPass;
	pci.subpass             = 0;

	VkPipeline pipeline;
	if ( vkCreateGraphicsPipelines( GlobalVulkan().device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline ) != VK_SUCCESS )
		ERROR_MESSAGE( "buildPipeline: vkCreateGraphicsPipelines failed" );
	return pipeline;
}

/// Allocate one UBO buffer per program and create its set-0 descriptor set.
static void allocUBO(
	VkDescriptorSetLayout setLayout0,
	VkBuffer&             outBuffer,
	VmaAllocation&        outAlloc,
	VkTransformUBO*&      outMapped,
	VkDescriptorSet&      outSet )
{
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size  = sizeof( VkTransformUBO );
	bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
	            VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo ai;
	vmaCreateBuffer( GlobalVulkan().allocator, &bci, &aci, &outBuffer, &outAlloc, &ai );
	outMapped = static_cast<VkTransformUBO*>( ai.pMappedData );

	// Initialise to identity transforms
	for ( int i = 0; i < 5; ++i )
		std::memcpy( reinterpret_cast<float*>( outMapped ) + i * 16,
		             s_mat4Identity, 64 );

	VkDescriptorSetAllocateInfo dsai = {};
	dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool     = g_renderDescPool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts        = &setLayout0;
	vkAllocateDescriptorSets( GlobalVulkan().device, &dsai, &outSet );

	VkDescriptorBufferInfo bufInfo = { outBuffer, 0, sizeof( VkTransformUBO ) };
	VkWriteDescriptorSet   write   = {};
	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet          = outSet;
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	write.pBufferInfo     = &bufInfo;
	vkUpdateDescriptorSets( GlobalVulkan().device, 1, &write, 0, nullptr );
}

// ── VkBumpProgram ─────────────────────────────────────────────────────────────

bool g_bumpGLSLPass_enabled = false;
bool g_depthfillPass_enabled = false;

class VkBumpProgram : public GLProgram
{
public:
	qtexture_t* m_light_attenuation_xy = nullptr;
	qtexture_t* m_light_attenuation_z  = nullptr;
private:
	VkDescriptorSetLayout m_setLayout0    = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout1    = VK_NULL_HANDLE;
	VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
	VkPipeline            m_pipeline       = VK_NULL_HANDLE;
	VkBuffer              m_uboBuffer      = VK_NULL_HANDLE;
	VmaAllocation         m_uboAlloc       = VK_NULL_HANDLE;
	VkTransformUBO*       m_uboMapped      = nullptr;
	VkDescriptorSet       m_uboSet         = VK_NULL_HANDLE;
public:
	VkBumpProgram(){}

	void create(){
		ensureDescriptorPool();
		++g_renderProgRefCount;
		VkDevice dev = GlobalVulkan().device;

		// Set 0: UBO
		VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			                                    VK_SHADER_STAGE_VERTEX_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo li0 = {};
		li0.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li0.bindingCount = 1;
		li0.pBindings    = &b0;
		vkCreateDescriptorSetLayout( dev, &li0, nullptr, &m_setLayout0 );

		// Set 1: 5 combined samplers (diffuse, bump, specular, attenXY, attenZ)
		VkDescriptorSetLayoutBinding bindings1[5] = {};
		for ( int i = 0; i < 5; ++i ) {
			bindings1[i] = { (uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
				              VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
		}
		VkDescriptorSetLayoutCreateInfo li1 = {};
		li1.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li1.bindingCount = 5;
		li1.pBindings    = bindings1;
		vkCreateDescriptorSetLayout( dev, &li1, nullptr, &m_setLayout1 );

		// Push constants: 56 bytes (view/light/color/scale/exp)
		VkPushConstantRange pcRange = {
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 56 };

		VkDescriptorSetLayout setLayouts[] = { m_setLayout0, m_setLayout1 };
		VkPipelineLayoutCreateInfo pli = {};
		pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pli.setLayoutCount         = 2;
		pli.pSetLayouts            = setLayouts;
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges    = &pcRange;
		vkCreatePipelineLayout( dev, &pli, nullptr, &m_pipelineLayout );

		StringOutputStream vertPath( 256 );
		StringOutputStream fragPath( 256 );
		vertPath( GlobalRadiant().getAppPath(),
		          "shaders/spv/lighting_dbs_omni.vert.spv" );
		fragPath( GlobalRadiant().getAppPath(),
		          "shaders/spv/lighting_dbs_omni.frag.spv" );
		VkShaderModule vertMod = loadSPIRV( vertPath.c_str() );
		VkShaderModule fragMod = loadSPIRV( fragPath.c_str() );
		m_pipeline = buildPipeline( vertMod, fragMod, m_pipelineLayout, 5 );
		vkDestroyShaderModule( dev, vertMod, nullptr );
		vkDestroyShaderModule( dev, fragMod, nullptr );

		allocUBO( m_setLayout0, m_uboBuffer, m_uboAlloc, m_uboMapped, m_uboSet );
	}

	void destroy(){
		VkDevice dev   = GlobalVulkan().device;
		if ( m_pipeline )      vkDestroyPipeline            ( dev, m_pipeline,       nullptr );
		if ( m_pipelineLayout ) vkDestroyPipelineLayout      ( dev, m_pipelineLayout, nullptr );
		if ( m_setLayout0 )    vkDestroyDescriptorSetLayout  ( dev, m_setLayout0,    nullptr );
		if ( m_setLayout1 )    vkDestroyDescriptorSetLayout  ( dev, m_setLayout1,    nullptr );
		if ( m_uboBuffer )     vmaDestroyBuffer               ( GlobalVulkan().allocator,
		                                                         m_uboBuffer, m_uboAlloc );
		m_pipeline = VK_NULL_HANDLE;  m_pipelineLayout = VK_NULL_HANDLE;
		m_setLayout0 = m_setLayout1 = VK_NULL_HANDLE;
		m_uboBuffer  = VK_NULL_HANDLE;  m_uboMapped = nullptr;  m_uboSet = VK_NULL_HANDLE;
		if ( --g_renderProgRefCount == 0 && g_renderDescPool != VK_NULL_HANDLE ) {
			vkDestroyDescriptorPool( dev, g_renderDescPool, nullptr );
			g_renderDescPool = VK_NULL_HANDLE;
		}
	}

	void enable(){
		if ( g_renderCmdBuffer == VK_NULL_HANDLE ) return;
		vkCmdBindPipeline( g_renderCmdBuffer,
			               VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline );
		vkCmdBindDescriptorSets( g_renderCmdBuffer,
			                      VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
			                      0, 1, &m_uboSet, 0, nullptr );
		debug_string( "enable bump" );
		g_bumpGLSLPass_enabled = true;
	}

	void disable(){
		debug_string( "disable bump" );
		g_bumpGLSLPass_enabled = false;
	}

	void setParameters( const Vector3& viewer,
	                    const Matrix4& localToWorld,
	                    const Vector3& origin,
	                    const Vector3& colour,
	                    const Matrix4& world2light ){
		if ( g_renderCmdBuffer == VK_NULL_HANDLE || !m_uboMapped ) return;

		Matrix4 world2local( localToWorld );
		matrix4_affine_invert( world2local );

		Vector3 localLight( origin );
		matrix4_transform_point( world2local, localLight );

		Vector3 localViewer( viewer );
		matrix4_transform_point( world2local, localViewer );

		Matrix4 local2light( world2light );
		matrix4_multiply_by_matrix4( local2light, localToWorld );

		// Write light-space transform into UBO slot 4
		std::memcpy( m_uboMapped->localToLight, &local2light, 64 );

		// Push constants (56 bytes)
		struct BumpPC {
			float view_origin[3];  float _p0;
			float light_origin[3]; float _p1;
			float light_color[3];  float _p2;
			float bump_scale;
			float specular_exponent;
		} pc;
		pc.view_origin[0]  = localViewer.x(); pc.view_origin[1]  = localViewer.y(); pc.view_origin[2]  = localViewer.z(); pc._p0 = 0;
		pc.light_origin[0] = localLight.x();  pc.light_origin[1] = localLight.y();  pc.light_origin[2] = localLight.z();  pc._p1 = 0;
		pc.light_color[0]  = colour.x();      pc.light_color[1]  = colour.y();      pc.light_color[2]  = colour.z();      pc._p2 = 0;
		pc.bump_scale         = 1.0f;
		pc.specular_exponent  = 32.0f;
		vkCmdPushConstants( g_renderCmdBuffer, m_pipelineLayout,
			               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			               0, sizeof( pc ), &pc );
	}
};

VkBumpProgram g_bumpGLSL;

// ── VkDepthFillProgram ────────────────────────────────────────────────────────

class VkDepthFillProgram : public GLProgram
{
private:
	VkDescriptorSetLayout m_setLayout0    = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout1    = VK_NULL_HANDLE;
	VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
	VkPipeline            m_pipeline       = VK_NULL_HANDLE;
	VkBuffer              m_uboBuffer      = VK_NULL_HANDLE;
	VmaAllocation         m_uboAlloc       = VK_NULL_HANDLE;
	VkTransformUBO*       m_uboMapped      = nullptr;
	VkDescriptorSet       m_uboSet         = VK_NULL_HANDLE;
public:
	void create(){
		ensureDescriptorPool();
		++g_renderProgRefCount;
		VkDevice dev = GlobalVulkan().device;

		VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			                                    VK_SHADER_STAGE_VERTEX_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo li0 = {};
		li0.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li0.bindingCount = 1;
		li0.pBindings = &b0;
		vkCreateDescriptorSetLayout( dev, &li0, nullptr, &m_setLayout0 );

		VkDescriptorSetLayoutBinding b1 = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			                                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo li1 = {};
		li1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li1.bindingCount = 1;
		li1.pBindings = &b1;
		vkCreateDescriptorSetLayout( dev, &li1, nullptr, &m_setLayout1 );

		VkDescriptorSetLayout setLayouts[] = { m_setLayout0, m_setLayout1 };
		VkPipelineLayoutCreateInfo pli = {};
		pli.sType           = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pli.setLayoutCount  = 2;
		pli.pSetLayouts     = setLayouts;
		vkCreatePipelineLayout( dev, &pli, nullptr, &m_pipelineLayout );

		StringOutputStream vertPath( 256 ), fragPath( 256 );
		vertPath( GlobalRadiant().getAppPath(), "shaders/spv/zfill.vert.spv" );
		fragPath( GlobalRadiant().getAppPath(), "shaders/spv/zfill.frag.spv" );
		VkShaderModule vertMod = loadSPIRV( vertPath.c_str() );
		VkShaderModule fragMod = loadSPIRV( fragPath.c_str() );
		m_pipeline = buildPipeline( vertMod, fragMod, m_pipelineLayout, 2 );
		vkDestroyShaderModule( dev, vertMod, nullptr );
		vkDestroyShaderModule( dev, fragMod, nullptr );

		allocUBO( m_setLayout0, m_uboBuffer, m_uboAlloc, m_uboMapped, m_uboSet );
	}
	void destroy(){
		VkDevice dev = GlobalVulkan().device;
		if ( m_pipeline )       vkDestroyPipeline           ( dev, m_pipeline,       nullptr );
		if ( m_pipelineLayout ) vkDestroyPipelineLayout     ( dev, m_pipelineLayout, nullptr );
		if ( m_setLayout0 )     vkDestroyDescriptorSetLayout( dev, m_setLayout0,     nullptr );
		if ( m_setLayout1 )     vkDestroyDescriptorSetLayout( dev, m_setLayout1,     nullptr );
		if ( m_uboBuffer )      vmaDestroyBuffer             ( GlobalVulkan().allocator,
		                                                        m_uboBuffer, m_uboAlloc );
		m_pipeline = VK_NULL_HANDLE;  m_pipelineLayout = VK_NULL_HANDLE;
		m_setLayout0 = m_setLayout1 = VK_NULL_HANDLE;
		m_uboBuffer  = VK_NULL_HANDLE;  m_uboMapped = nullptr;  m_uboSet = VK_NULL_HANDLE;
		if ( --g_renderProgRefCount == 0 && g_renderDescPool != VK_NULL_HANDLE ) {
			vkDestroyDescriptorPool( dev, g_renderDescPool, nullptr );
			g_renderDescPool = VK_NULL_HANDLE;
		}
	}
	void enable(){
		if ( g_renderCmdBuffer == VK_NULL_HANDLE ) return;
		vkCmdBindPipeline( g_renderCmdBuffer,
			               VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline );
		vkCmdBindDescriptorSets( g_renderCmdBuffer,
			                      VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
			                      0, 1, &m_uboSet, 0, nullptr );
		debug_string( "enable depthfill" );
		g_depthfillPass_enabled = true;
	}
	void disable(){
		debug_string( "disable depthfill" );
		g_depthfillPass_enabled = false;
	}
	void setParameters( const Vector3&, const Matrix4&, const Vector3&,
	                    const Vector3&, const Matrix4& ){}
};

VkDepthFillProgram g_depthFillGLSL;

// ── VkSkyboxProgram ───────────────────────────────────────────────────────────

class VkSkyboxProgram : public GLProgram
{
private:
	VkDescriptorSetLayout m_setLayout0    = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout1    = VK_NULL_HANDLE;
	VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
	VkPipeline            m_pipeline       = VK_NULL_HANDLE;
	VkBuffer              m_uboBuffer      = VK_NULL_HANDLE;
	VmaAllocation         m_uboAlloc       = VK_NULL_HANDLE;
	VkTransformUBO*       m_uboMapped      = nullptr;
	VkDescriptorSet       m_uboSet         = VK_NULL_HANDLE;
public:
	VkSkyboxProgram(){}

	void create(){
		ensureDescriptorPool();
		++g_renderProgRefCount;
		VkDevice dev = GlobalVulkan().device;

		VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			                                    VK_SHADER_STAGE_VERTEX_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo li0 = {};
		li0.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li0.bindingCount = 1;
		li0.pBindings = &b0;
		vkCreateDescriptorSetLayout( dev, &li0, nullptr, &m_setLayout0 );

		VkDescriptorSetLayoutBinding b1 = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			                                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo li1 = {};
		li1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li1.bindingCount = 1;
		li1.pBindings = &b1;
		vkCreateDescriptorSetLayout( dev, &li1, nullptr, &m_setLayout1 );

		VkPushConstantRange pcRange = {
			VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };  // vec3 + 4b pad

		VkDescriptorSetLayout setLayouts[] = { m_setLayout0, m_setLayout1 };
		VkPipelineLayoutCreateInfo pli = {};
		pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pli.setLayoutCount         = 2;
		pli.pSetLayouts            = setLayouts;
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges    = &pcRange;
		vkCreatePipelineLayout( dev, &pli, nullptr, &m_pipelineLayout );

		StringOutputStream vertPath( 256 ), fragPath( 256 );
		vertPath( GlobalRadiant().getAppPath(), "shaders/spv/skybox.vert.spv" );
		fragPath( GlobalRadiant().getAppPath(), "shaders/spv/skybox.frag.spv" );
		VkShaderModule vertMod = loadSPIRV( vertPath.c_str() );
		VkShaderModule fragMod = loadSPIRV( fragPath.c_str() );
		m_pipeline = buildPipeline( vertMod, fragMod, m_pipelineLayout, 1 );
		vkDestroyShaderModule( dev, vertMod, nullptr );
		vkDestroyShaderModule( dev, fragMod, nullptr );

		allocUBO( m_setLayout0, m_uboBuffer, m_uboAlloc, m_uboMapped, m_uboSet );
	}
	void destroy(){
		VkDevice dev = GlobalVulkan().device;
		if ( m_pipeline )       vkDestroyPipeline           ( dev, m_pipeline,       nullptr );
		if ( m_pipelineLayout ) vkDestroyPipelineLayout     ( dev, m_pipelineLayout, nullptr );
		if ( m_setLayout0 )     vkDestroyDescriptorSetLayout( dev, m_setLayout0,     nullptr );
		if ( m_setLayout1 )     vkDestroyDescriptorSetLayout( dev, m_setLayout1,     nullptr );
		if ( m_uboBuffer )      vmaDestroyBuffer             ( GlobalVulkan().allocator,
		                                                        m_uboBuffer, m_uboAlloc );
		m_pipeline = VK_NULL_HANDLE;  m_pipelineLayout = VK_NULL_HANDLE;
		m_setLayout0 = m_setLayout1 = VK_NULL_HANDLE;
		m_uboBuffer  = VK_NULL_HANDLE;  m_uboMapped = nullptr;  m_uboSet = VK_NULL_HANDLE;
		if ( --g_renderProgRefCount == 0 && g_renderDescPool != VK_NULL_HANDLE ) {
			vkDestroyDescriptorPool( dev, g_renderDescPool, nullptr );
			g_renderDescPool = VK_NULL_HANDLE;
		}
	}
	void enable(){
		if ( g_renderCmdBuffer == VK_NULL_HANDLE ) return;
		vkCmdBindPipeline( g_renderCmdBuffer,
			               VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline );
		vkCmdBindDescriptorSets( g_renderCmdBuffer,
			                      VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
			                      0, 1, &m_uboSet, 0, nullptr );
		debug_string( "enable skybox" );
	}
	void disable(){
		debug_string( "disable skybox" );
	}
	void setParameters( const Vector3& viewer,
	                    const Matrix4& /* localToWorld */,
	                    const Vector3& /* origin */,
	                    const Vector3& /* colour */,
	                    const Matrix4& /* world2light */ ){
		if ( g_renderCmdBuffer == VK_NULL_HANDLE ) return;
		struct SkyboxPC { float view_origin[3]; float _p0; } pc;
		pc.view_origin[0] = viewer.x();
		pc.view_origin[1] = viewer.y();
		pc.view_origin[2] = viewer.z();
		pc._p0 = 0;
		vkCmdPushConstants( g_renderCmdBuffer, m_pipelineLayout,
			               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( pc ), &pc );
	}
};

VkSkyboxProgram g_skyboxGLSL;



bool g_vertexArray_enabled = false;
bool g_normalArray_enabled = false;
bool g_texcoordArray_enabled = false;
bool g_colorArray_enabled = false;

inline bool OpenGLState_less( const OpenGLState& self, const OpenGLState& other ){
	//! Sort by sort-order override.
	if ( self.m_sort != other.m_sort ) {
		return self.m_sort < other.m_sort;
	}
	//! Sort by texture handle.
	if ( self.m_texture != other.m_texture ) {
		return self.m_texture < other.m_texture;
	}
	if ( self.m_texture1 != other.m_texture1 ) {
		return self.m_texture1 < other.m_texture1;
	}
	if ( self.m_texture2 != other.m_texture2 ) {
		return self.m_texture2 < other.m_texture2;
	}
	if ( self.m_texture3 != other.m_texture3 ) {
		return self.m_texture3 < other.m_texture3;
	}
	if ( self.m_texture4 != other.m_texture4 ) {
		return self.m_texture4 < other.m_texture4;
	}
	if ( self.m_texture5 != other.m_texture5 ) {
		return self.m_texture5 < other.m_texture5;
	}
	if ( self.m_texture6 != other.m_texture6 ) {
		return self.m_texture6 < other.m_texture6;
	}
	if ( self.m_texture7 != other.m_texture7 ) {
		return self.m_texture7 < other.m_texture7;
	}
	if ( self.m_textureSkyBox != other.m_textureSkyBox ) {
		return self.m_textureSkyBox < other.m_textureSkyBox;
	}
	//! Sort by state bit-vector.
	if ( self.m_state != other.m_state ) {
		return self.m_state < other.m_state;
	}
	//! Comparing address makes sure states are never equal.
	return &self < &other;
}

void OpenGLState_constructDefault( OpenGLState& state ){
	state.m_state = RENDER_DEFAULT;

	state.m_texture = 0;
	state.m_texture1 = 0;
	state.m_texture2 = 0;
	state.m_texture3 = 0;
	state.m_texture4 = 0;
	state.m_texture5 = 0;
	state.m_texture6 = 0;
	state.m_texture7 = 0;
	state.m_textureSkyBox = 0;

	state.m_colour[0] = 1;
	state.m_colour[1] = 1;
	state.m_colour[2] = 1;
	state.m_colour[3] = 1;

	state.m_depthfunc = RS_COMPARE_LESS;

	state.m_blend_src = RS_BLEND_SRC_ALPHA;
	state.m_blend_dst = RS_BLEND_ONE_MINUS_SRC_ALPHA;

	state.m_alphafunc = RS_COMPARE_ALWAYS;
	state.m_alpharef = 0;

	state.m_linewidth = 1;
	state.m_pointsize = 1;

	state.m_linestipple_factor = 1;
	state.m_linestipple_pattern = 0xaaaa;

	state.m_fog = OpenGLFogState();
}




/// \brief A container of Renderable references.
/// May contain the same Renderable multiple times, with different transforms.
class OpenGLStateBucket
{
public:
	struct RenderTransform
	{
		const Matrix4* m_transform;
		const OpenGLRenderable *m_renderable;
		const RendererLight* m_light;

		RenderTransform( const OpenGLRenderable& renderable, const Matrix4& transform, const RendererLight* light )
			: m_transform( &transform ), m_renderable( &renderable ), m_light( light ){
		}
	};

	typedef std::vector<RenderTransform> Renderables;

private:

	OpenGLState m_state;
	Renderables m_renderables;

public:
	OpenGLStateBucket(){
	}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& modelview, const RendererLight* light = 0 ){
		m_renderables.push_back( RenderTransform( renderable, modelview, light ) );
	}

	OpenGLState& state(){
		return m_state;
	}

	void render( OpenGLState& current, unsigned int globalstate, const Vector3& viewer, const Matrix4& viewMatrix );
};

#define LIGHT_SHADER_DEBUG 0

#if LIGHT_SHADER_DEBUG
typedef std::vector<Shader*> LightDebugShaders;
LightDebugShaders g_lightDebugShaders;
#endif

class OpenGLStateLess
{
public:
	bool operator()( const OpenGLState& self, const OpenGLState& other ) const {
		return OpenGLState_less( self, other );
	}
};

typedef ConstReference<OpenGLState> OpenGLStateReference;
typedef std::map<OpenGLStateReference, OpenGLStateBucket*, OpenGLStateLess> OpenGLStates;
OpenGLStates g_state_sorted;

class OpenGLStateBucketAdd
{
	OpenGLStateBucket& m_bucket;
	const OpenGLRenderable& m_renderable;
	const Matrix4& m_modelview;
public:
	typedef const RendererLight& first_argument_type;

	OpenGLStateBucketAdd( OpenGLStateBucket& bucket, const OpenGLRenderable& renderable, const Matrix4& modelview ) :
		m_bucket( bucket ), m_renderable( renderable ), m_modelview( modelview ){
	}
	void operator()( const RendererLight& light ){
		m_bucket.addRenderable( m_renderable, m_modelview, &light );
	}
};

class CountLights
{
	std::size_t m_count;
public:
	typedef RendererLight& first_argument_type;

	CountLights() : m_count( 0 ){
	}
	void operator()( const RendererLight& light ){
		++m_count;
	}
	std::size_t count() const {
		return m_count;
	}
};

class OpenGLShader final : public Shader
{
	typedef std::list<OpenGLStateBucket*> Passes;
	Passes m_passes;
	IShader* m_shader;
	std::size_t m_used;
	ModuleObservers m_observers;
public:
	OpenGLShader() : m_shader( 0 ), m_used( 0 ){
	}
	~OpenGLShader(){
	}
	void construct( const char* name );
	void destroy(){
		if ( m_shader ) {
			m_shader->DecRef();
		}
		m_shader = 0;

		for ( Passes::iterator i = m_passes.begin(); i != m_passes.end(); ++i )
		{
			delete *i;
		}
		m_passes.clear();
	}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& modelview, const LightList* lights ){
		for ( Passes::iterator i = m_passes.begin(); i != m_passes.end(); ++i )
		{
#if LIGHT_SHADER_DEBUG
			if ( ( ( *i )->state().m_state & RENDER_BUMP ) != 0 ) {
				if ( lights != 0 ) {
					CountLights counter;
					lights->forEachLight( makeCallback1( counter ) );
					globalOutputStream() << "count = " << counter.count() << '\n';
					for ( std::size_t i = 0; i < counter.count(); ++i )
					{
						g_lightDebugShaders[counter.count()]->addRenderable( renderable, modelview );
					}
				}
			}
			else
#else
			if ( ( ( *i )->state().m_state & RENDER_BUMP ) != 0 ) {
				if ( lights != 0 ) {
					OpenGLStateBucketAdd add( *( *i ), renderable, modelview );
					lights->forEachLight( makeCallback1( add ) );
				}
			}
			else
#endif
			{
				( *i )->addRenderable( renderable, modelview );
			}
		}
	}
	void incrementUsed(){
		if ( ++m_used == 1 && m_shader != 0 ) {
			m_shader->SetInUse( true );
		}
	}
	void decrementUsed(){
		if ( --m_used == 0 && m_shader != 0 ) {
			m_shader->SetInUse( false );
		}
	}
	bool realised() const {
		return m_shader != 0;
	}
	void attach( ModuleObserver& observer ){
		if ( realised() ) {
			observer.realise();
		}
		m_observers.attach( observer );
	}
	void detach( ModuleObserver& observer ){
		if ( realised() ) {
			observer.unrealise();
		}
		m_observers.detach( observer );
	}
	void realise( const CopiedString& name ){
		construct( name.c_str() );

		if ( m_used != 0 && m_shader != 0 ) {
			m_shader->SetInUse( true );
		}

		for ( Passes::iterator i = m_passes.begin(); i != m_passes.end(); ++i )
		{
			g_state_sorted.insert( OpenGLStates::value_type( OpenGLStateReference( ( *i )->state() ), *i ) );
		}

		m_observers.realise();
	}
	void unrealise(){
		m_observers.unrealise();

		for ( Passes::iterator i = m_passes.begin(); i != m_passes.end(); ++i )
		{
			g_state_sorted.erase( OpenGLStateReference( ( *i )->state() ) );
		}

		destroy();
	}
	qtexture_t& getTexture() const {
		ASSERT_NOTNULL( m_shader );
		return *m_shader->getTexture();
	}
	unsigned int getFlags() const {
		ASSERT_NOTNULL( m_shader );
		return m_shader->getFlags();
	}
	IShader& getShader() const {
		ASSERT_NOTNULL( m_shader );
		return *m_shader;
	}
	OpenGLState& appendDefaultPass(){
		m_passes.push_back( new OpenGLStateBucket );
		OpenGLState& state = m_passes.back()->state();
		OpenGLState_constructDefault( state );
		return state;
	}
};


inline bool lightEnabled( const RendererLight& light, const LightCullable& cullable ){
	return cullable.testLight( light );
}

typedef std::set<RendererLight*> RendererLights;

#define DEBUG_LIGHT_SYNC 0

class LinearLightList : public LightList
{
	LightCullable& m_cullable;
	RendererLights& m_allLights;
	Callback m_evaluateChanged;

	typedef std::list<RendererLight*> Lights;
	mutable Lights m_lights;
	mutable bool m_lightsChanged;
public:
	LinearLightList( LightCullable& cullable, RendererLights& lights, const Callback& evaluateChanged ) :
		m_cullable( cullable ), m_allLights( lights ), m_evaluateChanged( evaluateChanged ){
		m_lightsChanged = true;
	}
	void evaluateLights() const {
		m_evaluateChanged();
		if ( m_lightsChanged ) {
			m_lightsChanged = false;

			m_lights.clear();
			m_cullable.clearLights();
			for ( RendererLights::const_iterator i = m_allLights.begin(); i != m_allLights.end(); ++i )
			{
				if ( lightEnabled( *( *i ), m_cullable ) ) {
					m_lights.push_back( *i );
					m_cullable.insertLight( *( *i ) );
				}
			}
		}
#if ( DEBUG_LIGHT_SYNC )
		else
		{
			Lights lights;
			for ( RendererLights::const_iterator i = m_allLights.begin(); i != m_allLights.end(); ++i )
			{
				if ( lightEnabled( *( *i ), m_cullable ) ) {
					lights.push_back( *i );
				}
			}
			ASSERT_MESSAGE(
			    !std::lexicographical_compare( lights.begin(), lights.end(), m_lights.begin(), m_lights.end() )
			    && !std::lexicographical_compare( m_lights.begin(), m_lights.end(), lights.begin(), lights.end() ),
			    "lights out of sync"
			);
		}
#endif
	}
	void forEachLight( const RendererLightCallback& callback ) const {
		evaluateLights();

		for ( Lights::const_iterator i = m_lights.begin(); i != m_lights.end(); ++i )
		{
			callback( *( *i ) );
		}
	}
	void lightsChanged() const {
		m_lightsChanged = true;
	}
};

inline void setFogState( const OpenGLFogState& /*state*/ ){
	// Vulkan has no fixed-function fog; fog must be emulated in shaders if needed.
	// For now this is a no-op — no state is stored and no command is recorded.
}

#define DEBUG_SHADERS 0
void OpenGLState_apply( const OpenGLState& self, OpenGLState& current, unsigned int globalstate );

class OpenGLShaderCache final : public ShaderCache, public TexturesCacheObserver, public ModuleObserver
{
	class CreateOpenGLShader
	{
		OpenGLShaderCache* m_cache;
	public:
		explicit CreateOpenGLShader( OpenGLShaderCache* cache = 0 )
			: m_cache( cache ){
		}
		OpenGLShader* construct( const CopiedString& name ){
			OpenGLShader* shader = new OpenGLShader;
			if ( m_cache->realised() ) {
				shader->realise( name );
			}
			return shader;
		}
		void destroy( OpenGLShader* shader ){
			if ( m_cache->realised() ) {
				shader->unrealise();
			}
			delete shader;
		}
	};

	typedef HashedCache<CopiedString, OpenGLShader, HashString, std::equal_to<CopiedString>, CreateOpenGLShader> Shaders;
	Shaders m_shaders;
	std::size_t m_unrealised;

	bool m_lightingEnabled;

public:
	OpenGLShaderCache() :
		m_shaders( CreateOpenGLShader( this ) ),
		m_unrealised( 3 ), // wait until shaders, gl-context and textures are realised before creating any render-states
		m_lightingEnabled( true ),
		m_lightsChanged( true ),
		m_traverseRenderablesMutex( false ){
	}
	~OpenGLShaderCache(){
		for ( Shaders::iterator i = m_shaders.begin(); i != m_shaders.end(); ++i )
		{
			globalOutputStream() << "leaked shader: " << makeQuoted( ( *i ).key ) << '\n';
		}
	}
	Shader* capture( const char* name ){
		ASSERT_MESSAGE( name[0] == '$'
		                || *name == '['
		                || *name == '<'
		                || *name == '('
		                || *name == '{'
		                || strchr( name, '\\' ) == 0, "shader name contains invalid characters: " << makeQuoted( name ) );
#if DEBUG_SHADERS
		globalOutputStream() << "shaders capture: " << makeQuoted( name ) << '\n';
#endif
		return m_shaders.capture( name ).get();
	}
	void release( const char *name ){
#if DEBUG_SHADERS
		globalOutputStream() << "shaders release: " << makeQuoted( name ) << '\n';
#endif
		m_shaders.release( name );
	}
	void render( RenderStateFlags globalstate, const Matrix4& modelview, const Matrix4& projection, const Vector3& viewer ){
		// Upload view transforms to the active program UBOs.
		// The actual UBO binding happens in VkBumpProgram/VkDepthFillProgram/VkSkyboxProgram::enable().
		// Here we just store the combined MVP so enable() can push it.
		// (Phase 6 will wire g_renderCmdBuffer from the frame loop.)

		ASSERT_MESSAGE( realised(), "render states are not realised" );

		OpenGLState current;
		OpenGLState_constructDefault( current );
		current.m_sort = OpenGLState::eSortFirst;

		debug_string( "begin rendering" );
		for ( OpenGLStates::iterator i = g_state_sorted.begin(); i != g_state_sorted.end(); ++i )
		{
			( *i ).second->render( current, globalstate, viewer, modelview );
		}
		debug_string( "end rendering" );

		// Unbind streaming VBOs to restore client-side vertex array mode
		vbo_unbind();

		OpenGLState reset = current; /* reset some states */
		reset.m_state = current.m_state & ~RENDER_TEXT; /* popmatrix after RENDER_TEXT */
		reset.m_program = nullptr; /* disable shader */
		OpenGLState_apply( reset, current, globalstate );
	}
	void realise(){
		if ( --m_unrealised == 0 ) {
			if ( lightingEnabled() ) {
				g_bumpGLSL.create();
				g_depthFillGLSL.create();
			}

			g_skyboxGLSL.create();

			for ( Shaders::iterator i = m_shaders.begin(); i != m_shaders.end(); ++i )
			{
				if ( !( *i ).value.empty() ) {
					( *i ).value->realise( i->key );
				}
			}
		}
	}
	void unrealise(){
		if ( ++m_unrealised == 1 ) {
			for ( Shaders::iterator i = m_shaders.begin(); i != m_shaders.end(); ++i )
			{
				if ( !( *i ).value.empty() ) {
					( *i ).value->unrealise();
				}
			}
			if ( GlobalVulkan().contextValid && lightingEnabled() ) {
				g_bumpGLSL.destroy();
				g_depthFillGLSL.destroy();
			}
			if( GlobalVulkan().contextValid )
				g_skyboxGLSL.destroy();
		}
	}
	bool realised(){
		return m_unrealised == 0;
	}


	bool lightingEnabled() const {
		return m_lightingEnabled;
	}
	void extensionsInitialised(){
		setLightingEnabled( m_lightingEnabled );
	}
	void setLightingEnabled( bool enabled ){
		const bool refresh = ( m_lightingEnabled != enabled );

		if ( refresh ) {
			unrealise();
			GlobalShaderSystem().setLightingEnabled( enabled );
		}

		m_lightingEnabled = enabled;

		if ( refresh ) {
			realise();
		}
	}

// light culling

	RendererLights m_lights;
	bool m_lightsChanged;
	typedef std::map<LightCullable*, LinearLightList> LightLists;
	LightLists m_lightLists;

	const LightList& attach( LightCullable& cullable ){
		return ( *m_lightLists.insert( LightLists::value_type( &cullable, LinearLightList( cullable, m_lights, EvaluateChangedCaller( *this ) ) ) ).first ).second;
	}
	void detach( LightCullable& cullable ){
		m_lightLists.erase( &cullable );
	}
	void changed( LightCullable& cullable ){
		LightLists::iterator i = m_lightLists.find( &cullable );
		ASSERT_MESSAGE( i != m_lightLists.end(), "cullable not attached" );
		( *i ).second.lightsChanged();
	}
	void attach( RendererLight& light ){
		const bool inserted = m_lights.insert( &light ).second;
		ASSERT_MESSAGE( inserted, "light could not be attached" );
		changed( light );
	}
	void detach( RendererLight& light ){
		const bool erased = m_lights.erase( &light );
		ASSERT_MESSAGE( erased, "light could not be detached" );
		changed( light );
	}
	void changed( RendererLight& light ){
		m_lightsChanged = true;
	}
	void evaluateChanged(){
		if ( m_lightsChanged ) {
			m_lightsChanged = false;
			for ( LightLists::iterator i = m_lightLists.begin(); i != m_lightLists.end(); ++i )
			{
				( *i ).second.lightsChanged();
			}
		}
	}
	typedef MemberCaller<OpenGLShaderCache, &OpenGLShaderCache::evaluateChanged> EvaluateChangedCaller;

	typedef std::set<const Renderable*> Renderables;
	Renderables m_renderables;
	mutable bool m_traverseRenderablesMutex;

// renderables
	void attachRenderable( const Renderable& renderable ){
		ASSERT_MESSAGE( !m_traverseRenderablesMutex, "attaching renderable during traversal" );
		const bool inserted = m_renderables.insert( &renderable ).second;
		ASSERT_MESSAGE( inserted, "renderable could not be attached" );
	}
	void detachRenderable( const Renderable& renderable ){
		ASSERT_MESSAGE( !m_traverseRenderablesMutex, "detaching renderable during traversal" );
		const bool erased = m_renderables.erase( &renderable );
		ASSERT_MESSAGE( erased, "renderable could not be detached" );
	}
	void forEachRenderable( const RenderableCallback& callback ) const {
		ASSERT_MESSAGE( !m_traverseRenderablesMutex, "for-each during traversal" );
		m_traverseRenderablesMutex = true;
		for ( Renderables::const_iterator i = m_renderables.begin(); i != m_renderables.end(); ++i )
		{
			callback( *( *i ) );
		}
		m_traverseRenderablesMutex = false;
	}
};

static OpenGLShaderCache* g_ShaderCache;

void ShaderCache_extensionsInitialised(){
	g_ShaderCache->extensionsInitialised();
}

void ShaderCache_setBumpEnabled( bool enabled ){
	g_ShaderCache->setLightingEnabled( enabled );
}


Vector3 g_DebugShaderColours[256];
Shader* g_defaultPointLight = 0;

void ShaderCache_Construct(){
	g_ShaderCache = new OpenGLShaderCache;
	GlobalTexturesCache().attach( *g_ShaderCache );
	GlobalShaderSystem().attach( *g_ShaderCache );

	if ( g_pGameDescription->mGameType == "doom3" ) {
		g_defaultPointLight = g_ShaderCache->capture( "lights/defaultPointLight" );
		//Shader* overbright =
		g_ShaderCache->capture( "$OVERBRIGHT" );

#if LIGHT_SHADER_DEBUG
		for ( std::size_t i = 0; i < 256; ++i )
		{
			g_DebugShaderColours[i] = Vector3( i / 256.0, i / 256.0, i / 256.0 );
		}

		g_DebugShaderColours[0] = Vector3( 1, 0, 0 );
		g_DebugShaderColours[1] = Vector3( 1, 0.5, 0 );
		g_DebugShaderColours[2] = Vector3( 1, 1, 0 );
		g_DebugShaderColours[3] = Vector3( 0.5, 1, 0 );
		g_DebugShaderColours[4] = Vector3( 0, 1, 0 );
		g_DebugShaderColours[5] = Vector3( 0, 1, 0.5 );
		g_DebugShaderColours[6] = Vector3( 0, 1, 1 );
		g_DebugShaderColours[7] = Vector3( 0, 0.5, 1 );
		g_DebugShaderColours[8] = Vector3( 0, 0, 1 );
		g_DebugShaderColours[9] = Vector3( 0.5, 0, 1 );
		g_DebugShaderColours[10] = Vector3( 1, 0, 1 );
		g_DebugShaderColours[11] = Vector3( 1, 0, 0.5 );

		g_lightDebugShaders.reserve( 256 );
		StringOutputStream buffer( 256 );
		for ( std::size_t i = 0; i < 256; ++i )
		{
			buffer << '(' << g_DebugShaderColours[i].x() << ' ' << g_DebugShaderColours[i].y() << ' ' << g_DebugShaderColours[i].z() << ')';
			g_lightDebugShaders.push_back( g_ShaderCache->capture( buffer ) );
			buffer.clear();
		}
#endif
	}
}

void ShaderCache_Destroy(){
	if ( g_pGameDescription->mGameType == "doom3" ) {
		g_ShaderCache->release( "lights/defaultPointLight" );
		g_ShaderCache->release( "$OVERBRIGHT" );
		g_defaultPointLight = 0;

#if LIGHT_SHADER_DEBUG
		g_lightDebugShaders.clear();
		StringOutputStream buffer( 256 );
		for ( std::size_t i = 0; i < 256; ++i )
		{
			buffer << '(' << g_DebugShaderColours[i].x() << ' ' << g_DebugShaderColours[i].y() << ' ' << g_DebugShaderColours[i].z() << ')';
			g_ShaderCache->release( buffer );
		}
#endif
	}

	GlobalShaderSystem().detach( *g_ShaderCache );
	GlobalTexturesCache().detach( *g_ShaderCache );
	delete g_ShaderCache;
}

ShaderCache* GetShaderCache(){
	return g_ShaderCache;
}

/// Track texture slot changes in the current state — no GL call needed.
/// Actual Vulkan descriptor binding happens when we know which pipeline is active.
inline void setTextureState( uint32_t& current, const uint32_t& texture, GLenum /*textureUnit*/ ){
	if ( texture != current ) {
		current = texture;
	}
}

inline void setTextureState( uint32_t& current, const uint32_t& texture ){
	if ( texture != current ) {
		current = texture;
	}
}

/// In Vulkan, blend/depth/cull are baked into the pipeline or set via dynamic
/// state extensions — not per-draw glEnable/glDisable calls.  Record the flag
/// change in the tracking bitmask and return; no command is recorded here.
inline void setState( unsigned int /*state*/, unsigned int /*delta*/, unsigned int /*flag*/, GLenum /*glflag*/ ){
}

void OpenGLState_apply( const OpenGLState& self, OpenGLState& current, unsigned int globalstate ){
	debug_int( "sort", int(self.m_sort) );
	debug_int( "texture", self.m_texture );
	debug_int( "state", self.m_state );
	debug_int( "address", int(std::size_t( &self ) ) );

	count_state();

	if ( self.m_state & RENDER_OVERRIDE ) {
		globalstate |= RENDER_FILL;
	}
	if ( self.m_state & RENDER_TEXT ) {
		globalstate |= RENDER_TEXTURE | RENDER_BLEND | RENDER_FILL | RENDER_TEXT;
	}

	const unsigned int state = self.m_state & globalstate;
	const unsigned int delta = state ^ current.m_state;

	GlobalOpenGL_debugAssertNoErrors();

	// ── RENDER_TEXT matrix push/pop ──────────────────────────────────────────
	// In Vulkan the "text ortho projection" transform is handled by an overlay
	// pass (Phase 6). For now we just track the state transition.
	if ( delta & state & RENDER_TEXT ) {
		// Phase 6: set up ortho UBO for text overlay
		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_TEXT ) {
		// Phase 6: restore previous projection UBO
		GlobalOpenGL_debugAssertNoErrors();
	}

	// ── Program switching ────────────────────────────────────────────────────
	GLProgram* program = ( state & RENDER_PROGRAM ) != 0 ? self.m_program : 0;

	if ( program != current.m_program ) {
		if ( current.m_program != 0 ) {
			current.m_program->disable();
			debug_colour( "cleaning program" );
		}

		current.m_program = program;

		if ( current.m_program != 0 ) {
			current.m_program->enable();
		}
	}

	// ── Fill / line mode ─────────────────────────────────────────────────────
	// In Vulkan polygon fill/line mode is a pipeline state; tracked in current
	// but no dynamic command is recorded here (Phase 6 adds pipeline switching).
	// no-op: setState( state, delta, RENDER_FILL, ... ) handled above as no-op

	// ── Boolean render flags ─────────────────────────────────────────────────
	setState( state, delta, RENDER_OFFSETLINE,    0 );
	setState( state, delta, RENDER_LIGHTING,      0 );
	setState( state, delta, RENDER_TEXTURE,       0 );
	setState( state, delta, RENDER_BLEND,         0 );

	if ( delta & state & RENDER_TEXTURE ) {
		debug_colour( "setting texture" );
		g_texcoordArray_enabled = true;
	}
	else if ( delta & ~state & RENDER_TEXTURE ) {
		g_texcoordArray_enabled = false;
	}

	if ( delta & state & RENDER_LIGHTING ) {
		g_normalArray_enabled = true;
	}
	else if ( delta & ~state & RENDER_LIGHTING ) {
		g_normalArray_enabled = false;
	}

	setState( state, delta, RENDER_CULLFACE,      0 );
	setState( state, delta, RENDER_SMOOTH,        0 );
	setState( state, delta, RENDER_SCALED,        0 );
	setState( state, delta, RENDER_DEPTHTEST,     0 );
	setState( state, delta, RENDER_COLOURWRITE,   0 );
	setState( state, delta, RENDER_ALPHATEST,     0 );

	if ( delta & state & RENDER_DEPTHWRITE ) {
		debug_string( "enabled depth-buffer writing" );
		GlobalOpenGL_debugAssertNoErrors();
	}
	else if ( delta & ~state & RENDER_DEPTHWRITE ) {
		debug_string( "disabled depth-buffer writing" );
		GlobalOpenGL_debugAssertNoErrors();
	}

	if ( delta & state & RENDER_COLOURARRAY ) {
		debug_colour( "enabling color_array" );
		g_colorArray_enabled = true;
	}
	else if ( delta & ~state & RENDER_COLOURARRAY ) {
		debug_colour( "cleaning color_array" );
		g_colorArray_enabled = false;
	}

	setState( state, delta, RENDER_LINESTIPPLE,   0 );
	setState( state, delta, RENDER_POLYGONSTIPPLE,0 );
	setState( state, delta, RENDER_FOG,           0 );

	if ( ( state & RENDER_FOG ) != 0 ) {
		setFogState( self.m_fog );
		GlobalOpenGL_debugAssertNoErrors();
		current.m_fog = self.m_fog;
	}

	// ── Per-state scalar updates ─────────────────────────────────────────────
	if ( state & RENDER_DEPTHTEST && self.m_depthfunc != current.m_depthfunc ) {
		GlobalOpenGL_debugAssertNoErrors();
		current.m_depthfunc = self.m_depthfunc;
	}

	if ( state & RENDER_LINESTIPPLE
	     && ( self.m_linestipple_factor != current.m_linestipple_factor
	          || self.m_linestipple_pattern != current.m_linestipple_pattern ) ) {
		GlobalOpenGL_debugAssertNoErrors();
		current.m_linestipple_factor = self.m_linestipple_factor;
		current.m_linestipple_pattern = self.m_linestipple_pattern;
	}

	if ( state & RENDER_ALPHATEST
	     && ( self.m_alphafunc != current.m_alphafunc
	          || self.m_alpharef != current.m_alpharef ) ) {
		GlobalOpenGL_debugAssertNoErrors();
		current.m_alphafunc = self.m_alphafunc;
		current.m_alpharef  = self.m_alpharef;
	}

	// ── Texture slots ────────────────────────────────────────────────────────
	{
		uint32_t texture0 = self.m_texture;
		uint32_t texture1 = self.m_texture1;
		uint32_t texture2 = self.m_texture2;
		uint32_t texture3 = self.m_texture3;
		uint32_t texture4 = self.m_texture4;
		uint32_t texture5 = self.m_texture5;
		uint32_t texture6 = self.m_texture6;
		uint32_t texture7 = self.m_texture7;

		setTextureState( current.m_texture,  texture0, GL_TEXTURE0 );
		setTextureState( current.m_texture1, texture1, GL_TEXTURE1 );
		setTextureState( current.m_texture2, texture2, GL_TEXTURE2 );
		setTextureState( current.m_texture3, texture3, GL_TEXTURE3 );
		setTextureState( current.m_texture4, texture4, GL_TEXTURE4 );
		setTextureState( current.m_texture5, texture5, GL_TEXTURE5 );
		setTextureState( current.m_texture6, texture6, GL_TEXTURE6 );
		setTextureState( current.m_texture7, texture7, GL_TEXTURE7 );
	}

	if( current.m_textureSkyBox != self.m_textureSkyBox ){
		GlobalOpenGL_debugAssertNoErrors();
		current.m_textureSkyBox = self.m_textureSkyBox;
	}

	current.m_colour = self.m_colour;

	if ( state & RENDER_BLEND
	     && ( self.m_blend_src != current.m_blend_src || self.m_blend_dst != current.m_blend_dst ) ) {
		GlobalOpenGL_debugAssertNoErrors();
		current.m_blend_src = self.m_blend_src;
		current.m_blend_dst = self.m_blend_dst;
	}

	if ( !( state & RENDER_FILL ) && self.m_linewidth != current.m_linewidth ) {
		// Phase 6: vkCmdSetLineWidth(g_renderCmdBuffer, self.m_linewidth)
		current.m_linewidth = self.m_linewidth;
	}

	if ( !( state & RENDER_FILL ) && self.m_pointsize != current.m_pointsize ) {
		current.m_pointsize = self.m_pointsize;
	}

	current.m_state = state;

	GlobalOpenGL_debugAssertNoErrors();
}

void Renderables_flush( OpenGLStateBucket::Renderables& renderables, OpenGLState& current, unsigned int globalstate, const Vector3& viewer, const Matrix4& viewMatrix ){
	const Matrix4* transform = 0;

	// Sort by transform pointer to group renderables with the same transform.
	std::sort( renderables.begin(), renderables.end(),
		[]( const OpenGLStateBucket::RenderTransform& a, const OpenGLStateBucket::RenderTransform& b ){
			return a.m_transform < b.m_transform;
		} );

	if ( current.m_program != 0 && current.m_textureSkyBox != 0 && globalstate & RENDER_PROGRAM ) {
		current.m_program->setParameters( viewer, g_matrix4_identity, g_vector3_identity, g_vector3_identity, g_matrix4_identity );
	}

	for ( OpenGLStateBucket::Renderables::const_iterator i = renderables.begin(); i != renderables.end(); ++i )
	{
		if ( !transform || ( transform != ( *i ).m_transform && !matrix4_affine_equal( *transform, *( *i ).m_transform ) ) ) {
			count_transform();
			transform = ( *i ).m_transform;

			// Phase 6: write MVP into current program's UBO via:
			//   const Matrix4 combined = matrix4_multiplied_by_matrix4( viewMatrix, *transform );
			//   current.m_program->setMVP( combined );
			// For now we just track the transform pointer.
		}

		count_prim();

		if ( current.m_program != 0 && ( *i ).m_light != 0 ) {
			const IShader& lightShader = static_cast<OpenGLShader*>( ( *i ).m_light->getShader() )->getShader();
			if ( lightShader.firstLayer() != 0 ) {
				GLuint attenuation_xy = lightShader.firstLayer()->texture()->texture_number;
				GLuint attenuation_z = lightShader.lightFalloffImage() != 0
				                       ? lightShader.lightFalloffImage()->texture_number
				                       : static_cast<OpenGLShader*>( g_defaultPointLight )->getShader().lightFalloffImage()->texture_number;

				// Phase 6: bind attenuation textures into descriptor set 1 slots 3 and 4.
				setTextureState( current.m_texture3, attenuation_xy, GL_TEXTURE3 );
				setTextureState( current.m_texture4, attenuation_z, GL_TEXTURE4 );

				AABB lightBounds( ( *i ).m_light->aabb() );

				Matrix4 world2light( g_matrix4_identity );

				if ( ( *i ).m_light->isProjected() ) {
					world2light = ( *i ).m_light->projection();
					matrix4_multiply_by_matrix4( world2light, matrix4_transposed( ( *i ).m_light->rotation() ) );
					matrix4_translate_by_vec3( world2light, vector3_negated( lightBounds.origin ) );
				}
				if ( !( *i ).m_light->isProjected() ) {
					matrix4_translate_by_vec3( world2light, Vector3( 0.5f, 0.5f, 0.5f ) );
					matrix4_scale_by_vec3( world2light, Vector3( 0.5f, 0.5f, 0.5f ) );
					matrix4_scale_by_vec3( world2light, Vector3( 1.0f / lightBounds.extents.x(), 1.0f / lightBounds.extents.y(), 1.0f / lightBounds.extents.z() ) );
					matrix4_multiply_by_matrix4( world2light, matrix4_transposed( ( *i ).m_light->rotation() ) );
					matrix4_translate_by_vec3( world2light, vector3_negated( lightBounds.origin ) );
				}

				current.m_program->setParameters( viewer, *( *i ).m_transform, lightBounds.origin + ( *i ).m_light->offset(), ( *i ).m_light->colour(), world2light );
				debug_string( "set lightBounds parameters" );
			}
		}

		( *i ).m_renderable->render( current.m_state );
	}
	// Phase 6: restore view-matrix UBO slot after flush.
	renderables.clear();
}

void OpenGLStateBucket::render( OpenGLState& current, unsigned int globalstate, const Vector3& viewer, const Matrix4& viewMatrix ){
	if ( ( globalstate & m_state.m_state & RENDER_SCREEN ) != 0 ) {
		OpenGLState_apply( m_state, current, globalstate );
		debug_colour( "screen fill" );

		// Phase 6: Vulkan fullscreen-quad pass.
		// The identity-MVP screen quad uses a streaming VB:
		//   const Vertex3f screenQuad[4] = { {-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0} };
		//   VkDeviceSize offset = vbo_upload(screenQuad, sizeof(screenQuad));
		//   if ( g_renderCmdBuffer ) {
		//       vkCmdBindVertexBuffers( g_renderCmdBuffer, 0, 1, &g_streamVB, &offset );
		//       vkCmdDraw( g_renderCmdBuffer, 4, 1, 0, 0 );
		//   }
	}
	else if ( !m_renderables.empty() ) {
		OpenGLState_apply( m_state, current, globalstate );
		Renderables_flush( m_renderables, current, globalstate, viewer, viewMatrix );
	}
}


class OpenGLStateMap : public OpenGLStateLibrary
{
	typedef std::map<CopiedString, OpenGLState> States;
	States m_states;
public:
	~OpenGLStateMap(){
		ASSERT_MESSAGE( m_states.empty(), "OpenGLStateMap::~OpenGLStateMap: not empty" );
	}

	typedef States::iterator iterator;
	iterator begin(){
		return m_states.begin();
	}
	iterator end(){
		return m_states.end();
	}

	void getDefaultState( OpenGLState& state ) const {
		OpenGLState_constructDefault( state );
	}

	void insert( const char* name, const OpenGLState& state ){
		bool inserted = m_states.insert( States::value_type( name, state ) ).second;
		ASSERT_MESSAGE( inserted, "OpenGLStateMap::insert: " << name << " already exists" );
	}
	void erase( const char* name ){
		std::size_t count = m_states.erase( name );
		ASSERT_MESSAGE( count == 1, "OpenGLStateMap::erase: " << name << " does not exist" );
	}

	iterator find( const char* name ){
		return m_states.find( name );
	}
};

OpenGLStateMap* g_openglStates = 0;

inline uint32_t convertBlendFactor( BlendFactor factor ){
	switch ( factor )
	{
	case BLEND_ZERO:
		return VK_BLEND_FACTOR_ZERO;
	case BLEND_ONE:
		return VK_BLEND_FACTOR_ONE;
	case BLEND_SRC_COLOUR:
		return VK_BLEND_FACTOR_SRC_COLOR;
	case BLEND_ONE_MINUS_SRC_COLOUR:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case BLEND_SRC_ALPHA:
		return VK_BLEND_FACTOR_SRC_ALPHA;
	case BLEND_ONE_MINUS_SRC_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case BLEND_DST_COLOUR:
		return VK_BLEND_FACTOR_DST_COLOR;
	case BLEND_ONE_MINUS_DST_COLOUR:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case BLEND_DST_ALPHA:
		return VK_BLEND_FACTOR_DST_ALPHA;
	case BLEND_ONE_MINUS_DST_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case BLEND_SRC_ALPHA_SATURATE:
		return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	}
	return VK_BLEND_FACTOR_ZERO;
}

/// \todo Define special-case shaders in a data file.
void OpenGLShader::construct( const char* name ){
	OpenGLState& state = appendDefaultPass();
	switch ( name[0] )
	{
	case '{':	//add
		sscanf( name, "{%g %g %g}", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 1.0f;
		state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_BLEND | RENDER_FILL | RENDER_COLOURWRITE /*| RENDER_DEPTHWRITE */| RENDER_LIGHTING;
		state.m_blend_src = GL_ONE;
		state.m_blend_dst = GL_ONE;
//		state.m_blend_src = GL_DST_COLOR;
//		state.m_blend_dst = GL_SRC_COLOR;
//		state.m_blend_src = GL_DST_COLOR;
//		state.m_blend_dst = GL_ONE;
		state.m_sort = OpenGLState::eSortTranslucent;
		break;

	case '(':	//fill
		sscanf( name, "(%g %g %g)", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 1.0f;
		state.m_state = RENDER_FILL | RENDER_LIGHTING | RENDER_DEPTHTEST | RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
		state.m_sort = OpenGLState::eSortFullbright;
		break;

	case '[':	//blend
		sscanf( name, "[%g %g %g]", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 0.5f;
		state.m_state = RENDER_FILL | RENDER_LIGHTING | RENDER_DEPTHTEST | RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_BLEND;
		state.m_sort = OpenGLState::eSortTranslucent;
		break;

	case '<':	//wire
		sscanf( name, "<%g %g %g>", &state.m_colour[0], &state.m_colour[1], &state.m_colour[2] );
		state.m_colour[3] = 1;
		state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
		state.m_sort = OpenGLState::eSortFullbright;
		state.m_depthfunc = GL_LESS;
		state.m_linewidth = 1;
		state.m_pointsize = 1;
		break;

	case '$':
		{
			OpenGLStateMap::iterator i = g_openglStates->find( name );
			if ( i != g_openglStates->end() ) {
				state = ( *i ).second;
				break;
			}
		}
		if ( string_equal( name + 1, "TEXT" ) ) {
			state.m_state = RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_FILL | RENDER_TEXTURE | RENDER_BLEND | RENDER_TEXT;
			state.m_sort = OpenGLState::eSortText;
		}
		else if ( string_equal( name + 1, "POINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst;
			state.m_pointsize = 6;
		}
		else if ( string_equal( name + 1, "DEEPPOINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst;
			state.m_pointsize = 6;

			OpenGLState& hiddenLine = appendDefaultPass(); // glBeginQuery glEndQuery
			hiddenLine.m_state = RENDER_DEPTHTEST;
			hiddenLine.m_sort = OpenGLState::eSortControlFirst - 1;
			hiddenLine.m_pointsize = 6;
			hiddenLine.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "SELPOINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst + 1;
			state.m_pointsize = 4;
		}
		else if ( string_equal( name + 1, "BIGPOINT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortGUI1 + 1;
			state.m_pointsize = 6;
		}
		else if ( string_equal( name + 1, "PIVOT" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_linewidth = 2;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHTEST | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_linewidth = 2;
			hiddenLine.m_depthfunc = GL_GREATER;
		}
		else if ( string_equal( name + 1, "ZIPLINE_DEBUG" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_linewidth = 2;
			state.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "BLENDLINE" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_BLEND;
			state.m_sort = OpenGLState::eSortGUI0 - 1;
			state.m_linewidth = 1;
		}
		else if ( string_equal( name + 1, "LATTICE" ) ) {
			state.m_colour[0] = 1;
			state.m_colour[1] = 0.5;
			state.m_colour[2] = 0;
			state.m_colour[3] = 1;
			state.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortControlFirst;
		}
		else if ( string_equal( name + 1, "WIREFRAME" ) ) {
			state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortFullbright;
		}
		else if ( string_equal( name + 1, "CAM_HIGHLIGHT" ) ) {
			state.m_colour[0] = g_camwindow_globals.color_selbrushes3d[0];
			state.m_colour[1] = g_camwindow_globals.color_selbrushes3d[1];
			state.m_colour[2] = g_camwindow_globals.color_selbrushes3d[2];
			state.m_colour[3] = 0.3f;
			state.m_state = RENDER_FILL | RENDER_DEPTHTEST | RENDER_CULLFACE | RENDER_BLEND | RENDER_COLOURWRITE/* | RENDER_DEPTHWRITE*/;
			state.m_sort = OpenGLState::eSortHighlight;
			state.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "CAM_OVERLAY" ) ) {
#if 0
			state.m_state = RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortOverlayFirst;
#else
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_OFFSETLINE;
			state.m_sort = OpenGLState::eSortOverlayFirst + 1;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_colour[0] = 0.75;
			hiddenLine.m_colour[1] = 0.75;
			hiddenLine.m_colour[2] = 0.75;
			hiddenLine.m_colour[3] = 1;
			hiddenLine.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_OFFSETLINE | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortOverlayFirst;
			hiddenLine.m_depthfunc = GL_GREATER;
			hiddenLine.m_linestipple_factor = 2;
#endif
		}
		else if ( string_equal( name + 1, "CAM_WIRE" ) ) {
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE;// | RENDER_OFFSETLINE;
			state.m_colour[0] = 0.75;
			state.m_colour[1] = 0.75;
			state.m_colour[2] = 0.75;
			state.m_linewidth = 0.5;
			state.m_sort = OpenGLState::eSortOverlayFirst + 1;
			state.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "CAM_FACEWIRE" ) ) {
			state.m_colour[0] = g_camwindow_globals.color_selbrushes3d[0];
			state.m_colour[1] = g_camwindow_globals.color_selbrushes3d[1];
			state.m_colour[2] = g_camwindow_globals.color_selbrushes3d[2];
			state.m_colour[3] = 1;
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_OFFSETLINE;
			state.m_sort = OpenGLState::eSortOverlayFirst + 2;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_colour[0] = g_camwindow_globals.color_selbrushes3d[0];
			hiddenLine.m_colour[1] = g_camwindow_globals.color_selbrushes3d[1];
			hiddenLine.m_colour[2] = g_camwindow_globals.color_selbrushes3d[2];
			hiddenLine.m_colour[3] = 1;
			hiddenLine.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_OFFSETLINE | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortOverlayFirst + 1;
			hiddenLine.m_depthfunc = GL_GREATER;
			hiddenLine.m_linestipple_factor = 2;
		}
		else if ( string_equal( name + 1, "CAM_WORKZONE" ) ) {
			state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_BLEND | RENDER_COLOURARRAY | RENDER_OFFSETLINE | RENDER_SMOOTH;
			state.m_sort = OpenGLState::eSortOverlayFirst + 3;
			state.m_depthfunc = GL_LEQUAL;
		}
		else if ( string_equal( name + 1, "XY_OVERLAY" ) ) {
			state.m_colour[0] = g_xywindow_globals.color_selbrushes[0];
			state.m_colour[1] = g_xywindow_globals.color_selbrushes[1];
			state.m_colour[2] = g_xywindow_globals.color_selbrushes[2];
			state.m_colour[3] = 1;
			state.m_state = RENDER_COLOURWRITE | RENDER_LINESTIPPLE;
			state.m_sort = OpenGLState::eSortOverlayFirst;
			state.m_linewidth = 2;
			state.m_linestipple_factor = 3;
		}
		else if ( string_equal( name + 1, "DEBUG_CLIPPED" ) ) {
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortLast;
		}
		else if ( string_equal( name + 1, "POINTFILE" ) ) {
			state.m_colour[0] = 1;
			state.m_colour[1] = 0;
			state.m_colour[2] = 0;
			state.m_colour[3] = 1;
			state.m_state = RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortFullbright;
			state.m_linewidth = 4;
		}
#if 0
		else if ( string_equal( name + 1, "LIGHT_SPHERE" ) ) {
			state.m_colour[0] = .15f * .95f;
			state.m_colour[1] = .15f * .95f;
			state.m_colour[2] = .15f * .95f;
			state.m_colour[3] = 1;
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_BLEND | RENDER_FILL | RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_blend_src = GL_ONE;
			state.m_blend_dst = GL_ONE;
			state.m_sort = OpenGLState::eSortTranslucent;
		}
		else if ( string_equal( name + 1, "Q3MAP2_LIGHT_SPHERE" ) ) {
			state.m_colour[0] = .05f;
			state.m_colour[1] = .05f;
			state.m_colour[2] = .05f;
			state.m_colour[3] = 1;
			state.m_state = RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_BLEND | RENDER_FILL;
			state.m_blend_src = GL_ONE;
			state.m_blend_dst = GL_ONE;
			state.m_sort = OpenGLState::eSortTranslucent;
		}
#endif // 0
		else if ( string_equal( name + 1, "PLANE_WIRE_OVERLAY" ) ) {
			state.m_colour = Vector4( 1, 1, 0, 1 );
			state.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_OFFSETLINE;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_depthfunc = GL_LEQUAL;
			state.m_linewidth = 2;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_colour = state.m_colour;
			hiddenLine.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_depthfunc = GL_GREATER;
			hiddenLine.m_linestipple_factor = 2;
		}
		else if ( string_equal( name + 1, "WIRE_OVERLAY" ) ) {
#if 0
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST;
			state.m_sort = OpenGLState::eSortOverlayFirst;
#else
			state.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_state = RENDER_COLOURARRAY | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_LINESTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_depthfunc = GL_GREATER;
#endif
		}
		else if ( string_equal( name + 1, "FLATSHADE_OVERLAY" ) ) {
			state.m_state = RENDER_CULLFACE | RENDER_LIGHTING | RENDER_SMOOTH | RENDER_SCALED | RENDER_COLOURARRAY | RENDER_FILL | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_OVERRIDE;
			state.m_sort = OpenGLState::eSortGUI1;
			state.m_depthfunc = GL_LEQUAL;

			OpenGLState& hiddenLine = appendDefaultPass();
			hiddenLine.m_state = RENDER_CULLFACE | RENDER_LIGHTING | RENDER_SMOOTH | RENDER_SCALED | RENDER_COLOURARRAY | RENDER_FILL | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_DEPTHTEST | RENDER_OVERRIDE | RENDER_POLYGONSTIPPLE;
			hiddenLine.m_sort = OpenGLState::eSortGUI0;
			hiddenLine.m_depthfunc = GL_GREATER;
		}
		else if ( string_equal( name + 1, "CLIPPER_OVERLAY" ) ) {
			state.m_colour[0] = g_xywindow_globals.color_clipper[0];
			state.m_colour[1] = g_xywindow_globals.color_clipper[1];
			state.m_colour[2] = g_xywindow_globals.color_clipper[2];
			state.m_colour[3] = 1;
			state.m_state = RENDER_CULLFACE | RENDER_COLOURWRITE | RENDER_DEPTHWRITE | RENDER_FILL | RENDER_POLYGONSTIPPLE;
			state.m_sort = OpenGLState::eSortOverlayFirst;
		}
		else if ( string_equal( name + 1, "OVERBRIGHT" ) ) {
			const float lightScale = 2;
			state.m_colour[0] = lightScale * 0.5f;
			state.m_colour[1] = lightScale * 0.5f;
			state.m_colour[2] = lightScale * 0.5f;
			state.m_colour[3] = 0.5;
			state.m_state = RENDER_FILL | RENDER_BLEND | RENDER_COLOURWRITE | RENDER_SCREEN;
			state.m_sort = OpenGLState::eSortOverbrighten;
			state.m_blend_src = GL_DST_COLOR;
			state.m_blend_dst = GL_SRC_COLOR;
		}
		else
		{
			// default to something recognisable.. =)
			ERROR_MESSAGE( "hardcoded renderstate not found" );
			state.m_colour[0] = 1;
			state.m_colour[1] = 0;
			state.m_colour[2] = 1;
			state.m_colour[3] = 1;
			state.m_state = RENDER_COLOURWRITE | RENDER_DEPTHWRITE;
			state.m_sort = OpenGLState::eSortFirst;
		}
		break;
	default:
		// construction from IShader
		m_shader = QERApp_Shader_ForName( name );

		if ( g_ShaderCache->lightingEnabled() && m_shader->getBump() != 0 && m_shader->getBump()->texture_number != 0 ) { // is a bump shader
			state.m_state = RENDER_FILL | RENDER_CULLFACE | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_COLOURWRITE | RENDER_PROGRAM;
			state.m_colour[0] = 0;
			state.m_colour[1] = 0;
			state.m_colour[2] = 0;
			state.m_colour[3] = 1;
			state.m_sort = OpenGLState::eSortOpaque;

			state.m_program = &g_depthFillGLSL;

			OpenGLState& bumpPass = appendDefaultPass();
			bumpPass.m_texture = m_shader->getDiffuse()->texture_number;
			bumpPass.m_texture1 = m_shader->getBump()->texture_number;
			bumpPass.m_texture2 = m_shader->getSpecular()->texture_number;

			bumpPass.m_state = RENDER_BLEND | RENDER_FILL | RENDER_CULLFACE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_SMOOTH | RENDER_BUMP | RENDER_PROGRAM | RENDER_LIGHTING;

			bumpPass.m_program = &g_bumpGLSL;

			bumpPass.m_depthfunc = GL_LEQUAL;
			bumpPass.m_sort = OpenGLState::eSortMultiFirst;
			bumpPass.m_blend_src = GL_ONE;
			bumpPass.m_blend_dst = GL_ONE;
		}
		else if( m_shader->getSkyBox() != nullptr && m_shader->getSkyBox()->texture_number != 0 )
		{
			state.m_texture = m_shader->getTexture()->texture_number;
			state.m_textureSkyBox = m_shader->getSkyBox()->texture_number;

			state.m_state = RENDER_FILL | RENDER_CULLFACE | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_DEPTHWRITE | RENDER_COLOURWRITE | RENDER_PROGRAM;
			state.m_colour.vec3() = m_shader->getTexture()->color;
			state.m_colour[3] = 1.0f;
			state.m_sort = OpenGLState::eSortFullbright;

			state.m_program = &g_skyboxGLSL;
		}
		else
		{
			state.m_texture = m_shader->getTexture()->texture_number;

			state.m_state = RENDER_FILL | RENDER_TEXTURE | RENDER_DEPTHTEST | RENDER_COLOURWRITE | RENDER_LIGHTING | RENDER_SMOOTH;
			if ( ( m_shader->getFlags() & QER_CULL ) != 0 ) {
				if ( m_shader->getCull() == IShader::eCullBack ) {
					state.m_state |= RENDER_CULLFACE;
				}
			}
			else
			{
				state.m_state |= RENDER_CULLFACE;
			}
			if ( ( m_shader->getFlags() & QER_ALPHATEST ) != 0 ) {
				state.m_state |= RENDER_ALPHATEST;
				IShader::EAlphaFunc alphafunc;
				m_shader->getAlphaFunc( &alphafunc, &state.m_alpharef );
				switch ( alphafunc )
				{
				case IShader::eAlways:
					state.m_alphafunc = GL_ALWAYS;
					break;
				case IShader::eEqual:
					state.m_alphafunc = GL_EQUAL;
					break;
				case IShader::eLess:
					state.m_alphafunc = GL_LESS;
					break;
				case IShader::eGreater:
					state.m_alphafunc = GL_GREATER;
					break;
				case IShader::eLEqual:
					state.m_alphafunc = GL_LEQUAL;
					break;
				case IShader::eGEqual:
					state.m_alphafunc = GL_GEQUAL;
					break;
				}
			}
			state.m_colour.vec3() = m_shader->getTexture()->color;
			state.m_colour[3] = 1.0f;

			if ( ( m_shader->getFlags() & QER_TRANS ) != 0 ) {
				state.m_state |= RENDER_BLEND;
				state.m_colour[3] = m_shader->getTrans();
				state.m_sort = OpenGLState::eSortTranslucent;
				BlendFunc blendFunc = m_shader->getBlendFunc();
				state.m_blend_src = convertBlendFactor( blendFunc.m_src );
				state.m_blend_dst = convertBlendFactor( blendFunc.m_dst );
				state.m_depthfunc = GL_LEQUAL;
//				if ( state.m_blend_src == GL_SRC_ALPHA || state.m_blend_dst == GL_SRC_ALPHA ) {
//					state.m_state |= RENDER_DEPTHWRITE;
//				}
			}
			else
			{
				state.m_state |= RENDER_DEPTHWRITE;
				state.m_sort = OpenGLState::eSortFullbright;
			}
		}
	}
}


#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

class OpenGLStateLibraryAPI
{
	OpenGLStateMap m_stateMap;
public:
	typedef OpenGLStateLibrary Type;
	STRING_CONSTANT( Name, "*" );

	OpenGLStateLibraryAPI(){
		g_openglStates = &m_stateMap;
	}
	~OpenGLStateLibraryAPI(){
		g_openglStates = 0;
	}
	OpenGLStateLibrary* getTable(){
		return &m_stateMap;
	}
};

typedef SingletonModule<OpenGLStateLibraryAPI> OpenGLStateLibraryModule;
typedef Static<OpenGLStateLibraryModule> StaticOpenGLStateLibraryModule;
StaticRegisterModule staticRegisterOpenGLStateLibrary( StaticOpenGLStateLibraryModule::instance() );

class ShaderCacheDependencies : public GlobalShadersModuleRef, public GlobalTexturesModuleRef, public GlobalOpenGLStateLibraryModuleRef
{
public:
	ShaderCacheDependencies() :
		GlobalShadersModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "shaders" ) ){
	}
};

class ShaderCacheAPI
{
	ShaderCache* m_shaderCache;
public:
	typedef ShaderCache Type;
	STRING_CONSTANT( Name, "*" );

	ShaderCacheAPI(){
		ShaderCache_Construct();

		m_shaderCache = GetShaderCache();
	}
	~ShaderCacheAPI(){
		ShaderCache_Destroy();
	}
	ShaderCache* getTable(){
		return m_shaderCache;
	}
};

typedef SingletonModule<ShaderCacheAPI, ShaderCacheDependencies> ShaderCacheModule;
typedef Static<ShaderCacheModule> StaticShaderCacheModule;
StaticRegisterModule staticRegisterShaderCache( StaticShaderCacheModule::instance() );
