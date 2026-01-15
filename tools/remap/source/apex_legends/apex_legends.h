/* -------------------------------------------------------------------------------

   Copyright (C) 2022-2023 MRVN-Radiant and contributors.
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

#include "../titanfall2/titanfall2.h"
#include "qmath.h"
#include <cstdint>
#include "../remap.h"
#include "../lump_names.h"

void LoadR5BSPFile(rbspHeader_t *header, const char *filename);
void WriteR5BSPFile(const char *filename);
void CompileR5BSPFile();

// BVH Node child types (from IDA reverse engineering of CollBvh_VisitLeafs)
#define BVH_CHILD_NODE        0   // Internal node - traverse further
#define BVH_CHILD_NONE        1   // Empty child slot
#define BVH_CHILD_EMPTY       2   // Empty leaf
#define BVH_CHILD_BUNDLE      3   // Bundle of collision primitives
#define BVH_CHILD_TRISTRIP    4   // Triangle strip
#define BVH_CHILD_POLY3       5   // Triangle polygon
#define BVH_CHILD_POLY4       6   // Quad polygon
#define BVH_CHILD_POLY5PLUS   7   // 5+ vertex polygon
#define BVH_CHILD_CONVEXHULL  8   // Convex hull (used for brush collision)
#define BVH_CHILD_STATICPROP  9   // Static prop collision
#define BVH_CHILD_HEIGHTFIELD 10  // Heightfield terrain collision

namespace ApexLegends {
    void        EmitStubs();

    void        SetupGameLump();
    void        EmitStaticProp(entity_t &e);
    void        EmitEntity(const entity_t &e);
    void        BeginModel(entity_t &entity);
    void        EndModel();
    void        EmitBVHNode();
    int         EmitBVHDataleaf();
    int         EmitContentsMask( int mask );
    void        EmitMeshes(const entity_t &e);
    uint32_t    EmitTextureData(shaderInfo_t shader);
    uint16_t    EmitMaterialSort(uint32_t index, int offset, int count);
    void        EmitVertexUnlit(Shared::Vertex_t &vertex);
    void        EmitVertexLitFlat(Shared::Vertex_t &vertex);
    void        EmitVertexLitBump(Shared::Vertex_t &vertex);
    void        EmitVertexUnlitTS(Shared::Vertex_t &vertex);
    std::size_t EmitObjReferences(Shared::visNode_t &node);
    int         EmitVisChildrenOfTreeNode(Shared::visNode_t node);
    void        EmitVisTree();
    void        EmitLevelInfo();

    using Vertex_t = Vector3;

    using VertexNormal_t = Vector3;

    // 0x02
    struct TextureData_t {
        uint32_t  surfaceIndex;
        uint32_t  sizeX;
        uint32_t  sizeY;
        uint32_t  flags;
    };

    // 0x0F - dmodel_t from IDA (64 bytes)
    struct Model_t {
        MinMax    minmax;         // +0x00: mins/maxs (24 bytes)
        int32_t   meshIndex;      // +0x18: firstMesh
        int32_t   meshCount;      // +0x1C: meshCount
        int32_t   bvhNodeIndex;   // +0x20: nodeStart - index into BVH nodes
        int32_t   bvhLeafIndex;   // +0x24: leafDataStart - index into leaf data
        int32_t   vertexIndex;    // +0x28: vertStart - index into collision verts
        int32_t   bvhFlags;       // +0x2C: bvhFlags (& 1 = use packed vertices)
        float     origin[3];      // +0x30: origin for BVH bounds decoding
        float     scale;          // +0x3C: scale for BVH bounds decoding
    };
    static_assert(sizeof(Model_t) == 64, "Model_t must be exactly 64 bytes");

    // 0x12 - CollBvh4Node_s from IDA (64 bytes)
    // Layout: bounds[24] (48 bytes) + packedMetaData[4] (16 bytes)
    // packedMetaData[0]: cmIndex(8) | index0(24)
    // packedMetaData[1]: padding(8) | index1(24)
    // packedMetaData[2]: childType0(4) | childType1(4) | index2(24)
    // packedMetaData[3]: childType2(4) | childType3(4) | index3(24)
    struct BVHNode_t {
        int16_t   bounds[24];     // +0x00: 48 bytes of packed bounds

        int32_t   cmIndex : 8;    // +0x30: contents mask index
        int32_t   index0 : 24;    // +0x30: child 0 index (upper 24 bits)

        int32_t   padding : 8;    // +0x34: unused
        int32_t   index1 : 24;    // +0x34: child 1 index (upper 24 bits)

        int32_t   childType0 : 4; // +0x38: child 0 type (low nibble)
        int32_t   childType1 : 4; // +0x38: child 1 type (high nibble)
        int32_t   index2 : 24;    // +0x38: child 2 index (upper 24 bits)

        int32_t   childType2 : 4; // +0x3C: child 2 type (low nibble)
        int32_t   childType3 : 4; // +0x3C: child 3 type (high nibble)
        int32_t   index3 : 24;    // +0x3C: child 3 index (upper 24 bits)
    };
    static_assert(sizeof(BVHNode_t) == 64, "BVHNode_t must be exactly 64 bytes");

    // 0x47
    struct VertexUnlit_t {
        uint32_t  vertexIndex;
        uint32_t  normalIndex;
        Vector2   uv0;
        int32_t   negativeOne;
    };

    // 0x48
    struct VertexLitFlat_t {
        uint32_t  vertexIndex;
        uint32_t  normalIndex;
        Vector2   uv0;
        int32_t   unknown0;
    };

    // 0x49
    struct VertexLitBump_t {
        uint32_t  vertexIndex;
        uint32_t  normalIndex;
        Vector2   uv0;
        int32_t   negativeOne;
        Vector2   uv1;
        uint8_t   color[4];
    };

    // 0x4A
    struct VertexUnlitTS_t {
        uint32_t  vertexIndex;
        uint32_t  normalIndex;
        Vector2   uv0;
        int32_t   unknown0;
        int32_t   unknown1;
    };

    // 0x4B
    struct VertexBlinnPhong_t {
        uint32_t  vertexIndex;
        uint32_t  normalIndex;
        Vector2   uv0;
        Vector2   uv1;
    };

    // 0x50
    struct Mesh_t {
        uint32_t  triOffset;
        uint16_t  triCount;
        uint16_t  unknown[8];
        uint16_t  materialOffset;
        uint32_t  flags;
    };

    // 0x52
    struct MaterialSort_t {
        uint16_t  textureData;
        int16_t  lightmapIndex;
        uint16_t  unknown0;
        uint16_t  unknown1;
        uint32_t  vertexOffset;
    };

    // 0x77
    struct CellAABBNode_t {
        Vector3   mins;
        uint32_t  childCount : 8;
        uint32_t  firstChild : 16;
        uint32_t  childFlags : 8;
        Vector3   maxs;
        uint32_t  objRefCount : 8;
        uint32_t  objRefOffset : 16;
        uint32_t  objRefFlags : 8;
        
        CellAABBNode_t() : mins(), childCount(0), firstChild(0), childFlags(0),
                          maxs(), objRefCount(0), objRefOffset(0), objRefFlags(0) {}
    };

    // 0x7B
    struct LevelInfo_t {
        int32_t  unk0;
        int32_t  unk1;
        int32_t  unk2;
        int32_t  unk3;
        int32_t  unk4;
        float    unk5[3];
        int32_t  modelCount;
    };


    // GameLump Stub
    struct GameLump_Stub_t {
        uint32_t  version = 1;
        char      magic[4];
        uint32_t  const0 = 3080192;
        uint32_t  offset;
        uint32_t  length = 20;
        uint32_t  zeros[5] = {0, 0, 0, 0, 0};
    };

    // Packed collision vertex (6 bytes = 3 int16s)
    // Game decodes as: worldPos = origin + (int16 << 16) * scale
    #pragma pack(push, 1)
    struct PackedVertex_t {
        int16_t x, y, z;
    };
    #pragma pack(pop)
    static_assert(sizeof(PackedVertex_t) == 6, "PackedVertex_t must be exactly 6 bytes");

    // Float collision vertex (12 bytes = float3)
    // Used when bvhFlags & 1 == 0 (Type 4 TriStrip leaves)
    // Referenced directly as float3 from bvh->verts
    struct CollisionVertex_t {
        float x, y, z;
    };
    static_assert(sizeof(CollisionVertex_t) == 12, "CollisionVertex_t must be exactly 12 bytes");

    namespace Bsp {
        inline std::vector<TextureData_t>       textureData;
        inline std::vector<Model_t>             models;
        inline std::vector<int32_t>             contentsMasks;
        inline std::vector<BVHNode_t>           bvhNodes;
        inline std::vector<int32_t>             bvhLeafDatas;
        inline std::vector<PackedVertex_t>      packedVertices;      // Packed collision verts (lump 0x14), bvhFlags & 1
        inline std::vector<CollisionVertex_t>   collisionVertices;   // Float collision verts (lump 0x03), bvhFlags == 0
        inline std::vector<VertexUnlit_t>       vertexUnlitVertices;
        inline std::vector<VertexLitFlat_t>     vertexLitFlatVertices;
        inline std::vector<VertexLitBump_t>     vertexLitBumpVertices;
        inline std::vector<VertexUnlitTS_t>     vertexUnlitTSVertices;
        inline std::vector<VertexBlinnPhong_t>  vertexBlinnPhongVertices;
        inline std::vector<uint16_t>            meshIndices;
        inline std::vector<Mesh_t>              meshes;
        inline std::vector<MaterialSort_t>      materialSorts;
        inline std::vector<CellAABBNode_t>      cellAABBNodes;
        inline std::vector<int32_t>             objReferences;
        inline std::vector<LevelInfo_t>         levelInfo;

        // Stubs
        inline std::vector<uint8_t>  lightprobeParentInfos_stub;
        inline std::vector<uint8_t>  shadowEnvironments_stub;
        inline std::vector<uint8_t>  surfaceProperties_stub;
        inline std::vector<uint8_t>  unknown25_stub;
        inline std::vector<uint8_t>  unknown26_stub;
        inline std::vector<uint8_t>  unknown27_stub;
        inline std::vector<uint8_t>  cubemaps_stub;
        inline std::vector<uint8_t>  worldLights_stub;
        inline std::vector<uint8_t>  lightmapHeaders_stub;
        inline std::vector<uint8_t>  tweakLights_stub;
        inline std::vector<uint8_t>  lightmapDataSky_stub;
        inline std::vector<uint8_t>  csmAABBNodes_stub;
        inline std::vector<uint8_t>  lightmapDataRTLPage_stub;
    }
}
