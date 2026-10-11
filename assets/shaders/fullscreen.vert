#version 460 core

// Fullscreen pass (Renderer/PostProcess.h): one clip-space triangle that
// overhangs the screen. v_uv is 0..1 across the visible part.
layout(location = 0) in vec2 in_pos;

out vec2 v_uv;

void main()
{
    v_uv = in_pos * 0.5 + 0.5;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
