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
    Apex Legends Portals & Cells

    Generates portal and cell data for the BSP PVS (Potentially Visible Set) system.
    Sky portals are created from C_SKY brush faces to enable 3D skybox rendering.
*/

#include "apex_legends.h"
#include "../bspfile_abstract.h"
#include <algorithm>
#include <array>


/*
    EmitPortals()
    Generates sky portals from actual sky-textured brush faces (C_SKY),
    along with all related portal lumps: vertices, edges, vertex-edges,
    vertex-references, edge-references, and edge-intersections.
*/
void ApexLegends::EmitPortals() {
    Sys_FPrintf( SYS_VRB, "--- EmitPortals ---\n" );

    // Helper lambdas for writing binary data
    auto writeFloat = [](std::vector<uint8_t>& vec, float f) {
        uint8_t* p = reinterpret_cast<uint8_t*>(&f);
        vec.insert(vec.end(), p, p + 4);
    };
    auto writeVec3 = [&writeFloat](std::vector<uint8_t>& vec, float x, float y, float z) {
        writeFloat(vec, x);
        writeFloat(vec, y);
        writeFloat(vec, z);
    };
    auto writeU16 = [](std::vector<uint8_t>& vec, uint16_t v) {
        vec.push_back(v & 0xFF);
        vec.push_back((v >> 8) & 0xFF);
    };
    auto writeU32 = [](std::vector<uint8_t>& vec, uint32_t v) {
        uint8_t* p = reinterpret_cast<uint8_t*>(&v);
        vec.insert(vec.end(), p, p + 4);
    };

    // Collect sky faces from worldspawn brushes
    struct SkyFace {
        std::vector<Vector3> verts;  // winding vertices
        Plane3 plane;                // face plane (outward normal)
    };
    std::vector<SkyFace> skyFaces;

    if (!entities.empty()) {
        for (const brush_t& brush : entities[0].brushes) {
            for (const side_t& side : brush.sides) {
                if (!(side.compileFlags & C_SKY)) continue;
                if (side.winding.size() < 3) continue;
                SkyFace face;
                face.verts.assign(side.winding.begin(), side.winding.end());
                face.plane = side.plane;
                skyFaces.push_back(std::move(face));
            }
        }
    }

    Sys_FPrintf( SYS_VRB, "  Sky faces found: %d\n", (int)skyFaces.size() );

    // If no sky faces found, fall back to a large ceiling portal
    if (skyFaces.empty()) {
        SkyFace face;
        face.verts = {
            Vector3(-32768, -32768, 16384),
            Vector3( 32768, -32768, 16384),
            Vector3( 32768,  32768, 16384),
            Vector3(-32768,  32768, 16384)
        };
        face.plane = Plane3(Vector3(0, 0, 1), 16384);
        skyFaces.push_back(std::move(face));
        Sys_FPrintf( SYS_VRB, "  Using fallback ceiling portal\n" );
    }

    const uint16_t numPortals = (uint16_t)skyFaces.size();
    Sys_FPrintf( SYS_VRB, "  Total portals: %d\n", (int)numPortals );
    const uint16_t skyVirtualCell = 2;  // numCells(1) + 1

    auto& portals = ApexLegends::Bsp::portals;
    auto& pverts = ApexLegends::Bsp::portalVertices;
    auto& pedges = ApexLegends::Bsp::portalEdges;
    auto& pve = ApexLegends::Bsp::portalVertexEdges;
    auto& pvr = ApexLegends::Bsp::portalVertexReferences;
    auto& per = ApexLegends::Bsp::portalEdgeReferences;
    auto& pieh = ApexLegends::Bsp::portalEdgeIntersectHeader;
    auto& piee = ApexLegends::Bsp::portalEdgeIntersectEdge;
    auto& pieav = ApexLegends::Bsp::portalEdgeIntersectAtVertex;

    // Track global indices
    uint16_t globalVertIdx = 0;
    uint16_t globalEdgeIdx = 0;
    uint16_t globalRefIdx = 0;
    uint32_t globalIsectIdx = 0;

    // Per-portal data for edge intersection computation
    struct PortalEdgeInfo {
        uint16_t globalVertStart;
        uint16_t globalEdgeStart;
        uint16_t numEdges;
        // Each edge: two endpoint positions and their global vertex indices
        struct EdgeData {
            Vector3 posA, posB;
            uint16_t vertA, vertB;
        };
        std::vector<EdgeData> edges;
    };
    std::vector<PortalEdgeInfo> portalEdgeInfos;

    uint32_t basePlaneIdx = (uint32_t)Titanfall::Bsp::planes.size();

    for (uint16_t pi = 0; pi < numPortals; pi++) {
        const SkyFace& face = skyFaces[pi];
        const uint16_t nv = (uint16_t)face.verts.size();

        // Portal plane: inward normal (negated face normal, which points outward)
        Vector3 inNormal = -face.plane.normal();
        float inDist = (float)(-face.plane.dist());
        Titanfall::Bsp::planes.emplace_back(Plane3f(
            (float)inNormal.x(), (float)inNormal.y(), (float)inNormal.z(), inDist));

        // Emit portal struct (12 bytes)
        portals.push_back(0);              // isReversed = 0
        portals.push_back(1);              // portalType = 1 (sky)
        portals.push_back((uint8_t)nv);    // numEdges
        portals.push_back(0);              // pad
        writeU16(portals, globalRefIdx);   // firstRef
        writeU16(portals, skyVirtualCell); // cellTo
        writeU32(portals, basePlaneIdx + pi); // planeNum

        uint16_t portalVertStart = globalVertIdx;
        uint16_t portalEdgeStart = globalEdgeIdx;

        // Emit vertices (12 bytes each)
        for (uint16_t vi = 0; vi < nv; vi++) {
            const Vector3& v = face.verts[vi];
            writeVec3(pverts, (float)v.x(), (float)v.y(), (float)v.z());
        }

        // Emit edges: consecutive vertex pairs in original face winding order
        // (CCW from face normal direction). cross(v0_rel, v1_rel) in this order
        // produces inward-pointing edge normals needed for correct PVS clipping.
        PortalEdgeInfo edgeInfo;
        edgeInfo.globalVertStart = portalVertStart;
        edgeInfo.globalEdgeStart = globalEdgeIdx;
        edgeInfo.numEdges = nv;
        for (uint16_t ei = 0; ei < nv; ei++) {
            uint16_t v0 = portalVertStart + ei;
            uint16_t v1 = portalVertStart + ((ei + 1) % nv);
            writeU16(pedges, v0);
            writeU16(pedges, v1);
            edgeInfo.edges.push_back({
                face.verts[ei],
                face.verts[(ei + 1) % nv],
                v0, v1
            });
        }
        portalEdgeInfos.push_back(edgeInfo);

        // Emit vertex edges: for each vertex, list all edges incident to it
        // (up to 8, padded with 0xFFFF)
        for (uint16_t vi = 0; vi < nv; vi++) {
            uint16_t gvi = portalVertStart + vi;
            int count = 0;
            for (uint16_t ei = 0; ei < nv; ei++) {
                uint16_t v0 = portalVertStart + ei;
                uint16_t v1 = portalVertStart + ((ei + 1) % nv);
                if (v0 == gvi || v1 == gvi) {
                    writeU16(pve, portalEdgeStart + ei);
                    count++;
                }
            }
            for (int pad = count; pad < 8; pad++) {
                writeU16(pve, 0xFFFF);
            }
        }

        // Emit vertex references (original face winding order)
        for (uint16_t ei = 0; ei < nv; ei++) {
            writeU16(pvr, portalVertStart + ei);
        }

        // Emit edge references: engine decodes as (ref >> 1) = edgeIndex, (ref & 1) = directionBit
        for (uint16_t ei = 0; ei < nv; ei++) {
            writeU16(per, (portalEdgeStart + ei) << 1);  // direction = 0
        }

        globalVertIdx += nv;
        globalEdgeIdx += nv;
        globalRefIdx += nv;
    }

    Sys_FPrintf( SYS_VRB, "  Totals: %d vertices, %d edges, %d refs\n",
        (int)globalVertIdx, (int)globalEdgeIdx, (int)globalRefIdx );

    // Compute edge intersections between portals in the same cell
    // For each edge, find edges from OTHER portals that share a vertex position
    // (within tolerance) — these are the "intersecting" edges
    const float EDGE_ISECT_TOL = 1.0f;
    auto vecClose = [EDGE_ISECT_TOL](const Vector3& a, const Vector3& b) -> bool {
        return fabs(a.x() - b.x()) < EDGE_ISECT_TOL &&
               fabs(a.y() - b.y()) < EDGE_ISECT_TOL &&
               fabs(a.z() - b.z()) < EDGE_ISECT_TOL;
    };

    for (uint16_t pi = 0; pi < numPortals; pi++) {
        const PortalEdgeInfo& info = portalEdgeInfos[pi];
        for (uint16_t ei = 0; ei < info.numEdges; ei++) {
            const auto& curEdge = info.edges[ei];

            // Collect intersecting edges from other portals
            std::vector<uint16_t> isectEdges;
            std::vector<uint16_t> isectAtVertex;

            for (uint16_t oj = 0; oj < numPortals; oj++) {
                if (oj == pi) continue;
                const PortalEdgeInfo& other = portalEdgeInfos[oj];
                for (uint16_t oe = 0; oe < other.numEdges; oe++) {
                    const auto& otherEdge = other.edges[oe];
                    // Check if edges share a vertex
                    if (vecClose(curEdge.posA, otherEdge.posA) || vecClose(curEdge.posA, otherEdge.posB) ||
                        vecClose(curEdge.posB, otherEdge.posA) || vecClose(curEdge.posB, otherEdge.posB)) {
                        isectEdges.push_back(other.globalEdgeStart + oe);
                        // atVertex: global vertex index of the shared point on THIS edge
                        if (vecClose(curEdge.posA, otherEdge.posA) || vecClose(curEdge.posA, otherEdge.posB))
                            isectAtVertex.push_back(curEdge.vertA);
                        else
                            isectAtVertex.push_back(curEdge.vertB);
                    }
                }
            }

            // Write header entry (8 bytes): {first, count}
            // count must be >= 1 (engine uses do-while)
            uint32_t isectCount = std::max((uint32_t)isectEdges.size(), (uint32_t)1);
            writeU32(pieh, globalIsectIdx);
            writeU32(pieh, isectCount);

            if (isectEdges.empty()) {
                // No intersections — write sentinel entry
                for (int j = 0; j < 8; j++) writeU16(piee, 0xFFFF);
                for (int j = 0; j < 8; j++) writeU16(pieav, 0xFFFF);
                globalIsectIdx++;
            } else {
                // Write one entry per intersection
                for (size_t k = 0; k < isectEdges.size(); k++) {
                    // Edge entry: up to 8 edge indices
                    writeU16(piee, isectEdges[k]);
                    for (int j = 1; j < 8; j++) writeU16(piee, 0xFFFF);
                    // AtVertex entry: up to 8 indices
                    writeU16(pieav, isectAtVertex[k]);
                    for (int j = 1; j < 8; j++) writeU16(pieav, 0xFFFF);
                    globalIsectIdx++;
                }
            }
        }
    }

    Sys_FPrintf( SYS_VRB, "  Edge intersections: %d entries\n", (int)globalIsectIdx );
    Sys_FPrintf( SYS_VRB, "--- EmitPortals Complete ---\n" );
}


/*
    EmitCells()
    Generates the cells lump. Currently emits a single cell
    referencing all sky portals with skyFlags=1.
*/
void ApexLegends::EmitCells() {
    Sys_FPrintf( SYS_VRB, "--- EmitCells ---\n" );

    auto writeU16 = [](std::vector<uint8_t>& vec, uint16_t v) {
        vec.push_back(v & 0xFF);
        vec.push_back((v >> 8) & 0xFF);
    };

    const uint16_t numPortals = (uint16_t)(ApexLegends::Bsp::portals.size() / 12);

    // Cell BSP Nodes: single leaf node
    {
        constexpr std::array<uint8_t, 8> data = {
            0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
        };
        ApexLegends::Bsp::cellBSPNodes = { data.begin(), data.end() };
    }

    // struct mcell_t { uint16 numPortals, firstPortal, skyFlags, unk; }
    auto& cells = ApexLegends::Bsp::cells;
    writeU16(cells, numPortals);  // numPortals
    writeU16(cells, 0);           // firstPortal
    writeU16(cells, 1);           // skyFlags = 1
    writeU16(cells, 0xFFFF);      // unk

    Sys_FPrintf( SYS_VRB, "  Cell: %d portals, skyFlags=1\n", (int)numPortals );
    Sys_FPrintf( SYS_VRB, "--- EmitCells Complete ---\n" );
}
