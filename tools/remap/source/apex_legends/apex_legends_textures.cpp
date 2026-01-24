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
    Apex Legends Texture/Material Lumps

    This file handles texture and material-related BSP lumps:
    - Texture Data lump (0x02): Texture information
    - Surface Names lump (0x0F): Texture name strings
    - Material Sort lump (0x52): Material sorting data
*/

#include "../remap.h"
#include "../bspfile_abstract.h"
#include <algorithm>
#include <string>

/*
    EmitTextureData()
    Emits texture data and returns its index

    Texture data includes:
    - surfaceIndex: Index into surface names lump
    - sizeX, sizeY: Texture dimensions
    - flags: Surface flags
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

    Material sort entries group vertices by texture and lightmap for efficient rendering.
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
