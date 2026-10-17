#version 460 core

// Anti-aliasing (Renderer/PostProcess.h): FXAA 3.11, quality version
// (Timothy Lottes, NVIDIA, 2011), after Simon Rodriguez's walk-through.
// Runs on the finished, tone-mapped picture: u_source is what tonemap.frag
// wrote, gamma-encoded, with each pixel's luma in alpha.
//
// For each pixel on a contrasty edge it works out whether the edge runs
// horizontally or vertically, follows it both ways until the contrast
// ends (up to ITERATIONS steps, growing), and from where the pixel sits
// along that stretch, reads the picture a fraction of a pixel across the
// edge -- a blend along the edge's real slope instead of a blur. A
// sub-pixel term also softens features thinner than a pixel.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_source;
uniform vec2      u_texel;   // 1 / u_source's size

// Contrast below which a pixel is left alone: the larger of an absolute
// floor (dark areas) and a fraction of the local maximum.
const float EDGE_THRESHOLD_MIN = 0.0312;
const float EDGE_THRESHOLD_MAX = 0.125;

const int   ITERATIONS       = 12;
const float SUBPIXEL_QUALITY = 0.75;   // 0 = sharpest .. 1 = softest

// How far each step of the edge search goes, in pixels: short steps first.
float step_size(int i)
{
    return i < 5 ? 1.0 : i == 5 ? 1.5 : i < 10 ? 2.0 : i == 10 ? 4.0 : 8.0;
}

float luma_at(vec2 uv)          { return texture(u_source, uv).a; }
float luma_off(int x, int y)    { return textureOffset(u_source, v_uv, ivec2(x, y)).a; }

void main()
{
    vec4  center      = texture(u_source, v_uv);
    float luma_center = center.a;

    float luma_down  = luma_off( 0, -1);
    float luma_up    = luma_off( 0,  1);
    float luma_left  = luma_off(-1,  0);
    float luma_right = luma_off( 1,  0);

    float luma_min   = min(luma_center, min(min(luma_down, luma_up), min(luma_left, luma_right)));
    float luma_max   = max(luma_center, max(max(luma_down, luma_up), max(luma_left, luma_right)));
    float luma_range = luma_max - luma_min;

    if (luma_range < max(EDGE_THRESHOLD_MIN, luma_max * EDGE_THRESHOLD_MAX))
    {
        frag_color = vec4(center.rgb, 1.0);
        return;
    }

    float luma_down_left  = luma_off(-1, -1);
    float luma_up_right   = luma_off( 1,  1);
    float luma_up_left    = luma_off(-1,  1);
    float luma_down_right = luma_off( 1, -1);

    float luma_down_up     = luma_down + luma_up;
    float luma_left_right  = luma_left + luma_right;
    float luma_left_corners  = luma_down_left + luma_up_left;
    float luma_down_corners  = luma_down_left + luma_down_right;
    float luma_right_corners = luma_down_right + luma_up_right;
    float luma_up_corners    = luma_up_right + luma_up_left;

    // Which way the edge runs: compare the change across rows with the
    // change across columns.
    float edge_horizontal = abs(-2.0 * luma_left + luma_left_corners)
                          + abs(-2.0 * luma_center + luma_down_up) * 2.0
                          + abs(-2.0 * luma_right + luma_right_corners);
    float edge_vertical   = abs(-2.0 * luma_up + luma_up_corners)
                          + abs(-2.0 * luma_center + luma_left_right) * 2.0
                          + abs(-2.0 * luma_down + luma_down_corners);
    bool horizontal = edge_horizontal >= edge_vertical;

    // Which side of the pixel the edge is on: the steeper neighbour.
    float luma1     = horizontal ? luma_down : luma_left;
    float luma2     = horizontal ? luma_up : luma_right;
    float gradient1 = luma1 - luma_center;
    float gradient2 = luma2 - luma_center;
    bool  side1     = abs(gradient1) >= abs(gradient2);
    float gradient_scaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float step_length   = horizontal ? u_texel.y : u_texel.x;
    float luma_local_avg;
    if (side1)
    {
        step_length    = -step_length;
        luma_local_avg = 0.5 * (luma1 + luma_center);
    }
    else
    {
        luma_local_avg = 0.5 * (luma2 + luma_center);
    }

    // Start half a pixel towards the edge, then walk along it both ways.
    vec2 uv = v_uv;
    if (horizontal) uv.y += step_length * 0.5;
    else            uv.x += step_length * 0.5;

    vec2 offset = horizontal ? vec2(u_texel.x, 0.0) : vec2(0.0, u_texel.y);
    vec2 uv1    = uv - offset;
    vec2 uv2    = uv + offset;

    float luma_end1 = luma_at(uv1) - luma_local_avg;
    float luma_end2 = luma_at(uv2) - luma_local_avg;
    bool  reached1  = abs(luma_end1) >= gradient_scaled;
    bool  reached2  = abs(luma_end2) >= gradient_scaled;
    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    for (int i = 2; i < ITERATIONS && !(reached1 && reached2); ++i)
    {
        if (!reached1) luma_end1 = luma_at(uv1) - luma_local_avg;
        if (!reached2) luma_end2 = luma_at(uv2) - luma_local_avg;
        reached1 = abs(luma_end1) >= gradient_scaled;
        reached2 = abs(luma_end2) >= gradient_scaled;
        if (!reached1) uv1 -= offset * step_size(i);
        if (!reached2) uv2 += offset * step_size(i);
    }

    // Where the pixel sits along the edge's stretch decides how far across
    // the edge to read: most at the ends, none in the middle.
    float distance1 = horizontal ? (v_uv.x - uv1.x) : (v_uv.y - uv1.y);
    float distance2 = horizontal ? (uv2.x - v_uv.x) : (uv2.y - v_uv.y);
    bool  nearer1   = distance1 < distance2;
    float distance_final = min(distance1, distance2);
    float edge_length    = distance1 + distance2;
    float pixel_offset   = -distance_final / edge_length + 0.5;

    // Only if the nearer end's luma moves the same way as the centre's.
    bool  center_smaller    = luma_center < luma_local_avg;
    bool  correct_variation = ((nearer1 ? luma_end1 : luma_end2) < 0.0) != center_smaller;
    float final_offset      = correct_variation ? pixel_offset : 0.0;

    // Sub-pixel aliasing: how different the pixel is from its 3x3 average.
    float luma_average = (1.0 / 12.0) * (2.0 * (luma_down_up + luma_left_right) + luma_left_corners + luma_right_corners);
    float subpixel1    = clamp(abs(luma_average - luma_center) / luma_range, 0.0, 1.0);
    float subpixel2    = (-2.0 * subpixel1 + 3.0) * subpixel1 * subpixel1;
    final_offset       = max(final_offset, subpixel2 * subpixel2 * SUBPIXEL_QUALITY);

    vec2 final_uv = v_uv;
    if (horizontal) final_uv.y += final_offset * step_length;
    else            final_uv.x += final_offset * step_length;

    frag_color = vec4(texture(u_source, final_uv).rgb, 1.0);
}
