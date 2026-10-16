#version 460 core

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec3 in_color;

out vec3 v_color;

void main()
{
    v_color = in_color;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
