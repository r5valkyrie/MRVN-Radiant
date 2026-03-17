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
#include <string>
#include <cctype>

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
        int surfPropIdx;      // Index into Surface Properties lump
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
    uint32_t g_modelBVHNodeBase = 0;
    uint32_t g_modelBVHLeafBase = 0;

    // Map from (surfaceFlags, contentFlags, surfaceName) to surface property index
    // This ensures we don't emit duplicate surface property entries
    std::map<std::tuple<uint16_t, int, std::string>, int> g_surfacePropertyMap;

    // Emits a surface property and returns its index (0-2047 range, 11-bit)
    // Surface properties link collision surfaces to material names, content masks, and surface flags
    int EmitSurfaceProperty(int surfaceFlags, int contentFlags, const char* surfaceName) {
        // Get or create contents mask index
        int contentsIdx = ApexLegends::EmitContentsMask(contentFlags);
        if (contentsIdx > 255) {
            Sys_Warning("Contents mask index %d exceeds uint8_t max, clamping\n", contentsIdx);
            contentsIdx = 255;
        }

        // Sanitize surface name for the BSP (strip 'textures/' prefix, convert slashes)
        std::string name = surfaceName ? surfaceName : "concrete";
        if (name.rfind("textures/", 0) == 0) {
            name.erase(0, 9);  // Remove 'textures/' prefix
        }
        std::replace(name.begin(), name.end(), '/', '\\');
        // Convert to uppercase to match official maps
        for (char& c : name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        // Clamp surfaceFlags to uint16
        uint16_t surfFlags = static_cast<uint16_t>(surfaceFlags & 0xFFFF);

        // Check if we already have this surface property
        auto key = std::make_tuple(surfFlags, contentFlags, name);
        auto it = g_surfacePropertyMap.find(key);
        if (it != g_surfacePropertyMap.end()) {
            return it->second;
        }

        // Find or add the surface name to the Surface Names lump (textureDataData)
        // Search for exact null-terminated string match
        size_t nameOffset = std::string::npos;
        std::string searchName = name + '\0';
        const char* tableData = Titanfall::Bsp::textureDataData.data();
        size_t tableSize = Titanfall::Bsp::textureDataData.size();
        
        for (size_t pos = 0; pos < tableSize; ) {
            // Find the end of current string
            size_t strEnd = pos;
            while (strEnd < tableSize && tableData[strEnd] != '\0') {
                strEnd++;
            }
            
            // Compare string at this position
            size_t strLen = strEnd - pos;
            if (strLen == name.size() && std::memcmp(tableData + pos, name.c_str(), strLen) == 0) {
                nameOffset = pos;
                break;
            }
            
            // Move to next string (skip the null terminator)
            pos = strEnd + 1;
        }
        
        if (nameOffset == std::string::npos) {
            // Not found, append it
            nameOffset = Titanfall::Bsp::textureDataData.size();
            Titanfall::Bsp::textureDataData.insert(
                Titanfall::Bsp::textureDataData.end(),
                name.begin(), name.end());
            Titanfall::Bsp::textureDataData.push_back('\0');
        }

        // Create the surface property entry
        ApexLegends::CollSurfProps_t prop;
        prop.surfFlags = surfFlags;
        prop.surfTypeID = 0;  // Default surface type (could be extended to parse from surfaceproperties.txt)
        prop.contentsIdx = static_cast<uint8_t>(contentsIdx);
        prop.nameOffset = static_cast<uint32_t>(nameOffset);

        int propIdx = static_cast<int>(ApexLegends::Bsp::surfaceProperties.size());
        
        // Check if we exceed the 12-bit limit (0-4095, but bit 11 is a flag so 0-2047 safe)
        if (propIdx >= 2048) {
            Sys_Warning("Surface property index %d exceeds 11-bit limit, clamping\n", propIdx);
            return 2047;
        }

        ApexLegends::Bsp::surfaceProperties.push_back(prop);
        g_surfacePropertyMap[key] = propIdx;

        return propIdx;
    }

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

    // Get the most common surface property index from a set of triangles
    // Returns 0 if no triangles provided
    int GetLeafSurfPropIdx(const std::vector<int>& triIndices) {
        if (triIndices.empty()) {
            return 0;
        }
        // Use the first triangle's surface property (they should all be the same material in a leaf)
        // Could be extended to track per-poly surface properties if needed
        return g_collisionTris[triIndices[0]].surfPropIdx;
    }

    int SelectBestLeafType(const std::vector<int>& triIndices, int preferredType = BVH4_TYPE_TRISTRIP) {
        if (triIndices.empty()) {
            return BVH4_TYPE_EMPTY;
        }

        return BVH4_TYPE_TRISTRIP;
    }

    int EmitLeafDataForType(int leafType, const std::vector<int>& triIndices) {
        if (triIndices.empty()) {
            return 0;
        }

        int surfPropIdx = GetLeafSurfPropIdx(triIndices);

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

    // Builds a BVH4 tree over static props (each prop becomes a type-9 leaf)
    int BuildStaticPropBVH(const std::vector<int>& propIndices, int depth) {
        if (propIndices.empty()) return -1;

        int nodeIndex = g_bvhBuildNodes.size();
        g_bvhBuildNodes.emplace_back();

        // Compute bounds from all props
        MinMax bounds;
        bounds.mins = Vector3(std::numeric_limits<float>::max());
        bounds.maxs = Vector3(std::numeric_limits<float>::lowest());
        for (int idx : propIndices) {
            bounds.mins = Vec3Min(bounds.mins, g_collisionStaticProps[idx].bounds.mins);
            bounds.maxs = Vec3Max(bounds.maxs, g_collisionStaticProps[idx].bounds.maxs);
        }
        g_bvhBuildNodes[nodeIndex].bounds = bounds;
        g_bvhBuildNodes[nodeIndex].contentFlags = CONTENTS_SOLID;

        // Up to 4 props can be direct children of one node (each as a STATICPROP leaf)
        if (propIndices.size() <= 4 || depth >= MAX_BVH_DEPTH) {
            g_bvhBuildNodes[nodeIndex].isLeaf = true;
            g_bvhBuildNodes[nodeIndex].staticPropIndices.assign(propIndices.begin(), propIndices.end());
            return nodeIndex;
        }

        // Partition spatially along longest axis
        Vector3 size = bounds.maxs - bounds.mins;
        int axis = 0;
        if (size.y() > size.x()) axis = 1;
        if (size.z() > size[axis]) axis = 2;

        std::vector<int> sorted = propIndices;
        std::sort(sorted.begin(), sorted.end(), [axis](int a, int b) {
            Vector3 centA = (g_collisionStaticProps[a].bounds.mins + g_collisionStaticProps[a].bounds.maxs) * 0.5f;
            Vector3 centB = (g_collisionStaticProps[b].bounds.mins + g_collisionStaticProps[b].bounds.maxs) * 0.5f;
            return centA[axis] < centB[axis];
        });

        int numPartitions = (sorted.size() >= 8) ? 4 : 2;
        std::vector<int> partitions[4];
        size_t perPart = (sorted.size() + numPartitions - 1) / numPartitions;
        for (size_t i = 0; i < sorted.size(); i++) {
            int p = std::min((int)(i / perPart), numPartitions - 1);
            partitions[p].push_back(sorted[i]);
        }

        g_bvhBuildNodes[nodeIndex].isLeaf = false;
        for (int i = 0; i < 4; i++) {
            if (i < numPartitions && !partitions[i].empty()) {
                int childIdx = BuildStaticPropBVH(partitions[i], depth + 1);
                g_bvhBuildNodes[nodeIndex].childIndices[i] = childIdx;
                if (childIdx >= 0 && g_bvhBuildNodes[childIdx].isLeaf) {
                    // Will be handled specially in EmitBVH4Nodes
                    g_bvhBuildNodes[nodeIndex].childTypes[i] = BVH4_TYPE_NODE;
                } else {
                    g_bvhBuildNodes[nodeIndex].childTypes[i] = BVH4_TYPE_NODE;
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
            if (!buildNode.staticPropIndices.empty()) {
                // Static prop leaf: each prop becomes a STATICPROP child of this node
                int childTypes[4] = { BVH4_TYPE_NONE, BVH4_TYPE_NONE, BVH4_TYPE_NONE, BVH4_TYPE_NONE };
                int childIndexes[4] = { 0, 0, 0, 0 };
                MinMax childBounds[4];
                childBounds[0] = childBounds[1] = childBounds[2] = childBounds[3] = buildNode.bounds;

                for (size_t i = 0; i < buildNode.staticPropIndices.size() && i < 4; i++) {
                    int spIdx = buildNode.staticPropIndices[i];
                    childTypes[i] = BVH4_TYPE_STATICPROP;
                    childIndexes[i] = EmitStaticPropLeaf(g_collisionStaticProps[spIdx].propIndex) - g_modelBVHLeafBase;
                    childBounds[i] = g_collisionStaticProps[spIdx].bounds;
                }

                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType0 = childTypes[0];
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType1 = childTypes[1];
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType2 = childTypes[2];
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType3 = childTypes[3];

                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index0 = childIndexes[0];
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index1 = childIndexes[1];
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index2 = childIndexes[2];
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index3 = childIndexes[3];

                PackBoundsToInt16(childBounds, ApexLegends::Bsp::bvhNodes[bspNodeIndex].bounds);
            } else {
                // Triangle leaf (existing logic)
                int leafType = SelectBestLeafType(buildNode.triangleIndices, buildNode.preferredLeafType);
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType0 = leafType;
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType1 = BVH4_TYPE_NONE;
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType2 = BVH4_TYPE_NONE;
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].childType3 = BVH4_TYPE_NONE;

                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index0 = EmitLeafDataForType(leafType, buildNode.triangleIndices) - g_modelBVHLeafBase;
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index1 = 0;
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index2 = 0;
                ApexLegends::Bsp::bvhNodes[bspNodeIndex].index3 = 0;

                MinMax childBounds[4];
                childBounds[0] = buildNode.bounds;
                childBounds[1] = childBounds[2] = childBounds[3] = buildNode.bounds;
                PackBoundsToInt16(childBounds, ApexLegends::Bsp::bvhNodes[bspNodeIndex].bounds);
            }

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
                        childIndex = EmitBVH4Nodes(buildNode.childIndices[i], leafDataOffset) - g_modelBVHNodeBase;
                    } else if (childType != BVH4_TYPE_NONE && childType != BVH4_TYPE_EMPTY) {
                        const BVHBuildNode_t& childBuild = g_bvhBuildNodes[buildNode.childIndices[i]];
                        childIndex = EmitLeafDataForType(childType, childBuild.triangleIndices) - g_modelBVHLeafBase;
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
        g_surfacePropertyMap.clear();

        int skippedDegenerate = 0;
        int totalTris = 0;

        for (const Shared::Mesh_t& mesh : Shared::meshes) {
            if (mesh.noCollision) {
                continue;
            }

            int contentFlags = CONTENTS_SOLID;
            int surfaceFlags = 0;
            const char* surfaceName = "concrete";
            
            if (mesh.shaderInfo) {
                contentFlags = mesh.shaderInfo->contentFlags;
                surfaceFlags = mesh.shaderInfo->surfaceFlags;
                surfaceName = mesh.shaderInfo->shader.c_str();

                if (!(contentFlags & CONTENTS_SOLID) &&
                    !(contentFlags & CONTENTS_PLAYERCLIP) &&
                    !(contentFlags & CONTENTS_MONSTERCLIP)) {
                    continue;
                }
            }

            // Get or create surface property for this mesh
            int surfPropIdx = EmitSurfaceProperty(surfaceFlags, contentFlags, surfaceName);

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
                tri.surfaceFlags = surfaceFlags;
                tri.surfPropIdx = surfPropIdx;

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

    g_modelBVHNodeBase = static_cast<uint32_t>(model.bvhNodeIndex);
    g_modelBVHLeafBase = static_cast<uint32_t>(model.bvhLeafIndex);

    CollectTrianglesFromMeshes();

    bool hasTris = !g_collisionTris.empty();
    bool hasProps = !g_collisionStaticProps.empty();

    if (!hasTris && !hasProps) {
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

    // Compute overall bounds from triangles AND static props
    MinMax overallBounds;
    overallBounds.mins = Vector3(std::numeric_limits<float>::max());
    overallBounds.maxs = Vector3(std::numeric_limits<float>::lowest());

    for (const CollisionTri_t& tri : g_collisionTris) {
        overallBounds.mins = Vec3Min(overallBounds.mins, Vec3Min(Vec3Min(tri.v0, tri.v1), tri.v2));
        overallBounds.maxs = Vec3Max(overallBounds.maxs, Vec3Max(Vec3Max(tri.v0, tri.v1), tri.v2));
    }

    for (const CollisionStaticProp_t& sp : g_collisionStaticProps) {
        overallBounds.mins = Vec3Min(overallBounds.mins, sp.bounds.mins);
        overallBounds.maxs = Vec3Max(overallBounds.maxs, sp.bounds.maxs);
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

    g_modelCollisionVertexBase = static_cast<uint32_t>(ApexLegends::Bsp::collisionVertices.size());
    g_modelPackedVertexBase = static_cast<uint32_t>(ApexLegends::Bsp::packedVertices.size());

    model.origin[0] = center.x();
    model.origin[1] = center.y();
    model.origin[2] = center.z();
    model.scale = bvhScale;
    /* Store collision vertex base only; the final render vertex count
       is added in CompileR5BSPFile() once all entities are processed. */
    model.vertexIndex = g_modelCollisionVertexBase;
    model.bvhFlags = 0;

    g_bvhBuildNodes.clear();

    // Build triangle BVH
    int triRootIndex = -1;
    if (hasTris) {
        std::vector<int> allTriIndices(g_collisionTris.size());
        std::iota(allTriIndices.begin(), allTriIndices.end(), 0);
        triRootIndex = BuildBVH4Node(allTriIndices, 0);
    }

    // Build static prop BVH
    int propRootIndex = -1;
    if (hasProps) {
        std::vector<int> allPropIndices(g_collisionStaticProps.size());
        std::iota(allPropIndices.begin(), allPropIndices.end(), 0);
        propRootIndex = BuildStaticPropBVH(allPropIndices, 0);
        Sys_FPrintf(SYS_VRB, "  Built static prop collision BVH for %zu props\n", g_collisionStaticProps.size());
    }

    // Determine final root
    int rootBuildIndex;
    if (triRootIndex >= 0 && propRootIndex >= 0) {
        // Create wrapper root combining triangle BVH and static prop BVH
        rootBuildIndex = g_bvhBuildNodes.size();
        g_bvhBuildNodes.emplace_back();
        g_bvhBuildNodes[rootBuildIndex].bounds = overallBounds;
        g_bvhBuildNodes[rootBuildIndex].isLeaf = false;
        g_bvhBuildNodes[rootBuildIndex].contentFlags = CONTENTS_SOLID;
        g_bvhBuildNodes[rootBuildIndex].childIndices[0] = triRootIndex;
        g_bvhBuildNodes[rootBuildIndex].childTypes[0] = BVH4_TYPE_NODE;
        g_bvhBuildNodes[rootBuildIndex].childIndices[1] = propRootIndex;
        g_bvhBuildNodes[rootBuildIndex].childTypes[1] = BVH4_TYPE_NODE;
    } else if (triRootIndex >= 0) {
        rootBuildIndex = triRootIndex;
    } else {
        rootBuildIndex = propRootIndex;
    }

    if (rootBuildIndex < 0) {
        Sys_Warning("BVH build failed, emitting empty node\n");
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
    Sys_FPrintf(SYS_VRB, "  Emitted %zu surface properties\n", ApexLegends::Bsp::surfaceProperties.size());
    if (hasProps) {
        Sys_FPrintf(SYS_VRB, "  Emitted %zu static prop collision entries\n", g_collisionStaticProps.size());
    }

    g_bvhBuildNodes.clear();
    g_collisionTris.clear();
    g_collisionStaticProps.clear();
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

void ApexLegends::AddCollisionStaticProp(uint32_t propIndex, const MinMax& worldBounds) {
    CollisionStaticProp_t sp;
    sp.propIndex = propIndex;
    sp.bounds = worldBounds;
    g_collisionStaticProps.push_back(sp);
}


/*
    SerializeCollisionToEntity()
    Builds and serializes collision data directly from an entity's brush geometry
    into base64-encoded *coll0, *coll1, ... key-value pairs.

    Entities in .ent files (e.g. triggers in _script.ent) need their collision
    data embedded as *coll key-value pairs.  The format is a self-contained
    CollBvhSerializedHeader47_s blob with one BVH4 node whose children are
    convex hull leaves built from the entity's brushes.

    This is independent of the global BVH lump arrays – it reads directly from
    entity.brushes side windings.
*/
void ApexLegends::SerializeCollisionToEntity(entity_t &entity) {
    if (entity.brushes.empty()) {
        return;
    }

    /* ---- determine contents mask and surface name from entity class ---- */
    constexpr uint32_t TRIGGER_CONTENTS_MASK = 0x00EB1280;
    uint32_t contentsMask = TRIGGER_CONTENTS_MASK;

    const char *cls = entity.classname();
    std::string surfaceName = "TOOLS\\TOOLSTRIGGER";
    if (striEqualPrefix(cls, "trigger_out_of_bounds")) {
        surfaceName = "TOOLS\\TOOLSOUT_OF_BOUNDS";
    } else if (striEqualPrefix(cls, "trigger_no_zipline")) {
        surfaceName = "TOOLS\\TOOLSTRIGGER_NO_ZIPLINE";
    } else if (striEqualPrefix(cls, "trigger_slip")) {
        surfaceName = "TOOLS\\TOOLSTRIGGER_SLIP";
    } else if (striEqual(cls, "envmap_volume")) {
        surfaceName = "TOOLS\\TOOLSENVMAPVOLUME";
        contentsMask = 0x10E31240;
    }

    /* ---- collect per-brush convex hull geometry ---- */
    struct BrushHull {
        std::vector<Vector3> vertices;
        std::vector<std::vector<int>> sideWindings;  // full winding per brush side
        MinMax bounds;
    };

    std::vector<BrushHull> hulls;

    /* compute entity origin from brush geometry for the "origin" key */
    MinMax entityBounds;

    for (const brush_t &brush : entity.brushes) {
        BrushHull hull;
        std::map<std::tuple<float, float, float>, int> vertexMap;

        for (const side_t &side : brush.sides) {
            if (side.winding.size() < 3) continue;

            std::vector<int> sideIndices;
            for (const Vector3 &v : side.winding) {
                auto key = std::make_tuple(v.x(), v.y(), v.z());
                auto it = vertexMap.find(key);
                if (it == vertexMap.end()) {
                    int idx = static_cast<int>(hull.vertices.size());
                    vertexMap[key] = idx;
                    hull.vertices.push_back(v);
                    hull.bounds.extend(v);
                    entityBounds.extend(v);
                    sideIndices.push_back(idx);
                } else {
                    sideIndices.push_back(it->second);
                }
            }

            if (sideIndices.size() >= 3) {
                hull.sideWindings.push_back(sideIndices);
            }
        }

        if (!hull.vertices.empty() && !hull.sideWindings.empty()) {
            hulls.push_back(std::move(hull));
        }
    }

    if (hulls.empty()) {
        return;
    }

    /* compute entity origin (center of all brush geometry) */
    Vector3 entityOrigin = (entityBounds.mins + entityBounds.maxs) * 0.5f;

    /* make all vertex positions relative to entityOrigin */
    for (auto &hull : hulls) {
        hull.bounds = MinMax();
        for (Vector3 &v : hull.vertices) {
            v = v - entityOrigin;
            hull.bounds.extend(v);
        }
    }

    /* ---- compute overall relative bounds for BVH scale ---- */
    MinMax overallBounds;
    for (const auto &h : hulls) {
        overallBounds.extend(h.bounds);
    }

    /* BVH decode: worldPos = origin + (int16 * 65536) * scale
       origin is (0,0,0) in the blob; entityOrigin goes in the "origin" key.
       Scale must be small enough that bounds fit in int16 range. */
    Vector3 absMax(
        std::max(std::abs(overallBounds.mins.x()), std::abs(overallBounds.maxs.x())),
        std::max(std::abs(overallBounds.mins.y()), std::abs(overallBounds.maxs.y())),
        std::max(std::abs(overallBounds.mins.z()), std::abs(overallBounds.maxs.z()))
    );
    float maxCoord = std::max({ absMax.x(), absMax.y(), absMax.z(), 1.0f });
    /* scale so that maxCoord = ~32000 * 65536 * scale */
    float bvhScale = maxCoord / (32000.0f * 65536.0f);

    /* ---- build convex-hull leaf data ---- */
    /* Leaf format (from engine):
       +0x00: uint8 numVerts, numFaces, numTriFaces, numPolyFaces
       +0x04: float origin[3], scale  (hull-local decode for packed int16 verts)
       +0x14: int16[3] * numVerts     (6 bytes each, consecutive)
       Then: face indices (3 bytes per face, padded to 4)
       Then: face polygon data (CollLeafPoly_s)
       Then: uint32 trailing surfProp word */
    std::vector<uint8_t> leafDataBytes;
    std::vector<uint32_t> leafDataOffsets;

    auto pushU8 = [&leafDataBytes](uint8_t val) {
        leafDataBytes.push_back(val);
    };
    auto pushU32 = [&leafDataBytes](uint32_t val) {
        uint8_t b[4];
        memcpy(b, &val, 4);
        leafDataBytes.insert(leafDataBytes.end(), b, b + 4);
    };
    auto pushF32 = [&leafDataBytes](float val) {
        uint8_t b[4];
        memcpy(b, &val, 4);
        leafDataBytes.insert(leafDataBytes.end(), b, b + 4);
    };
    auto pushI16 = [&leafDataBytes](int16_t val) {
        uint8_t b[2];
        memcpy(b, &val, 2);
        leafDataBytes.insert(leafDataBytes.end(), b, b + 2);
    };

    for (const auto &hull : hulls) {
        leafDataOffsets.push_back(static_cast<uint32_t>(leafDataBytes.size()));

        int numVerts = std::min(static_cast<int>(hull.vertices.size()), 255);

        /* Categorize faces and build poly entries.
           Face indices: 1 per brush side (3 verts defining face plane).
           Tri polys: for 3-vert faces + fan-triangulated 5+-vert faces.
           Quad polys: for 4-vert faces (parallelogram encoding). */
        struct PolyEntry { int v0, v1, v2; };

        std::vector<std::array<int, 3>> faceIndices;
        std::vector<PolyEntry> triPolys;
        std::vector<PolyEntry> quadPolys;

        for (const auto &winding : hull.sideWindings) {
            int n = static_cast<int>(winding.size());
            if (n < 3) continue;

            /* Find vertex with minimum index to satisfy unsigned delta encoding */
            int minPos = 0;
            for (int i = 1; i < n; i++)
                if (winding[i] < winding[minPos]) minPos = i;

            /* Radiant windings are CW viewed from outside (outward normal side).
               The engine containment test computes cross(v1-v0, v2-v0) and needs
               it to point OUTWARD.  For CW winding, using {v0, prev, next} gives
               cross(prev-v0, next-v0) = outward normal. */
            if (n == 3) {
                int a = winding[minPos];
                int b = winding[(minPos + 1) % 3];  /* next */
                int c = winding[(minPos + 2) % 3];  /* prev */
                faceIndices.push_back({a, c, b});    /* {min, prev, next} → outward normal */
                triPolys.push_back({a, b, c});       /* poly data: winding order for surface */
            } else if (n == 4) {
                int v0 = winding[minPos];
                int v1 = winding[(minPos + 1) % 4];  /* next */
                int v2 = winding[(minPos + 3) % 4];  /* prev */
                /* derived v3 = winding[(minPos+2)%4] = v2+v1-v0 (parallelogram) */
                faceIndices.push_back({v0, v2, v1});  /* {min, prev, next} → outward normal */
                quadPolys.push_back({v0, v1, v2});    /* poly data unchanged */
            } else {
                /* 5+ vertex polygon: fan-triangulate for polys, face uses prev/next */
                std::vector<int> rot;
                rot.reserve(n);
                for (int i = 0; i < n; i++)
                    rot.push_back(winding[(minPos + i) % n]);
                faceIndices.push_back({rot[0], rot[n - 1], rot[1]});  /* {min, prev, next} */
                for (int i = 1; i + 1 < n; i++)
                    triPolys.push_back({rot[0], rot[i], rot[i + 1]});
            }
        }

        /* Sort polys by v0 for delta encoding */
        std::sort(triPolys.begin(), triPolys.end(),
            [](const PolyEntry &a, const PolyEntry &b) { return a.v0 < b.v0; });
        std::sort(quadPolys.begin(), quadPolys.end(),
            [](const PolyEntry &a, const PolyEntry &b) { return a.v0 < b.v0; });

        int numFaces = std::min(static_cast<int>(faceIndices.size()), 255);
        int numTriSets  = triPolys.empty()  ? 0 : (static_cast<int>(triPolys.size())  + 15) / 16;
        int numQuadSets = quadPolys.empty() ? 0 : (static_cast<int>(quadPolys.size()) + 15) / 16;

        /* header: 4 bytes */
        pushU8(static_cast<uint8_t>(numVerts));
        pushU8(static_cast<uint8_t>(numFaces));
        pushU8(static_cast<uint8_t>(numTriSets));
        pushU8(static_cast<uint8_t>(numQuadSets));

        /* hull-local origin + scale */
        Vector3 hCenter  = (hull.bounds.mins + hull.bounds.maxs) * 0.5f;
        Vector3 hAbsMax(
            std::max(std::abs(hull.bounds.mins.x() - hCenter.x()),
                     std::abs(hull.bounds.maxs.x() - hCenter.x())),
            std::max(std::abs(hull.bounds.mins.y() - hCenter.y()),
                     std::abs(hull.bounds.maxs.y() - hCenter.y())),
            std::max(std::abs(hull.bounds.mins.z() - hCenter.z()),
                     std::abs(hull.bounds.maxs.z() - hCenter.z()))
        );
        float hMaxCoord = std::max({ hAbsMax.x(), hAbsMax.y(), hAbsMax.z(), 1.0f });
        float hScale = hMaxCoord / (32000.0f * 65536.0f);

        pushF32(hCenter.x());
        pushF32(hCenter.y());
        pushF32(hCenter.z());
        pushF32(hScale);

        /* pack vertices as int16[3] (6 bytes each) */
        float invHScale = 1.0f / (hScale * 65536.0f);
        for (int i = 0; i < numVerts; i++) {
            const Vector3 &v = hull.vertices[i];
            auto clampI16 = [](double val) -> int16_t {
                if (val < -32768.0) val = -32768.0;
                if (val >  32767.0) val =  32767.0;
                return static_cast<int16_t>(val);
            };
            pushI16(clampI16((v.x() - hCenter.x()) * invHScale));
            pushI16(clampI16((v.y() - hCenter.y()) * invHScale));
            pushI16(clampI16((v.z() - hCenter.z()) * invHScale));
        }

        /* face indices – 3 bytes per face, padded to 4 */
        for (int i = 0; i < numFaces; i++) {
            pushU8(static_cast<uint8_t>(faceIndices[i][0]));
            pushU8(static_cast<uint8_t>(faceIndices[i][1]));
            pushU8(static_cast<uint8_t>(faceIndices[i][2]));
        }
        while (leafDataBytes.size() % 4 != 0) pushU8(0);

        /* Triangle poly sets (engine HullPolys_4_ format):
           Per-entry: v0delta(11) | d1(9) | d2(9) | reserved(3) */
        {
            int idx = 0;
            for (int s = 0; s < numTriSets; s++) {
                int count = std::min(16, static_cast<int>(triPolys.size()) - idx);
                uint16_t headerLow = static_cast<uint16_t>(((count - 1) & 0xF) << 12);
                pushU32(static_cast<uint32_t>(headerLow));

                uint32_t runBase = 0;
                for (int i = 0; i < count; i++) {
                    const auto &t = triPolys[idx + i];
                    uint32_t v0delta = static_cast<uint32_t>(t.v0) - runBase;
                    uint32_t d1 = static_cast<uint32_t>(t.v1 - t.v0 - 1);
                    uint32_t d2 = static_cast<uint32_t>(t.v2 - t.v0 - 1);
                    pushU32((v0delta & 0x7FF) | ((d1 & 0x1FF) << 11) | ((d2 & 0x1FF) << 20));
                    runBase = static_cast<uint32_t>(t.v0);
                }
                idx += count;
            }
        }

        /* Quad poly sets (engine HullPolys_6_ format):
           Per-entry: v0delta(10) | d1(9) | d2(9) | reserved(4) */
        {
            int idx = 0;
            for (int s = 0; s < numQuadSets; s++) {
                int count = std::min(16, static_cast<int>(quadPolys.size()) - idx);
                uint16_t headerLow = static_cast<uint16_t>(((count - 1) & 0xF) << 12);
                pushU32(static_cast<uint32_t>(headerLow));

                uint32_t runBase = 0;
                for (int i = 0; i < count; i++) {
                    const auto &q = quadPolys[idx + i];
                    uint32_t v0delta = static_cast<uint32_t>(q.v0) - runBase;
                    uint32_t d1 = static_cast<uint32_t>(q.v1 - q.v0 - 1);
                    uint32_t d2 = static_cast<uint32_t>(q.v2 - q.v0 - 1);
                    pushU32((v0delta & 0x3FF) | ((d1 & 0x1FF) << 10) | ((d2 & 0x1FF) << 19));
                    runBase = static_cast<uint32_t>(q.v0);
                }
                idx += count;
            }
        }
    }

    /* pad leaf data to 4-byte alignment */
    while (leafDataBytes.size() % 4 != 0) leafDataBytes.push_back(0);

    /* ---- surface property and name buffers ---- */
    ApexLegends::CollSurfProps_t surfProp{};
    surfProp.surfFlags   = striEqual(cls, "envmap_volume") ? 0x0610 : 0x0400;
    surfProp.surfTypeID  = 0;
    surfProp.contentsIdx = 0;
    surfProp.nameOffset  = 0;

    std::vector<uint8_t> surfNameBuf(surfaceName.begin(), surfaceName.end());
    surfNameBuf.push_back(0);
    while (surfNameBuf.size() % 4 != 0) surfNameBuf.push_back(0);

    /* ---- calculate blob layout (matching official order) ---- */
    /* Official layout:
       0x00: header (0x10 + numParts * 0x20)
       Then: surfProps, contentsMask, surfNameBuf  (at fixed offsets 0x30, 0x38, 0x3C for 1 part)
       Then: leaf data
       Then: BVH nodes (64-byte aligned) */
    const uint32_t headerSize = 0x10 + 1 * 0x20;  // 0x30

    uint32_t surfPropsOfs    = headerSize;                                          // 0x30
    uint32_t contentsMaskOfs = surfPropsOfs + sizeof(ApexLegends::CollSurfProps_t); // 0x38
    uint32_t surfNameBufOfs  = contentsMaskOfs + sizeof(uint32_t);                  // 0x3C
    uint32_t surfNameBufSize = static_cast<uint32_t>(surfNameBuf.size());

    uint32_t leafDataOfs  = surfNameBufOfs + surfNameBufSize;
    uint32_t leafDataSize = static_cast<uint32_t>(leafDataBytes.size());

    /* BVH nodes: 64-byte aligned after leaf data */
    uint32_t numNodes = 1;
    if (hulls.size() > 4) numNodes = 2;  /* won't happen, capped at 4 */
    uint32_t nodesOfs = (leafDataOfs + leafDataSize + 63) & ~63u;
    uint32_t nodesSize = numNodes * sizeof(ApexLegends::BVHNode_t);

    uint32_t totalBlobSize = nodesOfs + nodesSize;
    /* round up to multiple of 4 */
    totalBlobSize = (totalBlobSize + 3) & ~3u;

    /* ---- build BVH node(s) ---- */
    ApexLegends::BVHNode_t bvhNode;
    memset(&bvhNode, 0, sizeof(bvhNode));

    float invBvhScale = 1.0f / (bvhScale * 65536.0f);

    /* Compute child-0 bounds first (always present) */
    int16_t c0bounds[6]; /* minX, maxX, minY, maxY, minZ, maxZ */
    {
        const MinMax &hb = hulls[0].bounds;
        auto clampI16 = [](double v) -> int16_t {
            if (v < -32768.0) v = -32768.0;
            if (v >  32767.0) v =  32767.0;
            return static_cast<int16_t>(v);
        };
        c0bounds[0] = clampI16(std::floor(hb.mins.x() * invBvhScale));
        c0bounds[1] = clampI16(std::ceil (hb.maxs.x() * invBvhScale));
        c0bounds[2] = clampI16(std::floor(hb.mins.y() * invBvhScale));
        c0bounds[3] = clampI16(std::ceil (hb.maxs.y() * invBvhScale));
        c0bounds[4] = clampI16(std::floor(hb.mins.z() * invBvhScale));
        c0bounds[5] = clampI16(std::ceil (hb.maxs.z() * invBvhScale));
    }

    for (int c = 0; c < 4; c++) {
        int16_t bMinX, bMaxX, bMinY, bMaxY, bMinZ, bMaxZ;
        if (c < static_cast<int>(hulls.size())) {
            const MinMax &hb = hulls[c].bounds;
            auto clampI16 = [](double v) -> int16_t {
                if (v < -32768.0) v = -32768.0;
                if (v >  32767.0) v =  32767.0;
                return static_cast<int16_t>(v);
            };
            bMinX = clampI16(std::floor(hb.mins.x() * invBvhScale));
            bMaxX = clampI16(std::ceil (hb.maxs.x() * invBvhScale));
            bMinY = clampI16(std::floor(hb.mins.y() * invBvhScale));
            bMaxY = clampI16(std::ceil (hb.maxs.y() * invBvhScale));
            bMinZ = clampI16(std::floor(hb.mins.z() * invBvhScale));
            bMaxZ = clampI16(std::ceil (hb.maxs.z() * invBvhScale));
        } else {
            /* unused child – flip child 0's bounds (min>max prevents traversal) */
            bMinX = c0bounds[1]; bMaxX = c0bounds[0];
            bMinY = c0bounds[3]; bMaxY = c0bounds[2];
            bMinZ = c0bounds[5]; bMaxZ = c0bounds[4];
        }
        bvhNode.bounds[c]      = bMinX;
        bvhNode.bounds[4  + c] = bMaxX;
        bvhNode.bounds[8  + c] = bMinY;
        bvhNode.bounds[12 + c] = bMaxY;
        bvhNode.bounds[16 + c] = bMinZ;
        bvhNode.bounds[20 + c] = bMaxZ;
    }

    /* child types and leaf-data indices (DWORD offset into leaf data) */
    for (int c = 0; c < 4; c++) {
        int childType, childIdx;
        if (c < static_cast<int>(hulls.size())) {
            childType = BVH_CHILD_CONVEXHULL;
            childIdx  = static_cast<int>(leafDataOffsets[c] / 4);
        } else {
            childType = BVH_CHILD_NONE;
            childIdx  = 0;
        }
        switch (c) {
            case 0: bvhNode.childType0 = childType; bvhNode.index0 = childIdx; break;
            case 1: bvhNode.childType1 = childType; bvhNode.index1 = childIdx; break;
            case 2: bvhNode.childType2 = childType; bvhNode.index2 = childIdx; break;
            case 3: bvhNode.childType3 = childType; bvhNode.index3 = childIdx; break;
        }
    }

    bvhNode.cmIndex = 0;

    /* ---- assemble the blob ---- */
    std::vector<uint8_t> blob(totalBlobSize, 0);

    auto wr32 = [&blob](uint32_t ofs, uint32_t v) { memcpy(blob.data()+ofs, &v, 4); };
    auto wrf  = [&blob](uint32_t ofs, float   v) { memcpy(blob.data()+ofs, &v, 4); };

    /* header */
    wr32(0x00, contentsMaskOfs);
    wr32(0x04, surfPropsOfs);
    wr32(0x08, surfNameBufOfs);
    wr32(0x0C, 1);

    /* part 0 */
    wr32(0x10, 1);                         // bvhFlags = 1 (packed int16 verts)
    wr32(0x14, nodesOfs);
    wr32(0x18, leafDataOfs);
    wr32(0x1C, leafDataOfs);
    wrf (0x20, 0.0f);                     // origin = (0,0,0) in blob
    wrf (0x24, 0.0f);
    wrf (0x28, 0.0f);
    wrf (0x2C, bvhScale);

    /* payload */
    memcpy(blob.data() + surfPropsOfs,    &surfProp, sizeof(surfProp));
    wr32(contentsMaskOfs, contentsMask);
    memcpy(blob.data() + surfNameBufOfs,  surfNameBuf.data(), surfNameBufSize);
    memcpy(blob.data() + leafDataOfs,     leafDataBytes.data(), leafDataSize);
    memcpy(blob.data() + nodesOfs,        &bvhNode, sizeof(bvhNode));

    /* ---- base64 encode ---- */
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string base64;
    base64.reserve((totalBlobSize + 2) / 3 * 4);

    for (uint32_t i = 0; i < totalBlobSize; i += 3) {
        uint32_t n = static_cast<uint32_t>(blob[i]) << 16;
        if (i + 1 < totalBlobSize) n |= static_cast<uint32_t>(blob[i + 1]) << 8;
        if (i + 2 < totalBlobSize) n |= static_cast<uint32_t>(blob[i + 2]);

        base64 += b64[(n >> 18) & 0x3F];
        base64 += b64[(n >> 12) & 0x3F];
        base64 += (i + 1 < totalBlobSize) ? b64[(n >>  6) & 0x3F] : '=';
        base64 += (i + 2 < totalBlobSize) ? b64[(n      ) & 0x3F] : '=';
    }

    /* ---- split into *coll key-value pairs ---- */
    constexpr size_t COLL_LINE_LEN = 160;

    int lineNum = 0;
    for (size_t pos = 0; pos < base64.size(); pos += COLL_LINE_LEN) {
        size_t len = std::min(COLL_LINE_LEN, base64.size() - pos);
        char keyBuf[16];
        snprintf(keyBuf, sizeof(keyBuf), "*coll%d", lineNum);
        entity.setKeyValue(keyBuf, base64.substr(pos, len).c_str());
        lineNum++;
    }

    Sys_FPrintf(SYS_VRB,
        "  Serialized %u bytes collision (%zu hulls) into %d *coll keys for %s\n",
        totalBlobSize, hulls.size(), lineNum, entity.classname());

    /* set origin key from brush geometry center */
    char originBuf[128];
    snprintf(originBuf, sizeof(originBuf), "%.1f %.1f %.1f",
             entityOrigin.x(), entityOrigin.y(), entityOrigin.z());
    entity.setKeyValue("origin", originBuf);

    /* set mins/maxs keys (local-space bounds relative to origin) */
    char minsBuf[128], maxsBuf[128];
    Vector3 localMins = entityBounds.mins - entityOrigin;
    Vector3 localMaxs = entityBounds.maxs - entityOrigin;
    snprintf(minsBuf, sizeof(minsBuf), "%f %f %f",
             localMins.x(), localMins.y(), localMins.z());
    snprintf(maxsBuf, sizeof(maxsBuf), "%f %f %f",
             localMaxs.x(), localMaxs.y(), localMaxs.z());
    entity.setKeyValue("mins", minsBuf);
    entity.setKeyValue("maxs", maxsBuf);
}