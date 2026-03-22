/*
   glwidget.h — Vulkan window context management.

   Replaces the old QOpenGLWidget-based context.
   Callers create a QWindow with VulkanSurface type and call
   glwidget_context_created() on first expose.

   Copyright (C) 2001-2006, William Joseph — original file.
   Vulkan port — MRVN-Radiant contributors.

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

class QVulkanInstance;
class QWindow;

/// Create the shared QVulkanInstance.  Must be called before any QWindow is
/// shown.  Mirrors the old glwidget_setDefaultFormat() which set the OpenGL
/// surface format.
void glwidget_setDefaultFormat();

/// Called when the first Vulkan window becomes visible.  Initialises the
/// Vulkan device / swapchain and fires GLWidget_sharedContextCreated.
void glwidget_context_created( QWindow* window );

/// Called when the last Vulkan window is destroyed.
void glwidget_context_destroyed();

/// Returns the application-wide QVulkanInstance (never null after
/// glwidget_setDefaultFormat() returns successfully).
QVulkanInstance* glwidget_vulkanInstance();

extern void ( *GLWidget_sharedContextCreated  )();
extern void ( *GLWidget_sharedContextDestroyed )();
