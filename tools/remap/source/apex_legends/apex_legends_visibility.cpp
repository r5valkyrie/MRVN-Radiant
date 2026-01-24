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
    Apex Legends Visibility/Cell Lumps

    This file handles visibility and cell-related BSP lumps:
    - Cell AABB Nodes lump (0x77): BVH nodes for visibility culling
    - Object References lump (0x78): Object reference indices
    - Object Reference Bounds lump (0x79): Bounds per object reference
    - Cell AABB Num Obj Refs Total lump (0x25): Cumulative obj ref count per node
    - Cell AABB Fade Dists lump (0x27): Fade distances for LOD/streaming
*/

#include "../remap.h"
#include "../bspfile_abstract.h"
#include "../model.h"

/*
    EmitVisTree
    Emits the vistree to the bsp file
    
    Generates:
    - cellAABBNodes (lump 0x77): BVH nodes for visibility culling
    - objReferences (lump 0x78): Object reference indices
    - objReferenceBounds (lump 0x79): Bounds per object reference
    - cellAABBNumObjRefsTotal (lump 0x25): Cumulative obj ref count per node
    - cellAABBFadeDists (lump 0x27): Fade distances for LOD/streaming
*/
void ApexLegends::EmitVisTree() {
    Shared::visNode_t &root = Shared::visRoot;

    //TODO: Use actual world bounds once culling is verified working
    // Force large bounds to disable culling for debugging
    Vector3 largeMin(-50000.0f, -50000.0f, -50000.0f);
    Vector3 largeMax(50000.0f, 50000.0f, 50000.0f);

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
    
    // Emit all references with large bounds and fade distances
    for (Shared::visRef_t &ref : Shared::visRefs) {
        Titanfall::ObjReferenceBounds_t &rb = Titanfall::Bsp::objReferenceBounds.emplace_back();
        rb.maxs = largeMax;
        rb.mins = largeMin;

        ApexLegends::Bsp::objReferences.emplace_back(ref.index);
        
        // Fade distance: 0xFFFF = maximum draw distance (no fade)
        ApexLegends::Bsp::cellAABBFadeDists.push_back(0xFFFF);
    }
    
    // Emit cumulative obj ref count for each AABB node
    // For a single root node, this is just the total count
    uint32_t numObjRefs = static_cast<uint32_t>(Shared::visRefs.size());
    ApexLegends::Bsp::cellAABBNumObjRefsTotal.push_back(numObjRefs);
}

/*
    EmitObjReferences
    Emits object references to the BSP
    
    This function emits object references for a visibility node.
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

/*
    EmitVisChildrenOfTreeNode
    Emits visibility tree children recursively
    
    This function builds the visibility tree structure by recursively
    emitting child nodes.
*/
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
