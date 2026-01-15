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
    Encode a floating-point RGB color to the 8-byte HDR format
    
    Based on reverse engineering of Apex engine's lightmap format.
    The 8-byte format appears to be:
    - Bytes 0-2: Base RGB color (gamma corrected, sRGB)
    - Byte 3: Alpha (always 255)
    - Bytes 4-6: HDR/indirect RGB (can differ from base for bounce lighting)
    - Byte 7: HDR exponent/multiplier (128 = 1.0x, higher = brighter)
    
    Input color is in linear HDR space from ComputeLightmapLighting()
    which already includes ambient from light_environment.
*/
static void EncodeHDRTexel(const Vector3 &color, uint8_t *out) {
    // Input color already has ambient baked in from ComputeLightmapLighting
    // Just need to tonemap and encode
    
    // Find the maximum component to determine if we need HDR scaling
    float maxComponent = std::max({color.x(), color.y(), color.z(), 0.001f});
    
    // HDR exponent: if color exceeds 1.0, we need to scale up the multiplier
    // Multiplier of 128 = 1.0x, 255 = ~2.0x, 64 = 0.5x
    float hdrScale = 1.0f;
    uint8_t hdrExponent = 128;  // Default 1.0x
    
    if (maxComponent > 1.0f) {
        // Need HDR - scale color down and boost exponent
        hdrScale = 1.0f / maxComponent;
        // Map maxComponent to exponent: 128 + log2(maxComponent) * 32
        // This gives us ~4 stops of HDR range
        float logScale = std::log2(maxComponent);
        hdrExponent = (uint8_t)std::min(255.0f, 128.0f + logScale * 32.0f);
    }
    
    // Apply HDR scaling and clamp to [0, 1]
    float r = std::min(1.0f, std::max(0.0f, color.x() * hdrScale));
    float g = std::min(1.0f, std::max(0.0f, color.y() * hdrScale));
    float b = std::min(1.0f, std::max(0.0f, color.z() * hdrScale));
    
    // Apply gamma correction (linear to sRGB)
    r = std::pow(r, 1.0f / 2.2f);
    g = std::pow(g, 1.0f / 2.2f);
    b = std::pow(b, 1.0f / 2.2f);
    
    // Scale to 0-255
    out[0] = (uint8_t)(r * 255.0f);
    out[1] = (uint8_t)(g * 255.0f);
    out[2] = (uint8_t)(b * 255.0f);
    out[3] = 255;  // Alpha
    
    // HDR/indirect lighting (same as direct for now, could add bounce)
    out[4] = out[0];
    out[5] = out[1];
    out[6] = out[2];
    out[7] = hdrExponent;  // HDR multiplier
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
        uint8_t neutralValue = 128;  // Neutral gray
        size_t dataSize = 256 * 256 * 8;
        ApexLegends::Bsp::lightmapDataSky.resize(dataSize);
        for (size_t i = 0; i < dataSize; i += 8) {
            ApexLegends::Bsp::lightmapDataSky[i + 0] = neutralValue;
            ApexLegends::Bsp::lightmapDataSky[i + 1] = neutralValue;
            ApexLegends::Bsp::lightmapDataSky[i + 2] = neutralValue;
            ApexLegends::Bsp::lightmapDataSky[i + 3] = 255;  // A
            ApexLegends::Bsp::lightmapDataSky[i + 4] = neutralValue;
            ApexLegends::Bsp::lightmapDataSky[i + 5] = neutralValue;
            ApexLegends::Bsp::lightmapDataSky[i + 6] = neutralValue;
            ApexLegends::Bsp::lightmapDataSky[i + 7] = 128;  // Neutral multiplier
        }
        
        // RTL page lump must be empty (0 bytes) or a multiple of 126 bytes
        // Leave it empty since we have no RTL data
        ApexLegends::Bsp::lightmapDataRTLPage.clear();
        
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
    
    // RTL page lump contains 63 uint16 indices per page (126 bytes per page)
    // The lump size must be a multiple of 126 bytes, or empty
    // For now, leave empty since we don't generate RTL data
    ApexLegends::Bsp::lightmapDataRTLPage.clear();
    
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