#pragma once

#include "../Core/Defines.h"
#include "../Assets/AssetCache.h"
#include "../Scene/Light.h"
#include "../Scene/Material.h"

#include <cglm/cglm.h>

// The components a World entity can have (World/World.h). Plain data only:
// World owns the storage and the rules (unique names, asset reference
// counts, parent cycles), and Reflect.h describes every field so tools,
// JSON and editor panels can read and write them generically.

// [generation:12 | index:20]. ENTITY_NULL (0) is never a valid entity:
// generations start at 1.
typedef uint32 entity_id;
#define ENTITY_NULL 0

#define ENTITY_INDEX_BITS 20u
#define ENTITY_INDEX_MASK ((1u << ENTITY_INDEX_BITS) - 1u)
#define ENTITY_GEN_MASK   0xFFFu

// Storage slot of an ID. Only meaningful once the ID is known to be live
// (world_is_alive()); World's own functions check that for you.
static inline uint32 entity_slot(entity_id e) { return e & ENTITY_INDEX_MASK; }

typedef enum component_type
{
    COMPONENT_NAME,            // always present
    COMPONENT_TRANSFORM,       // always present
    COMPONENT_PARENT,
    COMPONENT_MESH_RENDERER,
    COMPONENT_LIGHT,
    COMPONENT_CAMERA,
    COMPONENT_SPIN,
    COMPONENT_REFLECTION_PROBE,
    COMPONENT_PLAYER_CONTROLLER,
    COMPONENT_SCENE_TRIGGER,
    COMPONENT_TYPE_COUNT
} component_type;

#define COMPONENT_BIT(type) (1u << (type))

#define ENTITY_NAME_MAX 64

typedef struct c_name
{
    char value[ENTITY_NAME_MAX];   // unique within its World
} c_name;

// Local placement relative to the parent (or the world, if unparented).
// Rotation is Euler angles in degrees, applied yaw (Y), then pitch (X), then
// roll (Z): local = T * Ry * Rx * Rz * S. Degrees and this order are what
// people (and LLMs) reason in; the matrix is derived from these each update.
typedef struct c_transform
{
    vec3 position;
    vec3 rotation_deg;
    vec3 scale;
} c_transform;

typedef struct c_parent
{
    entity_id parent;   // set through world_set_parent(), which rejects cycles
} c_parent;

typedef struct c_mesh_renderer
{
    asset_id          model;          // change through world_set_model() -- it keeps reference counts right
    b8                visible;
    b8                cast_shadows;
    material_override material;       // per-entity tweaks over the model's own materials
} c_mesh_renderer;

// Scene/Light.h's light: direction is in the entity's local space and
// rotated into world space by its transform; position comes from the
// entity's world position.
typedef light c_light;

// Looks down the entity's local -Z axis, with +Y up.
typedef struct c_camera
{
    f32 fov_degrees;   // vertical
    f32 near_plane;
    f32 far_plane;
} c_camera;

// Turns the entity's Transform.rotation_deg at a steady rate while the game
// runs (world_tick()). The editor never ticks -- it only previews the turning
// while drawing -- so what it saves is the pose as authored.
typedef struct c_spin
{
    vec3 degrees_per_second;   // added to rotation_deg (x = pitch, y = yaw, z = roll) each second
} c_spin;

// Captures the scene around the entity's position into a cube map
// (Renderer/ReflectionProbes.h), which shiny surfaces inside the box --
// the position +- extents, along the world axes -- reflect instead of the
// distant sky, as if the box's walls were what they see.
typedef struct c_reflection_probe
{
    vec3 extents;   // half the box's size along x, y, z, in meters
} c_reflection_probe;

typedef enum player_mode
{
    PLAYER_WALK,   // along the ground: looking up or down doesn't change the height
    PLAYER_FLY,    // where it looks, and up and down (the up_down action)
} player_mode;

// Lets the player move the entity -- a Camera, to walk or fly through the
// scene -- with the project's input actions (Platform/InputActions.h:
// move_x, move_y, up_down, look_x, look_y, sprint), while the game runs
// (World/PlayerController.h). Looking turns Transform.rotation_deg (yaw and
// pitch); walking keeps the height it starts at until there's physics.
typedef struct c_player_controller
{
    player_mode mode;
    f32         move_speed;          // meters per second
    f32         sprint_multiplier;   // move_speed times this while sprint is held
    f32         look_sensitivity;    // look_x / look_y times this
    b8          invert_look_y;
} c_player_controller;

// A box -- the entity's position +- extents, along the world axes -- that
// takes the player to another scene: when a PlayerController entity walks
// in, the view fades out, `scene` loads, the player is put at the entity
// called `spawn` there (if given) and the view fades back in
// (World/SceneTransition.h). In the game and the editor's Play.
typedef struct c_scene_trigger
{
    vec3 extents;                  // half the box's size along x, y, z, in meters
    f32  fade_seconds;             // each way, out and back in; 0 cuts straight over
    char scene[ENTITY_NAME_MAX];   // a scene name ("level_2"); "" = does nothing yet
    char spawn[ENTITY_NAME_MAX];   // an entity in that scene to arrive at; "" = where its player starts
} c_scene_trigger;
