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
#include <ctime>

/*
    BeginModel
*/
void ApexLegends::BeginModel(entity_t &entity) {
    // Clear surface properties for first model (worldspawn)
    if (ApexLegends::Bsp::models.empty()) {
        ApexLegends::Bsp::surfaceProperties.clear();
        
        // Emit a default surface property at index 0 for fallback
        // This ensures there's always at least one valid entry
        ApexLegends::CollSurfProps_t defaultProp;
        defaultProp.surfFlags = 0;
        defaultProp.surfTypeID = 0;  // Default surface type
        defaultProp.contentsIdx = 0; // Will reference CONTENTS_SOLID
        defaultProp.nameOffset = 0;  // Will reference first string in Surface Names
        ApexLegends::Bsp::surfaceProperties.push_back(defaultProp);
    }
    
    ApexLegends::Model_t &model = ApexLegends::Bsp::models.emplace_back();
    model.meshIndex = ApexLegends::Bsp::meshes.size();
    model.bvhNodeIndex = 0;  // Will be set by EmitBVHNode
    model.bvhLeafIndex = 0;  // Will be set by EmitBVHNode
    model.vertexIndex = 0;   // Will be set by EmitBVHNode (packed vertex base)
    model.bvhFlags = 0;      // Will be set by EmitBVHNode
    model.origin[0] = model.origin[1] = model.origin[2] = 0.0f;
    model.scale = 1.0f / 65536.0f;  // Default scale
}

/*
    EndModel
*/
void ApexLegends::EndModel() {
    ApexLegends::Model_t &model = ApexLegends::Bsp::models.back();
    model.meshCount = ApexLegends::Bsp::meshes.size() - model.meshIndex;

    // Only include mesh bounds for meshes belonging to this model
    for( int32_t i = model.meshIndex; i < model.meshIndex + model.meshCount; i++ ) {
        Titanfall::MeshBounds_t &meshBounds = Titanfall::Bsp::meshBounds.at(i);
        model.minmax.extend(meshBounds.origin - meshBounds.extents);
        model.minmax.extend(meshBounds.origin + meshBounds.extents);
    }
}