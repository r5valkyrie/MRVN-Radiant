# MRVN-Radiant

The open-source, cross-platform level editor for Respawn Entertainment Source based games.

MRVN-Radiant is a fork of NetRadiant-custom (GtkRadiant 1.4 &rarr; massive rewrite &rarr; 1.5 &rarr; NetRadiant &rarr; NetRadiant-custom &rarr; this)

<div align=left>
<img alt="GitHub Workflow Status" src="https://img.shields.io/github/actions/workflow/status/MRVNRadiant/MRVNRadiant/build.yml?style=for-the-badge">
<img alt="GitHub Issues" src="https://img.shields.io/github/issues/MRVNRadiant/MRVNRadiant?style=for-the-badge">
<img alt="GitHub Pullrequests" src="https://img.shields.io/github/issues-pr/MRVNRadiant/MRVNRadiant?style=for-the-badge">
</div>

## Supported games
- Titanfall Online ( [TFORevive](https://tforevive.net/) )
- Titanfall 2 ( [Northstar](https://northstar.tf) )
- Apex Legends ( [R5Reloaded](https://r5reloaded.com/) )

## Status
| Game | Coverage | Note |
|------|----------|------|
| Titanfall Online | 38% | No triangle collision, no lighting, no portals |
| Titanfall 2 | 37% | No triangle collision, no lighting, no portals |
| Apex Legends | 62% | No realtime lights, no portals, no water, no heightfields |

> Coverage = Lumps with generated data / Total lump count (128). Stub/empty lumps not counted.

> NOTE: These values are updated manually.

### Apex Legends Features
- **Geometry**: Meshes, mesh bounds, vertex types (unlit, lit flat, lit bump, unlit TS)
- **Collision**: BVH nodes, packed vertices, contents masks, surface properties
- **Lighting**: HDR lightmaps, light probes, world lights (point/spot/sky)
- **Shadows**: Shadow meshes (opaque/alpha), shadow environments, CSM AABB tree
- **Visibility**: Cell AABB tree, object references, fade distances, occlusion meshes
- **Materials**: Texture data, material sorts, surface names
- **Misc**: Cubemaps, static props (game lump), entity partitions, level info

## Other tools
- [bsp_tool](https://github.com/snake-biscuits/bsp_tool)
    - A Python library for analyzing .bsp files

- [bsp_regen](https://github.com/snake-biscuits/bsp_regen)
    - `Titanfall -> Titanfall | 2` Map Converter

- [MRVN-Explorer](https://github.com/MRVN-Radiant/MRVN-Explorer)
    - A OpenGL/ImGui respawn bsp viewer
