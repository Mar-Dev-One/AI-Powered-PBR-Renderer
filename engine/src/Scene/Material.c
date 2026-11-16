#include "Material.h"

#include <stdlib.h>

void material_init_default(material* m)
{
    *m = (material){ 0 };

    m->base_color[0] = m->base_color[1] = m->base_color[2] = m->base_color[3] = 1.0f;
    m->metallic           = 0.0f;
    m->roughness          = 0.5f;
    m->normal_scale       = 1.0f;
    m->occlusion_strength = 1.0f;
    m->alpha_mode         = MATERIAL_ALPHA_OPAQUE;
    m->alpha_cutoff       = 0.5f;
    m->double_sided       = false;

    m->transmission         = 0.0f;
    m->ior                  = 1.5f;
    m->thickness            = 0.0f;
    m->attenuation_color[0] = m->attenuation_color[1] = m->attenuation_color[2] = 1.0f;
    m->attenuation_distance = 0.0f;

    m->clearcoat           = 0.0f;
    m->clearcoat_roughness = 0.0f;
    m->specular            = 1.0f;
    m->specular_color[0] = m->specular_color[1] = m->specular_color[2] = 1.0f;
}

void material_bind(const material* m, rhi_shader shader, b8 flip_winding)
{
    int32 texture_mask = 0;

    for (int slot = 0; slot < MATERIAL_SLOT_COUNT; ++slot)
    {
        if (!m->textures[slot])
            continue;

        texture_mask |= (1 << slot);
        rhi_texture_bind(m->textures[slot], (uint32)slot);
    }

    // Sampler -> unit wiring. Cheap, and doing it here means a shader
    // never depends on someone remembering a one-time setup call.
    rhi_shader_set_int(shader, "u_base_color_tex",         MATERIAL_SLOT_BASE_COLOR);
    rhi_shader_set_int(shader, "u_metallic_roughness_tex", MATERIAL_SLOT_METALLIC_ROUGHNESS);
    rhi_shader_set_int(shader, "u_normal_tex",             MATERIAL_SLOT_NORMAL);
    rhi_shader_set_int(shader, "u_occlusion_tex",          MATERIAL_SLOT_OCCLUSION);
    rhi_shader_set_int(shader, "u_emissive_tex",           MATERIAL_SLOT_EMISSIVE);

    rhi_shader_set_int  (shader, "u_texture_mask", texture_mask);
    rhi_shader_set_vec4 (shader, "u_base_color", m->base_color[0], m->base_color[1], m->base_color[2], m->base_color[3]);
    rhi_shader_set_float(shader, "u_metallic", m->metallic);
    rhi_shader_set_float(shader, "u_roughness", m->roughness);
    rhi_shader_set_vec3 (shader, "u_emissive", m->emissive[0], m->emissive[1], m->emissive[2]);
    rhi_shader_set_float(shader, "u_normal_scale", m->normal_scale);
    rhi_shader_set_float(shader, "u_occlusion_strength", m->occlusion_strength);
    rhi_shader_set_int  (shader, "u_alpha_mode", (int32)m->alpha_mode);
    rhi_shader_set_float(shader, "u_alpha_cutoff", m->alpha_cutoff);
    rhi_shader_set_float(shader, "u_transmission", m->transmission);
    rhi_shader_set_float(shader, "u_ior", m->ior);
    rhi_shader_set_float(shader, "u_thickness", m->thickness);
    rhi_shader_set_vec3 (shader, "u_attenuation_color", m->attenuation_color[0], m->attenuation_color[1],
                         m->attenuation_color[2]);
    rhi_shader_set_float(shader, "u_attenuation_distance", m->attenuation_distance);
    rhi_shader_set_float(shader, "u_clearcoat", m->clearcoat);
    rhi_shader_set_float(shader, "u_clearcoat_roughness", m->clearcoat_roughness);
    rhi_shader_set_float(shader, "u_specular", m->specular);
    rhi_shader_set_vec3 (shader, "u_specular_color", m->specular_color[0], m->specular_color[1], m->specular_color[2]);

    rhi_render_state state = rhi_render_state_default();
    state.cull_back_faces = !m->double_sided;
    state.blend_enabled   = (m->alpha_mode == MATERIAL_ALPHA_BLEND);
    state.depth_write     = (m->alpha_mode != MATERIAL_ALPHA_BLEND);
    state.front_face_cw   = flip_winding;
    rhi_set_render_state(state);
}

void material_apply_override(material* m, const material_override* o)
{
    if (!o)
        return;

    if (o->fields & MATERIAL_OVERRIDE_BASE_COLOR)
    {
        m->base_color[0] = o->base_color[0];
        m->base_color[1] = o->base_color[1];
        m->base_color[2] = o->base_color[2];
    }
    if (o->fields & MATERIAL_OVERRIDE_METALLIC)
        m->metallic = o->metallic;
    if (o->fields & MATERIAL_OVERRIDE_ROUGHNESS)
        m->roughness = o->roughness;
    if (o->fields & MATERIAL_OVERRIDE_EMISSIVE)
    {
        m->emissive[0] = o->emissive[0];
        m->emissive[1] = o->emissive[1];
        m->emissive[2] = o->emissive[2];
    }
    if (o->fields & MATERIAL_OVERRIDE_TRANSMISSION)
        m->transmission = o->transmission;
    if (o->fields & MATERIAL_OVERRIDE_IOR)
        m->ior = o->ior;
    if (o->fields & MATERIAL_OVERRIDE_THICKNESS)
        m->thickness = o->thickness;
    if (o->fields & MATERIAL_OVERRIDE_ATTENUATION_COLOR)
    {
        m->attenuation_color[0] = o->attenuation_color[0];
        m->attenuation_color[1] = o->attenuation_color[1];
        m->attenuation_color[2] = o->attenuation_color[2];
    }
    if (o->fields & MATERIAL_OVERRIDE_ATTENUATION_DISTANCE)
        m->attenuation_distance = o->attenuation_distance;
    if (o->fields & MATERIAL_OVERRIDE_CLEARCOAT)
        m->clearcoat = o->clearcoat;
    if (o->fields & MATERIAL_OVERRIDE_CLEARCOAT_ROUGHNESS)
        m->clearcoat_roughness = o->clearcoat_roughness;
    if (o->fields & MATERIAL_OVERRIDE_SPECULAR)
        m->specular = o->specular;
    if (o->fields & MATERIAL_OVERRIDE_SPECULAR_COLOR)
    {
        m->specular_color[0] = o->specular_color[0];
        m->specular_color[1] = o->specular_color[1];
        m->specular_color[2] = o->specular_color[2];
    }
}

void material_destroy(material* m)
{
    if (!m)
        return;

    free(m->name);
    m->name = NULL;
}
