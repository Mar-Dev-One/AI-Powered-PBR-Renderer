#version 460 core

// Bake pass 4 (BRDF LUT): a clip-space quad covering the whole 2D target.
// v_uv runs 0..1 across it and is used directly as (N.V, roughness).
layout(location = 0) in vec2 in_pos;

out vec2 v_uv;

void main()
{
    v_uv = in_pos * 0.5 + 0.5;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
