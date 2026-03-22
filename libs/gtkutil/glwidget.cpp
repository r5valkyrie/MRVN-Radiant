/*
   glwidget.cpp — Vulkan window context management.

   Initialises the QVulkanInstance once at startup, then for the first Vulkan
   window that appears it calls VKContext_create() to build all Vulkan resources
   (physical device, swapchain, render pass, …) and fires the shared-context
   callback so that the rest of the application can set up its rendering state.

   Copyright (C) 2001-2006, William Joseph — original OpenGL version.
   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#include "glwidget.h"

#include "debugging/debugging.h"

#include "igl.h"            // GlobalVulkan()
#include "ivkcontext.h"     // VKContext_create / VKContext_destroy

#include <QVulkanInstance>
#include <QWindow>
#include <QVersionNumber>

void ( *GLWidget_sharedContextCreated  )() = nullptr;
void ( *GLWidget_sharedContextDestroyed )() = nullptr;

static QVulkanInstance* g_vkInstance    = nullptr;
static unsigned int     g_contextCount  = 0;

QVulkanInstance* glwidget_vulkanInstance()
{
	return g_vkInstance;
}

void glwidget_setDefaultFormat()
{
	g_vkInstance = new QVulkanInstance();
	g_vkInstance->setApiVersion( QVersionNumber( 1, 2 ) );

#ifdef _DEBUG
	g_vkInstance->setLayers( { "VK_LAYER_KHRONOS_validation" } );
#endif

	if ( !g_vkInstance->create() )
	{
		// Fall back to Vulkan 1.0 without validation layers
		delete g_vkInstance;
		g_vkInstance = new QVulkanInstance();
		g_vkInstance->setApiVersion( QVersionNumber( 1, 0 ) );
		const bool ok = g_vkInstance->create();
		ASSERT_MESSAGE( ok, "Failed to create QVulkanInstance" );
	}

	// Store our VkInstance reference before any window is created
	GlobalVulkan().instance = g_vkInstance->vkInstance();

	globalOutputStream() << "Vulkan instance created (API "
	                     << g_vkInstance->apiVersion().majorVersion() << '.'
	                     << g_vkInstance->apiVersion().minorVersion() << ")\n";
}

void glwidget_context_created( QWindow* window )
{
	if ( ++g_contextCount == 1 )
	{
		// Obtain the VkSurfaceKHR from the Qt-managed window.
		VkSurfaceKHR surface = g_vkInstance->surfaceForWindow( window );
		ASSERT_MESSAGE( surface != VK_NULL_HANDLE,
		                "glwidget_context_created: surfaceForWindow returned VK_NULL_HANDLE" );

		VKContext_create( surface,
		                  static_cast<uint32_t>( window->width()  ),
		                  static_cast<uint32_t>( window->height() ) );

		GlobalVulkan().contextValid = true;
		GLWidget_sharedContextCreated();
	}
}

void glwidget_context_destroyed()
{
	if ( --g_contextCount == 0 )
	{
		GlobalVulkan().contextValid = false;
		GLWidget_sharedContextDestroyed();
		VKContext_destroy();
	}
}
