/*
   lighting_dbs_omni.vert
   Diffuse/Bump/Specular lighting vertex shader — Vulkan GLSL 4.5 port.

   Replaces install/gl/lighting_DBS_omni_vp.glsl

   Copyright (C) 2004 Robert Beckebans <trebor_7@users.sourceforge.net>
   Vulkan port — MRVN-Radiant contributors.
   Licensed under the GNU Lesser General Public License v2 or later.
*/
#version 450

// ── Vertex inputs ─────────────────────────────────────────────────────────────
// Layout matches ArbitraryMeshVertex (56-byte stride):
//   offset  0 — texcoord  (vec2)
//   offset  8 — normal    (vec3)
//   offset 20 — vertex    (vec3)
//   offset 32 — tangent   (vec3)
//   offset 44 — binormal  (vec3)

layout(location = 0) in vec3 in_position;   // ArbitraryMeshVertex::vertex
layout(location = 1) in vec2 in_texcoord;   // ArbitraryMeshVertex::texcoord
layout(location = 2) in vec3 in_normal;     // ArbitraryMeshVertex::normal
layout(location = 3) in vec3 in_tangent;    // ArbitraryMeshVertex::tangent
layout(location = 4) in vec3 in_binormal;   // ArbitraryMeshVertex::bitangent

// ── Transform UBO (set 0, binding 0) ─────────────────────────────────────────
layout(set = 0, binding = 0) uniform TransformUBO {
    mat4 u_mvp;             // model-view-projection
    mat4 u_texMatrix0;      // diffuse UV transform
    mat4 u_texMatrix1;      // bump UV transform
    mat4 u_texMatrix2;      // specular UV transform
    mat4 u_localToLight;    // object-space → light-space transform
};

// ── Push constants ────────────────────────────────────────────────────────────
layout(push_constant) uniform PC {
    vec3  u_view_origin;        float _p0;
    vec3  u_light_origin;       float _p1;
    vec3  u_light_color;        float _p2;
    float u_bump_scale;
    float u_specular_exponent;
};

// ── Outputs ───────────────────────────────────────────────────────────────────
layout(location = 0) out vec3 var_vertex;
layout(location = 1) out vec4 var_tex_diffuse_bump;
layout(location = 2) out vec2 var_tex_specular;
layout(location = 3) out vec4 var_tex_atten_xy_z;
layout(location = 4) out mat3 var_mat_os2ts;   // uses locations 4, 5, 6

void main()
{
    gl_Position = u_mvp * vec4(in_position, 1.0);

    var_vertex = in_position;

    vec4 tc = vec4(in_texcoord, 0.0, 1.0);
    var_tex_diffuse_bump.xy = (u_texMatrix0 * tc).xy;
    var_tex_diffuse_bump.zw = (u_texMatrix1 * tc).xy;
    var_tex_specular        = (u_texMatrix2 * tc).xy;

    var_tex_atten_xy_z = u_localToLight * vec4(in_position, 1.0);

    // Object-space → tangent-space matrix (column order matches original shader)
    var_mat_os2ts = mat3(
        in_tangent.x,  in_binormal.x,  in_normal.x,
        in_tangent.y,  in_binormal.y,  in_normal.y,
        in_tangent.z,  in_binormal.z,  in_normal.z
    );
}
