#version 460 core

// Draws the environment behind the scene. Uses a [-1, 1] cube with the view
// matrix's translation stripped, so the camera stays at its centre and the
// sky never gets closer as the camera moves.
layout(location = 0) in vec3 in_pos;

out vec3 v_dir;

uniform mat4 u_view;
uniform mat4 u_projection;

void main()
{
    v_dir = in_pos;

    vec4 pos = u_projection * mat4(mat3(u_view)) * vec4(in_pos, 1.0);

    // Force the sky to the far plane so it never covers geometry. Not exactly
    // 1.0: the depth test is GL_LESS against a buffer cleared to 1.0, so a
    // fragment at exactly 1.0 would fail it.
    gl_Position = vec4(pos.xy, pos.w * 0.99999, pos.w);
}
