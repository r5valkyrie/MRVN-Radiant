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
    Apex Legends Vertex Lumps

    This file handles vertex-related BSP lumps:
    - Vertex Unlit lump (0x47): Unlit vertices
    - Vertex Lit Flat lump (0x48): Flat lit vertices
    - Vertex Lit Bump lump (0x49): Bump mapped lit vertices
    - Vertex Unlit TS lump (0x4A): Unlit with tangent space
    - Vertex Normals lump (0x04): Vertex normals
*/

#include "../remap.h"
#include "../bspfile_abstract.h"

/*
    EmitVertexUnlit
    Saves a vertex into ApexLegends::Bsp::vertexUnlitVertices

    Unlit vertex format:
    - vertexIndex: Index into vertices lump
    - normalIndex: Index into vertex normals lump
    - uv0: Texture coordinates
    - negativeOne: Always -1 (unused)
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

    Lit Flat vertex format:
    - vertexIndex: Index into vertices lump
    - normalIndex: Index into vertex normals lump
    - uv0: Texture coordinates
    - unknown0: Unknown (usually 0)
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

    Lit Bump vertex format:
    - vertexIndex: Index into vertices lump
    - normalIndex: Index into vertex normals lump
    - uv0: Texture coordinates
    - negativeOne: Special marker value (0x00FFFFFF, NOT -1)
    - uv1: Lightmap coordinates
    - normalIndex2: Second normal index with 0x80000000 flag
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

    Unlit TS vertex format:
    - vertexIndex: Index into vertices lump
    - normalIndex: Index into vertex normals lump
    - uv0: Texture coordinates
    - unknown0: Special marker value (0x00FFFF01)
    - unknown1: Unknown (usually 0)
*/
void ApexLegends::EmitVertexUnlitTS(Shared::Vertex_t &vertex) {
    ApexLegends::VertexUnlitTS_t& ul = ApexLegends::Bsp::vertexUnlitTSVertices.emplace_back();
    ul.vertexIndex = Titanfall::EmitVertex(vertex.xyz);
    ul.normalIndex = Titanfall::EmitVertexNormal(vertex.normal);
    ul.uv0         = vertex.textureUV;
    ul.unknown0    = 0x00FFFF01;  // Special marker value (matches official BSP)
    ul.unknown1    = 0;
}
