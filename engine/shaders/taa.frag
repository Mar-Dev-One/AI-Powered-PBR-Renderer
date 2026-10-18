#version 460 core

// Temporal anti-aliasing (Renderer/PostProcess.h post_taa()). Every frame
// the camera is shifted by a different sub-pixel amount (SceneRenderer.c),
// so a pixel sees a slightly different spot of the scene each time;
// blending each frame into a running history averages those spots -- the
// pixel's whole area, as supersampling would, without drawing it twice.
//
// Per pixel: where the surface it shows was on screen last frame
// (reprojected through the depth buffer -- camera motion; moving objects
// aren't tracked), last frame's result there, clipped to the range of
// colors around the pixel now (so history that no longer belongs -- a
// moving object, something uncovered -- can't ghost), then a small part of
// the new frame mixed in.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_current;      // this frame, jittered, linear HDR
uniform sampler2D u_history;      // the last result, linear HDR
uniform sampler2D u_depth;        // this frame's depth
uniform mat4      u_reprojection; // this frame's NDC -> last frame's clip space
uniform float     u_blend;        // the new frame's share (1 = no history)

// Colors are compared in YCoCg (brightness apart from hue: a tighter box
// than RGB) and after a Reinhard squash, so a few very bright pixels don't
// stretch the box or flicker.
vec3 to_ycocg(vec3 c)
{
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 from_ycocg(vec3 c)
{
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 squash(vec3 c)   { return c / (1.0 + luma(c)); }
vec3 unsquash(vec3 c) { return c / max(1.0 - luma(c), 1e-4); }

// Moves `history` towards the box's centre until it's inside the box.
vec3 clip_to_box(vec3 history, vec3 box_min, vec3 box_max)
{
    vec3 center = 0.5 * (box_max + box_min);
    vec3 extent = 0.5 * (box_max - box_min) + 1e-4;
    vec3 offset = history - center;
    vec3 ratio  = abs(offset / extent);
    float m     = max(ratio.x, max(ratio.y, ratio.z));
    return m > 1.0 ? center + offset / m : history;
}

void main()
{
    ivec2 pixel   = ivec2(gl_FragCoord.xy);
    vec3  current = max(texelFetch(u_current, pixel, 0).rgb, vec3(0.0));

    if (u_blend >= 1.0)
    {
        frag_color = vec4(current, 1.0);
        return;
    }

    // The 3 x 3 neighbourhood's mean and spread, squashed, in YCoCg.
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec3 c = to_ycocg(squash(max(texelFetch(u_current, pixel + ivec2(x, y), 0).rgb, vec3(0.0))));
            m1 += c;
            m2 += c * c;
        }
    }
    vec3 mean  = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 box_min = mean - 1.25 * sigma;
    vec3 box_max = mean + 1.25 * sigma;

    // Where this pixel's surface was last frame.
    float depth     = texelFetch(u_depth, pixel, 0).r;
    vec4  ndc       = vec4(v_uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4  prev_clip = u_reprojection * ndc;
    vec2  prev_uv   = prev_clip.xy / prev_clip.w * 0.5 + 0.5;

    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0))))
    {
        frag_color = vec4(current, 1.0);   // came on screen this frame: nothing to blend with
        return;
    }

    vec3 history = to_ycocg(squash(max(texture(u_history, prev_uv).rgb, vec3(0.0))));
    history      = unsquash(from_ycocg(clip_to_box(history, box_min, box_max)));

    // Brightness-weighted blend (Karis): a pixel far brighter than its
    // history counts less, which keeps specular glints from flickering.
    float w_current = u_blend / (1.0 + luma(current));
    float w_history = (1.0 - u_blend) / (1.0 + luma(history));
    vec3  result    = (current * w_current + history * w_history) / (w_current + w_history);

    frag_color = vec4(result, 1.0);
}
