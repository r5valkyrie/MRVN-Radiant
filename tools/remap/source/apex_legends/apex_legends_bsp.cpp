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
#include "../hiprt_trace.h"
#include <ctime>
#include <chrono>
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
    AddLump(file, header.lumps[R5_LUMP_PLANES],                   Titanfall::Bsp::planes);
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

    // GameLump (0x23) - Static Props
    // Format: Header -> PathHeader -> Paths[] -> PropHeader -> Props[] -> numParented -> ParentInfos[]
    {
        const uint32_t numPaths = ApexLegends::Bsp::gameLumpPaths.size();
        const uint32_t numProps = ApexLegends::Bsp::gameLumpProps.size();
        const uint32_t numParented = ApexLegends::Bsp::gameLumpParentInfos.size();

        // Inner data size (everything after the 20-byte GameLumpHeader)
        const uint32_t innerSize = sizeof(ApexLegends::GameLumpPathHeader_t)
                                 + sizeof(Titanfall::GameLumpPath_t) * numPaths
                                 + sizeof(ApexLegends::GameLumpPropHeader_t)
                                 + sizeof(ApexLegends::GameLumpProp_t) * numProps
                                 + sizeof(uint32_t)  // numParentedStaticProps
                                 + sizeof(ApexLegends::GameLumpParentInfo_t) * numParented;

        // Outer lump size (header + inner data)
        header.lumps[R5_LUMP_GAME_LUMP].offset = ftell(file);
        header.lumps[R5_LUMP_GAME_LUMP].length = sizeof(ApexLegends::GameLumpHeader_t) + innerSize;

        // Set inner header fields
        ApexLegends::Bsp::gameLumpHeader.offset = ftell(file) + sizeof(ApexLegends::GameLumpHeader_t);
        ApexLegends::Bsp::gameLumpHeader.length = innerSize;

        // Write GameLumpHeader
        SafeWrite(file, &ApexLegends::Bsp::gameLumpHeader, sizeof(ApexLegends::GameLumpHeader_t));

        // Write paths
        SafeWrite(file, &ApexLegends::Bsp::gameLumpPathHeader, sizeof(ApexLegends::GameLumpPathHeader_t));
        if (numPaths > 0) {
            SafeWrite(file, ApexLegends::Bsp::gameLumpPaths.data(),
                      sizeof(Titanfall::GameLumpPath_t) * numPaths);
        }

        // Write prop header + props
        SafeWrite(file, &ApexLegends::Bsp::gameLumpPropHeader, sizeof(ApexLegends::GameLumpPropHeader_t));
        if (numProps > 0) {
            SafeWrite(file, ApexLegends::Bsp::gameLumpProps.data(),
                      sizeof(ApexLegends::GameLumpProp_t) * numProps);
        }

        // Write parented static props
        SafeWrite(file, &numParented, sizeof(uint32_t));
        if (numParented > 0) {
            SafeWrite(file, ApexLegends::Bsp::gameLumpParentInfos.data(),
                      sizeof(ApexLegends::GameLumpParentInfo_t) * numParented);
        }
    }

    AddLump(file, header.lumps[R5_LUMP_CELL_AABB_NUM_OBJ_REFS_TOTAL], ApexLegends::Bsp::cellAABBNumObjRefsTotal);
    AddLump(file, header.lumps[R5_LUMP_CSM_AABB_NUM_OBJ_REFS_TOTAL],  ApexLegends::Bsp::csmNumObjRefsTotalForAabb);
    AddLump(file, header.lumps[R5_LUMP_CELL_AABB_FADEDISTS],         ApexLegends::Bsp::cellAABBFadeDists);
    AddLump(file, header.lumps[R5_LUMP_CUBEMAPS],                ApexLegends::Bsp::cubemaps);
    AddLump(file, header.lumps[R5_LUMP_WORLD_LIGHTS],            ApexLegends::Bsp::worldLights);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_UNLIT],            ApexLegends::Bsp::vertexUnlitVertices);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_LIT_FLAT],         ApexLegends::Bsp::vertexLitFlatVertices);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_LIT_BUMP],         ApexLegends::Bsp::vertexLitBumpVertices);
    AddLump(file, header.lumps[R5_LUMP_VERTEX_UNLIT_TS],         ApexLegends::Bsp::vertexUnlitTSVertices);
    AddLump(file, header.lumps[R5_LUMP_MESH_INDICES],            ApexLegends::Bsp::meshIndices);
    AddLump(file, header.lumps[R5_LUMP_MESHES],                  ApexLegends::Bsp::meshes);
    AddLump(file, header.lumps[R5_LUMP_MESH_BOUNDS],             ApexLegends::Bsp::meshBounds);
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
    
    AddLump(file, header.lumps[R5_LUMP_CELL_BSP_NODES],          ApexLegends::Bsp::cellBSPNodes);
    AddLump(file, header.lumps[R5_LUMP_CELLS],                   ApexLegends::Bsp::cells);
    AddLump(file, header.lumps[R5_LUMP_PORTALS],                         ApexLegends::Bsp::portals);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_VERTICES],                 ApexLegends::Bsp::portalVertices);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_EDGES],                    ApexLegends::Bsp::portalEdges);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_VERTEX_EDGES],             ApexLegends::Bsp::portalVertexEdges);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_VERTEX_REFERENCES],        ApexLegends::Bsp::portalVertexReferences);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_EDGE_REFERENCES],          ApexLegends::Bsp::portalEdgeReferences);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_EDGE_INTERSECT_EDGE],      ApexLegends::Bsp::portalEdgeIntersectEdge);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_EDGE_INTERSECT_AT_VERTEX], ApexLegends::Bsp::portalEdgeIntersectAtVertex);
    AddLump(file, header.lumps[R5_LUMP_PORTAL_EDGE_INTERSECT_HEADER],    ApexLegends::Bsp::portalEdgeIntersectHeader);
    AddLump(file, header.lumps[R5_LUMP_OCCLUSION_MESH_VERTICES], ApexLegends::Bsp::occlusionMeshVertices);
    AddLump(file, header.lumps[R5_LUMP_OCCLUSION_MESH_INDICES],  ApexLegends::Bsp::occlusionMeshIndices);
    AddLump(file, header.lumps[R5_LUMP_CELL_AABB_NODES],         ApexLegends::Bsp::cellAABBNodes);
    AddLump(file, header.lumps[R5_LUMP_OBJ_REFERENCES],          ApexLegends::Bsp::objReferences);
    AddLump(file, header.lumps[R5_LUMP_OBJ_REFERENCE_BOUNDS],    ApexLegends::Bsp::objReferenceBounds);
    AddLump(file, header.lumps[R5_LUMP_LEVEL_INFO],              ApexLegends::Bsp::levelInfo);

    // Shadow mesh lumps
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_OPAQUE_VERTICES], ApexLegends::Bsp::shadowMeshOpaqueVerts);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_ALPHA_VERTICES],  ApexLegends::Bsp::shadowMeshAlphaVerts);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_INDICES],         ApexLegends::Bsp::shadowMeshIndices);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESHES],               ApexLegends::Bsp::shadowMeshes);

    //RTL lightmap data - generated by EmitLightmaps()
    //AddLump(file, header.lumps[R5_LUMP_LIGHTMAP_DATA_RTL_PAGE], ApexLegends::Bsp::lightmapDataRTLPage);
    //AddLump(file, header.lumps[R5_LUMP_LIGHTMAP_DATA_REAL_TIME_LIGHTS], ApexLegends::Bsp::lightmapDataRealTimeLights);

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
    auto compileStart = std::chrono::steady_clock::now();
    ApexLegends::SetupGameLump();

    const char *phaseNames[] = {
        "Static Props",
        "Models & Geometry",
        "Vis Tree",
        "Entities & Lighting",
        "GPU Ray Tracing",
        "Lightmaps",
        "Light Probes",
        "Portals & Cells",
        "Write BSP",
    };
    Sys_ConsoleInit( source, 9, phaseNames );

    Sys_PhaseBegin( 0 );
    {
        int propCount = 0;
        for (entity_t &entity : entities) {
            if (striEqual(entity.classname(), "prop_static")) {
                ApexLegends::EmitStaticProp(entity);
                propCount++;
            }
        }
        Sys_Printf("  %d static props\n", propCount);
    }

    Sys_PhaseBegin( 1 );
    {
        int modelIndex = 0;
        int brushEntityCount = 0;
        for (entity_t &entity : entities) {
            const char *pszClassname = entity.classname();

            #define ENT_IS(classname) striEqual(pszClassname, classname)

            if (ENT_IS("worldspawn")) {
                Sys_Printf("\n  Model %d: worldspawn\n", modelIndex);
                ApexLegends::BeginModel(entity);
                Shared::MakeMeshes(entity);
                if (!noLightmaps) {
                    ApexLegends::SetupSurfaceLightmaps();
                }
                ApexLegends::EmitMeshes(entity);
                ApexLegends::EmitBVHNode();
                ApexLegends::EndModel();
                modelIndex++;
            } else if (ENT_IS("prop_static")) {
                continue; // Already processed in Phase 1
            } else if (ENT_IS("func_occluder")) {
                Titanfall::EmitOcclusionMeshes(entity);
                // Move occlusion data from shared Titanfall vectors to Apex vectors
                ApexLegends::Bsp::occlusionMeshVertices = std::move(Titanfall::Bsp::occlusionMeshVertices);
                ApexLegends::Bsp::occlusionMeshIndices = std::move(Titanfall::Bsp::occlusionMeshIndices);
                continue; // Don't emit as entity
            } else {
                if (!entity.brushes.empty()) {
                    Sys_FPrintf(SYS_VRB, "--- Model %d: %s ---\n", modelIndex, pszClassname);
                    ApexLegends::BeginModel(entity);
                    Shared::MakeMeshes(entity);
                    ApexLegends::EmitMeshes(entity);
                    ApexLegends::EmitBVHNode();
                    ApexLegends::EndModel();
                    modelIndex++;
                    brushEntityCount++;

                    /* Entities routed to .ent files need their collision BVH
                    serialized as *coll key-value pairs, since the engine
                    loads collision for these from the entity string rather
                    than from BSP lumps.  envmap_volume goes to the BSP lump
                    but still carries inline *coll data. */
                    if (!ApexLegends::EntityGoesToBSPLump(entity)
                        || striEqual(entity.classname(), "envmap_volume")) {
                        ApexLegends::SerializeCollisionToEntity(entity);
                    }
                }
            }

            /* envmap_volume: ensure cubeMapIndex is set (engine reads this key
               to look up cubemap samples in lump 0x2A) */
            if (striEqual(entity.classname(), "envmap_volume")) {
                const char *cubemapID = entity.valueForKey("cubemapID");
                if (cubemapID[0] != '\0') {
                    entity.setKeyValue("cubeMapIndex", cubemapID);
                } else {
                    entity.setKeyValue("cubeMapIndex", "0");
                    entity.setKeyValue("cubemapID", "0");
                }
            }

            ApexLegends::EmitEntity(entity);

            #undef ENT_IS
        }
        Sys_Printf("  %d models (%d worldspawn + %d brush entities)\n",
                    modelIndex, 1, brushEntityCount);
    }

    /* Fix up model.vertexIndex for all models */
    {
        uint32_t totalRenderVerts = static_cast<uint32_t>(Titanfall::Bsp::vertices.size());
        for (ApexLegends::Model_t &model : ApexLegends::Bsp::models) {
            model.vertexIndex += totalRenderVerts;
        }
    }

    /* Regenerate worldspawn meshes for vis/lighting passes */
    Shared::MakeMeshes(entities[0]);

    Sys_PhaseBegin( 2 );
    {
        auto t0 = std::chrono::steady_clock::now();
        Shared::MakeVisReferences();
        Shared::visRoot = Shared::MakeVisTree(Shared::visRefs, 1e30f);
        Shared::MergeVisTree(Shared::visRoot);
        ApexLegends::EmitVisTree();
        auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Sys_Printf("  Vis tree built (%.2fs)\n", dt);
    }

    Sys_PhaseBegin( 3 );
    Titanfall::EmitEntityPartitions();
    ApexLegends::EmitLevelInfo();
    ApexLegends::EmitWorldLights();
    ApexLegends::EmitCubemaps();
    ApexLegends::EmitShadowMeshes();
    ApexLegends::EmitShadowEnvironments();

    Sys_PhaseBegin( 4 );
    if (noLightmaps && (noLightProbes || singleLightProbe)) {
        Sys_Printf("  Skipped (lightmaps and probes disabled)\n");
    } else if (HIPRTTrace::Init()) {
        HIPRTTrace::BuildScene(true);
    }

    Sys_PhaseBegin( 5 );
    if (noLightmaps) {
        Sys_Printf("  Lightmaps disabled\n");
    }
    // EmitLightmaps handles the empty-surfaces case and creates a minimal stub.
    // When noLightmaps is set, SetupSurfaceLightmaps was skipped so surfaces are empty.
    ApexLegends::EmitLightmaps();

    Sys_PhaseBegin( 6 );
    if (noLightProbes || singleLightProbe) {
        Sys_Printf("  Light probes %s, generating stub\n",
                   noLightProbes ? "disabled by -nolightprobes" : "limited by -singlelightprobe");
        ApexLegends::EmitSingleLightProbe();
    } else {
        ApexLegends::EmitLightProbes();
    }

    /* Emit info_lightprobe entities into BSP entity lump (0000) for each
       generated probe reference.  The engine expects these in the main
       entity lump so it can locate light probes at runtime. */
    {
        /* EmitEntityPartitions() already null-terminated the entity vector,
           so pop it, append new entities, then re-add the terminator. */
        if (!Titanfall::Bsp::entities.empty() && Titanfall::Bsp::entities.back() == '\0') {
            Titanfall::Bsp::entities.pop_back();
        }

        for (const LightProbeRef_t &ref : ApexLegends::Bsp::lightprobeReferences) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "{\n\"origin\" \"%.6g %.6g %.6g\"\n\"classname\" \"info_lightprobe\"\n}\n",
                     ref.origin[0], ref.origin[1], ref.origin[2]);
            const char *p = buf;
            while (*p) {
                Titanfall::Bsp::entities.push_back(*p++);
            }
        }

        Titanfall::Bsp::entities.push_back('\0');

        Sys_Printf("  %zu info_lightprobe entities written to entity lump\n",
                   ApexLegends::Bsp::lightprobeReferences.size());
    }

    HIPRTTrace::Shutdown();

    Sys_PhaseBegin( 7 );
    ApexLegends::EmitPortals();
    ApexLegends::EmitCells();

    Sys_PhaseBegin( 8 );
    EndBSPFile( true );
    g_compileHandledEndBSP = true;
    Sys_PhaseEnd();

    auto totalTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count();
    Sys_ConsoleShutdown( totalTime );
}
