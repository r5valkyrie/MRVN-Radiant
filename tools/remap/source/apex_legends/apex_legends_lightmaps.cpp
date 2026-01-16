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
    std::vector<Vector3> luxels; // Per-texel lighting values (RGB HDR)
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
        
        // Calculate lightmap size based on surface area
        float uExtent = 0, vExtent = 0;
        for (const Shared::Vertex_t &vert : mesh.vertices) {
            Vector3 localPos = vert.xyz - bounds.mins;
            float u = vector3_dot(localPos, tangent);
            float v = vector3_dot(localPos, bitangent);
            uExtent = std::max(uExtent, std::abs(u));
            vExtent = std::max(vExtent, std::abs(v));
        }
        
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
        surfLM.luxels.resize(rect.width * rect.height, Vector3(0, 0, 0));
        
        LightmapBuild::surfaces.push_back(surfLM);
        litSurfaces++;
        meshIndex++;
    }
    
    Sys_Printf("     %9d lit surfaces\n", litSurfaces);
    Sys_Printf("     %9d lightmap pages\n", (int)ApexLegends::Bsp::lightmapPages.size());
}


/*
    ComputeLightmapLighting
    Compute lighting for each texel.
    
    IMPORTANT: emit_skyambient and emit_skylight are applied DYNAMICALLY by the engine
    from the worldLights lump (0x36). Changing _ambient/_light in the .ent file updates
    the map in real-time because the engine reads these values at runtime.
    
    Lightmaps should only contain:
    - Indirect/bounced lighting (requires full radiosity, not implemented)
    - Static point/spot light contributions that aren't realtime
    
    For now, we provide a neutral base that the engine's dynamic lighting adds to.
*/
void ApexLegends::ComputeLightmapLighting() {
    Sys_Printf("--- ComputeLightmapLighting ---\n");
    
    // NOTE: We do NOT bake emit_skyambient or emit_skylight here!
    // The engine applies these dynamically from worldLights lump.
    // This is why changing _ambient/_light in .ent files works on official maps.
    
    if (ApexLegends::Bsp::worldLights.empty()) {
        Sys_Printf("  No worldlights found\n");
    }
    
    int totalTexels = 0;
    
    for (SurfaceLightmap_t &surf : LightmapBuild::surfaces) {
        // For each texel
        for (int y = 0; y < surf.rect.height; y++) {
            for (int x = 0; x < surf.rect.width; x++) {
                // Compute world position for this texel
                float localU = (x + 0.5f) * LIGHTMAP_SAMPLE_SIZE;
                float localV = (y + 0.5f) * LIGHTMAP_SAMPLE_SIZE;
                
                Vector3 worldPos = surf.worldBounds.mins 
                    + surf.tangent * localU 
                    + surf.bitangent * localV;
                
                // Offset slightly along normal to avoid self-intersection
                worldPos = worldPos + surf.plane.normal() * 0.1f;
                
                // Start with neutral base - engine adds dynamic ambient/sun on top
                // A small base value prevents completely black areas
                Vector3 color(0.1f, 0.1f, 0.1f);
                
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
                        float NdotL = vector3_dot(surf.plane.normal(), lightDir);
                        
                        if (NdotL > 0) {
                            float atten = 1.0f;
                            if (light.quadratic_attn > 0 || light.linear_attn > 0) {
                                atten = 1.0f / (light.constant_attn + 
                                               light.linear_attn * dist + 
                                               light.quadratic_attn * dist * dist);
                            } else {
                                atten = 1.0f / (1.0f + dist * dist * 0.0001f);
                            }
                            color = color + lightColor * NdotL * atten * 100.0f;
                        }
                    } else if (light.type == emit_spotlight) {
                        // Static spotlight
                        Vector3 toLight = lightPos - worldPos;
                        float dist = vector3_length(toLight);
                        if (dist < 0.001f) continue;
                        
                        Vector3 lightDir = toLight / dist;
                        float NdotL = vector3_dot(surf.plane.normal(), lightDir);
                        
                        if (NdotL > 0) {
                            float spotDot = vector3_dot(-lightDir, light.normal);
                            if (spotDot > light.stopdot2) {
                                float spotAtten = 1.0f;
                                if (spotDot < light.stopdot) {
                                    spotAtten = (spotDot - light.stopdot2) / (light.stopdot - light.stopdot2);
                                }
                                float distAtten = 1.0f / (1.0f + dist * dist * 0.0001f);
                                color = color + lightColor * NdotL * spotAtten * distAtten * 100.0f;
                            }
                        }
                    }
                }
                
                // Store in luxel array
                surf.luxels[y * surf.rect.width + x] = color;
                totalTexels++;
            }
        }
    }
    
    Sys_Printf("     %9d texels computed\n", totalTexels);
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
        
        // RTL data will be generated by EmitRealTimeLightmaps()
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
    
    // RTL data will be generated by EmitRealTimeLightmaps()
    
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
            
            // Convert to texel coordinates within the allocated rect
            float texelU = localU / LIGHTMAP_SAMPLE_SIZE;
            float texelV = localV / LIGHTMAP_SAMPLE_SIZE;
            
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

// Test if a position can see the sky (is outdoors)
// Returns: 0.0 = fully indoors, 1.0 = fully outdoors, in between = partial
static float TestSkyVisibility(const Vector3 &origin) {
    // Cast rays upward in a cone to test for sky visibility
    const int NUM_SKY_RAYS = 8;
    const float SKY_TRACE_DIST = 8192.0f;  // How far to trace upward
    
    int skyHits = 0;
    
    // Directions: up, and slightly tilted in various directions
    static const Vector3 skyDirs[] = {
        vector3_normalised(Vector3(0, 0, 1)),
        vector3_normalised(Vector3(0.3f, 0, 0.95f)),
        vector3_normalised(Vector3(-0.3f, 0, 0.95f)),
        vector3_normalised(Vector3(0, 0.3f, 0.95f)),
        vector3_normalised(Vector3(0, -0.3f, 0.95f)),
        vector3_normalised(Vector3(0.2f, 0.2f, 0.96f)),
        vector3_normalised(Vector3(-0.2f, 0.2f, 0.96f)),
        vector3_normalised(Vector3(0.2f, -0.2f, 0.96f)),
    };
    
    for (int i = 0; i < NUM_SKY_RAYS; i++) {
        // If ray is NOT blocked, we can see the sky
        if (!TraceRayAgainstMeshes(origin, skyDirs[i], SKY_TRACE_DIST)) {
            skyHits++;
        }
    }
    
    return static_cast<float>(skyHits) / static_cast<float>(NUM_SKY_RAYS);
}

// Test if a position is in shadow from the sun
// Returns: 0.0 = fully in shadow, 1.0 = fully lit by sun
static float TestSunVisibility(const Vector3 &origin, const Vector3 &sunDir) {
    const float SUN_TRACE_DIST = 16384.0f;  // Sun is very far away
    
    // Trace toward the sun (opposite of sun direction since sunDir points from sun TO world)
    Vector3 toSun = vector3_normalised(sunDir * -1.0f);
    
    // If ray is NOT blocked, we can see the sun
    if (!TraceRayAgainstMeshes(origin, toSun, SUN_TRACE_DIST)) {
        return 1.0f;
    }
    
    return 0.0f;
}

// Helper to clamp float to int16 range before casting
static int16_t ClampToInt16(float value) {
    return static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
}

// Compute SH coefficients for a single probe based on position and visibility
static void ComputeProbeSH(LightProbe_v50_t &probe, const Vector3 &position, 
                           const SkyEnvironment &sky, float skyVis, float sunVis) {
    // Based on official map analysis (bright outdoor probes):
    // - DC values: 2000-4000 for sunlit outdoor areas
    // - Directional values: 1500-2500 (about 50-70% of DC)
    // - With ambient color ~0.5-0.65, shScale should be ~4000-6000
    const float shScale = 10000.0f;
    
    // Directional scale - controls shadow intensity on entities
    // Higher = more contrast between sun-facing and shadow sides
    // Official maps show directional ~50-70% of DC magnitude
    const float dirMultiplier = 0.6f;
    
    // Base ambient varies by sky vi`sibility:
    // - Outdoors (skyVis=1): full ambient color
    // - Indoors (skyVis=0): reduced ambient (darker, cooler tint for indoor shadow)
    const float indoorAmbientScale = 0.35f;  // Indoor areas get 35% of outdoor ambient
    const float ambientScale = indoorAmbientScale + (1.0f - indoorAmbientScale) * skyVis;
    
    // Indoor areas have cooler tint (less red, slightly more blue)
    Vector3 effectiveAmbient;
    effectiveAmbient[0] = sky.ambientColor[0] * ambientScale * (0.8f + 0.2f * skyVis);  // Less red indoors
    effectiveAmbient[1] = sky.ambientColor[1] * ambientScale;
    effectiveAmbient[2] = sky.ambientColor[2] * ambientScale * (1.0f + 0.1f * (1.0f - skyVis));  // Slightly more blue indoors
    
    // Directional SH (sun shading) only applies if sun is visible
    // If sun is blocked, directional components are zero (flat ambient lighting)
    // This creates the shadow effect on entities - surfaces facing away from sun are darker
    const float dirScale = shScale * dirMultiplier * sunVis;
    
    // Directional coefficients from sun direction
    float dirX = -sky.sunDir[0];
    float dirY = -sky.sunDir[1];
    float dirZ = -sky.sunDir[2];
    
    // Red channel - clamp all values to int16 range
    probe.ambientSH[0][0] = ClampToInt16(dirX * sky.sunColor[0] * dirScale);  // R X
    probe.ambientSH[0][1] = ClampToInt16(dirY * sky.sunColor[0] * dirScale);  // R Y
    probe.ambientSH[0][2] = ClampToInt16(dirZ * sky.sunColor[0] * dirScale);  // R Z
    probe.ambientSH[0][3] = ClampToInt16(effectiveAmbient[0] * shScale);       // R DC
    
    // Green channel
    probe.ambientSH[1][0] = ClampToInt16(dirX * sky.sunColor[1] * dirScale);  // G X
    probe.ambientSH[1][1] = ClampToInt16(dirY * sky.sunColor[1] * dirScale);  // G Y
    probe.ambientSH[1][2] = ClampToInt16(dirZ * sky.sunColor[1] * dirScale);  // G Z
    probe.ambientSH[1][3] = ClampToInt16(effectiveAmbient[1] * shScale);       // G DC
    
    // Blue channel
    probe.ambientSH[2][0] = ClampToInt16(dirX * sky.sunColor[2] * dirScale);  // B X
    probe.ambientSH[2][1] = ClampToInt16(dirY * sky.sunColor[2] * dirScale);  // B Y
    probe.ambientSH[2][2] = ClampToInt16(dirZ * sky.sunColor[2] * dirScale);  // B Z
    probe.ambientSH[2][3] = ClampToInt16(effectiveAmbient[2] * shScale);       // B DC
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
    
    // Collect probe positions from info_lightprobe entities
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
    
    // If no info_lightprobe entities, create a single probe at world center
    if (probePositions.empty()) {
        Sys_Printf("     No info_lightprobe entities found, creating single probe at world center\n");
        
        // Calculate world bounds from all meshes
        MinMax worldBounds;
        for (const Shared::Mesh_t &mesh : Shared::meshes) {
            worldBounds.extend(mesh.minmax.mins);
            worldBounds.extend(mesh.minmax.maxs);
        }
        
        // If no geometry, create a minimal bounds
        if (!worldBounds.valid()) {
            worldBounds.extend(Vector3(-1024, -1024, -512));
            worldBounds.extend(Vector3(1024, 1024, 512));
        }
        
        // Single probe at center
        Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
        probePositions.push_back(center);
    } else {
        Sys_Printf("     Found %zu info_lightprobe entities\n", probePositions.size());
    }
    
    // Create base probe with SH data from sky
    LightProbe_v50_t baseProbe;
    memset(&baseProbe, 0, sizeof(baseProbe));
    baseProbe.staticLightIndexes[0] = 0xFFFF;
    baseProbe.staticLightIndexes[1] = 0xFFFF;
    baseProbe.staticLightIndexes[2] = 0xFFFF;
    baseProbe.staticLightIndexes[3] = 0xFFFF;
    
    // Get sky environment (shared for all probes)
    SkyEnvironment sky = GetSkyEnvironment();
    LogSkyEnvironment(sky);
    
    // Compute per-probe SH based on visibility
    Sys_Printf("     Computing probe lighting (%zu probes)...\n", probePositions.size());
    
    for (size_t i = 0; i < probePositions.size(); i++) {
        const Vector3 &pos = probePositions[i];
        
        // TODO: Enable visibility testing once trace nodes are set up
        // For now, use fixed values (outdoor, sunlit)
        float skyVis = TestSkyVisibility(pos);
        float sunVis = TestSunVisibility(pos, sky.sunDir);
        //float skyVis = 1.0f;  // Assume outdoor
        //float sunVis = 1.0f;  // Assume sunlit
        
        // Create probe with appropriate SH
        LightProbe_v50_t probe = baseProbe;
        ComputeProbeSH(probe, pos, sky, skyVis, sunVis);
        
        ApexLegends::Bsp::lightprobes.push_back(probe);
    }
    
    // Create probe references (position + probe index)
    for (size_t i = 0; i < probePositions.size(); i++) {
        LightProbeRef_t ref;
        ref.origin = probePositions[i];
        ref.lightProbeIndex = static_cast<uint32_t>(i);
        ref.cubemapID = -1;  // INVALID_CUBEMAP_ID
        ref.padding = 0;
        ApexLegends::Bsp::lightprobeReferences.push_back(ref);
    }
    
    // For simplicity, create a single leaf node that contains all probes
    // This is valid and works - the engine will iterate through all refs
    // A proper tree would be faster for large probe counts but single leaf works
    LightProbeTree_t leaf;
    leaf.tag = (0 << 2) | 3;  // refStart=0, type=3 (leaf)
    leaf.refCount = static_cast<uint32_t>(probePositions.size());
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

void ApexLegends::EmitRealTimeLightmaps() {
    Sys_Printf("--- EmitRealTimeLightmaps ---\n");
    
    ApexLegends::Bsp::lightmapDataRealTimeLights.clear();
    ApexLegends::Bsp::lightmapDataRTLPage.clear();
    
    // Collect all RTL worldlights
    // Based on analysis of official maps, ALL point/spot lights (types 1 and 2)
    // are included in RTL pages - the WORLDLIGHT_FLAG_REALTIME flag is NOT used
    // to determine RTL inclusion. Sky lights (types 3, 5) are excluded.
    std::vector<uint16_t> rtlLightIndices;
    for (size_t i = 0; i < ApexLegends::Bsp::worldLights.size(); i++) {
        const WorldLight_t &light = ApexLegends::Bsp::worldLights[i];
        // Include point lights (type 1) and spotlights (type 2)
        // Exclude skylight (type 3), surface (type 4), skyambient (type 5)
        if (light.type == 1 || light.type == 2) {  // emit_point or emit_spotlight
            rtlLightIndices.push_back(static_cast<uint16_t>(i));
        }
    }
    
    Sys_Printf("     %9zu RTL worldlights found\n", rtlLightIndices.size());
    
    // If no RTL lights, emit empty lumps (valid - engine handles this)
    if (rtlLightIndices.empty() || ApexLegends::Bsp::lightmapHeaders.empty()) {
        Sys_Printf("     RTL data: empty (no RTL lights or no lightmaps)\n");
        return;
    }
    
    // Build RTL pages (63 lights per page)
    std::vector<RTLPage_t> rtlPages;
    {
        RTLPage_t currentPage = {};
        currentPage.count = 0;
        
        for (size_t i = 0; i < rtlLightIndices.size(); i++) {
            if (currentPage.count >= 63) {
                rtlPages.push_back(currentPage);
                currentPage = {};
                currentPage.count = 0;
            }
            currentPage.lightIndices[currentPage.count++] = rtlLightIndices[i];
        }
        
        // Push the last page if it has any lights
        if (currentPage.count > 0) {
            // Fill remaining slots with 0xFFFF (invalid)
            for (int j = currentPage.count; j < 63; j++) {
                currentPage.lightIndices[j] = 0xFFFF;
            }
            rtlPages.push_back(currentPage);
        }
    }
    
    Sys_Printf("     %9zu RTL pages created\n", rtlPages.size());
    
    // Serialize RTL pages to lump 0x7A (126 bytes per page = 63 uint16)
    ApexLegends::Bsp::lightmapDataRTLPage.resize(rtlPages.size() * 126);
    for (size_t p = 0; p < rtlPages.size(); p++) {
        uint8_t *pageData = &ApexLegends::Bsp::lightmapDataRTLPage[p * 126];
        for (int i = 0; i < 63; i++) {
            uint16_t idx = rtlPages[p].lightIndices[i];
            pageData[i * 2 + 0] = idx & 0xFF;
            pageData[i * 2 + 1] = (idx >> 8) & 0xFF;
        }
    }
    
    // Generate per-texel RTL data for each lightmap
    // For each lightmap, calculate which RTL lights can affect each texel
    size_t totalTexels = 0;
    size_t affectedTexels = 0;
    
    for (const LightmapHeader_t &header : ApexLegends::Bsp::lightmapHeaders) {
        int width = header.width;
        int height = header.height;
        int texelCount = width * height;
        int alignedWidth = (width + 3) & ~3;
        int alignedHeight = (height + 3) & ~3;
        int alignedSize = alignedWidth * alignedHeight;
        
        // Size formula from engine (Mod_LoadLightmapDataRealTimeLights):
        // For BSP-embedded lumps (non-RPak): 2*(alignedSize + 2*texelCount) + (alignedSize >> 1)
        // External RPak lumps use just: 2*(alignedSize + 2*texelCount)
        // We're writing BSP file, so use non-RPak formula
        int rpakSize = 2 * (alignedSize + 2 * texelCount);
        int rtlDataSize = rpakSize + (alignedSize >> 1);  // Non-RPak format
        
        size_t startOffset = ApexLegends::Bsp::lightmapDataRealTimeLights.size();
        ApexLegends::Bsp::lightmapDataRealTimeLights.resize(startOffset + rtlDataSize);
        uint8_t *rtlData = &ApexLegends::Bsp::lightmapDataRealTimeLights[startOffset];
        
        // Initialize entire buffer to zero first
        memset(rtlData, 0, rtlDataSize);
        
        // Per-texel data: 4 bytes each, format is FF FF FF 00 for "no RTL lights"
        for (int t = 0; t < texelCount; t++) {
            int offset = t * 4;
            rtlData[offset + 0] = 0xFF;  // mask byte 0
            rtlData[offset + 1] = 0xFF;  // mask byte 1
            rtlData[offset + 2] = 0xFF;  // mask byte 2
            rtlData[offset + 3] = 0x00;  // page index (0 = no page)
        }
        
        // The remaining bytes (2*alignedSize + alignedSize/2) are BC-compressed mask data
        // Already zeroed by memset above - zero means no RTL masks
        
        totalTexels += texelCount;
        
        // TODO: For full implementation, we would:
        // 1. For each texel, compute world position from lightmap UV
        // 2. For each RTL light, check if it can reach this texel (visibility + range)
        // 3. Build a bitmask of which lights in the page affect the texel
        // 4. Encode: byte 0 = offset, bytes 1-2 = mask bits, byte 3 = page index
        //
        // For now, we emit "no RTL lights affect any texel" which is valid
        // and allows the map to load - RTL lights will be evaluated uniformly
    }
    
    Sys_Printf("     %9zu bytes RTL data (%zu texels)\n", 
               ApexLegends::Bsp::lightmapDataRealTimeLights.size(), totalTexels);
    Sys_Printf("     %9zu bytes RTL pages (%zu pages)\n",
               ApexLegends::Bsp::lightmapDataRTLPage.size(), rtlPages.size());
}