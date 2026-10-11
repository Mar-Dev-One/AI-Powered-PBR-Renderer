#include "History.h"

#include <stdio.h>
#include <string.h>

void
history_init(command_history* h)
{
    memset(h, 0, sizeof(*h));
}

// Retains (delta = +1) or releases (-1) the models a change's states use.
static void
adjust_change_assets(world* w, const history_change* c, int delta)
{
    const entity_state* states[2] = { &c->before, &c->after };
    for (int i = 0; i < 2; ++i)
    {
        const entity_state* s = states[i];
        if (!s->alive || !(s->mask & COMPONENT_BIT(COMPONENT_MESH_RENDERER)))
            continue;

        if (delta > 0)
            assets_retain(w->assets, s->mesh_renderer.model);
        else
            assets_release(w->assets, s->mesh_renderer.model);
    }
}

static void
free_step(world* w, history_step* step)
{
    for (uint32 i = 0; i < step->change_count; ++i)
        adjust_change_assets(w, &step->changes[i], -1);

    free(step->changes);
    memset(step, 0, sizeof(*step));
}

// Same for a transaction's start snapshot.
static void
adjust_start_assets(world* w, const entity_state* states, uint32 count, int delta)
{
    for (uint32 i = 0; i < count; ++i)
    {
        history_change as_change = { states[i], states[i] };
        as_change.after.alive = false;   // count each state once
        adjust_change_assets(w, &as_change, delta);
    }
}

void
history_shutdown(command_history* h, world* w)
{
    for (uint32 i = 0; i < h->step_count; ++i)
        free_step(w, &h->steps[i]);
    free(h->steps);

    if (h->start)
    {
        adjust_start_assets(w, h->start, h->start_count, -1);
        free(h->start);
    }

    memset(h, 0, sizeof(*h));
}

void
history_capture(const world* w, entity_state** out_states, uint32* out_count)
{
    uint32 count = world_slot_count(w);

    entity_state* states = malloc((count ? count : 1) * sizeof(*states));
    if (!states)
        FATAL("history: out of memory");

    world_capture_states(w, states);
    *out_states = states;
    *out_count  = count;
}

uint32
history_diff(const entity_state* before, uint32 before_count,
             const entity_state* after, uint32 after_count,
             history_change** out_changes)
{
    uint32 slots = before_count > after_count ? before_count : after_count;

    history_change* changes = NULL;
    uint32          count   = 0;

    for (uint32 i = 0; i < slots; ++i)
    {
        entity_state b, a;
        if (i < before_count) b = before[i]; else world_empty_state(i, &b);
        if (i < after_count)  a = after[i];  else world_empty_state(i, &a);

        if (!entity_state_differs(&b, &a, NULL))
            continue;

        if (!changes)
        {
            changes = malloc(slots * sizeof(*changes));
            if (!changes)
                FATAL("history: out of memory");
        }
        changes[count].before = b;
        changes[count].after  = a;
        ++count;
    }

    *out_changes = changes;
    return count;
}

void
history_push(command_history* h, world* w, const char* label, history_change* changes, uint32 change_count)
{
    // A new step makes the undone ones unreachable.
    for (uint32 i = h->cursor; i < h->step_count; ++i)
        free_step(w, &h->steps[i]);
    h->step_count = h->cursor;

    if (h->step_count == HISTORY_MAX_STEPS)
    {
        free_step(w, &h->steps[0]);
        memmove(h->steps, h->steps + 1, (HISTORY_MAX_STEPS - 1) * sizeof(*h->steps));
        h->step_count--;
    }

    if (!h->steps)
    {
        h->steps = calloc(HISTORY_MAX_STEPS, sizeof(*h->steps));
        if (!h->steps)
            FATAL("history: out of memory");
    }

    history_step* step = &h->steps[h->step_count++];
    snprintf(step->label, sizeof(step->label), "%s", label ? label : "change");
    step->changes      = changes;
    step->change_count = change_count;

    for (uint32 i = 0; i < change_count; ++i)
        adjust_change_assets(w, &changes[i], +1);

    h->cursor = h->step_count;
}

void
history_apply(world* w, const history_change* changes, uint32 count, b8 to_after)
{
    if (count == 0)
        return;

    entity_state* states = malloc(count * sizeof(*states));
    if (!states)
        FATAL("history: out of memory");

    for (uint32 i = 0; i < count; ++i)
        states[i] = to_after ? changes[i].after : changes[i].before;

    world_apply_states(w, states, count);
    world_update(w);
    free(states);
}

b8
history_can_undo(const command_history* h)
{
    return h->cursor > 0;
}

b8
history_can_redo(const command_history* h)
{
    return h->cursor < h->step_count;
}

const history_step*
history_undo(command_history* h, world* w)
{
    if (!history_can_undo(h))
        return NULL;

    history_step* step = &h->steps[--h->cursor];
    history_apply(w, step->changes, step->change_count, false);
    return step;
}

const history_step*
history_redo(command_history* h, world* w)
{
    if (!history_can_redo(h))
        return NULL;

    history_step* step = &h->steps[h->cursor++];
    history_apply(w, step->changes, step->change_count, true);
    return step;
}

b8
history_in_transaction(const command_history* h)
{
    return h->depth > 0;
}

void
history_begin(command_history* h, const world* w, const char* label)
{
    if (h->depth++ > 0)
        return;   // nested: the outermost transaction already holds the start state

    snprintf(h->label, sizeof(h->label), "%s", label && label[0] ? label : "transaction");
    history_capture(w, &h->start, &h->start_count);
    adjust_start_assets((world*)w, h->start, h->start_count, +1);
}

static void
end_transaction(command_history* h, world* w)
{
    adjust_start_assets(w, h->start, h->start_count, -1);
    free(h->start);
    h->start       = NULL;
    h->start_count = 0;
    h->depth       = 0;
}

b8
history_commit(command_history* h, world* w, const history_change** out_changes, uint32* out_count)
{
    if (out_changes) *out_changes = NULL;
    if (out_count)   *out_count   = 0;

    if (h->depth == 0)
        return false;
    if (--h->depth > 0)
        return true;   // inner commit: the outermost one records

    h->depth = 1;   // end_transaction() resets it

    entity_state* now;
    uint32        now_count;
    history_capture(w, &now, &now_count);

    history_change* changes;
    uint32          count = history_diff(h->start, h->start_count, now, now_count, &changes);
    free(now);

    // Push before releasing the start snapshot, so no model in both dips
    // to zero references in between.
    if (count > 0)
    {
        history_push(h, w, h->label, changes, count);
        if (out_changes) *out_changes = h->steps[h->step_count - 1].changes;
        if (out_count)   *out_count   = count;
    }
    end_transaction(h, w);
    return true;
}

b8
history_rollback(command_history* h, world* w, history_change** out_changes, uint32* out_count)
{
    if (out_changes) *out_changes = NULL;
    if (out_count)   *out_count   = 0;

    if (h->depth == 0)
        return false;

    entity_state* now;
    uint32        now_count;
    history_capture(w, &now, &now_count);

    // before = now (abandoned), after = start (restored).
    history_change* changes;
    uint32          count = history_diff(now, now_count, h->start, h->start_count, &changes);
    free(now);

    history_apply(w, changes, count, true);
    end_transaction(h, w);

    if (out_changes) *out_changes = changes; else free(changes);
    if (out_count)   *out_count   = count;
    return true;
}
