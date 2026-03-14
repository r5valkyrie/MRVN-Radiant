/* -------------------------------------------------------------------------------

   Copyright (C) 2022-2025 MRVN-Radiant and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of MRVN-Radiant.

   MRVN-Radiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   MRVN-Radiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

   ------------------------------------------------------------------------------- */

/*
    Apex Legends Lightmap Implementation

    This file implements lightmap generation for Apex Legends BSPs.
    
    Lightmap format (Type 1, uncompressed HDR):
    - 8 bytes per texel
    - Layout: Each texel stores HDR lighting data
    
    Process:
    1. SetupSurfaceLightmaps() - Allocate UV space for each lit surface
    2. ComputeLightmapLighting() - Ray trace from worldlights to compute lighting
    3. EmitLightmaps() - Encode to HDR format and write to BSP lumps
*/

#include "../remap.h"
#include "../bspfile_abstract.h"
#include "../hiprt_trace.h"
#include "apex_legends.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Maximum lightmap page dimensions
constexpr uint16_t MAX_LIGHTMAP_WIDTH = 1024;
constexpr uint16_t MAX_LIGHTMAP_HEIGHT = 1024;

// Lightmap texel density (units per texel)
constexpr float LIGHTMAP_SAMPLE_SIZE = 16.0f;

// Minimum allocation size for lightmap rectangles
constexpr int MIN_LIGHTMAP_WIDTH = 4;
constexpr int MIN_LIGHTMAP_HEIGHT = 4;

// Radiosity settings
constexpr int RADIOSITY_BOUNCES = 0;        // Number of light bounces (0 = direct only)
constexpr float RADIOSITY_SCALE = 0.5f;     // Energy retention per bounce
constexpr int RADIOSITY_SAMPLES = 32;       // Hemisphere samples for indirect lighting

// Adaptive supersampling settings (from Source SDK vrad2 ComputeLightmapGradients/BuildSupersampleFaceLights)
constexpr int SUPERSAMPLE_PASSES = 2;                       // Max iterative refinement passes
constexpr float SUPERSAMPLE_GRADIENT_THRESHOLD = 0.0625f;   // Gradient threshold to trigger supersampling
constexpr int SUPERSAMPLE_GRID = 4;                         // 4x4 sub-sample grid per texel

// Ambient occlusion settings for indirect lightmap channel
constexpr int AO_HEMISPHERE_RAYS = 48;            // Hemisphere ray count for sky AO per texel
constexpr float AO_TRACE_DISTANCE = 2048.0f;     // Max trace distance for AO rays
constexpr float INDIRECT_AMBIENT_SCALE = 0.5f;   // Scale factor for sky ambient in indirect channel

// Light probe settings (adapted from Source SDK leaf_ambient_lighting.cpp)
constexpr int LIGHT_PROBE_GRID_SPACING = 256;   // Units between probes on grid
constexpr int LIGHT_PROBE_MIN_SPACING = 128;    // Minimum spacing between probes
constexpr int LIGHT_PROBE_MAX_PER_AXIS = 64;    // Max probes per axis (prevent explosion)
constexpr int LIGHT_PROBE_MAX_COUNT = -1;       // Maximum total light probes (-1 = unlimited)
constexpr float LIGHT_PROBE_TRACE_DIST = 64000.0f;  // Ray trace distance


// =============================================================================
// SPHERICAL SAMPLING DIRECTIONS (from Source SDK anorms.h)
// 162 uniformly distributed directions for ambient cube sampling
// This provides much better coverage than 8 upward rays
// =============================================================================
constexpr int NUM_SPHERE_NORMALS = 162;

// Pre-computed uniformly distributed unit vectors over sphere (Fibonacci lattice)
static const Vector3 g_SphereNormals[NUM_SPHERE_NORMALS] = {
    // These are 162 uniformly distributed directions over the unit sphere
    // Generated using Fibonacci sphere distribution for optimal coverage
    {-0.525731f, 0.000000f, 0.850651f}, {-0.442863f, 0.238856f, 0.864188f},
    {-0.295242f, 0.000000f, 0.955423f}, {-0.309017f, 0.500000f, 0.809017f},
    {-0.162460f, 0.262866f, 0.951056f}, {0.000000f, 0.000000f, 1.000000f},
    {0.000000f, 0.850651f, 0.525731f}, {-0.147621f, 0.716567f, 0.681718f},
    {0.147621f, 0.716567f, 0.681718f}, {0.000000f, 0.525731f, 0.850651f},
    {0.309017f, 0.500000f, 0.809017f}, {0.525731f, 0.000000f, 0.850651f},
    {0.295242f, 0.000000f, 0.955423f}, {0.442863f, 0.238856f, 0.864188f},
    {0.162460f, 0.262866f, 0.951056f}, {-0.681718f, 0.147621f, 0.716567f},
    {-0.809017f, 0.309017f, 0.500000f}, {-0.587785f, 0.425325f, 0.688191f},
    {-0.850651f, 0.525731f, 0.000000f}, {-0.864188f, 0.442863f, 0.238856f},
    {-0.716567f, 0.681718f, 0.147621f}, {-0.688191f, 0.587785f, 0.425325f},
    {-0.500000f, 0.809017f, 0.309017f}, {-0.238856f, 0.864188f, 0.442863f},
    {-0.425325f, 0.688191f, 0.587785f}, {-0.716567f, 0.681718f, -0.147621f},
    {-0.500000f, 0.809017f, -0.309017f}, {-0.525731f, 0.850651f, 0.000000f},
    {0.000000f, 0.850651f, -0.525731f}, {-0.238856f, 0.864188f, -0.442863f},
    {0.000000f, 0.955423f, -0.295242f}, {-0.262866f, 0.951056f, -0.162460f},
    {0.000000f, 1.000000f, 0.000000f}, {0.000000f, 0.955423f, 0.295242f},
    {-0.262866f, 0.951056f, 0.162460f}, {0.238856f, 0.864188f, 0.442863f},
    {0.262866f, 0.951056f, 0.162460f}, {0.500000f, 0.809017f, 0.309017f},
    {0.238856f, 0.864188f, -0.442863f}, {0.262866f, 0.951056f, -0.162460f},
    {0.500000f, 0.809017f, -0.309017f}, {0.850651f, 0.525731f, 0.000000f},
    {0.716567f, 0.681718f, 0.147621f}, {0.716567f, 0.681718f, -0.147621f},
    {0.525731f, 0.850651f, 0.000000f}, {0.425325f, 0.688191f, 0.587785f},
    {0.864188f, 0.442863f, 0.238856f}, {0.688191f, 0.587785f, 0.425325f},
    {0.809017f, 0.309017f, 0.500000f}, {0.681718f, 0.147621f, 0.716567f},
    {0.587785f, 0.425325f, 0.688191f}, {0.955423f, 0.295242f, 0.000000f},
    {1.000000f, 0.000000f, 0.000000f}, {0.951056f, 0.162460f, 0.262866f},
    {0.850651f, -0.525731f, 0.000000f}, {0.955423f, -0.295242f, 0.000000f},
    {0.864188f, -0.442863f, 0.238856f}, {0.951056f, -0.162460f, 0.262866f},
    {0.809017f, -0.309017f, 0.500000f}, {0.681718f, -0.147621f, 0.716567f},
    {0.850651f, 0.000000f, 0.525731f}, {0.864188f, 0.442863f, -0.238856f},
    {0.809017f, 0.309017f, -0.500000f}, {0.951056f, 0.162460f, -0.262866f},
    {0.525731f, 0.000000f, -0.850651f}, {0.681718f, 0.147621f, -0.716567f},
    {0.681718f, -0.147621f, -0.716567f}, {0.850651f, 0.000000f, -0.525731f},
    {0.809017f, -0.309017f, -0.500000f}, {0.864188f, -0.442863f, -0.238856f},
    {0.951056f, -0.162460f, -0.262866f}, {0.147621f, 0.716567f, -0.681718f},
    {0.309017f, 0.500000f, -0.809017f}, {0.425325f, 0.688191f, -0.587785f},
    {0.442863f, 0.238856f, -0.864188f}, {0.587785f, 0.425325f, -0.688191f},
    {0.688191f, 0.587785f, -0.425325f}, {-0.147621f, 0.716567f, -0.681718f},
    {-0.309017f, 0.500000f, -0.809017f}, {0.000000f, 0.525731f, -0.850651f},
    {-0.525731f, 0.000000f, -0.850651f}, {-0.442863f, 0.238856f, -0.864188f},
    {-0.295242f, 0.000000f, -0.955423f}, {-0.162460f, 0.262866f, -0.951056f},
    {0.000000f, 0.000000f, -1.000000f}, {0.295242f, 0.000000f, -0.955423f},
    {0.162460f, 0.262866f, -0.951056f}, {-0.442863f, -0.238856f, -0.864188f},
    {-0.309017f, -0.500000f, -0.809017f}, {-0.162460f, -0.262866f, -0.951056f},
    {0.000000f, -0.850651f, -0.525731f}, {-0.147621f, -0.716567f, -0.681718f},
    {0.147621f, -0.716567f, -0.681718f}, {0.000000f, -0.525731f, -0.850651f},
    {0.309017f, -0.500000f, -0.809017f}, {0.442863f, -0.238856f, -0.864188f},
    {0.162460f, -0.262866f, -0.951056f}, {0.238856f, -0.864188f, -0.442863f},
    {0.500000f, -0.809017f, -0.309017f}, {0.425325f, -0.688191f, -0.587785f},
    {0.716567f, -0.681718f, -0.147621f}, {0.688191f, -0.587785f, -0.425325f},
    {0.587785f, -0.425325f, -0.688191f}, {0.000000f, -0.955423f, -0.295242f},
    {0.000000f, -1.000000f, 0.000000f}, {0.262866f, -0.951056f, -0.162460f},
    {0.000000f, -0.850651f, 0.525731f}, {0.000000f, -0.955423f, 0.295242f},
    {0.238856f, -0.864188f, 0.442863f}, {0.262866f, -0.951056f, 0.162460f},
    {0.500000f, -0.809017f, 0.309017f}, {0.716567f, -0.681718f, 0.147621f},
    {0.525731f, -0.850651f, 0.000000f}, {-0.238856f, -0.864188f, -0.442863f},
    {-0.500000f, -0.809017f, -0.309017f}, {-0.262866f, -0.951056f, -0.162460f},
    {-0.850651f, -0.525731f, 0.000000f}, {-0.716567f, -0.681718f, -0.147621f},
    {-0.716567f, -0.681718f, 0.147621f}, {-0.525731f, -0.850651f, 0.000000f},
    {-0.500000f, -0.809017f, 0.309017f}, {-0.238856f, -0.864188f, 0.442863f},
    {-0.262866f, -0.951056f, 0.162460f}, {-0.864188f, -0.442863f, 0.238856f},
    {-0.809017f, -0.309017f, 0.500000f}, {-0.688191f, -0.587785f, 0.425325f},
    {-0.681718f, -0.147621f, 0.716567f}, {-0.442863f, -0.238856f, 0.864188f},
    {-0.587785f, -0.425325f, 0.688191f}, {-0.309017f, -0.500000f, 0.809017f},
    {-0.147621f, -0.716567f, 0.681718f}, {-0.425325f, -0.688191f, 0.587785f},
    {0.147621f, -0.716567f, 0.681718f}, {0.309017f, -0.500000f, 0.809017f},
    {0.442863f, -0.238856f, 0.864188f}, {0.587785f, -0.425325f, 0.688191f},
    {0.688191f, -0.587785f, 0.425325f}, {0.864188f, -0.442863f, -0.238856f},
    {0.809017f, -0.309017f, -0.500000f}, {0.688191f, -0.587785f, -0.425325f},
    {-0.681718f, -0.147621f, -0.716567f}, {-0.864188f, -0.442863f, -0.238856f},
    {-0.809017f, -0.309017f, -0.500000f}, {-0.688191f, -0.587785f, -0.425325f},
    {-0.681718f, 0.147621f, -0.716567f}, {-0.850651f, 0.000000f, -0.525731f},
    {-0.587785f, -0.425325f, -0.688191f}, {-0.425325f, -0.688191f, -0.587785f},
    {-0.587785f, 0.425325f, -0.688191f}, {-0.425325f, 0.688191f, -0.587785f},
    {-0.955423f, 0.295242f, 0.000000f}, {-0.951056f, 0.162460f, 0.262866f},
    {-1.000000f, 0.000000f, 0.000000f}, {-0.850651f, 0.000000f, 0.525731f},
    {-0.955423f, -0.295242f, 0.000000f}, {-0.951056f, -0.162460f, 0.262866f},
    {-0.864188f, 0.442863f, -0.238856f}, {-0.951056f, 0.162460f, -0.262866f},
    {-0.809017f, 0.309017f, -0.500000f}, {-0.864188f, -0.442863f, -0.238856f},
    {-0.951056f, -0.162460f, -0.262866f}, {-0.809017f, -0.309017f, -0.500000f},
};

// 6 box directions for ambient cube (+X, -X, +Y, -Y, +Z, -Z)
static const Vector3 g_BoxDirections[6] = {
    {1.0f, 0.0f, 0.0f},   // +X
    {-1.0f, 0.0f, 0.0f},  // -X
    {0.0f, 1.0f, 0.0f},   // +Y
    {0.0f, -1.0f, 0.0f},  // -Y
    {0.0f, 0.0f, 1.0f},   // +Z
    {0.0f, 0.0f, -1.0f},  // -Z
};





// Global radiosity patch data
namespace RadiosityData {
    struct Patch_t {
        Vector3 origin;         // Center of patch
        Vector3 normal;         // Patch normal
        Vector3 reflectivity;   // Surface reflectivity (from texture)
        Vector3 totalLight;     // Accumulated direct + indirect light
        Vector3 directLight;    // Direct lighting only
        float area;             // Patch area in world units
        int meshIndex;          // Which mesh this patch belongs to
        int luxelIndex;         // Which luxel this is
    };
    
    std::vector<Patch_t> patches;
    bool initialized = false;
}


/*
    LightmapRect_t
    Represents a rectangular region in a lightmap atlas
*/
struct LightmapRect_t {
    int x, y;           // Position in atlas
    int width, height;  // Size in texels
    int pageIndex;      // Which lightmap page this is on
};


/*
    SurfaceLightmap_t
    Per-surface lightmap data during building
*/
struct SurfaceLightmap_t {
    int meshIndex;              // Index into Shared::meshes
    LightmapRect_t rect;        // Allocated rectangle
    MinMax worldBounds;         // World-space bounds
    Plane3f plane;              // Surface plane
    Vector3 tangent;            // U direction in world space
    Vector3 bitangent;          // V direction in world space
    float uMin, uMax;           // Tangent-space U bounds (for proper UV normalization)
    float vMin, vMax;           // Tangent-space V bounds (for proper UV normalization)
    std::vector<Vector3> luxels;         // Per-texel direct lighting (RGB HDR)
    std::vector<Vector3> indirectLuxels;  // Per-texel indirect/ambient lighting (RGB HDR)
};


// Global state for lightmap building
namespace LightmapBuild {
    std::vector<SurfaceLightmap_t> surfaces;
    std::vector<std::vector<bool>> atlasUsed;  // Per-page usage bitmap
    int currentPage = 0;
    int pageRowHeight = 0;      // Current row height for simple packing
    int pageCursorX = 0;        // Current X position in row
    int pageCursorY = 0;        // Current Y position (row start)
}


/*
    InitLightmapAtlas
    Initialize a new lightmap atlas page with neutral base lighting.
    
    The engine applies emit_skyambient and emit_skylight dynamically from
    worldLights lump, so lightmaps should be neutral - not pre-baked with
    ambient color. This allows _ambient/_light changes in .ent files to
    take effect immediately in-game.
*/
static void InitLightmapAtlas() {
    ApexLegends::LightmapPage_t page;
    page.width = MAX_LIGHTMAP_WIDTH;
    page.height = MAX_LIGHTMAP_HEIGHT;
    
    // Initialize unallocated texels with neutral ambient values.
    // Official lightmaps use signed exponent byte 0 (multiplier 1.0x) for shadow/ambient.
    // Computed texels overwrite these; unallocated texels stay at neutral.
    // Engine requires non-zero lightmap data to enable lightmap rendering.
    size_t dataSize = page.width * page.height * 8;
    page.pixels.resize(dataSize);
    for (size_t i = 0; i < dataSize; i += 8) {
        // Direct light: neutral gray, signed exp=0 (byte 0, multiplier 1.0x)
        page.pixels[i + 0] = 128;
        page.pixels[i + 1] = 128;
        page.pixels[i + 2] = 128;
        page.pixels[i + 3] = 128;   // signed exp 0 -> 2^0 = 1.0
        // Indirect light: neutral tinted gray, signed exp=0
        page.pixels[i + 4] = 128;
        page.pixels[i + 5] = 128;
        page.pixels[i + 6] = 128;
        page.pixels[i + 7] = 128;   // signed exp 0 -> 2^0 = 1.0
    }
    ApexLegends::Bsp::lightmapPages.push_back(page);
    
    // Initialize usage bitmap
    std::vector<bool> used(MAX_LIGHTMAP_WIDTH * MAX_LIGHTMAP_HEIGHT, false);
    LightmapBuild::atlasUsed.push_back(used);
    
    LightmapBuild::pageCursorX = 0;
    LightmapBuild::pageCursorY = 0;
    LightmapBuild::pageRowHeight = 0;
}

/*
    AllocateLightmapRect
    Simple row-based packing algorithm
    Returns true if allocation succeeded
*/
static bool AllocateLightmapRect(int width, int height, LightmapRect_t &rect) {
    // Ensure we have at least one page
    if (ApexLegends::Bsp::lightmapPages.empty()) {
        InitLightmapAtlas();
    }
    
    // Try to fit in current row
    if (LightmapBuild::pageCursorX + width <= MAX_LIGHTMAP_WIDTH &&
        LightmapBuild::pageCursorY + height <= MAX_LIGHTMAP_HEIGHT) {
        
        rect.x = LightmapBuild::pageCursorX;
        rect.y = LightmapBuild::pageCursorY;
        rect.width = width;
        rect.height = height;
        rect.pageIndex = LightmapBuild::currentPage;
        
        LightmapBuild::pageCursorX += width;
        LightmapBuild::pageRowHeight = std::max(LightmapBuild::pageRowHeight, height);
        
        return true;
    }
    
    // Try to start a new row
    if (width <= MAX_LIGHTMAP_WIDTH &&
        LightmapBuild::pageCursorY + LightmapBuild::pageRowHeight + height <= MAX_LIGHTMAP_HEIGHT) {
        
        LightmapBuild::pageCursorY += LightmapBuild::pageRowHeight;
        LightmapBuild::pageCursorX = 0;
        LightmapBuild::pageRowHeight = 0;
        
        rect.x = 0;
        rect.y = LightmapBuild::pageCursorY;
        rect.width = width;
        rect.height = height;
        rect.pageIndex = LightmapBuild::currentPage;
        
        LightmapBuild::pageCursorX = width;
        LightmapBuild::pageRowHeight = height;
        
        return true;
    }
    
    // Need a new page
    LightmapBuild::currentPage++;
    InitLightmapAtlas();
    
    if (width > MAX_LIGHTMAP_WIDTH || height > MAX_LIGHTMAP_HEIGHT) {
        // Surface is too large - clamp it
        Sys_Warning("Surface too large for lightmap: %dx%d\n", width, height);
        width = std::min(width, (int)MAX_LIGHTMAP_WIDTH);
        height = std::min(height, (int)MAX_LIGHTMAP_HEIGHT);
    }
    
    rect.x = 0;
    rect.y = 0;
    rect.width = width;
    rect.height = height;
    rect.pageIndex = LightmapBuild::currentPage;
    
    LightmapBuild::pageCursorX = width;
    LightmapBuild::pageRowHeight = height;
    
    return true;
}


/*
    ComputeSurfaceBasis
    Compute tangent/bitangent vectors for a planar surface
*/
static void ComputeSurfaceBasis(const Plane3f &plane, Vector3 &tangent, Vector3 &bitangent) {
    Vector3 normal = plane.normal();
    
    // Choose an arbitrary vector not parallel to normal
    Vector3 up(0, 0, 1);
    if (std::abs(vector3_dot(normal, up)) > 0.9f) {
        up = Vector3(1, 0, 0);
    }
    
    // Compute tangent and bitangent
    tangent = vector3_normalised(vector3_cross(up, normal));
    bitangent = vector3_normalised(vector3_cross(normal, tangent));
}


/*
    SetupSurfaceLightmaps
    Allocate lightmap space for each lit surface and compute UV mappings
*/
void ApexLegends::SetupSurfaceLightmaps() {
    LightmapBuild::surfaces.clear();
    LightmapBuild::atlasUsed.clear();
    ApexLegends::Bsp::lightmapPages.clear();
    LightmapBuild::currentPage = 0;
    LightmapBuild::pageCursorX = 0;
    LightmapBuild::pageCursorY = 0;
    LightmapBuild::pageRowHeight = 0;
    
    int meshIndex = 0;
    int litSurfaces = 0;
    
    for (const Shared::Mesh_t &mesh : Shared::meshes) {
        // Only process lit surfaces (LIT_BUMP vertex type)
        if (!CHECK_FLAG(mesh.shaderInfo->surfaceFlags, S_VERTEX_LIT_BUMP)) {
            meshIndex++;
            continue;
        }
        
        // Compute surface bounds
        MinMax bounds;
        for (const Shared::Vertex_t &vert : mesh.vertices) {
            bounds.extend(vert.xyz);
        }
        
        // Compute surface plane from first triangle
        if (mesh.triangles.size() < 3) {
            meshIndex++;
            continue;
        }
        
        const Vector3 &v0 = mesh.vertices[mesh.triangles[0]].xyz;
        const Vector3 &v1 = mesh.vertices[mesh.triangles[1]].xyz;
        const Vector3 &v2 = mesh.vertices[mesh.triangles[2]].xyz;
        
        Vector3 edge1 = v1 - v0;
        Vector3 edge2 = v2 - v0;
        Vector3 normal = vector3_normalised(vector3_cross(edge1, edge2));
        float dist = vector3_dot(normal, v0);
        Plane3f plane(normal, dist);
        
        // Compute tangent basis
        Vector3 tangent, bitangent;
        ComputeSurfaceBasis(plane, tangent, bitangent);
        
        // Calculate lightmap size based on surface area in tangent space
        // Track both min and max to properly handle surfaces where vertices
        // can be on the negative side of the tangent/bitangent from bounds.mins
        float uMin = FLT_MAX, uMax = -FLT_MAX;
        float vMin = FLT_MAX, vMax = -FLT_MAX;
        for (const Shared::Vertex_t &vert : mesh.vertices) {
            Vector3 localPos = vert.xyz - bounds.mins;
            float u = vector3_dot(localPos, tangent);
            float v = vector3_dot(localPos, bitangent);
            uMin = std::min(uMin, u);
            uMax = std::max(uMax, u);
            vMin = std::min(vMin, v);
            vMax = std::max(vMax, v);
        }
        
        float uExtent = uMax - uMin;
        float vExtent = vMax - vMin;
        
        int lmWidth = std::max(MIN_LIGHTMAP_WIDTH, (int)std::ceil(uExtent / LIGHTMAP_SAMPLE_SIZE) + 1);
        int lmHeight = std::max(MIN_LIGHTMAP_HEIGHT, (int)std::ceil(vExtent / LIGHTMAP_SAMPLE_SIZE) + 1);
        
        // Allocate rectangle
        LightmapRect_t rect;
        if (!AllocateLightmapRect(lmWidth, lmHeight, rect)) {
            Sys_Warning("Failed to allocate lightmap for mesh %d\n", meshIndex);
            meshIndex++;
            continue;
        }
        
        // Create surface lightmap entry
        SurfaceLightmap_t surfLM;
        surfLM.meshIndex = meshIndex;
        surfLM.rect = rect;
        surfLM.worldBounds = bounds;
        surfLM.plane = plane;
        surfLM.tangent = tangent;
        surfLM.bitangent = bitangent;
        surfLM.uMin = uMin;
        surfLM.uMax = uMax;
        surfLM.vMin = vMin;
        surfLM.vMax = vMax;
        surfLM.luxels.resize(rect.width * rect.height, Vector3(0, 0, 0));
        surfLM.indirectLuxels.resize(rect.width * rect.height, Vector3(0, 0, 0));
        
        LightmapBuild::surfaces.push_back(surfLM);
        litSurfaces++;
        meshIndex++;
    }
    
    if (litSurfaces > 0) {
        Sys_Printf("     %9d lit surfaces\n", litSurfaces);
        Sys_Printf("     %9d lightmap pages\n", (int)ApexLegends::Bsp::lightmapPages.size());
    }
}


// =============================================================================
// RADIOSITY / BOUNCE LIGHTING
// Compute indirect illumination from light bouncing off surfaces
// =============================================================================

/*
    InitRadiosityPatches
    Create patches from all lightmap luxels for radiosity computation.
    Now uses actual texture colors for surface reflectivity to enable color bleeding.
*/
static void InitRadiosityPatches() {
    if (RadiosityData::initialized) return;
    
    RadiosityData::patches.clear();
    
    for (const SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        const Shared::Mesh_t &mesh = Shared::meshes[surf.meshIndex];
        
        // Get surface reflectivity from texture colors for accurate color bleeding
        Vector3 reflectivity(0.5f, 0.5f, 0.5f);  // Default 50% neutral reflectance
        if (mesh.shaderInfo) {
            // Try to get average color from the shader's texture image
            if (mesh.shaderInfo->averageColor[0] > 0 || 
                mesh.shaderInfo->averageColor[1] > 0 || 
                mesh.shaderInfo->averageColor[2] > 0) {
                // Convert from 0-255 to 0-1 range, clamp to reasonable reflectivity
                reflectivity[0] = std::min(0.9f, mesh.shaderInfo->averageColor[0] / 255.0f);
                reflectivity[1] = std::min(0.9f, mesh.shaderInfo->averageColor[1] / 255.0f);
                reflectivity[2] = std::min(0.9f, mesh.shaderInfo->averageColor[2] / 255.0f);
            } else if (mesh.shaderInfo->color[0] > 0 || 
                       mesh.shaderInfo->color[1] > 0 || 
                       mesh.shaderInfo->color[2] > 0) {
                // Use shader's explicit color as fallback
                reflectivity = mesh.shaderInfo->color;
                // Clamp to valid reflectivity range
                reflectivity[0] = std::min(0.9f, std::max(0.1f, reflectivity[0]));
                reflectivity[1] = std::min(0.9f, std::max(0.1f, reflectivity[1]));
                reflectivity[2] = std::min(0.9f, std::max(0.1f, reflectivity[2]));
            }
        }
        
        for (int y = 0; y < surf.rect.height; y++) {
            for (int x = 0; x < surf.rect.width; x++) {
                RadiosityData::Patch_t patch;
                
                // Compute world position from texel coordinates
                // Normalize texel to [0,1] within the rect, then map to tangent-space bounds
                float normalizedU = (surf.rect.width > 1) ? (float)x / (surf.rect.width - 1) : 0.5f;
                float normalizedV = (surf.rect.height > 1) ? (float)y / (surf.rect.height - 1) : 0.5f;
                float localU = surf.uMin + normalizedU * (surf.uMax - surf.uMin);
                float localV = surf.vMin + normalizedV * (surf.vMax - surf.vMin);
                patch.origin = surf.worldBounds.mins 
                    + surf.tangent * localU 
                    + surf.bitangent * localV
                    + surf.plane.normal() * 0.1f;
                
                patch.normal = surf.plane.normal();
                patch.reflectivity = reflectivity;
                patch.totalLight = Vector3(0, 0, 0);
                patch.directLight = Vector3(0, 0, 0);
                patch.area = LIGHTMAP_SAMPLE_SIZE * LIGHTMAP_SAMPLE_SIZE;
                patch.meshIndex = surf.meshIndex;
                patch.luxelIndex = y * surf.rect.width + x;
                
                // For more accurate results, sample texture at this specific luxel position
                // This allows per-texel color variation for detailed color bleeding
                if (mesh.shaderInfo && mesh.shaderInfo->shaderImage && 
                    mesh.shaderInfo->shaderImage->pixels &&
                    mesh.shaderInfo->shaderImage->width > 0 && 
                    mesh.shaderInfo->shaderImage->height > 0) {
                    
                    // Calculate UV at this patch position
                    // Use mesh vertices to find approximate UV for this world position
                    // For now, sample at a normalized UV based on surface position
                    Vector2 texelUV;
                    texelUV[0] = normalizedU;
                    texelUV[1] = normalizedV;
                    
                    Color4f texColor;
                    if (RadSampleImage(mesh.shaderInfo->shaderImage->pixels,
                                      mesh.shaderInfo->shaderImage->width,
                                      mesh.shaderInfo->shaderImage->height,
                                      texelUV, texColor)) {
                        // Use texture color as reflectivity
                        patch.reflectivity[0] = std::min(0.9f, texColor[0] / 255.0f);
                        patch.reflectivity[1] = std::min(0.9f, texColor[1] / 255.0f);
                        patch.reflectivity[2] = std::min(0.9f, texColor[2] / 255.0f);
                    }
                }
                
                RadiosityData::patches.push_back(patch);
            }
        }
    }
    
    RadiosityData::initialized = true;
}


/*
    ComputeFormFactor
    Compute the form factor between two patches (simplified)
    This is the fraction of energy leaving patch A that reaches patch B
*/
static float ComputeFormFactor(const RadiosityData::Patch_t &from, 
                               const RadiosityData::Patch_t &to) {
    Vector3 delta = to.origin - from.origin;
    float distSq = vector3_dot(delta, delta);
    
    if (distSq < 1.0f) return 0.0f;
    
    float dist = std::sqrt(distSq);
    Vector3 dir = delta / dist;
    
    // Cosine of angle from sender
    float cosFrom = vector3_dot(from.normal, dir);
    if (cosFrom <= 0) return 0.0f;
    
    // Cosine of angle to receiver
    float cosTo = vector3_dot(to.normal, -dir);
    if (cosTo <= 0) return 0.0f;
    
    // Form factor: (cos_a * cos_b * area) / (pi * r^2)
    float ff = (cosFrom * cosTo * to.area) / (M_PI * distSq);
    
    return std::min(1.0f, ff);
}

// Forward declaration for TraceRayAgainstMeshes (defined later in file)
static bool TraceRayAgainstMeshes(const Vector3 &origin, const Vector3 &dir, float maxDist);


/*
    GatherRadiosityLight
    Gather indirect light from surrounding patches (one bounce iteration)
*/
static void GatherRadiosityLight(int bounceNum) {
    if (RadiosityData::patches.empty()) return;
    
    Sys_Printf("     Radiosity bounce %d (%zu patches)...\n", bounceNum, RadiosityData::patches.size());
    
    // Store incoming light for this bounce
    std::vector<Vector3> incomingLight(RadiosityData::patches.size(), Vector3(0, 0, 0));
    
    // For each receiving patch
    for (size_t i = 0; i < RadiosityData::patches.size(); i++) {
        RadiosityData::Patch_t &receiver = RadiosityData::patches[i];
        Vector3 gathered(0, 0, 0);
        
        // Sample a subset of sender patches (for performance)
        int sampleStep = std::max(1, static_cast<int>(RadiosityData::patches.size() / RADIOSITY_SAMPLES));
        
        for (size_t j = 0; j < RadiosityData::patches.size(); j += sampleStep) {
            if (i == j) continue;
            
            const RadiosityData::Patch_t &sender = RadiosityData::patches[j];
            
            // Skip if sender has no light to give
            float senderEnergy = sender.totalLight.x() + sender.totalLight.y() + sender.totalLight.z();
            if (senderEnergy < 0.001f) continue;
            
            float ff = ComputeFormFactor(receiver, sender);
            if (ff < 0.0001f) continue;
            
            // Optional: visibility check (expensive but more accurate)
            // For performance, we skip this for most samples
            if ((i + j) % 8 == 0) {
                Vector3 dir = vector3_normalised(sender.origin - receiver.origin);
                float dist = vector3_length(sender.origin - receiver.origin);
                if (TraceRayAgainstMeshes(receiver.origin, dir, dist - 1.0f)) {
                    continue;  // Blocked
                }
            }
            
            // Gather light from sender, modulated by sender's reflectivity
            Vector3 contribution;
            contribution[0] = sender.totalLight[0] * sender.reflectivity[0] * ff;
            contribution[1] = sender.totalLight[1] * sender.reflectivity[1] * ff;
            contribution[2] = sender.totalLight[2] * sender.reflectivity[2] * ff;
            
            gathered = gathered + contribution * static_cast<float>(sampleStep);
        }
        
        incomingLight[i] = gathered * RADIOSITY_SCALE;
    }
    
    // Apply gathered light to patches
    for (size_t i = 0; i < RadiosityData::patches.size(); i++) {
        RadiosityData::patches[i].totalLight = RadiosityData::patches[i].totalLight + incomingLight[i];
    }
}


// Forward declarations for sky environment (defined later with light probe code)
struct SkyEnvironment {
    Vector3 ambientColor;     // Normalized ambient color (0-1)
    float ambientIntensity;   // Ambient brightness (HDR scale)
    Vector3 sunDir;
    Vector3 sunColor;
    float sunIntensity;
    bool valid;
};
static SkyEnvironment GetSkyEnvironment();
static void LogSkyEnvironment(const SkyEnvironment &sky);


// =============================================================================
// ADAPTIVE SUPERSAMPLING (adapted from Source SDK vrad2/lightmap.cpp)
// ComputeLightmapGradients / BuildSupersampleFaceLights
// =============================================================================

/*
    TexelLighting_t
    Result of lighting computation at a single point.
*/
struct TexelLighting_t {
    Vector3 direct;
    Vector3 indirect;
};

/*
    ComputeLightingAtPoint
    Compute direct and indirect lighting at an arbitrary world position.
    Extracted from the per-texel loop to enable reuse during supersampling.
*/
static TexelLighting_t ComputeLightingAtPoint(const Vector3 &worldPos, const Vector3 &sampleNormal,
                                              const SkyEnvironment &sky) {
    TexelLighting_t result;
    result.direct = Vector3(0, 0, 0);
    result.indirect = Vector3(1, 1, 1);  // Default: full ambient (1.0 = no occlusion)

    Vector3 directLight(0, 0, 0);
    float aoFactor = 1.0f;

    // Sun: trace shadow ray
    if (sky.valid) {
        Vector3 sunDir = sky.sunDir * -1.0f;
        float sunNdotL = vector3_dot(sampleNormal, sunDir);
        if (sunNdotL > 0) {
            if (!TraceRayAgainstMeshes(worldPos, sunDir, 65536.0f)) {
                directLight = directLight + sky.sunColor * (sunNdotL * sky.sunIntensity);
            }
        }
    }

    // Static point/spot lights with shadow rays
    for (const WorldLight_t &light : ApexLegends::Bsp::worldLights) {
        if (light.type == emit_skyambient || light.type == emit_skylight) continue;
        if (light.flags & WORLDLIGHT_FLAG_REALTIME) continue;

        Vector3 lightPos(light.origin[0], light.origin[1], light.origin[2]);

        if (light.type == emit_point) {
            Vector3 toLight = lightPos - worldPos;
            float dist = vector3_length(toLight);
            if (dist < 0.001f) continue;

            Vector3 lightDir = toLight / dist;
            float NdotL = vector3_dot(sampleNormal, lightDir);
            if (NdotL > 0) {
                if (TraceRayAgainstMeshes(worldPos, lightDir, dist - 1.0f)) continue;

                float atten = 1.0f;
                if (light.quadratic_attn > 0 || light.linear_attn > 0) {
                    atten = 1.0f / (light.constant_attn +
                                   light.linear_attn * dist +
                                   light.quadratic_attn * dist * dist);
                } else {
                    atten = 1.0f / (1.0f + dist * dist * 0.0001f);
                }
                directLight = directLight + light.intensity * NdotL * atten * 100.0f;
            }
        } else if (light.type == emit_spotlight) {
            Vector3 toLight = lightPos - worldPos;
            float dist = vector3_length(toLight);
            if (dist < 0.001f) continue;

            Vector3 lightDir = toLight / dist;
            float NdotL = vector3_dot(sampleNormal, lightDir);
            if (NdotL > 0) {
                float spotDot = vector3_dot(-lightDir, light.normal);
                if (spotDot > light.stopdot2) {
                    if (TraceRayAgainstMeshes(worldPos, lightDir, dist - 1.0f)) continue;

                    float spotAtten = 1.0f;
                    if (spotDot < light.stopdot) {
                        spotAtten = (spotDot - light.stopdot2) / (light.stopdot - light.stopdot2);
                    }
                    float distAtten = 1.0f / (1.0f + dist * dist * 0.0001f);
                    directLight = directLight + light.intensity * NdotL * spotAtten * distAtten * 100.0f;
                }
            }
        }
    }

    // AO: hemisphere ray tracing for ambient occlusion
    if (sky.valid) {
        int hemiRays = 0;
        int unoccluded = 0;
        int step = std::max(1, NUM_SPHERE_NORMALS / AO_HEMISPHERE_RAYS);
        for (int i = 0; i < NUM_SPHERE_NORMALS; i += step) {
            float cosTheta = vector3_dot(g_SphereNormals[i], sampleNormal);
            if (cosTheta <= 0) continue;
            hemiRays++;
            if (!TraceRayAgainstMeshes(worldPos, g_SphereNormals[i], AO_TRACE_DISTANCE)) {
                unoccluded++;
            }
        }
        aoFactor = (hemiRays > 0) ? (float)unoccluded / hemiRays : 1.0f;
        aoFactor = std::pow(aoFactor, 1.5f);
    }

    // Direct channel: sun + point/spot lights + ambient fill (AO-modulated)
    // Official data shows shadow direct ~200-250 linear, sunlit ~4M linear
    Vector3 ambientFill = sky.ambientColor * (sky.ambientIntensity * aoFactor);
    result.direct = directLight + ambientFill;

    // Indirect channel: ambient/bounce lighting modulated by AO
    // Official data shows indirect ~130 linear at exp=0
    result.indirect = sky.ambientColor * (sky.ambientIntensity * aoFactor * INDIRECT_AMBIENT_SCALE);

    return result;
}

/*
    ComputeTexelWorldPos
    Compute world-space position for a (potentially sub-texel) coordinate.
*/
static Vector3 ComputeTexelWorldPos(const SurfaceLightmap_t &surf, float texelX, float texelY) {
    float normalizedU = (surf.rect.width > 1) ? texelX / (surf.rect.width - 1) : 0.5f;
    float normalizedV = (surf.rect.height > 1) ? texelY / (surf.rect.height - 1) : 0.5f;
    normalizedU = std::max(0.0f, std::min(1.0f, normalizedU));
    normalizedV = std::max(0.0f, std::min(1.0f, normalizedV));
    float localU = surf.uMin + normalizedU * (surf.uMax - surf.uMin);
    float localV = surf.vMin + normalizedV * (surf.vMax - surf.vMin);
    return surf.worldBounds.mins
        + surf.tangent * localU
        + surf.bitangent * localV
        + surf.plane.normal() * 0.1f;
}

/*
    ComputeTexelIntensity
    Convert HDR color to perceptual intensity for gradient computation.
    Same formula as Source SDK ComputeLuxelIntensity.
*/
static float ComputeTexelIntensity(const Vector3 &color) {
    float intensity = std::max({color.x(), color.y(), color.z()});
    return std::pow(std::max(0.0f, intensity / 256.0f), 1.0f / 2.2f);
}

/*
    ComputeLightmapGradients
    Compute maximum intensity gradient at each texel from its 8 neighbors.
    Adapted directly from Source SDK vrad2/lightmap.cpp ComputeLightmapGradients.
    High gradients indicate shadow edges or lighting discontinuities that need supersampling.
*/
static void ComputeLightmapGradients(const SurfaceLightmap_t &surf,
                                      const std::vector<float> &intensity,
                                      const std::vector<bool> &processed,
                                      std::vector<float> &gradient) {
    int w = surf.rect.width;
    int h = surf.rect.height;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            if (processed[idx]) continue;

            gradient[idx] = 0.0f;
            float val = intensity[idx];

            // Check all 8 neighbors (same as Source SDK)
            if (y > 0) {
                if (x > 0)   gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[(y-1)*w + (x-1)]));
                             gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[(y-1)*w + x]));
                if (x < w-1) gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[(y-1)*w + (x+1)]));
            }
            if (y < h-1) {
                if (x > 0)   gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[(y+1)*w + (x-1)]));
                             gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[(y+1)*w + x]));
                if (x < w-1) gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[(y+1)*w + (x+1)]));
            }
            if (x > 0)   gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[y*w + (x-1)]));
            if (x < w-1) gradient[idx] = std::max(gradient[idx], std::abs(val - intensity[y*w + (x+1)]));
        }
    }
}

/*
    SupersampleTexel
    Resample lighting at a texel using an NxN sub-sample grid.
    Adapted from Source SDK SupersampleLightAtPoint.
    Returns averaged lighting from all valid sub-samples.
*/
static TexelLighting_t SupersampleTexel(const SurfaceLightmap_t &surf, int texelX, int texelY,
                                        const SkyEnvironment &sky) {
    TexelLighting_t total;
    total.direct = Vector3(0, 0, 0);
    total.indirect = Vector3(0, 0, 0);
    int validSamples = 0;

    Vector3 sampleNormal = surf.plane.normal();

    for (int sy = 0; sy < SUPERSAMPLE_GRID; sy++) {
        for (int sx = 0; sx < SUPERSAMPLE_GRID; sx++) {
            // Sub-texel position centered within each grid cell
            float subX = texelX + (sx + 0.5f) / SUPERSAMPLE_GRID;
            float subY = texelY + (sy + 0.5f) / SUPERSAMPLE_GRID;

            Vector3 worldPos = ComputeTexelWorldPos(surf, subX, subY);
            TexelLighting_t sample = ComputeLightingAtPoint(worldPos, sampleNormal, sky);

            total.direct = total.direct + sample.direct;
            total.indirect = total.indirect + sample.indirect;
            validSamples++;
        }
    }

    if (validSamples > 0) {
        float inv = 1.0f / validSamples;
        total.direct = total.direct * inv;
        total.indirect = total.indirect * inv;
    }

    return total;
}


/*
    ComputeLightmapLighting
    Compute lighting for each texel.
    
    The direct lightmap channel bakes sun (emit_skylight) WITH shadow rays,
    creating proper shadow contrast. The indirect channel stores ambient-only
    so shadowed areas appear dark.
    
    Static point/spot lights are baked into both channels with shadow tracing.
    
    Enhanced with:
    - Sun shadow baking (direct channel only)
    - Shadow ray tracing for all static lights
    - Radiosity (bounced indirect lighting)
*/
void ApexLegends::ComputeLightmapLighting() {
    Sys_FPrintf(SYS_VRB, "--- ComputeLightmapLighting ---\n");
    
    // We bake emit_skylight (sun) WITH shadow rays into the direct lightmap channel.
    // The indirect channel gets only ambient/bounce (no sun), so shadowed areas are dark.
    // emit_skyambient brightness is still dynamic from worldLights lump.
    
    if (ApexLegends::Bsp::worldLights.empty()) {
        Sys_Printf("  No worldlights found\n");
    }
    
    // Get sky environment for sun shadow baking
    SkyEnvironment sky = GetSkyEnvironment();
    LogSkyEnvironment(sky);
    
    // Initialize radiosity patches for bounce lighting
    if (RADIOSITY_BOUNCES > 0) {
        InitRadiosityPatches();
    }
    
    int totalTexels = 0;
    int patchIndex = 0;  // Track patch index for radiosity
    
    Sys_Printf("     Computing direct lighting...\n");
    
    int totalSurfaces = static_cast<int>(LightmapBuild::surfaces.size());
    int surfacesDone = 0;
    int lastPct = -1;
    auto lightingStart = std::chrono::high_resolution_clock::now();
    
    // Pre-compute sun direction once
    Vector3 sunDir(0, 0, 0);
    if (sky.valid) {
        sunDir = sky.sunDir * -1.0f;
    }
    
    for (SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        Vector3 sampleNormal = surf.plane.normal();
        int numTexels = surf.rect.width * surf.rect.height;
        
        // ===== Phase 1: Generate all visibility rays for this surface =====
        std::vector<Vector3> rayOrigins;
        std::vector<Vector3> rayDirs;
        std::vector<float>   rayMaxDists;
        
        // Per-texel bookkeeping
        struct TexelRayInfo {
            int firstRayIdx;
            int numSunRays;    // 0 or 1
            int numLightRays;
            int numAoRays;
            float sunNdotL;
            int firstLightContribIdx;
        };
        std::vector<TexelRayInfo> texelInfo(numTexels);
        std::vector<Vector3> lightContribs;  // pre-computed color per light shadow ray
        
        for (int texelIdx = 0; texelIdx < numTexels; texelIdx++) {
            int x = texelIdx % surf.rect.width;
            int y = texelIdx / surf.rect.width;
            Vector3 worldPos = ComputeTexelWorldPos(surf, (float)x, (float)y);
            
            TexelRayInfo &info = texelInfo[texelIdx];
            info.firstRayIdx = static_cast<int>(rayOrigins.size());
            info.numSunRays = 0;
            info.numLightRays = 0;
            info.numAoRays = 0;
            info.sunNdotL = 0;
            info.firstLightContribIdx = static_cast<int>(lightContribs.size());
            
            // Sun shadow ray
            if (sky.valid) {
                float ndotl = vector3_dot(sampleNormal, sunDir);
                info.sunNdotL = ndotl;
                if (ndotl > 0) {
                    rayOrigins.push_back(worldPos);
                    rayDirs.push_back(sunDir);
                    rayMaxDists.push_back(65536.0f);
                    info.numSunRays = 1;
                }
            }
            
            // Point/spot light shadow rays
            for (const WorldLight_t &light : ApexLegends::Bsp::worldLights) {
                if (light.type == emit_skyambient || light.type == emit_skylight) continue;
                if (light.flags & WORLDLIGHT_FLAG_REALTIME) continue;
                
                Vector3 lightPos(light.origin[0], light.origin[1], light.origin[2]);
                
                if (light.type == emit_point) {
                    Vector3 toLight = lightPos - worldPos;
                    float dist = vector3_length(toLight);
                    if (dist < 0.001f) continue;
                    
                    Vector3 lightDir = toLight / dist;
                    float NdotL = vector3_dot(sampleNormal, lightDir);
                    if (NdotL <= 0) continue;
                    
                    float atten;
                    if (light.quadratic_attn > 0 || light.linear_attn > 0) {
                        atten = 1.0f / (light.constant_attn +
                                       light.linear_attn * dist +
                                       light.quadratic_attn * dist * dist);
                    } else {
                        atten = 1.0f / (1.0f + dist * dist * 0.0001f);
                    }
                    
                    rayOrigins.push_back(worldPos);
                    rayDirs.push_back(lightDir);
                    rayMaxDists.push_back(dist - 1.0f);
                    lightContribs.push_back(light.intensity * NdotL * atten * 100.0f);
                    info.numLightRays++;
                    
                } else if (light.type == emit_spotlight) {
                    Vector3 toLight = lightPos - worldPos;
                    float dist = vector3_length(toLight);
                    if (dist < 0.001f) continue;
                    
                    Vector3 lightDir = toLight / dist;
                    float NdotL = vector3_dot(sampleNormal, lightDir);
                    if (NdotL <= 0) continue;
                    
                    float spotDot = vector3_dot(-lightDir, light.normal);
                    if (spotDot <= light.stopdot2) continue;
                    
                    float spotAtten = 1.0f;
                    if (spotDot < light.stopdot) {
                        spotAtten = (spotDot - light.stopdot2) / (light.stopdot - light.stopdot2);
                    }
                    float distAtten = 1.0f / (1.0f + dist * dist * 0.0001f);
                    
                    rayOrigins.push_back(worldPos);
                    rayDirs.push_back(lightDir);
                    rayMaxDists.push_back(dist - 1.0f);
                    lightContribs.push_back(light.intensity * NdotL * spotAtten * distAtten * 100.0f);
                    info.numLightRays++;
                }
            }
            
            // AO hemisphere rays
            if (sky.valid) {
                int step = std::max(1, NUM_SPHERE_NORMALS / AO_HEMISPHERE_RAYS);
                for (int i = 0; i < NUM_SPHERE_NORMALS; i += step) {
                    float cosTheta = vector3_dot(g_SphereNormals[i], sampleNormal);
                    if (cosTheta <= 0) continue;
                    rayOrigins.push_back(worldPos);
                    rayDirs.push_back(g_SphereNormals[i]);
                    rayMaxDists.push_back(AO_TRACE_DISTANCE);
                    info.numAoRays++;
                }
            }
        }
        
        // ===== Phase 2: Batch GPU dispatch =====
        int totalRays = static_cast<int>(rayOrigins.size());
        std::vector<uint8_t> hitResults(totalRays, 0);
        if (totalRays > 0 && HIPRTTrace::IsSceneReady()) {
            HIPRTTrace::BatchTestVisibility(totalRays, rayOrigins.data(), rayDirs.data(),
                                            rayMaxDists.data(), hitResults.data());
        }
        
        // ===== Phase 3: Process results and compute lighting =====
        for (int texelIdx = 0; texelIdx < numTexels; texelIdx++) {
            const TexelRayInfo &info = texelInfo[texelIdx];
            Vector3 directLight(0, 0, 0);
            float aoFactor = 1.0f;
            
            int rayIdx = info.firstRayIdx;
            
            // Sun contribution
            if (info.numSunRays > 0) {
                if (!hitResults[rayIdx]) {
                    directLight = directLight + sky.sunColor * (info.sunNdotL * sky.sunIntensity);
                }
                rayIdx++;
            }
            
            // Light contributions
            for (int li = 0; li < info.numLightRays; li++) {
                if (!hitResults[rayIdx]) {
                    directLight = directLight + lightContribs[info.firstLightContribIdx + li];
                }
                rayIdx++;
            }
            
            // AO
            if (info.numAoRays > 0) {
                int unoccluded = 0;
                for (int a = 0; a < info.numAoRays; a++) {
                    if (!hitResults[rayIdx + a]) unoccluded++;
                }
                aoFactor = (float)unoccluded / info.numAoRays;
                aoFactor = std::pow(aoFactor, 1.5f);
            }
            
            Vector3 ambientFill = sky.ambientColor * (sky.ambientIntensity * aoFactor);
            Vector3 finalDirect = directLight + ambientFill;
            Vector3 finalIndirect = sky.ambientColor * (sky.ambientIntensity * aoFactor * INDIRECT_AMBIENT_SCALE);
            
            int x = texelIdx % surf.rect.width;
            int y = texelIdx / surf.rect.width;
            surf.luxels[y * surf.rect.width + x] = finalDirect;
            surf.indirectLuxels[y * surf.rect.width + x] = finalIndirect;
            
            if (RADIOSITY_BOUNCES > 0 && patchIndex < static_cast<int>(RadiosityData::patches.size())) {
                RadiosityData::patches[patchIndex].directLight = finalDirect;
                RadiosityData::patches[patchIndex].totalLight = finalDirect;
                patchIndex++;
            }
            
            totalTexels++;
        }
        
        surfacesDone++;
        int pct = (totalSurfaces > 0) ? (surfacesDone * 100 / totalSurfaces) : 100;
        if (pct != lastPct && (pct % 10 == 0 || surfacesDone == totalSurfaces)) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - lightingStart).count();
            Sys_Printf("     ...%d%% (%d/%d surfaces, %d rays, %.1fs elapsed)\n", pct, surfacesDone, totalSurfaces, totalRays, elapsed);
            lastPct = pct;
        }
    }
    
    {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - lightingStart).count();
        Sys_Printf("     %9d texels computed (direct) in %.2fs\n", totalTexels, elapsed);
    }
    
    // =====================================================
    // ADAPTIVE SUPERSAMPLING (Source SDK style)
    // Detect high-gradient areas and supersample them to
    // smooth shadow edges and lighting discontinuities.
    // Uses batch GPU ray dispatch for performance.
    // =====================================================
    if (SUPERSAMPLE_PASSES > 0) {
        Sys_Printf("     Computing adaptive supersampling (%d passes, %dx%d grid)...\n",
                   SUPERSAMPLE_PASSES, SUPERSAMPLE_GRID, SUPERSAMPLE_GRID);
        int totalSupersampled = 0;
        auto ssStart = std::chrono::high_resolution_clock::now();
        
        int ssSubSamples = SUPERSAMPLE_GRID * SUPERSAMPLE_GRID;
        int ssSurfacesDone = 0;
        int ssTotalSurfaces = static_cast<int>(LightmapBuild::surfaces.size());
        int ssLastPct = -1;
        
        for (SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
            int w = surf.rect.width;
            int h = surf.rect.height;
            int numTexels = w * h;
            
            // Compute intensity at each texel for gradient detection
            std::vector<float> intensity(numTexels);
            for (int i = 0; i < numTexels; i++) {
                intensity[i] = ComputeTexelIntensity(surf.luxels[i]);
            }
            
            std::vector<bool> processed(numTexels, false);
            std::vector<float> gradient(numTexels, 0.0f);
            Vector3 sampleNormal = surf.plane.normal();
            
            for (int pass = 0; pass < SUPERSAMPLE_PASSES; pass++) {
                ComputeLightmapGradients(surf, intensity, processed, gradient);
                
                // Identify texels needing supersampling this pass
                std::vector<int> ssTexels;
                for (int i = 0; i < numTexels; i++) {
                    if (processed[i]) continue;
                    if (gradient[i] < SUPERSAMPLE_GRADIENT_THRESHOLD) continue;
                    ssTexels.push_back(i);
                }
                if (ssTexels.empty()) break;
                
                // ===== Phase 1: Generate all rays for all sub-samples =====
                struct SubSampleRayInfo {
                    int firstRayIdx;
                    int numSunRays;
                    int numLightRays;
                    int numAoRays;
                    float sunNdotL;
                    int firstLightContribIdx;
                };
                
                int totalSubSamples = static_cast<int>(ssTexels.size()) * ssSubSamples;
                std::vector<SubSampleRayInfo> subInfo(totalSubSamples);
                std::vector<Vector3> rayOrigins;
                std::vector<Vector3> rayDirs;
                std::vector<float>   rayMaxDists;
                std::vector<Vector3> lightContribs;
                
                // Reserve a rough estimate to avoid excessive reallocation
                int estRaysPerSample = 1 + static_cast<int>(ApexLegends::Bsp::worldLights.size()) + AO_HEMISPHERE_RAYS;
                rayOrigins.reserve(totalSubSamples * estRaysPerSample);
                rayDirs.reserve(totalSubSamples * estRaysPerSample);
                rayMaxDists.reserve(totalSubSamples * estRaysPerSample);
                
                int subIdx = 0;
                for (int texelIdx : ssTexels) {
                    int texelX = texelIdx % w;
                    int texelY = texelIdx / w;
                    
                    for (int sy = 0; sy < SUPERSAMPLE_GRID; sy++) {
                        for (int sx = 0; sx < SUPERSAMPLE_GRID; sx++) {
                            float subX = texelX + (sx + 0.5f) / SUPERSAMPLE_GRID;
                            float subY = texelY + (sy + 0.5f) / SUPERSAMPLE_GRID;
                            Vector3 worldPos = ComputeTexelWorldPos(surf, subX, subY);
                            
                            SubSampleRayInfo &info = subInfo[subIdx];
                            info.firstRayIdx = static_cast<int>(rayOrigins.size());
                            info.numSunRays = 0;
                            info.numLightRays = 0;
                            info.numAoRays = 0;
                            info.sunNdotL = 0;
                            info.firstLightContribIdx = static_cast<int>(lightContribs.size());
                            
                            // Sun shadow ray
                            if (sky.valid) {
                                float ndotl = vector3_dot(sampleNormal, sunDir);
                                info.sunNdotL = ndotl;
                                if (ndotl > 0) {
                                    rayOrigins.push_back(worldPos);
                                    rayDirs.push_back(sunDir);
                                    rayMaxDists.push_back(65536.0f);
                                    info.numSunRays = 1;
                                }
                            }
                            
                            // Point/spot light shadow rays
                            for (const WorldLight_t &light : ApexLegends::Bsp::worldLights) {
                                if (light.type == emit_skyambient || light.type == emit_skylight) continue;
                                if (light.flags & WORLDLIGHT_FLAG_REALTIME) continue;
                                
                                Vector3 lightPos(light.origin[0], light.origin[1], light.origin[2]);
                                
                                if (light.type == emit_point) {
                                    Vector3 toLight = lightPos - worldPos;
                                    float dist = vector3_length(toLight);
                                    if (dist < 0.001f) continue;
                                    
                                    Vector3 lightDir = toLight / dist;
                                    float NdotL = vector3_dot(sampleNormal, lightDir);
                                    if (NdotL <= 0) continue;
                                    
                                    float atten;
                                    if (light.quadratic_attn > 0 || light.linear_attn > 0) {
                                        atten = 1.0f / (light.constant_attn +
                                                       light.linear_attn * dist +
                                                       light.quadratic_attn * dist * dist);
                                    } else {
                                        atten = 1.0f / (1.0f + dist * dist * 0.0001f);
                                    }
                                    
                                    rayOrigins.push_back(worldPos);
                                    rayDirs.push_back(lightDir);
                                    rayMaxDists.push_back(dist - 1.0f);
                                    lightContribs.push_back(light.intensity * NdotL * atten * 100.0f);
                                    info.numLightRays++;
                                    
                                } else if (light.type == emit_spotlight) {
                                    Vector3 toLight = lightPos - worldPos;
                                    float dist = vector3_length(toLight);
                                    if (dist < 0.001f) continue;
                                    
                                    Vector3 lightDir = toLight / dist;
                                    float NdotL = vector3_dot(sampleNormal, lightDir);
                                    if (NdotL <= 0) continue;
                                    
                                    float spotDot = vector3_dot(-lightDir, light.normal);
                                    if (spotDot <= light.stopdot2) continue;
                                    
                                    float spotAtten = 1.0f;
                                    if (spotDot < light.stopdot) {
                                        spotAtten = (spotDot - light.stopdot2) / (light.stopdot - light.stopdot2);
                                    }
                                    float distAtten = 1.0f / (1.0f + dist * dist * 0.0001f);
                                    
                                    rayOrigins.push_back(worldPos);
                                    rayDirs.push_back(lightDir);
                                    rayMaxDists.push_back(dist - 1.0f);
                                    lightContribs.push_back(light.intensity * NdotL * spotAtten * distAtten * 100.0f);
                                    info.numLightRays++;
                                }
                            }
                            
                            // AO hemisphere rays
                            if (sky.valid) {
                                int step = std::max(1, NUM_SPHERE_NORMALS / AO_HEMISPHERE_RAYS);
                                for (int i = 0; i < NUM_SPHERE_NORMALS; i += step) {
                                    float cosTheta = vector3_dot(g_SphereNormals[i], sampleNormal);
                                    if (cosTheta <= 0) continue;
                                    rayOrigins.push_back(worldPos);
                                    rayDirs.push_back(g_SphereNormals[i]);
                                    rayMaxDists.push_back(AO_TRACE_DISTANCE);
                                    info.numAoRays++;
                                }
                            }
                            
                            subIdx++;
                        }
                    }
                }
                
                // ===== Phase 2: Batch GPU dispatch =====
                int totalRays = static_cast<int>(rayOrigins.size());
                std::vector<uint8_t> hitResults(totalRays, 0);
                if (totalRays > 0 && HIPRTTrace::IsSceneReady()) {
                    HIPRTTrace::BatchTestVisibility(totalRays, rayOrigins.data(), rayDirs.data(),
                                                    rayMaxDists.data(), hitResults.data());
                }
                
                // ===== Phase 3: Process results — average sub-samples per texel =====
                subIdx = 0;
                for (int texelIdx : ssTexels) {
                    Vector3 totalDirect(0, 0, 0);
                    Vector3 totalIndirect(0, 0, 0);
                    
                    for (int s = 0; s < ssSubSamples; s++) {
                        const SubSampleRayInfo &info = subInfo[subIdx];
                        Vector3 directLight(0, 0, 0);
                        float aoFactor = 1.0f;
                        int rayIdx = info.firstRayIdx;
                        
                        // Sun
                        if (info.numSunRays > 0) {
                            if (!hitResults[rayIdx]) {
                                directLight = directLight + sky.sunColor * (info.sunNdotL * sky.sunIntensity);
                            }
                            rayIdx++;
                        }
                        
                        // Lights
                        for (int li = 0; li < info.numLightRays; li++) {
                            if (!hitResults[rayIdx]) {
                                directLight = directLight + lightContribs[info.firstLightContribIdx + li];
                            }
                            rayIdx++;
                        }
                        
                        // AO
                        if (info.numAoRays > 0) {
                            int unoccluded = 0;
                            for (int a = 0; a < info.numAoRays; a++) {
                                if (!hitResults[rayIdx + a]) unoccluded++;
                            }
                            aoFactor = (float)unoccluded / info.numAoRays;
                            aoFactor = std::pow(aoFactor, 1.5f);
                        }
                        
                        Vector3 ambientFill = sky.ambientColor * (sky.ambientIntensity * aoFactor);
                        totalDirect = totalDirect + directLight + ambientFill;
                        totalIndirect = totalIndirect + sky.ambientColor * (sky.ambientIntensity * aoFactor * INDIRECT_AMBIENT_SCALE);
                        
                        subIdx++;
                    }
                    
                    float inv = 1.0f / ssSubSamples;
                    surf.luxels[texelIdx] = totalDirect * inv;
                    surf.indirectLuxels[texelIdx] = totalIndirect * inv;
                    intensity[texelIdx] = ComputeTexelIntensity(surf.luxels[texelIdx]);
                    processed[texelIdx] = true;
                    totalSupersampled++;
                }
            }
            
            ssSurfacesDone++;
            int pct = (ssTotalSurfaces > 0) ? (ssSurfacesDone * 100 / ssTotalSurfaces) : 100;
            if (pct != ssLastPct && (pct % 10 == 0 || ssSurfacesDone == ssTotalSurfaces)) {
                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(now - ssStart).count();
                Sys_Printf("     ...%d%% (%d/%d surfaces, %d supersampled, %.1fs elapsed)\n",
                           pct, ssSurfacesDone, ssTotalSurfaces, totalSupersampled, elapsed);
                ssLastPct = pct;
            }
        }
        
        auto ssEnd = std::chrono::high_resolution_clock::now();
        double ssElapsed = std::chrono::duration<double>(ssEnd - ssStart).count();
        Sys_Printf("     %9d texels supersampled in %.2fs\n", totalSupersampled, ssElapsed);
    }
    
    // =====================================================
    // RADIOSITY: Compute bounce lighting
    // =====================================================
    if (RADIOSITY_BOUNCES > 0 && !RadiosityData::patches.empty()) {
        Sys_Printf("     Computing %d radiosity bounce(s)...\n", RADIOSITY_BOUNCES);
        
        for (int bounce = 1; bounce <= RADIOSITY_BOUNCES; bounce++) {
            GatherRadiosityLight(bounce);
        }
        
        // Apply bounced light back to luxels
        patchIndex = 0;
        for (SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
            for (int y = 0; y < surf.rect.height; y++) {
                for (int x = 0; x < surf.rect.width; x++) {
                    if (patchIndex < static_cast<int>(RadiosityData::patches.size())) {
                        // Add indirect light (total - direct = indirect)
                        Vector3 indirect = RadiosityData::patches[patchIndex].totalLight 
                                         - RadiosityData::patches[patchIndex].directLight;
                        surf.luxels[y * surf.rect.width + x] = 
                            surf.luxels[y * surf.rect.width + x] + indirect;
                        patchIndex++;
                    }
                }
            }
        }
        
        Sys_Printf("     Radiosity complete\n");
    }
}


/*
    EncodeHDRTexel
    Encode a floating-point linear RGB color to ColorRGBExp32 format.
    
    Format (4 bytes): R, G, B mantissa (0-255, linear) + exponent byte.
    Engine decode: linear = (mantissa / 255) * 2^(byte - 128)
    
    Uses unsigned bias-128 exponent stored in the 4th byte:
      byte 128 = 2^0   = 1.0x  (neutral)
      byte 129 = 2^1   = 2.0x  (bright)
      byte 136 = 2^8   = 256x  (sun direct)
      byte 127 = 2^-1  = 0.5x  (dim)
      byte 0   = 2^-128 ≈ 0    (black)
    
    Mantissa is kept in 128..255 range for precision.
*/
static void EncodeHDRTexel(const Vector3 &color, uint8_t *out) {
    float r = std::max(0.0f, color.x());
    float g = std::max(0.0f, color.y());
    float b = std::max(0.0f, color.z());
    float maxComponent = std::max({r, g, b});
    
    if (maxComponent < 1e-12f) {
        // Near-black: exponent 0 = 2^-128 ≈ 0
        out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
        return;
    }
    
    // Find exponent such that mantissa = color / 2^(exp-128) falls in 0-255 range
    // We want mantissa in 128-255 for precision
    int exp = 128 + (int)std::ceil(std::log2(maxComponent));
    exp = std::max(0, std::min(255, exp));
    
    // Scale: mantissa = color * 255 / 2^(exp-128)
    // Equivalent: mantissa = color * 255 * 2^(128-exp)
    float scale = 255.0f / std::pow(2.0f, (float)(exp - 128));
    
    out[0] = (uint8_t)std::min(255, std::max(0, (int)(r * scale + 0.5f)));
    out[1] = (uint8_t)std::min(255, std::max(0, (int)(g * scale + 0.5f)));
    out[2] = (uint8_t)std::min(255, std::max(0, (int)(b * scale + 0.5f)));
    out[3] = (uint8_t)exp;
}

// Encode both direct and indirect channels into 8-byte output
// Both channels use the same bias-128 RGBE format
static void EncodeHDRTexelPair(const Vector3 &directColor, const Vector3 &indirectColor, uint8_t *out) {
    EncodeHDRTexel(directColor, out);      // bytes 0-3: direct
    EncodeHDRTexel(indirectColor, out + 4); // bytes 4-7: indirect
}


/*
    EmitLightmaps
    Convert computed lighting to BSP format and write lumps
    
    Note: SetupSurfaceLightmaps() must be called before this, which happens
    in EmitMeshes() to ensure UV coordinates are available during vertex emission.
*/
void ApexLegends::EmitLightmaps() 
{
    // Compute lighting if we have surfaces allocated
    if (!LightmapBuild::surfaces.empty()) {
        ComputeLightmapLighting();
    }

    Sys_FPrintf(SYS_VRB, "--- EmitLightmaps ---\n");
    
    // If no lightmaps were generated, create a minimal stub
    if (ApexLegends::Bsp::lightmapPages.empty()) {
        Sys_Printf("  No lit surfaces, creating minimal lightmap stub\n");
        
        LightmapHeader_t header;
        header.type = 1;           // Type 1 = 8 bytes per pixel
        header.compressedType = 0;
        header.tag = 0;
        header.unknown = 0;
        header.width = 256;
        header.height = 256;
        ApexLegends::Bsp::lightmapHeaders.push_back(header);
        
        // Black stub lightmap - engine applies dynamic ambient/sun from worldLights
        // Engine expects PLANAR format: all direct texels first, then all indirect texels
        // Black (all zeros) prevents bright bleed and lets engine handle dynamic lighting
        size_t numTexels = 256 * 256;
        size_t dataSize = numTexels * 8;  // 4 bytes direct + 4 bytes indirect per texel
        ApexLegends::Bsp::lightmapDataSky.resize(dataSize, 0);
        
        return;
    }
    
    // Encode luxels to lightmap pages, tracking which pixels were computed
    std::vector<std::vector<bool>> computedPixels(ApexLegends::Bsp::lightmapPages.size());
    for (size_t p = 0; p < ApexLegends::Bsp::lightmapPages.size(); p++) {
        const auto &pg = ApexLegends::Bsp::lightmapPages[p];
        computedPixels[p].resize((size_t)pg.width * pg.height, false);
    }
    
    for (SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        ApexLegends::LightmapPage_t &page = ApexLegends::Bsp::lightmapPages[surf.rect.pageIndex];
        
        for (int y = 0; y < surf.rect.height; y++) {
            for (int x = 0; x < surf.rect.width; x++) {
                int idx = y * surf.rect.width + x;
                const Vector3 &directColor = surf.luxels[idx];
                const Vector3 &indirectColor = surf.indirectLuxels[idx];
                
                int px = surf.rect.x + x;
                int py = surf.rect.y + y;
                int offset = (py * page.width + px) * 8;
                
                EncodeHDRTexelPair(directColor, indirectColor, &page.pixels[offset]);
                computedPixels[surf.rect.pageIndex][(size_t)py * page.width + px] = true;
            }
        }
    }
    
    // Dilate (pad) lightmap edges to prevent bilinear filtering from
    // sampling neutral unallocated pixels at surface boundaries.
    // 2 passes of 4-neighbor dilation using computed-pixel bitmap.
    for (size_t pi = 0; pi < ApexLegends::Bsp::lightmapPages.size(); pi++) {
        auto &page = ApexLegends::Bsp::lightmapPages[pi];
        auto &computed = computedPixels[pi];
        int w = page.width;
        int h = page.height;
        
        for (int pass = 0; pass < 2; pass++) {
            std::vector<uint8_t> dilated = page.pixels;
            std::vector<bool> newComputed = computed;
            
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    if (computed[(size_t)y * w + x]) continue;  // Already has computed data
                    
                    int off = (y * w + x) * 8;
                    
                    // Check 4 neighbors for a computed pixel to copy from
                    const int dx[] = {-1, 1, 0, 0};
                    const int dy[] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; d++) {
                        int nx = x + dx[d];
                        int ny = y + dy[d];
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                        
                        if (computed[(size_t)ny * w + nx]) {
                            int noff = (ny * w + nx) * 8;
                            memcpy(&dilated[off], &page.pixels[noff], 8);
                            newComputed[(size_t)y * w + x] = true;
                            break;
                        }
                    }
                }
            }
            
            page.pixels = dilated;
            computed = newComputed;
        }
    }
    
    // Create headers and concatenate pixel data in PLANAR format
    // Engine expects: [all direct texels (4 bytes each)] [all indirect texels (4 bytes each)]
    // NOT interleaved per-texel as stored in page.pixels during building
    for (size_t i = 0; i < ApexLegends::Bsp::lightmapPages.size(); i++) {
        const ApexLegends::LightmapPage_t &page = ApexLegends::Bsp::lightmapPages[i];
        
        LightmapHeader_t header;
        header.type = 1;           // Type 1 = 8 bytes per pixel uncompressed HDR
        header.compressedType = 0;
        header.tag = 0;
        header.unknown = 0;
        header.width = page.width;
        header.height = page.height;
        ApexLegends::Bsp::lightmapHeaders.push_back(header);
        
        // Write planar: all direct texels first, then all indirect texels
        size_t numTexels = (size_t)page.width * page.height;
        size_t baseOffset = ApexLegends::Bsp::lightmapDataSky.size();
        ApexLegends::Bsp::lightmapDataSky.resize(baseOffset + numTexels * 8);
        uint8_t *dst = &ApexLegends::Bsp::lightmapDataSky[baseOffset];
        
        // Sub-texture 0 (direct light): bytes 0-3 of each interleaved texel
        for (size_t t = 0; t < numTexels; t++) {
            size_t srcOff = t * 8;
            dst[t * 4 + 0] = page.pixels[srcOff + 0];
            dst[t * 4 + 1] = page.pixels[srcOff + 1];
            dst[t * 4 + 2] = page.pixels[srcOff + 2];
            dst[t * 4 + 3] = page.pixels[srcOff + 3];
        }
        
        // Sub-texture 1 (indirect light): bytes 4-7 of each interleaved texel
        uint8_t *dst2 = dst + numTexels * 4;
        for (size_t t = 0; t < numTexels; t++) {
            size_t srcOff = t * 8;
            dst2[t * 4 + 0] = page.pixels[srcOff + 4];
            dst2[t * 4 + 1] = page.pixels[srcOff + 5];
            dst2[t * 4 + 2] = page.pixels[srcOff + 6];
            dst2[t * 4 + 3] = page.pixels[srcOff + 7];
        }
    }

    Sys_Printf("     %9zu lightmap pages\n", ApexLegends::Bsp::lightmapHeaders.size());
    Sys_Printf("     %9zu bytes data\n", ApexLegends::Bsp::lightmapDataSky.size());
}

/*
    GetLightmapUV
    Look up the lightmap UV for a vertex position in a specific mesh
    Returns UV in [0,1] range normalized to the lightmap page
*/
bool ApexLegends::GetLightmapUV(int meshIndex, const Vector3 &worldPos, Vector2 &outUV) {
    // Find the surface lightmap for this mesh
    for (const SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        if (surf.meshIndex == meshIndex) {
            // Calculate local position relative to surface bounds
            Vector3 localPos = worldPos - surf.worldBounds.mins;
            
            // Project onto tangent/bitangent to get local UV
            float localU = vector3_dot(localPos, surf.tangent);
            float localV = vector3_dot(localPos, surf.bitangent);
            
            // Normalize U/V to [0,1] based on actual tangent-space bounds
            // This properly handles surfaces where vertices can be on either side
            // of the tangent/bitangent directions from the world bounds origin
            float uRange = surf.uMax - surf.uMin;
            float vRange = surf.vMax - surf.vMin;
            
            // Avoid division by zero for degenerate surfaces
            float normalizedU = (uRange > 0.001f) ? (localU - surf.uMin) / uRange : 0.0f;
            float normalizedV = (vRange > 0.001f) ? (localV - surf.vMin) / vRange : 0.0f;
            
            // Convert to texel coordinates within the allocated rect (excluding 0.5 border)
            float texelU = normalizedU * (surf.rect.width - 1);
            float texelV = normalizedV * (surf.rect.height - 1);
            
            // Add offset for rect position in atlas and normalize to [0,1]
            float atlasU = (surf.rect.x + texelU + 0.5f) / MAX_LIGHTMAP_WIDTH;
            float atlasV = (surf.rect.y + texelV + 0.5f) / MAX_LIGHTMAP_HEIGHT;
            
            // Clamp to valid range
            outUV.x() = std::max(0.0f, std::min(1.0f, atlasU));
            outUV.y() = std::max(0.0f, std::min(1.0f, atlasV));
            
            return true;
        }
    }
    
    // No lightmap for this mesh - return center of lightmap page
    // This samples from the bright stub lightmap instead of the corner
    // Using 0.5 ensures we sample from the middle where lighting is uniform
    outUV = Vector2(0.5f, 0.5f);
    return false;
}


/*
    GetLightmapPageIndex
    Get the lightmap page index for a mesh
    Returns 0 (stub page) if mesh has no specific lightmap allocation
    The stub lightmap page always exists with bright neutral lighting
*/
int16_t ApexLegends::GetLightmapPageIndex(int meshIndex) {
    for (const SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        if (surf.meshIndex == meshIndex) {
            return static_cast<int16_t>(surf.rect.pageIndex);
        }
    }
    // Return 0 (default stub page) for meshes without specific lightmap allocation
    // This ensures all LIT_BUMP meshes have a valid bright lightmap
    return 0;
}


/*
    EmitLightProbes
    Generate light probe data for ambient lighting on a 3D grid
    
    Light probes provide ambient lighting for dynamic entities (players, props).
    Probes are placed on a grid throughout the playable space.
    The engine uses a binary tree to look up probes spatially.
    
    Structure hierarchy:
    - LightProbeParentInfo: Associates probes with brush models (0 = worldspawn)
    - LightProbeTree: Binary tree for spatial lookup
    - LightProbeRef: Spatial reference to a probe
    - LightProbe: Actual ambient data (SH coefficients + static light refs)
*/

// Parse "_light" key format: "R G B brightness" (e.g., "255 252 242 510")
// Returns normalized RGB color (0-1) and brightness multiplier
static bool ParseLightKey(const char *value, Vector3 &outColor, float &outBrightness) {
    if (!value || !value[0]) return false;
    
    float r, g, b, brightness;
    if (sscanf(value, "%f %f %f %f", &r, &g, &b, &brightness) == 4) {
        // Normalize RGB from 0-255 to 0-1 range
        outColor[0] = r / 255.0f;
        outColor[1] = g / 255.0f;
        outColor[2] = b / 255.0f;
        outBrightness = brightness;
        return true;
    }
    // Try 3-component format (no brightness)
    if (sscanf(value, "%f %f %f", &r, &g, &b) == 3) {
        outColor[0] = r / 255.0f;
        outColor[1] = g / 255.0f;
        outColor[2] = b / 255.0f;
        outBrightness = 1.0f;
        return true;
    }
    return false;
}

// Extract sky environment from light_environment entity and worldlights (called once)
static SkyEnvironment GetSkyEnvironment() {
    SkyEnvironment sky;
    sky.ambientColor = Vector3(0.65f, 0.55f, 0.45f);  // Default warm outdoor
    sky.ambientIntensity = 10.5f;                       // Default ambient HDR intensity
    sky.sunDir = Vector3(0.5f, 0.5f, -0.707f);        // Default: sun from upper-right
    sky.sunColor = Vector3(1.0f, 0.95f, 0.85f);       // Default warm sunlight
    sky.sunIntensity = 10.5f;                         // Default HDR sun intensity
    sky.valid = false;
    
    bool foundSkyAmbient = false;
    bool foundSkyLight = false;
    
    // First, try to get data directly from light_environment entity
    // This is more reliable as it reads the raw _light and _ambient keys
    for (const entity_t &entity : entities) {
        const char *classname = entity.classname();
        if (striEqual(classname, "light_environment")) {
            // Get sun color from _light key (format: "R G B brightness")
            const char *lightValue = entity.valueForKey("_light");
            Vector3 lightColor;
            float brightness;
            if (ParseLightKey(lightValue, lightColor, brightness)) {
                sky.sunColor = lightColor;
                // Use raw brightness to produce realistic HDR intensity
                // Official data shows direct sun exponents of 10-20 (linear values 1K-1M)
                //sky.sunIntensity = brightness;
                foundSkyLight = true;
                Sys_Printf("     Found light_environment _light: %s (intensity=%.1f)\n", lightValue, brightness);
            }
            
            // Get ambient color from _ambient key
            const char *ambientValue = entity.valueForKey("_ambient");
            Vector3 ambientColor;
            float ambientBrightness;
            if (ParseLightKey(ambientValue, ambientColor, ambientBrightness)) {
                sky.ambientColor = ambientColor;
                //sky.ambientIntensity = ambientBrightness;
                foundSkyAmbient = true;
                Sys_Printf("     Found light_environment _ambient: %s (intensity=%.1f)\n", ambientValue, ambientBrightness);
            }
            
            // Get sun direction from angles or pitch/SunSpreadAngle
            Vector3 angles;
            if (entity.read_keyvalue(angles, "angles")) {
                // Convert angles to direction vector
                float pitch = angles[0] * (M_PI / 180.0f);
                float yaw = angles[1] * (M_PI / 180.0f);
                sky.sunDir[0] = std::cos(yaw) * std::cos(pitch);
                sky.sunDir[1] = std::sin(yaw) * std::cos(pitch);
                sky.sunDir[2] = -std::sin(pitch);
                float len = std::sqrt(sky.sunDir[0]*sky.sunDir[0] + sky.sunDir[1]*sky.sunDir[1] + sky.sunDir[2]*sky.sunDir[2]);
                if (len > 0.001f) {
                    sky.sunDir = sky.sunDir * (1.0f / len);
                }
            }
            
            break;  // Only use first light_environment
        }
    }
    
    // Fallback: try to get data from worldlights if not found in entity
    if (!foundSkyAmbient || !foundSkyLight) {
        for (const WorldLight_t &light : ApexLegends::Bsp::worldLights) {
            if (light.type == 5 && !foundSkyAmbient) {  // emit_skyambient
                float maxIntensity = std::max({light.intensity[0], light.intensity[1], light.intensity[2], 1.0f});
                sky.ambientColor[0] = light.intensity[0] / maxIntensity;
                sky.ambientColor[1] = light.intensity[1] / maxIntensity;
                sky.ambientColor[2] = light.intensity[2] / maxIntensity;
                sky.ambientIntensity = maxIntensity;
                foundSkyAmbient = true;
            }
            if (light.type == 3 && !foundSkyLight) {  // emit_skylight (sun)
                sky.sunDir = Vector3(light.normal[0], light.normal[1], light.normal[2]);
                float len = std::sqrt(sky.sunDir[0]*sky.sunDir[0] + sky.sunDir[1]*sky.sunDir[1] + sky.sunDir[2]*sky.sunDir[2]);
                if (len > 0.001f) {
                    sky.sunDir = sky.sunDir * (1.0f / len);
                }
                float maxIntensity = std::max({light.intensity[0], light.intensity[1], light.intensity[2], 1.0f});
                sky.sunColor[0] = light.intensity[0] / maxIntensity;
                sky.sunColor[1] = light.intensity[1] / maxIntensity;
                sky.sunColor[2] = light.intensity[2] / maxIntensity;
                // Use raw intensity for realistic HDR values
                sky.sunIntensity = maxIntensity;
                foundSkyLight = true;
            }
        }
    }
    
    sky.valid = true;
    
    if (!foundSkyAmbient) {
        Sys_Printf("     Warning: No emit_skyambient found, using default\n");
    }
    if (!foundSkyLight) {
        Sys_Printf("     Warning: No emit_skylight found, using default sun direction\n");
    }
    
    return sky;
}

// Simple ray-triangle intersection using Möller–Trumbore algorithm
// Returns true if ray hits triangle, sets t to distance
static bool RayTriangleIntersect(const Vector3 &rayOrigin, const Vector3 &rayDir,
                                  const Vector3 &v0, const Vector3 &v1, const Vector3 &v2,
                                  float &t) {
    const float EPSILON = 0.0001f;
    
    Vector3 edge1 = v1 - v0;
    Vector3 edge2 = v2 - v0;
    Vector3 h = vector3_cross(rayDir, edge2);
    float a = vector3_dot(edge1, h);
    
    if (a > -EPSILON && a < EPSILON)
        return false;  // Ray parallel to triangle
    
    float f = 1.0f / a;
    Vector3 s = rayOrigin - v0;
    float u = f * vector3_dot(s, h);
    
    if (u < 0.0f || u > 1.0f)
        return false;
    
    Vector3 q = vector3_cross(s, edge1);
    float v = f * vector3_dot(rayDir, q);
    
    if (v < 0.0f || u + v > 1.0f)
        return false;
    
    t = f * vector3_dot(edge2, q);
    
    return t > EPSILON;  // Hit in front of ray origin
}

// Fallback brute-force ray tracing (used when HIPRT is not available)
static bool TraceRayAgainstMeshes_Fallback(const Vector3 &origin, const Vector3 &dir, float maxDist) {
    float closestHit = maxDist;
    
    for (const Shared::Mesh_t &mesh : Shared::meshes) {
        // Skip sky surfaces - we want to detect hitting sky as "not blocked"
        if (mesh.shaderInfo && (mesh.shaderInfo->compileFlags & C_SKY))
            continue;
        
        // Quick bounds check
        // Expand bounds slightly for the ray
        MinMax expandedBounds = mesh.minmax;
        expandedBounds.mins -= Vector3(1, 1, 1);
        expandedBounds.maxs += Vector3(1, 1, 1);
        
        // Simple ray-box intersection check for early out
        // Check if origin is inside or ray intersects box
        bool mayIntersect = true;
        for (int axis = 0; axis < 3; axis++) {
            if (origin[axis] < expandedBounds.mins[axis] && dir[axis] <= 0) mayIntersect = false;
            if (origin[axis] > expandedBounds.maxs[axis] && dir[axis] >= 0) mayIntersect = false;
        }
        if (!mayIntersect) continue;
        
        // Test all triangles in mesh
        for (size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
            const Vector3 &v0 = mesh.vertices[mesh.triangles[i]].xyz;
            const Vector3 &v1 = mesh.vertices[mesh.triangles[i + 1]].xyz;
            const Vector3 &v2 = mesh.vertices[mesh.triangles[i + 2]].xyz;
            
            float t;
            if (RayTriangleIntersect(origin, dir, v0, v1, v2, t)) {
                if (t < closestHit) {
                    return true;  // Found a hit, ray is blocked
                }
            }
        }
    }
    
    return false;  // No hit, ray reaches destination
}

// Trace a ray against all mesh geometry
// Uses HIPRT BVH acceleration when available, falls back to brute force
// Returns true if something is hit within maxDist
static bool TraceRayAgainstMeshes(const Vector3 &origin, const Vector3 &dir, float maxDist) {
    // Use HIPRT if scene is ready (much faster - O(log n) vs O(n))
    if (HIPRTTrace::IsSceneReady()) {
        return HIPRTTrace::TestVisibility(origin, dir, maxDist);
    }
    
    // Fallback to brute force ray tracing
    return TraceRayAgainstMeshes_Fallback(origin, dir, maxDist);
}


/*
    TraceRayGetSurfaceColor
    Trace a ray and return the color of the hit surface.
    Uses texture sampling when available for accurate color bleeding.
    
    Returns true if a surface was hit, with outColor set to the surface color.
    The color is normalized to 0-1 range for use in radiosity calculations.
*/
static bool TraceRayGetSurfaceColor(const Vector3 &origin, const Vector3 &dir, 
                                    float maxDist, Vector3 &outColor, float &outDist) {
    // Default neutral gray if we can't sample
    outColor = Vector3(0.5f, 0.5f, 0.5f);
    outDist = maxDist;
    
    // Try HIPRT extended trace for UV coordinates
    if (HIPRTTrace::IsSceneReady()) {
        float hitDist;
        Vector3 hitNormal;
        int meshIndex;
        Vector2 hitUV;
        int primID;
        
        if (HIPRTTrace::TraceRayExtended(origin, dir, maxDist, hitDist, hitNormal, 
                                          meshIndex, hitUV, primID)) {
            outDist = hitDist;
            
            // Get the mesh and its shader
            if (meshIndex >= 0 && meshIndex < static_cast<int>(Shared::meshes.size())) {
                const Shared::Mesh_t &mesh = Shared::meshes[meshIndex];
                
                if (mesh.shaderInfo) {
                    // Try to sample the actual texture
                    if (mesh.shaderInfo->shaderImage && 
                        mesh.shaderInfo->shaderImage->pixels &&
                        mesh.shaderInfo->shaderImage->width > 0 && 
                        mesh.shaderInfo->shaderImage->height > 0) {
                        
                        Color4f texColor;
                        if (RadSampleImage(mesh.shaderInfo->shaderImage->pixels,
                                          mesh.shaderInfo->shaderImage->width,
                                          mesh.shaderInfo->shaderImage->height,
                                          hitUV, texColor)) {
                            // Convert from 0-255 to 0-1 range
                            outColor[0] = texColor[0] / 255.0f;
                            outColor[1] = texColor[1] / 255.0f;
                            outColor[2] = texColor[2] / 255.0f;
                            return true;
                        }
                    }
                    
                    // Fall back to shader average color if available
                    if (mesh.shaderInfo->averageColor[0] > 0 || 
                        mesh.shaderInfo->averageColor[1] > 0 || 
                        mesh.shaderInfo->averageColor[2] > 0) {
                        outColor[0] = mesh.shaderInfo->averageColor[0] / 255.0f;
                        outColor[1] = mesh.shaderInfo->averageColor[1] / 255.0f;
                        outColor[2] = mesh.shaderInfo->averageColor[2] / 255.0f;
                        return true;
                    }
                    
                    // Fall back to shader color if set
                    if (mesh.shaderInfo->color[0] > 0 || 
                        mesh.shaderInfo->color[1] > 0 || 
                        mesh.shaderInfo->color[2] > 0) {
                        outColor = mesh.shaderInfo->color;
                        return true;
                    }
                }
            }
            return true;  // Hit something, even if we couldn't get color
        }
        return false;  // No hit
    }
    
    // Fallback: just do visibility check, use neutral color
    if (TraceRayAgainstMeshes_Fallback(origin, dir, maxDist)) {
        return true;
    }
    return false;
}


// =============================================================================
// SOURCE SDK STYLE LIGHT PROBE COMPUTATION
// Adapted from leaf_ambient_lighting.cpp
// =============================================================================

/*
    ComputeAmbientFromSphericalSamples
    Sample lighting from 162 directions and accumulate into 6-sided ambient cube.
    This is the core of Source SDK's ambient lighting computation.
    
    The 6-sided cube stores lighting from each major axis direction:
    - cube[0] = +X, cube[1] = -X
    - cube[2] = +Y, cube[3] = -Y  
    - cube[4] = +Z, cube[5] = -Z
    
    Enhanced with texture color sampling for accurate color bleeding.
*/
static void ComputeAmbientFromSphericalSamples(const Vector3 &position, 
                                                const SkyEnvironment &sky,
                                                Vector3 lightBoxColor[6]) {
    // Initialize cube to zero
    for (int i = 0; i < 6; i++) {
        lightBoxColor[i] = Vector3(0, 0, 0);
    }
    
    // Sample lighting from 162 uniformly distributed directions
    Vector3 radcolor[NUM_SPHERE_NORMALS];
    
    for (int i = 0; i < NUM_SPHERE_NORMALS; i++) {
        const Vector3 &dir = g_SphereNormals[i];
        radcolor[i] = Vector3(0, 0, 0);
        
        // Trace ray in this direction with offset to avoid self-intersection
        Vector3 rayOrigin = position + dir * 2.0f;
        
        Vector3 surfaceColor;
        float hitDist;
        if (!TraceRayGetSurfaceColor(rayOrigin, dir, LIGHT_PROBE_TRACE_DIST, surfaceColor, hitDist)) {
            // Ray reached sky - add sky contribution based on direction
            
            // Sky ambient contribution (uniform from all directions)
            radcolor[i] = radcolor[i] + sky.ambientColor * 0.5f;
            
            // Sun contribution (directional, only if facing sun)
            // Sun direction points FROM sun TO world, so we check dot with -sunDir
            float sunDot = vector3_dot(dir, sky.sunDir * -1.0f);
            if (sunDot > 0) {
                // Add sun color weighted by how directly we're looking at the sun
                radcolor[i] = radcolor[i] + sky.sunColor * (sunDot * sky.sunIntensity * 0.5f);
            }
            
            // Additional hemisphere sky gradient (brighter toward zenith)
            float upDot = dir[2];  // Z is up
            if (upDot > 0) {
                radcolor[i] = radcolor[i] + sky.ambientColor * (upDot * 0.3f);
            }
        } else {
            // Ray hit geometry - use surface color for bounce lighting
            // This creates color bleeding (e.g., red walls tint nearby areas red)
            
            // Base ambient contribution modulated by surface color (reflectivity)
            Vector3 bounceColor;
            bounceColor[0] = sky.ambientColor[0] * surfaceColor[0];
            bounceColor[1] = sky.ambientColor[1] * surfaceColor[1];
            bounceColor[2] = sky.ambientColor[2] * surfaceColor[2];
            
            // Distance falloff - closer surfaces contribute more
            float distFactor = 1.0f;
            if (hitDist < 512.0f) {
                // Nearby surfaces get stronger contribution
                distFactor = 1.0f + (512.0f - hitDist) / 512.0f * 0.5f;
            }
            
            // Scale for indirect lighting (typical reflectance ~0.35-0.5)
            radcolor[i] = bounceColor * (0.4f * distFactor);
        }
    }
    
    // Accumulate samples into 6-sided ambient cube
    // Each cube side gets contribution from directions facing that side
    for (int j = 0; j < 6; j++) {
        float totalWeight = 0.0f;
        
        for (int i = 0; i < NUM_SPHERE_NORMALS; i++) {
            float weight = vector3_dot(g_SphereNormals[i], g_BoxDirections[j]);
            if (weight > 0) {
                totalWeight += weight;
                lightBoxColor[j] = lightBoxColor[j] + radcolor[i] * weight;
            }
        }
        
        // Normalize by total weight
        if (totalWeight > 0.0f) {
            lightBoxColor[j] = lightBoxColor[j] * (1.0f / totalWeight);
        }
    }
    
    // Add contribution from point lights (emit_surface lights)
    // Similar to Source SDK's AddEmitSurfaceLights
    for (const WorldLight_t &light : ApexLegends::Bsp::worldLights) {
        // Skip non-point lights
        if (light.type == emit_skyambient || light.type == emit_skylight) {
            continue;
        }
        
        // Check visibility to this light
        Vector3 lightPos(light.origin[0], light.origin[1], light.origin[2]);
        Vector3 delta = lightPos - position;
        float distSq = vector3_dot(delta, delta);
        
        if (distSq < 1.0f) continue;  // Too close
        
        float dist = std::sqrt(distSq);
        Vector3 dirToLight = delta * (1.0f / dist);
        
        // Check if light is blocked (with offset to avoid self-intersection)
        Vector3 shadowOrigin = position + dirToLight * 2.0f;
        if (TraceRayAgainstMeshes(shadowOrigin, dirToLight, dist - 4.0f)) {
            continue;  // Light is occluded
        }
        
        // Distance falloff (inverse square)
        float falloff = 1.0f / (distSq + 1.0f);
        
        // Light intensity - already in linear RGB (not 0-255, so don't divide)
        // Scale down since probe values are typically 0-1 range and lights can be bright
        Vector3 lightColor = light.intensity * 0.01f;
        
        // Add to appropriate cube sides based on direction
        for (int i = 0; i < 6; i++) {
            float weight = vector3_dot(dirToLight, g_BoxDirections[i]);
            if (weight > 0) {
                lightBoxColor[i] = lightBoxColor[i] + lightColor * (weight * falloff);
            }
        }
    }
}

// Helper to clamp float to int16 range before casting
static int16_t ClampToInt16(float value) {
    return static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
}

/*
    ConvertCubeToSphericalHarmonics
    Convert 6-sided ambient cube to L1 spherical harmonics format used by Apex.
    
    Apex uses 4 coefficients per color channel:
    - [0] = X gradient (difference between +X and -X)
    - [1] = Y gradient (difference between +Y and -Y)
    - [2] = Z gradient (difference between +Z and -Z)
    - [3] = DC (average/constant term)
    
    Based on official map analysis:
    - DC values typically range from 0 to 3500 for normally lit areas
    - Gradient values range from -3500 to +3500
    - Input cube colors should be in 0-1 normalized range
*/
static void ConvertCubeToSphericalHarmonics(const Vector3 lightBoxColor[6], LightProbe_t &probe) {

    const float shScale = 8192.0f;
    
    for (int channel = 0; channel < 3; channel++) {
        // Extract color components for this channel from each cube side
        float posX = lightBoxColor[0][channel];  // +X
        float negX = lightBoxColor[1][channel];  // -X
        float posY = lightBoxColor[2][channel];  // +Y
        float negY = lightBoxColor[3][channel];  // -Y
        float posZ = lightBoxColor[4][channel];  // +Z
        float negZ = lightBoxColor[5][channel];  // -Z
        
        // DC term = average of all sides
        float dc = (posX + negX + posY + negY + posZ + negZ) / 6.0f;
        
        // Directional gradients (difference between opposite sides)
        // These encode the directional variation in lighting
        float gradX = (posX - negX) * 0.5f;
        float gradY = (posY - negY) * 0.5f;
        float gradZ = (posZ - negZ) * 0.5f;
        
        // Store in probe (scale to int16 range)
        probe.ambientSH[channel][0] = ClampToInt16(gradX * shScale);  // X gradient
        probe.ambientSH[channel][1] = ClampToInt16(gradY * shScale);  // Y gradient
        probe.ambientSH[channel][2] = ClampToInt16(gradZ * shScale);  // Z gradient
        probe.ambientSH[channel][3] = ClampToInt16(dc * shScale);     // DC (ambient)
    }
}

/*
    IsPositionInsideSolid
    Check if a position is inside solid geometry using 6-directional ray tests.
*/
static bool IsPositionInsideSolid(const Vector3 &pos, float testDist = 32.0f) {
    // Trace in all 6 cardinal directions - if all hit nearby, we're inside solid
    // Use small offset to avoid false positives from nearby surfaces
    float offset = 2.0f;
    bool hitPosX = TraceRayAgainstMeshes(pos + Vector3(offset, 0, 0), Vector3(1, 0, 0), testDist);
    bool hitNegX = TraceRayAgainstMeshes(pos + Vector3(-offset, 0, 0), Vector3(-1, 0, 0), testDist);
    bool hitPosY = TraceRayAgainstMeshes(pos + Vector3(0, offset, 0), Vector3(0, 1, 0), testDist);
    bool hitNegY = TraceRayAgainstMeshes(pos + Vector3(0, -offset, 0), Vector3(0, -1, 0), testDist);
    bool hitPosZ = TraceRayAgainstMeshes(pos + Vector3(0, 0, offset), Vector3(0, 0, 1), testDist);
    bool hitNegZ = TraceRayAgainstMeshes(pos + Vector3(0, 0, -offset), Vector3(0, 0, -1), testDist);
    
    return hitPosX && hitNegX && hitPosY && hitNegY && hitPosZ && hitNegZ;
}

/*
    GetDistanceToNearestSurface
    Returns approximate distance to nearest geometry in any direction.
    Also returns push direction to move away from surfaces.
*/
static float GetDistanceToNearestSurface(const Vector3 &pos, Vector3 &outPushDir) {
    float minDist = FLT_MAX;
    outPushDir = Vector3(0, 0, 0);
    
    // Test cardinal directions
    const Vector3 testDirs[6] = {
        Vector3(1, 0, 0), Vector3(-1, 0, 0),
        Vector3(0, 1, 0), Vector3(0, -1, 0),
        Vector3(0, 0, 1), Vector3(0, 0, -1)
    };
    
    for (int i = 0; i < 6; i++) {
        const Vector3 &dir = testDirs[i];
        float offset = 2.0f;
        
        // Use HIPRT for precise distance if available
        if (HIPRTTrace::IsSceneReady()) {
            float hitDist;
            Vector3 hitNormal;
            int meshIndex;
            if (HIPRTTrace::TraceRay(pos + dir * offset, dir, 512.0f, hitDist, hitNormal, meshIndex)) {
                if (hitDist < minDist) {
                    minDist = hitDist;
                    outPushDir = dir * -1.0f;  // Push away from nearest surface
                }
            }
        } else {
            // Fallback: binary search to find approximate distance
            for (float testDist = 16.0f; testDist <= 512.0f; testDist *= 2.0f) {
                if (TraceRayAgainstMeshes(pos + dir * offset, dir, testDist)) {
                    if (testDist < minDist) {
                        minDist = testDist;
                        outPushDir = dir * -1.0f;
                    }
                    break;
                }
            }
        }
    }
    
    return minDist;
}

/*
    PushProbeAwayFromSurfaces
    Move probe position to maintain minimum distance from geometry.
*/
static Vector3 PushProbeAwayFromSurfaces(const Vector3 &pos, float minDistance) {
    Vector3 result = pos;
    
    // Iteratively push away from surfaces
    for (int iter = 0; iter < 4; iter++) {
        Vector3 pushDir;
        float nearestDist = GetDistanceToNearestSurface(result, pushDir);
        
        if (nearestDist >= minDistance) {
            break;  // Far enough from surfaces
        }
        
        // Push position away from nearest surface
        float pushAmount = minDistance - nearestDist + 8.0f;  // Extra padding
        result = result + pushDir * pushAmount;
    }
    
    return result;
}

/*
    GenerateProbePositionsVoronoi
    Generate light probe positions using Voronoi-based adaptive placement.
    
    This approach:
    1. Samples geometry surfaces (mesh vertices, face centers) as seed points
    2. Clusters seeds using K-means to find natural groupings
    3. Uses Lloyd relaxation to optimize probe positions
    4. Results in more probes where geometry is dense, fewer in open areas
    
    Much better quality than uniform grid for complex maps.
*/
static void GenerateProbePositionsVoronoi(const MinMax &worldBounds, 
                                          std::vector<Vector3> &probePositions) {
    Sys_Printf("     Generating Voronoi-based probe positions...\n");
    
    // =========================================================================
    // Step 1: Collect geometry sample points
    // =========================================================================
    std::vector<Vector3> geometrySamples;
    geometrySamples.reserve(65536);
    
    // Sample mesh vertices and face centers
    for (const Shared::Mesh_t &mesh : Shared::meshes) {
        // Skip meshes with specific material properties (like skybox)
        // Add all vertices as samples
        for (size_t v = 0; v < mesh.vertices.size(); v++) {
            // Subsample - take every Nth vertex to avoid explosion
            if (v % 4 == 0) {
                geometrySamples.push_back(mesh.vertices[v].xyz);
            }
        }
        
        // Add face centers
        for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3) {
            const Vector3 &v0 = mesh.vertices[mesh.triangles[t + 0]].xyz;
            const Vector3 &v1 = mesh.vertices[mesh.triangles[t + 1]].xyz;
            const Vector3 &v2 = mesh.vertices[mesh.triangles[t + 2]].xyz;
            Vector3 center = (v0 + v1 + v2) * (1.0f / 3.0f);
            geometrySamples.push_back(center);
        }
    }
    
    Sys_FPrintf(SYS_VRB, "     Collected %zu geometry samples\n", geometrySamples.size());
    
    if (geometrySamples.empty()) {
        // Fallback: use world center
        Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
        probePositions.push_back(center);
        Sys_Printf("     No geometry samples, using world center\n");
        return;
    }
    
    // =========================================================================
    // Step 2: Offset samples above surfaces for probe positions
    // Uses batch GPU dispatch for solid rejection tests
    // =========================================================================
    std::vector<Vector3> candidatePositions;
    candidatePositions.reserve(geometrySamples.size());
    
    constexpr float MIN_SURFACE_DISTANCE = 72.0f;  // Minimum distance from any surface
    
    auto step2Start = std::chrono::high_resolution_clock::now();
    
    // Phase 1: Batch solid rejection - test all elevated positions at once
    Sys_FPrintf(SYS_VRB, "     Step 2: Testing %zu positions for solid rejection...\n", geometrySamples.size());
    {
        size_t N = geometrySamples.size();
        std::vector<Vector3> elevatedPositions(N);
        for (size_t i = 0; i < N; i++) {
            elevatedPositions[i] = geometrySamples[i] + Vector3(0, 0, 96.0f);
        }
        
        // Generate 6 cardinal direction rays per position for solid test
        const Vector3 cardinalDirs[6] = {
            Vector3(1,0,0), Vector3(-1,0,0),
            Vector3(0,1,0), Vector3(0,-1,0),
            Vector3(0,0,1), Vector3(0,0,-1)
        };
        const float solidOffset = 2.0f;
        const float solidTestDist = 48.0f;
        
        size_t totalRays = N * 6;
        std::vector<Vector3> rayOrigins(totalRays);
        std::vector<Vector3> rayDirs(totalRays);
        std::vector<float>   rayMaxDists(totalRays);
        
        for (size_t i = 0; i < N; i++) {
            for (int d = 0; d < 6; d++) {
                size_t idx = i * 6 + d;
                rayOrigins[idx] = elevatedPositions[i] + cardinalDirs[d] * solidOffset;
                rayDirs[idx] = cardinalDirs[d];
                rayMaxDists[idx] = solidTestDist;
            }
        }
        
        // Batch GPU dispatch
        std::vector<uint8_t> hitResults(totalRays, 0);
        if (totalRays > 0 && HIPRTTrace::IsSceneReady()) {
            HIPRTTrace::BatchTestVisibility(static_cast<int>(totalRays), rayOrigins.data(),
                                            rayDirs.data(), rayMaxDists.data(), hitResults.data());
        }
        
        // Filter: keep positions where NOT all 6 directions hit (not inside solid)
        std::vector<Vector3> passedFirst;
        passedFirst.reserve(N);
        for (size_t i = 0; i < N; i++) {
            size_t base = i * 6;
            bool allHit = hitResults[base] && hitResults[base+1] && hitResults[base+2] &&
                          hitResults[base+3] && hitResults[base+4] && hitResults[base+5];
            if (!allHit) {
                passedFirst.push_back(elevatedPositions[i]);
            }
        }
        
        Sys_FPrintf(SYS_VRB, "     %zu / %zu passed initial solid rejection (%.1fs)\n", 
                   passedFirst.size(), N,
                   std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - step2Start).count());
        
        // Phase 2: Push probes away from surfaces (iterative, uses closest-hit)
        // Batch across all surviving positions per iteration
        Sys_FPrintf(SYS_VRB, "     Pushing %zu probes away from surfaces...\n", passedFirst.size());
        
        std::vector<Vector3> pushedPositions = passedFirst;
        std::vector<bool> converged(pushedPositions.size(), false);
        
        for (int iter = 0; iter < 4; iter++) {
            // Collect all active positions that need distance checks
            std::vector<size_t> activeIndices;
            for (size_t i = 0; i < pushedPositions.size(); i++) {
                if (!converged[i]) activeIndices.push_back(i);
            }
            if (activeIndices.empty()) break;
            
            // Generate 6 closest-hit rays per active position
            size_t numActive = activeIndices.size();
            size_t numRays = numActive * 6;
            std::vector<Vector3> distRayOrigins(numRays);
            std::vector<Vector3> distRayDirs(numRays);
            std::vector<float>   distRayMaxDists(numRays, 512.0f);
            
            for (size_t a = 0; a < numActive; a++) {
                const Vector3 &pos = pushedPositions[activeIndices[a]];
                for (int d = 0; d < 6; d++) {
                    size_t idx = a * 6 + d;
                    distRayOrigins[idx] = pos + cardinalDirs[d] * solidOffset;
                    distRayDirs[idx] = cardinalDirs[d];
                }
            }
            
            // Batch closest-hit dispatch
            std::vector<float> hitDists(numRays, -1.0f);
            if (numRays > 0 && HIPRTTrace::IsSceneReady()) {
                HIPRTTrace::BatchTraceRay(static_cast<int>(numRays), distRayOrigins.data(),
                                          distRayDirs.data(), distRayMaxDists.data(), hitDists.data());
            }
            
            // Process results: find nearest surface per position and push away
            for (size_t a = 0; a < numActive; a++) {
                size_t posIdx = activeIndices[a];
                float minDist = FLT_MAX;
                int minDir = -1;
                
                for (int d = 0; d < 6; d++) {
                    float dist = hitDists[a * 6 + d];
                    if (dist >= 0 && dist < minDist) {
                        minDist = dist;
                        minDir = d;
                    }
                }
                
                if (minDist >= MIN_SURFACE_DISTANCE) {
                    converged[posIdx] = true;
                } else if (minDir >= 0) {
                    float pushAmount = MIN_SURFACE_DISTANCE - minDist + 8.0f;
                    pushedPositions[posIdx] = pushedPositions[posIdx] + cardinalDirs[minDir] * (-pushAmount);
                }
            }
        }
        
        // Phase 3: Batch second solid rejection for pushed positions
        {
            size_t M = pushedPositions.size();
            size_t totalRays2 = M * 6;
            std::vector<Vector3> rayOrigins2(totalRays2);
            std::vector<Vector3> rayDirs2(totalRays2);
            std::vector<float>   rayMaxDists2(totalRays2);
            
            for (size_t i = 0; i < M; i++) {
                for (int d = 0; d < 6; d++) {
                    size_t idx = i * 6 + d;
                    rayOrigins2[idx] = pushedPositions[i] + cardinalDirs[d] * solidOffset;
                    rayDirs2[idx] = cardinalDirs[d];
                    rayMaxDists2[idx] = 32.0f;
                }
            }
            
            std::vector<uint8_t> hitResults2(totalRays2, 0);
            if (totalRays2 > 0 && HIPRTTrace::IsSceneReady()) {
                HIPRTTrace::BatchTestVisibility(static_cast<int>(totalRays2), rayOrigins2.data(),
                                                rayDirs2.data(), rayMaxDists2.data(), hitResults2.data());
            }
            
            for (size_t i = 0; i < M; i++) {
                size_t base = i * 6;
                bool allHit = hitResults2[base] && hitResults2[base+1] && hitResults2[base+2] &&
                              hitResults2[base+3] && hitResults2[base+4] && hitResults2[base+5];
                if (!allHit) {
                    candidatePositions.push_back(pushedPositions[i]);
                }
            }
        }
    }
    
    {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - step2Start).count();
        Sys_FPrintf(SYS_VRB, "     %zu valid candidate positions after solid rejection (%.2fs)\n", candidatePositions.size(), elapsed);
    }
    
    if (candidatePositions.empty()) {
        Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
        probePositions.push_back(center);
        return;
    }
    
    // =========================================================================
    // Step 3: K-means clustering to find probe centroids
    // =========================================================================
    // Target probe count based on world size and geometry density
    Vector3 size = worldBounds.maxs - worldBounds.mins;
    float worldVolume = size[0] * size[1] * size[2];
    float avgDimension = std::cbrt(worldVolume);
    
    // Aim for roughly GRID_SPACING spacing, but let geometry density influence
    int targetProbes = std::max(8, 
        (int)(worldVolume / (LIGHT_PROBE_GRID_SPACING * LIGHT_PROBE_GRID_SPACING * LIGHT_PROBE_GRID_SPACING)));
    
    // Increase target if we have dense geometry
    float densityFactor = std::min(4.0f, (float)candidatePositions.size() / 1000.0f);
    targetProbes = (int)(targetProbes * (1.0f + densityFactor));
    
    // Apply max count limit if not unlimited (-1)
    if (LIGHT_PROBE_MAX_COUNT >= 0) {
        targetProbes = std::min(LIGHT_PROBE_MAX_COUNT, targetProbes);
    }
    
    Sys_FPrintf(SYS_VRB, "     Target probe count: %d (world avg dimension: %.0f)\n", targetProbes, avgDimension);
    
    // Initialize K-means centroids using K-means++ seeding
    std::vector<Vector3> centroids;
    centroids.reserve(targetProbes);
    
    // First centroid: random sample
    centroids.push_back(candidatePositions[0]);
    
    // K-means++ seeding: each new centroid is chosen with probability proportional
    // to squared distance from nearest existing centroid
    std::vector<float> minDistSq(candidatePositions.size(), FLT_MAX);
    
    while (centroids.size() < (size_t)targetProbes && centroids.size() < candidatePositions.size()) {
        // Update minimum distances
        const Vector3 &lastCentroid = centroids.back();
        float totalWeight = 0;
        
        for (size_t i = 0; i < candidatePositions.size(); i++) {
            Vector3 delta = candidatePositions[i] - lastCentroid;
            float distSq = vector3_dot(delta, delta);
            minDistSq[i] = std::min(minDistSq[i], distSq);
            totalWeight += minDistSq[i];
        }
        
        if (totalWeight < 0.001f) break;
        
        // Pick next centroid with probability proportional to D^2
        float threshold = (float)(centroids.size() * 7919 % 10000) / 10000.0f * totalWeight;
        float cumulative = 0;
        size_t chosen = 0;
        
        for (size_t i = 0; i < candidatePositions.size(); i++) {
            cumulative += minDistSq[i];
            if (cumulative >= threshold) {
                chosen = i;
                break;
            }
        }
        
        centroids.push_back(candidatePositions[chosen]);
        
        // Progress indicator
        if (centroids.size() % 100 == 0) {
            Sys_FPrintf(SYS_VRB, "       Seeded %zu / %d centroids...\n", centroids.size(), targetProbes);
        }
    }
    
    Sys_FPrintf(SYS_VRB, "     Seeded %zu initial centroids, running Lloyd relaxation...\n", centroids.size());
    
    // =========================================================================
    // Step 4: Lloyd relaxation (K-means iterations)
    // =========================================================================
    constexpr int MAX_LLOYD_ITERATIONS = 10;
    
    std::vector<int> assignments(candidatePositions.size(), -1);
    std::vector<Vector3> newCentroids(centroids.size());
    std::vector<int> clusterCounts(centroids.size());
    
    for (int iter = 0; iter < MAX_LLOYD_ITERATIONS; iter++) {
        // Assign each candidate to nearest centroid
        for (size_t i = 0; i < candidatePositions.size(); i++) {
            float minDist = FLT_MAX;
            int nearest = 0;
            
            for (size_t c = 0; c < centroids.size(); c++) {
                Vector3 delta = candidatePositions[i] - centroids[c];
                float dist = vector3_dot(delta, delta);
                if (dist < minDist) {
                    minDist = dist;
                    nearest = static_cast<int>(c);
                }
            }
            
            assignments[i] = nearest;
        }
        
        // Compute new centroids as cluster means
        std::fill(newCentroids.begin(), newCentroids.end(), Vector3(0, 0, 0));
        std::fill(clusterCounts.begin(), clusterCounts.end(), 0);
        
        for (size_t i = 0; i < candidatePositions.size(); i++) {
            int c = assignments[i];
            newCentroids[c] = newCentroids[c] + candidatePositions[i];
            clusterCounts[c]++;
        }
        
        // Update centroids
        float maxMove = 0;
        for (size_t c = 0; c < centroids.size(); c++) {
            if (clusterCounts[c] > 0) {
                Vector3 updated = newCentroids[c] * (1.0f / clusterCounts[c]);
                Vector3 delta = updated - centroids[c];
                float moveDist = static_cast<float>(vector3_dot(delta, delta));
                maxMove = std::max(maxMove, moveDist);
                centroids[c] = updated;
            }
        }
        
        // Convergence check
        if (maxMove < 1.0f) {
            Sys_FPrintf(SYS_VRB, "     Lloyd converged after %d iterations\n", iter + 1);
            break;
        }
    }
    
    // =========================================================================
    // Step 5: Filter final positions and enforce minimum spacing
    // Uses batch GPU dispatch for solid rejection
    // =========================================================================
    std::vector<Vector3> finalPositions;
    finalPositions.reserve(centroids.size());
    
    constexpr float FINAL_MIN_SURFACE_DISTANCE = 64.0f;
    
    Sys_FPrintf(SYS_VRB, "     Step 5: Filtering %zu centroids...\n", centroids.size());
    auto step5Start = std::chrono::high_resolution_clock::now();
    
    {
        const Vector3 cardinalDirs[6] = {
            Vector3(1,0,0), Vector3(-1,0,0),
            Vector3(0,1,0), Vector3(0,-1,0),
            Vector3(0,0,1), Vector3(0,0,-1)
        };
        const float solidOffset = 2.0f;
        
        // Phase 1: Batch solid rejection of centroids
        size_t M = centroids.size();
        size_t totalRays = M * 6;
        std::vector<Vector3> rayOrigins(totalRays);
        std::vector<Vector3> rayDirs(totalRays);
        std::vector<float>   rayMaxDists(totalRays);
        
        for (size_t i = 0; i < M; i++) {
            for (int d = 0; d < 6; d++) {
                size_t idx = i * 6 + d;
                rayOrigins[idx] = centroids[i] + cardinalDirs[d] * solidOffset;
                rayDirs[idx] = cardinalDirs[d];
                rayMaxDists[idx] = 32.0f;
            }
        }
        
        std::vector<uint8_t> solidHits(totalRays, 0);
        if (totalRays > 0 && HIPRTTrace::IsSceneReady()) {
            HIPRTTrace::BatchTestVisibility(static_cast<int>(totalRays), rayOrigins.data(),
                                            rayDirs.data(), rayMaxDists.data(), solidHits.data());
        }
        
        // Collect non-solid centroids
        std::vector<Vector3> validCentroids;
        for (size_t i = 0; i < M; i++) {
            size_t base = i * 6;
            bool allHit = solidHits[base] && solidHits[base+1] && solidHits[base+2] &&
                          solidHits[base+3] && solidHits[base+4] && solidHits[base+5];
            if (!allHit) {
                validCentroids.push_back(centroids[i]);
            }
        }
        
        Sys_FPrintf(SYS_VRB, "     %zu / %zu centroids passed solid rejection\n", validCentroids.size(), M);
        
        // Phase 2: Batch push away from surfaces
        std::vector<Vector3> pushedPositions = validCentroids;
        std::vector<bool> converged(pushedPositions.size(), false);
        
        for (int iter = 0; iter < 4; iter++) {
            std::vector<size_t> activeIndices;
            for (size_t i = 0; i < pushedPositions.size(); i++) {
                if (!converged[i]) activeIndices.push_back(i);
            }
            if (activeIndices.empty()) break;
            
            size_t numActive = activeIndices.size();
            size_t numRays = numActive * 6;
            std::vector<Vector3> distRayOrigins(numRays);
            std::vector<Vector3> distRayDirs(numRays);
            std::vector<float>   distRayMaxDists(numRays, 512.0f);
            
            for (size_t a = 0; a < numActive; a++) {
                const Vector3 &pos = pushedPositions[activeIndices[a]];
                for (int d = 0; d < 6; d++) {
                    size_t idx = a * 6 + d;
                    distRayOrigins[idx] = pos + cardinalDirs[d] * solidOffset;
                    distRayDirs[idx] = cardinalDirs[d];
                }
            }
            
            std::vector<float> hitDists(numRays, -1.0f);
            if (numRays > 0 && HIPRTTrace::IsSceneReady()) {
                HIPRTTrace::BatchTraceRay(static_cast<int>(numRays), distRayOrigins.data(),
                                          distRayDirs.data(), distRayMaxDists.data(), hitDists.data());
            }
            
            for (size_t a = 0; a < numActive; a++) {
                size_t posIdx = activeIndices[a];
                float minDist = FLT_MAX;
                int minDir = -1;
                
                for (int d = 0; d < 6; d++) {
                    float dist = hitDists[a * 6 + d];
                    if (dist >= 0 && dist < minDist) {
                        minDist = dist;
                        minDir = d;
                    }
                }
                
                if (minDist >= FINAL_MIN_SURFACE_DISTANCE) {
                    converged[posIdx] = true;
                } else if (minDir >= 0) {
                    float pushAmount = FINAL_MIN_SURFACE_DISTANCE - minDist + 8.0f;
                    pushedPositions[posIdx] = pushedPositions[posIdx] + cardinalDirs[minDir] * (-pushAmount);
                }
            }
        }
        
        // Phase 3: Batch second solid rejection + minimum spacing filter
        {
            size_t P = pushedPositions.size();
            size_t totalRays2 = P * 6;
            std::vector<Vector3> rayOrigins2(totalRays2);
            std::vector<Vector3> rayDirs2(totalRays2);
            std::vector<float>   rayMaxDists2(totalRays2);
            
            for (size_t i = 0; i < P; i++) {
                for (int d = 0; d < 6; d++) {
                    size_t idx = i * 6 + d;
                    rayOrigins2[idx] = pushedPositions[i] + cardinalDirs[d] * solidOffset;
                    rayDirs2[idx] = cardinalDirs[d];
                    rayMaxDists2[idx] = 24.0f;
                }
            }
            
            std::vector<uint8_t> solidHits2(totalRays2, 0);
            if (totalRays2 > 0 && HIPRTTrace::IsSceneReady()) {
                HIPRTTrace::BatchTestVisibility(static_cast<int>(totalRays2), rayOrigins2.data(),
                                                rayDirs2.data(), rayMaxDists2.data(), solidHits2.data());
            }
            
            for (size_t i = 0; i < P; i++) {
                size_t base = i * 6;
                bool allHit = solidHits2[base] && solidHits2[base+1] && solidHits2[base+2] &&
                              solidHits2[base+3] && solidHits2[base+4] && solidHits2[base+5];
                if (allHit) continue;
                
                // Skip if too close to an existing probe
                bool tooClose = false;
                for (const Vector3 &existing : finalPositions) {
                    Vector3 delta = pushedPositions[i] - existing;
                    if (vector3_dot(delta, delta) < LIGHT_PROBE_MIN_SPACING * LIGHT_PROBE_MIN_SPACING) {
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose) {
                    finalPositions.push_back(pushedPositions[i]);
                }
            }
        }
    }
    
    {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - step5Start).count();
        Sys_FPrintf(SYS_VRB, "     %zu final positions after filtering (%.2fs)\n", finalPositions.size(), elapsed);
    }
    
    // =========================================================================
    // Step 6: Add probes at shadow/light transition boundaries
    // Uses batch GPU dispatch for shadow boundary detection
    // =========================================================================
    Sys_FPrintf(SYS_VRB, "     Step 6: Detecting shadow boundaries (%zu probes)...\n", finalPositions.size());
    auto step6Start = std::chrono::high_resolution_clock::now();
    
    std::vector<Vector3> shadowBoundaryProbes;
    
    {
        // Generate 8 upward-angled rays per probe for shadow boundary detection
        size_t N = finalPositions.size();
        size_t totalRays = N * 8;
        std::vector<Vector3> rayOrigins(totalRays);
        std::vector<Vector3> rayDirs(totalRays);
        std::vector<float>   rayMaxDists(totalRays);
        
        std::vector<Vector3> testDirs(8);
        for (int d = 0; d < 8; d++) {
            float angle = d * M_PI / 4.0f;
            testDirs[d] = Vector3(std::cos(angle) * 0.5f, std::sin(angle) * 0.5f, 0.707f);
        }
        
        for (size_t i = 0; i < N; i++) {
            for (int d = 0; d < 8; d++) {
                size_t idx = i * 8 + d;
                rayOrigins[idx] = finalPositions[i] + testDirs[d] * 2.0f;
                rayDirs[idx] = testDirs[d];
                rayMaxDists[idx] = 8192.0f;
            }
        }
        
        // Batch dispatch
        std::vector<uint8_t> hitResults(totalRays, 0);
        if (totalRays > 0 && HIPRTTrace::IsSceneReady()) {
            HIPRTTrace::BatchTestVisibility(static_cast<int>(totalRays), rayOrigins.data(),
                                            rayDirs.data(), rayMaxDists.data(), hitResults.data());
        }
        
        // Process results: find probes at shadow boundaries and add extra probes
        const Vector3 cardinalDirs[6] = {
            Vector3(1,0,0), Vector3(-1,0,0),
            Vector3(0,1,0), Vector3(0,-1,0),
            Vector3(0,0,1), Vector3(0,0,-1)
        };
        
        // Collect candidate boundary probe positions
        std::vector<Vector3> boundaryCandidates;
        
        for (size_t i = 0; i < N; i++) {
            int sunlitCount = 0;
            for (int d = 0; d < 8; d++) {
                if (!hitResults[i * 8 + d]) sunlitCount++;
            }
            
            if (sunlitCount > 0 && sunlitCount < 8) {
                for (int dir = 0; dir < 4; dir++) {
                    float angle = dir * M_PI / 2.0f;
                    Vector3 offset(std::cos(angle) * 64.0f, std::sin(angle) * 64.0f, 0);
                    Vector3 newPos = finalPositions[i] + offset;
                    boundaryCandidates.push_back(newPos);
                }
            }
        }
        
        if (!boundaryCandidates.empty()) {
            // Batch solid rejection for boundary candidates
            size_t BC = boundaryCandidates.size();
            size_t bcTotalRays = BC * 6;
            std::vector<Vector3> bcRayOrigins(bcTotalRays);
            std::vector<Vector3> bcRayDirs(bcTotalRays);
            std::vector<float>   bcRayMaxDists(bcTotalRays);
            
            for (size_t i = 0; i < BC; i++) {
                for (int d = 0; d < 6; d++) {
                    size_t idx = i * 6 + d;
                    bcRayOrigins[idx] = boundaryCandidates[i] + cardinalDirs[d] * 2.0f;
                    bcRayDirs[idx] = cardinalDirs[d];
                    bcRayMaxDists[idx] = 24.0f;
                }
            }
            
            std::vector<uint8_t> bcHits(bcTotalRays, 0);
            if (bcTotalRays > 0 && HIPRTTrace::IsSceneReady()) {
                HIPRTTrace::BatchTestVisibility(static_cast<int>(bcTotalRays), bcRayOrigins.data(),
                                                bcRayDirs.data(), bcRayMaxDists.data(), bcHits.data());
            }
            
            for (size_t i = 0; i < BC; i++) {
                size_t base = i * 6;
                bool allHit = bcHits[base] && bcHits[base+1] && bcHits[base+2] &&
                              bcHits[base+3] && bcHits[base+4] && bcHits[base+5];
                if (allHit) continue;
                
                Vector3 &newPos = boundaryCandidates[i];
                
                bool tooClose = false;
                for (const Vector3 &existing : finalPositions) {
                    Vector3 delta = newPos - existing;
                    if (vector3_dot(delta, delta) < 48.0f * 48.0f) {
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose) {
                    for (const Vector3 &existing : shadowBoundaryProbes) {
                        Vector3 delta = newPos - existing;
                        if (vector3_dot(delta, delta) < 48.0f * 48.0f) {
                            tooClose = true;
                            break;
                        }
                    }
                }
                
                if (!tooClose) {
                    shadowBoundaryProbes.push_back(newPos);
                }
            }
        }
    }
    
    // Add shadow boundary probes to final list
    for (const Vector3 &sbp : shadowBoundaryProbes) {
        finalPositions.push_back(sbp);
    }
    
    {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - step6Start).count();
        if (!shadowBoundaryProbes.empty()) {
            Sys_FPrintf(SYS_VRB, "     Added %zu shadow boundary probes (%.2fs)\n", shadowBoundaryProbes.size(), elapsed);
        } else {
            Sys_FPrintf(SYS_VRB, "     No shadow boundary probes needed (%.2fs)\n", elapsed);
        }
    }
    
    // =========================================================================
    // Step 7: Gap-filling - add probes on a regular grid across all floors
    // The Voronoi approach is geometry-driven and misses open floor areas.
    // This ensures every walkable floor area has probe coverage.
    // =========================================================================
    Sys_FPrintf(SYS_VRB, "     Gap-filling floor areas with grid...\n");
    
    constexpr float FLOOR_GRID_SPACING = 128.0f;  // Dense grid for good floor coverage
    constexpr float PROBE_HEIGHT_ABOVE_FLOOR = 64.0f;  // Player height above floor
    constexpr float MIN_PROBE_SPACING = 64.0f;  // Don't place if another probe is closer than this
    
    std::vector<Vector3> gapFillProbes;
    
    // Compute tighter bounds from actual mesh geometry (worldBounds might include skybox)
    MinMax meshBounds;
    for (const Shared::Mesh_t &mesh : Shared::meshes) {
        // Skip sky surfaces
        if (mesh.shaderInfo && (mesh.shaderInfo->compileFlags & C_SKY))
            continue;
        meshBounds.extend(mesh.minmax.mins);
        meshBounds.extend(mesh.minmax.maxs);
    }
    
    if (!meshBounds.valid()) {
        meshBounds = worldBounds;
    }
    
    Vector3 meshSize = meshBounds.maxs - meshBounds.mins;
    Sys_FPrintf(SYS_VRB, "     Mesh bounds: (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)\n",
               meshBounds.mins[0], meshBounds.mins[1], meshBounds.mins[2],
               meshBounds.maxs[0], meshBounds.maxs[1], meshBounds.maxs[2]);
    
    // Use 2D grid based on mesh bounds (not world bounds which may be huge)
    int gridX = std::max(1, (int)std::ceil(meshSize[0] / FLOOR_GRID_SPACING));
    int gridY = std::max(1, (int)std::ceil(meshSize[1] / FLOOR_GRID_SPACING));
    
    gridX = std::min(gridX, 256);
    gridY = std::min(gridY, 256);
    
    Sys_FPrintf(SYS_VRB, "     Floor grid: %d x %d (%d cells)\n", gridX, gridY, gridX * gridY);
    
    int floorsFound = 0;
    int probesAdded = 0;
    
    for (int iy = 0; iy < gridY; iy++) {
        for (int ix = 0; ix < gridX; ix++) {
            float posX = meshBounds.mins[0] + (ix + 0.5f) * (meshSize[0] / gridX);
            float posY = meshBounds.mins[1] + (iy + 0.5f) * (meshSize[1] / gridY);
            
            // Try multiple trace start heights to handle different scenarios
            // Start from reasonable heights within the mesh bounds
            float floorZ = meshBounds.mins[2];
            bool foundFloor = false;
            
            // Try tracing from several heights (top of mesh, middle, etc.)
            float traceHeights[] = {
                meshBounds.maxs[2] - 8.0f,
                meshBounds.mins[2] + meshSize[2] * 0.75f,
                meshBounds.mins[2] + meshSize[2] * 0.5f,
                meshBounds.mins[2] + meshSize[2] * 0.25f
            };
            
            for (float startZ : traceHeights) {
                if (foundFloor) break;
                
                Vector3 rayStart(posX, posY, startZ);
                Vector3 rayDir(0, 0, -1);
                float maxTrace = startZ - meshBounds.mins[2] + 16.0f;
                
                if (HIPRTTrace::IsSceneReady()) {
                    float hitDist;
                    Vector3 hitNormal;
                    int meshIndex;
                    if (HIPRTTrace::TraceRay(rayStart, rayDir, maxTrace, hitDist, hitNormal, meshIndex)) {
                        // Accept any upward-facing surface as floor
                        if (hitNormal[2] > 0.1f) {
                            floorZ = rayStart[2] - hitDist;
                            foundFloor = true;
                        }
                    }
                } else {
                    // Fallback without HIPRT
                    if (TraceRayAgainstMeshes(rayStart, rayDir, maxTrace)) {
                        floorZ = meshBounds.mins[2] + 16.0f;
                        foundFloor = true;
                    }
                }
            }
            
            if (foundFloor) floorsFound++;
            
            if (!foundFloor) continue;
            
            Vector3 probePos(posX, posY, floorZ + PROBE_HEIGHT_ABOVE_FLOOR);
            
            // Quick solid check - very relaxed
            if (IsPositionInsideSolid(probePos, 4.0f)) {
                continue;
            }
            
            // Only skip if VERY close to an existing probe (avoid duplicates)
            bool tooClose = false;
            for (const Vector3 &existing : finalPositions) {
                Vector3 delta = probePos - existing;
                if (vector3_dot(delta, delta) < MIN_PROBE_SPACING * MIN_PROBE_SPACING) {
                    tooClose = true;
                    break;
                }
            }
            if (!tooClose) {
                for (const Vector3 &existing : gapFillProbes) {
                    Vector3 delta = probePos - existing;
                    if (vector3_dot(delta, delta) < MIN_PROBE_SPACING * MIN_PROBE_SPACING) {
                        tooClose = true;
                        break;
                    }
                }
            }
            
            if (!tooClose) {
                gapFillProbes.push_back(probePos);
                probesAdded++;
            }
        }
    }
    
    Sys_FPrintf(SYS_VRB, "     Found %d floor cells, added %d gap-fill probes\n", floorsFound, probesAdded);
    
    // Add gap-fill probes
    for (const Vector3 &gfp : gapFillProbes) {
        finalPositions.push_back(gfp);
    }
    
    if (!gapFillProbes.empty()) {
        Sys_FPrintf(SYS_VRB, "     Added %zu gap-fill probes in empty areas\n", gapFillProbes.size());
    }
    
    probePositions = std::move(finalPositions);
    Sys_Printf("     Generated %zu Voronoi-based probe positions\n", probePositions.size());
}

/*
    AssignStaticLightsToProbe
    Finds the most influential worldlights for a probe at the given position.
    Assigns up to 4 light indices to the probe's staticLightIndexes array.
    Sets staticLightFlags[0] based on how many valid lights were found.
    
    The algorithm:
    1. Skip sky lights (emit_skyambient, emit_skylight) - they don't use indices
    2. For each point/spot light, calculate influence based on distance and intensity
    3. Sort by influence and take the top 4
    4. Assign indices, using 0xFFFF for empty slots
*/
static void AssignStaticLightsToProbe(const Vector3 &probePos, LightProbe_t &probe) {
    // Structure to hold light influence data
    struct LightInfluence {
        uint16_t index;
        float influence;
    };
    
    std::vector<LightInfluence> influences;
    influences.reserve(ApexLegends::Bsp::worldLights.size());
    
    // Calculate influence for each non-sky worldlight
    for (size_t i = 0; i < ApexLegends::Bsp::worldLights.size(); i++) {
        const WorldLight_t &light = ApexLegends::Bsp::worldLights[i];
        
        // Skip sky lights - they're handled separately via SH ambient
        if (light.type == emit_skyambient || light.type == emit_skylight) {
            continue;
        }
        
        // Calculate distance to light
        Vector3 delta = light.origin - probePos;
        float distSq = vector3_dot(delta, delta);
        float dist = std::sqrt(distSq);
        
        // Calculate light intensity magnitude
        float intensityMag = vector3_length(light.intensity);
        if (intensityMag < 0.001f) {
            continue;
        }
        
        // Calculate influence based on intensity and distance falloff
        // Use quadratic falloff as a reasonable approximation
        float influence = intensityMag / (1.0f + distSq * 0.0001f);
        
        // For spotlights, reduce influence if probe is outside the cone
        if (light.type == emit_spotlight) {
            Vector3 dirToProbe = vector3_normalised(delta);
            float dot = -vector3_dot(dirToProbe, light.normal);  // Negative because delta points from light to probe
            
            // If outside outer cone, skip this light
            if (dot < light.stopdot2) {
                continue;
            }
            
            // Attenuate based on cone falloff
            if (dot < light.stopdot) {
                float t = (dot - light.stopdot2) / (light.stopdot - light.stopdot2);
                influence *= t * t;  // Quadratic falloff in penumbra
            }
        }
        
        if (influence > 0.001f) {
            influences.push_back({static_cast<uint16_t>(i), influence});
        }
    }
    
    // Sort by influence (highest first)
    std::sort(influences.begin(), influences.end(), 
              [](const LightInfluence &a, const LightInfluence &b) {
                  return a.influence > b.influence;
              });
    
    // Assign up to 4 lights
    // Store raw 0-based worldlight indices in the BSP.
    // Engine's Mod_LoadLightProbes adds +32 at load time to convert to
    // global light handles (0-31 = shadow envs, 32+ = worldlights).
    int validLightCount = 0;
    for (int slot = 0; slot < 4; slot++) {
        if (slot < static_cast<int>(influences.size())) {
            probe.staticLightIndexes[slot] = influences[slot].index;
            validLightCount++;
        } else {
            probe.staticLightIndexes[slot] = 0xFFFF;  // No light in this slot
        }
    }
    
    // Set staticLightFlags - each VALID light slot needs 0xFF!
    // CRITICAL: Game breaks the loop when flag == 0, so each valid slot must have 0xFF
    // Pattern from official maps:
    //   1 light:  (255, 0, 0, 0)
    //   2 lights: (255, 255, 0, 0)
    //   3 lights: (255, 255, 255, 0)
    //   4 lights: (255, 255, 255, 255)
    for (int slot = 0; slot < 4; slot++) {
        probe.staticLightFlags[slot] = (slot < validLightCount) ? 0xFF : 0x00;
    }
}

/*
    CompressProbeList
    Remove redundant probes that can be reconstructed from neighbors.
    Adapted from Source SDK's CompressAmbientSampleList.
*/
struct ProbeCandidate {
    Vector3 pos;
    Vector3 cube[6];
    bool keep;
};

static void CompressProbeList(std::vector<ProbeCandidate> &candidates, 
                               int maxProbes = 1024) {
    if (candidates.size() <= (size_t)maxProbes) return;
    
    Sys_FPrintf(SYS_VRB, "     Compressing %zu probes to %d...\n", candidates.size(), maxProbes);
    
    // Mark all as kept initially
    for (auto &c : candidates) c.keep = true;
    
    // Iteratively remove least valuable probes
    while (true) {
        size_t keptCount = 0;
        for (const auto &c : candidates) {
            if (c.keep) keptCount++;
        }
        
        if (keptCount <= (size_t)maxProbes) break;
        
        // Find the probe with smallest color difference from its nearest neighbor
        float minDiff = FLT_MAX;
        size_t minIdx = 0;
        
        for (size_t i = 0; i < candidates.size(); i++) {
            if (!candidates[i].keep) continue;
            
            // Find nearest neighbor
            float nearestDist = FLT_MAX;
            float colorDiff = 0;
            
            for (size_t j = 0; j < candidates.size(); j++) {
                if (i == j || !candidates[j].keep) continue;
                
                Vector3 delta = candidates[j].pos - candidates[i].pos;
                float dist = vector3_dot(delta, delta);
                
                if (dist < nearestDist) {
                    nearestDist = dist;
                    
                    // Compute color difference between cubes
                    colorDiff = 0;
                    for (int k = 0; k < 6; k++) {
                        for (int c = 0; c < 3; c++) {
                            float diff = std::abs(candidates[i].cube[k][c] - 
                                                   candidates[j].cube[k][c]);
                            colorDiff = std::max(colorDiff, diff);
                        }
                    }
                }
            }
            
            // Score: small distance + small color diff = redundant
            float score = (1.0f / (nearestDist + 1.0f)) * (1.0f - colorDiff);
            
            if (score < minDiff || (score == minDiff && colorDiff < 0.01f)) {
                minDiff = score;
                minIdx = i;
            }
        }
        
        candidates[minIdx].keep = false;
    }
    
    // Remove non-kept probes
    std::vector<ProbeCandidate> kept;
    for (auto &c : candidates) {
        if (c.keep) kept.push_back(c);
    }
    candidates = kept;
    
    Sys_FPrintf(SYS_VRB, "     Kept %zu probes after compression\n", candidates.size());
}

// Legacy function for logging (called once to print sky info)
static void LogSkyEnvironment(const SkyEnvironment &sky) {
    Sys_FPrintf(SYS_VRB, "     Sun direction: (%.2f, %.2f, %.2f)\n", sky.sunDir[0], sky.sunDir[1], sky.sunDir[2]);
    Sys_FPrintf(SYS_VRB, "     Sun intensity: %.2f, color: (%.2f, %.2f, %.2f)\n", sky.sunIntensity, sky.sunColor[0], sky.sunColor[1], sky.sunColor[2]);
    Sys_FPrintf(SYS_VRB, "     Ambient color: (%.2f, %.2f, %.2f)\n", sky.ambientColor[0], sky.ambientColor[1], sky.ambientColor[2]);
}


/*
    BuildLightProbeTreeRecursive
    Recursively builds a KD-tree for spatial lightprobe lookup.
    
    The tree uses axis-aligned splits to partition probes into leaves.
    Each leaf can contain up to MAX_PROBES_PER_LEAF probes.
    The split axis cycles through X(0), Y(1), Z(2).
    
    Tree node format:
    - tag: (index << 2) | type
      - type 0/1/2: internal node, split on X/Y/Z axis, index = child node index
      - type 3: leaf node, index = first probe ref index
    - value: split coordinate (float) for internal, ref count (uint32) for leaf
*/
constexpr int MAX_PROBES_PER_LEAF = 4;  // Max probes per leaf node (official maps use 1-4)

struct ProbeRefRange {
    uint32_t start;
    uint32_t count;
};

// Forward declaration for recursive building
static void BuildLightProbeTreeRecursiveFill(
    std::vector<uint32_t> &refIndices,
    uint32_t start, uint32_t count,
    int depth,
    uint32_t nodeIndex);

static uint32_t BuildLightProbeTreeRecursive(
    std::vector<uint32_t> &refIndices,  // Indices into lightprobeReferences (will be reordered)
    uint32_t start, uint32_t count,      // Range in refIndices to process
    int depth)
{
    // Get references for bounds calculation
    const auto &refs = ApexLegends::Bsp::lightprobeReferences;
    
    // If few enough probes, create a leaf
    if (count <= MAX_PROBES_PER_LEAF || depth > 20) {
        LightProbeTree_t leaf;
        leaf.tag = (start << 2) | 3;  // type 3 = leaf
        leaf.refCount = count;
        
        uint32_t nodeIndex = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeTree.size());
        ApexLegends::Bsp::lightprobeTree.push_back(leaf);
        return nodeIndex;
    }
    
    // Calculate bounds of probes in this range
    Vector3 mins(FLT_MAX, FLT_MAX, FLT_MAX);
    Vector3 maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    
    for (uint32_t i = start; i < start + count; i++) {
        const Vector3 &pos = refs[refIndices[i]].origin;
        mins[0] = std::min(mins[0], pos[0]);
        mins[1] = std::min(mins[1], pos[1]);
        mins[2] = std::min(mins[2], pos[2]);
        maxs[0] = std::max(maxs[0], pos[0]);
        maxs[1] = std::max(maxs[1], pos[1]);
        maxs[2] = std::max(maxs[2], pos[2]);
    }
    
    // Choose split axis - use the longest axis for best balance
    Vector3 extents = maxs - mins;
    int splitAxis = 0;
    if (extents[1] > extents[0] && extents[1] >= extents[2]) splitAxis = 1;
    else if (extents[2] > extents[0] && extents[2] > extents[1]) splitAxis = 2;
    
    // Choose split position - median of probe positions on this axis
    std::vector<float> axisValues;
    axisValues.reserve(count);
    for (uint32_t i = start; i < start + count; i++) {
        axisValues.push_back(refs[refIndices[i]].origin[splitAxis]);
    }
    std::sort(axisValues.begin(), axisValues.end());
    float splitValue = axisValues[count / 2];
    
    // Partition probes around split plane
    // Move all probes <= splitValue to left side
    uint32_t leftCount = 0;
    for (uint32_t i = start; i < start + count; i++) {
        if (refs[refIndices[i]].origin[splitAxis] <= splitValue) {
            std::swap(refIndices[start + leftCount], refIndices[i]);
            leftCount++;
        }
    }
    
    // Handle edge case where all probes are on one side
    if (leftCount == 0) leftCount = 1;
    if (leftCount == count) leftCount = count - 1;
    
    uint32_t rightCount = count - leftCount;
    
    // Reserve this internal node
    uint32_t nodeIndex = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeTree.size());
    ApexLegends::Bsp::lightprobeTree.push_back(LightProbeTree_t{});  // Placeholder
    
    // CRITICAL: Children must be consecutive in the array!
    // Left child at index childIdx, right child at childIdx+1
    // Reserve both slots NOW before recursing
    uint32_t childIdx = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeTree.size());
    ApexLegends::Bsp::lightprobeTree.push_back(LightProbeTree_t{});  // Left child placeholder
    ApexLegends::Bsp::lightprobeTree.push_back(LightProbeTree_t{});  // Right child placeholder
    
    // Fill in this node first (we know child indices now)
    LightProbeTree_t &node = ApexLegends::Bsp::lightprobeTree[nodeIndex];
    node.tag = (childIdx << 2) | splitAxis;  // type 0/1/2 = X/Y/Z split
    
    // Store split value as float
    union {
        float f;
        uint32_t i;
    } conv;
    conv.f = splitValue;
    node.refCount = conv.i;  // refCount field holds splitValue for internal nodes
    
    // Now recursively fill the children
    BuildLightProbeTreeRecursiveFill(refIndices, start, leftCount, depth + 1, childIdx);
    BuildLightProbeTreeRecursiveFill(refIndices, start + leftCount, rightCount, depth + 1, childIdx + 1);
    
    return nodeIndex;
}

// Version that fills a pre-allocated node slot
static void BuildLightProbeTreeRecursiveFill(
    std::vector<uint32_t> &refIndices,
    uint32_t start, uint32_t count,
    int depth,
    uint32_t nodeIndex)
{
    const auto &refs = ApexLegends::Bsp::lightprobeReferences;
    
    // If few enough probes, create a leaf
    if (count <= MAX_PROBES_PER_LEAF || depth > 20) {
        LightProbeTree_t &leaf = ApexLegends::Bsp::lightprobeTree[nodeIndex];
        leaf.tag = (start << 2) | 3;  // type 3 = leaf
        leaf.refCount = count;
        return;
    }
    
    // Calculate bounds
    Vector3 mins(FLT_MAX, FLT_MAX, FLT_MAX);
    Vector3 maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    
    for (uint32_t i = start; i < start + count; i++) {
        const Vector3 &pos = refs[refIndices[i]].origin;
        mins[0] = std::min(mins[0], pos[0]);
        mins[1] = std::min(mins[1], pos[1]);
        mins[2] = std::min(mins[2], pos[2]);
        maxs[0] = std::max(maxs[0], pos[0]);
        maxs[1] = std::max(maxs[1], pos[1]);
        maxs[2] = std::max(maxs[2], pos[2]);
    }
    
    // Choose split axis
    Vector3 extents = maxs - mins;
    int splitAxis = 0;
    if (extents[1] > extents[0] && extents[1] >= extents[2]) splitAxis = 1;
    else if (extents[2] > extents[0] && extents[2] > extents[1]) splitAxis = 2;
    
    // Choose split position (median)
    std::vector<float> axisValues;
    axisValues.reserve(count);
    for (uint32_t i = start; i < start + count; i++) {
        axisValues.push_back(refs[refIndices[i]].origin[splitAxis]);
    }
    std::sort(axisValues.begin(), axisValues.end());
    float splitValue = axisValues[count / 2];
    
    // Partition probes
    uint32_t leftCount = 0;
    for (uint32_t i = start; i < start + count; i++) {
        if (refs[refIndices[i]].origin[splitAxis] <= splitValue) {
            std::swap(refIndices[start + leftCount], refIndices[i]);
            leftCount++;
        }
    }
    
    if (leftCount == 0) leftCount = 1;
    if (leftCount == count) leftCount = count - 1;
    uint32_t rightCount = count - leftCount;
    
    // Reserve child slots (consecutive pair)
    uint32_t childIdx = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeTree.size());
    ApexLegends::Bsp::lightprobeTree.push_back(LightProbeTree_t{});  // Left
    ApexLegends::Bsp::lightprobeTree.push_back(LightProbeTree_t{});  // Right
    
    // Fill this node
    LightProbeTree_t &node = ApexLegends::Bsp::lightprobeTree[nodeIndex];
    node.tag = (childIdx << 2) | splitAxis;
    
    union { float f; uint32_t i; } conv;
    conv.f = splitValue;
    node.refCount = conv.i;
    
    // Recurse
    BuildLightProbeTreeRecursiveFill(refIndices, start, leftCount, depth + 1, childIdx);
    BuildLightProbeTreeRecursiveFill(refIndices, start + leftCount, rightCount, depth + 1, childIdx + 1);
}

/*
    BuildLightProbeTree
    Builds the spatial lookup tree for lightprobes.
    This is a KD-tree that allows efficient nearest-probe lookup.
*/
static void BuildLightProbeTree() {
    ApexLegends::Bsp::lightprobeTree.clear();
    
    uint32_t numRefs = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeReferences.size());
    
    if (numRefs == 0) {
        // No probes - create empty leaf
        LightProbeTree_t leaf;
        leaf.tag = (0 << 2) | 3;
        leaf.refCount = 0;
        ApexLegends::Bsp::lightprobeTree.push_back(leaf);
        return;
    }
    
    // Create index array for sorting
    std::vector<uint32_t> refIndices(numRefs);
    for (uint32_t i = 0; i < numRefs; i++) {
        refIndices[i] = i;
    }
    
    // Build tree recursively
    BuildLightProbeTreeRecursive(refIndices, 0, numRefs, 0);
    
    // Reorder lightprobeReferences to match the tree's expected order
    std::vector<LightProbeRef_t> reorderedRefs(numRefs);
    for (uint32_t i = 0; i < numRefs; i++) {
        reorderedRefs[i] = ApexLegends::Bsp::lightprobeReferences[refIndices[i]];
    }
    ApexLegends::Bsp::lightprobeReferences = std::move(reorderedRefs);
    
    Sys_FPrintf(SYS_VRB, "     Built KD-tree with %zu nodes for %u probes\n", 
               ApexLegends::Bsp::lightprobeTree.size(), numRefs);
}


/*
    EmitSingleLightProbe
    Creates a minimal single light probe at world center.
    Used when -nolightprobes or -singlelightprobe is specified
    to skip the expensive GPU ray tracing probe generation.
*/
void ApexLegends::EmitSingleLightProbe() {
    Sys_FPrintf(SYS_VRB, "--- EmitSingleLightProbe ---\n");
    
    ApexLegends::Bsp::lightprobes.clear();
    ApexLegends::Bsp::lightprobeReferences.clear();
    ApexLegends::Bsp::lightprobeTree.clear();
    ApexLegends::Bsp::lightprobeParentInfos.clear();
    ApexLegends::Bsp::staticPropLightprobeIndices.clear();
    
    // Calculate world center from meshes
    MinMax worldBounds;
    for (const Shared::Mesh_t &mesh : Shared::meshes) {
        worldBounds.extend(mesh.minmax.mins);
        worldBounds.extend(mesh.minmax.maxs);
    }
    if (!worldBounds.valid()) {
        worldBounds.extend(Vector3(-1024, -1024, -512));
        worldBounds.extend(Vector3(1024, 1024, 512));
    }
    Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
    
    // Create a default probe with neutral ambient
    LightProbe_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.staticLightIndexes[0] = 0xFFFF;
    probe.staticLightIndexes[1] = 0xFFFF;
    probe.staticLightIndexes[2] = 0xFFFF;
    probe.staticLightIndexes[3] = 0xFFFF;
    probe.staticLightFlags[0] = 0x00;
    probe.staticLightFlags[1] = 0x00;
    probe.staticLightFlags[2] = 0x00;
    probe.staticLightFlags[3] = 0x00;
    probe.lightingFlags = 0x0096;
    probe.reserved = 0xFFFF;
    probe.padding0 = 0xFFFFFFFF;
    probe.padding1 = 0x00000000;
    
    // Set neutral ambient SH (DC term only, small positive value)
    // SH coefficient [3] is the DC (average) term, scale is 8192
    for (int ch = 0; ch < 3; ch++) {
        probe.ambientSH[ch][0] = 0;  // X gradient
        probe.ambientSH[ch][1] = 0;  // Y gradient
        probe.ambientSH[ch][2] = 0;  // Z gradient
        probe.ambientSH[ch][3] = 512; // DC (small neutral ambient)
    }
    
    ApexLegends::Bsp::lightprobes.push_back(probe);
    
    // Create probe reference
    LightProbeRef_t ref;
    ref.origin = center;
    ref.lightProbeIndex = 0;
    ref.cubemapID = -1;
    ref.padding = 0;
    ApexLegends::Bsp::lightprobeReferences.push_back(ref);
    
    // Build minimal tree (single leaf)
    LightProbeTree_t leaf;
    leaf.tag = (0 << 2) | 3;  // index 0, type 3 = leaf
    leaf.refCount = 1;
    ApexLegends::Bsp::lightprobeTree.push_back(leaf);
    
    // Create parent info for worldspawn
    LightProbeParentInfo_t info;
    info.brushIdx = 0;
    info.cubemapIdx = 0;
    info.lightProbeCount = 1;
    info.firstLightProbeRef = 0;
    info.lightProbeTreeHead = 0;
    info.lightProbeTreeNodeCount = 1;
    info.lightProbeRefCount = 1;
    ApexLegends::Bsp::lightprobeParentInfos.push_back(info);
    
    Sys_Printf("     %9d light probe (stub)\n", 1);
    
    // Populate static prop lightprobe indices (all point to single probe)
    const uint32_t numStaticProps = ApexLegends::Bsp::gameLumpPropHeader.numStaticProps;
    if (numStaticProps > 0) {
        ApexLegends::Bsp::staticPropLightprobeIndices.resize(numStaticProps, 0);
        Sys_FPrintf(SYS_VRB, "     %9u static prop lightprobe indices (all -> probe 0)\n", numStaticProps);
    }
}


void ApexLegends::EmitLightProbes() {
    Sys_FPrintf(SYS_VRB, "--- EmitLightProbes ---\n");
    
    ApexLegends::Bsp::lightprobes.clear();
    ApexLegends::Bsp::lightprobeReferences.clear();
    ApexLegends::Bsp::lightprobeTree.clear();
    ApexLegends::Bsp::lightprobeParentInfos.clear();
    ApexLegends::Bsp::staticPropLightprobeIndices.clear();
    
    // Get sky environment first (needed for lighting computation)
    SkyEnvironment sky = GetSkyEnvironment();
    LogSkyEnvironment(sky);
    
    // Calculate world bounds from all meshes
    MinMax worldBounds;
    for (const Shared::Mesh_t &mesh : Shared::meshes) {
        worldBounds.extend(mesh.minmax.mins);
        worldBounds.extend(mesh.minmax.maxs);
    }
    
    if (!worldBounds.valid()) {
        worldBounds.extend(Vector3(-1024, -1024, -512));
        worldBounds.extend(Vector3(1024, 1024, 512));
    }
    
    // Collect probe positions - either from entities or generate grid
    std::vector<Vector3> probePositions;
    
    for (const entity_t &entity : entities) {
        const char *classname = entity.classname();
        if (striEqual(classname, "info_lightprobe")) {
            Vector3 origin;
            if (entity.read_keyvalue(origin, "origin")) {
                probePositions.push_back(origin);
            }
        }
    }
    
    size_t manualProbeCount = probePositions.size();
    if (manualProbeCount > 0) {
        Sys_Printf("     Found %zu info_lightprobe entities\n", manualProbeCount);
    }
    
    // Always generate Voronoi-based probes, combining with any manual ones
    Sys_FPrintf(SYS_VRB, "     Generating Voronoi-based placement...\n");
    std::vector<Vector3> generatedPositions;
    GenerateProbePositionsVoronoi(worldBounds, generatedPositions);
    
    // Add generated probes, but skip any too close to manual probes
    constexpr float MANUAL_PROBE_EXCLUSION_RADIUS = 48.0f;
    size_t skippedNearManual = 0;
    
    for (const Vector3 &genPos : generatedPositions) {
        bool tooCloseToManual = false;
        for (size_t i = 0; i < manualProbeCount; i++) {
            Vector3 delta = genPos - probePositions[i];
            if (vector3_dot(delta, delta) < MANUAL_PROBE_EXCLUSION_RADIUS * MANUAL_PROBE_EXCLUSION_RADIUS) {
                tooCloseToManual = true;
                skippedNearManual++;
                break;
            }
        }
        if (!tooCloseToManual) {
            probePositions.push_back(genPos);
        }
    }
    
    if (manualProbeCount > 0) {
        Sys_Printf("     Combined %zu manual + %zu generated probes (%zu skipped near manual)\n", 
                   manualProbeCount, generatedPositions.size() - skippedNearManual, skippedNearManual);
    }
    
    // Ensure we have at least one probe
    if (probePositions.empty()) {
        Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
        probePositions.push_back(center);
        Sys_Printf("     Using single probe at world center\n");
    }
    
    // Create base probe template with correct default values
    // Based on analysis of official Apex Legends BSP files
    LightProbe_t baseProbe;
    memset(&baseProbe, 0, sizeof(baseProbe));
    baseProbe.staticLightIndexes[0] = 0xFFFF;
    baseProbe.staticLightIndexes[1] = 0xFFFF;
    baseProbe.staticLightIndexes[2] = 0xFFFF;
    baseProbe.staticLightIndexes[3] = 0xFFFF;
    // staticLightFlags: 0xFF for each valid light slot, 0x00 for unused
    // Template starts with no valid lights - AssignNearestLights will set these
    baseProbe.staticLightFlags[0] = 0x00;
    baseProbe.staticLightFlags[1] = 0x00;
    baseProbe.staticLightFlags[2] = 0x00;
    baseProbe.staticLightFlags[3] = 0x00;
    baseProbe.lightingFlags = 0x0096;      // Usually 150 (0x96) in official maps
    baseProbe.reserved = 0xFFFF;           // Usually 0xFFFF in official maps
    baseProbe.padding0 = 0xFFFFFFFF;       // Usually 0xFFFFFFFF in official maps  
    baseProbe.padding1 = 0x00000000;       // Always 0 in official maps
    
    // Count non-sky worldlights for point light shadow rays
    std::vector<size_t> pointLightIndices;
    for (size_t li = 0; li < ApexLegends::Bsp::worldLights.size(); li++) {
        const WorldLight_t &light = ApexLegends::Bsp::worldLights[li];
        if (light.type == emit_skyambient || light.type == emit_skylight) continue;
        pointLightIndices.push_back(li);
    }
    
    // Compute per-probe lighting using batch GPU dispatch
    Sys_Printf("     Computing lighting for %zu probes (%zu point lights)...\n", probePositions.size(), pointLightIndices.size());
    auto probeLightStart = std::chrono::high_resolution_clock::now();
    
    std::vector<ProbeCandidate> candidates;
    candidates.reserve(probePositions.size());
    
    size_t numProbes = probePositions.size();
    size_t numPointLights = pointLightIndices.size();
    
    // ===== Phase 1: Batch all 162-direction visibility rays for all probes =====
    Sys_FPrintf(SYS_VRB, "     Phase 1: Generating %zu direction rays (%zu probes x 162 dirs)...\n",
               numProbes * NUM_SPHERE_NORMALS, numProbes);
    
    size_t totalDirRays = numProbes * NUM_SPHERE_NORMALS;
    std::vector<Vector3> dirRayOrigins(totalDirRays);
    std::vector<Vector3> dirRayDirs(totalDirRays);
    std::vector<float>   dirRayMaxDists(totalDirRays, LIGHT_PROBE_TRACE_DIST);
    
    for (size_t p = 0; p < numProbes; p++) {
        const Vector3 &pos = probePositions[p];
        for (int d = 0; d < NUM_SPHERE_NORMALS; d++) {
            size_t idx = p * NUM_SPHERE_NORMALS + d;
            dirRayOrigins[idx] = pos + g_SphereNormals[d] * 2.0f;
            dirRayDirs[idx] = g_SphereNormals[d];
        }
    }
    
    // Batch visibility test (hit = geometry, miss = sky)
    std::vector<uint8_t> dirHits(totalDirRays, 0);
    if (totalDirRays > 0 && HIPRTTrace::IsSceneReady()) {
        HIPRTTrace::BatchTestVisibility(static_cast<int>(totalDirRays), dirRayOrigins.data(),
                                        dirRayDirs.data(), dirRayMaxDists.data(), dirHits.data());
    }
    
    {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - probeLightStart).count();
        Sys_FPrintf(SYS_VRB, "     Phase 1 complete: %zu direction rays dispatched (%.2fs)\n", totalDirRays, elapsed);
    }
    
    // ===== Phase 2: Batch closest-hit for rays that hit geometry (need distance) =====
    // Collect indices of rays that hit
    std::vector<size_t> hitRayIndices;
    hitRayIndices.reserve(totalDirRays / 2);
    for (size_t i = 0; i < totalDirRays; i++) {
        if (dirHits[i]) hitRayIndices.push_back(i);
    }
    
    Sys_FPrintf(SYS_VRB, "     Phase 2: %zu rays hit geometry, getting distances...\n", hitRayIndices.size());
    
    // Batch closest-hit for hit rays to get distance (for distance falloff)
    std::vector<float> hitDistances(totalDirRays, -1.0f);
    if (!hitRayIndices.empty() && HIPRTTrace::IsSceneReady()) {
        size_t numHitRays = hitRayIndices.size();
        std::vector<Vector3> hitRayOrigins(numHitRays);
        std::vector<Vector3> hitRayDirections(numHitRays);
        std::vector<float>   hitRayMaxDists(numHitRays, LIGHT_PROBE_TRACE_DIST);
        std::vector<float>   hitRayDists(numHitRays, -1.0f);
        
        for (size_t i = 0; i < numHitRays; i++) {
            hitRayOrigins[i] = dirRayOrigins[hitRayIndices[i]];
            hitRayDirections[i] = dirRayDirs[hitRayIndices[i]];
        }
        
        HIPRTTrace::BatchTraceRay(static_cast<int>(numHitRays), hitRayOrigins.data(),
                                   hitRayDirections.data(), hitRayMaxDists.data(), hitRayDists.data());
        
        // Map distances back to the full array
        for (size_t i = 0; i < numHitRays; i++) {
            hitDistances[hitRayIndices[i]] = hitRayDists[i];
        }
    }
    
    {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - probeLightStart).count();
        Sys_FPrintf(SYS_VRB, "     Phase 2 complete (%.2fs)\n", elapsed);
    }
    
    // ===== Phase 3: Batch point light shadow rays for all probes =====
    // Pre-compute per-probe per-light ray data
    struct ProbeLightRayInfo {
        Vector3 dirToLight;
        float dist;
        float falloff;
        Vector3 lightColor;
    };
    
    std::vector<Vector3> lightRayOrigins;
    std::vector<Vector3> lightRayDirs;
    std::vector<float>   lightRayMaxDists;
    std::vector<size_t>  lightRayProbeIdx;  // which probe this ray belongs to
    std::vector<ProbeLightRayInfo> lightRayInfos;
    
    if (numPointLights > 0) {
        Sys_FPrintf(SYS_VRB, "     Phase 3: Generating point light shadow rays (%zu probes x %zu lights)...\n",
                   numProbes, numPointLights);
        
        lightRayOrigins.reserve(numProbes * numPointLights);
        lightRayDirs.reserve(numProbes * numPointLights);
        lightRayMaxDists.reserve(numProbes * numPointLights);
        lightRayProbeIdx.reserve(numProbes * numPointLights);
        lightRayInfos.reserve(numProbes * numPointLights);
        
        for (size_t p = 0; p < numProbes; p++) {
            const Vector3 &pos = probePositions[p];
            
            for (size_t li : pointLightIndices) {
                const WorldLight_t &light = ApexLegends::Bsp::worldLights[li];
                Vector3 lightPos(light.origin[0], light.origin[1], light.origin[2]);
                Vector3 delta = lightPos - pos;
                float distSq = vector3_dot(delta, delta);
                
                if (distSq < 1.0f) continue;
                
                float dist = std::sqrt(distSq);
                Vector3 dirToLight = delta * (1.0f / dist);
                
                lightRayOrigins.push_back(pos + dirToLight * 2.0f);
                lightRayDirs.push_back(dirToLight);
                lightRayMaxDists.push_back(dist - 4.0f);
                lightRayProbeIdx.push_back(p);
                
                ProbeLightRayInfo info;
                info.dirToLight = dirToLight;
                info.dist = dist;
                info.falloff = 1.0f / (distSq + 1.0f);
                info.lightColor = light.intensity * 0.01f;
                lightRayInfos.push_back(info);
            }
        }
        
        // Batch dispatch
        std::vector<uint8_t> lightHits(lightRayOrigins.size(), 0);
        if (!lightRayOrigins.empty() && HIPRTTrace::IsSceneReady()) {
            HIPRTTrace::BatchTestVisibility(static_cast<int>(lightRayOrigins.size()),
                                            lightRayOrigins.data(), lightRayDirs.data(),
                                            lightRayMaxDists.data(), lightHits.data());
        }
        
        // Store hit results for Phase 4 processing
        // We'll use the lightHits vector directly in Phase 4
        // (swap into a member variable for use below)
        lightRayOrigins.clear(); // free memory, we have lightHits and lightRayInfos
        lightRayDirs.clear();
        lightRayMaxDists.clear();
        
        // Process point light contributions per probe
        // Build per-probe light box contributions
        std::vector<std::array<Vector3, 6>> probeLightContribs(numProbes);
        for (size_t p = 0; p < numProbes; p++) {
            for (int i = 0; i < 6; i++) probeLightContribs[p][i] = Vector3(0, 0, 0);
        }
        
        for (size_t r = 0; r < lightRayInfos.size(); r++) {
            if (lightHits[r]) continue;  // Light is occluded
            
            size_t probeIdx = lightRayProbeIdx[r];
            const ProbeLightRayInfo &info = lightRayInfos[r];
            
            for (int i = 0; i < 6; i++) {
                float weight = vector3_dot(info.dirToLight, g_BoxDirections[i]);
                if (weight > 0) {
                    probeLightContribs[probeIdx][i] = probeLightContribs[probeIdx][i] + 
                        info.lightColor * (weight * info.falloff);
                }
            }
        }
        
        {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - probeLightStart).count();
            Sys_FPrintf(SYS_VRB, "     Phase 3 complete: %zu light shadow rays (%.2fs)\n", lightRayInfos.size(), elapsed);
        }
        
        // ===== Phase 4: Assemble final ambient cubes from all data =====
        Sys_FPrintf(SYS_VRB, "     Phase 4: Assembling %zu probe ambient cubes...\n", numProbes);
        
        for (size_t p = 0; p < numProbes; p++) {
            ProbeCandidate candidate;
            candidate.pos = probePositions[p];
            candidate.keep = true;
            
            // Compute radcolor from direction ray results
            Vector3 radcolor[NUM_SPHERE_NORMALS];
            
            for (int d = 0; d < NUM_SPHERE_NORMALS; d++) {
                size_t rayIdx = p * NUM_SPHERE_NORMALS + d;
                const Vector3 &dir = g_SphereNormals[d];
                radcolor[d] = Vector3(0, 0, 0);
                
                if (!dirHits[rayIdx]) {
                    // Sky contribution
                    radcolor[d] = radcolor[d] + sky.ambientColor * 0.5f;
                    
                    float sunDot = vector3_dot(dir, sky.sunDir * -1.0f);
                    if (sunDot > 0) {
                        radcolor[d] = radcolor[d] + sky.sunColor * (sunDot * sky.sunIntensity * 0.5f);
                    }
                    
                    float upDot = dir[2];
                    if (upDot > 0) {
                        radcolor[d] = radcolor[d] + sky.ambientColor * (upDot * 0.3f);
                    }
                } else {
                    // Geometry hit - use neutral bounce color (surface color requires extended trace)
                    float hitDist = hitDistances[rayIdx];
                    float distFactor = 1.0f;
                    if (hitDist >= 0 && hitDist < 512.0f) {
                        distFactor = 1.0f + (512.0f - hitDist) / 512.0f * 0.5f;
                    }
                    
                    // Use ambient color * neutral reflectance since we don't have surface color
                    Vector3 bounceColor;
                    bounceColor[0] = sky.ambientColor[0] * 0.5f;
                    bounceColor[1] = sky.ambientColor[1] * 0.5f;
                    bounceColor[2] = sky.ambientColor[2] * 0.5f;
                    radcolor[d] = bounceColor * (0.4f * distFactor);
                }
            }
            
            // Accumulate into 6-sided ambient cube
            for (int j = 0; j < 6; j++) {
                float totalWeight = 0.0f;
                candidate.cube[j] = Vector3(0, 0, 0);
                
                for (int d = 0; d < NUM_SPHERE_NORMALS; d++) {
                    float weight = vector3_dot(g_SphereNormals[d], g_BoxDirections[j]);
                    if (weight > 0) {
                        totalWeight += weight;
                        candidate.cube[j] = candidate.cube[j] + radcolor[d] * weight;
                    }
                }
                
                if (totalWeight > 0.0f) {
                    candidate.cube[j] = candidate.cube[j] * (1.0f / totalWeight);
                }
                
                // Add point light contributions
                candidate.cube[j] = candidate.cube[j] + probeLightContribs[p][j];
            }
            
            candidates.push_back(candidate);
        }
    } else {
        // No point lights - simpler path using only direction rays
        for (size_t p = 0; p < numProbes; p++) {
            ProbeCandidate candidate;
            candidate.pos = probePositions[p];
            candidate.keep = true;
            
            Vector3 radcolor[NUM_SPHERE_NORMALS];
            
            for (int d = 0; d < NUM_SPHERE_NORMALS; d++) {
                size_t rayIdx = p * NUM_SPHERE_NORMALS + d;
                const Vector3 &dir = g_SphereNormals[d];
                radcolor[d] = Vector3(0, 0, 0);
                
                if (!dirHits[rayIdx]) {
                    radcolor[d] = radcolor[d] + sky.ambientColor * 0.5f;
                    float sunDot = vector3_dot(dir, sky.sunDir * -1.0f);
                    if (sunDot > 0) {
                        radcolor[d] = radcolor[d] + sky.sunColor * (sunDot * sky.sunIntensity * 0.5f);
                    }
                    float upDot = dir[2];
                    if (upDot > 0) {
                        radcolor[d] = radcolor[d] + sky.ambientColor * (upDot * 0.3f);
                    }
                } else {
                    float hitDist = hitDistances[rayIdx];
                    float distFactor = 1.0f;
                    if (hitDist >= 0 && hitDist < 512.0f) {
                        distFactor = 1.0f + (512.0f - hitDist) / 512.0f * 0.5f;
                    }
                    Vector3 bounceColor;
                    bounceColor[0] = sky.ambientColor[0] * 0.5f;
                    bounceColor[1] = sky.ambientColor[1] * 0.5f;
                    bounceColor[2] = sky.ambientColor[2] * 0.5f;
                    radcolor[d] = bounceColor * (0.4f * distFactor);
                }
            }
            
            for (int j = 0; j < 6; j++) {
                float totalWeight = 0.0f;
                candidate.cube[j] = Vector3(0, 0, 0);
                
                for (int d = 0; d < NUM_SPHERE_NORMALS; d++) {
                    float weight = vector3_dot(g_SphereNormals[d], g_BoxDirections[j]);
                    if (weight > 0) {
                        totalWeight += weight;
                        candidate.cube[j] = candidate.cube[j] + radcolor[d] * weight;
                    }
                }
                
                if (totalWeight > 0.0f) {
                    candidate.cube[j] = candidate.cube[j] * (1.0f / totalWeight);
                }
            }
            
            candidates.push_back(candidate);
        }
    }
    
    {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - probeLightStart).count();
        Sys_Printf("     Finished computing %zu probe(s) in %.2fs\n", probePositions.size(), elapsed);
    }
    
    // Compress probe list if we have too many (Source SDK style optimization)
    // Remove redundant probes that can be reconstructed from neighbors
    //CompressProbeList(candidates, LIGHT_PROBE_MAX_COUNT);
    
    // Convert candidates to final probe data
    for (const ProbeCandidate &candidate : candidates) {
        LightProbe_t probe = baseProbe;
        
        // Convert 6-sided cube to spherical harmonics
        ConvertCubeToSphericalHarmonics(candidate.cube, probe);
        
        // Assign static worldlight indices for entity lighting
        // This finds the most influential lights at this probe's position
        AssignStaticLightsToProbe(candidate.pos, probe);
        
        ApexLegends::Bsp::lightprobes.push_back(probe);
        
        // Create probe reference
        LightProbeRef_t ref;
        ref.origin = candidate.pos;
        ref.lightProbeIndex = static_cast<uint32_t>(ApexLegends::Bsp::lightprobes.size() - 1);
        ref.cubemapID = -1;
        ref.padding = 0;
        ApexLegends::Bsp::lightprobeReferences.push_back(ref);
    }
    
    // Build spatial lookup tree (KD-tree)
    // This is required for proper probe lookup - a single leaf doesn't work
    // for maps with probes spread over large distances
    BuildLightProbeTree();
    
    // Create parent info for worldspawn
    LightProbeParentInfo_t info;
    info.brushIdx = 0;
    info.cubemapIdx = 0;
    info.lightProbeCount = static_cast<uint32_t>(ApexLegends::Bsp::lightprobes.size());
    info.firstLightProbeRef = 0;
    info.lightProbeTreeHead = 0;
    info.lightProbeTreeNodeCount = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeTree.size());
    info.lightProbeRefCount = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeReferences.size());
    ApexLegends::Bsp::lightprobeParentInfos.push_back(info);
    
    Sys_Printf("     %9zu light probes\n", ApexLegends::Bsp::lightprobes.size());
    Sys_Printf("     %9zu probe references\n", ApexLegends::Bsp::lightprobeReferences.size());
    Sys_Printf("     %9zu tree nodes\n", ApexLegends::Bsp::lightprobeTree.size());
    
    // Count probes with static lights assigned
    size_t probesWithLights = 0;
    for (const auto &probe : ApexLegends::Bsp::lightprobes) {
        if (probe.staticLightIndexes[0] != 0xFFFF) {
            probesWithLights++;
        }
    }
    Sys_Printf("     %9zu probes with static lights\n", probesWithLights);

    // Populate static prop lightprobe indices (lump 0x66)
    // One entry per static prop, pointing to the nearest light probe
    const uint32_t numStaticProps = ApexLegends::Bsp::gameLumpPropHeader.numStaticProps;
    if (numStaticProps > 0 && !ApexLegends::Bsp::lightprobeReferences.empty()) {
        ApexLegends::Bsp::staticPropLightprobeIndices.resize(numStaticProps);
        for (uint32_t i = 0; i < numStaticProps; i++) {
            const ApexLegends::GameLumpProp_t &prop = ApexLegends::Bsp::gameLumpProps[i];
            // Find nearest light probe reference by distance
            float bestDistSq = FLT_MAX;
            uint32_t bestIdx = 0;
            for (uint32_t j = 0; j < ApexLegends::Bsp::lightprobeReferences.size(); j++) {
                const LightProbeRef_t &ref = ApexLegends::Bsp::lightprobeReferences[j];
                float dx = ref.origin[0] - prop.origin[0];
                float dy = ref.origin[1] - prop.origin[1];
                float dz = ref.origin[2] - prop.origin[2];
                float distSq = dx*dx + dy*dy + dz*dz;
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestIdx = ref.lightProbeIndex;
                }
            }
            ApexLegends::Bsp::staticPropLightprobeIndices[i] = bestIdx;
        }
        Sys_FPrintf(SYS_VRB, "     %9u static prop lightprobe indices\n", numStaticProps);
    }
    
    // Export probe positions for visualization in Radiant
    if (!ApexLegends::Bsp::lightprobeReferences.empty()) {
        // Write .probes file (simple XYZ format, one probe per line)
        const auto probesFilename = StringStream(source, ".probes");
        FILE *probesFile = fopen(probesFilename, "w");
        if (probesFile) {
            Sys_FPrintf(SYS_VRB, "     Writing probe positions to %s\n", probesFilename.c_str());
            
            // Header comment
            fprintf(probesFile, "# Light probe positions exported by remap\n");
            fprintf(probesFile, "# Format: X Y Z [R G B] (RGB is average ambient color, optional)\n");
            fprintf(probesFile, "# Total probes: %zu\n", ApexLegends::Bsp::lightprobeReferences.size());
            
            for (size_t i = 0; i < ApexLegends::Bsp::lightprobeReferences.size(); i++) {
                const LightProbeRef_t &ref = ApexLegends::Bsp::lightprobeReferences[i];
                
                // Get the probe's ambient color from spherical harmonics (DC term)
                float r = 0.5f, g = 0.5f, b = 0.5f;  // default gray
                if (ref.lightProbeIndex < ApexLegends::Bsp::lightprobes.size()) {
                    const LightProbe_t &probe = ApexLegends::Bsp::lightprobes[ref.lightProbeIndex];
                    // SH DC term is the average ambient - extract from coefficient 3 (not 0!)
                    // ambientSH[channel][coefficient] where channel 0=R, 1=G, 2=B
                    // Coefficients: [0]=X gradient, [1]=Y gradient, [2]=Z gradient, [3]=DC
                    // Scale back from int16 range using the same scale factor (8192)
                    r = std::min(1.0f, std::max(0.0f, (float)probe.ambientSH[0][3] / 8192.0f));
                    g = std::min(1.0f, std::max(0.0f, (float)probe.ambientSH[1][3] / 8192.0f));
                    b = std::min(1.0f, std::max(0.0f, (float)probe.ambientSH[2][3] / 8192.0f));
                }
                
                fprintf(probesFile, "%.2f %.2f %.2f %.3f %.3f %.3f\n", 
                        ref.origin[0], ref.origin[1], ref.origin[2],
                        r, g, b);
            }
            
            fclose(probesFile);
            Sys_FPrintf(SYS_VRB, "     Probe positions exported for Radiant visualization\n");
        } else {
            Sys_Warning("Could not write probe file: %s\n", probesFilename.c_str());
        }
    }
}


/*
    EmitRealTimeLightmaps
    Generate real-time lighting data per lightmap texel
    
    Lumps involved:
      0x69 (LIGHTMAP_DATA_REAL_TIME_LIGHTS) - Per-texel light indices
      0x7A (LIGHTMAP_DATA_RTL_PAGE) - Compressed page data (126 bytes per page = 63 uint16)
    
    The RTL system allows each lightmap texel to reference specific worldlights
    that can affect it at runtime. This enables:
      - Per-texel light masking (only relevant lights affect each texel)
      - Efficient runtime light updates (skip texels not affected by a light)
    
    Per-lightmap RTL data size calculation (from engine):
      rpakSize = 2 * (alignedSize + 2 * texelCount)
      where alignedSize = ((width + 3) & ~3) * ((height + 3) & ~3)
    
    Per-texel format (4 bytes):
      - bytes 0-2: 24-bit mask (which of the 63 lights in the page affect this texel)
                   Actually split: byte 0 = offset into page, bytes 1-2 = mask bits
      - byte 3: page index (which RTL page, 0-255)
      - Special value 0xFFFFFF00 means no RTL lights affect this texel
    
    RTL Pages (126 bytes = 63 uint16 each):
      - Each page contains up to 63 worldlight indices
      - Lights with WORLDLIGHT_FLAG_REALTIME flag are RTL lights
*/

// RTL page - holds 63 light indices
struct RTLPage_t {
    uint16_t lightIndices[63];
    int count;  // How many lights are in this page (0-63)
};

// Per-texel RTL info during computation
struct TexelRTLInfo_t {
    Vector3 worldPos;       // World position of this texel
    Vector3 normal;         // Surface normal at this texel
    bool valid;             // Is this texel part of a surface?
};

// Check if an RTL light can potentially affect a texel
static bool CanLightAffectTexel(const WorldLight_t &light, const Vector3 &texelPos, 
                                 const Vector3 &texelNormal, float maxRadius) {
    Vector3 lightPos(light.origin[0], light.origin[1], light.origin[2]);
    Vector3 toLight = lightPos - texelPos;
    float distSq = vector3_dot(toLight, toLight);
    float dist = sqrtf(distSq);
    
    // Range check - use light radius or maxRadius as fallback
    float effectiveRadius = (light.radius > 0) ? light.radius : maxRadius;
    if (dist > effectiveRadius) {
        return false;
    }
    
    // Facing check - light must be in front of surface
    Vector3 toLightDir = toLight * (1.0f / std::max(dist, 0.001f));
    float facing = vector3_dot(texelNormal, toLightDir);
    if (facing <= 0.0f) {
        return false;
    }
    
    // For spotlights, check cone angle
    if (light.type == 2) {  // emit_spotlight
        Vector3 lightDir(light.normal[0], light.normal[1], light.normal[2]);
        Vector3 negToLight = toLightDir * -1.0f;
        float spotDot = vector3_dot(lightDir, negToLight);
        
        // Outside outer cone - no effect
        if (spotDot < light.stopdot2) {
            return false;
        }
    }
    
    // Visibility check - trace from texel to light
    // Offset start position along normal to avoid self-intersection
    Vector3 traceStart = texelPos + texelNormal * 1.0f;
    Vector3 traceDir = vector3_normalised(lightPos - traceStart);
    float traceDist = vector3_length(lightPos - traceStart) - 1.0f;
    
    if (traceDist > 0 && TraceRayAgainstMeshes(traceStart, traceDir, traceDist)) {
        return false;  // Blocked by geometry
    }
    
    return true;
}

void ApexLegends::EmitRealTimeLightmaps() {
    Sys_Printf("--- EmitRealTimeLightmaps ---\n");
    
    // Clear RTL lumps - let engine use fallback textures
    // When m_InitialRealTimeLightData is null, engine uses:
    //   - Texture 0: TEXTURE_WHITE_UINT (0xFFFFFFFF = no RTL masking)
    //   - Texture 1: TEXTURE_BLACK
    //   - Texture 2: TEXTURE_BLACK
    // This avoids striping artifacts from incorrect BC-compressed texture data
    ApexLegends::Bsp::lightmapDataRealTimeLights.clear();
    ApexLegends::Bsp::lightmapDataRTLPage.clear();
    
    // Count RTL-capable lights for info
    size_t rtlLightCount = 0;
    for (const WorldLight_t &light : ApexLegends::Bsp::worldLights) {
        if (light.type == 1 || light.type == 2) {  // point or spotlight
            rtlLightCount++;
        }
    }
    
    // TODO: Full RTL implementation requires:
    // 1. Proper BC7 compression for texture 1 (light influence gradients)
    // 2. Proper BC4 compression for texture 2 (additional mask data)
    // Without proper BC-compressed textures, the engine shows striping artifacts
}