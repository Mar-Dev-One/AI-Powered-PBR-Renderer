#pragma once

#include "../Core/Defines.h"
#include "../World/World.h"

// Undo/redo for a World, as a list of steps. Each step holds the before and
// after state (World.h's entity_state) of exactly the slots it changed, so
// undo and redo restore entities in place, IDs included.
//
// Steps are recorded by the command layer (Commands.h): one per mutating
// command, or one per transaction -- history_begin() ... history_commit()
// groups any number of commands into a single step (one chat turn, one
// batch). Recording a new step discards anything that could be redone.
//
// Models referenced by recorded states are retained for as long as the
// step is in the history, so undoing back to a model that nothing else uses
// any more still finds it loaded.

#define HISTORY_MAX_STEPS   128
#define HISTORY_LABEL_MAX   96

typedef struct history_change
{
    entity_state before;
    entity_state after;
} history_change;

typedef struct history_step
{
    char            label[HISTORY_LABEL_MAX];
    history_change* changes;
    uint32          change_count;
} history_step;

typedef struct command_history
{
    history_step* steps;        // oldest first
    uint32        step_count;
    uint32        cursor;       // steps[0, cursor) are applied; [cursor, step_count) can be redone

    // Open transaction (depth > 0): the world as it was when it began.
    uint32        depth;
    char          label[HISTORY_LABEL_MAX];
    entity_state* start;
    uint32        start_count;
} command_history;

void history_init(command_history* h);
void history_shutdown(command_history* h, world* w);

// Snapshot of every slot of `w`: *out_states is malloc'd, caller frees.
void history_capture(const world* w, entity_state** out_states, uint32* out_count);

// The slots that differ between two snapshots (which may have different
// lengths if the world grew in between). *out_changes is malloc'd (NULL if
// nothing changed), caller frees. Returns the number of changes.
uint32 history_diff(const entity_state* before, uint32 before_count,
                    const entity_state* after, uint32 after_count,
                    history_change** out_changes);

// Records a step (takes ownership of `changes`). Discards the redo tail and,
// past HISTORY_MAX_STEPS, the oldest step.
void history_push(command_history* h, world* w, const char* label, history_change* changes, uint32 change_count);

// Puts `w` back to every change's `before` (undo) or `after` (redo) state.
void history_apply(world* w, const history_change* changes, uint32 count, b8 to_after);

b8                  history_can_undo(const command_history* h);
b8                  history_can_redo(const command_history* h);

// Undo / redo one step. Return the step, or NULL if there is none.
const history_step* history_undo(command_history* h, world* w);
const history_step* history_redo(command_history* h, world* w);

// --- Transactions -------------------------------------------------------------
// Nest freely: only the outermost begin/commit pair records a step.

b8   history_in_transaction(const command_history* h);
void history_begin(command_history* h, const world* w, const char* label);

// Ends the innermost transaction. When it was the outermost, records one
// step with everything that changed since history_begin() (if anything did)
// and hands the changes back through *out_changes / *out_count (both may be
// NULL) without transferring ownership. Returns false if none was open.
b8 history_commit(command_history* h, world* w, const history_change** out_changes, uint32* out_count);

// Abandons every open transaction and puts `w` back to how it was at the
// outermost history_begin(). *out_changes (caller frees; may be NULL) gets
// what was reverted, as before = the abandoned state, after = restored.
// Returns false if none was open.
b8 history_rollback(command_history* h, world* w, history_change** out_changes, uint32* out_count);
