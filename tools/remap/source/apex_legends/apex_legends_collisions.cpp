/* -------------------------------------------------------------------------------

   Copyright (C) 2022-2024 MRVN-Radiant and contributors.
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


#include "../remap.h"
#include "../bspfile_abstract.h"
#include <ctime>
#include <algorithm>
#include <numeric>
#include <limits>
#include <cstring>
#include <map>
#include <tuple>

// BVH4 Collision System
//
// CollBvh4Node_s: 64-byte BVH4 nodes with packed bounds and metadata
// Child types: 0=Node, 1=None, 2=Empty, 3=Bundle, 4=TriStrip, 5=Poly3, 6=Poly4, 7=Poly5+, 8=ConvexHull, 9=StaticProp, 10=Heightfield
// Packed vertices: 6 bytes (int16 x,y,z), decoded as: world = origin + (int16 << 16) * scale
// Node bounds: int16[24] in SOA format, same decode formula
//
// Poly vertex indexing (from CollPoly_Visit_5):
//   baseVertex stored in header bytes 2-3 (not shifted)
//   running_base = baseVertex << 10
//   Per-triangle: v0 = running_base + offset, v1 = v0 + 1 + delta1, v2 = v0 + 1 + delta2
//   running_base updated to v0 after each triangle
//
// Vertex decode:
//   packed_verts[idx] at byte offset idx * 6
//   SSE: cvtepi32_ps(unpacklo_epi16(0, int16x3)) produces (int16 << 16)
//   world.xyz = origin.xyz + (int16 << 16) * origin.w

namespace {
    // BVH4 child types (matches Collision::ChildType enum)
    constexpr int BVH4_TYPE_NODE        = 0;  // Internal BVH node
    constexpr int BVH4_TYPE_NONE        = 1;  // Empty child slot
    constexpr int BVH4_TYPE_EMPTY       = 2;  // Empty leaf
    constexpr int BVH4_TYPE_BUNDLE      = 3;  // Bundle of leaf entries
    constexpr int BVH4_TYPE_TRISTRIP    = 4;  // Triangle strip (FLOAT vertices)
    constexpr int BVH4_TYPE_POLY3       = 5;  // Triangle (PACKED int16 vertices)
    constexpr int BVH4_TYPE_POLY4       = 6;  // Quad (PACKED int16 vertices)
    constexpr int BVH4_TYPE_POLY5PLUS   = 7;  // 5+ vertex polygon (PACKED int16 vertices)
    constexpr int BVH4_TYPE_CONVEXHULL  = 8;  // Convex hull (brush collision)
    constexpr int BVH4_TYPE_STATICPROP  = 9;  // Static prop reference
    constexpr int BVH4_TYPE_HEIGHTFIELD = 10; // Heightfield terrain

    // Component-wise min/max for Vector3
    inline Vector3 Vec3Min(const Vector3& a, const Vector3& b) {
        return Vector3(
            std::min(a.x(), b.x()),
            std::min(a.y(), b.y()),
            std::min(a.z(), b.z())
        );
    }

    inline Vector3 Vec3Max(const Vector3& a, const Vector3& b) {
        return Vector3(
            std::max(a.x(), b.x()),
            std::max(a.y(), b.y()),
            std::max(a.z(), b.z())
        );
    }

    constexpr int MAX_TRIS_PER_LEAF = 16;
    constexpr int MAX_BVH_DEPTH = 32;
    constexpr float MIN_TRIANGLE_EDGE = 0.1f;
    constexpr float MIN_TRIANGLE_AREA = 0.01f;

    struct CollisionTri_t {
        Vector3 v0, v1, v2;
        Vector3 normal;
        int contentFlags;
        int surfaceFlags;
    };

    struct CollisionHull_t {
        std::vector<Vector3> vertices;
        std::vector<std::array<int, 3>> faces;
        std::vector<Plane3f> planes;
        int contentFlags;
        Vector3 origin;
        float scale;
    };

    struct CollisionStaticProp_t {
        uint32_t propIndex;
        MinMax bounds;
    };

    struct CollisionHeightfield_t {
        uint8_t cellX, cellY;
        std::array<int16_t, 16> heights;
        uint8_t materialIndex;
        MinMax bounds;
    };

    struct BVHBuildNode_t {
        MinMax bounds;
        int childIndices[4] = { -1, -1, -1, -1 };
        int childTypes[4] = { BVH4_TYPE_NONE, BVH4_TYPE_NONE, BVH4_TYPE_NONE, BVH4_TYPE_NONE };
        std::vector<int> triangleIndices;
        std::vector<int> hullIndices;
        std::vector<int> staticPropIndices;
        std::vector<int> heightfieldIndices;
        bool isLeaf = false;
        int contentFlags = CONTENTS_SOLID;
        int preferredLeafType = BVH4_TYPE_TRISTRIP;
    };

    std::vector<CollisionTri_t> g_collisionTris;
    std::vector<CollisionHull_t> g_collisionHulls;
    std::vector<CollisionStaticProp_t> g_collisionStaticProps;
    std::vector<CollisionHeightfield_t> g_collisionHeightfields;
    std::vector<BVHBuildNode_t> g_bvhBuildNodes;
    Vector3 g_bvhOrigin = Vector3(0, 0, 0);
    float g_bvhScale = 1.0f / 65536.0f;
    uint32_t g_modelPackedVertexBase = 0;
    uint32_t g_modelCollisionVertexBase = 0;

    // Snaps vertex to grid to prevent floating point precision issues
    Vector3 SnapVertexToGrid(const Vector3& vert) {
        constexpr float GRID_SIZE = 0.03125f;
        constexpr float invGrid = 1.0f / GRID_SIZE;

        return Vector3(
            std::round(vert.x() * invGrid) / invGrid,
            std::round(vert.y() * invGrid) / invGrid,
            std::round(vert.z() * invGrid) / invGrid
        );
    }

    // Encodes world position as packed int16x3 vertex
    // Decode: world = origin + (int16 << 16) * scale
    uint32_t EmitPackedVertex(const Vector3& worldPos) {
        float invScaleFactor = 1.0f / (g_bvhScale * 65536.0f);

        float px = (worldPos.x() - g_bvhOrigin.x()) * invScaleFactor;
        float py = (worldPos.y() - g_bvhOrigin.y()) * invScaleFactor;
        float pz = (worldPos.z() - g_bvhOrigin.z()) * invScaleFactor;

        ApexLegends::PackedVertex_t vert;
        vert.x = static_cast<int16_t>(std::clamp(px, -32768.0f, 32767.0f));
        vert.y = static_cast<int16_t>(std::clamp(py, -32768.0f, 32767.0f));
        vert.z = static_cast<int16_t>(std::clamp(pz, -32768.0f, 32767.0f));

        uint32_t idx = static_cast<uint32_t>(ApexLegends::Bsp::packedVertices.size());
        ApexLegends::Bsp::packedVertices.push_back(vert);

        return idx;
    }

    // Adds float3 collision vertex to lump
    uint32_t EmitCollisionVertex(const Vector3& worldPos) {
        ApexLegends::CollisionVertex_t vert;
        vert.x = worldPos.x();
        vert.y = worldPos.y();
        vert.z = worldPos.z();
        
        uint32_t idx = static_cast<uint32_t>(ApexLegends::Bsp::collisionVertices.size());
        ApexLegends::Bsp::collisionVertices.push_back(vert);
        
        return idx;
    }

    // Converts float bounds to int16_t format
    // Layout: [Xmin x4][Xmax x4][Ymin x4][Ymax x4][Zmin x4][Zmax x4]
    // Decode: worldPos = origin + (int16 * 65536) * scale
    void PackBoundsToInt16(const MinMax bounds[4], int16_t outBounds[24]) {
        float invScaleFactor = 1.0f / (g_bvhScale * 65536.0f);

        for (int child = 0; child < 4; child++) {
            float minX = (bounds[child].mins[0] - g_bvhOrigin[0]) * invScaleFactor;
            float minY = (bounds[child].mins[1] - g_bvhOrigin[1]) * invScaleFactor;
            float minZ = (bounds[child].mins[2] - g_bvhOrigin[2]) * invScaleFactor;
            float maxX = (bounds[child].maxs[0] - g_bvhOrigin[0]) * invScaleFactor;
            float maxY = (bounds[child].maxs[1] - g_bvhOrigin[1]) * invScaleFactor;
            float maxZ = (bounds[child].maxs[2] - g_bvhOrigin[2]) * invScaleFactor;

            minX = std::clamp(minX, -32768.0f, 32767.0f);
            minY = std::clamp(minY, -32768.0f, 32767.0f);
            minZ = std::clamp(minZ, -32768.0f, 32767.0f);
            maxX = std::clamp(maxX, -32768.0f, 32767.0f);
            maxY = std::clamp(maxY, -32768.0f, 32767.0f);
            maxZ = std::clamp(maxZ, -32768.0f, 32767.0f);

            // Layout: [Xmin x4][Xmax x4][Ymin x4][Ymax x4][Zmin x4][Zmax x4]
            outBounds[0 + child]  = static_cast<int16_t>(std::floor(minX));  // X min
            outBounds[4 + child]  = static_cast<int16_t>(std::ceil(maxX));   // X max
            outBounds[8 + child]  = static_cast<int16_t>(std::floor(minY));  // Y min
            outBounds[12 + child] = static_cast<int16_t>(std::ceil(maxY));   // Y max
            outBounds[16 + child] = static_cast<int16_t>(std::floor(minZ));  // Z min
            outBounds[20 + child] = static_cast<int16_t>(std::ceil(maxZ));   // Z max
        }
    }

    // Emits a Type 5 (Poly3) triangle leaf using packed vertices
    // Header: [0-11] surfPropIdx, [12-15] numPolys-1, [16-31] baseVertex
    // Per-triangle: [0-10] v0 offset, [11-19] v1 delta, [20-28] v2 delta, [29-31] edge flags (must be 7)
    // Vertex decode: running_base = baseVertex << 10, v0 = running_base + offset, v1 = v0 + 1 + delta1, v2 = v0 + 1 + delta2
    int EmitPoly3Leaf(const std::vector<int>& triIndices, int surfPropIdx = 0) {
        if (triIndices.empty()) {
            return ApexLegends::EmitBVHDataleaf();
        }
        
        int numTris = std::min((int)triIndices.size(), 16);
        int leafIndex = ApexLegends::Bsp::bvhLeafDatas.size();

        uint32_t baseVertexGlobal = static_cast<uint32_t>(ApexLegends::Bsp::packedVertices.size());

        // Swap v1/v2 to reverse winding order (game computes normals as (v1-v0) × (v0-v2))
        for (int i = 0; i < numTris; i++) {
            const CollisionTri_t& tri = g_collisionTris[triIndices[i]];
            EmitPackedVertex(tri.v0);
            EmitPackedVertex(tri.v2);
            EmitPackedVertex(tri.v1);
        }

        uint32_t baseVertexRelative = baseVertexGlobal - g_modelPackedVertexBase;
        uint32_t baseVertexEncoded = baseVertexRelative >> 10;

        uint32_t header = (surfPropIdx & 0xFFF) | (((numTris - 1) & 0xF) << 12);
        uint32_t headerWord = header | (baseVertexEncoded << 16);
        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(headerWord));

        uint32_t running_base = baseVertexEncoded << 10;

        for (int i = 0; i < numTris; i++) {
            uint32_t v0_global = baseVertexRelative + i * 3;
            uint32_t v1_global = v0_global + 1;
            uint32_t v2_global = v0_global + 2;

            uint32_t v0_offset = v0_global - running_base;
            uint32_t v1_delta = v1_global - (v0_global + 1);
            uint32_t v2_delta = v2_global - (v0_global + 1);

            constexpr uint32_t EDGE_FLAGS_TEST_ALL = 7;
            uint32_t triData = (v0_offset & 0x7FF) | ((v1_delta & 0x1FF) << 11) | ((v2_delta & 0x1FF) << 20) | (EDGE_FLAGS_TEST_ALL << 29);
            ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(triData));

            running_base = v0_global;
        }
        
        return leafIndex;
    }

    // Emits a Type 4 (TriStrip) leaf using FLOAT vertices
    // Header: [0-11] surfPropIdx, [12-15] numPolys-1, [16-31] baseVertex
    // Per-triangle: [0-10] v0 offset, [11-19] v1 delta, [20-28] v2 delta, [29-31] edge flags (must be 7)
    int EmitTriangleStripLeaf(const std::vector<int>& triIndices, int surfPropIdx = 0) {
        if (triIndices.empty()) {
            return ApexLegends::EmitBVHDataleaf();
        }
        
        int numTris = std::min((int)triIndices.size(), 16);
        int leafIndex = ApexLegends::Bsp::bvhLeafDatas.size();

        uint32_t firstVertexIdx = static_cast<uint32_t>(ApexLegends::Bsp::collisionVertices.size());

        // Swap v1/v2 to reverse winding order
        for (int i = 0; i < numTris; i++) {
            const CollisionTri_t& tri = g_collisionTris[triIndices[i]];
            EmitCollisionVertex(tri.v0);
            EmitCollisionVertex(tri.v2);
            EmitCollisionVertex(tri.v1);
        }

        uint32_t baseVertexRelative = firstVertexIdx - g_modelCollisionVertexBase;
        uint32_t baseVertexEncoded = baseVertexRelative >> 10;

        uint32_t headerWord = (surfPropIdx & 0xFFF) |
                              (((numTris - 1) & 0xF) << 12) |
                              (baseVertexEncoded << 16);
        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(headerWord));

        uint32_t running_base = baseVertexEncoded << 10;

        for (int i = 0; i < numTris; i++) {
            uint32_t v0_idx = baseVertexRelative + i * 3;
            uint32_t v1_idx = v0_idx + 1;
            uint32_t v2_idx = v0_idx + 2;

            uint32_t v0_offset = v0_idx - running_base;
            uint32_t v1_delta = v1_idx - v0_idx - 1;
            uint32_t v2_delta = v2_idx - v0_idx - 1;

            constexpr uint32_t EDGE_FLAGS_TEST_ALL = 7;
            uint32_t triData = (v0_offset & 0x7FF) | ((v1_delta & 0x1FF) << 11) | ((v2_delta & 0x1FF) << 20) | (EDGE_FLAGS_TEST_ALL << 29);
            ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(triData));

            running_base = v0_idx;
        }
        
        return leafIndex;
    }

    // Emits a quad polygon leaf (type 6) - converts to triangles and uses Poly3
    int EmitPoly4Leaf(const std::vector<int>& quadIndices, int surfPropIdx = 0) {
        return EmitPoly3Leaf(quadIndices, surfPropIdx);
    }

    // Emits a convex hull leaf (type 8) for brush collision
    // Layout: [0] numVerts, [1] numFaces, [2] numTriSets, [3] numQuadSets,
    //         [4-19] origin (3 floats), [16-19] scale (float),
    //         [20+] vertices (6 bytes each), face indices (3 bytes each),
    //         embedded Poly data, final surfPropIdx
    // Vertex decode: worldPos = origin + (int16 << 16) * scale
    int EmitConvexHullLeaf(const CollisionHull_t& hull, int surfPropIdx = 0) {
        if (hull.vertices.empty() || hull.faces.empty()) {
            return ApexLegends::EmitBVHDataleaf();
        }
        
        int leafIndex = ApexLegends::Bsp::bvhLeafDatas.size();

        int numVerts = std::min((int)hull.vertices.size(), 255);
        int numFaces = std::min((int)hull.faces.size(), 255);
        int numTriSets = 1;
        int numQuadSets = 0;

        uint32_t header = (numVerts & 0xFF) |
                          ((numFaces & 0xFF) << 8) |
                          ((numTriSets & 0xFF) << 16) |
                          ((numQuadSets & 0xFF) << 24);
        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(header));

        union { float f; int32_t i; } floatInt;
        floatInt.f = hull.origin.x();
        ApexLegends::Bsp::bvhLeafDatas.push_back(floatInt.i);
        floatInt.f = hull.origin.y();
        ApexLegends::Bsp::bvhLeafDatas.push_back(floatInt.i);
        floatInt.f = hull.origin.z();
        ApexLegends::Bsp::bvhLeafDatas.push_back(floatInt.i);

        floatInt.f = hull.scale;
        ApexLegends::Bsp::bvhLeafDatas.push_back(floatInt.i);

        float invScale = 1.0f / (hull.scale * 65536.0f);
        std::vector<int16_t> packedVerts;
        packedVerts.reserve(numVerts * 3);

        for (int i = 0; i < numVerts; i++) {
            const Vector3& v = hull.vertices[i];
            float px = (v.x() - hull.origin.x()) * invScale;
            float py = (v.y() - hull.origin.y()) * invScale;
            float pz = (v.z() - hull.origin.z()) * invScale;

            packedVerts.push_back(static_cast<int16_t>(std::clamp(px, -32768.0f, 32767.0f)));
            packedVerts.push_back(static_cast<int16_t>(std::clamp(py, -32768.0f, 32767.0f)));
            packedVerts.push_back(static_cast<int16_t>(std::clamp(pz, -32768.0f, 32767.0f)));
        }

        for (size_t i = 0; i < packedVerts.size(); i += 2) {
            uint32_t word = static_cast<uint16_t>(packedVerts[i]);
            if (i + 1 < packedVerts.size()) {
                word |= static_cast<uint32_t>(static_cast<uint16_t>(packedVerts[i + 1])) << 16;
            }
            ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(word));
        }

        std::vector<uint8_t> faceBytes;
        faceBytes.reserve(numFaces * 3);

        for (int i = 0; i < numFaces; i++) {
            faceBytes.push_back(static_cast<uint8_t>(hull.faces[i][0]));
            faceBytes.push_back(static_cast<uint8_t>(hull.faces[i][1]));
            faceBytes.push_back(static_cast<uint8_t>(hull.faces[i][2]));
        }

        while (faceBytes.size() % 4 != 0) {
            faceBytes.push_back(0);
        }

        for (size_t i = 0; i < faceBytes.size(); i += 4) {
            uint32_t word = faceBytes[i] |
                           (faceBytes[i + 1] << 8) |
                           (faceBytes[i + 2] << 16) |
                           (faceBytes[i + 3] << 24);
            ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(word));
        }

        if (numTriSets > 0 && numFaces > 0) {
            int trisToEmit = std::min(numFaces, 16);

            uint32_t polyHeader = (surfPropIdx & 0xFFF) | (((trisToEmit - 1) & 0xF) << 12);
            uint32_t baseVertex = 0;
            uint32_t headerWord = polyHeader | (baseVertex << 16);
            ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(headerWord));

            uint32_t running_base = 0;

            for (int i = 0; i < trisToEmit; i++) {
                uint32_t v0 = hull.faces[i][0];
                uint32_t v1 = hull.faces[i][1];
                uint32_t v2 = hull.faces[i][2];

                uint32_t v0_offset = v0 - running_base;
                int32_t v1_delta = static_cast<int32_t>(v1) - static_cast<int32_t>(v0) - 1;
                int32_t v2_delta = static_cast<int32_t>(v2) - static_cast<int32_t>(v0) - 1;

                v1_delta = std::clamp(v1_delta, -256, 255);
                v2_delta = std::clamp(v2_delta, -256, 255);

                uint32_t triData = (v0_offset & 0x7FF) |
                                   ((static_cast<uint32_t>(v1_delta) & 0x1FF) << 11) |
                                   ((static_cast<uint32_t>(v2_delta) & 0x1FF) << 20);
                ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(triData));

                running_base = v0;
            }
        }

        uint32_t finalSurfProp = surfPropIdx & 0xFFF;
        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(finalSurfProp));

        return leafIndex;
    }

    int EmitConvexHullLeaf(const std::vector<int>& triIndices, int contentsMaskIdx = 0) {
        if (triIndices.empty()) {
            return ApexLegends::EmitBVHDataleaf();
        }

        CollisionHull_t hull;

        Vector3 mins(FLT_MAX, FLT_MAX, FLT_MAX);
        Vector3 maxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        std::map<std::tuple<float, float, float>, int> vertexMap;

        for (int triIdx : triIndices) {
            const CollisionTri_t& tri = g_collisionTris[triIdx];

            for (const Vector3* vp : {&tri.v0, &tri.v1, &tri.v2}) {
                mins = Vec3Min(mins, *vp);
                maxs = Vec3Max(maxs, *vp);

                auto key = std::make_tuple(vp->x(), vp->y(), vp->z());
                if (vertexMap.find(key) == vertexMap.end()) {
                    int idx = hull.vertices.size();
                    vertexMap[key] = idx;
                    hull.vertices.push_back(*vp);
                }
            }
        }

        for (int triIdx : triIndices) {
            const CollisionTri_t& tri = g_collisionTris[triIdx];

            auto k0 = std::make_tuple(tri.v0.x(), tri.v0.y(), tri.v0.z());
            auto k1 = std::make_tuple(tri.v1.x(), tri.v1.y(), tri.v1.z());
            auto k2 = std::make_tuple(tri.v2.x(), tri.v2.y(), tri.v2.z());

            hull.faces.push_back({vertexMap[k0], vertexMap[k1], vertexMap[k2]});
        }

        Vector3 center = (mins + maxs) * 0.5f;
        Vector3 extent = maxs - mins;
        float maxExtent = std::max({extent.x(), extent.y(), extent.z()});

        hull.origin = center;
        hull.scale = maxExtent / 65536.0f;
        if (hull.scale < 1e-6f) hull.scale = 1.0f;

        hull.contentFlags = CONTENTS_SOLID;

        return EmitConvexHullLeaf(hull, 0);
    }

    int EmitStaticPropLeaf(uint32_t propIndex) {
        int leafIndex = ApexLegends::Bsp::bvhLeafDatas.size();
        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(propIndex));
        return leafIndex;
    }

    int EmitHeightfieldLeaf(const CollisionHeightfield_t& hfield) {
        int leafIndex = ApexLegends::Bsp::bvhLeafDatas.size();

        uint32_t cellData = hfield.cellX | (hfield.cellY << 8) | (0 << 16);
        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(cellData));

        uint32_t matData = hfield.materialIndex;
        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(matData));

        for (int i = 0; i < 16; i += 2) {
            uint32_t packed = (static_cast<uint16_t>(hfield.heights[i])) |
                             (static_cast<uint16_t>(hfield.heights[i + 1]) << 16);
            ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(packed));
        }

        return leafIndex;
    }

    int EmitBundleLeaf(const std::vector<std::pair<int, int>>& children) {
        if (children.empty()) {
            return ApexLegends::EmitBVHDataleaf();
        }

        int leafIndex = ApexLegends::Bsp::bvhLeafDatas.size();

        ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(children.size()));

        for (const auto& child : children) {
            uint32_t entry = (0) | (child.second << 8) | (1 << 16);
            ApexLegends::Bsp::bvhLeafDatas.push_back(static_cast<int32_t>(entry));
        }

        return leafIndex;
    }

    int SelectBestLeafType(const std::vector<int>& triIndices, int preferredType = BVH4_TYPE_TRISTRIP) {
        if (triIndices.empty()) {
            return BVH4_TYPE_EMPTY;
        }

        return BVH4_TYPE_TRISTRIP;
    }

    int EmitLeafDataForType(int leafType, const std::vector<int>& triIndices, int surfPropIdx = 0) {
        if (triIndices.empty()) {
            return 0;
        }

        switch (leafType) {
            case BVH4_TYPE_TRISTRIP:
                return EmitTriangleStripLeaf(triIndices, surfPropIdx);

            case BVH4_TYPE_CONVEXHULL:
                return EmitConvexHullLeaf(triIndices, surfPropIdx);

            case BVH4_TYPE_POLY3:
            default:
                return EmitPoly3Leaf(triIndices, surfPropIdx);
        }
    }

    MinMax ComputeTriangleBounds(const CollisionTri_t& tri) {
        MinMax bounds;
        bounds.mins = Vec3Min(Vec3Min(tri.v0, tri.v1), tri.v2);
        bounds.maxs = Vec3Max(Vec3Max(tri.v0, tri.v1), tri.v2);
        return bounds;
    }

    float ComputeTriangleArea(const CollisionTri_t& tri) {
        Vector3 edge1 = tri.v1 - tri.v0;
        Vector3 edge2 = tri.v2 - tri.v0;
        Vector3 cross = vector3_cross(edge1, edge2);
        return vector3_length(cross) * 0.5f;
    }

    float ComputeMinEdgeLength(const CollisionTri_t& tri) {
        float edge0 = vector3_length(tri.v1 - tri.v0);
        float edge1 = vector3_length(tri.v2 - tri.v1);
        float edge2 = vector3_length(tri.v0 - tri.v2);
        return std::min({edge0, edge1, edge2});
    }

    bool IsDegenerateTriangle(const CollisionTri_t& tri) {
        if (ComputeMinEdgeLength(tri) < MIN_TRIANGLE_EDGE) {
            return true;
        }
        if (ComputeTriangleArea(tri) < MIN_TRIANGLE_AREA) {
            return true;
        }
        return false;
    }

    MinMax ComputeBoundsForTriangles(const std::vector<int>& triIndices) {
        MinMax bounds;
        bounds.mins = Vector3(std::numeric_limits<float>::max());
        bounds.maxs = Vector3(std::numeric_limits<float>::lowest());

        for (int idx : triIndices) {
            const CollisionTri_t& tri = g_collisionTris[idx];
            MinMax triBounds = ComputeTriangleBounds(tri);
            bounds.mins = Vec3Min(bounds.mins, triBounds.mins);
            bounds.maxs = Vec3Max(bounds.maxs, triBounds.maxs);
        }

        return bounds;
    }

    Vector3 ComputeTriangleCentroid(const CollisionTri_t& tri) {
        return (tri.v0 + tri.v1 + tri.v2) / 3.0f;
    }

    int PartitionTriangles(const std::vector<int>& triIndices, const MinMax& bounds,
                           std::vector<int> outPartitions[4]) {
        if (triIndices.size() <= MAX_TRIS_PER_LEAF) {
            outPartitions[0] = triIndices;
            return 1;
        }

        Vector3 size = bounds.maxs - bounds.mins;
        int axis = 0;
        if (size.y() > size.x()) axis = 1;
        if (size.z() > size[axis]) axis = 2;

        std::vector<int> sorted = triIndices;
        std::sort(sorted.begin(), sorted.end(), [axis](int a, int b) {
            Vector3 centA = ComputeTriangleCentroid(g_collisionTris[a]);
            Vector3 centB = ComputeTriangleCentroid(g_collisionTris[b]);
            return centA[axis] < centB[axis];
        });

        size_t count = sorted.size();
        int numPartitions;

        if (count >= 16) {
            numPartitions = 4;
        } else if (count >= 8) {
            numPartitions = 4;
        } else {
            numPartitions = 2;
        }

        for (int i = 0; i < numPartitions; i++) {
            outPartitions[i].clear();
        }

        size_t trisPerPartition = (count + numPartitions - 1) / numPartitions;
        for (size_t i = 0; i < count; i++) {
            int partition = std::min((int)(i / trisPerPartition), numPartitions - 1);
            outPartitions[partition].push_back(sorted[i]);
        }

        int actualPartitions = 0;
        for (int i = 0; i < numPartitions; i++) {
            if (!outPartitions[i].empty()) {
                if (i != actualPartitions) {
                    outPartitions[actualPartitions] = std::move(outPartitions[i]);
                    outPartitions[i].clear();
                }
                actualPartitions++;
            }
        }

        return actualPartitions;
    }

    int BuildBVH4Node(const std::vector<int>& triIndices, int depth) {
        if (triIndices.empty()) {
            return -1;
        }

        int nodeIndex = g_bvhBuildNodes.size();
        g_bvhBuildNodes.emplace_back();

        g_bvhBuildNodes[nodeIndex].bounds = ComputeBoundsForTriangles(triIndices);

        g_bvhBuildNodes[nodeIndex].contentFlags = 0;
        for (int idx : triIndices) {
            g_bvhBuildNodes[nodeIndex].contentFlags |= g_collisionTris[idx].contentFlags;
        }
        if (g_bvhBuildNodes[nodeIndex].contentFlags == 0) {
            g_bvhBuildNodes[nodeIndex].contentFlags = CONTENTS_SOLID;
        }

        if (triIndices.size() <= MAX_TRIS_PER_LEAF || depth >= MAX_BVH_DEPTH) {
            g_bvhBuildNodes[nodeIndex].isLeaf = true;
            g_bvhBuildNodes[nodeIndex].triangleIndices = triIndices;
            return nodeIndex;
        }

        MinMax nodeBounds = g_bvhBuildNodes[nodeIndex].bounds;

        std::vector<int> partitions[4];
        int numPartitions = PartitionTriangles(triIndices, nodeBounds, partitions);

        if (numPartitions <= 1) {
            g_bvhBuildNodes[nodeIndex].isLeaf = true;
            g_bvhBuildNodes[nodeIndex].triangleIndices = triIndices;
            return nodeIndex;
        }

        g_bvhBuildNodes[nodeIndex].isLeaf = false;
        for (int i = 0; i < 4; i++) {
            if (i < numPartitions && !partitions[i].empty()) {
                if (partitions[i].size() <= MAX_TRIS_PER_LEAF) {
                    int leafIndex = g_bvhBuildNodes.size();
                    g_bvhBuildNodes.emplace_back();
                    g_bvhBuildNodes[leafIndex].bounds = ComputeBoundsForTriangles(partitions[i]);
                    g_bvhBuildNodes[leafIndex].isLeaf = true;
                    g_bvhBuildNodes[leafIndex].triangleIndices = partitions[i];
                    g_bvhBuildNodes[leafIndex].contentFlags = 0;
                    for (int idx : partitions[i]) {
                        g_bvhBuildNodes[leafIndex].contentFlags |= g_collisionTris[idx].contentFlags;
                    }
                    if (g_bvhBuildNodes[leafIndex].contentFlags == 0) {
                        g_bvhBuildNodes[leafIndex].contentFlags = CONTENTS_SOLID;
                    }
                    g_bvhBuildNodes[nodeIndex].childIndices[i] = leafIndex;
                    g_bvhBuildNodes[nodeIndex].childTypes[i] = SelectBestLeafType(partitions[i], g_bvhBuildNodes[leafIndex].preferredLeafType);
                } else {
                    int childIdx = BuildBVH4Node(partitions[i], depth + 1);
                    g_bvhBuildNodes[nodeIndex].childIndices[i] = childIdx;
                    if (childIdx >= 0) {
                        if (g_bvhBuildNodes[childIdx].isLeaf) {
                            g_bvhBuildNodes[nodeIndex].childTypes[i] = SelectBestLeafType(g_bvhBuildNodes[childIdx].triangleIndices, g_bvhBuildNodes[childIdx].preferredLeafType);
                        } else {
                            g_bvhBuildNodes[nodeIndex].childTypes[i] = BVH4_TYPE_NODE;
                        }
                    }
                }
            } else {
                g_bvhBuildNodes[nodeIndex].childIndices[i] = -1;
                g_bvhBuildNodes[nodeIndex].childTypes[i] = BVH4_TYPE_NONE;
            }
        }

        return nodeIndex;
    }

    int EmitBVH4Nodes(int buildNodeIndex, int& leafDataOffset) {
        if (buildNodeIndex < 0 || buildNodeIndex >= (int)g_bvhBuildNodes.size()) {
            return -1;
        }

        const BVHBuildNode_t& buildNode = g_bvhBuildNodes[buildNodeIndex];
        int bspNodeIndex = ApexLegends::Bsp::bvhNodes.size();
        ApexLegends::Bsp::bvhNodes.emplace_back();

        memset(&ApexLegends::Bsp::bvhNodes[bspNodeIndex], 0, sizeof(ApexLegends::BVHNode_t));
        ApexLegends::Bsp::bvhNodes[bspNodeIndex].cmIndex = ApexLegends::EmitContentsMask(buildNode.contentFlags);

        if (buildNode.isLeaf) {
            int leafType = SelectBestLeafType(buildNode.triangleIndices, buildNode.preferredLeafType);
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType0 = leafType;
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType1 = BVH4_TYPE_NONE;
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType2 = BVH4_TYPE_NONE;
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType3 = BVH4_TYPE_NONE;

            ApexLegends::Bsp::bvhNodes[bspNodeIndex].index0 = EmitLeafDataForType(leafType, buildNode.triangleIndices);
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].index1 = 0;
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].index2 = 0;
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].index3 = 0;

            MinMax childBounds[4];
            childBounds[0] = buildNode.bounds;
            childBounds[1] = childBounds[2] = childBounds[3] = buildNode.bounds;
            PackBoundsToInt16(childBounds, ApexLegends::Bsp::bvhNodes[bspNodeIndex].bounds);

        } else {
            MinMax childBounds[4];

            for (int i = 0; i < 4; i++) {
                if (buildNode.childIndices[i] >= 0) {
                    const BVHBuildNode_t& childBuild = g_bvhBuildNodes[buildNode.childIndices[i]];
                    childBounds[i] = childBuild.bounds;
                } else {
                    childBounds[i] = buildNode.bounds;
                }
            }
            
            PackBoundsToInt16(childBounds, ApexLegends::Bsp::bvhNodes[bspNodeIndex].bounds);

            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType0 = buildNode.childTypes[0];
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType1 = buildNode.childTypes[1];
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType2 = buildNode.childTypes[2];
            ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType3 = buildNode.childTypes[3];

            for (int i = 0; i < 4; i++) {
                int childIndex = 0;
                int childType = buildNode.childTypes[i];

                if (buildNode.childIndices[i] >= 0) {
                    if (childType == BVH4_TYPE_NODE) {
                        childIndex = EmitBVH4Nodes(buildNode.childIndices[i], leafDataOffset);
                    } else if (childType != BVH4_TYPE_NONE && childType != BVH4_TYPE_EMPTY) {
                        const BVHBuildNode_t& childBuild = g_bvhBuildNodes[buildNode.childIndices[i]];
                        childIndex = EmitLeafDataForType(childType, childBuild.triangleIndices);
                    }
                }

                switch (i) {
                    case 0: ApexLegends::Bsp::bvhNodes[bspNodeIndex].index0 = childIndex; break;
                    case 1: ApexLegends::Bsp::bvhNodes[bspNodeIndex].index1 = childIndex; break;
                    case 2: ApexLegends::Bsp::bvhNodes[bspNodeIndex].index2 = childIndex; break;
                    case 3: ApexLegends::Bsp::bvhNodes[bspNodeIndex].index3 = childIndex; break;
                }
            }
        }

        return bspNodeIndex;
    }

    void CollectTrianglesFromMeshes() {
        g_collisionTris.clear();

        int skippedDegenerate = 0;
        int totalTris = 0;

        for (const Shared::Mesh_t& mesh : Shared::meshes) {
            int contentFlags = CONTENTS_SOLID;
            if (mesh.shaderInfo) {
                contentFlags = mesh.shaderInfo->contentFlags;

                if (!(contentFlags & CONTENTS_SOLID) &&
                    !(contentFlags & CONTENTS_PLAYERCLIP) &&
                    !(contentFlags & CONTENTS_MONSTERCLIP)) {
                    continue;
                }
            }

            const std::vector<Shared::Vertex_t>& verts = mesh.vertices;
            const std::vector<uint16_t>& indices = mesh.triangles;

            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                totalTris++;

                CollisionTri_t tri;
                tri.v0 = SnapVertexToGrid(verts[indices[i]].xyz);
                tri.v1 = SnapVertexToGrid(verts[indices[i + 1]].xyz);
                tri.v2 = SnapVertexToGrid(verts[indices[i + 2]].xyz);

                Vector3 edge1 = tri.v1 - tri.v0;
                Vector3 edge2 = tri.v2 - tri.v0;
                tri.normal = vector3_cross(edge1, edge2);
                float len = vector3_length(tri.normal);
                if (len > 0.0001f) {
                    tri.normal = tri.normal / len;
                }

                if (IsDegenerateTriangle(tri)) {
                    skippedDegenerate++;
                    continue;
                }

                tri.contentFlags = contentFlags;
                tri.surfaceFlags = mesh.shaderInfo ? mesh.shaderInfo->surfaceFlags : 0;

                g_collisionTris.push_back(tri);
            }
        }
    }
}

void ApexLegends::EmitBVHNode() {
    Sys_FPrintf(SYS_VRB, "--- Emitting Collision BVH ---\n");

    if (ApexLegends::Bsp::collisionVertices.empty()) {
        ApexLegends::CollisionVertex_t dummy;
        dummy.x = dummy.y = dummy.z = 0.0f;
        ApexLegends::Bsp::collisionVertices.push_back(dummy);
    }

    ApexLegends::Model_t& model = ApexLegends::Bsp::models.back();

    model.bvhNodeIndex = ApexLegends::Bsp::bvhNodes.size();
    model.bvhLeafIndex = ApexLegends::Bsp::bvhLeafDatas.size();

    CollectTrianglesFromMeshes();

    if (g_collisionTris.empty()) {
        Sys_FPrintf(SYS_WRN, "Warning: No collision triangles, emitting empty BVH node\n");

        model.origin[0] = model.origin[1] = model.origin[2] = 0.0f;
        model.scale = 1.0f / 65536.0f;
        model.vertexIndex = 0;
        model.bvhFlags = 0;
        ApexLegends::BVHNode_t& node = ApexLegends::Bsp::bvhNodes.emplace_back();
        memset(&node, 0, sizeof(node));
        node.cmIndex = EmitContentsMask(CONTENTS_SOLID);
        node.childType0 = BVH4_TYPE_NONE;
        node.childType1 = BVH4_TYPE_NONE;
        node.childType2 = BVH4_TYPE_NONE;
        node.childType3 = BVH4_TYPE_NONE;
        return;
    }

    MinMax overallBounds;
    overallBounds.mins = Vector3(std::numeric_limits<float>::max());
    overallBounds.maxs = Vector3(std::numeric_limits<float>::lowest());

    for (const CollisionTri_t& tri : g_collisionTris) {
        overallBounds.mins = Vec3Min(overallBounds.mins, Vec3Min(Vec3Min(tri.v0, tri.v1), tri.v2));
        overallBounds.maxs = Vec3Max(overallBounds.maxs, Vec3Max(Vec3Max(tri.v0, tri.v1), tri.v2));
    }

    Vector3 center = (overallBounds.mins + overallBounds.maxs) * 0.5f;
    Vector3 extents = (overallBounds.maxs - overallBounds.mins) * 0.5f;
    float maxExtent = std::max({extents.x(), extents.y(), extents.z(), 1.0f});

    float bvhScale;
    if (maxExtent <= 32000.0f) {
        bvhScale = 1.0f / 65536.0f;
    } else {
        bvhScale = maxExtent / (32000.0f * 65536.0f);
    }

    g_bvhOrigin = center;
    g_bvhScale = bvhScale;

    uint32_t renderVertexCount = static_cast<uint32_t>(Titanfall::Bsp::vertices.size());
    g_modelCollisionVertexBase = static_cast<uint32_t>(ApexLegends::Bsp::collisionVertices.size());
    g_modelPackedVertexBase = static_cast<uint32_t>(ApexLegends::Bsp::packedVertices.size());

    model.origin[0] = center.x();
    model.origin[1] = center.y();
    model.origin[2] = center.z();
    model.scale = bvhScale;
    model.vertexIndex = renderVertexCount + g_modelCollisionVertexBase;
    model.bvhFlags = 0;

    std::vector<int> allTriIndices(g_collisionTris.size());
    std::iota(allTriIndices.begin(), allTriIndices.end(), 0);

    g_bvhBuildNodes.clear();
    int rootBuildIndex = BuildBVH4Node(allTriIndices, 0);

    if (rootBuildIndex < 0) {
        Sys_FPrintf(SYS_WRN, "Warning: BVH build failed, emitting empty node\n");
        ApexLegends::BVHNode_t& node = ApexLegends::Bsp::bvhNodes.emplace_back();
        memset(&node, 0, sizeof(node));
        node.cmIndex = EmitContentsMask(CONTENTS_SOLID);
        node.childType0 = BVH4_TYPE_NONE;
        node.childType1 = BVH4_TYPE_NONE;
        node.childType2 = BVH4_TYPE_NONE;
        node.childType3 = BVH4_TYPE_NONE;
        return;
    }

    int leafDataOffset = 0;
    int rootNodeIndex = EmitBVH4Nodes(rootBuildIndex, leafDataOffset);
    (void)rootNodeIndex;

    Sys_FPrintf(SYS_VRB, "  Emitted %zu BVH nodes\n", ApexLegends::Bsp::bvhNodes.size() - model.bvhNodeIndex);
    Sys_FPrintf(SYS_VRB, "  Emitted %zu BVH leaf data entries\n", ApexLegends::Bsp::bvhLeafDatas.size() - model.bvhLeafIndex);
    Sys_FPrintf(SYS_VRB, "  Emitted %zu collision vertices\n", ApexLegends::Bsp::collisionVertices.size() - g_modelCollisionVertexBase);

    g_bvhBuildNodes.clear();
    g_collisionTris.clear();
}

int ApexLegends::EmitBVHDataleaf() {
    int index = ApexLegends::Bsp::bvhLeafDatas.size();
    ApexLegends::Bsp::bvhLeafDatas.emplace_back(0);
    return index;
}

int ApexLegends::EmitContentsMask(int mask) {
    for (size_t i = 0; i < ApexLegends::Bsp::contentsMasks.size(); i++) {
        if (ApexLegends::Bsp::contentsMasks[i] == mask) {
            return static_cast<int>(i);
        }
    }

    ApexLegends::Bsp::contentsMasks.emplace_back(mask);
    return static_cast<int>(ApexLegends::Bsp::contentsMasks.size() - 1);
}