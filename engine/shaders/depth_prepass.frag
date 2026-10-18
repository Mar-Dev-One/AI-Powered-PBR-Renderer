#version 460 core

// Depth pre-pass (Renderer/SceneRenderer.h): pbr.vert + this, with color
// writes off, lays down the opaque scene's depth before anything is shaded
// -- for SSAO to read, and so the main pass shades each pixel once. Alpha-
// masked surfaces (leaves, fences) discard their holes exactly as pbr.frag
// does; otherwise the holes would block what's behind them.
//
// The material uniforms are set by material_bind(), same as for pbr.frag.

in vec2 v_uv;

out vec4 frag_color;

uniform vec4      u_base_color;
uniform int       u_alpha_mode;        // 0 opaque, 1 mask, 2 blend (never drawn here)
uniform float     u_alpha_cutoff;
uniform int       u_texture_mask;      // bit N set = material slot N has a texture
uniform sampler2D u_base_color_tex;    // slot 0

void main()
{
    if (u_alpha_mode == 1)
    {
        float alpha = u_base_color.a;
        if ((u_texture_mask & 1) != 0)
            alpha *= texture(u_base_color_tex, v_uv).a;
        if (alpha < u_alpha_cutoff)
            discard;
    }

    frag_color = vec4(0.0);   // not written: color writes are off
}
