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

#pragma once

/*
    HIPRT Ray Tracing Acceleration
    
    This module provides GPU-accelerated ray tracing using AMD HIP RT.
    HIPRT builds a BVH (Bounding Volume Hierarchy) from mesh geometry on
    the GPU, enabling O(log n) ray intersection tests instead of O(n) brute force.
    
    Usage:
        1. Call HIPRTTrace::Init() once at startup
        2. Call HIPRTTrace::BuildScene() after meshes are loaded
        3. Use HIPRTTrace::TestVisibility() for shadow/visibility rays
        4. Call HIPRTTrace::Shutdown() when done
*/

#include <cstdint>
#include "math/vector.h"

namespace HIPRTTrace {

// Initialize HIPRT context and allocate resources
// Returns true on success, false if HIPRT is not available
bool Init();

// Shutdown HIPRT and free all resources
void Shutdown();

// Build BVH scene from current Shared::meshes
// Call this after meshes are loaded but before ray tracing
// skipSkyMeshes: if true, meshes with C_SKY flag are excluded (for shadow rays)
void BuildScene(bool skipSkyMeshes = true);

// Clear the current scene (call before rebuilding)
void ClearScene();

// Test if a ray is blocked by geometry (shadow/visibility test)
// Returns true if ray hits something before maxDist
// origin: ray start position
// dir: normalized ray direction
// maxDist: maximum distance to test
bool TestVisibility(const Vector3 &origin, const Vector3 &dir, float maxDist);

// Batch visibility test - test many rays at once on the GPU
// Much faster than individual TestVisibility calls due to amortized launch overhead
// count: number of rays
// origins: array of ray origins (length = count)
// dirs: array of ray directions (length = count)
// maxDists: array of max distances (length = count)
// results: output array - 1 if ray hits something, 0 if miss (length = count)
void BatchTestVisibility(int count, const Vector3* origins, const Vector3* dirs,
                         const float* maxDists, uint8_t* results);

// Batch closest-hit trace - trace many rays at once on the GPU
// Returns hit distance and normal per ray. Much faster than individual TraceRay calls.
// count: number of rays
// origins: array of ray origins (length = count)
// dirs: array of ray directions (length = count)
// maxDists: array of max distances (length = count)
// outHitDists: output hit distances, -1.0f for miss (length = count)
// outHitNormalXs/Ys/Zs: output hit normal components (length = count each, can be nullptr)
// outMeshIndices: output mesh indices, -1 for miss (length = count, can be nullptr)
void BatchTraceRay(int count, const Vector3* origins, const Vector3* dirs,
                   const float* maxDists, float* outHitDists,
                   float* outHitNormalXs = nullptr, float* outHitNormalYs = nullptr,
                   float* outHitNormalZs = nullptr, int* outMeshIndices = nullptr);

// Trace a ray and get hit information
// Returns true if ray hits something
// origin: ray start position
// dir: normalized ray direction
// maxDist: maximum distance to test
// outHitDist: distance to hit point (only valid if returns true)
// outHitNormal: surface normal at hit point (only valid if returns true)
// outMeshIndex: index of mesh that was hit (only valid if returns true)
bool TraceRay(const Vector3 &origin, const Vector3 &dir, float maxDist,
              float &outHitDist, Vector3 &outHitNormal, int &outMeshIndex);

// Trace a ray and get extended hit information including UV coordinates
// Returns true if ray hits something
// origin: ray start position
// dir: normalized ray direction
// maxDist: maximum distance to test
// outHitDist: distance to hit point (only valid if returns true)
// outHitNormal: surface normal at hit point (only valid if returns true)
// outMeshIndex: index of mesh that was hit (only valid if returns true)
// outHitUV: texture UV coordinates at hit point (only valid if returns true)
// outPrimID: triangle index within mesh (only valid if returns true)
bool TraceRayExtended(const Vector3 &origin, const Vector3 &dir, float maxDist,
                      float &outHitDist, Vector3 &outHitNormal, int &outMeshIndex,
                      Vector2 &outHitUV, int &outPrimID);

// Check if HIPRT scene is ready for ray tracing
bool IsSceneReady();

// Get statistics about the current scene
struct SceneStats {
    size_t numMeshes;
    size_t numTriangles;
    size_t numVertices;
    double buildTimeMs;
};
SceneStats GetSceneStats();

} // namespace HIPRTTrace
