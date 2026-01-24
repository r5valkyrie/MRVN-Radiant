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

/*
    EmitEntity()
    Saves an entity into its corresponding .ent file or the lump in the .bsp
    
    Entities are categorized into different files based on their classname:
    - env: lights, fog, sky, color entities
    - fx: particle effects
    - script: info_target, prop_dynamic, trigger_hurt
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

    Sys_FPrintf(SYS_VRB, "  Emitting static prop: %s\n", model);

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
    
    // Apex uses Source engine style angles: pitch (X), yaw (Z), roll (Y)
    // GameLumpProp_t stores angles directly as Vector3
    prop.angles = angles;
    
    prop.origin = origin;
    prop.scale = e.floatForKey("scale", "1.0");
    prop.modelName = pathIdx;
    prop.solid = 0;
    prop.flags = 0;
    prop.skin = 0;
    prop.envCubemap = 0;
    prop.fade_scale = 1.0f;
    prop.unk = Vector3(0, 0, 0);
    prop.cpu_level[0] = -1;
    prop.cpu_level[1] = -1;
    prop.gpu_level[0] = -1;
    prop.gpu_level[1] = -1;
    prop.diffuse_modulation[0] = 255;
    prop.diffuse_modulation[1] = 255;
    prop.diffuse_modulation[2] = 255;
    prop.diffuse_modulation[3] = 255;
    prop.collision_flags[0] = 0;
    prop.collision_flags[1] = 0;
}

/*
    SetupGameLump()
    Initializes the GameLump header data
*/
void ApexLegends::SetupGameLump() {
    Titanfall2::Bsp::gameLumpHeader.version = 1;
    memcpy(Titanfall2::Bsp::gameLumpHeader.ident, "prps", 4);
    Titanfall2::Bsp::gameLumpHeader.gameConst = 851968;  // Apex Legends constant
    
    Titanfall2::Bsp::gameLumpPathHeader.numPaths = 0;
    
    Titanfall2::Bsp::gameLumpPropHeader.numProps = 0;
    Titanfall2::Bsp::gameLumpPropHeader.unk0 = 0;
    Titanfall2::Bsp::gameLumpPropHeader.unk1 = 0;
    
    Titanfall2::Bsp::gameLumpUnknownHeader.unknown = 0;
}
