#version 460 core

// Vertex stage shared by every IBL bake pass (equirect_to_cube, irradiance,
// prefilter). Draws a [-1, 1] cube from its centre, one face at a time; the
// interpolated position doubles as the world-space direction each texel of
// that cube map face represents.
layout(location = 0) in vec3 in_pos;

out vec3 v_dir;

uniform mat4 u_view_projection;   // rotation-only view for one cube face * 90 degree projection

void main()
{
    v_dir = in_pos;
    gl_Position = u_view_projection * vec4(in_pos, 1.0);
}
