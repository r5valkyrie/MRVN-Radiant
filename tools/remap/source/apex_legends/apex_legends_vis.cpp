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


#include "../remap.h"
#include "../model.h"
#include "../bspfile_abstract.h"
#include <ctime>

/*
    EmitVisTree
    Emits the vistree to the bsp file
*/
void ApexLegends::EmitVisTree() {
    Shared::visNode_t &root = Shared::visRoot;

    // Force large bounds to disable culling for debugging
    Vector3 largeMin(-32000.0f, -32000.0f, -32000.0f);
    Vector3 largeMax(32000.0f, 32000.0f, 32000.0f);

    ApexLegends::CellAABBNode_t &bn = ApexLegends::Bsp::cellAABBNodes.emplace_back();
    bn.maxs = largeMax;
    bn.mins = largeMin;
    bn.firstChild = 0;  // No children - put all refs in root
    bn.childCount = 0;
    bn.childFlags = 0x40;
    
    // Put ALL mesh references directly in the root node
    bn.objRefOffset = 0;
    bn.objRefCount = Shared::visRefs.size();
    bn.objRefFlags = 0x40;
    
    // Emit all references with large bounds
    for (Shared::visRef_t &ref : Shared::visRefs) {
        Titanfall::ObjReferenceBounds_t &rb = Titanfall::Bsp::objReferenceBounds.emplace_back();
        rb.maxs = largeMax;
        rb.mins = largeMin;

        ApexLegends::Bsp::objReferences.emplace_back(ref.index);
    }
}


/*
    EmitMeshes()
    writes the mesh list to the bsp
*/
void ApexLegends::EmitMeshes(const entity_t &e) {
    Sys_Printf("  EmitMeshes: %zu meshes from Shared::meshes\n", Shared::meshes.size());

    // Setup lightmaps first so we can get UV coordinates for lit vertices
    ApexLegends::SetupSurfaceLightmaps();

    int meshIndex = 0;
    for (const Shared::Mesh_t &mesh : Shared::meshes) {
        ApexLegends::Mesh_t &m = ApexLegends::Bsp::meshes.emplace_back();
        m.flags = mesh.shaderInfo->surfaceFlags;
        m.triOffset = Titanfall::Bsp::meshIndices.size();
        m.triCount = mesh.triangles.size() / 3;

        int vertexOffset;
        uint16_t vertexType;
        if (CHECK_FLAG(m.flags, S_VERTEX_LIT_BUMP)) {
            vertexOffset = ApexLegends::Bsp::vertexLitBumpVertices.size();
            vertexType = 2;
        } else if (CHECK_FLAG(m.flags, S_VERTEX_UNLIT)) {
            vertexOffset = ApexLegends::Bsp::vertexUnlitVertices.size();
            vertexType = 1;
        } else if (CHECK_FLAG(m.flags, S_VERTEX_UNLIT_TS)) {
            vertexOffset = ApexLegends::Bsp::vertexUnlitTSVertices.size();
            vertexType = 3;
        } else {
            // Default to LIT_BUMP for _wldc error material fallback (aspect 7)
            // Using UNLIT would give _wldu error material (aspect 6)
            vertexOffset = ApexLegends::Bsp::vertexLitBumpVertices.size();
            vertexType = 2;
            m.flags |= S_VERTEX_LIT_BUMP;
            mesh.shaderInfo->surfaceFlags |= S_VERTEX_LIT_BUMP;
        }

        uint16_t vertexCount = (uint16_t)mesh.vertices.size();

        // Emit texture related structs - need this first to get materialSortOffset
        // Get lightmap page index for this mesh (returns 0 for non-lit meshes)
        int16_t lightmapPageIdx = ApexLegends::GetLightmapPageIndex(meshIndex);

        // Emit texture related structs
        uint32_t textureIndex = ApexLegends::EmitTextureData(*mesh.shaderInfo);
        m.materialOffset = ApexLegends::EmitMaterialSort(textureIndex, vertexOffset, mesh.vertices.size(), lightmapPageIdx);
        int materialSortOffset = ApexLegends::Bsp::materialSorts.at(m.materialOffset).vertexOffset;
        
        // Calculate relative vertex offset (relative to MaterialSort's vertexOffset)
        int relativeVertexOffset = vertexOffset - materialSortOffset;

        // Fill in the unknown array - these fields are critical for mesh rendering
        // unknown[0-1]: vertexOffset (relative to MaterialSort's vertexOffset, stored as uint32)
        // unknown[2-3]: vertexCount (stored as uint32)
        // unknown[4]: vertexType (0=LIT_FLAT, 1=UNLIT, 2=LIT_BUMP, 3=UNLIT_TS)
        // unknown[5]: unused
        // unknown[6-7]: const0 (must be 0xFFFFFF00 = 4294967040)
        m.unknown[0] = (uint16_t)(relativeVertexOffset & 0xFFFF);
        m.unknown[1] = (uint16_t)((relativeVertexOffset >> 16) & 0xFFFF);
        m.unknown[2] = vertexCount;
        m.unknown[3] = 0;
        m.unknown[4] = vertexType;
        m.unknown[5] = 0;
        // const0 = 0xFFFFFF00 -> low 16 bits = 0xFF00, high 16 bits = 0xFFFF
        m.unknown[6] = 0xFF00;
        m.unknown[7] = 0xFFFF;
        
        MinMax  aabb;

        // Save vertices and vertexnormals
        for (std::size_t i = 0; i < mesh.vertices.size(); i++) {
            Shared::Vertex_t vertex = mesh.vertices.at(i);

            // Check against aabb
            aabb.extend(vertex.xyz);

            if (CHECK_FLAG(m.flags, S_VERTEX_LIT_BUMP)) {
                // Get lightmap UV for this vertex
                Vector2 lightmapUV(0, 0);
                ApexLegends::GetLightmapUV(meshIndex, vertex.xyz, lightmapUV);
                ApexLegends::EmitVertexLitBump(vertex, lightmapUV);
            } else if (CHECK_FLAG(m.flags, S_VERTEX_UNLIT)) {
                ApexLegends::EmitVertexUnlit(vertex);
            } else if (CHECK_FLAG(m.flags, S_VERTEX_UNLIT_TS)) {
                ApexLegends::EmitVertexUnlitTS(vertex);
            } else {
                // There's no way to get here, if future code changes break this exit with an error
                Error("Attempted to write VertexLitFlat, this is an error!");
                ApexLegends::EmitVertexLitFlat(vertex);
            }
        }

        // Save triangles
        for (uint16_t triangle : mesh.triangles) {
            Titanfall::Bsp::meshIndices.emplace_back(triangle + vertexOffset - materialSortOffset);
        }

        // Save MeshBounds
        Titanfall::MeshBounds_t &mb = Titanfall::Bsp::meshBounds.emplace_back();
        mb.origin = (aabb.maxs + aabb.mins) / 2;
        mb.extents = (aabb.maxs - aabb.mins) / 2;

        meshIndex++;
    }

    Sys_Printf("  EmitMeshes: %zu meshes written to ApexLegends::Bsp::meshes\n", ApexLegends::Bsp::meshes.size());
}


/*
    EmitTextureData()
    Emits texture data and returns its index
*/
uint32_t ApexLegends::EmitTextureData(shaderInfo_t shader) {
    std::string  tex = shader.shader.c_str();
    std::size_t  index;

    // Strip 'textures/'
    tex.erase(tex.begin(), tex.begin() + strlen("textures/"));
    std::replace(tex.begin(), tex.end(), '/', '\\');  // Do we even need to do this?

    // Check if it's already saved
    std::string table = std::string(Titanfall::Bsp::textureDataData.begin(), Titanfall::Bsp::textureDataData.end());
    index = table.find(tex);
    if (index != std::string::npos) {
        // Is already saved, find the index of its textureData
        for (std::size_t i = 0; i < ApexLegends::Bsp::textureData.size(); i++) {
            ApexLegends::TextureData_t &td = ApexLegends::Bsp::textureData.at(i);

            if (td.surfaceIndex == index) {
                return i;
            }
        }
    }

    // Wasn't already saved, save it
    index = ApexLegends::Bsp::textureData.size();

    // Debug: log texture emission
    Sys_Printf("  Emitting texture: %s (flags: 0x%X)\n", tex.c_str(), shader.surfaceFlags);

    // Add to Table
    StringOutputStream data;
    data << tex.c_str();
    std::vector<char> str = { data.begin(), data.end() };
    str.push_back('\0');  // Add null terminator

    ApexLegends::TextureData_t &td = ApexLegends::Bsp::textureData.emplace_back();
    td.surfaceIndex = Titanfall::Bsp::textureDataData.size();
    td.sizeX = shader.shaderImage->width;
    td.sizeY = shader.shaderImage->height;
    td.flags = shader.surfaceFlags;

    Titanfall::Bsp::textureDataData.insert(Titanfall::Bsp::textureDataData.end(), str.begin(), str.end());

    return index;
}


/*
    EmitMaterialSort()
    Tries to create a material sort of the last texture
    lightmapIdx: Index of the lightmap page (0 for default/non-lit meshes)
*/
uint16_t ApexLegends::EmitMaterialSort(uint32_t index, int offset, int count, int16_t lightmapIdx) {
        /* Check if the material sort we need already exists */
        std::size_t pos = 0;
        for (ApexLegends::MaterialSort_t &ms : ApexLegends::Bsp::materialSorts) {
            if (ms.textureData == index && ms.lightmapIndex == lightmapIdx && offset - ms.vertexOffset + count < 65535) {
                return pos;
            }
            pos++;
        }

        ApexLegends::MaterialSort_t &ms = ApexLegends::Bsp::materialSorts.emplace_back();
        ms.textureData = index;
        ms.lightmapIndex = lightmapIdx;
        ms.unknown0 = 0;
        ms.unknown1 = 0;
        ms.vertexOffset = offset;

        return pos;
}


/*
    EmitVertexUnlit
    Saves a vertex into ApexLegends::Bsp::vertexUnlitVertices
*/
void ApexLegends::EmitVertexUnlit(Shared::Vertex_t &vertex) {
    ApexLegends::VertexUnlit_t& ul = ApexLegends::Bsp::vertexUnlitVertices.emplace_back();
    ul.vertexIndex = Titanfall::EmitVertex(vertex.xyz);
    ul.normalIndex = Titanfall::EmitVertexNormal(vertex.normal);
    ul.uv0         = vertex.textureUV;
    ul.negativeOne = -1;
}


/*
    EmitVertexLitFlat
    Saves a vertex into ApexLegends::Bsp::vertexLitFlatVertices
    NOTE: Lit Flat crashes r5r and so is substituted with VertexUnlit in EmitMeshes
*/
void ApexLegends::EmitVertexLitFlat(Shared::Vertex_t &vertex) {
    ApexLegends::VertexLitFlat_t& lf = ApexLegends::Bsp::vertexLitFlatVertices.emplace_back();
    lf.vertexIndex = Titanfall::EmitVertex(vertex.xyz);
    lf.normalIndex = Titanfall::EmitVertexNormal(vertex.normal);
    lf.uv0         = vertex.textureUV;
    lf.unknown0    = 0;
}


/*
    EmitVertexLitBump
    Saves a vertex into ApexLegends::Bsp::vertexLitBumpVertices
    lightmapUV: Lightmap UV coordinates in [0,1] range
*/
void ApexLegends::EmitVertexLitBump(Shared::Vertex_t &vertex, const Vector2 &lightmapUV) {
    ApexLegends::VertexLitBump_t& lv = ApexLegends::Bsp::vertexLitBumpVertices.emplace_back();
    lv.vertexIndex = Titanfall::EmitVertex(vertex.xyz);
    lv.normalIndex = Titanfall::EmitVertexNormal(vertex.normal);
    lv.uv0         = vertex.textureUV;
    lv.negativeOne = 0x00FFFFFF;  // Special marker value (NOT -1, which causes crash)
    lv.uv1         = lightmapUV;
    // normalIndex2: second normal index with 0x80000000 flag - reuse same normal
    lv.normalIndex2 = lv.normalIndex | 0x80000000;
}


/*
    EmitVertexUnlitTS
    Saves a vertex into ApexLegends::Bsp::vertexUnlitTSVertices
*/
void ApexLegends::EmitVertexUnlitTS(Shared::Vertex_t &vertex) {
    ApexLegends::VertexUnlitTS_t& ul = ApexLegends::Bsp::vertexUnlitTSVertices.emplace_back();
    ul.vertexIndex = Titanfall::EmitVertex(vertex.xyz);
    ul.normalIndex = Titanfall::EmitVertexNormal(vertex.normal);
    ul.uv0         = vertex.textureUV;
    ul.unknown0    = 0x00FFFF01;  // Special marker value (matches official BSP)
    ul.unknown1    = 0;
}

/*
    EmitObjReferences
    emits obj references to the bsp
*/
std::size_t ApexLegends::EmitObjReferences(Shared::visNode_t &node) {
    for (Shared::visRef_t &ref : node.refs) {
        Titanfall::ObjReferenceBounds_t &rb = Titanfall::Bsp::objReferenceBounds.emplace_back();
        rb.maxs = ref.minmax.maxs;
        rb.mins = ref.minmax.mins;

        ApexLegends::Bsp::objReferences.emplace_back(ref.index);
    }

    return ApexLegends::Bsp::objReferences.size() - node.refs.size();
}


int ApexLegends::EmitVisChildrenOfTreeNode(Shared::visNode_t node) {
    int index = ApexLegends::Bsp::cellAABBNodes.size();  // Index of first child of node

    for (std::size_t i = 0; i < node.children.size(); i++) {
        Shared::visNode_t &n = node.children.at(i);

        ApexLegends::CellAABBNode_t &bn = ApexLegends::Bsp::cellAABBNodes.emplace_back();
        bn.maxs = n.minmax.maxs;
        bn.mins = n.minmax.mins;
        bn.childCount = n.children.size();
        bn.firstChild = 0;  // Will be set later if there are children
        bn.childFlags = 0x40;
        
        // Initialize objRef fields - must be explicitly zeroed
        bn.objRefCount = 0;
        bn.objRefOffset = 0;
        bn.objRefFlags = 0;

        if (n.refs.size()) {
            bn.objRefOffset = ApexLegends::EmitObjReferences(n);
            bn.objRefCount = n.refs.size();
            bn.objRefFlags = 0x40;
        }
    }

    for (std::size_t i = 0; i < node.children.size(); i++) {
        int firstChild = ApexLegends::EmitVisChildrenOfTreeNode(node.children.at(i));

        ApexLegends::CellAABBNode_t &bn = ApexLegends::Bsp::cellAABBNodes.at(index + i);
        if (node.children.at(i).children.size() > 0) {
            bn.firstChild = firstChild;
        }
    }

    return index;
}

/*
   EmitEntity()
   Saves an entity into it's corresponding .ent file or the lump in the .bsp
*/
void ApexLegends::EmitEntity(const entity_t &e) {
    StringOutputStream data;
    data << "{\n";
    for (const epair_t& pair : e.epairs) {
        data << "\"" << pair.key.c_str() << "\" \"" << pair.value.c_str() << "\"\n";
    }
    data << "}\n";

    std::vector<char> str = { data.begin(), data.end() };

    // env
    if (striEqualPrefix(e.valueForKey("classname"), "light")
     || striEqualPrefix(e.valueForKey("classname"), "color")
     || striEqualPrefix(e.valueForKey("classname"), "fog")
     || striEqualPrefix(e.valueForKey("classname"), "env")
     || striEqualPrefix(e.valueForKey("classname"), "sky")) {
        Titanfall::Ent::env.insert(Titanfall::Ent::env.end(), str.begin(), str.end());
    // fx
    } else if(striEqualPrefix(e.valueForKey("classname"), "info_particle")) {
        Titanfall::Ent::fx.insert(Titanfall::Ent::fx.end(), str.begin(), str.end());
    // script
    } else if (striEqualPrefix(e.valueForKey("classname"), "info_target")
            || striEqualPrefix(e.valueForKey("classname"), "prop_dynamic")
            || striEqualPrefix(e.valueForKey("classname"), "trigger_hurt")) {
        Titanfall::Ent::script.insert(Titanfall::Ent::script.end(), str.begin(), str.end());
    // snd
    // spawn
    } else if (striEqualPrefix(e.valueForKey("classname"), "info_")) {
        Titanfall::Ent::spawn.insert(Titanfall::Ent::spawn.end(), str.begin(), str.end());
    // bsp entity lump
    } else {
        Titanfall::Bsp::entities.insert(Titanfall::Bsp::entities.end(), str.begin(), str.end());
    }
}

/*
    EmitLevelInfo
    Emits apex level info
*/
void ApexLegends::EmitLevelInfo() {
    ApexLegends::LevelInfo_t &li = ApexLegends::Bsp::levelInfo.emplace_back();
    li.unk0 = 51;
    li.unk1 = 51;
    li.unk2 = 51;
    li.unk3 = 256;
    li.unk4 = 22;

    // Find the last light_environment entity and extract sun direction for unk5
    // unk5 is the sun direction vector used by the engine
    Vector3 sunDir(0.0f, 0.0f, -1.0f);  // Default: straight down
    for (const entity_t& e : entities) {
        if (striEqual(e.classname(), "light_environment")) {
            // Get sun direction from angles
            // light_environment uses "angles" key with format "pitch yaw roll"
            // or separate "pitch" key. The pitch key seems to be the primary one.
            Vector3 angles = e.vectorForKey("angles");
            float pitch = e.floatForKey("pitch", "0");
            // If pitch key exists, it takes precedence (and is often the negative)
            if (e.valueForKey("pitch")) {
                angles[0] = -pitch;  // pitch key is negative of the actual pitch
            }
            sunDir = ApexLegends::vector3_from_angles(angles);
        }
    }
    li.unk5[0] = sunDir.x();
    li.unk5[1] = sunDir.y();
    li.unk5[2] = sunDir.z();

    li.modelCount = 0;
    for( Model_t &model : ApexLegends::Bsp::models ) {
        if( model.meshCount )
            li.modelCount++;
    }
}

/*
    EmitStaticProp()
    Emits a static prop
*/
void ApexLegends::EmitStaticProp(entity_t &e) {
    const char *model = e.valueForKey("model");
    int16_t pathIdx = -1;
    MinMax minmax;

    std::vector<const AssMeshWalker*> meshes = LoadModelWalker( model, 0 );
    if(meshes.empty()) {
        Sys_Warning("Failed to load model: %s\n", model);
        return;
    }

    Sys_Printf("Emitting static prop: %s\n", model);

    for(std::size_t i = 0; i < Titanfall::Bsp::gameLumpPaths.size(); i++) {
        Titanfall::GameLumpPath_t &path = Titanfall::Bsp::gameLumpPaths.at(i);

        if(!string_compare_nocase(path.path, model)) {
            pathIdx = i;
            break;
        }
    }

    if(pathIdx == -1) {
        Titanfall2::Bsp::gameLumpPathHeader.numPaths++;
        Titanfall::GameLumpPath_t &path = Titanfall::Bsp::gameLumpPaths.emplace_back();
        strncpy(path.path, model, 127);
        pathIdx = Titanfall::Bsp::gameLumpPaths.size() - 1;
    }

    Titanfall2::Bsp::gameLumpPropHeader.numProps++;
    Titanfall2::Bsp::gameLumpPropHeader.unk0++;
    Titanfall2::Bsp::gameLumpPropHeader.unk1++;
    Titanfall2::GameLumpProp_t &prop = Titanfall2::Bsp::gameLumpProps.emplace_back();
    Vector3 origin;
    Vector3 angles;
    if(!e.read_keyvalue(origin, "origin")) {
        origin.x() = 0.0f;
        origin.y() = 0.0f;
        origin.z() = 0.0f;
    }
    if(!e.read_keyvalue(angles, "angles")) {
        angles.x() = 0.0f;
        angles.y() = 0.0f;
        angles.z() = 0.0f;
    }

    prop.origin = origin;
    prop.angles = angles;
    prop.scale = 1.0f;
    prop.modelName = pathIdx;
    prop.solid = 6;
    prop.flags = 84;
    prop.envCubemap = -1;
    prop.fade_scale = -1.0f;
    prop.cpu_level[0] = -1.0f;
    prop.cpu_level[1] = -1.0f;
    prop.gpu_level[0] = -1.0f;
    prop.gpu_level[1] = -1.0f;
    prop.unk.x() = 1.0f;
    prop.unk.y() = 0.0f;
    prop.unk.z() = 0.0f;

    for ( const auto mesh : meshes )
    {
        mesh->forEachFace( [&minmax, &origin]( const Vector3 ( &xyz )[3], const Vector2 ( &st )[3] ){
                        minmax.extend(xyz[0] + origin);
            minmax.extend(xyz[1] + origin);
            minmax.extend(xyz[2] + origin);
                } );
    }

    Shared::visRef_t &ref = Shared::visRefs.emplace_back();

    ref.minmax = minmax;
    ref.index = ApexLegends::Bsp::models.at(0).meshCount + Titanfall2::Bsp::gameLumpProps.size() - 1;
}

void ApexLegends::SetupGameLump() {
    Titanfall2::Bsp::gameLumpHeader.version = 1;
    memcpy( &Titanfall2::Bsp::gameLumpHeader.ident, "prps", 4 );
    Titanfall2::Bsp::gameLumpHeader.gameConst = 3080192;
}
