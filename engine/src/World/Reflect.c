#include "Reflect.h"

#include <stddef.h>
#include <string.h>

#define COUNT_OF(array) ((uint32)(sizeof(array) / sizeof((array)[0])))

// Offset of a field inside c_mesh_renderer's embedded material_override.
#define MATERIAL_OFFSET(field) \
    (uint32)(offsetof(c_mesh_renderer, material) + offsetof(material_override, field))

static const field_desc NAME_FIELDS[] = {
    { .name = "value", .type = FIELD_STRING, .offset = offsetof(c_name, value), .via_setter = true,
      .doc = "Unique name within the world; set through world_rename()" },
};

static const field_desc TRANSFORM_FIELDS[] = {
    { .name = "position", .type = FIELD_VEC3, .offset = offsetof(c_transform, position),
      .doc = "Position relative to the parent, in meters (Y up)" },
    { .name = "rotation_deg", .type = FIELD_VEC3, .offset = offsetof(c_transform, rotation_deg),
      .min = -360.0f, .max = 360.0f,
      .doc = "Euler angles in degrees (x = pitch, y = yaw, z = roll), applied yaw, pitch, roll" },
    { .name = "scale", .type = FIELD_VEC3, .offset = offsetof(c_transform, scale),
      .min = 0.0001f, .max = 10000.0f,
      .doc = "Scale along each local axis" },
};

static const field_desc PARENT_FIELDS[] = {
    { .name = "parent", .type = FIELD_ENTITY, .offset = offsetof(c_parent, parent), .via_setter = true,
      .doc = "Entity this one is placed relative to; set through world_set_parent()" },
};

static const field_desc MESH_RENDERER_FIELDS[] = {
    { .name = "model", .type = FIELD_ASSET, .offset = offsetof(c_mesh_renderer, model), .via_setter = true,
      .doc = "Model asset drawn at this entity; set through world_set_model()" },
    { .name = "visible", .type = FIELD_BOOL, .offset = offsetof(c_mesh_renderer, visible),
      .doc = "Whether it is drawn" },
    { .name = "cast_shadows", .type = FIELD_BOOL, .offset = offsetof(c_mesh_renderer, cast_shadows),
      .doc = "Whether it casts shadows from the key directional light" },
    { .name = "material.base_color", .type = FIELD_COLOR3, .offset = MATERIAL_OFFSET(base_color),
      .min = 0.0f, .max = 1.0f, .override_bit = MATERIAL_OVERRIDE_BASE_COLOR,
      .doc = "Override of the surface color, linear RGB" },
    { .name = "material.metallic", .type = FIELD_F32, .offset = MATERIAL_OFFSET(metallic),
      .min = 0.0f, .max = 1.0f, .override_bit = MATERIAL_OVERRIDE_METALLIC,
      .doc = "Override: 0 = dielectric (plastic, wood, stone), 1 = metal" },
    { .name = "material.roughness", .type = FIELD_F32, .offset = MATERIAL_OFFSET(roughness),
      .min = 0.0f, .max = 1.0f, .override_bit = MATERIAL_OVERRIDE_ROUGHNESS,
      .doc = "Override: 0 = mirror-smooth, 1 = fully matte" },
    { .name = "material.emissive", .type = FIELD_COLOR3, .offset = MATERIAL_OFFSET(emissive),
      .min = 0.0f, .max = 100.0f, .override_bit = MATERIAL_OVERRIDE_EMISSIVE,
      .doc = "Override of emitted light, linear RGB (can exceed 1 for glowing surfaces)" },
};

static const char* const LIGHT_TYPE_NAMES[] = { "directional", "point", "spot" };

// FIELD_ENUM fields are read and written as int.
_Static_assert(sizeof(light_type) == sizeof(int), "light_type must be int-sized for FIELD_ENUM");

static const field_desc LIGHT_FIELDS[] = {
    { .name = "type", .type = FIELD_ENUM, .offset = offsetof(light, type),
      .enum_names = LIGHT_TYPE_NAMES, .enum_count = COUNT_OF(LIGHT_TYPE_NAMES),
      .doc = "directional (sun-like, everywhere), point (bulb), or spot (cone)" },
    { .name = "color", .type = FIELD_COLOR3, .offset = offsetof(light, color),
      .min = 0.0f, .max = 1.0f, .doc = "Light color, linear RGB" },
    { .name = "intensity", .type = FIELD_F32, .offset = offsetof(light, intensity),
      .min = 0.0f, .max = 1000.0f,
      .doc = "Directional: illuminance (lux-like). Point/spot: luminous intensity, falls off with distance squared" },
    { .name = "direction", .type = FIELD_VEC3, .offset = offsetof(light, direction), .nonzero = true,
      .doc = "Directional/spot: direction the light travels, in the entity's local space" },
    { .name = "range", .type = FIELD_F32, .offset = offsetof(light, range),
      .min = 0.0f, .max = 1000.0f, .doc = "Point/spot: distance where the light fades to zero; 0 = no limit" },
    { .name = "inner_cone_degrees", .type = FIELD_F32, .offset = offsetof(light, inner_cone_degrees),
      .min = 0.0f, .max = 89.0f, .doc = "Spot: half-angle of the full-intensity core" },
    { .name = "outer_cone_degrees", .type = FIELD_F32, .offset = offsetof(light, outer_cone_degrees),
      .min = 0.0f, .max = 89.0f, .doc = "Spot: half-angle where the light reaches zero" },
    { .name = "cast_shadows", .type = FIELD_BOOL, .offset = offsetof(light, cast_shadows),
      .doc = "Directional/spot: whether it casts shadows (the first directional one, the first 4 spots)" },
};

static const field_desc CAMERA_FIELDS[] = {
    { .name = "fov_degrees", .type = FIELD_F32, .offset = offsetof(c_camera, fov_degrees),
      .min = 1.0f, .max = 179.0f, .doc = "Vertical field of view" },
    { .name = "near_plane", .type = FIELD_F32, .offset = offsetof(c_camera, near_plane),
      .min = 0.001f, .max = 100.0f, .doc = "Closest visible distance" },
    { .name = "far_plane", .type = FIELD_F32, .offset = offsetof(c_camera, far_plane),
      .min = 0.01f, .max = 100000.0f, .doc = "Farthest visible distance" },
};

static const field_desc SPIN_FIELDS[] = {
    { .name = "degrees_per_second", .type = FIELD_VEC3, .offset = offsetof(c_spin, degrees_per_second),
      .min = -3600.0f, .max = 3600.0f,
      .doc = "Turn rate in degrees per second about each axis (x = pitch, y = yaw, z = roll); the game turns the entity; the editor previews it without changing the saved rotation" },
};

static const field_desc REFLECTION_PROBE_FIELDS[] = {
    { .name = "extents", .type = FIELD_VEC3, .offset = offsetof(c_reflection_probe, extents),
      .min = 0.1f, .max = 500.0f,
      .doc = "Half the size of the probe's box along x, y, z in meters: shiny surfaces inside it reflect what the probe sees, lined up with the box's walls" },
};

static const char* const PLAYER_MODE_NAMES[] = { "walk", "fly" };
_Static_assert(sizeof(player_mode) == sizeof(int), "player_mode must be int-sized for FIELD_ENUM");

static const field_desc PLAYER_CONTROLLER_FIELDS[] = {
    { .name = "mode", .type = FIELD_ENUM, .offset = offsetof(c_player_controller, mode),
      .enum_names = PLAYER_MODE_NAMES, .enum_count = COUNT_OF(PLAYER_MODE_NAMES),
      .doc = "walk (along the ground, at the height it starts at) or fly (where it looks, plus up and down)" },
    { .name = "move_speed", .type = FIELD_F32, .offset = offsetof(c_player_controller, move_speed),
      .min = 0.0f, .max = 1000.0f, .doc = "Meters per second; about 1.4 is a stroll, 4 a jog" },
    { .name = "sprint_multiplier", .type = FIELD_F32, .offset = offsetof(c_player_controller, sprint_multiplier),
      .min = 1.0f, .max = 20.0f, .doc = "move_speed times this while the sprint action is held" },
    { .name = "look_sensitivity", .type = FIELD_F32, .offset = offsetof(c_player_controller, look_sensitivity),
      .min = 0.01f, .max = 10.0f, .doc = "How fast the mouse and right stick turn the view, times the input actions' own rates" },
    { .name = "invert_look_y", .type = FIELD_BOOL, .offset = offsetof(c_player_controller, invert_look_y),
      .doc = "Pushing the mouse or stick up looks down" },
};

static const field_desc SCENE_TRIGGER_FIELDS[] = {
    { .name = "scene", .type = FIELD_STRING, .offset = offsetof(c_scene_trigger, scene), .may_be_empty = true,
      .doc = "The scene to go to, by name (scene.list), e.g. \"level_2\"; empty: the trigger does nothing" },
    { .name = "spawn", .type = FIELD_STRING, .offset = offsetof(c_scene_trigger, spawn), .may_be_empty = true,
      .doc = "An entity in that scene where the player arrives, facing its way (an empty entity works); "
             "empty: where that scene's player starts" },
    { .name = "extents", .type = FIELD_VEC3, .offset = offsetof(c_scene_trigger, extents),
      .min = 0.05f, .max = 500.0f,
      .doc = "Half the size of the box along x, y, z in meters, around the entity's position: walking into it changes scene" },
    { .name = "fade_seconds", .type = FIELD_F32, .offset = offsetof(c_scene_trigger, fade_seconds),
      .min = 0.0f, .max = 5.0f, .doc = "How long the view takes to fade out, and again to fade in; 0 cuts straight over" },
};

static const component_desc COMPONENTS[COMPONENT_TYPE_COUNT] = {
    [COMPONENT_NAME]          = { "Name",         COMPONENT_NAME,          sizeof(c_name),          NAME_FIELDS,          COUNT_OF(NAME_FIELDS) },
    [COMPONENT_TRANSFORM]     = { "Transform",    COMPONENT_TRANSFORM,     sizeof(c_transform),     TRANSFORM_FIELDS,     COUNT_OF(TRANSFORM_FIELDS) },
    [COMPONENT_PARENT]        = { "Parent",       COMPONENT_PARENT,        sizeof(c_parent),        PARENT_FIELDS,        COUNT_OF(PARENT_FIELDS) },
    [COMPONENT_MESH_RENDERER] = { "MeshRenderer", COMPONENT_MESH_RENDERER, sizeof(c_mesh_renderer), MESH_RENDERER_FIELDS, COUNT_OF(MESH_RENDERER_FIELDS) },
    [COMPONENT_LIGHT]         = { "Light",        COMPONENT_LIGHT,         sizeof(c_light),         LIGHT_FIELDS,         COUNT_OF(LIGHT_FIELDS) },
    [COMPONENT_CAMERA]        = { "Camera",       COMPONENT_CAMERA,        sizeof(c_camera),        CAMERA_FIELDS,        COUNT_OF(CAMERA_FIELDS) },
    [COMPONENT_SPIN]          = { "Spin",         COMPONENT_SPIN,          sizeof(c_spin),          SPIN_FIELDS,          COUNT_OF(SPIN_FIELDS) },
    [COMPONENT_REFLECTION_PROBE] = { "ReflectionProbe", COMPONENT_REFLECTION_PROBE, sizeof(c_reflection_probe),
                                     REFLECTION_PROBE_FIELDS, COUNT_OF(REFLECTION_PROBE_FIELDS) },
    [COMPONENT_PLAYER_CONTROLLER] = { "PlayerController", COMPONENT_PLAYER_CONTROLLER, sizeof(c_player_controller),
                                      PLAYER_CONTROLLER_FIELDS, COUNT_OF(PLAYER_CONTROLLER_FIELDS) },
    [COMPONENT_SCENE_TRIGGER] = { "SceneTrigger", COMPONENT_SCENE_TRIGGER, sizeof(c_scene_trigger),
                                  SCENE_TRIGGER_FIELDS, COUNT_OF(SCENE_TRIGGER_FIELDS) },
};

const component_desc*
reflect_component(component_type type)
{
    return (uint32)type < COMPONENT_TYPE_COUNT ? &COMPONENTS[type] : NULL;
}

const component_desc*
reflect_component_by_name(const char* name)
{
    if (!name)
        return NULL;

    for (uint32 i = 0; i < COMPONENT_TYPE_COUNT; ++i)
    {
        if (strcmp(COMPONENTS[i].name, name) == 0)
            return &COMPONENTS[i];
    }
    return NULL;
}

const field_desc*
reflect_field(const component_desc* component, const char* name)
{
    if (!component || !name)
        return NULL;

    for (uint32 i = 0; i < component->field_count; ++i)
    {
        if (strcmp(component->fields[i].name, name) == 0)
            return &component->fields[i];
    }
    return NULL;
}

void*
reflect_component_data(const world* w, entity_id e, component_type type)
{
    if (!world_has(w, e, type))
        return NULL;

    // world_has() validated `e`, so its slot is safe to use here.
    uint32 index = entity_slot(e);

    switch (type)
    {
        case COMPONENT_NAME:          return &w->names[index];
        case COMPONENT_TRANSFORM:     return &w->transforms[index];
        case COMPONENT_PARENT:        return &w->parents[index];
        case COMPONENT_MESH_RENDERER: return &w->mesh_renderers[index];
        case COMPONENT_LIGHT:         return &w->lights[index];
        case COMPONENT_CAMERA:        return &w->cameras[index];
        case COMPONENT_SPIN:          return &w->spins[index];
        case COMPONENT_REFLECTION_PROBE: return &w->reflection_probes[index];
        case COMPONENT_PLAYER_CONTROLLER: return &w->player_controllers[index];
        case COMPONENT_SCENE_TRIGGER:     return &w->scene_triggers[index];
        default:                      return NULL;
    }
}
