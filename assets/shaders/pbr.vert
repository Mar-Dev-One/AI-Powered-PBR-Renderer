#version 460 core

// CSM_MAX_CASCADES and friends are injected at compile time from the C
// headers (see scene_renderer_shader_defines() in Renderer/SceneRenderer.c),
// so the C and GLSL values can't drift apart.
#ifndef CSM_MAX_CASCADES
#error "pbr.vert must be compiled with SceneRenderer's defines"
#endif

// Vertex layout is Scene/Mesh.h's mesh_vertex.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;   // xyz = tangent, w = bitangent handedness (+1/-1)

out vec3 v_world_pos;
out vec3 v_normal;
out vec4 v_tangent;
out vec2 v_uv;
out float v_view_depth;

uniform mat4 u_model;
uniform mat4 u_view;                   // camera view only (no projection) -- just for v_view_depth below
uniform mat4 u_view_projection;
uniform mat4 u_normal_matrix;          // inverse-transpose of u_model; only the upper 3x3 is read

void main()
{
    vec4 world_pos = u_model * vec4(in_pos, 1.0);

    v_world_pos = world_pos.xyz;
    v_normal    = mat3(u_normal_matrix) * in_normal;

    // Tangents are directions along the surface, so they transform with the
    // model matrix itself (not the inverse-transpose that normals need).
    v_tangent   = vec4(mat3(u_model) * in_tangent.xyz, in_tangent.w);
    v_uv        = in_uv;

    // Distance along the camera's forward axis, used by pbr.frag to pick
    // which cascade a fragment falls into. Also affine in world_pos, same
    // reasoning as above.
    v_view_depth = -(u_view * world_pos).z;

    gl_Position = u_view_projection * world_pos;
}
