#version 460 core

// Depth of field, the blur (Renderer/PostProcess.h post_depth_of_field()),
// at half size: a gather over a disc of u_taps samples on a golden-angle
// spiral (Gustafsson, "Bokeh depth of field in a single pass", 2018). A
// sample counts towards this pixel only if its own blur reaches this far --
// so a bright spot spreads into an even disc (bokeh) -- and a sample behind
// this pixel's surface may reach no further than twice this pixel's own
// blur: a blurred background can't spill over a sharp subject in front of
// it, while a blurred foreground still covers what's behind it.
//
// Out: the blurred color, and in alpha how much a blurred foreground covers
// this pixel (0..1). The pixel's own blur comes from the full-size depth
// when compositing, so sharp edges stay sharp.

out vec4 frag_color;

uniform sampler2D u_source;     // dof_prepare.frag's: color, signed blur in texels
uniform int       u_taps;
uniform float     u_max_coc;    // texels of this size

const float GOLDEN_ANGLE = 2.39996323;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2  texel = 1.0 / vec2(textureSize(u_source, 0));

    vec4  centre      = texelFetch(u_source, pixel, 0);
    float centre_size = abs(centre.a);

    vec3  sum        = centre.rgb;
    float count      = 1.0;
    float near_cover = 0.0;

    for (int i = 0; i < u_taps; ++i)
    {
        float radius = u_max_coc * sqrt((float(i) + 0.5) / float(u_taps));
        float angle  = float(i) * GOLDEN_ANGLE;
        vec2  offset = vec2(cos(angle), sin(angle)) * radius;

        // Filtered, at the exact spot: rounded to whole texels the disc's
        // edge -- a bokeh's outline -- comes out ragged.
        vec4  s    = textureLod(u_source, (gl_FragCoord.xy + offset) * texel, 0.0);
        float size = abs(s.a);
        if (s.a > centre.a)   // behind this pixel's surface
            size = min(size, centre_size * 2.0);

        // How much of this sample's blur reaches here: none short of its
        // radius, all of it past -- half a texel of softening between.
        float reach = smoothstep(radius - 0.5, radius + 0.5, size);
        sum   += mix(sum / count, s.rgb, reach);
        count += 1.0;

        if (s.a < centre.a && s.a < -1.0)   // a blurred surface in front of this one
            near_cover = max(near_cover, reach);
    }

    frag_color = vec4(sum / count, near_cover);
}
