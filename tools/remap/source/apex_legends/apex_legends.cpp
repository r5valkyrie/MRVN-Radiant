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
#include "../bspfile_abstract.h"
#include "../embree_trace.h"
#include <ctime>
#include <cstdio>
#include <unordered_map>
#include <cfloat>


/*
   LoadR2BSPFile()
   loads a titanfall2 bsp file
*/
void LoadR5BSPFile(rbspHeader_t *header, const char *filename) {
    // Nothing yet
}


/*
   WriteR2BSPFile()
   writes a apex bsp file
*/
void WriteR5BSPFile(const char *filename) {
    rbspHeader_t header{};

    /* set up header */
    memcpy(header.ident, g_game->bspIdent, 4);
    header.version = LittleLong(g_game->bspVersion);
    header.mapVersion = 30;
    header.maxLump = 127;

    /* write initial header */
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
                                              + sizeof(Titanfall::GameLumpPath_t) * Titanfall2::Bsp::gameLumpPathHeader.numPaths
                                              + sizeof(Titanfall2::GameLumpPropHeader_t)
                                              + sizeof(Titanfall2::GameLumpProp_t) * Titanfall2::Bsp::gameLumpPropHeader.numProps
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
    //AddLump(file, header.lumps[R5_LUMP_LIGHTMAP_DATA_REAL_TIME_LIGHTS], ApexLegends::Bsp::lightmapDataRealTimeLights);
    
    AddLump(file, header.lumps[R5_LUMP_CELL_BSP_NODES],          Titanfall::Bsp::cellBSPNodes_stub);           // stub
    AddLump(file, header.lumps[R5_LUMP_CELLS],                   Titanfall::Bsp::cells_stub);                  // stub
    AddLump(file, header.lumps[R5_LUMP_OCCLUSION_MESH_VERTICES], Titanfall::Bsp::occlusionMeshVertices);
    AddLump(file, header.lumps[R5_LUMP_OCCLUSION_MESH_INDICES],  Titanfall::Bsp::occlusionMeshIndices);
    AddLump(file, header.lumps[R5_LUMP_CELL_AABB_NODES],         ApexLegends::Bsp::cellAABBNodes);
    AddLump(file, header.lumps[R5_LUMP_OBJ_REFERENCES],          ApexLegends::Bsp::objReferences);
    AddLump(file, header.lumps[R5_LUMP_OBJ_REFERENCE_BOUNDS],    Titanfall::Bsp::objReferenceBounds);
    //AddLump(file, header.lumps[R5_LUMP_LIGHTMAP_DATA_RTL_PAGE],  ApexLegends::Bsp::lightmapDataRTLPage);
    AddLump(file, header.lumps[R5_LUMP_LEVEL_INFO],              ApexLegends::Bsp::levelInfo);

    // Shadow mesh lumps
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_OPAQUE_VERTICES], ApexLegends::Bsp::shadowMeshOpaqueVerts);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_ALPHA_VERTICES],  ApexLegends::Bsp::shadowMeshAlphaVerts);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESH_INDICES],         ApexLegends::Bsp::shadowMeshIndices);
    AddLump(file, header.lumps[R5_LUMP_SHADOW_MESHES],               ApexLegends::Bsp::shadowMeshes);

    /* emit bsp size */
    const int size = ftell(file);
    //Sys_Printf("Wrote %.1f MB (%d bytes)\n", (float)size / (1024 * 1024), size);

    /* write the completed header */
    fseek(file, 0, SEEK_SET);
    SafeWrite(file, &header, sizeof(header));

    /* ensure all data is written to disk */
    fflush(file);

    /* close the file */
    fclose(file);
}


/*
   CompileR5BSPFile()
   Compiles a v47 bsp file
*/
void CompileR5BSPFile() {
    ApexLegends::SetupGameLump();
    
    for (entity_t &entity : entities) {
        const char *pszClassname = entity.classname();

        #define ENT_IS(classname) striEqual(pszClassname, classname)

        /* visible geo */
        if (ENT_IS("worldspawn")) {
            ApexLegends::BeginModel(entity);

            /* generate bsp meshes from map brushes */
            Shared::MakeMeshes(entity);
            ApexLegends::EmitMeshes(entity);

            ApexLegends::EmitBVHNode();

            ApexLegends::EndModel();
        }  else if (ENT_IS("prop_static")) { // Compile as static props into gamelump
            // TODO: use prop_static instead
            ApexLegends::EmitStaticProp(entity);
            continue; // Don't emit as entity
        } else if (ENT_IS("func_occluder")) {
            Titanfall::EmitOcclusionMeshes( entity );
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
    ApexLegends::EmitStubs();
}


/*
    EmitStubs
    Fills out all the nessesary lumps we dont generate so the map at least boots and we can noclip around
*/
void ApexLegends::EmitStubs() {
    // No longer needed - cell AABB lumps are now generated in EmitVisTree()
}

/*
    EmitCubemaps
    Emits cubemap sample positions from env_cubemap entities
    
    The engine uses these positions to capture environment reflections at runtime
    via the buildcubemaps command. Each cubemap sample has:
    - origin: Position where the cubemap will be captured (stored as int32 in lump)
    - guid: Asset GUID for pre-baked cubemaps (0 = runtime capture required)
    
    Lumps:
    - 0x2A (CUBEMAPS): Array of CubemapSample_t (16 bytes each)
    - 0x2B (CUBEMAPS_AMBIENT_RCP): Array of floats, one per cubemap (reciprocal of ambient)
    
    If no env_cubemap entities exist, creates a single default at world center
*/
void ApexLegends::EmitCubemaps() {
    Sys_Printf("--- EmitCubemaps ---\n");
    
    ApexLegends::Bsp::cubemaps.clear();
    ApexLegends::Bsp::cubemapsAmbientRcp.clear();
    
    // Collect env_cubemap entities
    std::vector<Vector3> cubemapPositions;
    
    for (const entity_t &entity : entities) {
        const char *classname = entity.classname();
        if (striEqual(classname, "env_cubemap")) {
            Vector3 origin;
            if (entity.read_keyvalue(origin, "origin")) {
                cubemapPositions.push_back(origin);
            }
        }
    }
    
    // If no cubemaps, generate default positions based on world bounds
    if (cubemapPositions.empty()) {
        // Calculate world bounds
        MinMax worldBounds;
        for (const Shared::Mesh_t &mesh : Shared::meshes) {
            worldBounds.extend(mesh.minmax.mins);
            worldBounds.extend(mesh.minmax.maxs);
        }
        
        if (worldBounds.valid()) {
            // Place a single cubemap at world center
            Vector3 center = (worldBounds.mins + worldBounds.maxs) * 0.5f;
            cubemapPositions.push_back(center);
            Sys_Printf("     No env_cubemap entities, using world center\n");
        } else {
            // Fallback if no geometry
            cubemapPositions.push_back(Vector3(0, 0, 0));
            Sys_Printf("     No geometry, using origin\n");
        }
    } else {
        Sys_Printf("     Found %zu env_cubemap entities\n", cubemapPositions.size());
    }
    
    // Emit cubemap samples
    for (const Vector3 &pos : cubemapPositions) {
        CubemapSample_t sample;
        // Engine reads origin as int32[3] and converts to float
        sample.origin[0] = static_cast<int32_t>(pos[0]);
        sample.origin[1] = static_cast<int32_t>(pos[1]);
        sample.origin[2] = static_cast<int32_t>(pos[2]);
        sample.guid = 0;  // No pre-baked texture, runtime capture required
        ApexLegends::Bsp::cubemaps.push_back(sample);
        
        // Ambient RCP: reciprocal of ambient contribution
        // Default to 1.0 (full ambient)
        ApexLegends::Bsp::cubemapsAmbientRcp.push_back(1.0f);
    }
    
    Sys_Printf("     %9zu cubemap samples\n", ApexLegends::Bsp::cubemaps.size());
}

/*
    EmitWorldLights
    Emits world lights from entities into the BSP worldlights lump
    
    Light ordering is critical for the engine:
    1. emit_skyambient lights must be at the beginning
    2. emit_skylight lights must follow directly after emit_skyambient lights
    3. Other lights (spotlight, point) come after
    
    Each light_environment entity creates TWO lights:
    - One emit_skyambient (ambient sky lighting)
    - One emit_skylight (directional sun lighting)
*/
void ApexLegends::EmitWorldLights() {
    Sys_Printf("--- EmitWorldLights ---\n");
    ApexLegends::Bsp::worldLights.clear();

    // Separate lists for proper ordering
    std::vector<WorldLight_t> skyAmbientLights;
    std::vector<WorldLight_t> skyLights;
    std::vector<WorldLight_t> otherLights;

    for (const entity_t& e : entities) {
        const char* classname = e.classname();

        // Check for light entities
        bool isLightEnvironment = false;
        emittype_t type = emit_surface;

        if (striEqual(classname, "light")) {
            const char* spawnflags = e.valueForKey("spawnflags");
            if (spawnflags && atoi(spawnflags) & 1) {
                type = emit_spotlight;
            } else {
                type = emit_point;
            }
        } else if (striEqual(classname, "light_spotlight") || striEqual(classname, "light_spot")) {
            type = emit_spotlight;
        } else if (striEqual(classname, "light_environment")) {
            isLightEnvironment = true;
        } else {
            continue;
        }

        // Get common light properties
        Vector3 origin = e.vectorForKey("origin");

        // Get angles for normal/direction
        // light_environment uses "angles" key with format "pitch yaw roll"
        // or separate "pitch" key. The pitch key seems to be the primary one.
        Vector3 angles = e.vectorForKey("angles");
        float pitch = e.floatForKey("pitch", "0");
        // If pitch key exists, it takes precedence (and is often the negative)
        if (e.valueForKey("pitch")) {
            angles[0] = -pitch;  // pitch key is negative of the actual pitch
        }
        Vector3 normal = ApexLegends::vector3_from_angles(angles);

        // Light color/intensity - format is "R G B brightness" (4 values)
        // Final intensity = (R * brightness) / (255 * 255)
        Vector3 intensity(1.0f, 1.0f, 1.0f);
        const char* lightVal = e.valueForKey("_light");
        if (lightVal) {
            float r, g, b, brightness = 255.0f;
            if (sscanf(lightVal, "%f %f %f %f", &r, &g, &b, &brightness) >= 3) {
                // Scale by brightness and normalize
                intensity = Vector3(r, g, b) * (brightness / 65025.0f);  // 65025 = 255*255
            }
        }

        if (isLightEnvironment) {
            // light_environment creates TWO lights: emit_skyambient and emit_skylight
            
            // Get ambient color - format is "R G B brightness" (4 values)
            Vector3 ambientIntensity = intensity * 0.1f;  // Default to 10% of sun if not specified
            const char* ambientVal = e.valueForKey("_ambient");
            if (ambientVal) {
                float r, g, b, brightness = 255.0f;
                if (sscanf(ambientVal, "%f %f %f %f", &r, &g, &b, &brightness) >= 3) {
                    ambientIntensity = Vector3(r, g, b) * (brightness / 65025.0f);
                }
            }

            // Sun highlight size for skylight
            float sunHighlightSize = e.floatForKey("SunSpreadAngle", "0");

            // 1. Create emit_skyambient light (ambient sky lighting)
            WorldLight_t ambientLight = {};
            ambientLight.origin = origin;
            ambientLight.intensity = ambientIntensity;
            ambientLight.normal = Vector3(0.0f, 0.0f, 0.0f);
            ambientLight.shadowUpresFactor = 0;
            ambientLight.shadowFilterSize = 0;
            ambientLight.shadowBias = 0.0f;
            ambientLight.bounceBoost = 0.0f;
            ambientLight.type = emit_skyambient;
            ambientLight.style = 0;
            ambientLight.stopdot = 0.0f;
            ambientLight.stopdot2 = 0.0f;
            ambientLight.exponent = 0.0f;
            ambientLight.radius = 0.0f;
            ambientLight.constant_attn = 0.0f;
            ambientLight.linear_attn = 0.0f;
            ambientLight.quadratic_attn = 0.0f;
            ambientLight.flags = 0;
            ambientLight.texdata = 0;
            ambientLight.owner = 0;
            ambientLight.emitter_radius = 0.0f;
            ambientLight.sun_highlight_size_or_vfog_boost = 0.0f;
            ambientLight.specular_intensity = 0.0f;
            skyAmbientLights.push_back(ambientLight);

            // 2. Create emit_skylight (directional sun lighting)
            WorldLight_t sunLight = {};
            sunLight.origin = origin;
            sunLight.intensity = intensity;
            sunLight.normal = normal;  // Sun direction
            sunLight.shadowUpresFactor = 0;
            sunLight.shadowFilterSize = 0;
            sunLight.shadowBias = 0.0f;
            sunLight.bounceBoost = 0.0f;
            sunLight.type = emit_skylight;
            sunLight.style = 0;
            sunLight.stopdot = 0.0f;
            sunLight.stopdot2 = 0.0f;
            sunLight.exponent = 0.0f;
            sunLight.radius = 0.0f;
            sunLight.constant_attn = 0.0f;
            sunLight.linear_attn = 0.0f;
            sunLight.quadratic_attn = 0.0f;
            sunLight.flags = 0x18;  // skylight uses flags 0x18
            sunLight.texdata = 0;
            sunLight.owner = 0;
            sunLight.emitter_radius = 0.0f;
            sunLight.sun_highlight_size_or_vfog_boost = sunHighlightSize;
            sunLight.specular_intensity = 0.0f; 
            skyLights.push_back(sunLight);

        } else {
            // Regular light (point, spotlight)
            // Parse shadow parameters
            int shadowUpres = e.intForKey("shadow_upres");
            int shadowFilterSize = e.intForKey("shadow_filter_size");
            float shadowBias = e.floatForKey("shadow_bias");
            
            // Parse light parameters
            float emitterRadius = e.floatForKey("_emitter_radius");
            float specularIntensity = e.floatForKey("_specular_intensity");
            if (specularIntensity == 0.0f && !e.valueForKey("_specular_intensity")) {
                specularIntensity = 1.0f;  // Default to 1.0 if not specified
            }
            float exponent = e.floatForKey("_exponent");
            if (exponent == 0.0f && !e.valueForKey("_exponent")) {
                exponent = 1.0f;  // Default to 1.0 if not specified
            }
            int style = e.intForKey("style");
            
            // Parse distance/attenuation - Apex uses distance-based falloff
            float distance = e.floatForKey("_distance");
            float fiftyPercent = e.floatForKey("_fifty_percent_distance");
            float zeroPercent = e.floatForKey("_zero_percent_distance");
            
            // Calculate attenuation from distance parameters if provided
            // Otherwise use explicit attenuation values
            float constantAttn = e.floatForKey("_constant_attn");
            float linearAttn = e.floatForKey("_linear_attn");
            float quadraticAttn = e.floatForKey("_quadratic_attn");
            if (quadraticAttn == 0.0f && !e.valueForKey("_quadratic_attn")) {
                quadraticAttn = 1.0f;  // Default to 1.0 if not specified
            }
            
            // If distance-based falloff is specified, convert to attenuation
            if (fiftyPercent > 0 && zeroPercent > 0) {
                // Use distance values for radius calculation
                // The radius field stores the zero percent distance
                distance = zeroPercent;
            }
            
            // Parse flags from entity properties
            int flags = 0;
            bool isRealtime = e.intForKey("realtime") != 0;
            bool hasRealtimeShadows = e.intForKey("realtime_shadows") != 0;
            bool isPbrFalloff = e.intForKey("_pbr_falloff") != 0 || e.valueForKey("_pbr_falloff") == nullptr;  // Default to PBR
            bool isTweakable = e.intForKey("tweakable") != 0;
            
            if (isRealtime) flags |= WORLDLIGHT_FLAG_REALTIME;
            if (hasRealtimeShadows) flags |= WORLDLIGHT_FLAG_REALTIME_SHADOWS;
            if (isPbrFalloff) flags |= WORLDLIGHT_FLAG_PBR_FALLOFF;
            if (isTweakable || isRealtime) flags |= WORLDLIGHT_FLAG_TWEAK;  // Realtime lights are tweakable
            
            // Add additional common flags based on official maps (bit 3 and 4 often set)
            flags |= 0x08;  // Bit 3 - commonly set in official maps
            flags |= 0x10;  // Bit 4 - commonly set in official maps
            
            WorldLight_t light = {};
            light.origin = origin;
            light.intensity = intensity;
            light.normal = normal;
            light.shadowUpresFactor = shadowUpres;
            light.shadowFilterSize = shadowFilterSize;
            light.shadowBias = shadowBias;
            light.bounceBoost = 0.0f;
            light.type = type;
            light.style = style;
            light.texdata = 0;
            light.owner = 0;
            light.emitter_radius = emitterRadius;
            light.sun_highlight_size_or_vfog_boost = 0.0f;
            light.specular_intensity = specularIntensity;

            if (type == emit_spotlight) {
                float innerCone = e.floatForKey("_inner_cone");
                float outerCone = e.floatForKey("_cone");
                // Defaults if not specified
                if (innerCone == 0.0f && !e.valueForKey("_inner_cone")) innerCone = 40.0f;
                if (outerCone == 0.0f && !e.valueForKey("_cone")) outerCone = 45.0f;
                light.stopdot = std::cos(degrees_to_radians(innerCone));
                light.stopdot2 = std::cos(degrees_to_radians(outerCone));
                light.exponent = exponent;
                light.constant_attn = constantAttn;
                light.linear_attn = linearAttn;
                light.quadratic_attn = quadraticAttn;
            } else {
                // Point light
                light.stopdot = 0.0f;
                light.stopdot2 = 0.0f;
                light.exponent = exponent;
                light.constant_attn = constantAttn;
                light.linear_attn = linearAttn;
                light.quadratic_attn = quadraticAttn;
            }

            light.radius = distance > 0 ? distance : e.floatForKey("_radius");
            light.flags = flags;

            otherLights.push_back(light);
        }
    }

    // Build final list in correct order:
    // 1. All emit_skyambient lights first
    // 2. All emit_skylight lights second (must match count of skyambient)
    // 3. All other lights after
    for (const auto& light : skyAmbientLights) {
        ApexLegends::Bsp::worldLights.push_back(light);
    }
    for (const auto& light : skyLights) {
        ApexLegends::Bsp::worldLights.push_back(light);
    }
    for (const auto& light : otherLights) {
        ApexLegends::Bsp::worldLights.push_back(light);
    }

    // Generate tweak lights list - indices of all lights with WORLDLIGHT_FLAG_TWEAK set
    ApexLegends::Bsp::tweakLights.clear();
    for (uint32_t i = 0; i < ApexLegends::Bsp::worldLights.size(); i++) {
        if (ApexLegends::Bsp::worldLights[i].flags & WORLDLIGHT_FLAG_TWEAK) {
            ApexLegends::Bsp::tweakLights.push_back(i);
        }
    }

    Sys_Printf("     %9zu world lights\n", ApexLegends::Bsp::worldLights.size());
    Sys_Printf("     %9zu light environments\n", skyAmbientLights.size());
    Sys_Printf("     %9zu point/spot lights\n", otherLights.size());
    Sys_Printf("     %9zu tweakable lights\n", ApexLegends::Bsp::tweakLights.size());
}

/*
    EmitShadowMeshes
    Generates shadow mesh data from the world geometry.
    Shadow meshes are simplified versions of world geometry used for cascaded shadow maps.
    
    This function creates:
    - Shadow mesh opaque vertices (lump 0x7C) - vertex positions only
    - Shadow mesh indices (lump 0x7E) - triangle indices
    - Shadow meshes (lump 0x7F) - mesh descriptors
    - CSM AABB nodes (lump 0x63) - bounding volume hierarchy for shadow culling
    
    Must be called AFTER EmitMeshes() since we use the mesh data.
*/
void ApexLegends::EmitShadowMeshes() {
    Sys_Printf("--- EmitShadowMeshes ---\n");
    
    // Clear any existing data
    ApexLegends::Bsp::shadowMeshOpaqueVerts.clear();
    ApexLegends::Bsp::shadowMeshAlphaVerts.clear();
    ApexLegends::Bsp::shadowMeshIndices.clear();
    ApexLegends::Bsp::shadowMeshes.clear();
    ApexLegends::Bsp::csmAABBNodes.clear();
    ApexLegends::Bsp::csmObjRefsTotal.clear();
    ApexLegends::Bsp::csmNumObjRefsTotalForAabb.clear();
    
    // We'll create one shadow mesh per world mesh (model 0)
    // For simplicity, we iterate the meshes from model 0 (worldspawn)
    
    if (ApexLegends::Bsp::models.empty()) {
        Sys_Printf("  No models, skipping shadow mesh generation\n");
        return;
    }
    
    const auto& worldModel = ApexLegends::Bsp::models[0];
    
    // Get the base vertex data - we need to collect vertices from the different vertex formats
    // For shadow meshes, we only need position data
    
    uint32_t totalTriangles = 0;
    uint32_t totalVertices = 0;
    
    // Calculate world bounds for CSM AABB using MinMax class
    MinMax worldBounds;
    
    // Iterate through world meshes and generate shadow geometry
    for (int32_t meshIdx = worldModel.meshIndex; meshIdx < worldModel.meshIndex + worldModel.meshCount; meshIdx++) {
        if (meshIdx < 0 || meshIdx >= static_cast<int32_t>(ApexLegends::Bsp::meshes.size())) {
            continue;
        }
        
        const auto& mesh = ApexLegends::Bsp::meshes[meshIdx];
        
        // Skip meshes with no triangles
        if (mesh.triCount == 0) {
            continue;
        }
        
        // Get the vertex offset from the material sort entry
        // The mesh's materialOffset indexes into the materialSorts array
        uint32_t vertexOffset = 0;
        if (mesh.materialOffset < ApexLegends::Bsp::materialSorts.size()) {
            vertexOffset = ApexLegends::Bsp::materialSorts[mesh.materialOffset].vertexOffset;
        }
        
        // Get the vertices for this mesh based on vertex type
        // The mesh references indices into the meshIndices array, which point to vertices
        uint32_t firstShadowVert = static_cast<uint32_t>(ApexLegends::Bsp::shadowMeshOpaqueVerts.size());
        uint32_t firstShadowIndex = static_cast<uint32_t>(ApexLegends::Bsp::shadowMeshIndices.size());
        
        // Map from original vertex index to shadow vertex index
        std::unordered_map<uint16_t, uint16_t> vertexRemap;
        
        // Process indices for this mesh
        uint32_t indexStart = mesh.triOffset;
        uint32_t indexCount = mesh.triCount * 3;  // triCount is triangle count, need 3 indices per tri
        
        for (uint32_t i = 0; i < indexCount; i++) {
            if (indexStart + i >= Titanfall::Bsp::meshIndices.size()) {
                break;
            }
            
            uint16_t origIdx = Titanfall::Bsp::meshIndices[indexStart + i];
            
            // Check if we've already remapped this vertex
            auto it = vertexRemap.find(origIdx);
            if (it != vertexRemap.end()) {
                ApexLegends::Bsp::shadowMeshIndices.push_back(it->second);
            } else {
                // Add new shadow vertex
                uint16_t newIdx = static_cast<uint16_t>(ApexLegends::Bsp::shadowMeshOpaqueVerts.size() - firstShadowVert);
                vertexRemap[origIdx] = newIdx;
                
                // Get the vertex position based on vertex type
                Vector3 pos(0, 0, 0);
                
                // The index in meshIndices is relative to the materialSort's vertexOffset
                // Add vertexOffset to get the actual vertex index in the vertex format array
                uint32_t actualIdx = vertexOffset + origIdx;
                
                // The vertex format structs (VertexLitBump_t, etc.) contain a vertexIndex
                // which points to the actual position in Titanfall::Bsp::vertices
                uint32_t positionIndex = 0;
                
                // Vertex type bits in mesh.flags:
                // S_VERTEX_LIT_FLAT  = 0x000
                // S_VERTEX_LIT_BUMP  = 0x200
                // S_VERTEX_UNLIT     = 0x400
                // S_VERTEX_UNLIT_TS  = 0x600
                switch (mesh.flags & 0x600) {
                    case 0x000:  // S_VERTEX_LIT_FLAT
                        if (actualIdx < ApexLegends::Bsp::vertexLitFlatVertices.size()) {
                            positionIndex = ApexLegends::Bsp::vertexLitFlatVertices[actualIdx].vertexIndex;
                        }
                        break;
                    case 0x200:  // S_VERTEX_LIT_BUMP
                        if (actualIdx < ApexLegends::Bsp::vertexLitBumpVertices.size()) {
                            positionIndex = ApexLegends::Bsp::vertexLitBumpVertices[actualIdx].vertexIndex;
                        }
                        break;
                    case 0x400:  // S_VERTEX_UNLIT
                        if (actualIdx < ApexLegends::Bsp::vertexUnlitVertices.size()) {
                            positionIndex = ApexLegends::Bsp::vertexUnlitVertices[actualIdx].vertexIndex;
                        }
                        break;
                    case 0x600:  // S_VERTEX_UNLIT_TS
                        if (actualIdx < ApexLegends::Bsp::vertexUnlitTSVertices.size()) {
                            positionIndex = ApexLegends::Bsp::vertexUnlitTSVertices[actualIdx].vertexIndex;
                        }
                        break;
                    default:
                        // Fallback: use index directly
                        positionIndex = origIdx;
                        break;
                }
                
                // Get the actual position from the vertices array
                if (positionIndex < Titanfall::Bsp::vertices.size()) {
                    pos = Titanfall::Bsp::vertices[positionIndex];
                }
                
                // Update world bounds
                worldBounds.extend(pos);
                
                ApexLegends::Bsp::shadowMeshOpaqueVerts.push_back(pos);
                ApexLegends::Bsp::shadowMeshIndices.push_back(newIdx);
            }
        }
        
        // Create the shadow mesh entry if we added any geometry
        uint32_t shadowTriCount = (static_cast<uint32_t>(ApexLegends::Bsp::shadowMeshIndices.size()) - firstShadowIndex) / 3;
        if (shadowTriCount > 0) {
            ShadowMesh_t shadowMesh = {};
            shadowMesh.firstVertex = firstShadowVert;
            shadowMesh.triangleCount = shadowTriCount;
            shadowMesh.drawType = 1;  // Opaque
            shadowMesh.materialSortIdx = 0xFFFF;  // No material
            
            ApexLegends::Bsp::shadowMeshes.push_back(shadowMesh);
            totalTriangles += shadowTriCount;
        }
    }
    
    totalVertices = static_cast<uint32_t>(ApexLegends::Bsp::shadowMeshOpaqueVerts.size());
    
    // Create CSM object references - one reference per shadow mesh
    // The CSM system uses this to know which meshes to render for shadows
    for (uint32_t i = 0; i < ApexLegends::Bsp::shadowMeshes.size(); i++) {
        ApexLegends::Bsp::csmObjRefsTotal.push_back(i);
    }

    // Force large bounds to disable culling for debugging
    Vector3 largeMin(-50000.0f, -50000.0f, -50000.0f);
    Vector3 largeMax(50000.0f, 50000.0f, 50000.0f);
    
    // Create a single root CSM AABB node that encompasses all geometry
    if (!ApexLegends::Bsp::shadowMeshOpaqueVerts.empty()) {
        // Create root node with world bounds
        CSMAABBNode_t rootNode = {};

        //TODO: Use actual world bounds once CSM culling is verified working
        rootNode.mins = largeMin; //worldBounds.mins;
        rootNode.maxs = largeMax; //worldBounds.maxs;
        
        // CSM AABB node format (based on engine decompilation):
        // child0 (offset 0x0C):
        //   - Bits 0-7: Number of child nodes (0 for leaf)
        //   - Bits 8-31: First child node index
        // child1 (offset 0x1C):
        //   - Bits 0-7: Object ref count (for leaf nodes when child0 low byte is 0)
        //   - Bits 8-30: First obj ref index
        //
        // For a leaf node: child0 = 0, child1 = (startIndex << 8) | count
        
        uint32_t objRefCount = static_cast<uint32_t>(ApexLegends::Bsp::csmObjRefsTotal.size());
        uint32_t startIndex = 0;
        
        rootNode.child0 = 0;  // 0 children (leaf node)
        rootNode.child1 = (startIndex << 8) | (objRefCount & 0xFF);  // start index in high bits, count in low byte
        
        ApexLegends::Bsp::csmAABBNodes.push_back(rootNode);
        
        // numObjRefsTotalForAabb is used for internal nodes (when child0 low byte > 0)
        // For leaf nodes the count comes from child1's low byte, but we still need
        // an entry in this array for each node
        ApexLegends::Bsp::csmNumObjRefsTotalForAabb.push_back(objRefCount);
    }
    
    Sys_Printf("     %9zu shadow meshes\n", ApexLegends::Bsp::shadowMeshes.size());
    Sys_Printf("     %9u triangles\n", totalTriangles);
    Sys_Printf("     %9u vertices\n", totalVertices);
    Sys_Printf("     %9zu CSM AABB nodes\n", ApexLegends::Bsp::csmAABBNodes.size());
    Sys_Printf("     %9zu CSM obj refs\n", ApexLegends::Bsp::csmObjRefsTotal.size());
}

/*
    EmitShadowEnvironments
    Generates the shadow environment data for each light_environment.
    The shadow environment defines the sun direction for cascaded shadow maps.
    
    Must be called AFTER EmitWorldLights() since we iterate through the worldLights
    to find emit_skylight entries which contain the sun direction.
*/
void ApexLegends::EmitShadowEnvironments() {
    Sys_Printf("--- EmitShadowEnvironments ---\n");
    ApexLegends::Bsp::shadowEnvironments.clear();

    // Count light environments by finding emit_skyambient lights
    // (each light_environment creates one emit_skyambient + one emit_skylight)
    size_t numLightEnvironments = 0;
    for (const auto& light : ApexLegends::Bsp::worldLights) {
        if (light.type == emit_skyambient) {
            numLightEnvironments++;
        }
    }

    // Get the shadow mesh data counts for reference
    uint32_t numShadowMeshes = static_cast<uint32_t>(ApexLegends::Bsp::shadowMeshes.size());
    uint32_t numCSMNodes = static_cast<uint32_t>(ApexLegends::Bsp::csmAABBNodes.size());
    uint32_t numCSMObjRefs = static_cast<uint32_t>(ApexLegends::Bsp::csmObjRefsTotal.size());

    if (numLightEnvironments == 0) {
        // No light_environment entities - create a default one
        // This is required for the engine to work properly
        Sys_Printf("  No light_environment found, creating default shadow environment\n");
        
        ShadowEnvironment_t defaultEnv = {};
        defaultEnv.beginAabbs = 0;
        defaultEnv.beginObjRefs = 0;
        defaultEnv.beginShadowMeshes = 0;
        defaultEnv.endAabbs = numCSMNodes;
        defaultEnv.endObjRefs = numCSMObjRefs;
        defaultEnv.endShadowMeshes = numShadowMeshes;
        // Default sun direction (pointing down and slightly south-west)
        defaultEnv.shadowDir = Vector3(0.5227f, 0.2733f, -0.8072f);
        
        ApexLegends::Bsp::shadowEnvironments.push_back(defaultEnv);
    } else {
        // Create shadow environment for each light_environment
        // The shadowDir in the shadow environment should match the light's normal
        // (the direction light travels, i.e., from sun towards ground)
        
        for (const auto& light : ApexLegends::Bsp::worldLights) {
            if (light.type == emit_skylight) {
                ShadowEnvironment_t env = {};
                env.beginAabbs = 0;
                env.beginObjRefs = 0;
                env.beginShadowMeshes = 0;
                env.endAabbs = numCSMNodes;
                env.endObjRefs = numCSMObjRefs;
                env.endShadowMeshes = numShadowMeshes;
                
                // shadowDir is the direction light travels (from sun towards ground)
                // This matches the worldlight's normal direction
                env.shadowDir = light.normal;
                
                ApexLegends::Bsp::shadowEnvironments.push_back(env);
            }
        }
    }

    Sys_Printf("     %9zu shadow environments\n", ApexLegends::Bsp::shadowEnvironments.size());
}
