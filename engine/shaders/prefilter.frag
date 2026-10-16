#version 460 core

// Bake pass 3: the specular half of the split-sum approximation. Convolves
// the environment with the GGX lobe for one roughness value, once per mip
// level of the prefiltered cube map (roughness = mip / (mip_count - 1)).
// pbr.frag then picks a mip from the surface's roughness.
//
// Assumes N = V = R (the standard split-sum simplification), which is why the
// stretched-lobe look at grazing angles is only approximate -- that's the
// known cost of prefiltering.

in vec3 v_dir;

out vec4 frag_color;

uniform samplerCube u_environment;
uniform float u_roughness;
uniform float u_target_size;   // edge length in texels of the mip being written

const float PI = 3.14159265359;
const uint  SAMPLE_COUNT = 1024u;

float radical_inverse_vdc(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;   // / 0x100000000
}

vec2 hammersley(uint i, uint n)
{
    return vec2(float(i) / float(n), radical_inverse_vdc(i));
}

// GGX importance sampling: returns a half vector around N distributed like
// the GGX normal distribution, so more samples land where the lobe is strong.
vec3 importance_sample_ggx(vec2 xi, vec3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(1.0 - cos_theta * cos_theta);

    vec3 h = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

    vec3 helper = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 tangent = normalize(cross(helper, N));
    vec3 bitangent = cross(N, tangent);

    return normalize(tangent * h.x + bitangent * h.y + N * h.z);
}

float distribution_ggx(float NoH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

void main()
{
    vec3 N = normalize(v_dir);
    float source_size = float(textureSize(u_environment, 0).x);

    // A perfect mirror: the lobe is a delta, and the GGX terms below are 0/0.
    // Just copy the environment, from the mip whose resolution matches this
    // (smaller) target so it isn't aliased.
    if (u_roughness < 0.001)
    {
        frag_color = vec4(textureLod(u_environment, N, max(log2(source_size / u_target_size), 0.0)).rgb, 1.0);
        return;
    }

    vec3 V = N;

    // Solid angle of one source texel; each sample is read from the mip whose
    // texel covers the solid angle that sample represents (1 / (N * pdf)).
    // Dense samples in the lobe's core read fine mips, sparse ones in the
    // tails read blurry mips -- this is what suppresses the sparkle noise a
    // bright sun would otherwise cause.
    float sa_texel = 4.0 * PI / (6.0 * source_size * source_size);

    vec3  color = vec3(0.0);
    float weight = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 xi = hammersley(i, SAMPLE_COUNT);
        vec3 H  = importance_sample_ggx(xi, N, u_roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NoL = dot(N, L);
        if (NoL > 0.0)
        {
            float NoH = max(dot(N, H), 0.0);
            float HoV = max(dot(H, V), 1e-4);

            float D   = distribution_ggx(NoH, u_roughness);
            float pdf = D * NoH / (4.0 * HoV) + 1e-4;

            float sa_sample = 1.0 / (float(SAMPLE_COUNT) * pdf + 1e-4);
            float lod = 0.5 * log2(sa_sample / sa_texel);

            color  += textureLod(u_environment, L, lod).rgb * NoL;
            weight += NoL;
        }
    }

    frag_color = vec4(color / max(weight, 1e-4), 1.0);
}
