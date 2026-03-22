/*
   lighting_dbs_omni.frag
   Diffuse/Bump/Specular lighting fragment shader — Vulkan GLSL 4.5 port.

   Replaces install/gl/lighting_DBS_omni_fp.glsl

   Copyright (C) 2004 Robert Beckebans <trebor_7@users.sourceforge.net>
   Vulkan port — MRVN-Radiant contributors.
   Licensed under the GNU Lesser General Public License v2 or later.
*/
#version 450

// ── Samplers (set 1) ──────────────────────────────────────────────────────────
layout(set = 1, binding = 0) uniform sampler2D u_diffusemap;
layout(set = 1, binding = 1) uniform sampler2D u_bumpmap;
layout(set = 1, binding = 2) uniform sampler2D u_specularmap;
layout(set = 1, binding = 3) uniform sampler2D u_attenuationmap_xy;
layout(set = 1, binding = 4) uniform sampler2D u_attenuationmap_z;

// ── Push constants ────────────────────────────────────────────────────────────
layout(push_constant) uniform PC {
    vec3  u_view_origin;        float _p0;
    vec3  u_light_origin;       float _p1;
    vec3  u_light_color;        float _p2;
    float u_bump_scale;
    float u_specular_exponent;
};

// ── Inputs ────────────────────────────────────────────────────────────────────
layout(location = 0) in vec3 var_vertex;
layout(location = 1) in vec4 var_tex_diffuse_bump;
layout(location = 2) in vec2 var_tex_specular;
layout(location = 3) in vec4 var_tex_atten_xy_z;
layout(location = 4) in mat3 var_mat_os2ts;    // uses locations 4, 5, 6

// ── Output ────────────────────────────────────────────────────────────────────
layout(location = 0) out vec4 out_color;

void main()
{
    // View & light directions in tangent space
    vec3 V = normalize(var_mat_os2ts * (u_view_origin  - var_vertex));
    vec3 L = normalize(var_mat_os2ts * (u_light_origin - var_vertex));
    vec3 H = normalize(L + V);

    // Normal from bump map
    vec3 N = 2.0 * (texture(u_bumpmap, var_tex_diffuse_bump.zw).xyz - 0.5);
    N.z *= u_bump_scale;
    N = normalize(N);

    // Diffuse
    vec4 diffuse = texture(u_diffusemap, var_tex_diffuse_bump.xy);
    diffuse.rgb *= u_light_color * clamp(dot(N, L), 0.0, 1.0);

    // Specular
    vec3 specular = texture(u_specularmap, var_tex_specular).rgb
                  * u_light_color
                  * pow(clamp(dot(N, H), 0.0, 1.0), u_specular_exponent);

    // Attenuation (projective XY + linear Z)
    // Projective: divide st by w manually (textureProj equivalent)
    vec3 atten_xy  = textureProj(u_attenuationmap_xy,
                                 vec3(var_tex_atten_xy_z.x,
                                      var_tex_atten_xy_z.y,
                                      var_tex_atten_xy_z.w)).rgb;
    vec3 atten_z   = texture(u_attenuationmap_z,
                              vec2(var_tex_atten_xy_z.z, 0.0)).rgb;

    out_color.a   = diffuse.a;
    out_color.rgb = (diffuse.rgb + specular) * atten_xy * atten_z;
}
