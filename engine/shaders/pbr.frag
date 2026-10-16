#version 460 core

// Metallic-roughness PBR (Cook-Torrance GGX) for models loaded through
// Scene/Model.h. Direct light is up to MAX_LIGHTS punctual lights
// (directional/point/spot, uploaded by Renderer/Lighting.h); indirect light is
// image-based (Renderer/IBL.h: baked irradiance + GGX-prefiltered environment)
// when u_use_ibl is set, and otherwise a cheap sky/ground hemisphere -- enough
// that metals aren't pitch black while no environment is loaded.
// Output is linear HDR radiance for Renderer/PostProcess.h's RGBA16F scene
// target; exposure, tone mapping and gamma happen there (tonemap.frag).

// Injected at compile time from the C side (scene_renderer_shader_defines(),
// Renderer/SceneRenderer.c), never written out here:
//   CSM_MAX_CASCADES                            Renderer/Shadow.h
//   MAX_LIGHTS                                  Renderer/Lighting.h's LIGHTING_MAX_LIGHTS
//   LIGHT_DIRECTIONAL / LIGHT_POINT / LIGHT_SPOT  Scene/Light.h's light_type values
#if !defined(CSM_MAX_CASCADES) || !defined(MAX_LIGHTS) || !defined(LIGHT_SPOT)
#error "pbr.frag must be compiled with SceneRenderer's defines"
#endif

in vec3 v_world_pos;
in vec3 v_normal;
in vec4 v_tangent;
in vec2 v_uv;
in float v_view_depth;

out vec4 frag_color;

// --- Material (set by material_bind, see Scene/Material.h) ------------------
uniform vec4  u_base_color;
uniform float u_metallic;
uniform float u_roughness;
uniform vec3  u_emissive;
uniform float u_normal_scale;
uniform float u_occlusion_strength;
uniform int   u_alpha_mode;        // 0 opaque, 1 mask, 2 blend
uniform float u_alpha_cutoff;
uniform int   u_texture_mask;      // bit N set = material slot N has a texture

uniform sampler2D u_base_color_tex;          // slot 0, sRGB
uniform sampler2D u_metallic_roughness_tex;  // slot 1, G = roughness, B = metallic
uniform sampler2D u_normal_tex;              // slot 2
uniform sampler2D u_occlusion_tex;           // slot 3, R = occlusion
uniform sampler2D u_emissive_tex;            // slot 4, sRGB

// --- Scene -------------------------------------------------------------------
// Layout set by lighting_bind() (Renderer/Lighting.h). Units follow glTF's
// KHR_lights_punctual: directional intensity is illuminance, point/spot
// intensity is luminous intensity, falling off with 1/d^2.
struct scene_light
{
    int   type;        // LIGHT_DIRECTIONAL / LIGHT_POINT / LIGHT_SPOT
    vec3  color;       // linear RGB
    float intensity;
    vec3  direction;   // world space, normalized: the direction the light travels
    vec3  position;    // world space; unused by LIGHT_DIRECTIONAL
    float range;       // attenuation reaches 0 here; 0 = no cutoff
    float inner_cos;   // LIGHT_SPOT: cos of the full-intensity half-angle
    float outer_cos;   // LIGHT_SPOT: cos of the zero-intensity half-angle
};

uniform scene_light u_lights[MAX_LIGHTS];
uniform int         u_light_count;

// Index into u_lights[] of the light the cascades below were rendered for,
// or -1 if no light is shadowed (set by lighting_bind()).
uniform int u_shadow_light_index;

uniform vec3  u_view_pos;
uniform float u_ambient_strength;

// Cascaded shadow maps (Renderer/Shadow.h): one depth-only texture per
// cascade, near cascades first. u_cascade_splits[i] is the view-space
// (camera-forward-axis) distance where cascade i's coverage ends -- see
// pick_cascade() below. Entries >= u_cascade_count are unused.
// The maps are comparison samplers (rhi_framebuffer_desc::depth_compare):
// each texture() call does a 2x2 bilinear PCF in hardware.
// u_cascade_texel_sizes[i] is one texel's world-space width in cascade i.
uniform sampler2DShadow u_shadow_maps[CSM_MAX_CASCADES];
uniform mat4            u_light_view_projection[CSM_MAX_CASCADES];
uniform float           u_cascade_splits[CSM_MAX_CASCADES];
uniform float           u_cascade_texel_sizes[CSM_MAX_CASCADES];
uniform int             u_cascade_count;

// Debug aid: tints each fragment by which cascade it landed in (cascade 0 =
// red, through to blue) so the split scheme/fit can be sanity-checked
// visually. Off (0) by default; Shadow.h doesn't set this itself, so it
// stays 0 unless something explicitly opts in.
uniform int u_debug_show_cascades;

// --- Image-based lighting (set by ibl_bind, see Renderer/IBL.h) ---------------
uniform int         u_use_ibl;              // 0 = hemisphere fallback below
uniform samplerCube u_irradiance_map;       // slot 9, diffuse irradiance / pi
uniform samplerCube u_prefiltered_map;      // slot 10, mip N = GGX-blurred at roughness N / max_lod
uniform float       u_prefiltered_max_lod;  // last mip index of u_prefiltered_map
uniform sampler2D   u_brdf_lut;             // slot 12, split-sum BRDF: (N.V, roughness) -> (F0 scale, bias)

const float PI = 3.14159265359;

bool has_texture(int slot) { return (u_texture_mask & (1 << slot)) != 0; }

// GGX / Trowbridge-Reitz normal distribution.
float distribution_ggx(float NoH, float alpha)
{
    float a2 = alpha * alpha;
    float d  = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Height-correlated Smith visibility term (already includes the 1/(4 NoL NoV)).
float visibility_smith_ggx(float NoV, float NoL, float alpha)
{
    float a2 = alpha * alpha;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 fresnel_schlick(float VoH, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
}

// Analytic fit of the split-sum environment BRDF (Karis, mobile PBR):
// (F0 scale, bias), same as one u_brdf_lut texel. Only used without a baked
// environment (and so without u_brdf_lut); the IBL path samples the LUT.
vec2 env_brdf_approx(float roughness, float NoV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572,  0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.04);
    vec4  r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Specular occlusion (Lagarde and de Rousiers): how much of the specular
// lobe an AO value blocks. Approaches the AO value itself on rough surfaces,
// and no occlusion at glancing views on smooth ones, where the lobe points
// out of the cavity.
float specular_occlusion(float NoV, float ao, float roughness)
{
    return clamp(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

// Which cascade a fragment at this view-space depth falls into -- the
// first one whose split_far (u_cascade_splits[i]) hasn't been passed yet,
// or the last cascade if the fragment is beyond every split (happens for
// depths between the camera's actual far plane and whatever the last
// cascade's ortho box still covers; clamping to the last cascade there
// beats an unshadowed fragment).
// Returns -1 (no cascade / nothing casts a shadow) when u_cascade_count is
// 0 -- draw_material_grid() (testbed/main.c) uses that to light its sphere
// grid without a shadow pass, rather than routing through a dummy
// always-lit cascade the way the single-shadow-map version used an
// identity light matrix for the same purpose.
int pick_cascade(float view_depth)
{
    for (int i = 0; i < u_cascade_count; ++i)
    {
        if (view_depth < u_cascade_splits[i])
            return i;
    }
    return u_cascade_count - 1;
}

// 12-tap Poisson disk on the unit disk. Rotated per pixel below so the
// fixed tap pattern turns into fine noise instead of visible banding.
const vec2 POISSON_DISK[12] = vec2[](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
    vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
    vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
    vec2( 0.896,  0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598)
);

// Filter radius in shadow-map texels. Each tap is itself a 2x2 hardware
// PCF, so even the edge of the disk is bilinearly smoothed.
const float SHADOW_FILTER_RADIUS = 1.75;

// Fraction of each cascade's depth range, at its far end, over which it is
// cross-faded with the next cascade -- hides the seam where texel density
// (and so shadow sharpness) changes between cascades.
const float CASCADE_BLEND_FRACTION = 0.12;

// Interleaved gradient noise (Jimenez): a cheap per-pixel value in [0, 1)
// with no visible repeating pattern.
float interleaved_gradient_noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Fraction of cascade `cascade_index`'s light blocked at `world_pos`:
// 0 = fully lit .. 1 = fully shadowed.
//
// Biasing, both scaled to this cascade's texel size so it holds for every
// cascade: the lookup point is pushed out along the geometric normal
// (normal offset -- grows as the surface turns edge-on to the light, which
// is where acne comes from), plus a small depth bias in units of texels.
float sample_cascade(int cascade_index, vec3 world_pos, vec3 geometric_normal, float NoL, float rotation)
{
    float texel_world = u_cascade_texel_sizes[cascade_index];
    float sin_theta   = sqrt(clamp(1.0 - NoL * NoL, 0.0, 1.0));
    vec3  offset_pos  = world_pos + geometric_normal * texel_world * (0.5 + SHADOW_FILTER_RADIUS * sin_theta);

    vec4 light_clip = u_light_view_projection[cascade_index] * vec4(offset_pos, 1.0);
    vec3 proj = light_clip.xyz / light_clip.w;   // no-op for the orthographic cascades, kept for a future perspective (spot) shadow
    proj = proj * 0.5 + 0.5;                     // clip [-1, 1] -> texture/depth [0, 1]

    // Outside this cascade's box: nothing known about occluders there.
    if (proj.z > 1.0 || any(lessThan(proj.xy, vec2(0.0))) || any(greaterThan(proj.xy, vec2(1.0))))
        return 0.0;

    // Depth bias in texels: the cascade's ortho depth range is twice its
    // width (Shadow.c), so one texel of depth is about texel.x / 2.
    vec2  texel      = 1.0 / vec2(textureSize(u_shadow_maps[cascade_index], 0));
    float depth_bias = texel.x * (0.5 + sin_theta);
    float reference  = proj.z - depth_bias;

    float s = sin(rotation), c = cos(rotation);
    mat2  rotate = mat2(c, s, -s, c);

    float lit = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        vec2 uv = proj.xy + rotate * POISSON_DISK[i] * SHADOW_FILTER_RADIUS * texel;
        lit += texture(u_shadow_maps[cascade_index], vec3(uv, reference));
    }

    return 1.0 - lit / 12.0;
}

// Cascaded shadow lookup for the shadowed light: samples the cascade picked
// by view depth, and near that cascade's far edge cross-fades into the next.
float shadow_calculation(int cascade_index, vec3 geometric_normal, float NoL)
{
    float rotation = interleaved_gradient_noise(gl_FragCoord.xy) * 6.2831853;
    float shadow   = sample_cascade(cascade_index, v_world_pos, geometric_normal, NoL, rotation);

    if (cascade_index + 1 < u_cascade_count)
    {
        float slice_near = cascade_index > 0 ? u_cascade_splits[cascade_index - 1] : 0.0;
        float slice_far  = u_cascade_splits[cascade_index];
        float band       = (slice_far - slice_near) * CASCADE_BLEND_FRACTION;
        float t          = clamp((v_view_depth - (slice_far - band)) / band, 0.0, 1.0);

        if (t > 0.0)
        {
            float next = sample_cascade(cascade_index + 1, v_world_pos, geometric_normal, NoL, rotation);
            shadow = mix(shadow, next, t);
        }
    }

    return shadow;
}

// Smooth window that takes punctual light falloff to exactly 0 at `range`
// (KHR_lights_punctual's recommended form), times inverse-square falloff.
float distance_attenuation(float dist_sq, float range)
{
    float attenuation = 1.0 / max(dist_sq, 1e-4);
    if (range > 0.0)
    {
        float ratio  = dist_sq / (range * range);   // (d / range)^2
        float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
        attenuation *= window * window;
    }
    return attenuation;
}

float spot_attenuation(vec3 L, scene_light light)
{
    // L points from the surface to the light; direction is the way the light travels.
    float cos_angle = dot(-L, light.direction);
    float t = clamp((cos_angle - light.outer_cos) / max(light.inner_cos - light.outer_cos, 1e-4), 0.0, 1.0);
    return t * t;
}

// Cook-Torrance GGX specular + Lambert diffuse for one light, scaled by
// N.L. `radiance` is the light's color * intensity * attenuation.
//
// `energy_compensation` scales specular back up for the energy single-scatter
// GGX loses on rough surfaces (light that bounces between microfacets more
// than once before leaving) -- see main().
vec3 shade_light(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 diffuse_color, vec3 f0, float alpha, float NoV,
                 vec3 energy_compensation)
{
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0)
        return vec3(0.0);

    vec3  H   = normalize(V + L);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);

    vec3 F = fresnel_schlick(VoH, f0);
    vec3 specular = distribution_ggx(NoH, alpha) * visibility_smith_ggx(NoV, NoL, alpha) * F * energy_compensation;
    vec3 diffuse  = (1.0 - F) * diffuse_color / PI;

    return (diffuse + specular) * radiance * NoL;
}

vec3 hemisphere(vec3 dir)
{
    const vec3 sky    = vec3(0.55, 0.65, 0.85);
    const vec3 ground = vec3(0.22, 0.20, 0.18);
    return mix(ground, sky, dir.y * 0.5 + 0.5);
}

void main()
{
    // --- Surface inputs ------------------------------------------------------
    vec4 base = u_base_color;
    if (has_texture(0))
        base *= texture(u_base_color_tex, v_uv);

    if (u_alpha_mode == 1 && base.a < u_alpha_cutoff)
        discard;

    float roughness = u_roughness;
    float metallic  = u_metallic;
    if (has_texture(1))
    {
        vec3 mr = texture(u_metallic_roughness_tex, v_uv).rgb;
        roughness *= mr.g;
        metallic  *= mr.b;
    }
    roughness = clamp(roughness, 0.045, 1.0);   // a perfect mirror makes the highlight vanish
    metallic  = clamp(metallic, 0.0, 1.0);

    float occlusion = 1.0;
    if (has_texture(3))
        occlusion = 1.0 + u_occlusion_strength * (texture(u_occlusion_tex, v_uv).r - 1.0);

    vec3 emissive = u_emissive;
    if (has_texture(4))
        emissive *= texture(u_emissive_tex, v_uv).rgb;

    // --- Shading normal ------------------------------------------------------
    vec3 N = normalize(v_normal);
    if (!gl_FrontFacing)
        N = -N;   // double-sided materials: light the back face as its own front

    vec3 geometric_normal = N;   // before normal mapping -- what the shadow bias offsets along

    if (has_texture(2))
    {
        vec3 T = normalize(v_tangent.xyz - N * dot(N, v_tangent.xyz));   // re-orthogonalise after interpolation
        vec3 B = cross(N, T) * v_tangent.w;

        vec3 tn = texture(u_normal_tex, v_uv).xyz * 2.0 - 1.0;
        tn.xy *= u_normal_scale;

        N = normalize(mat3(T, B, N) * tn);
    }

    vec3 V = normalize(u_view_pos - v_world_pos);
    float NoV = max(dot(N, V), 1e-4);

    vec3 diffuse_color = base.rgb * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
    float alpha = roughness * roughness;

    // Split-sum directional albedo: dfg.x scales F0, dfg.y is a bias, and
    // dfg.x + dfg.y (Ess) is the fraction of energy single-scatter GGX keeps
    // when F0 = 1.
    vec2  dfg = u_use_ibl != 0 ? texture(u_brdf_lut, vec2(NoV, roughness)).rg
                               : env_brdf_approx(roughness, NoV);
    float ess = max(dfg.x + dfg.y, 1e-4);

    // Kulla-Conty, as Filament approximates it: scaling specular by
    // 1 + F0 * (1/Ess - 1) adds back the multiple-scattering energy.
    vec3 energy_compensation = 1.0 + f0 * (1.0 / ess - 1.0);

    // --- Direct light ----------------------------------------------------------
    int  cascade_index = pick_cascade(v_view_depth);
    vec3 direct        = vec3(0.0);

    for (int i = 0; i < u_light_count; ++i)
    {
        scene_light light = u_lights[i];

        vec3  L;
        float attenuation = 1.0;
        if (light.type == LIGHT_DIRECTIONAL)
        {
            L = -light.direction;
        }
        else
        {
            vec3  to_light = light.position - v_world_pos;
            float dist_sq  = dot(to_light, to_light);
            L = to_light * inversesqrt(max(dist_sq, 1e-8));

            attenuation = distance_attenuation(dist_sq, light.range);
            if (light.type == LIGHT_SPOT)
                attenuation *= spot_attenuation(L, light);
        }

        if (attenuation <= 0.0)
            continue;

        if (i == u_shadow_light_index && cascade_index >= 0)
        {
            float geometric_NoL = clamp(dot(geometric_normal, L), 0.0, 1.0);
            attenuation *= 1.0 - shadow_calculation(cascade_index, geometric_normal, geometric_NoL);
        }

        vec3 radiance = light.color * light.intensity * attenuation;
        direct += shade_light(N, V, L, radiance, diffuse_color, f0, alpha, NoV, energy_compensation);
    }

    // --- Ambient --------------------------------------------------------------------
    vec3 R = reflect(-V, N);

    vec3 irradiance;
    vec3 reflection;
    if (u_use_ibl != 0)
    {
        irradiance = texture(u_irradiance_map, N).rgb;
        reflection = textureLod(u_prefiltered_map, R, roughness * u_prefiltered_max_lod).rgb;
    }
    else
    {
        // Hemisphere stand-in used until an environment is loaded.
        irradiance = hemisphere(N);
        reflection = mix(hemisphere(R), irradiance, roughness);   // rougher = blurrier = closer to the average
    }

    // Multiple-scattering split sum (Fdez-Aguera 2019). FssEss is the usual
    // single-scatter specular term, with a roughness-aware Fresnel; FmsEms
    // is the energy that scatters more than once, which arrives roughly like
    // the diffuse irradiance; diffuse gets only what neither specular term
    // took.
    vec3  Fr     = max(vec3(1.0 - roughness), f0) - f0;
    vec3  k_s    = f0 + Fr * pow(1.0 - NoV, 5.0);
    vec3  FssEss = k_s * dfg.x + dfg.y;
    float ems    = 1.0 - ess;
    vec3  F_avg  = f0 + (1.0 - f0) / 21.0;
    vec3  FmsEms = ems * FssEss * F_avg / (1.0 - F_avg * ems);
    vec3  k_d    = diffuse_color * max(1.0 - FssEss - FmsEms, 0.0);

    float specular_ao = specular_occlusion(NoV, occlusion, roughness);

    vec3 ambient = (FssEss * reflection * specular_ao + (FmsEms + k_d) * irradiance * occlusion)
                 * u_ambient_strength;

    vec3 color = direct + ambient + emissive;

    // Debug: overlay a per-cascade tint (red -> green -> blue -> yellow,
    // CSM_MAX_CASCADES == 4) so a cascade fit/split problem shows up as a
    // wrong-looking color band instead of having to guess from shadow
    // artifacts alone.
    if (u_debug_show_cascades != 0 && cascade_index >= 0)
    {
        const vec3 cascade_tints[CSM_MAX_CASCADES] = vec3[](
            vec3(1.0, 0.3, 0.3), vec3(0.3, 1.0, 0.3), vec3(0.3, 0.3, 1.0), vec3(1.0, 1.0, 0.3)
        );
        color = mix(color, cascade_tints[cascade_index], 0.35);
    }

    frag_color = vec4(color, u_alpha_mode == 2 ? base.a : 1.0);
}
