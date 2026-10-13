"""The tools an AI gets for building scenes -- defined once, here.

Each Tool is what an LLM sees (name, description, JSON Schema for its
input) plus how it maps onto an engine command (engine/src/Commands). The
MCP server (mcp_server.py) and the in-app agent both expose exactly this
list, so they can't drift apart.

Most tools pass their arguments straight through to a command of the same
argument names; a few are conveniences that build the command's params
(add_light turns {type, position, aim_at, ...} into an entity.create).
Every tool carries a working example; the tests run them all against the
real engine, so a renamed command or argument fails a test rather than an
LLM's tool call.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

# --- Schema building blocks -------------------------------------------------------

def _vec3(description: str, minimum: float | None = None, maximum: float | None = None) -> dict:
    items: dict = {"type": "number"}
    if minimum is not None:
        items["minimum"] = minimum
    if maximum is not None:
        items["maximum"] = maximum
    return {"type": "array", "items": items, "minItems": 3, "maxItems": 3, "description": description}


def _color(description: str) -> dict:
    return _vec3(description + " Linear RGB, each 0-1.", 0.0, 1.0)


ENTITY = {"type": "string", "description": "Entity name, as describe_scene lists it."}
NULLABLE_ENTITY = {"type": ["string", "null"], "description": "Entity name, or null for the scene root."}

POSITION = _vec3("Position [x, y, z] in meters. Y is up; the ground is at y = 0.")

ON_GROUND = {
    "type": "boolean",
    "description": "Rest it on the ground (its lowest point at y = 0). With position, only x and z are used.",
}

RELATIVE_TO = {
    "type": "object",
    "description": (
        "Place it next to or on another entity without overlapping it. 'on_top' stacks it on the "
        "target; the sides keep both on the same floor. left = -X, right = +X, front = +Z (towards "
        "the default camera), behind = -Z."
    ),
    "properties": {
        "entity": ENTITY,
        "side": {"type": "string", "enum": ["on_top", "left", "right", "front", "behind"]},
        "gap": {"type": "number", "minimum": 0, "description": "Space between them in meters (default 0.05 beside, 0 on top)."},
    },
    "required": ["entity", "side"],
    "additionalProperties": False,
}

MATERIAL = {
    "type": "object",
    "description": "Override the model's own material. Omit a field to keep the model's value.",
    "properties": {
        "base_color": _color("Surface color."),
        "metallic": {"type": "number", "minimum": 0, "maximum": 1, "description": "0 = plastic, wood, stone; 1 = metal."},
        "roughness": {"type": "number", "minimum": 0, "maximum": 1, "description": "0 = mirror-smooth, 1 = fully matte."},
        "emissive": _vec3("Emitted light, linear RGB; above 1 glows.", 0.0, 100.0),
    },
    "additionalProperties": False,
}

COMPONENT = {"type": "string", "enum": ["Transform", "MeshRenderer", "Light", "Camera"]}


def _schema(properties: dict, required: list[str] | None = None) -> dict:
    return {
        "type": "object",
        "properties": properties,
        "required": required or [],
        "additionalProperties": False,
    }


# --- Tool -------------------------------------------------------------------------

@dataclass(frozen=True)
class Tool:
    name: str
    description: str
    input_schema: dict
    method: str                                        # engine command it runs
    example: dict = field(default_factory=dict)        # a valid call; run by the tests
    build: Callable[[dict], dict] | None = None        # args -> command params (default: same names)
    read_only: bool = False                            # never changes the scene
    app_command: bool = False                          # provided by the app (Editor), not the engine core
    returns_image: bool = False                        # result has base64 PNG "data" (view.screenshot)

    def to_command(self, args: dict | None) -> tuple[str, dict]:
        """The engine command (method, params) for one call of this tool."""
        args = {k: v for k, v in (args or {}).items() if v is not None or k == "parent"}
        return self.method, (self.build(args) if self.build else args)


def _build_add_light(args: dict) -> dict:
    """add_light -> entity.create with a Transform and a Light."""
    light: dict[str, Any] = {"type": args["type"]}
    for key in ("color", "intensity", "range", "inner_cone_degrees", "outer_cone_degrees"):
        if key in args:
            light[key] = args[key]

    position = args.get("position")
    direction = args.get("direction")
    aim_at = args.get("aim_at")
    if aim_at is not None:
        if position is None:
            raise ValueError("aim_at needs a position to aim from")
        direction = [aim_at[i] - position[i] for i in range(3)]
        if not any(abs(c) > 1e-6 for c in direction):
            raise ValueError("aim_at must differ from position")
    if direction is not None:
        light["direction"] = direction

    components: dict[str, Any] = {"Light": light}
    if position is not None:
        components["Transform"] = {"position": position}

    params: dict[str, Any] = {"components": components}
    if "name" in args:
        params["name"] = args["name"]
    return params


# --- The tools ----------------------------------------------------------------------

TOOLS: list[Tool] = [
    # --- Look --------------------------------------------------------------------
    Tool(
        "describe_scene",
        "Lists every entity in the scene with its components. Call this first, before changing "
        "anything, and whenever unsure what exists. Use summary=true for large scenes.",
        _schema({"summary": {"type": "boolean", "description": "One short line per entity instead of every field."}}),
        "scene.describe", example={"summary": True}, read_only=True,
    ),
    Tool(
        "get_entity",
        "One entity with all of its component fields.",
        _schema({"entity": ENTITY}, ["entity"]),
        "entity.get", example={"entity": "table"}, read_only=True,
    ),
    Tool(
        "list_assets",
        "Models that can be placed: built-in shapes (builtin:cube, builtin:sphere, builtin:plane -- "
        "1 m across) and model files under models/.",
        _schema({}), "assets.list", read_only=True,
    ),
    Tool(
        "describe_components",
        "Every component's fields with type, range and meaning: what set_component accepts.",
        _schema({}), "components.describe", read_only=True,
    ),
    Tool(
        "screenshot",
        "Renders the scene as an image (without the editor's panels). Use it after building or "
        "changing something to check that it looks right: things floating, overlapping, too dark, "
        "out of view.",
        _schema({"max_size": {"type": "integer", "minimum": 64, "maximum": 4096,
                              "description": "Longest side in pixels (default 768)."}}),
        "view.screenshot", example={"max_size": 256}, read_only=True, app_command=True, returns_image=True,
    ),

    # --- Build ---------------------------------------------------------------------
    Tool(
        "place_model",
        "Adds a model to the scene, sized in meters and placed by intent: on the ground (the "
        "default), on top of or beside another entity, or at a position. Give 'size' (its longest "
        "side in meters) for real-world proportions -- model files come in arbitrary units.",
        _schema({
            "model": {"type": "string", "description": "Asset key from list_assets, e.g. 'builtin:cube' or 'models/Avocado.glb'."},
            "name": {"type": "string", "description": "Unique name; defaults to the model's file name. Taken names get _2, _3..."},
            "size": {"type": "number", "exclusiveMinimum": 0, "description": "Longest side in meters."},
            "scale": {"type": "number", "exclusiveMinimum": 0, "description": "Uniform scale factor instead of size."},
            "rotation_deg": {"type": "number", "description": "Turn about the vertical axis, in degrees."},
            "position": POSITION,
            "on_ground": ON_GROUND,
            "relative_to": RELATIVE_TO,
            "material": MATERIAL,
        }, ["model"]),
        "place_model",
        example={"model": "builtin:cube", "name": "table", "size": 1.2, "material": {"base_color": [0.45, 0.3, 0.2], "roughness": 0.6}},
    ),
    Tool(
        "add_light",
        "Adds a light. directional = sunlight from a direction (no position); point = a bulb "
        "shining in all directions from a position; spot = a cone from a position, best aimed "
        "with aim_at. Intensity: directional about 1-5; point and spot about 5-50 (they fade with "
        "distance).",
        _schema({
            "type": {"type": "string", "enum": ["directional", "point", "spot"]},
            "name": {"type": "string"},
            "color": _color("Light color."),
            "intensity": {"type": "number", "minimum": 0, "maximum": 1000},
            "position": POSITION,
            "direction": _vec3("Direction the light travels, e.g. [0, -1, 0] straight down. For directional and spot."),
            "aim_at": _vec3("A point to aim a spot (or directional) light at, instead of direction. Needs position."),
            "range": {"type": "number", "minimum": 0, "description": "Point/spot: distance where it fades out; 0 = no limit."},
            "inner_cone_degrees": {"type": "number", "minimum": 0, "maximum": 89, "description": "Spot: half-angle of the bright core."},
            "outer_cone_degrees": {"type": "number", "minimum": 0, "maximum": 89, "description": "Spot: half-angle where it fades to zero; at least inner."},
        }, ["type"]),
        "entity.create",
        example={"type": "spot", "name": "desk_lamp", "position": [0, 2, 1], "aim_at": [0, 1, 0], "intensity": 20,
                 "color": [1, 0.85, 0.6], "inner_cone_degrees": 15, "outer_cone_degrees": 30},
        build=_build_add_light,
    ),
    Tool(
        "create_entity",
        "Creates an entity from raw components (see describe_components) -- for anything "
        "place_model and add_light don't cover, e.g. an empty parent to group things under.",
        _schema({
            "name": {"type": "string"},
            "parent": NULLABLE_ENTITY,
            "components": {"type": "object", "description": "Component name -> fields, e.g. {\"Transform\": {\"position\": [0, 1, 0]}}."},
        }),
        "entity.create", example={"name": "shelf_group", "components": {"Transform": {"position": [2, 0, 0]}}},
    ),

    # --- Change ---------------------------------------------------------------------
    Tool(
        "move_entity",
        "Moves an entity: onto the ground, on top of or beside another entity, or to a position. "
        "Works on entities without a parent.",
        _schema({"entity": ENTITY, "position": POSITION, "on_ground": ON_GROUND, "relative_to": RELATIVE_TO}, ["entity"]),
        "entity.place",
        example={"entity": "desk_lamp_base", "relative_to": {"entity": "table", "side": "on_top"}},
    ),
    Tool(
        "set_component",
        "Changes some fields of one component (adding the component if missing), e.g. a light's "
        "intensity, a Transform's rotation_deg, a MeshRenderer's material.roughness. Only the "
        "fields given change. describe_components lists every field.",
        _schema({"entity": ENTITY, "component": COMPONENT,
                 "fields": {"type": "object", "description": "Field name -> new value, e.g. {\"intensity\": 8}."}},
                ["entity", "component", "fields"]),
        "component.set",
        example={"entity": "desk_lamp", "component": "Light", "fields": {"intensity": 25}},
    ),
    Tool(
        "remove_component",
        "Removes a MeshRenderer, Light or Camera from an entity (the entity stays).",
        _schema({"entity": ENTITY, "component": {"type": "string", "enum": ["MeshRenderer", "Light", "Camera"]}},
                ["entity", "component"]),
        "component.remove", example={"entity": "desk_lamp", "component": "Light"},
    ),
    Tool(
        "update_entity",
        "Renames an entity and/or changes its parent (null = scene root). A child moves with its parent.",
        _schema({"entity": ENTITY, "name": {"type": "string"}, "parent": NULLABLE_ENTITY}, ["entity"]),
        "entity.update", example={"entity": "shelf_group", "name": "shelves"},
    ),
    Tool(
        "delete_entity",
        "Deletes an entity and everything parented to it (undo brings them back).",
        _schema({"entity": ENTITY}, ["entity"]), "entity.destroy", example={"entity": "shelves"},
    ),
    Tool(
        "run_batch",
        "Runs several tool-level engine commands as one all-or-nothing step: if any fails, none "
        "of them happen, and one undo reverts them all. Each command is "
        "{\"cmd\": <engine command>, \"args\": {...}} using engine command names (place_model, "
        "entity.create, entity.place, component.set, entity.update, entity.destroy).",
        _schema({
            "commands": {"type": "array", "minItems": 1, "items": {
                "type": "object",
                "properties": {"cmd": {"type": "string"}, "args": {"type": "object"}},
                "required": ["cmd"],
            }},
            "label": {"type": "string", "description": "What the whole batch does, shown in the undo history."},
        }, ["commands"]),
        "batch",
        example={"label": "add a lamp base", "commands": [
            {"cmd": "place_model", "args": {"model": "builtin:cube", "name": "desk_lamp_base", "size": 0.15}},
            {"cmd": "component.set", "args": {"entity": "desk_lamp_base", "component": "MeshRenderer",
                                               "fields": {"material.metallic": 1.0}}},
        ]},
    ),
    Tool("undo", "Undoes the last change (a whole run_batch counts as one).", _schema({}), "history.undo"),
    Tool("redo", "Redoes the last undone change.", _schema({}), "history.redo"),
    Tool(
        "clear_scene",
        "Deletes EVERY entity, lights included. Only when asked to start over; undo restores it.",
        _schema({}), "scene.clear",
    ),

    # --- View and environment (provided by the app) --------------------------------------
    Tool(
        "set_camera",
        "Moves the camera and/or the point it looks at.",
        _schema({"position": _vec3("Camera position in meters."), "target": _vec3("Point the camera looks at.")}),
        "view.set_camera", example={"position": [3, 2, 4], "target": [0, 0.5, 0]}, app_command=True,
    ),
    Tool(
        "frame_view",
        "Points the camera at an entity (or at everything visible, if none is given) so it fills the view.",
        _schema({"entity": ENTITY}), "view.frame", example={}, app_command=True,
    ),
    Tool(
        "list_environments",
        "The HDRI environments available to light the scene, and the current one.",
        _schema({}), "environment.list", read_only=True, app_command=True,
    ),
    Tool(
        "set_environment",
        "Changes the HDRI that lights the scene and fills the background (see list_environments).",
        _schema({
            "hdri": {"type": "string", "description": "HDRI path from list_environments."},
            "use_ibl": {"type": "boolean", "description": "Light the scene from the HDRI."},
            "show_skybox": {"type": "boolean", "description": "Show the HDRI as the background."},
            "sky_blur": {"type": "number", "minimum": 0, "maximum": 1},
        }),
        "environment.set", example={"show_skybox": True}, app_command=True,
    ),
    Tool(
        "set_render",
        "Overall brightness (exposure, about 0.5-2) and the strength of the environment's ambient light.",
        _schema({"exposure": {"type": "number", "exclusiveMinimum": 0, "maximum": 16},
                 "ambient_strength": {"type": "number", "minimum": 0, "maximum": 4}}),
        "render.set", example={"exposure": 1.0}, app_command=True,
    ),

    # --- Files ------------------------------------------------------------------------------
    Tool(
        "save_scene",
        "Saves the whole scene under a name (letters, digits, '_' and '-').",
        _schema({"name": {"type": "string", "pattern": "^[A-Za-z0-9_-]{1,64}$"}}, ["name"]),
        "scene.save", example={"name": "tool_test"},
    ),
    Tool("list_scenes", "Names of the saved scenes.", _schema({}), "scene.list", read_only=True),
    Tool(
        "load_scene",
        "Loads a saved scene, replacing the current one (or adding to it with replace=false).",
        _schema({"name": {"type": "string"}, "replace": {"type": "boolean"}}, ["name"]),
        "scene.load", example={"name": "tool_test"},
    ),
]

TOOLS_BY_NAME: dict[str, Tool] = {tool.name: tool for tool in TOOLS}


def check_against_engine(commands_list: dict, *, include_app_commands: bool = True) -> list[str]:
    """Problems between TOOLS and the engine's own command table (the result
    of commands.list): missing commands, and pass-through tools whose
    argument names the command doesn't take. Empty when everything lines up."""
    commands = {c["name"]: {a["name"] for a in c.get("args", [])} for c in commands_list.get("commands", [])}
    problems = []
    for tool in TOOLS:
        if tool.app_command and not include_app_commands:
            continue
        if tool.method not in commands:
            problems.append(f"{tool.name}: the engine has no command '{tool.method}'")
            continue
        if tool.build is None:
            unknown = set(tool.input_schema.get("properties", {})) - commands[tool.method]
            if unknown:
                problems.append(f"{tool.name}: '{tool.method}' takes no argument(s) {sorted(unknown)}")
    return problems
