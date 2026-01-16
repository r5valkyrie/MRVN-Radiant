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
#include "apex_legends.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

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

// =============================================================================
// ENHANCED LIGHTING FEATURES (adapted from Source SDK VRAD concepts)
// =============================================================================

// Supersampling settings
constexpr int SUPERSAMPLE_LEVEL = 2;        // 2x2 = 4 samples per texel (set to 1 to disable)
constexpr float SUPERSAMPLE_JITTER = 0.25f; // Jitter amount for AA samples

// Radiosity settings
constexpr int RADIOSITY_BOUNCES = 2;        // Number of light bounces (0 = direct only)
constexpr float RADIOSITY_SCALE = 0.5f;     // Energy retention per bounce
constexpr int RADIOSITY_SAMPLES = 32;       // Hemisphere samples for indirect lighting

// Phong shading settings (smooth normal interpolation)
constexpr float PHONG_ANGLE_THRESHOLD = 45.0f;  // Degrees - edges sharper than this won't smooth
constexpr float SMOOTHING_GROUP_HARD_EDGE = 0.707f;  // cos(45 degrees)

// Light probe settings (adapted from Source SDK leaf_ambient_lighting.cpp)
constexpr int LIGHT_PROBE_GRID_SPACING = 256;   // Units between probes on grid
constexpr int LIGHT_PROBE_MIN_SPACING = 128;    // Minimum spacing between probes
constexpr int LIGHT_PROBE_MAX_PER_AXIS = 64;    // Max probes per axis (prevent explosion)
constexpr float LIGHT_PROBE_TRACE_DIST = 16384.0f;  // Ray trace distance


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


/*
    EdgeKey_t
    Hash key for edge lookup (vertex pair, order-independent)
*/
struct EdgeKey_t {
    uint64_t v0, v1;  // Sorted vertex indices
    
    EdgeKey_t(size_t a, size_t b) {
        if (a < b) { v0 = a; v1 = b; }
        else { v0 = b; v1 = a; }
    }
    
    bool operator==(const EdgeKey_t &other) const {
        return v0 == other.v0 && v1 == other.v1;
    }
};

struct EdgeKeyHash {
    size_t operator()(const EdgeKey_t &k) const {
        return std::hash<uint64_t>()(k.v0) ^ (std::hash<uint64_t>()(k.v1) << 1);
    }
};


/*
    EdgeShare_t
    Tracks faces that share an edge (for smooth normal interpolation)
    Adapted from Source SDK's edgeshare_t
*/
struct EdgeShare_t {
    int meshIndex[2];           // Indices of meshes sharing this edge
    int triangleIndex[2];       // Triangle indices within each mesh
    Vector3 interfaceNormal;    // Blended normal at the edge
    bool coplanar;              // Are the faces coplanar?
    int numFaces;               // How many faces share this edge (1 or 2)
};


/*
    FaceNeighbor_t
    Per-mesh neighbor information for smooth shading
    Adapted from Source SDK's faceneighbor_t
*/
struct FaceNeighbor_t {
    std::vector<int> neighborMeshes;    // Neighboring meshes that share vertices
    std::vector<Vector3> vertexNormals; // Smoothed normal per vertex
    Vector3 faceNormal;                 // Flat face normal
};


// Global edge sharing data for phong shading
namespace PhongData {
    std::unordered_map<EdgeKey_t, EdgeShare_t, EdgeKeyHash> edgeShare;
    std::vector<FaceNeighbor_t> faceNeighbors;
    bool initialized = false;
}


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
    std::vector<Vector3> luxels; // Per-texel lighting values (RGB HDR)
    std::vector<Vector3> luxelNormals;  // Per-texel interpolated normals (for phong shading)
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
    
    // Use neutral gray - engine adds dynamic ambient/sun on top
    // This is the base that indirect/bounced lighting would add to
    uint8_t neutralValue = 128;  // Neutral gray (0.5 after gamma)
    
    size_t dataSize = page.width * page.height * 8;
    page.pixels.resize(dataSize);
    for (size_t i = 0; i < dataSize; i += 8) {
        // Neutral base - dynamic lighting adds on top
        page.pixels[i + 0] = neutralValue;
        page.pixels[i + 1] = neutralValue;
        page.pixels[i + 2] = neutralValue;
        page.pixels[i + 3] = 255;  // A
        page.pixels[i + 4] = neutralValue;
        page.pixels[i + 5] = neutralValue;
        page.pixels[i + 6] = neutralValue;
        page.pixels[i + 7] = 128;  // Neutral multiplier
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
    Sys_Printf("--- SetupSurfaceLightmaps ---\n");
    
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
        surfLM.luxelNormals.resize(rect.width * rect.height, plane.normal());
        
        LightmapBuild::surfaces.push_back(surfLM);
        litSurfaces++;
        meshIndex++;
    }
    
    Sys_Printf("     %9d lit surfaces\n", litSurfaces);
    Sys_Printf("     %9d lightmap pages\n", (int)ApexLegends::Bsp::lightmapPages.size());
}


// =============================================================================
// PHONG SHADING / SMOOTH NORMAL INTERPOLATION
// Adapted from Source SDK VRAD's PairEdges() and GetPhongNormal()
// =============================================================================

/*
    BuildEdgeSharing
    Build edge-to-face mapping for smooth normal interpolation across mesh edges.
    This allows lighting to smoothly transition across connected surfaces.
*/
static void BuildEdgeSharing() {
    if (PhongData::initialized) return;
    
    Sys_Printf("     Building edge sharing for smooth normals...\n");
    
    PhongData::edgeShare.clear();
    PhongData::faceNeighbors.clear();
    PhongData::faceNeighbors.resize(Shared::meshes.size());
    
    // Build edge map: for each edge, track which meshes/triangles use it
    for (size_t meshIdx = 0; meshIdx < Shared::meshes.size(); meshIdx++) {
        const Shared::Mesh_t &mesh = Shared::meshes[meshIdx];
        
        // Skip non-lit surfaces
        if (!CHECK_FLAG(mesh.shaderInfo->surfaceFlags, S_VERTEX_LIT_BUMP)) {
            continue;
        }
        
        // Compute face normal
        Vector3 faceNormal(0, 0, 1);
        if (mesh.triangles.size() >= 3) {
            const Vector3 &v0 = mesh.vertices[mesh.triangles[0]].xyz;
            const Vector3 &v1 = mesh.vertices[mesh.triangles[1]].xyz;
            const Vector3 &v2 = mesh.vertices[mesh.triangles[2]].xyz;
            faceNormal = vector3_normalised(vector3_cross(v1 - v0, v2 - v0));
        }
        PhongData::faceNeighbors[meshIdx].faceNormal = faceNormal;
        PhongData::faceNeighbors[meshIdx].vertexNormals.resize(mesh.vertices.size(), faceNormal);
        
        // Process each triangle's edges
        for (size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
            int triIdx = static_cast<int>(i / 3);
            
            // Three edges per triangle
            for (int e = 0; e < 3; e++) {
                size_t idx0 = mesh.triangles[i + e];
                size_t idx1 = mesh.triangles[i + ((e + 1) % 3)];
                
                // Create a unique key based on vertex positions (quantized)
                const Vector3 &p0 = mesh.vertices[idx0].xyz;
                const Vector3 &p1 = mesh.vertices[idx1].xyz;
                
                // Quantize positions to handle floating point imprecision
                auto quantize = [](const Vector3 &v) -> uint64_t {
                    int32_t x = static_cast<int32_t>(v.x() * 8.0f);
                    int32_t y = static_cast<int32_t>(v.y() * 8.0f);
                    int32_t z = static_cast<int32_t>(v.z() * 8.0f);
                    return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
                           (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
                           (static_cast<uint64_t>(z & 0xFFFFF));
                };
                
                EdgeKey_t key(quantize(p0), quantize(p1));
                
                auto it = PhongData::edgeShare.find(key);
                if (it == PhongData::edgeShare.end()) {
                    // New edge
                    EdgeShare_t share;
                    share.meshIndex[0] = static_cast<int>(meshIdx);
                    share.meshIndex[1] = -1;
                    share.triangleIndex[0] = triIdx;
                    share.triangleIndex[1] = -1;
                    share.interfaceNormal = faceNormal;
                    share.coplanar = false;
                    share.numFaces = 1;
                    PhongData::edgeShare[key] = share;
                } else {
                    // Existing edge - add second face
                    EdgeShare_t &share = it->second;
                    if (share.numFaces == 1 && share.meshIndex[0] != static_cast<int>(meshIdx)) {
                        share.meshIndex[1] = static_cast<int>(meshIdx);
                        share.triangleIndex[1] = triIdx;
                        share.numFaces = 2;
                        
                        // Check if faces are smooth (angle less than threshold)
                        Vector3 otherNormal = PhongData::faceNeighbors[share.meshIndex[0]].faceNormal;
                        float dot = vector3_dot(faceNormal, otherNormal);
                        
                        if (dot > SMOOTHING_GROUP_HARD_EDGE) {
                            // Smooth edge - blend normals
                            share.interfaceNormal = vector3_normalised(faceNormal + otherNormal);
                            share.coplanar = (dot > 0.999f);
                            
                            // Track as neighbors
                            PhongData::faceNeighbors[meshIdx].neighborMeshes.push_back(share.meshIndex[0]);
                            PhongData::faceNeighbors[share.meshIndex[0]].neighborMeshes.push_back(static_cast<int>(meshIdx));
                        }
                    }
                }
            }
        }
    }
    
    // Now compute smoothed vertex normals by averaging neighbor contributions
    for (size_t meshIdx = 0; meshIdx < Shared::meshes.size(); meshIdx++) {
        FaceNeighbor_t &fn = PhongData::faceNeighbors[meshIdx];
        if (fn.neighborMeshes.empty()) continue;
        
        const Shared::Mesh_t &mesh = Shared::meshes[meshIdx];
        
        for (size_t vIdx = 0; vIdx < mesh.vertices.size(); vIdx++) {
            const Vector3 &pos = mesh.vertices[vIdx].xyz;
            Vector3 smoothNormal = fn.faceNormal;
            float totalWeight = 1.0f;
            
            // Check neighbor meshes for shared vertices
            for (int neighborIdx : fn.neighborMeshes) {
                const Shared::Mesh_t &neighbor = Shared::meshes[neighborIdx];
                const FaceNeighbor_t &neighborFN = PhongData::faceNeighbors[neighborIdx];
                
                // Find vertices at the same position
                for (size_t nv = 0; nv < neighbor.vertices.size(); nv++) {
                    Vector3 diff = neighbor.vertices[nv].xyz - pos;
                    if (vector3_length(diff) < 0.5f) {
                        // Same vertex - blend normals
                        float weight = 1.0f;
                        smoothNormal = smoothNormal + neighborFN.faceNormal * weight;
                        totalWeight += weight;
                        break;
                    }
                }
            }
            
            fn.vertexNormals[vIdx] = vector3_normalised(smoothNormal);
        }
    }
    
    PhongData::initialized = true;
    Sys_Printf("     Built %zu shared edges\n", PhongData::edgeShare.size());
}


/*
    GetPhongNormal
    Get interpolated (phong) normal at a world position on a surface.
    Adapted from Source SDK's GetPhongNormal().
*/
static Vector3 GetPhongNormal(int meshIndex, const Vector3 &worldPos, const Vector3 &flatNormal) {
    if (!PhongData::initialized || meshIndex < 0 || meshIndex >= static_cast<int>(PhongData::faceNeighbors.size())) {
        return flatNormal;
    }
    
    const FaceNeighbor_t &fn = PhongData::faceNeighbors[meshIndex];
    if (fn.neighborMeshes.empty()) {
        return flatNormal;  // No neighbors - use flat normal
    }
    
    const Shared::Mesh_t &mesh = Shared::meshes[meshIndex];
    if (mesh.vertices.empty() || mesh.triangles.size() < 3) {
        return flatNormal;
    }
    
    // Find the triangle containing this point and interpolate vertex normals
    for (size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
        const Vector3 &v0 = mesh.vertices[mesh.triangles[i]].xyz;
        const Vector3 &v1 = mesh.vertices[mesh.triangles[i + 1]].xyz;
        const Vector3 &v2 = mesh.vertices[mesh.triangles[i + 2]].xyz;
        
        // Compute barycentric coordinates
        Vector3 edge0 = v1 - v0;
        Vector3 edge1 = v2 - v0;
        Vector3 vp = worldPos - v0;
        
        float d00 = vector3_dot(edge0, edge0);
        float d01 = vector3_dot(edge0, edge1);
        float d11 = vector3_dot(edge1, edge1);
        float d20 = vector3_dot(vp, edge0);
        float d21 = vector3_dot(vp, edge1);
        
        float denom = d00 * d11 - d01 * d01;
        if (std::abs(denom) < 0.0001f) continue;
        
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        float u = 1.0f - v - w;
        
        // Check if point is in this triangle (with some tolerance)
        if (u >= -0.1f && v >= -0.1f && w >= -0.1f && u <= 1.1f && v <= 1.1f && w <= 1.1f) {
            // Interpolate vertex normals
            const Vector3 &n0 = fn.vertexNormals[mesh.triangles[i]];
            const Vector3 &n1 = fn.vertexNormals[mesh.triangles[i + 1]];
            const Vector3 &n2 = fn.vertexNormals[mesh.triangles[i + 2]];
            
            Vector3 interpNormal = n0 * u + n1 * v + n2 * w;
            return vector3_normalised(interpNormal);
        }
    }
    
    return flatNormal;
}


// =============================================================================
// SUPERSAMPLING
// Take multiple samples per texel and average for anti-aliased lighting
// =============================================================================

// Jitter offsets for 2x2 supersampling (rotated grid pattern)
static const float supersampleOffsets[4][2] = {
    { -0.25f, -0.125f },
    {  0.25f, -0.375f },
    { -0.125f, 0.375f },
    {  0.375f, 0.125f }
};


// =============================================================================
// RADIOSITY / BOUNCE LIGHTING
// Compute indirect illumination from light bouncing off surfaces
// =============================================================================

/*
    InitRadiosityPatches
    Create patches from all lightmap luxels for radiosity computation
*/
static void InitRadiosityPatches() {
    if (RadiosityData::initialized) return;
    
    RadiosityData::patches.clear();
    
    for (const SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        const Shared::Mesh_t &mesh = Shared::meshes[surf.meshIndex];
        
        // Get surface reflectivity from shader (approximate from texture name)
        Vector3 reflectivity(0.5f, 0.5f, 0.5f);  // Default 50% reflectance
        if (mesh.shaderInfo) {
            // Could parse actual texture colors here for better results
            // For now, use neutral reflectivity
            reflectivity = Vector3(0.4f, 0.4f, 0.4f);
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


/*
    ComputeLightmapLighting
    Compute lighting for each texel.
    
    IMPORTANT: emit_skyambient and emit_skylight are applied DYNAMICALLY by the engine
    from the worldLights lump (0x36). Changing _ambient/_light in the .ent file updates
    the map in real-time because the engine reads these values at runtime.
    
    Lightmaps should only contain:
    - Indirect/bounced lighting (radiosity)
    - Static point/spot light contributions that aren't realtime
    
    Enhanced with:
    - Phong shading (smooth normal interpolation across edges)
    - Supersampling (anti-aliased lighting)
    - Radiosity (bounced indirect lighting)
*/
void ApexLegends::ComputeLightmapLighting() {
    Sys_Printf("--- ComputeLightmapLighting ---\n");
    
    // Build edge sharing data for phong shading
    BuildEdgeSharing();
    
    // NOTE: We do NOT bake emit_skyambient or emit_skylight here!
    // The engine applies these dynamically from worldLights lump.
    // This is why changing _ambient/_light in .ent files works on official maps.
    
    if (ApexLegends::Bsp::worldLights.empty()) {
        Sys_Printf("  No worldlights found\n");
    }
    
    // Initialize radiosity patches for bounce lighting
    if (RADIOSITY_BOUNCES > 0) {
        InitRadiosityPatches();
    }
    
    int totalTexels = 0;
    int patchIndex = 0;  // Track patch index for radiosity
    
    Sys_Printf("     Computing direct lighting");
    if (SUPERSAMPLE_LEVEL > 1) {
        Sys_Printf(" with %dx%d supersampling", SUPERSAMPLE_LEVEL, SUPERSAMPLE_LEVEL);
    }
    Sys_Printf("...\n");
    
    for (SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        // For each texel
        for (int y = 0; y < surf.rect.height; y++) {
            for (int x = 0; x < surf.rect.width; x++) {
                // =====================================================
                // SUPERSAMPLING: Take multiple samples and average
                // =====================================================
                Vector3 accumColor(0, 0, 0);
                int numSamples = (SUPERSAMPLE_LEVEL > 1) ? 4 : 1;
                
                for (int sampleIdx = 0; sampleIdx < numSamples; sampleIdx++) {
                    // Get sample offset (jittered for anti-aliasing)
                    float offsetU = (numSamples > 1) ? supersampleOffsets[sampleIdx][0] : 0.0f;
                    float offsetV = (numSamples > 1) ? supersampleOffsets[sampleIdx][1] : 0.0f;
                    
                    // Compute world position for this sample
                    // Normalize texel to [0,1] within the rect, then map to tangent-space bounds
                    float normalizedU = (surf.rect.width > 1) ? (x + 0.5f + offsetU) / (surf.rect.width - 1) : 0.5f;
                    float normalizedV = (surf.rect.height > 1) ? (y + 0.5f + offsetV) / (surf.rect.height - 1) : 0.5f;
                    normalizedU = std::max(0.0f, std::min(1.0f, normalizedU));
                    normalizedV = std::max(0.0f, std::min(1.0f, normalizedV));
                    float localU = surf.uMin + normalizedU * (surf.uMax - surf.uMin);
                    float localV = surf.vMin + normalizedV * (surf.vMax - surf.vMin);
                    
                    Vector3 worldPos = surf.worldBounds.mins 
                        + surf.tangent * localU 
                        + surf.bitangent * localV;
                    
                    // Offset slightly along normal to avoid self-intersection
                    worldPos = worldPos + surf.plane.normal() * 0.1f;
                    
                    // =====================================================
                    // PHONG SHADING: Get interpolated normal at this position
                    // =====================================================
                    Vector3 sampleNormal = GetPhongNormal(surf.meshIndex, worldPos, surf.plane.normal());
                    
                    // Start with neutral base - engine adds dynamic ambient/sun on top
                    // A small base value prevents completely black areas
                    Vector3 sampleColor(0.1f, 0.1f, 0.1f);
                    
                    // Only bake NON-realtime static lights (point lights, spotlights)
                    // Skip emit_skyambient and emit_skylight - they're dynamic!
                    for (const WorldLight_t &light : ApexLegends::Bsp::worldLights) {
                        // Skip sky lighting - applied dynamically by engine
                        if (light.type == emit_skyambient || light.type == emit_skylight) {
                            continue;
                        }
                        
                        // Skip realtime lights - they're computed per-frame
                        if (light.flags & WORLDLIGHT_FLAG_REALTIME) {
                            continue;
                        }
                        
                        Vector3 lightPos(light.origin[0], light.origin[1], light.origin[2]);
                        Vector3 lightColor = light.intensity;
                        
                        if (light.type == emit_point) {
                            // Static point light
                            Vector3 toLight = lightPos - worldPos;
                            float dist = vector3_length(toLight);
                            if (dist < 0.001f) continue;
                            
                            Vector3 lightDir = toLight / dist;
                            // Use phong normal for smoother lighting across edges
                            float NdotL = vector3_dot(sampleNormal, lightDir);
                            
                            if (NdotL > 0) {
                                float atten = 1.0f;
                                if (light.quadratic_attn > 0 || light.linear_attn > 0) {
                                    atten = 1.0f / (light.constant_attn + 
                                                   light.linear_attn * dist + 
                                                   light.quadratic_attn * dist * dist);
                                } else {
                                    atten = 1.0f / (1.0f + dist * dist * 0.0001f);
                                }
                                sampleColor = sampleColor + lightColor * NdotL * atten * 100.0f;
                            }
                        } else if (light.type == emit_spotlight) {
                            // Static spotlight
                            Vector3 toLight = lightPos - worldPos;
                            float dist = vector3_length(toLight);
                            if (dist < 0.001f) continue;
                            
                            Vector3 lightDir = toLight / dist;
                            // Use phong normal for smoother lighting across edges
                            float NdotL = vector3_dot(sampleNormal, lightDir);
                            
                            if (NdotL > 0) {
                                float spotDot = vector3_dot(-lightDir, light.normal);
                                if (spotDot > light.stopdot2) {
                                    float spotAtten = 1.0f;
                                    if (spotDot < light.stopdot) {
                                        spotAtten = (spotDot - light.stopdot2) / (light.stopdot - light.stopdot2);
                                    }
                                    float distAtten = 1.0f / (1.0f + dist * dist * 0.0001f);
                                    sampleColor = sampleColor + lightColor * NdotL * spotAtten * distAtten * 100.0f;
                                }
                            }
                        }
                    }
                    
                    accumColor = accumColor + sampleColor;
                }
                
                // Average the supersamples
                Vector3 finalColor = accumColor * (1.0f / static_cast<float>(numSamples));
                
                // Store in luxel array
                surf.luxels[y * surf.rect.width + x] = finalColor;
                
                // Store direct light for radiosity if enabled
                if (RADIOSITY_BOUNCES > 0 && patchIndex < static_cast<int>(RadiosityData::patches.size())) {
                    RadiosityData::patches[patchIndex].directLight = finalColor;
                    RadiosityData::patches[patchIndex].totalLight = finalColor;
                    patchIndex++;
                }
                
                totalTexels++;
            }
        }
    }
    
    Sys_Printf("     %9d texels computed (direct)\n", totalTexels);
    
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
    Encode a floating-point RGB color to the 8-byte RGBE-style HDR format
    
    Based on analysis of official Apex Legends lightmaps:
    - Bytes 0-2: Direct light RGB mantissa (0-255, sRGB gamma)
    - Byte 3: Direct light exponent (0-255, typical range 0-60)
    - Bytes 4-6: Indirect/bounce light RGB mantissa (0-255, sRGB gamma)
    - Byte 7: Indirect light exponent (0-255, typical range 0-60)
    
    Format is RGBE-style where:
      finalColor = (RGB / 255) * 2^(exponent / 8)
    
    Examples from official maps:
    - exp=0: multiplier 1.0x (normal/shadow areas)
    - exp=8: multiplier 2.0x
    - exp=16: multiplier 4.0x
    - exp=50-60: very bright sunlit areas (80-150x)
    
    For simple maps without bounce lighting, direct and indirect are the same.
*/
static void EncodeHDRTexel(const Vector3 &color, uint8_t *out) {
    // Input is in linear HDR space (0.0 to potentially >1.0 for HDR)
    // Find the maximum component to determine exponent
    float maxComponent = std::max({color.x(), color.y(), color.z()});
    
    uint8_t exponent;
    float scale;
    
    if (maxComponent <= 0.001f) {
        // Near-black - use exp=0 with dark RGB
        exponent = 0;
        scale = 1.0f;
    } else if (maxComponent <= 1.0f) {
        // LDR range - exp=0 gives 1.0x multiplier
        exponent = 0;
        scale = 1.0f;
    } else {
        // HDR range - need positive exponent
        // exp = 8 * log2(maxComponent) rounded up
        int exp = (int)std::ceil(8.0f * std::log2(maxComponent));
        exponent = (uint8_t)std::min(255, std::max(0, exp));
        
        // Scale factor: divide by 2^(exp/8) to get mantissa in 0-1 range
        scale = 1.0f / std::pow(2.0f, exponent / 8.0f);
    }
    
    // Scale color and apply gamma correction (linear to sRGB)
    float r = std::pow(std::min(1.0f, std::max(0.0f, color.x() * scale)), 1.0f / 2.2f);
    float g = std::pow(std::min(1.0f, std::max(0.0f, color.y() * scale)), 1.0f / 2.2f);
    float b = std::pow(std::min(1.0f, std::max(0.0f, color.z() * scale)), 1.0f / 2.2f);
    
    // Direct light (bytes 0-3)
    out[0] = (uint8_t)(r * 255.0f);
    out[1] = (uint8_t)(g * 255.0f);
    out[2] = (uint8_t)(b * 255.0f);
    out[3] = exponent;
    
    // Indirect light (bytes 4-7) - same as direct for now
    out[4] = out[0];
    out[5] = out[1];
    out[6] = out[2];
    out[7] = exponent;
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

    Sys_Printf("--- EmitLightmaps ---\n");
    
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
        
        // Neutral stub lightmap - engine applies dynamic ambient/sun from worldLights
        // This allows _ambient/_light changes in .ent files to work immediately
        // Format: RGB (sRGB gamma) + exponent where finalColor = RGB * 2^(exp/8)
        // exp=0 means 2^0 = 1.0x multiplier (neutral)
        // RGB=180 gives a mid-gray tone in sRGB (linear ~0.45)
        uint8_t neutralGray = 180;  // Mid-gray mantissa in sRGB
        uint8_t exponent = 0;       // Neutral exposure (2^0 = 1.0x)
        size_t dataSize = 256 * 256 * 8;
        ApexLegends::Bsp::lightmapDataSky.resize(dataSize);
        for (size_t i = 0; i < dataSize; i += 8) {
            ApexLegends::Bsp::lightmapDataSky[i + 0] = neutralGray;  // R
            ApexLegends::Bsp::lightmapDataSky[i + 1] = neutralGray;  // G
            ApexLegends::Bsp::lightmapDataSky[i + 2] = neutralGray;  // B
            ApexLegends::Bsp::lightmapDataSky[i + 3] = exponent;     // Direct exp
            ApexLegends::Bsp::lightmapDataSky[i + 4] = neutralGray;  // R indirect
            ApexLegends::Bsp::lightmapDataSky[i + 5] = neutralGray;  // G indirect
            ApexLegends::Bsp::lightmapDataSky[i + 6] = neutralGray;  // B indirect
            ApexLegends::Bsp::lightmapDataSky[i + 7] = exponent;     // Indirect exp
        }
        
        return;
    }
    
    // Encode luxels to lightmap pages
    for (SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        ApexLegends::LightmapPage_t &page = ApexLegends::Bsp::lightmapPages[surf.rect.pageIndex];
        
        for (int y = 0; y < surf.rect.height; y++) {
            for (int x = 0; x < surf.rect.width; x++) {
                const Vector3 &color = surf.luxels[y * surf.rect.width + x];
                
                int px = surf.rect.x + x;
                int py = surf.rect.y + y;
                int offset = (py * page.width + px) * 8;
                
                EncodeHDRTexel(color, &page.pixels[offset]);
            }
        }
    }
    
    // Create headers and concatenate pixel data
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
        
        // Append pixel data
        ApexLegends::Bsp::lightmapDataSky.insert(
            ApexLegends::Bsp::lightmapDataSky.end(),
            page.pixels.begin(), page.pixels.end());
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

// Sky environment data extracted from worldlights
struct SkyEnvironment {
    Vector3 ambientColor;      // Base ambient color (from emit_skyambient)
    Vector3 sunDir;            // Sun direction (from emit_skylight)
    Vector3 sunColor;          // Sun color (normalized)
    float sunIntensity;        // Sun brightness multiplier
    bool valid;                // Whether sky data was found
};

// Parse "_light" key format: "R G B brightness" (e.g., "255 252 242 510")
// Returns normalized RGB color (0-1) and brightness multiplier
static bool ParseLightKey(const char *value, Vector3 &outColor, float &outBrightness) {
    if (!value || !value[0]) return false;
    
    float r, g, b, brightness;
    if (sscanf(value, "%f %f %f %f", &r, &g, &b, &brightness) == 4) {
        // Normalize RGB from 0-255 to 0-1
        outColor[0] = r;
        outColor[1] = g;
        outColor[2] = b;
        outBrightness = brightness;
        return true;
    }
    // Try 3-component format (no brightness)
    if (sscanf(value, "%f %f %f", &r, &g, &b) == 3) {
        outColor[0] = r;
        outColor[1] = g;
        outColor[2] = b;
        outBrightness = 1.0f;
        return true;
    }
    return false;
}

// Extract sky environment from light_environment entity and worldlights (called once)
static SkyEnvironment GetSkyEnvironment() {
    SkyEnvironment sky;
    sky.ambientColor = Vector3(0.65f, 0.55f, 0.45f);  // Default warm outdoor
    sky.sunDir = Vector3(0.5f, 0.5f, -0.707f);        // Default: sun from upper-right
    sky.sunColor = Vector3(1.0f, 0.95f, 0.85f);       // Default warm sunlight
    sky.sunIntensity = 1.5f;
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
                // Scale intensity - typical _light brightness ~200-1000
                // We want sunIntensity to be around 2-4 for good contrast
                sky.sunIntensity = brightness / 50.0f;
                sky.sunIntensity = std::min(5.0f, std::max(1.0f, sky.sunIntensity));
                foundSkyLight = true;
                Sys_Printf("     Found light_environment _light: %s\n", lightValue);
            }
            
            // Get ambient color from _ambient key
            const char *ambientValue = entity.valueForKey("_ambient");
            Vector3 ambientColor;
            float ambientBrightness;
            if (ParseLightKey(ambientValue, ambientColor, ambientBrightness)) {
                sky.ambientColor = ambientColor;
                foundSkyAmbient = true;
                Sys_Printf("     Found light_environment _ambient: %s\n", ambientValue);
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
                // Scale intensity - typical emit_skylight has intensity ~200-1000
                sky.sunIntensity = maxIntensity / 50.0f;
                sky.sunIntensity = std::min(5.0f, std::max(1.0f, sky.sunIntensity));
                foundSkyLight = true;
            }
        }
    }
    
    sky.valid = foundSkyAmbient || foundSkyLight;
    
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

// Trace a ray against all mesh geometry
// Returns true if something is hit within maxDist
static bool TraceRayAgainstMeshes(const Vector3 &origin, const Vector3 &dir, float maxDist) {
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
        
        // Trace ray in this direction
        if (!TraceRayAgainstMeshes(position, dir, LIGHT_PROBE_TRACE_DIST)) {
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
            // Ray hit geometry - use dark ambient for occluded directions
            // This simulates indirect lighting bounce (simplified)
            radcolor[i] = sky.ambientColor * 0.15f;
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
        
        // Check if light is blocked
        if (TraceRayAgainstMeshes(position, dirToLight, dist - 1.0f)) {
            continue;  // Light is occluded
        }
        
        // Distance falloff (inverse square)
        float falloff = 1.0f / (distSq + 1.0f);
        
        // Light intensity
        Vector3 lightColor(
            light.intensity[0] / 255.0f,
            light.intensity[1] / 255.0f,
            light.intensity[2] / 255.0f
        );
        
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
*/
static void ConvertCubeToSphericalHarmonics(const Vector3 lightBoxColor[6], 
                                             LightProbe_v50_t &probe) {
    // SH scale factor - based on analysis of official maps
    // DC values typically 2000-4000 for well-lit areas
    const float shScale = 8000.0f;
    
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
    bool hitPosX = TraceRayAgainstMeshes(pos, Vector3(1, 0, 0), testDist);
    bool hitNegX = TraceRayAgainstMeshes(pos, Vector3(-1, 0, 0), testDist);
    bool hitPosY = TraceRayAgainstMeshes(pos, Vector3(0, 1, 0), testDist);
    bool hitNegY = TraceRayAgainstMeshes(pos, Vector3(0, -1, 0), testDist);
    bool hitPosZ = TraceRayAgainstMeshes(pos, Vector3(0, 0, 1), testDist);
    bool hitNegZ = TraceRayAgainstMeshes(pos, Vector3(0, 0, -1), testDist);
    
    return hitPosX && hitNegX && hitPosY && hitNegY && hitPosZ && hitNegZ;
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
    
    Sys_Printf("     Collected %zu geometry samples\n", geometrySamples.size());
    
    if (geometrySamples.empty()) {
        // Fallback: use world center
        Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
        probePositions.push_back(center);
        Sys_Printf("     No geometry samples, using world center\n");
        return;
    }
    
    // =========================================================================
    // Step 2: Offset samples above surfaces for probe positions
    // =========================================================================
    std::vector<Vector3> candidatePositions;
    candidatePositions.reserve(geometrySamples.size());
    
    for (const Vector3 &sample : geometrySamples) {
        // Offset sample upward (probes should be in playable space, not embedded in geometry)
        Vector3 probePos = sample + Vector3(0, 0, 64.0f);
        
        // Check if this position is valid (not inside solid)
        if (!IsPositionInsideSolid(probePos, 48.0f)) {
            candidatePositions.push_back(probePos);
        }
    }
    
    Sys_Printf("     %zu valid candidate positions after solid rejection\n", candidatePositions.size());
    
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
    int targetProbes = std::max(8, std::min(2048, 
        (int)(worldVolume / (LIGHT_PROBE_GRID_SPACING * LIGHT_PROBE_GRID_SPACING * LIGHT_PROBE_GRID_SPACING))));
    
    // Increase target if we have dense geometry
    float densityFactor = std::min(4.0f, (float)candidatePositions.size() / 1000.0f);
    targetProbes = std::min(2048, (int)(targetProbes * (1.0f + densityFactor)));
    
    Sys_Printf("     Target probe count: %d (world avg dimension: %.0f)\n", targetProbes, avgDimension);
    
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
            Sys_Printf("       Seeded %zu / %d centroids...\n", centroids.size(), targetProbes);
        }
    }
    
    Sys_Printf("     Seeded %zu initial centroids, running Lloyd relaxation...\n", centroids.size());
    
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
            Sys_Printf("     Lloyd converged after %d iterations\n", iter + 1);
            break;
        }
    }
    
    // =========================================================================
    // Step 5: Filter final positions and enforce minimum spacing
    // =========================================================================
    std::vector<Vector3> finalPositions;
    finalPositions.reserve(centroids.size());
    
    for (const Vector3 &centroid : centroids) {
        // Skip if inside solid
        if (IsPositionInsideSolid(centroid, 32.0f)) continue;
        
        // Skip if too close to an existing probe
        bool tooClose = false;
        for (const Vector3 &existing : finalPositions) {
            Vector3 delta = centroid - existing;
            if (vector3_dot(delta, delta) < LIGHT_PROBE_MIN_SPACING * LIGHT_PROBE_MIN_SPACING) {
                tooClose = true;
                break;
            }
        }
        
        if (!tooClose) {
            finalPositions.push_back(centroid);
        }
    }
    
    probePositions = std::move(finalPositions);
    Sys_Printf("     Generated %zu Voronoi-based probe positions\n", probePositions.size());
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
    
    Sys_Printf("     Compressing %zu probes to %d...\n", candidates.size(), maxProbes);
    
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
    
    Sys_Printf("     Kept %zu probes after compression\n", candidates.size());
}

// Legacy function for logging (called once to print sky info)
static void LogSkyEnvironment(const SkyEnvironment &sky) {
    Sys_Printf("     Sun direction: (%.2f, %.2f, %.2f)\n", sky.sunDir[0], sky.sunDir[1], sky.sunDir[2]);
    Sys_Printf("     Sun intensity: %.2f, color: (%.2f, %.2f, %.2f)\n", sky.sunIntensity, sky.sunColor[0], sky.sunColor[1], sky.sunColor[2]);
    Sys_Printf("     Ambient color: (%.2f, %.2f, %.2f)\n", sky.ambientColor[0], sky.ambientColor[1], sky.ambientColor[2]);
}

void ApexLegends::EmitLightProbes() {
    Sys_Printf("--- EmitLightProbes ---\n");
    
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
    
    if (!probePositions.empty()) {
        Sys_Printf("     Found %zu info_lightprobe entities\n", probePositions.size());
    } else {
        // No manual probes - generate Voronoi-based adaptive placement
        Sys_Printf("     No info_lightprobe entities, generating Voronoi-based placement...\n");
        GenerateProbePositionsVoronoi(worldBounds, probePositions);
    }
    
    // Ensure we have at least one probe
    if (probePositions.empty()) {
        Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
        probePositions.push_back(center);
        Sys_Printf("     Using single probe at world center\n");
    }
    
    // Create base probe template
    LightProbe_v50_t baseProbe;
    memset(&baseProbe, 0, sizeof(baseProbe));
    baseProbe.staticLightIndexes[0] = 0xFFFF;
    baseProbe.staticLightIndexes[1] = 0xFFFF;
    baseProbe.staticLightIndexes[2] = 0xFFFF;
    baseProbe.staticLightIndexes[3] = 0xFFFF;
    
    // Compute per-probe lighting using spherical sampling
    Sys_Printf("     Computing probe lighting using 162-direction spherical sampling...\n");
    
    std::vector<ProbeCandidate> candidates;
    candidates.reserve(probePositions.size());
    
    for (size_t i = 0; i < probePositions.size(); i++) {
        const Vector3 &pos = probePositions[i];
        
        ProbeCandidate candidate;
        candidate.pos = pos;
        candidate.keep = true;
        
        // Compute ambient using Source SDK style spherical sampling
        // This samples 162 directions and accumulates into 6-sided cube
        ComputeAmbientFromSphericalSamples(pos, sky, candidate.cube);
        
        candidates.push_back(candidate);
        
        // Progress indicator for large probe counts
        if ((i + 1) % 100 == 0 || i + 1 == probePositions.size()) {
            Sys_Printf("       Computed %zu / %zu probes...\n", i + 1, probePositions.size());
        }
    }
    
    Sys_Printf("     Finished computing %zu probe(s)\n", probePositions.size());
    
    // Compress probe list if we have too many (Source SDK style optimization)
    // Remove redundant probes that can be reconstructed from neighbors
    CompressProbeList(candidates, 2048);
    
    // Convert candidates to final probe data
    for (const ProbeCandidate &candidate : candidates) {
        LightProbe_v50_t probe = baseProbe;
        
        // Convert 6-sided cube to spherical harmonics
        ConvertCubeToSphericalHarmonics(candidate.cube, probe);
        
        ApexLegends::Bsp::lightprobes.push_back(probe);
        
        // Create probe reference
        LightProbeRef_t ref;
        ref.origin = candidate.pos;
        ref.lightProbeIndex = static_cast<uint32_t>(ApexLegends::Bsp::lightprobes.size() - 1);
        ref.cubemapID = -1;
        ref.padding = 0;
        ApexLegends::Bsp::lightprobeReferences.push_back(ref);
    }
    
    // Build spatial lookup tree
    // For simplicity, create a single leaf node containing all probes
    // This works correctly - engine iterates through all refs
    // A proper BSP tree would be faster for large counts but this is valid
    LightProbeTree_t leaf;
    leaf.tag = (0 << 2) | 3;  // refStart=0, type=3 (leaf)
    leaf.refCount = static_cast<uint32_t>(ApexLegends::Bsp::lightprobeReferences.size());
    ApexLegends::Bsp::lightprobeTree.push_back(leaf);
    
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