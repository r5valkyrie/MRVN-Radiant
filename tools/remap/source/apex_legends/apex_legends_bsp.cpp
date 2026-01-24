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
    Apex Legends BSP File I/O

    This file handles reading and writing of Apex Legends BSP files.
    Contains functions for loading and writing BSP file format.
*/

#include "../remap.h"
#include "../bspfile_abstract.h"
#include "../embree_trace.h"
#include <ctime>
#include <cstdio>
#include <unordered_map>
#include <cfloat>

// Ensure Q3MAP_VERSION is defined before using it
#ifndef Q3MAP_VERSION
#define Q3MAP_VERSION "1.0.0"
#endif

/*
    LoadR5BSPFile()
    Loads an Apex Legends BSP file
*/
void LoadR5BSPFile(rbspHeader_t *header, const char *filename) {
    // Not yet implemented
}

/*
    WriteR5BSPFile()
    Writes an Apex Legends BSP file to disk

    This function writes all BSP lumps in the correct order and format.
    The data packing must be exact to match the game's expectations.
*/
void WriteR5BSPFile(const char *filename) {
    rbspHeader_t header{};

    /* Set up header */
    memcpy(header.ident, g_game->bspIdent, 4);
    header.version = LittleLong(g_game->bspVersion);
    header.mapVersion = 30;
    header.maxLump = 127;

    /* Write initial header */
    FILE* file = SafeOpenWrite(filename);
    SafeWrite(file, &header, sizeof(header));    /* overwritten later */

    /* :) */
    {
        char message[64] = REMAP_MOTD;
        SafeWrite(file, &message, sizeof(message));
    }
    {
        char message[64];
        strncpy(message, StringOutputStream(64)("Version:        ", Q3MAP_VERSION).c_str(), 63);
        SafeWrite(file, &message, sizeof(message));
    }
    {
        time_t t;
        time(&t);
        char message[64];
        strncpy(message, StringOutputStream(64)("Time:           ", asctime(localtime(&t))).c_str(), 63);
        SafeWrite(file, &message, sizeof(message));
    }

    /* Write lumps */
    AddLump(file, header.lumps[R5_LUMP_ENTITIES],                 Titanfall::Bsp::entities);
    AddLump(file, header.lumps[R5_LUMP_TEXTURE_DATA],             ApexLegends::Bsp::textureData);
    
    // Write lump 3: Render vertices followed by collision vertices
    // Both are float3 format. Collision's model.vertexIndex points past render verts.
    {
        header.lumps[R5_LUMP_VERTICES].offset = ftell(file);
        // Write render vertices first
        if (!Titanfall::Bsp::vertices.empty()) {
            SafeWrite(file, Titanfall::Bsp::vertices.data(), 
                      Titanfall::Bsp::vertices.size() * sizeof(Vector3));
        }
        // Append collision vertices
        if (!ApexLegends::Bsp::collisionVertices.empty()) {
            SafeWrite(file, ApexLegends::Bsp::collisionVertices.data(),
                      ApexLegends::Bsp::collisionVertices.size() * sizeof(ApexLegends::CollisionVertex_t));
        }
        header.lumps[R5_LUMP_VERTICES].length = ftell(file) - header.lumps[R5_LUMP_VERTICES].offset;
    }
    AddLump(file, header.lumps[R5_LUMP_LIGHTPROBE_PARENT_INFOS],  ApexLegends::Bsp::lightprobeParentInfos);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_ENVIRONMENTS],      ApexLegends::Bsp::shadowEnvironments);
    AddLump(file, header.lumps[R5_LUMP_MODELS],                   ApexLegends::Bsp::models);
    AddLump(file, header.lumps[R5_LUMP_SURFACE_NAMES],            Titanfall::Bsp::textureDataData);
    AddLump(file, header.lumps[R5_LUMP_CONTENTS_MASKS],           ApexLegends::Bsp::contentsMasks);
    AddLump(file, header.lumps[R5_LUMP_SURFACE_PROPERTIES],       ApexLegends::Bsp::surfaceProperties);
    AddLump(file, header.lumps[R5_LUMP_BVH_NODES],                ApexLegends::Bsp::bvhNodes);
    AddLump(file, header.lumps[R5_LUMP_BVH_LEAF_DATA],            ApexLegends::Bsp::bvhLeafDatas);
    AddLump(file, header.lumps[R5_LUMP_PACKED_VERTICES],          ApexLegends::Bsp::packedVertices);
    AddLump(file, header.lumps[R5_LUMP_ENTITY_PARTITIONS],        Titanfall::Bsp::entityPartitions);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_NORMALS],           Titanfall::Bsp::vertexNormals);

    // GameLump
    {
        header.lumps[R5_LUMP_GAME_LUMP].offset = ftell(file);
        header.lumps[R5_LUMP_GAME_LUMP].length = sizeof(Titanfall2::GameLumpHeader_t)
                                               + sizeof(Titanfall2::GameLumpPathHeader_t)
                                               + sizeof(Titanfall::GameLumpPath_t) * Titanfall2::Bsp::gameLumpPathHeader.numPaths
                                               + sizeof(Titanfall2::GameLumpPropHeader_t)
                                               + sizeof(Titanfall2::GameLumpProp_t) * Titanfall2::Bsp::gameLumpPropHeader.numProps
                                               + sizeof(Titanfall2::GameLumpUnknownHeader_t);

        Titanfall2::Bsp::gameLumpHeader.offset = ftell(file) + sizeof(Titanfall2::GameLumpHeader_t);
        Titanfall2::Bsp::gameLumpHeader.length = sizeof(Titanfall2::GameLumpPathHeader_t)
                                              + sizeof(Titanfall::GameLumpPath_t) * Titanfall::Bsp::gameLumpPaths.size()
                                              + sizeof(Titanfall2::GameLumpPropHeader_t)
                                              + sizeof(Titanfall2::GameLumpProp_t) * Titanfall::Bsp::gameLumpProps.size()
                                              + sizeof(Titanfall2::GameLumpUnknownHeader_t);

        SafeWrite(file, &Titanfall2::Bsp::gameLumpHeader, sizeof(Titanfall2::GameLumpHeader_t));
        SafeWrite(file, &Titanfall2::Bsp::gameLumpPathHeader, sizeof(Titanfall2::GameLumpPathHeader_t));
        SafeWrite(file, Titanfall::Bsp::gameLumpPaths.data(), sizeof(Titanfall::GameLumpPath_t) * Titanfall::Bsp::gameLumpPaths.size());
        SafeWrite(file, &Titanfall2::Bsp::gameLumpPropHeader, sizeof(Titanfall2::GameLumpPropHeader_t));
        SafeWrite(file, Titanfall2::Bsp::gameLumpProps.data(), sizeof(Titanfall2::GameLumpProp_t) * Titanfall2::Bsp::gameLumpProps.size());
        SafeWrite(file, &Titanfall2::Bsp::gameLumpUnknownHeader, sizeof(Titanfall2::GameLumpUnknownHeader_t));
    }

    AddLump(file, header.lumps[R5_LUMP_CELL_AABB_NUM_OBJ_REFS_TOTAL], ApexLegends::Bsp::cellAABBNumObjRefsTotal);
    AddLump(file, header.lumps[R5_LUMP_CSM_AABB_NUM_OBJ_REFS_TOTAL],  ApexLegends::Bsp::csmNumObjRefsTotalForAabb);
    AddLump(file, header.lumps[R5_LUMP_CELL_AABB_FADEDISTS],         ApexLegends::Bsp::cellAABBFadeDists);
    AddLump(file, header.lumps[R5_LUMP_CUBEMAPS],                ApexLegends::Bsp::cubemaps);
    AddLump(file, header.lumps[R5_LUMP_CUBEMAPS_AMBIENT_RCP],    ApexLegends::Bsp::cubemapsAmbientRcp);
    AddLump(file, header.lumps[R5_LUMP_WORLD_LIGHTS],            ApexLegends::Bsp::worldLights);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_UNLIT],            ApexLegends::Bsp::vertexUnlitVertices);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_LIT_FLAT],         ApexLegends::Bsp::vertexLitFlatVertices);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_LIT_BUMP],         ApexLegends::Bsp::vertexLitBumpVertices);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_UNLIT_TS],         ApexLegends::Bsp::vertexUnlitTSVertices);
    AddLump(file, header.lumps[R5_LUMP_MESH_INDICES],            Titanfall::Bsp::meshIndices);
    AddLump(file, header.lumps[R5_LUMP_MESHES],                  ApexLegends::Bsp::meshes);
    AddLump(file, header.lumps[R5_LUMP_MESH_BOUNDS],             Titanfall::Bsp::meshBounds);
    AddLump(file, header.lumps[R5_LUMP_MATERIAL_SORT],           ApexLegends::Bsp::materialSorts);
    
    // Lightmap lumps - generated by EmitLightmaps()
    AddLump(file, header.lumps[R5_LUMP_LIGHTMAP_HEADERS],        ApexLegends::Bsp::lightmapHeaders);
    AddLump(file, header.lumps[R5_LUMP_TWEAK_LIGHTS],            ApexLegends::Bsp::tweakLights);
    AddLump(file, header.lumps[R5_LUMP_LIGHTMAP_DATA_SKY],       ApexLegends::Bsp::lightmapDataSky);
    AddLump(file, header.lumps[R5_LUMP_CSM_AABB_NODES],          ApexLegends::Bsp::csmAABBNodes);
    AddLump(file, header.lumps[R5_LUMP_CSM_OBJ_REFERENCES],      ApexLegends::Bsp::csmObjRefsTotal);
    
    // Light probe lumps - generated by EmitLightProbes()
    AddLump(file, header.lumps[R5_LUMP_LIGHTPROBES],                    ApexLegends::Bsp::lightprobes);
    AddLump(file, header.lumps[R5_LUMP_STATIC_PROP_LIGHTPROBE_INDICES], ApexLegends::Bsp::staticPropLightprobeIndices);
    AddLump(file, header.lumps[R5_LUMP_LIGHTPROBE_TREE],                ApexLegends::Bsp::lightprobeTree);
    AddLump(file, header.lumps[R5_LUMP_LIGHTPROBE_REFERENCES],          ApexLegends::Bsp::lightprobeReferences);
    
    AddLump(file, header.lumps[R5_LUMP_CELL_BSP_NODES],          Titanfall::Bsp::cellBSPNodes_stub);           // stub
    AddLump(file, header.lumps[R5_LUMP_CELLS],                   Titanfall::Bsp::cells_stub);                  // stub
    AddLump(file, header.lumps[R5_LUMP_OCCLUSION_MESH_VERTICES], Titanfall::Bsp::occlusionMeshVertices);
    AddLump(file, header.lumps[R5_LUMP_OCCLUSION_MESH_INDICES],  Titanfall::Bsp::occlusionMeshIndices);
    AddLump(file, header.lumps[R5_LUMP_CELL_AABB_NODES],         ApexLegends::Bsp::cellAABBNodes);
    AddLump(file, header.lumps[R5_LUMP_OBJ_REFERENCES],          ApexLegends::Bsp::objReferences);
    AddLump(file, header.lumps[R5_LUMP_OBJ_REFERENCE_BOUNDS],    Titanfall::Bsp::objReferenceBounds);
    AddLump(file, header.lumps[R5_LUMP_LEVEL_INFO],              ApexLegends::Bsp::levelInfo);

    // Shadow mesh lumps
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_OPAQUE_VERTICES], ApexLegends::Bsp::shadowMeshOpaqueVerts);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_ALPHA_VERTICES],  ApexLegends::Bsp::shadowMeshAlphaVerts);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_INDICES],         ApexLegends::Bsp::shadowMeshIndices);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESHES],               ApexLegends::Bsp::shadowMeshes);

    /* Emit bsp size */
    const int size = ftell(file);
    //Sys_Printf("Wrote %.1f MB (%d bytes)\n", (float)size / (1024 * 1024), size);

    /* Write the completed header */
    fseek(file, 0, SEEK_SET);
    SafeWrite(file, &header, sizeof(header));

    /* Ensure all data is written to disk */
    fflush(file);

    /* Close the file */
    fclose(file);
}

/*
    CompileR5BSPFile()
    Compiles an Apex Legends BSP file

    This is the main entry point for BSP compilation.
    It orchestrates all lump generation in the correct order.
*/
void CompileR5BSPFile() {
    ApexLegends::SetupGameLump();
    
    for (entity_t &entity : entities) {
        const char *pszClassname = entity.classname();

        #define ENT_IS(classname) striEqual(pszClassname, classname)

        /* Visible geo */
        if (ENT_IS("worldspawn")) {
            ApexLegends::BeginModel(entity);

            /* Generate bsp meshes from map brushes */
            Shared::MakeMeshes(entity);
            ApexLegends::EmitMeshes(entity);

            ApexLegends::EmitBVHNode();

            ApexLegends::EndModel();
        } else if (ENT_IS("prop_static")) { // Compile as static props into gamelump
            // TODO: use prop_static instead
            ApexLegends::EmitStaticProp(entity);
            continue; // Don't emit as entity
        } else if (ENT_IS("func_occluder")) {
            Titanfall::EmitOcclusionMeshes(entity);
            continue; // Don't emit as entity
        } else {
        }

        ApexLegends::EmitEntity(entity);

        #undef ENT_IS
    }

    Shared::MakeVisReferences();
    Shared::visRoot = Shared::MakeVisTree(Shared::visRefs, 1e30f);
    Shared::MergeVisTree(Shared::visRoot);
    ApexLegends::EmitVisTree();

    Titanfall::EmitEntityPartitions();

    ApexLegends::EmitLevelInfo();
    ApexLegends::EmitWorldLights();
    ApexLegends::EmitCubemaps();           // Cubemap sample positions
    ApexLegends::EmitShadowMeshes();
    ApexLegends::EmitShadowEnvironments();
    
    // Initialize Embree for accelerated ray tracing (used by lightmaps and light probes)
    if (EmbreeTrace::Init()) {
        EmbreeTrace::BuildScene(true);  // Build BVH, skip sky meshes for shadow rays
    }
    
    ApexLegends::EmitLightmaps();
    ApexLegends::EmitLightProbes();        // Light probes for ambient lighting
    
    // Clean up Embree resources
    EmbreeTrace::Shutdown();

    //TODO: Implement real-time lightmaps
    //ApexLegends::EmitRealTimeLightmaps();  // Per-texel RTL data

    Titanfall::EmitStubs();
}
