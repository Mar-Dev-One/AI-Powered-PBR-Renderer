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
}

void material_destroy(material* m)
{
    if (!m)
        return;

    free(m->name);
    m->name = NULL;
}
