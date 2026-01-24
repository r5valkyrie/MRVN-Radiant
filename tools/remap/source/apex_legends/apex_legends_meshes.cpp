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
    Apex Legends Mesh Lumps

    This file handles mesh-related BSP lumps:
    - Meshes lump (0x50): Mesh descriptors
    - Mesh Indices lump (0x51): Triangle indices
    - Mesh Bounds lump (0x52): Per-mesh bounding boxes
*/

#include "../remap.h"
#include "../bspfile_abstract.h"
#include <algorithm>

/*
    EmitMeshes()
    Writes the mesh list to the BSP

    This function processes all meshes from the entity and emits:
    - Mesh descriptors with vertex type and material info
    - Vertex data in the appropriate format (unlit, lit flat, lit bump, unlit ts)
    - Triangle indices
    - Mesh bounds
*/
void ApexLegends::EmitMeshes(const entity_t &e) {
    // Setup lightmaps first so we can get UV coordinates for lit vertices
    ApexLegends::SetupSurfaceLightmaps();

    Sys_FPrintf(SYS_VRB, "--- Emitting Meshes ---\n");

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
        // Get lightmap page index for this mesh (returns 0 for stub page if no allocation)
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

    Sys_FPrintf(SYS_VRB, "  Emitted %zu meshes\n", ApexLegends::Bsp::meshes.size());
    Sys_FPrintf(SYS_VRB, "    VertexUnlit: %zu vertices\n", ApexLegends::Bsp::vertexUnlitVertices.size());
    Sys_FPrintf(SYS_VRB, "    VertexLitFlat: %zu vertices\n", ApexLegends::Bsp::vertexLitFlatVertices.size());
    Sys_FPrintf(SYS_VRB, "    VertexLitBump: %zu vertices\n", ApexLegends::Bsp::vertexLitBumpVertices.size());
    Sys_FPrintf(SYS_VRB, "    VertexUnlitTS: %zu vertices\n", ApexLegends::Bsp::vertexUnlitTSVertices.size());
}
