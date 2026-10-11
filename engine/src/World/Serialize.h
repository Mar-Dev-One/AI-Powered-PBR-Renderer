#pragma once

#include "../Core/Defines.h"
#include "World.h"

#include "cJSON.h"

// JSON <-> World, driven entirely by World/Reflect.h: no component or field
// is named in Serialize.c, so a new one is (de)serialized as soon as it is
// described there. This is the data format of the command layer and, through
// it, of every tool an LLM calls.
//
// Format (field names are Reflect.h's, dotted ones stay flat keys):
//   entity:    { "id": 1048577, "name": "lamp", "parent": "table" | null,
//                "components": { "Transform": {...}, "Light": {...}, ... } }
//   vec3/color: [x, y, z]            enum:   "point"   (its name)
//   asset:      "builtin:cube" / "models/Avocado.glb" / null
//   entity ref: "name" or numeric id / null
// Name and Parent are the "name" / "parent" keys rather than entries under
// "components". A MeshRenderer material override field is only written out
// when it is set; setting it to null clears it (back to the model's own).
//
// Numbers are rounded to 6 significant digits on the way out, so an f32 like
// 0.3f reads back as 0.3, not 0.30000001192092896.

// One entity. NULL if `e` is stale. Caller owns the result (cJSON_Delete).
cJSON* serialize_entity(const world* w, entity_id e);

// Just one component's fields. NULL if `e` is stale or lacks it.
cJSON* serialize_component(const world* w, entity_id e, component_type type);

// { "entities": [ ... ] }, parents before their children.
cJSON* serialize_world(const world* w);

// Writes the fields present in `fields` (a JSON object; omitted fields keep
// their values) into `e`'s `type` component, adding the component with
// defaults first if `e` lacks it. All-or-nothing: every field is validated
// (known name, right JSON type, in range, entity/asset resolvable, no name
// clash or parent cycle) before anything is written, so on false nothing
// changed and `error` (may be NULL) says what to fix, naming the field and
// the accepted values. COMPONENT_NAME and COMPONENT_PARENT are accepted too
// ({ "value": "new_name" } / { "parent": "table" }).
b8 deserialize_component(world* w, entity_id e, component_type type, const cJSON* fields,
                         char* error, size_t error_size);
