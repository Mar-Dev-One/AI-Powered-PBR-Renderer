#version 460 core

// Depth of field, back at full size (Renderer/PostProcess.h
// post_depth_of_field()): each pixel mixes from the sharp scene to the
// half-size blur by its own circle of confusion -- from the full-size depth,
// so an in-focus edge stays crisp -- or by how much a blurred foreground
// covers it, whichever is more.
//
// u_show_focus (the "focus" debug view) draws the circle of confusion
// instead: black at the focus, orange in front of it and blue behind,
// brighter the blurrier.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_scene;     // linear HDR, full size
uniform sampler2D u_depth;     // the scene's depth buffer, full size
uniform sampler2D u_blurred;   // dof_blur.frag's, half size
uniform vec2      u_planes;    // the camera's near, far
uniform float     u_coc_scale;
uniform float     u_focus;
uniform float     u_max_coc;   // full-size pixels
uniform int       u_show_focus;

float view_depth(float depth)
{
    float z = depth * 2.0 - 1.0;
    return 2.0 * u_planes.x * u_planes.y / (u_planes.y + u_planes.x - z * (u_planes.y - u_planes.x));
}

float circle_of_confusion(float depth)
{
    return clamp(u_coc_scale * (1.0 - u_focus / view_depth(depth)), -u_max_coc, u_max_coc);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float coc   = circle_of_confusion(texelFetch(u_depth, pixel, 0).r);

    if (u_show_focus != 0)
    {
        float t    = abs(coc) / max(u_max_coc, 1e-4);
        vec3  tint = coc < 0.0 ? vec3(1.0, 0.55, 0.15) : vec3(0.2, 0.55, 1.0);
        frag_color = vec4(pow(tint * t, vec3(2.2)), 1.0);   // tonemap.frag gamma-encodes data views
        return;
    }

    vec3 sharp = texelFetch(u_scene, pixel, 0).rgb;

    // The blur, softened a little more on the way up: four filtered taps
    // around this spot (about three half-size texels across) fill the gaps
    // a few dozen taps leave in a large disc, which otherwise show as lumps
    // in a bright bokeh.
    vec2 half_texel = 1.0 / vec2(textureSize(u_blurred, 0));
    vec4 blurred = 0.25 * (texture(u_blurred, v_uv + vec2(-0.75, -0.75) * half_texel)
                         + texture(u_blurred, v_uv + vec2( 0.75, -0.75) * half_texel)
                         + texture(u_blurred, v_uv + vec2(-0.75,  0.75) * half_texel)
                         + texture(u_blurred, v_uv + vec2( 0.75,  0.75) * half_texel));

    // Under a pixel of blur nothing changes; by two it's all blur.
    float amount = max(smoothstep(1.0, 2.0, abs(coc)), blurred.a);
    frag_color = vec4(mix(sharp, blurred.rgb, amount), 1.0);
}
