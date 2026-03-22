/*
   skybox.frag
   Skybox fragment shader — Vulkan GLSL 4.5 port.

   Replaces install/gl/skybox_fp.glsl

   Vulkan port — MRVN-Radiant contributors.
   Licensed under the GNU Lesser General Public License v2 or later.
*/
#version 450

layout(set = 1, binding = 0) uniform samplerCube u_skybox;

layout(location = 0) in  vec3 var_direction;
layout(location = 0) out vec4 out_color;

void main()
{
    // Rotate/flip to match original Q3 cubemap orientation.
    out_color = texture(u_skybox, vec3(-var_direction.y,
                                       var_direction.z,
                                       var_direction.x));
}
