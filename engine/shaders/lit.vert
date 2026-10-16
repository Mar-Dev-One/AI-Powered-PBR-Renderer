#version 460 core

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

out vec3 v_world_pos;
out vec3 v_normal;
out vec2 v_uv;

uniform mat4 u_model;
uniform mat4 u_view_projection;

// Inverse-transpose of u_model. A plain mat4 rather than a dedicated mat3
// uniform (RHI only exposes rhi_shader_set_mat4/vec3/float today) -- only
// the upper-left 3x3 is ever read, via mat3(u_normal_matrix) below, so the
// translation column is simply ignored.
uniform mat4 u_normal_matrix;

void main()
{
    vec4 world_pos = u_model * vec4(in_pos, 1.0);

    v_world_pos = world_pos.xyz;
    v_normal    = mat3(u_normal_matrix) * in_normal;
    v_uv        = in_uv;

    gl_Position = u_view_projection * world_pos;
}
