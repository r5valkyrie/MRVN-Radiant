/*
   qgl.cpp — Vulkan module registration (replaces the old OpenGL module).

   Registers the VulkanBinding singleton with the module system under the
   legacy name "qgl" so that all existing module lookups continue to work.

   Copyright (C) 1999-2006, Id Software Inc. — original file.
   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#include "igl.h"            // VulkanBinding, GlobalVulkan()
#include "debugging/debugging.h"

// ── Shutdown helper ───────────────────────────────────────────────────────────

void QGL_Shutdown( VulkanBinding& )
{
	globalOutputStream() << "Shutting down Vulkan module...\n";
}

// ── Error assertion ───────────────────────────────────────────────────────────

void VK_assertNoErrors( const char* file, int line )
{
	// In Vulkan, all errors are returned as VkResult from API calls.
	// Any error that was not checked at call-site will have already triggered
	// ERROR_MESSAGE via VK_CHECK() in vkcontext / vktexture.  Nothing to poll.
	(void)file; (void)line;
}

// ── Anisotropy query (replaces QGL_maxTextureAnisotropy) ─────────────────────

float QGL_maxTextureAnisotropy()
{
	return GlobalVulkan().maxAnisotropy;
}

// ── Module registration ───────────────────────────────────────────────────────

class VkAPI
{
	VulkanBinding m_vk;
public:
	typedef VulkanBinding Type;
	STRING_CONSTANT( Name, "*" );

	VkAPI()
	{
		m_vk.assertNoErrors = &VK_assertNoErrors;
	}
	~VkAPI()
	{
		QGL_Shutdown( m_vk );
	}
	VulkanBinding* getTable()
	{
		return &m_vk;
	}
};

#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

typedef SingletonModule<VkAPI> VkModule;
typedef Static<VkModule> StaticVkModule;
StaticRegisterModule staticRegisterVk( StaticVkModule::instance() );

// ── QGL_sharedContext stubs (called from mainframe.cpp) ───────────────────────
// In the Vulkan port these are no-ops — context lifecycle is handled by
// glwidget_context_created / glwidget_context_destroyed in glwidget.cpp.

void QGL_sharedContextCreated( OpenGLBinding& )
{
	// Phase 6: Vulkan resources already initialised in glwidget_context_created()
}

void QGL_sharedContextDestroyed( OpenGLBinding& )
{
	// Phase 6: Vulkan teardown handled in glwidget_context_destroyed()
}

