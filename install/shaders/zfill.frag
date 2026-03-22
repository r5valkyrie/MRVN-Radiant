/*
   zfill.frag
   Depth-fill pass fragment shader — Vulkan GLSL 4.5 port.

   Replaces install/gl/zfill_fp.glsl

   Copyright (C) 2004 Robert Beckebans <trebor_7@users.sourceforge.net>
   Vulkan port — MRVN-Radiant contributors.
   Licensed under the GNU Lesser General Public License v2 or later.
*/
#version 450

layout(set = 1, binding = 0) uniform sampler2D u_colormap;

layout(location = 0) in  vec2 var_texcoord;
layout(location = 0) out vec4 out_color;

void main()
{
    // Write alpha from colormap; RGB is black (depth-fill pass, colour unused).
    out_color = vec4(0.0, 0.0, 0.0, texture(u_colormap, var_texcoord).a);
}
