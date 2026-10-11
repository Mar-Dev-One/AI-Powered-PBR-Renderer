#pragma once

#include "../Core/Defines.h"
#include "Components.h"
#include "World.h"

// A description of every component's fields -- name, type, where it lives
// in the struct, valid range, and a one-line doc -- so code that handles
// components generically never hard-codes them: the editor's Entities
// panel (UI/EntityPanel.h), and next, JSON (de)serialization, command
// validation and the tool schemas handed to an LLM.
//
// Writing: most fields can be written in place at `offset`. Fields marked
// `via_setter` must not be -- they carry an invariant World enforces
// (unique names, asset reference counts, parent cycles), so writes go
// through world_rename / world_set_model / world_set_parent instead.
// `override_bit` != 0 marks a MeshRenderer material field: writing it must
// also set that bit in material.fields, or the value is ignored at draw.

typedef enum field_type
{
    FIELD_F32,
    FIELD_BOOL,     // b8
    FIELD_VEC3,     // 3 x f32
    FIELD_COLOR3,   // 3 x f32, linear RGB
    FIELD_ENUM,     // stored as a C enum (int-sized); see enum_names
    FIELD_STRING,   // char[ENTITY_NAME_MAX]
    FIELD_ASSET,    // asset_id
    FIELD_ENTITY,   // entity_id
    FIELD_FLAGS     // uint32 bitmask
} field_type;

typedef struct field_desc
{
    const char*        name;          // dotted for nested data, e.g. "material.base_color"
    field_type         type;
    uint32             offset;        // byte offset within the component struct
    f32                min, max;      // numeric range; min == max means unbounded
    const char* const* enum_names;    // FIELD_ENUM: names indexed by value
    uint32             enum_count;
    b8                 via_setter;
    uint32             override_bit;
    const char*        doc;
} field_desc;

typedef struct component_desc
{
    const char*       name;   // "Transform", "MeshRenderer", ...
    component_type    type;
    uint32            size;
    const field_desc* fields;
    uint32            field_count;
} component_desc;

// Descriptor for `type`, or NULL if out of range.
const component_desc* reflect_component(component_type type);

// Case-sensitive lookup by component_desc::name; NULL if unknown.
const component_desc* reflect_component_by_name(const char* name);

// Lookup of one field by name within a component; NULL if unknown.
const field_desc* reflect_field(const component_desc* component, const char* name);

// Address of `e`'s `type` component, NULL if `e` is stale or lacks it.
// COMPONENT_PARENT points at the stored parent ID.
void* reflect_component_data(const world* w, entity_id e, component_type type);
