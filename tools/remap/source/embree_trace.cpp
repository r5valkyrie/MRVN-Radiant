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
    Embree Ray Tracing Implementation
    
    Uses Intel Embree 4 for hardware-accelerated BVH ray tracing.
    Falls back to disabled state if Embree is not available at compile time.
*/

#include "embree_trace.h"
#include "remap.h"
#include "bspfile_shared.h"

#ifdef USE_EMBREE
#include <embree4/rtcore.h>
#include <chrono>
#endif

namespace EmbreeTrace {

// =============================================================================
// Internal State
// =============================================================================

#ifdef USE_EMBREE

static RTCDevice g_device = nullptr;
static RTCScene g_scene = nullptr;
static bool g_sceneReady = false;
static SceneStats g_stats = {};

// Map from Embree geometry ID to mesh index
static std::vector<int> g_geomToMesh;

// Error callback for Embree
static void EmbreeErrorCallback(void* userPtr, RTCError code, const char* str) {
    const char* errorType = "Unknown";
    switch (code) {
        case RTC_ERROR_NONE: return;
        case RTC_ERROR_UNKNOWN: errorType = "Unknown"; break;
        case RTC_ERROR_INVALID_ARGUMENT: errorType = "Invalid argument"; break;
        case RTC_ERROR_INVALID_OPERATION: errorType = "Invalid operation"; break;
        case RTC_ERROR_OUT_OF_MEMORY: errorType = "Out of memory"; break;
        case RTC_ERROR_UNSUPPORTED_CPU: errorType = "Unsupported CPU"; break;
        case RTC_ERROR_CANCELLED: errorType = "Cancelled"; break;
        default: break;
    }
    Sys_Warning("Embree error (%s): %s\n", errorType, str ? str : "No message");
}

#endif // USE_EMBREE


// =============================================================================
// Public API Implementation
// =============================================================================

bool Init() {
#ifdef USE_EMBREE
    if (g_device != nullptr) {
        return true;  // Already initialized
    }
    
    Sys_Printf("Initializing Embree ray tracing...\n");
    
    // Create Embree device with default configuration
    // Use all available threads for BVH building
    g_device = rtcNewDevice("threads=0");
    
    if (g_device == nullptr) {
        RTCError error = rtcGetDeviceError(nullptr);
        Sys_Warning("Failed to create Embree device (error %d)\n", error);
        return false;
    }
    
    // Set error callback
    rtcSetDeviceErrorFunction(g_device, EmbreeErrorCallback, nullptr);
    
    Sys_Printf("  Embree device created successfully\n");
    return true;
    
#else
    // Embree not available at compile time
    return false;
#endif
}


void Shutdown() {
#ifdef USE_EMBREE
    ClearScene();
    
    if (g_device != nullptr) {
        rtcReleaseDevice(g_device);
        g_device = nullptr;
        Sys_Printf("Embree device released\n");
    }
#endif
}


void ClearScene() {
#ifdef USE_EMBREE
    if (g_scene != nullptr) {
        rtcReleaseScene(g_scene);
        g_scene = nullptr;
    }
    g_sceneReady = false;
    g_geomToMesh.clear();
    g_stats = {};
#endif
}


void BuildScene(bool skipSkyMeshes) {
#ifdef USE_EMBREE
    if (g_device == nullptr) {
        Sys_Warning("Embree device not initialized, cannot build scene\n");
        return;
    }
    
    // Clear any existing scene
    ClearScene();
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    Sys_Printf("Building Embree BVH scene...\n");
    
    // Create new scene with default flags
    // RTC_BUILD_QUALITY_HIGH gives best ray tracing performance at cost of build time
    g_scene = rtcNewScene(g_device);
    rtcSetSceneBuildQuality(g_scene, RTC_BUILD_QUALITY_HIGH);
    rtcSetSceneFlags(g_scene, RTC_SCENE_FLAG_ROBUST);
    
    g_stats.numMeshes = 0;
    g_stats.numTriangles = 0;
    g_stats.numVertices = 0;
    
    // Add each mesh as a geometry
    for (size_t meshIdx = 0; meshIdx < Shared::meshes.size(); meshIdx++) {
        const Shared::Mesh_t &mesh = Shared::meshes[meshIdx];
        
        // Skip sky meshes for shadow rays
        if (skipSkyMeshes && mesh.shaderInfo && 
            (mesh.shaderInfo->compileFlags & C_SKY)) {
            continue;
        }
        
        // Skip meshes with no triangles
        if (mesh.triangles.size() < 3 || mesh.vertices.empty()) {
            continue;
        }
        
        size_t numTris = mesh.triangles.size() / 3;
        size_t numVerts = mesh.vertices.size();
        
        // Create triangle mesh geometry
        RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);
        
        // Set vertex buffer
        float* vertices = (float*)rtcSetNewGeometryBuffer(
            geom, RTC_BUFFER_TYPE_VERTEX, 0,
            RTC_FORMAT_FLOAT3, sizeof(float) * 3, numVerts);
        
        for (size_t v = 0; v < numVerts; v++) {
            vertices[v * 3 + 0] = mesh.vertices[v].xyz.x();
            vertices[v * 3 + 1] = mesh.vertices[v].xyz.y();
            vertices[v * 3 + 2] = mesh.vertices[v].xyz.z();
        }
        
        // Set index buffer
        // Embree wants 32-bit indices, mesh uses 16-bit
        unsigned* indices = (unsigned*)rtcSetNewGeometryBuffer(
            geom, RTC_BUFFER_TYPE_INDEX, 0,
            RTC_FORMAT_UINT3, sizeof(unsigned) * 3, numTris);
        
        for (size_t t = 0; t < numTris; t++) {
            indices[t * 3 + 0] = mesh.triangles[t * 3 + 0];
            indices[t * 3 + 1] = mesh.triangles[t * 3 + 1];
            indices[t * 3 + 2] = mesh.triangles[t * 3 + 2];
        }
        
        // Commit geometry and attach to scene
        rtcCommitGeometry(geom);
        unsigned geomID = rtcAttachGeometry(g_scene, geom);
        rtcReleaseGeometry(geom);  // Scene now owns it
        
        // Track mapping from geometry ID to mesh index
        if (geomID >= g_geomToMesh.size()) {
            g_geomToMesh.resize(geomID + 1, -1);
        }
        g_geomToMesh[geomID] = static_cast<int>(meshIdx);
        
        g_stats.numMeshes++;
        g_stats.numTriangles += numTris;
        g_stats.numVertices += numVerts;
    }
    
    // Build the BVH
    rtcCommitScene(g_scene);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    g_stats.buildTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    g_sceneReady = true;
    
    Sys_Printf("  %zu meshes, %zu triangles, %zu vertices\n", 
               g_stats.numMeshes, g_stats.numTriangles, g_stats.numVertices);
    Sys_Printf("  BVH built in %.2f ms\n", g_stats.buildTimeMs);
    
#else
    Sys_Warning("Embree not available, using fallback ray tracing\n");
#endif
}


bool TestVisibility(const Vector3 &origin, const Vector3 &dir, float maxDist) {
#ifdef USE_EMBREE
    if (!g_sceneReady || g_scene == nullptr) {
        return false;  // No scene, assume not blocked
    }
    
    // Create occluded ray (shadow ray)
    RTCRay ray;
    ray.org_x = origin.x();
    ray.org_y = origin.y();
    ray.org_z = origin.z();
    ray.dir_x = dir.x();
    ray.dir_y = dir.y();
    ray.dir_z = dir.z();
    ray.tnear = 0.1f;  // Small offset to avoid self-intersection
    ray.tfar = maxDist;
    ray.mask = 0xFFFFFFFF;
    ray.flags = 0;
    
    // Use occlusion test (faster than full intersection for shadow rays)
    rtcOccluded1(g_scene, &ray);
    
    // If tfar becomes negative, ray was occluded
    return (ray.tfar < 0.0f);
    
#else
    return false;
#endif
}


bool TraceRay(const Vector3 &origin, const Vector3 &dir, float maxDist,
              float &outHitDist, Vector3 &outHitNormal, int &outMeshIndex) {
#ifdef USE_EMBREE
    if (!g_sceneReady || g_scene == nullptr) {
        return false;
    }
    
    // Create ray+hit structure
    RTCRayHit rayhit;
    rayhit.ray.org_x = origin.x();
    rayhit.ray.org_y = origin.y();
    rayhit.ray.org_z = origin.z();
    rayhit.ray.dir_x = dir.x();
    rayhit.ray.dir_y = dir.y();
    rayhit.ray.dir_z = dir.z();
    rayhit.ray.tnear = 0.1f;
    rayhit.ray.tfar = maxDist;
    rayhit.ray.mask = 0xFFFFFFFF;
    rayhit.ray.flags = 0;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;
    
    // Trace ray
    rtcIntersect1(g_scene, &rayhit);
    
    // Check for hit
    if (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }
    
    // Extract hit information
    outHitDist = rayhit.ray.tfar;
    outHitNormal = Vector3(rayhit.hit.Ng_x, rayhit.hit.Ng_y, rayhit.hit.Ng_z);
    outHitNormal = vector3_normalised(outHitNormal);
    
    // Map geometry ID to mesh index
    if (rayhit.hit.geomID < g_geomToMesh.size()) {
        outMeshIndex = g_geomToMesh[rayhit.hit.geomID];
    } else {
        outMeshIndex = -1;
    }
    
    return true;
    
#else
    return false;
#endif
}


bool TraceRayExtended(const Vector3 &origin, const Vector3 &dir, float maxDist,
                      float &outHitDist, Vector3 &outHitNormal, int &outMeshIndex,
                      Vector2 &outHitUV, int &outPrimID) {
#ifdef USE_EMBREE
    if (!g_sceneReady || g_scene == nullptr) {
        return false;
    }
    
    // Create ray+hit structure
    RTCRayHit rayhit;
    rayhit.ray.org_x = origin.x();
    rayhit.ray.org_y = origin.y();
    rayhit.ray.org_z = origin.z();
    rayhit.ray.dir_x = dir.x();
    rayhit.ray.dir_y = dir.y();
    rayhit.ray.dir_z = dir.z();
    rayhit.ray.tnear = 0.1f;
    rayhit.ray.tfar = maxDist;
    rayhit.ray.mask = 0xFFFFFFFF;
    rayhit.ray.flags = 0;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;
    
    // Trace ray
    rtcIntersect1(g_scene, &rayhit);
    
    // Check for hit
    if (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return false;
    }
    
    // Extract hit information
    outHitDist = rayhit.ray.tfar;
    outHitNormal = Vector3(rayhit.hit.Ng_x, rayhit.hit.Ng_y, rayhit.hit.Ng_z);
    outHitNormal = vector3_normalised(outHitNormal);
    outPrimID = rayhit.hit.primID;
    
    // Map geometry ID to mesh index
    if (rayhit.hit.geomID < g_geomToMesh.size()) {
        outMeshIndex = g_geomToMesh[rayhit.hit.geomID];
    } else {
        outMeshIndex = -1;
        return false;
    }
    
    // Interpolate UV coordinates from barycentric coordinates
    // Embree gives us u,v barycentric; w = 1 - u - v
    float u = rayhit.hit.u;
    float v = rayhit.hit.v;
    float w = 1.0f - u - v;
    
    // Get the mesh and triangle vertices
    if (outMeshIndex >= 0 && outMeshIndex < static_cast<int>(Shared::meshes.size())) {
        const Shared::Mesh_t &mesh = Shared::meshes[outMeshIndex];
        size_t triIdx = outPrimID * 3;
        
        if (triIdx + 2 < mesh.triangles.size()) {
            uint16_t i0 = mesh.triangles[triIdx + 0];
            uint16_t i1 = mesh.triangles[triIdx + 1];
            uint16_t i2 = mesh.triangles[triIdx + 2];
            
            if (i0 < mesh.vertices.size() && i1 < mesh.vertices.size() && i2 < mesh.vertices.size()) {
                const Vector2 &uv0 = mesh.vertices[i0].textureUV;
                const Vector2 &uv1 = mesh.vertices[i1].textureUV;
                const Vector2 &uv2 = mesh.vertices[i2].textureUV;
                
                // Barycentric interpolation
                outHitUV[0] = w * uv0[0] + u * uv1[0] + v * uv2[0];
                outHitUV[1] = w * uv0[1] + u * uv1[1] + v * uv2[1];
            } else {
                outHitUV = Vector2(0, 0);
            }
        } else {
            outHitUV = Vector2(0, 0);
        }
    } else {
        outHitUV = Vector2(0, 0);
    }
    
    return true;
    
#else
    return false;
#endif
}


bool IsSceneReady() {
#ifdef USE_EMBREE
    return g_sceneReady;
#else
    return false;
#endif
}


SceneStats GetSceneStats() {
#ifdef USE_EMBREE
    return g_stats;
#else
    return SceneStats{};
#endif
}

} // namespace EmbreeTrace
