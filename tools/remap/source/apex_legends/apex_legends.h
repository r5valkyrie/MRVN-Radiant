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
#include <cmath>
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

// Light emit types (from decompiled code)
enum emittype_t : int32_t {
    emit_surface = 0,
    emit_point = 1,
    emit_spotlight = 2,
    emit_skylight = 3,
    emit_quakelight = 4,
    emit_skyambient = 5
};

// World light flags
constexpr int32_t WORLDLIGHT_FLAG_REALTIME         = 0x01;  // Realtime light (dynamic)
constexpr int32_t WORLDLIGHT_FLAG_REALTIME_SHADOWS = 0x02;  // Casts realtime shadows
constexpr int32_t WORLDLIGHT_FLAG_PBR_FALLOFF      = 0x04;  // Uses PBR falloff curve
constexpr int32_t WORLDLIGHT_FLAG_TWEAK            = 0x80;  // Can be adjusted at runtime (tweak light)

// 0x36 (54) - dworldlight_t (112 bytes)
// World lights for dynamic lighting in the BSP
// Layout verified from IDA reverse engineering
#pragma pack(push, 1)
struct WorldLight_t {
    Vector3  origin;                          // +0x00 - light position (12 bytes)
    Vector3  intensity;                       // +0x0C - RGB intensity (12 bytes)
    Vector3  normal;                          // +0x18 - light direction (12 bytes)
    int32_t  shadowUpresFactor;               // +0x24 - shadow upres factor
    int32_t  shadowFilterSize;                // +0x28 - shadow filter size
    float    shadowBias;                      // +0x2C - shadow map bias
    float    bounceBoost;                     // +0x30 - bounce boost for GI
    int32_t  type;                            // +0x34 - light type (emittype_t)
    int32_t  style;                           // +0x38 - light style
    float    stopdot;                         // +0x3C - inner cone cosine
    float    stopdot2;                        // +0x40 - penumbra cosine (outer cone)
    float    exponent;                        // +0x44 - spotlight exponent
    float    radius;                          // +0x48 - light radius
    float    constant_attn;                   // +0x4C - constant attenuation
    float    linear_attn;                     // +0x50 - linear attenuation
    float    quadratic_attn;                  // +0x54 - quadratic attenuation
    int32_t  flags;                           // +0x58 - light flags
    int32_t  texdata;                         // +0x5C - texture data index
    int32_t  owner;                           // +0x60 - owner entity index
    float    emitter_radius;                  // +0x64 - emitter radius
    float    sun_highlight_size_or_vfog_boost;// +0x68 - sun highlight size (skylight) or vfog boost
    float    specular_intensity;              // +0x6C - specular intensity
};
#pragma pack(pop)
static_assert(sizeof(WorldLight_t) == 112, "WorldLight_t must be exactly 112 bytes");

// Shadow Environment structure (lump 0x05)
// One entry per light_environment, contains sun direction for cascaded shadow maps
#pragma pack(push, 1)
struct ShadowEnvironment_t {
    uint32_t beginAabbs;         // +0x00 - start index into CSM AABB nodes
    uint32_t beginObjRefs;       // +0x04 - start index into CSM obj references
    uint32_t beginShadowMeshes;  // +0x08 - start index into shadow meshes
    uint32_t endAabbs;           // +0x0C - end index into CSM AABB nodes
    uint32_t endObjRefs;         // +0x10 - end index into CSM obj references
    uint32_t endShadowMeshes;    // +0x14 - end index into shadow meshes
    Vector3  shadowDir;          // +0x18 - normalized sun direction vector (direction light travels, FROM sun towards ground)
};
#pragma pack(pop)
static_assert(sizeof(ShadowEnvironment_t) == 36, "ShadowEnvironment_t must be exactly 36 bytes");

// Shadow Mesh structure (lump 0x7F) - 12 bytes
// References shadow mesh vertices and indices for shadow casting
#pragma pack(push, 1)
struct ShadowMesh_t {
    uint32_t firstVertex;        // +0x00 - first vertex index (in opaque or alpha verts)
    uint32_t triangleCount;      // +0x04 - number of triangles
    uint16_t drawType;           // +0x08 - 0=world, 1=opaque, etc.
    uint16_t materialSortIdx;    // +0x0A - material sort index (for alpha tested)
};
#pragma pack(pop)
static_assert(sizeof(ShadowMesh_t) == 12, "ShadowMesh_t must be exactly 12 bytes");

// Shadow Mesh Opaque Vertex (lump 0x7C) - 12 bytes, just position
using ShadowMeshOpaqueVertex_t = Vector3;

// Shadow Mesh Alpha Vertex (lump 0x7D) - 20 bytes, position + tex coords
#pragma pack(push, 1)
struct ShadowMeshAlphaVertex_t {
    Vector3  position;           // +0x00 - vertex position
    uint32_t texCoord[2];        // +0x0C - packed texture coordinates
};
#pragma pack(pop)
static_assert(sizeof(ShadowMeshAlphaVertex_t) == 20, "ShadowMeshAlphaVertex_t must be exactly 20 bytes");

// Light Probe structure (lump 0x65) - 48 bytes
// Used for BSP v47+ (Apex Legends). Verified against official map lump sizes:
//   - mp_rr_canyonlands: 28352592 bytes / 48 = 590679 probes (exact)
//   - mp_rr_desertlands: 33877392 bytes / 48 = 705779 probes (exact)
// Stores ambient lighting as L1 spherical harmonics + references to static lights
// Used by Mod_LeafAmbientColorAtPosWithNormal for ambient lighting lookups
//
// SH coefficient format per channel:
//   [0] = X gradient (posX - negX directional difference)
//   [1] = Y gradient (posY - negY directional difference)  
//   [2] = Z gradient (posZ - negZ directional difference)
//   [3] = DC term (average ambient)
//
// Scale factor: Game multiplies int16 SH values by 0.00012207031 (1/8192)
// This means int16 value of 8192 (0x2000) = 1.0 linear light intensity
// The game's default probe uses 0x2000 for DC terms when no probe data exists
//
// Official map analysis shows:
//   - DC range: 0 to ~8192 for normally lit areas (can exceed for HDR)
//   - Gradient range: -8192 to +8192 
//   - lightingFlags (offset 36): usually 0x0096 (150 decimal)
//   - Padding0 (offset 40): usually 0xFFFFFFFF for most probes
//   - Padding1 (offset 44): always 0x00000000
#pragma pack(push, 1)
struct LightProbe_t {
    int16_t  ambientSH[3][4];       // +0x00 - Spherical harmonics for R, G, B (24 bytes)
    uint16_t staticLightIndexes[4]; // +0x18 - Indices into worldLights (8 bytes)
                                    //         Stored as: actualIndex + (32 - numLightEnvs)
                                    //         Game retrieves: storedIndex - (32 - numLightEnvs)
                                    //         0xFFFF = no light in this slot
    uint8_t  staticLightFlags[4];   // +0x20 - Weight/flags for each static light (4 bytes)
                                    //         Game calls this "staticLightWeights"
                                    //         0xFF = valid light, 0x00 = no light
                                    //         CRITICAL: Game breaks loop when weight[slot] == 0
                                    //         So EACH valid slot must have non-zero weight!
    uint16_t lightingFlags;         // +0x24 - Lighting flags (usually 0x0096 = 150)
    uint16_t reserved;              // +0x26 - Reserved (usually 0xFFFF)
    uint32_t padding0;              // +0x28 - Padding (usually 0xFFFFFFFF)
    uint32_t padding1;              // +0x2C - Padding (always 0x00000000)
};
#pragma pack(pop)
static_assert(sizeof(LightProbe_t) == 48, "LightProbe_t must be exactly 48 bytes");

// Light Probe Reference (lump 0x68) - 20 bytes  
// References a light probe in the spatial lookup tree
#pragma pack(push, 1)
struct LightProbeRef_t {
    Vector3  origin;             // +0x00 - Position of the probe (12 bytes)
    uint32_t lightProbeIndex;    // +0x0C - Index into lightprobes lump
    int16_t  cubemapID;          // +0x10 - Cubemap index (-1 = invalid)
    int16_t  padding;            // +0x12 - Padding/unknown
};
#pragma pack(pop)
static_assert(sizeof(LightProbeRef_t) == 20, "LightProbeRef_t must be exactly 20 bytes");

// Light Probe Tree Node (lump 0x67) - 8 bytes
// Binary tree for spatial light probe lookup
// Tag format: (index << 2) | type
//   type 0: internal node, split on X axis
//   type 1: internal node, split on Y axis
//   type 2: internal node, split on Z axis
//   type 3: leaf node pointing to LightProbeRef range
#pragma pack(push, 1)
struct LightProbeTree_t {
    uint32_t tag;                // +0x00 - Encoded type and child/ref index
    union {
        float    splitValue;     // +0x04 - Split plane coordinate (for internal nodes, type 0-2)
        uint32_t refCount;       // +0x04 - Number of probe refs (for leaf nodes, type 3)
    };
};
#pragma pack(pop)
static_assert(sizeof(LightProbeTree_t) == 8, "LightProbeTree_t must be exactly 8 bytes");

// Cubemap Sample (lump 0x2A) - 16 bytes
// Defines positions where cubemaps should be captured
// The engine's buildcubemaps command uses these to create environment maps
// NOTE: Origin is stored as int32[3] in the lump, engine converts to float
#pragma pack(push, 1)
struct CubemapSample_t {
    int32_t  origin[3];          // +0x00 - Position for cubemap capture (as int32, converted to float by engine)
    uint32_t guid;               // +0x0C - Asset GUID (0 if not pre-baked, runtime will generate)
};
#pragma pack(pop)
static_assert(sizeof(CubemapSample_t) == 16, "CubemapSample_t must be exactly 16 bytes");

// Light Probe Parent Info (lump 0x04) - 28 bytes
// Associates light probes with brush models (0 = worldspawn)
#pragma pack(push, 1)
struct LightProbeParentInfo_t {
    uint32_t brushIdx;           // +0x00 - Brush model index (0 for worldspawn)
    uint32_t cubemapIdx;         // +0x04 - Associated cubemap index
    uint32_t lightProbeCount;    // +0x08 - Number of light probes for this model
    uint32_t firstLightProbeRef; // +0x0C - First reference index
    uint32_t lightProbeTreeHead; // +0x10 - Root of probe tree
    uint32_t lightProbeTreeNodeCount; // +0x14 - Number of tree nodes
    uint32_t lightProbeRefCount; // +0x18 - Number of references
};
#pragma pack(pop)
static_assert(sizeof(LightProbeParentInfo_t) == 28, "LightProbeParentInfo_t must be exactly 28 bytes");

// CSM AABB Node (lump 0x63) - 32 bytes
// Bounding volume hierarchy for cascaded shadow map culling
#pragma pack(push, 1)
struct CSMAABBNode_t {
    Vector3  mins;               // +0x00 - AABB minimum
    uint32_t child0;             // +0x0C - child node or data index
    Vector3  maxs;               // +0x10 - AABB maximum
    uint32_t child1;             // +0x1C - child node or data index
};
#pragma pack(pop)
static_assert(sizeof(CSMAABBNode_t) == 32, "CSMAABBNode_t must be exactly 32 bytes");

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
    uint16_t    EmitMaterialSort(uint32_t index, int offset, int count, int16_t lightmapIdx);
    void        EmitVertexUnlit(Shared::Vertex_t &vertex);
    void        EmitVertexLitFlat(Shared::Vertex_t &vertex);
    void        EmitVertexLitBump(Shared::Vertex_t &vertex, const Vector2 &lightmapUV);
    void        EmitVertexUnlitTS(Shared::Vertex_t &vertex);
    std::size_t EmitObjReferences(Shared::visNode_t &node);
    int         EmitVisChildrenOfTreeNode(Shared::visNode_t node);
    void        EmitVisTree();
    void        EmitLevelInfo();
    void        EmitWorldLights();
    void        EmitCubemaps();
    void        EmitShadowEnvironments();
    void        EmitShadowMeshes();
    void        EmitLightmaps();
    void        SetupSurfaceLightmaps();
    void        ComputeLightmapLighting();
    
    // Light probe system - generates ambient lighting data for the map
    // Light probes store spherical harmonics for ambient + references to static lights
    void        EmitLightProbes();
    void        EmitLightProbeTree();
    void        EmitRealTimeLightmaps();
    
    // Lightmap UV lookup - returns UV in [0,1] range for the lightmap atlas
    // Returns false if mesh has no lightmap allocation
    bool        GetLightmapUV(int meshIndex, const Vector3 &worldPos, Vector2 &outUV);

    // Get the lightmap page index for a mesh
    // Returns 0 (default page) for non-lit meshes - all meshes have a valid lightmap index
    int16_t     GetLightmapPageIndex(int meshIndex);

    /*
        vector3_from_angles
        Converts Source engine style angles (pitch, yaw, roll) to a direction vector
        Pitch: rotation around X axis (degrees), positive = down
        Yaw: rotation around Z axis (degrees)
        Roll: rotation around Y axis (degrees) - not used for direction
    */
    inline Vector3 vector3_from_angles(const Vector3& angles) {
        // Convert to radians
        const float pitch = degrees_to_radians(angles.x());
        const float yaw = degrees_to_radians(angles.y());

        // Calculate direction vector
        // Source engine: pitch down is positive, so we negate the Z component
        const float cp = std::cos(pitch);
        return Vector3(
            cp * std::cos(yaw),
            cp * std::sin(yaw),
            -std::sin(pitch)
        );
    }

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

    // 0x11 - CollSurfProps_s (8 bytes)
    // Surface properties for collision system
    // References Surface Names lump (0x0F) via nameOffset
    // References Contents Masks lump (0x10) via contentsIdx
    // From IDA reverse engineering of Coll_ExpandQueryResultProperties
    #pragma pack(push, 1)
    struct CollSurfProps_t {
        uint16_t surfFlags;      // +0x00: Surface flags (SURF_* flags)
        uint8_t  surfTypeID;     // +0x02: Surface type ID (for footsteps, impacts, decals)
        uint8_t  contentsIdx;    // +0x03: Index into Contents Masks lump
        uint32_t nameOffset;     // +0x04: Offset into Surface Names lump
    };
    #pragma pack(pop)
    static_assert(sizeof(CollSurfProps_t) == 8, "CollSurfProps_t must be exactly 8 bytes");

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
        uint32_t  normalIndex2;  // Second normal index with 0x80000000 flag
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

    // 0x50 - Apex Legends mesh structure (28 bytes) - dmesh_t in engine
    // Vertex types: 0=UNLIT, 1=LIT_FLAT, 2=LIT_BUMP, 3=UNLIT_TS
    struct Mesh_t {
        uint32_t  triOffset;
        uint16_t  triCount;
        uint16_t  unknown[8];
        uint16_t  materialOffset;
        uint32_t  flags;
    };
    static_assert(sizeof(Mesh_t) == 28, "Mesh_t must be exactly 28 bytes");

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

    // 0x53 - Lightmap Header (8 bytes)
    // Engine parses this in Mod_LoadLightmapHeaders
    // Format types determine bytes per pixel in lightmap data:
    //   Type 1/10: 8 bytes per pixel (HDR)
    //   Type 4/8:  BC block compression 4x4
    //   Type 5:    ASTC 5x5 blocks
    //   Type 6:    ASTC 6x6 blocks
    //   Type 7:    ASTC 8x8 blocks
    //   Type 9:    12 bytes per pixel
    #pragma pack(push, 1)
    struct LightmapHeader_t {
        uint8_t   type;            // 0x00: lightmap format type
        uint8_t   compressedType;  // 0x01: compressed format type
        uint8_t   tag;             // 0x02: tag
        uint8_t   unknown;         // 0x03: padding/unknown
        uint16_t  width;           // 0x04: lightmap width
        uint16_t  height;          // 0x06: lightmap height
    };
    #pragma pack(pop)
    static_assert(sizeof(LightmapHeader_t) == 8, "LightmapHeader_t must be exactly 8 bytes");

    // Lightmap format types
    enum class LightmapType : uint8_t {
        HDR_8BPP       = 1,   // 8 bytes per pixel (uncompressed HDR)
        BC_4X4_A       = 4,   // BC block compression 4x4
        ASTC_5X5       = 5,   // ASTC 5x5 blocks
        ASTC_6X6       = 6,   // ASTC 6x6 blocks
        ASTC_8X8       = 7,   // ASTC 8x8 blocks  
        BC_4X4_B       = 8,   // BC block compression 4x4
        HDR_12BPP      = 9,   // 12 bytes per pixel
        HDR_8BPP_ALT   = 10,  // 8 bytes per pixel alternate
    };

    // Per-lightmap page data during building
    struct LightmapPage_t {
        uint16_t             width;
        uint16_t             height;
        std::vector<uint8_t> pixels;      // 8 bytes per pixel for Type 1
    };

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
        inline std::vector<ShadowEnvironment_t> shadowEnvironments;
        inline std::vector<WorldLight_t>        worldLights;
        inline std::vector<uint32_t>            tweakLights;              // Lump 0x55 - indices of tweakable lights

        // Shadow mesh lumps (0x7C-0x7F)
        inline std::vector<ShadowMeshOpaqueVertex_t> shadowMeshOpaqueVerts;  // Lump 0x7C
        inline std::vector<ShadowMeshAlphaVertex_t>  shadowMeshAlphaVerts;   // Lump 0x7D
        inline std::vector<uint16_t>                 shadowMeshIndices;      // Lump 0x7E
        inline std::vector<ShadowMesh_t>             shadowMeshes;           // Lump 0x7F
        
        // CSM lumps (0x26, 0x63-0x64)
        inline std::vector<CSMAABBNode_t>            csmAABBNodes;           // Lump 0x63
        inline std::vector<uint32_t>                 csmObjRefsTotal;        // Lump 0x64
        inline std::vector<uint32_t>                 csmNumObjRefsTotalForAabb;  // Lump 0x26 - cumulative obj ref count per node

        // Lightmap lumps (0x53, 0x55, 0x5A, 0x56)
        inline std::vector<LightmapHeader_t>    lightmapHeaders;        // Lump 0x53
        inline std::vector<uint8_t>             lightmapDataSky;        // Lump 0x55 (uncompressed)
        inline std::vector<uint8_t>             lightmapDataRTLPage;    // Lump 0x5A
        inline std::vector<LightmapPage_t>      lightmapPages;          // Working data during build

        // Light probe lumps (0x04, 0x65-0x68)
        // These provide ambient lighting for models and world geometry
        inline std::vector<LightProbeParentInfo_t> lightprobeParentInfos;   // Lump 0x04
        inline std::vector<LightProbe_t>           lightprobes;             // Lump 0x65 (48 bytes)
        inline std::vector<uint32_t>               staticPropLightprobeIndices; // Lump 0x66
        inline std::vector<LightProbeTree_t>       lightprobeTree;          // Lump 0x67
        inline std::vector<LightProbeRef_t>        lightprobeReferences;    // Lump 0x68
        
        // Realtime light data (lump 0x69)
        // Per-texel data for dynamic lights affecting lightmapped surfaces
        inline std::vector<uint8_t>                lightmapDataRealTimeLights; // Lump 0x69
        
        // Cubemap lumps (0x2A, 0x2B)
        inline std::vector<CubemapSample_t>      cubemaps;            // Lump 0x2A - cubemap sample positions
        inline std::vector<float>                cubemapsAmbientRcp;  // Lump 0x2B - ambient reciprocal per cubemap
        
        // Surface Properties lump (0x11)
        // Maps surface property indices to contents masks, surface types, and surface names
        inline std::vector<CollSurfProps_t>      surfaceProperties;   // Lump 0x11
        
        // Cell AABB system lumps (visibility/streaming)
        inline std::vector<uint32_t>             cellAABBNumObjRefsTotal;  // Lump 0x25 - cumulative obj ref count per node
        inline std::vector<uint16_t>             cellAABBFadeDists;        // Lump 0x27 - fade distances for objects
    }
}
