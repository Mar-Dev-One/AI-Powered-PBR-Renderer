// editor/Viewport.c
//
// The Viewport panel: the scene on screen (Play's copy while playing)
// rendered at the panel's size, through the editor camera or a Camera
// entity; its toolbar; mouse orbit, zoom and picking; and what's drawn over
// the picture -- the overlays (light markers with the selected light's range
// or cone, camera frustums, reflection probe boxes, the ground grid), the
// selection's outline and the gizmo.

#include "Editor.h"

#include "Platform/Window.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* const DEBUG_VIEW_LABELS[SCENE_VIEW_COUNT] = {
    "Lit", "Albedo", "Normals", "Roughness", "Metallic", "AO", "Shadow", "Lighting", "Wireframe", "Lights"
};

#define COLOR_SELECTION 0xFF00AAFFu   // orange (ABGR)
#define COLOR_CAMERA    0xC0E0E0E0u
#define COLOR_PROBE     0xB0FFD040u   // cyan-ish
#define COLOR_GRID      0x30FFFFFFu
#define COLOR_AXIS_X    0x806060FFu
#define COLOR_AXIS_Z    0x80FF7060u

static ImU32
rgba(f32 r, f32 g, f32 b, f32 a)
{
    uint32 R = (uint32)(glm_clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32 G = (uint32)(glm_clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32 B = (uint32)(glm_clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32 A = (uint32)(glm_clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (A << 24) | (B << 16) | (G << 8) | R;
}

// --- The camera on screen -----------------------------------------------------------

void
update_view_camera(editor_state* state)
{
    world* w      = active_world(state);
    f32    aspect = state->view_height > 0.0f ? state->view_width / state->view_height : state->cam.aspect;

    if (state->look_through && !world_has(w, state->look_through, COMPONENT_CAMERA))
        state->look_through = ENTITY_NULL;   // deleted, or its Camera removed

    entity_id e = ENTITY_NULL;
    if (play_active(&state->play) && state->play_game_camera)
        e = world_next(w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_CAMERA));   // what the game picks
    if (!e)
        e = state->look_through;

    if (e && world_camera_view(w, e, aspect, &state->view_cam))
    {
        state->view_cam_entity = e;
        return;
    }
    state->view_cam        = state->cam;
    state->view_cam_entity = ENTITY_NULL;
}

// --- Input ------------------------------------------------------------------------

// A click in the view at (x, y) pixels from its top-left corner: selects
// what's under it, or clears the selection on empty space.
static void
select_at(editor_state* state, f32 x, f32 y)
{
    vec3 origin, direction;
    camera_screen_ray(&state->view_cam, x, y, state->view_width, state->view_height, origin, direction);

    // Lights, cameras and probes have nothing to click on: a 25 cm sphere stands in.
    select_entity(state, world_pick(active_world(state), origin, direction, 0.25f, NULL));
}

// The viewport's mouse: left-drag orbits, a short left click selects, the
// wheel zooms. Reads the item just submitted -- the invisible button
// covering the view -- so ImGui decides who owns the mouse: a press that
// starts on the view keeps orbiting even when the drag leaves it, and a
// press anywhere else never reaches here. Orbiting or zooming while looking
// through a Camera entity goes back to the editor camera, from that view.
static void
view_input(editor_state* state)
{
    const ImGuiIO* io = igGetIO_Nil();

    if (igIsItemActivated())
    {
        state->press_x        = io->MousePos.x;
        state->press_y        = io->MousePos.y;
        state->press_on_gizmo = state->gizmo_hot;
    }

    const f32 click_slop = 4.0f;   // pixels
    b8 dragged = fabsf(io->MousePos.x - state->press_x) > click_slop || fabsf(io->MousePos.y - state->press_y) > click_slop;

    if (igIsItemActive() && !state->press_on_gizmo && dragged && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f))
    {
        if (state->view_cam_entity)
            use_editor_camera(state);
        orbit_drag(&state->orbit, &state->cam, io->MouseDelta.x, io->MouseDelta.y);
    }

    // Released after barely moving: a click, not an orbit -- select.
    if (igIsItemDeactivated() && !state->press_on_gizmo && !dragged)
        select_at(state, io->MousePos.x - state->view_x, io->MousePos.y - state->view_y);

    if (igIsItemHovered(0) && io->MouseWheel != 0.0f)
    {
        if (state->view_cam_entity)
            use_editor_camera(state);
        orbit_zoom(&state->orbit, &state->cam, io->MouseWheel);
    }
}

// --- Drawing over the picture ---------------------------------------------------------
// World-space lines projected onto the view, clipped against the camera's
// near plane (so a line passing behind the camera still draws its visible
// part), into the Viewport window's draw list.

typedef struct overlay
{
    ImDrawList* draw;
    mat4        view_projection;
    f32         x, y, width, height;
} overlay;

static overlay
overlay_begin(const editor_state* state, ImDrawList* draw)
{
    overlay o = { draw, GLM_MAT4_IDENTITY_INIT, state->view_x, state->view_y, state->view_width, state->view_height };
    camera_get_view_projection(&state->view_cam, o.view_projection);
    return o;
}

static ImVec2_c
overlay_screen(const overlay* o, const vec4 clip)
{
    return (ImVec2_c){ o->x + (clip[0] / clip[3] * 0.5f + 0.5f) * o->width,
                       o->y + (0.5f - clip[1] / clip[3] * 0.5f) * o->height };
}

static void
overlay_line(const overlay* o, const vec3 a, const vec3 b, ImU32 color, f32 thickness)
{
    vec4 ca, cb;
    glm_mat4_mulv((vec4*)o->view_projection, (vec4){ a[0], a[1], a[2], 1.0f }, ca);
    glm_mat4_mulv((vec4*)o->view_projection, (vec4){ b[0], b[1], b[2], 1.0f }, cb);

    // In front of the near plane: z >= -w (OpenGL clip space), with a margin.
    f32 da = ca[2] + ca[3] - 1e-4f, db = cb[2] + cb[3] - 1e-4f;
    if (da < 0.0f && db < 0.0f)
        return;
    if (da < 0.0f)
        glm_vec4_lerp(ca, cb, da / (da - db), ca);
    else if (db < 0.0f)
        glm_vec4_lerp(cb, ca, db / (db - da), cb);

    ImDrawList_AddLine(o->draw, overlay_screen(o, ca), overlay_screen(o, cb), color, thickness);
}

// Where a world point lands, if it's in front of the camera.
static b8
overlay_point(const overlay* o, const vec3 p, ImVec2_c* out)
{
    vec4 clip;
    glm_mat4_mulv((vec4*)o->view_projection, (vec4){ p[0], p[1], p[2], 1.0f }, clip);
    if (clip[3] <= 1e-4f)
        return false;
    *out = overlay_screen(o, clip);
    return true;
}

// The 12 edges of the box lo..hi (world axes).
static void
overlay_box(const overlay* o, const vec3 lo, const vec3 hi, ImU32 color, f32 thickness)
{
    static const int EDGES[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },   // along x
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },   // along y
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },   // along z
    };
    vec3 corners[8];
    for (int i = 0; i < 8; ++i)
    {
        corners[i][0] = (i & 1) ? hi[0] : lo[0];
        corners[i][1] = (i & 2) ? hi[1] : lo[1];
        corners[i][2] = (i & 4) ? hi[2] : lo[2];
    }
    for (int i = 0; i < 12; ++i)
        overlay_line(o, corners[EDGES[i][0]], corners[EDGES[i][1]], color, thickness);
}

// A circle of `radius` around `center` in the plane spanned by `u` and `v`
// (unit vectors).
static void
overlay_circle(const overlay* o, const vec3 center, const vec3 u, const vec3 v, f32 radius, ImU32 color)
{
    const int segments = 40;
    vec3      previous = { 0 };
    for (int i = 0; i <= segments; ++i)
    {
        f32  angle = (f32)i / (f32)segments * 2.0f * GLM_PIf;
        vec3 p;
        glm_vec3_copy((f32*)center, p);
        glm_vec3_muladds((f32*)u, cosf(angle) * radius, p);
        glm_vec3_muladds((f32*)v, sinf(angle) * radius, p);
        if (i > 0)
            overlay_line(o, previous, p, color, 1.0f);
        glm_vec3_copy(p, previous);
    }
}

// Two unit vectors at right angles to `axis` (unit) and to each other.
static void
perpendiculars(const vec3 axis, vec3 u, vec3 v)
{
    vec3 helper = { 0.0f, 1.0f, 0.0f };
    if (fabsf(axis[1]) > 0.9f)
        glm_vec3_copy((vec3){ 1.0f, 0.0f, 0.0f }, helper);
    glm_vec3_cross((f32*)axis, helper, u);
    glm_vec3_normalize(u);
    glm_vec3_cross((f32*)axis, u, v);
}

static void
entity_position(const world* w, entity_id e, vec3 out)
{
    const vec4* m = world_matrix(w, e);
    out[0] = m[3][0];
    out[1] = m[3][1];
    out[2] = m[3][2];
}

// The ground grid: 1 m lines at y = 0 around the point the camera looks
// at, with the x and z axes tinted.
static void
draw_grid(const editor_state* state, const overlay* o)
{
    const int half = 20;   // meters each way
    f32 cx = floorf(state->view_cam.target[0]), cz = floorf(state->view_cam.target[2]);
    for (int i = -half; i <= half; ++i)
    {
        f32 x = cx + (f32)i, z = cz + (f32)i;
        overlay_line(o, (vec3){ x, 0.0f, cz - half }, (vec3){ x, 0.0f, cz + half }, x == 0.0f ? COLOR_AXIS_Z : COLOR_GRID, 1.0f);
        overlay_line(o, (vec3){ cx - half, 0.0f, z }, (vec3){ cx + half, 0.0f, z }, z == 0.0f ? COLOR_AXIS_X : COLOR_GRID, 1.0f);
    }
}

// A marker per light in its color; the selected light also shows what it
// reaches: a point light's range as three rings, a spot light's cone out to
// its range, a sun's direction as a line.
static void
draw_light_overlays(const editor_state* state, const world* w, const overlay* o)
{
    const uint32 mask = COMPONENT_BIT(COMPONENT_LIGHT);
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        const c_light* l        = world_light(w, e);
        b8             selected = e == state->selected;
        vec3           position;
        entity_position(w, e, position);

        // Direction in the world: the entity's rotation applied to the local one.
        vec3 direction;
        mat3 rotation;
        glm_mat4_pick3((vec4*)world_matrix(w, e), rotation);
        glm_mat3_mulv(rotation, (f32*)l->direction, direction);
        glm_vec3_normalize(direction);

        f32   peak  = glm_max(glm_max(l->color[0], l->color[1]), glm_max(l->color[2], 1e-3f));
        ImU32 color = rgba(l->color[0] / peak, l->color[1] / peak, l->color[2] / peak, selected ? 1.0f : 0.85f);
        ImU32 faint = rgba(l->color[0] / peak, l->color[1] / peak, l->color[2] / peak, 0.55f);

        ImVec2_c p;
        if (overlay_point(o, position, &p))
        {
            ImDrawList_AddCircleFilled(o->draw, p, 6.0f, 0xC0202020u, 16);
            ImDrawList_AddCircleFilled(o->draw, p, 4.5f, color, 16);
            if (l->type == LIGHT_DIRECTIONAL)   // a sun: rays around the disc
                for (int i = 0; i < 8; ++i)
                {
                    f32 a = (f32)i * GLM_PIf / 4.0f;
                    ImDrawList_AddLine(o->draw, (ImVec2_c){ p.x + cosf(a) * 8.0f, p.y + sinf(a) * 8.0f },
                                       (ImVec2_c){ p.x + cosf(a) * 12.0f, p.y + sinf(a) * 12.0f }, color, 1.5f);
                }
            if (selected)
                ImDrawList_AddCircle(o->draw, p, 12.0f, COLOR_SELECTION, 24, 2.0f);
        }

        if (l->type == LIGHT_DIRECTIONAL)
        {
            vec3 end;
            glm_vec3_copy(position, end);
            glm_vec3_muladds(direction, selected ? 3.0f : 1.0f, end);
            overlay_line(o, position, end, selected ? color : faint, selected ? 2.0f : 1.0f);
        }
        else if (l->type == LIGHT_SPOT)
        {
            f32  length = l->range > 0.0f ? l->range : 3.0f;
            vec3 tip;
            glm_vec3_copy(position, tip);
            glm_vec3_muladds(direction, selected ? length : 0.75f, tip);
            if (!selected)
            {
                overlay_line(o, position, tip, faint, 1.0f);
                continue;
            }

            // The outer cone out to its range, and the inner cone's ring.
            vec3 u, v;
            perpendiculars(direction, u, v);
            f32 outer = tanf(glm_rad(l->outer_cone_degrees)) * length;
            f32 inner = tanf(glm_rad(l->inner_cone_degrees)) * length;
            overlay_circle(o, tip, u, v, outer, color);
            overlay_circle(o, tip, u, v, inner, faint);
            for (int i = 0; i < 4; ++i)
            {
                f32  a = (f32)i * GLM_PIf / 2.0f;
                vec3 rim;
                glm_vec3_copy(tip, rim);
                glm_vec3_muladds(u, cosf(a) * outer, rim);
                glm_vec3_muladds(v, sinf(a) * outer, rim);
                overlay_line(o, position, rim, color, 1.0f);
            }
        }
        else if (l->type == LIGHT_POINT && selected && l->range > 0.0f)
        {
            overlay_circle(o, position, (vec3){ 1, 0, 0 }, (vec3){ 0, 0, 1 }, l->range, color);
            overlay_circle(o, position, (vec3){ 1, 0, 0 }, (vec3){ 0, 1, 0 }, l->range, faint);
            overlay_circle(o, position, (vec3){ 0, 0, 1 }, (vec3){ 0, 1, 0 }, l->range, faint);
        }
    }
}

// Each Camera entity's view as a pyramid, 1 m deep (2 m when selected), with
// a tick on top -- except the one being looked through.
static void
draw_camera_overlays(const editor_state* state, const world* w, const overlay* o)
{
    const uint32 mask   = COMPONENT_BIT(COMPONENT_CAMERA);
    const f32    aspect = state->view_height > 0.0f ? state->view_width / state->view_height : 16.0f / 9.0f;
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        camera c;
        if (e == state->view_cam_entity || !world_camera_view(w, e, aspect, &c))
            continue;

        b8    selected = e == state->selected;
        ImU32 color    = selected ? COLOR_SELECTION : COLOR_CAMERA;
        f32   depth    = selected ? 2.0f : 1.0f;

        vec3 forward, right, up;
        glm_vec3_sub(c.target, c.position, forward);
        glm_vec3_normalize(forward);
        glm_vec3_cross(forward, c.up, right);
        glm_vec3_normalize(right);
        glm_vec3_cross(right, forward, up);

        f32  half_h = tanf(glm_rad(c.fov_degrees) * 0.5f) * depth, half_w = half_h * aspect;
        vec3 center, corners[4];
        glm_vec3_copy(c.position, center);
        glm_vec3_muladds(forward, depth, center);
        for (int i = 0; i < 4; ++i)
        {
            glm_vec3_copy(center, corners[i]);
            glm_vec3_muladds(right, (i == 0 || i == 3) ? -half_w : half_w, corners[i]);
            glm_vec3_muladds(up, i < 2 ? half_h : -half_h, corners[i]);
            overlay_line(o, c.position, corners[i], color, 1.0f);
        }
        for (int i = 0; i < 4; ++i)
            overlay_line(o, corners[i], corners[(i + 1) % 4], color, 1.0f);

        vec3 top;
        glm_vec3_copy(center, top);
        glm_vec3_muladds(up, half_h * 1.4f, top);
        overlay_line(o, corners[0], top, color, 1.0f);
        overlay_line(o, top, corners[1], color, 1.0f);
    }
}

// Each ReflectionProbe's box (and its centre, where it captures from).
static void
draw_probe_overlays(const editor_state* state, const world* w, const overlay* o)
{
    const uint32 mask = COMPONENT_BIT(COMPONENT_REFLECTION_PROBE);
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        const c_reflection_probe* probe = world_reflection_probe(w, e);
        b8   selected = e == state->selected;
        vec3 center, lo, hi;
        entity_position(w, e, center);
        glm_vec3_sub(center, (f32*)probe->extents, lo);
        glm_vec3_add(center, (f32*)probe->extents, hi);
        overlay_box(o, lo, hi, selected ? COLOR_SELECTION : COLOR_PROBE, selected ? 2.0f : 1.0f);

        ImVec2_c p;
        if (overlay_point(o, center, &p))
            ImDrawList_AddCircle(o->draw, p, 5.0f, COLOR_PROBE, 12, 1.5f);
    }
}

// What to outline for `e`: its own bounds, else the bounds of everything
// parented under it (a pivot), else false.
static b8
selection_bounds(const world* w, entity_id e, vec3 lo, vec3 hi)
{
    if (world_bounds(w, e, lo, hi))
        return true;

    b8 any = false;
    glm_vec3_fill(lo, FLT_MAX);
    glm_vec3_fill(hi, -FLT_MAX);
    for (entity_id child = world_next(w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_MESH_RENDERER)); child;
         child = world_next(w, child, COMPONENT_BIT(COMPONENT_MESH_RENDERER)))
    {
        b8 under = false;
        for (entity_id p = world_parent(w, child); p && !under; p = world_parent(w, p))
            under = p == e;

        vec3 child_lo, child_hi;
        if (under && world_bounds(w, child, child_lo, child_hi))
        {
            glm_vec3_minv(lo, child_lo, lo);
            glm_vec3_maxv(hi, child_hi, hi);
            any = true;
        }
    }
    return any;
}

// The selection's bounding box (or a ring at its position for a light,
// camera or empty), with its name. Call after the world was drawn, with the
// same pose (inside a Spin preview).
static void
draw_selection(editor_state* state, const overlay* o)
{
    const world* w = active_world(state);
    if (!world_is_alive(w, state->selected))
        return;

    entity_id e = state->selected;
    vec3      lo, hi, label_at;
    if (selection_bounds(w, e, lo, hi))
    {
        overlay_box(o, lo, hi, COLOR_SELECTION, 2.0f);
        glm_vec3_copy((vec3){ (lo[0] + hi[0]) * 0.5f, hi[1], (lo[2] + hi[2]) * 0.5f }, label_at);
    }
    else
    {
        entity_position(w, e, label_at);
        ImVec2_c p;
        if (!world_has(w, e, COMPONENT_LIGHT) && overlay_point(o, label_at, &p))
            ImDrawList_AddCircle(o->draw, p, 12.0f, COLOR_SELECTION, 24, 2.0f);
    }

    ImVec2_c p;
    if (overlay_point(o, label_at, &p))
        ImDrawList_AddText_Vec2(o->draw, (ImVec2_c){ p.x + 6.0f, p.y - 22.0f }, COLOR_SELECTION, world_name(w, e), NULL);
}

// --- Gizmo --------------------------------------------------------------------------

// Runs {cmd, args} for the gizmo (takes `args`); false and a console line on failure.
static b8
gizmo_run(editor_state* state, command_context* ctx, const char* cmd, cJSON* args)
{
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", cmd);
    cJSON_AddItemToObject(request, "args", args ? args : cJSON_CreateObject());

    state->gizmo_executing = true;
    cJSON* response = cmd_execute(ctx, request);
    state->gizmo_executing = false;

    b8 ok = cJSON_IsTrue(cJSON_GetObjectItem(response, "ok"));
    if (!ok)
    {
        const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "message"));
        console_info(&state->console, message ? message : "gizmo: edit failed");
    }
    cJSON_Delete(response);
    cJSON_Delete(request);
    return ok;
}

// Ends the drag's history group: the whole drag becomes one undo step.
void
gizmo_finish_edit(editor_state* state, command_context* ctx)
{
    if (!state->gizmo_editing)
        return;
    state->gizmo_editing = false;
    gizmo_run(state, ctx, "history.commit", NULL);
}

// Draws the handles on the selection and turns a drag into Transform edits.
// The gizmo works on the world matrix; the edit is the Transform that gives
// it: local = inverse(parent world) * new world, decomposed back into
// position / rotation_deg / scale (World/World.h transform_from_matrix()).
// `view_hovered`: the mouse is over the view; `view_item`: the ID of the
// view's input button, which mustn't keep the handles from taking a click
// (UI/Gizmo.h).
static void
draw_gizmo(editor_state* state, ImDrawList* draw, b8 view_hovered, ImGuiID view_item)
{
    command_context* ctx = active_commands(state);
    world*           w   = ctx->w;

    if (!world_is_alive(w, state->selected))
    {
        state->gizmo_hot = false;
        gizmo_finish_edit(state, ctx);
        return;
    }
    entity_id e = state->selected;

    mat4 view, projection, model;
    camera_get_view(&state->view_cam, view);
    camera_get_projection(&state->view_cam, projection);
    glm_mat4_copy((vec4*)world_matrix(w, e), model);

    const f32  snap_values[] = { state->snap_move, state->snap_rotate, state->snap_scale };
    const b8   snapping      = state->gizmo_snap != igGetIO_Nil()->KeyCtrl;   // Ctrl flips it while dragging
    const f32* snap          = snapping ? &snap_values[state->gizmo_op] : NULL;

    // Live only while the mouse is over the view (or mid-drag); otherwise
    // drawn but deaf, so a click on a panel in front never grabs a handle.
    const b8 interactive = view_hovered || gizmo_is_using();

    b8 changed = gizmo_manipulate((const f32*)view, (const f32*)projection, state->gizmo_op, state->gizmo_space,
                                  (f32*)model, snap, state->view_x, state->view_y, state->view_width,
                                  state->view_height, draw, interactive, view_item);
    state->gizmo_hot = gizmo_is_over() || gizmo_is_using();

    if (changed)
    {
        // Back to the parent's space.
        entity_id parent = world_parent(w, e);
        mat4      local;
        if (parent)
        {
            mat4 parent_inverse;
            glm_mat4_inv((vec4*)world_matrix(w, parent), parent_inverse);
            glm_mat4_mul(parent_inverse, model, local);
        }
        else
        {
            glm_mat4_copy(model, local);
        }

        c_transform t;
        transform_from_matrix(local, &t);
        for (int axis = 0; axis < 3; ++axis)
            t.scale[axis] = glm_max(t.scale[axis], 0.0001f);   // Transform's smallest scale

        static const char* const VERBS[] = { "move", "rotate", "scale" };
        if (!state->gizmo_editing && ctx->history)
        {
            char   label[ENTITY_NAME_MAX + 16];
            cJSON* args = cJSON_CreateObject();
            snprintf(label, sizeof(label), "%s %s", VERBS[state->gizmo_op], world_name(w, e));
            cJSON_AddStringToObject(args, "label", label);
            state->gizmo_editing = gizmo_run(state, ctx, "history.begin", args);
        }

        // Only what this operation changes, so a move never nudges the
        // rotation by a decomposition rounding error.
        cJSON* fields = cJSON_CreateObject();
        if (state->gizmo_op == GIZMO_TRANSLATE)
            cJSON_AddItemToObject(fields, "position", cJSON_CreateFloatArray(t.position, 3));
        else if (state->gizmo_op == GIZMO_ROTATE)
            cJSON_AddItemToObject(fields, "rotation_deg", cJSON_CreateFloatArray(t.rotation_deg, 3));
        else
            cJSON_AddItemToObject(fields, "scale", cJSON_CreateFloatArray(t.scale, 3));

        cJSON* args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "entity", world_name(w, e));
        cJSON_AddStringToObject(args, "component", "Transform");
        cJSON_AddItemToObject(args, "fields", fields);
        gizmo_run(state, ctx, "component.set", args);
        world_update(w);   // this frame's outline and panels see the new pose
    }

    if (state->gizmo_editing && !gizmo_is_using())
        gizmo_finish_edit(state, ctx);
}

// --- Toolbar ---------------------------------------------------------------------

// A button that can be greyed out, with a tooltip saying why (or what it does).
static b8
toolbar_button(const char* label, b8 enabled, const char* tooltip)
{
    igBeginDisabled(!enabled);
    b8 pressed = igButton(label, (ImVec2_c){ 0, 0 });
    igEndDisabled();
    if (tooltip && igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        igSetTooltip("%s", tooltip);
    igSameLine(0.0f, -1.0f);
    return pressed;
}

static void
toolbar_separator(void)
{
    igTextDisabled("|");
    igSameLine(0.0f, -1.0f);
}

static void
tooltip(const char* text)
{
    if (igIsItemHovered(0))
        igSetTooltip("%s", text);
}

// The camera combo: the editor's own camera, or any Camera entity.
static void
draw_camera_picker(editor_state* state)
{
    world*      w       = active_world(state);
    const char* current = state->view_cam_entity ? world_name(w, state->view_cam_entity) : "Editor camera";

    igSetNextItemWidth(130.0f);
    if (igBeginCombo("##camera", current, 0))
    {
        if (igSelectable_Bool("Editor camera", state->view_cam_entity == ENTITY_NULL, 0, (ImVec2_c){ 0, 0 }))
            use_editor_camera(state);
        const uint32 mask  = COMPONENT_BIT(COMPONENT_CAMERA);
        uint32       count = 0;
        for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask), ++count)
        {
            if (igSelectable_Bool(world_name(w, e), state->view_cam_entity == e, 0, (ImVec2_c){ 0, 0 }))
            {
                state->look_through     = e;
                state->play_game_camera = false;
            }
        }
        if (count == 0)
            igTextDisabled("(no Camera entities -- Scene > Add > Camera)");
        igEndCombo();
    }
    tooltip("Look through the editor's camera, or a Camera entity: what a player sees from it.\n"
            "Dragging or zooming goes back to the editor camera.");
    igSameLine(0.0f, -1.0f);

    igCheckbox("Game camera", &state->play_game_camera);
    tooltip("While playing, look through the scene's first Camera, as the exported game does");
    igSameLine(0.0f, -1.0f);
}

// Along the Viewport's top, in two rows: Play / Pause / Step / Stop and the
// gizmo (operation, space, snapping); then the camera, the debug view, the
// overlays and Export.
static void
draw_toolbar(editor_state* state)
{
    const b8 playing = play_active(&state->play);

    if (!playing)
    {
        b8 busy = chat_busy(&state->chat);
        if (toolbar_button("Play", !busy, busy ? "A chat turn is running" : "Run the scene here (Ctrl+P)"))
            play_start(state);
    }
    else if (toolbar_button("Stop", true, "Back to the edited scene; changes made while playing are dropped (Ctrl+P)"))
    {
        play_stop(state);
    }
    if (toolbar_button(state->play.mode == PLAY_PAUSED ? "Resume" : "Pause", playing, "Ctrl+Shift+P"))
        play_toggle_pause(&state->play);
    if (toolbar_button("Step", state->play.mode == PLAY_PAUSED, "One frame forward (F10)"))
        state->play.step = true;
    if (playing)
    {
        igTextColored(state->play.mode == PLAY_PAUSED ? COLOR_WARNING : COLOR_OK, "%s %.1f s",
                      state->play.mode == PLAY_PAUSED ? "Paused" : "Playing", (double)state->play.time);
        igSameLine(0.0f, -1.0f);
    }
    toolbar_separator();

    igRadioButton_IntPtr("Move", (int*)&state->gizmo_op, GIZMO_TRANSLATE);
    tooltip("W");
    igSameLine(0.0f, -1.0f);
    igRadioButton_IntPtr("Rotate", (int*)&state->gizmo_op, GIZMO_ROTATE);
    tooltip("E");
    igSameLine(0.0f, -1.0f);
    igRadioButton_IntPtr("Scale", (int*)&state->gizmo_op, GIZMO_SCALE);
    tooltip("R");
    igSameLine(0.0f, -1.0f);
    toolbar_separator();

    igRadioButton_IntPtr("Local", (int*)&state->gizmo_space, GIZMO_LOCAL);
    igSameLine(0.0f, -1.0f);
    igRadioButton_IntPtr("World", (int*)&state->gizmo_space, GIZMO_WORLD);
    if (state->gizmo_op == GIZMO_SCALE)
        tooltip("Scale always follows the object's own axes");
    igSameLine(0.0f, -1.0f);
    toolbar_separator();

    igCheckbox("Snap", &state->gizmo_snap);
    tooltip("Ctrl while dragging flips it");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(70.0f);
    if (state->gizmo_op == GIZMO_TRANSLATE)
        igDragFloat("##step", &state->snap_move, 0.01f, 0.01f, 10.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
    else if (state->gizmo_op == GIZMO_ROTATE)
        igDragFloat("##step", &state->snap_rotate, 0.5f, 1.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
    else
        igDragFloat("##step", &state->snap_scale, 0.01f, 0.01f, 1.0f, "x%.2f", ImGuiSliderFlags_AlwaysClamp);
    igNewLine();

    draw_camera_picker(state);
    toolbar_separator();

    igSetNextItemWidth(110.0f);
    igCombo_Str_arr("##view", (int*)&state->debug_view, DEBUG_VIEW_LABELS, SCENE_VIEW_COUNT, -1);
    tooltip("What the view shows: the lit scene, or one input to its shading");
    igSameLine(0.0f, -1.0f);

    if (igButton("Overlays", (ImVec2_c){ 0, 0 }))
        igOpenPopup_Str("##overlays", 0);
    tooltip("What's drawn over the scene: light markers, camera frustums, probe boxes, the grid");
    if (igBeginPopup("##overlays", 0))
    {
        igCheckbox("Lights", &state->overlays.lights);
        igCheckbox("Cameras", &state->overlays.cameras);
        igCheckbox("Reflection probes", &state->overlays.probes);
        igCheckbox("Ground grid", &state->overlays.grid);
        igEndPopup();
    }
    igSameLine(0.0f, -1.0f);
    toolbar_separator();

    if (toolbar_button("Export", true, "Make a folder with the game a player can run"))
        open_export_dialog(state);
    igNewLine();
}

// A frame around the view while playing -- green running, amber paused --
// so a scene about to be thrown away is never mistaken for the edited one.
static void
draw_play_frame(editor_state* state, ImDrawList* draw)
{
    if (!play_active(&state->play))
        return;

    const ImU32 color = state->play.mode == PLAY_PAUSED ? 0xFF00B4FF : 0xFF50C850;   // ABGR
    ImDrawList_AddRect(draw, (ImVec2_c){ state->view_x + 1.0f, state->view_y + 1.0f },
                       (ImVec2_c){ state->view_x + state->view_width - 1.0f, state->view_y + state->view_height - 1.0f },
                       color, 0.0f, 3.0f, 0);
}

// "Camera: front_cam" in the corner while looking through a Camera entity.
static void
draw_camera_label(editor_state* state, ImDrawList* draw)
{
    if (!state->view_cam_entity)
        return;
    char label[ENTITY_NAME_MAX + 32];
    snprintf(label, sizeof(label), "Looking through %s", world_name(active_world(state), state->view_cam_entity));
    ImDrawList_AddText_Vec2(draw, (ImVec2_c){ state->view_x + 10.0f, state->view_y + 8.0f }, 0xFFE0E0E0u, label, NULL);
}

// --- Spin preview -------------------------------------------------------------------
// The editor never runs world_tick() on the edited scene (Play ticks a
// copy): what it holds -- and what panels, commands and saving see -- is the
// authored pose. To still show Spin components moving, a frame's draw is
// bracketed by spin_preview_begin() (turn every spinning entity by `time`)
// and spin_preview_end() (put the authored rotations back).

typedef struct spin_preview
{
    entity_id* entities;
    vec3*      rotations;   // authored rotation_deg, restored by spin_preview_end()
    uint32     count;
} spin_preview;

static spin_preview
spin_preview_begin(world* w, f32 time)
{
    spin_preview preview = { 0 };
    const uint32 mask    = COMPONENT_BIT(COMPONENT_SPIN);

    uint32 capacity = world_count(w);
    if (capacity == 0)
        return preview;
    preview.entities  = malloc(capacity * sizeof(*preview.entities));
    preview.rotations = malloc(capacity * sizeof(*preview.rotations));
    if (!preview.entities || !preview.rotations)
        FATAL("editor: out of memory");

    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        c_transform*  t    = world_transform(w, e);
        const c_spin* spin = world_spin(w, e);

        preview.entities[preview.count] = e;
        glm_vec3_copy(t->rotation_deg, preview.rotations[preview.count]);
        ++preview.count;

        for (int axis = 0; axis < 3; ++axis)
            t->rotation_deg[axis] = fmodf(t->rotation_deg[axis] + spin->degrees_per_second[axis] * time, 360.0f);
    }
    return preview;
}

static void
spin_preview_end(world* w, spin_preview* preview)
{
    for (uint32 i = 0; i < preview->count; ++i)
    {
        c_transform* t = world_transform(w, preview->entities[i]);
        if (t)
            glm_vec3_copy(preview->rotations[i], t->rotation_deg);
    }

    // World matrices and bounds back to the authored pose too, for commands
    // that read them (placement relative to an entity, frame, describe).
    if (preview->count)
        world_update(w);

    free(preview->entities);
    free(preview->rotations);
    *preview = (spin_preview){ 0 };
}

// --- The panel ----------------------------------------------------------------------

// (Re)makes the view's render target at `width` x `height`, with the scene
// renderer's HDR target and the camera's aspect to match. A no-op if the
// size is unchanged.
static void
fit_view_target(editor_state* state, uint16 width, uint16 height)
{
    if (state->view_target && state->view_target_width == width && state->view_target_height == height)
        return;

    if (state->view_target)
        rhi_framebuffer_destroy(state->view_target);

    rhi_framebuffer_attachment color = { .format = RHI_FORMAT_RGBA8 };
    state->view_target = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                  = width,
        .height                 = height,
        .color_attachments      = &color,
        .color_attachment_count = 1,
    });
    state->view_target_width  = width;
    state->view_target_height = height;

    scene_renderer_resize(&state->renderer, width, height);
    camera_set_aspect(&state->cam, (f32)width / (f32)height);
}

// The toolbar, then the scene on screen (the copy while playing, ticked by
// `dt`) rendered at the panel's size into view_target and shown as an image,
// with the overlays, the selection outline and the gizmo on top and the
// mouse controls (view_input()) over it. Docked into the main area the
// first time.
void
draw_viewport(editor_state* state, ImGuiID dockspace, f32 dt)
{
    igSetNextWindowDockID(dockspace, ImGuiCond_FirstUseEver);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2_c){ 0.0f, 0.0f });
    // NoNavInputs: with the view focused, keys stay shortcuts (W/E/R, F,
    // Ctrl+Z ...) instead of moving ImGui's keyboard focus.
    b8 open = igBegin(WINDOW_VIEWPORT, NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                             | ImGuiWindowFlags_NoNavInputs);
    igPopStyleVar(1);

    // The game keeps running even when the panel is hidden behind a tab.
    play_advance(&state->play, dt);

    if (open)
    {
        // The toolbar gets the padding the image doesn't.
        ImVec2_c start = igGetCursorScreenPos();
        igSetCursorScreenPos((ImVec2_c){ start.x + 6.0f, start.y + 4.0f });
        igBeginGroup();
        draw_toolbar(state);
        igEndGroup();
        ImVec2_c after = igGetCursorScreenPos();
        igSetCursorScreenPos((ImVec2_c){ start.x, after.y + 2.0f });
    }

    ImVec2_c pos  = igGetCursorScreenPos();
    ImVec2_c size = igGetContentRegionAvail();
    if (!open || size.x < 1.0f || size.y < 1.0f)
    {
        // Hidden behind another tab, collapsed, or no room yet (the first
        // frame of a new layout): nothing to draw or drag. The empty item
        // closes the line the cursor was moved to, as ImGui requires.
        state->gizmo_hot = false;
        gizmo_finish_edit(state, active_commands(state));
        if (open)
            igDummy((ImVec2_c){ 0.0f, 0.0f });
        igEnd();
        return;
    }

    state->view_x      = pos.x;
    state->view_y      = pos.y;
    state->view_width  = floorf(size.x);
    state->view_height = floorf(size.y);
    fit_view_target(state, (uint16)state->view_width, (uint16)state->view_height);

    // One item over the whole view, so ImGui hands it the mouse (and doesn't
    // drag the window around instead).
    igInvisibleButton("##view", (ImVec2_c){ state->view_width, state->view_height }, ImGuiButtonFlags_MouseButtonLeft);
    b8      hovered   = igIsItemHovered(0);
    ImGuiID view_item = igGetItemID();
    view_input(state);

    // Playing, the copy really moves; editing, Spin is only previewed and
    // the authored pose put back after the draw.
    world*       shown   = active_world(state);
    spin_preview preview = { 0 };
    if (!play_active(&state->play))
        preview = spin_preview_begin(shown, (f32)window_get_time());
    world_update(shown);
    update_view_camera(state);   // a Camera entity may be spinning or parented

    scene_render_settings settings = build_render_settings(state);
    settings.delta_time            = dt;     // auto-exposure adapts at the eye's pace
    scene_renderer_draw_to(&state->renderer, shown, &state->view_cam, &settings, state->view_target);

    // GL's rows run bottom-up: flip V.
    ImDrawList*    draw    = igGetWindowDrawList();
    rhi_texture    color   = rhi_framebuffer_get_color_texture(state->view_target, 0);
    ImTextureRef_c texture = { NULL, (ImTextureID)rhi_texture_native_handle(color) };
    ImDrawList_AddImage(draw, texture, pos, (ImVec2_c){ pos.x + state->view_width, pos.y + state->view_height },
                        (ImVec2_c){ 0.0f, 1.0f }, (ImVec2_c){ 1.0f, 0.0f }, 0xFFFFFFFF);

    // Everything drawn over the picture stays inside it.
    ImDrawList_PushClipRect(draw, pos, (ImVec2_c){ pos.x + state->view_width, pos.y + state->view_height }, true);
    overlay o = overlay_begin(state, draw);
    if (state->overlays.grid)
        draw_grid(state, &o);
    if (state->overlays.probes)
        draw_probe_overlays(state, shown, &o);
    if (state->overlays.cameras)
        draw_camera_overlays(state, shown, &o);
    if (state->overlays.lights)
        draw_light_overlays(state, shown, &o);
    draw_selection(state, &o);
    ImDrawList_PopClipRect(draw);

    spin_preview_end(shown, &preview);
    draw_play_frame(state, draw);
    draw_camera_label(state, draw);

    // Handles on the selection, at its saved pose (after any Spin preview).
    draw_gizmo(state, draw, hovered, view_item);
    igEnd();
}
