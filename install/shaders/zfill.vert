/*
   zfill.vert
   Depth-fill pass vertex shader — Vulkan GLSL 4.5 port.

   Replaces install/gl/zfill_vp.glsl

   Copyright (C) 2004 Robert Beckebans <trebor_7@users.sourceforge.net>
   Vulkan port — MRVN-Radiant contributors.
   Licensed under the GNU Lesser General Public License v2 or later.
*/
#version 450

// ── Vertex inputs ─────────────────────────────────────────────────────────────
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_texcoord;

// ── Transform UBO (set 0, binding 0) ─────────────────────────────────────────
layout(set = 0, binding = 0) uniform TransformUBO {
    mat4 u_mvp;
    mat4 u_texMatrix0;  // color/alpha map UV transform
};

// ── Outputs ───────────────────────────────────────────────────────────────────
layout(location = 0) out vec2 var_texcoord;

void main()
{
    gl_Position  = u_mvp * vec4(in_position, 1.0);
    var_texcoord = (u_texMatrix0 * vec4(in_texcoord, 0.0, 1.0)).xy;
}
