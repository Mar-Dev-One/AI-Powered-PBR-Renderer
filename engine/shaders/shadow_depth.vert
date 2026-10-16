#version 460 core

// Depth-only pass: renders the scene from the light's point of view into a
// depth-only framebuffer (see Scene/Model.h's model_draw_depth()). Position
// is the only attribute that matters here, but the vertex layout is fixed
// by Mesh.c's mesh_create() regardless of which shader is bound, so the
// other locations are simply left unused.
layout(location = 0) in vec3 in_pos;

uniform mat4 u_model;
uniform mat4 u_light_view_projection;

void main()
{
    gl_Position = u_light_view_projection * u_model * vec4(in_pos, 1.0);
}
