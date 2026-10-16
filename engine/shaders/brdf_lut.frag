#version 460 core

// Bake pass 4: the BRDF half of the split-sum approximation. For every
// (N.V, perceptual roughness) pair, integrates the specular BRDF against the
// environment assumed to be a constant white, and splits Schlick's Fresnel
// out of the integral:
//
//     integral ~= F0 * scale + bias        (scale = R, bias = G)
//
// pbr.frag samples this at (N.V, roughness). It uses the same height-correlated
// Smith visibility as its direct-light path, so ambient and direct specular
// agree on the geometry term.

in vec2 v_uv;

out vec2 frag_color;

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

// Same visibility term (and 1/(4 NoL NoV)) as visibility_smith_ggx in pbr.frag.
float visibility_smith_ggx(float NoV, float NoL, float alpha)
{
    float a2 = alpha * alpha;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

void main()
{
    float NoV       = max(v_uv.x, 1e-3);   // V exactly in the surface plane is degenerate
    float roughness = v_uv.y;
    float alpha     = roughness * roughness;

    // Tangent space: N = +Z, V in the XZ plane.
    vec3 V = vec3(sqrt(1.0 - NoV * NoV), 0.0, NoV);

    float scale = 0.0;
    float bias  = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 xi = hammersley(i, SAMPLE_COUNT);

        // GGX importance-sampled half vector (see prefilter.frag).
        float phi       = 2.0 * PI * xi.x;
        float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
        float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
        vec3  H = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

        vec3  L   = 2.0 * dot(V, H) * H - V;
        float NoL = L.z;
        float NoH = H.z;
        float VoH = dot(V, H);

        if (NoL > 0.0 && VoH > 0.0)
        {
            // With pdf = D * NoH / (4 VoH) the D cancels, leaving
            // 4 * V * NoL * VoH / NoH per sample.
            float vis   = visibility_smith_ggx(NoV, NoL, alpha);
            float vis_h = 4.0 * vis * NoL * VoH / max(NoH, 1e-5);
            float fc    = pow(1.0 - VoH, 5.0);

            scale += (1.0 - fc) * vis_h;
            bias  += fc * vis_h;
        }
    }

    frag_color = vec2(scale, bias) / float(SAMPLE_COUNT);
}
