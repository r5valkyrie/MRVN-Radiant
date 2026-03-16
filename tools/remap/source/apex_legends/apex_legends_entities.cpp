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
    Apex Legends Entity Lumps

    This file handles entity-related BSP lumps:
    - Entities lump (0x00): Entity definitions
    - GameLump: Static props and paths
*/

#include "../remap.h"
#include "../bspfile_abstract.h"
#include "../model.h"
#include <cstring>
#include <cmath>
#include <algorithm>

/*
    EntityGoesToBSPLump()
    Returns true if the entity will be stored in the BSP entity lump (lump 0x00)
    rather than an external .ent file (_env, _fx, _script, _snd, _spawn).
    
    Entities in .ent files need *coll serialization for collision data;
    entities in the BSP lump get collision via BSP lumps directly.
*/
bool ApexLegends::EntityGoesToBSPLump(const entity_t &e) {
    const char *cls = e.valueForKey("classname");
    
    // Entities that start with "env" but go into BSP lump
    if (striEqual(cls, "envmap_volume")) {
        return true;
    }
    
    // env
    if (striEqualPrefix(cls, "light")
     || striEqualPrefix(cls, "color")
     || striEqualPrefix(cls, "fog")
     || striEqualPrefix(cls, "env")
     || striEqualPrefix(cls, "sky")) {
        return false;
    }
    // fx
    if (striEqualPrefix(cls, "info_particle")) {
        return false;
    }
    // script
    if (striEqualPrefix(cls, "info_target")
     || striEqualPrefix(cls, "prop_dynamic")
     || striEqualPrefix(cls, "trigger_hurt")
     || striEqualPrefix(cls, "trigger_out_of_bounds")
     || striEqualPrefix(cls, "trigger_no_zipline")
     || striEqualPrefix(cls, "trigger_multiple")
     || striEqualPrefix(cls, "trigger_slip")
     || striEqualPrefix(cls, "zipline")
     || striEqualPrefix(cls, "trigger_teleport")
     || striEqualPrefix(cls, "script_mover")) {
        return false;
    }
    // snd
    if (striEqualPrefix(cls, "ambient_generic")
     || striEqualPrefix(cls, "trigger_soundscape")
     || striEqualPrefix(cls, "soundscape_")) {
        return false;
    }
    // spawn
    if (striEqualPrefix(cls, "info_")) {
        return false;
    }
    // Everything else goes to BSP entity lump
    return true;
}

/*
    EmitEntity()
    Saves an entity into its corresponding .ent file or the lump in the .bsp
    
    Entities are categorized into different files based on their classname:
    - env: lights, fog, sky, color entities
    - fx: particle effects
    - script: info_target, prop_dynamic, trigger_hurt
    - snd: ambient and soundscape entities
    - spawn: info_* entities
    - BSP entity lump: all other entities
*/
void ApexLegends::EmitEntity(const entity_t &e) {
    StringOutputStream data;
    data << "{\n";
    for (const epair_t& pair : e.epairs) {
        data << "\"" << pair.key.c_str() << "\" \"" << pair.value.c_str() << "\"\n";
    }
    data << "}\n";

    std::vector<char> str = { data.begin(), data.end() };

    // Entities that start with "env" but go into BSP entity lump
    if (striEqual(e.valueForKey("classname"), "envmap_volume")) {
        Titanfall::Bsp::entities.insert(Titanfall::Bsp::entities.end(), str.begin(), str.end());
    // env
    } else if (striEqualPrefix(e.valueForKey("classname"), "light")
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
            || striEqualPrefix(e.valueForKey("classname"), "trigger_hurt")
            || striEqualPrefix(e.valueForKey("classname"), "trigger_out_of_bounds")
            || striEqualPrefix(e.valueForKey("classname"), "zipline") 
            || striEqualPrefix(e.valueForKey("classname"), "trigger_teleport")
            || striEqualPrefix(e.valueForKey("classname"), "script_mover")
        ) {
        Titanfall::Ent::script.insert(Titanfall::Ent::script.end(), str.begin(), str.end());
        // snd
    } else if (striEqualPrefix(e.valueForKey("classname"), "ambient_generic")
            || striEqualPrefix(e.valueForKey("classname"), "trigger_soundscape")
            || striEqualPrefix(e.valueForKey("classname"), "soundscape_")
        ) {
        Titanfall::Ent::snd.insert(Titanfall::Ent::snd.end(), str.begin(), str.end());
    // spawn
    } else if (striEqualPrefix(e.valueForKey("classname"), "info_")) {
        Titanfall::Ent::spawn.insert(Titanfall::Ent::spawn.end(), str.begin(), str.end());
    // bsp entity lump
    } else {
        Titanfall::Bsp::entities.insert(Titanfall::Bsp::entities.end(), str.begin(), str.end());
    }
}

/*
    EmitStaticProp()
    Emits a static prop into the GameLump

    Static props are compiled into the GameLump section of the BSP.
    Each prop references a model path and has position/rotation data.
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

    // Strip "models/" prefix — Apex game lump paths start at "mdl/"
    const char *lumpPath = model;
    if (striEqualPrefix(lumpPath, "models/"))
        lumpPath += 7;

    Sys_FPrintf(SYS_VRB, "  Emitting static prop: %s\n", lumpPath);

    // Find existing path or add new one
    for(std::size_t i = 0; i < ApexLegends::Bsp::gameLumpPaths.size(); i++) {
        Titanfall::GameLumpPath_t &path = ApexLegends::Bsp::gameLumpPaths.at(i);

        if(!string_compare_nocase(path.path, lumpPath)) {
            pathIdx = i;
            break;
        }
    }

    if(pathIdx == -1) {
        ApexLegends::Bsp::gameLumpPathHeader.numPaths++;
        Titanfall::GameLumpPath_t &path = ApexLegends::Bsp::gameLumpPaths.emplace_back();
        strncpy(path.path, lumpPath, 127);
        path.path[127] = '\0';
        pathIdx = ApexLegends::Bsp::gameLumpPaths.size() - 1;
    }

    // Increment prop counts
    // All remap-compiled props are treated as opaque for now
    ApexLegends::Bsp::gameLumpPropHeader.numStaticProps++;
    ApexLegends::Bsp::gameLumpPropHeader.numOpaqueProps++;
    ApexLegends::Bsp::gameLumpPropHeader.firstTransProp++;

    ApexLegends::GameLumpProp_t &prop = ApexLegends::Bsp::gameLumpProps.emplace_back();
    memset(&prop, 0, sizeof(prop));

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

    // floatForKey/intForKey treat all arguments as key names, not defaults
    float scale = e.floatForKey("modelscale");
    if (scale == 0.0f) scale = 1.0f;
    prop.scale = scale;

    prop.modelName = pathIdx;

    int solid = e.intForKey("solid");
    if (solid == 0) solid = 6;  // Default: SOLID_VPHYSICS
    prop.solid = (uint8_t)solid;

    prop.flags = (uint8_t)e.intForKey("staticPropFlags") | 0x04;  // Bit 2 always set (engine requires for staticPropFlags 0x40)
    prop.skin = (uint16_t)e.intForKey("skin");
    prop.envCubemap = 0xFFFF;       // Use default cubemap

    float fadeDist = e.floatForKey("fademindist");
    if (fadeDist == 0.0f) fadeDist = -1.0f;  // -1 = engine calculates default
    prop.fadeDist = fadeDist;
    // Use prop origin as lighting sample point; set flags bit 1 for custom origin
    prop.flags |= 0x02;
    prop.lightingOrigin = origin;
    prop.diffuseModulation[0] = 255;  // R
    prop.diffuseModulation[1] = 255;  // G
    prop.diffuseModulation[2] = 255;  // B
    prop.diffuseModulation[3] = 255;  // A
    prop.precompiledWind = 0;         // No wind
    prop.setDressLevel = 0;           // Default LOD level

    // Compute world-space AABB for collision BVH integration
    // Only register if the prop has collision (solid != 0)
    if (prop.solid != 0) {
        // Compute local-space AABB from model mesh vertices
        MinMax localBounds;
        localBounds.mins = Vector3( FLT_MAX,  FLT_MAX,  FLT_MAX);
        localBounds.maxs = Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        for (const AssMeshWalker* mesh : meshes) {
            mesh->forEachFace([&](const Vector3 (&xyz)[3], const Vector2 (&)[3]) {
                for (int i = 0; i < 3; i++) {
                    localBounds.mins.x() = std::min(localBounds.mins.x(), xyz[i].x());
                    localBounds.mins.y() = std::min(localBounds.mins.y(), xyz[i].y());
                    localBounds.mins.z() = std::min(localBounds.mins.z(), xyz[i].z());
                    localBounds.maxs.x() = std::max(localBounds.maxs.x(), xyz[i].x());
                    localBounds.maxs.y() = std::max(localBounds.maxs.y(), xyz[i].y());
                    localBounds.maxs.z() = std::max(localBounds.maxs.z(), xyz[i].z());
                }
            });
        }

        if (localBounds.mins.x() <= localBounds.maxs.x()) {
            // Build rotation matrix from Source engine angles (pitch, yaw, roll)
            // R = Rz(yaw) * Ry(pitch) * Rx(roll)
            const float sp = std::sin(degrees_to_radians(angles.x()));
            const float cp = std::cos(degrees_to_radians(angles.x()));
            const float sy = std::sin(degrees_to_radians(angles.y()));
            const float cy = std::cos(degrees_to_radians(angles.y()));
            const float sr = std::sin(degrees_to_radians(angles.z()));
            const float cr = std::cos(degrees_to_radians(angles.z()));

            // Rotation matrix rows (Source AngleMatrix convention)
            // row0: forward
            // row1: right
            // row2: up
            float m[3][3];
            m[0][0] = cp * cy;                     m[0][1] = cp * sy;                     m[0][2] = -sp;
            m[1][0] = sr * sp * cy + cr * (-sy);    m[1][1] = sr * sp * sy + cr * cy;      m[1][2] = sr * cp;
            m[2][0] = cr * sp * cy + (-sr) * (-sy); m[2][1] = cr * sp * sy + (-sr) * cy;   m[2][2] = cr * cp;

            // Transform 8 AABB corners to world space and compute world AABB
            MinMax worldBounds;
            worldBounds.mins = Vector3( FLT_MAX,  FLT_MAX,  FLT_MAX);
            worldBounds.maxs = Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

            for (int corner = 0; corner < 8; corner++) {
                Vector3 local(
                    (corner & 1) ? localBounds.maxs.x() : localBounds.mins.x(),
                    (corner & 2) ? localBounds.maxs.y() : localBounds.mins.y(),
                    (corner & 4) ? localBounds.maxs.z() : localBounds.mins.z()
                );

                // Apply scale
                local *= scale;

                // Apply rotation then translation
                Vector3 world(
                    m[0][0] * local.x() + m[1][0] * local.y() + m[2][0] * local.z() + origin.x(),
                    m[0][1] * local.x() + m[1][1] * local.y() + m[2][1] * local.z() + origin.y(),
                    m[0][2] * local.x() + m[1][2] * local.y() + m[2][2] * local.z() + origin.z()
                );

                worldBounds.mins.x() = std::min(worldBounds.mins.x(), world.x());
                worldBounds.mins.y() = std::min(worldBounds.mins.y(), world.y());
                worldBounds.mins.z() = std::min(worldBounds.mins.z(), world.z());
                worldBounds.maxs.x() = std::max(worldBounds.maxs.x(), world.x());
                worldBounds.maxs.y() = std::max(worldBounds.maxs.y(), world.y());
                worldBounds.maxs.z() = std::max(worldBounds.maxs.z(), world.z());
            }

            uint32_t propIndex = static_cast<uint32_t>(ApexLegends::Bsp::gameLumpProps.size() - 1);
            ApexLegends::AddCollisionStaticProp(propIndex, worldBounds);
        }
    }
}

/*
    SetupGameLump()
    Initializes the GameLump header data
*/
void ApexLegends::SetupGameLump() {
    ApexLegends::Bsp::gameLumpHeader.version = 1;
    memcpy(ApexLegends::Bsp::gameLumpHeader.ident, "prps", 4);
    ApexLegends::Bsp::gameLumpHeader.gameConst = 3080192;  // 0x002F0000 = bspVersion(47) << 16

    ApexLegends::Bsp::gameLumpPathHeader.numPaths = 0;

    ApexLegends::Bsp::gameLumpPropHeader.numStaticProps = 0;
    ApexLegends::Bsp::gameLumpPropHeader.numOpaqueProps = 0;
    ApexLegends::Bsp::gameLumpPropHeader.firstTransProp = 0;
}
