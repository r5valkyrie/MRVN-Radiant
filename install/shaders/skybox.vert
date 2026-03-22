/*
   skybox.vert
   Skybox vertex shader — Vulkan GLSL 4.5 port.

   Replaces install/gl/skybox_vp.glsl

   Vulkan port — MRVN-Radiant contributors.
   Licensed under the GNU Lesser General Public License v2 or later.
*/
#version 450

// ── Vertex inputs ─────────────────────────────────────────────────────────────
layout(location = 0) in vec3 in_position;

// ── Transform UBO (set 0, binding 0) ─────────────────────────────────────────
layout(set = 0, binding = 0) uniform TransformUBO {
    mat4 u_mvp;
};

// ── Push constants ────────────────────────────────────────────────────────────
layout(push_constant) uniform PC {
    vec3  u_view_origin;  float _p0;
};

// ── Output ────────────────────────────────────────────────────────────────────
layout(location = 0) out vec3 var_direction;

void main()
{
    gl_Position   = u_mvp * vec4(in_position, 1.0);
    var_direction = in_position - u_view_origin;
}
