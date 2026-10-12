#pragma once

#include "../Chat/ChatSession.h"

// An ImGui window for the in-app chat (Chat/ChatSession.h): the
// conversation with streaming replies, one collapsible line per tool call,
// a busy indicator, Send / Stop, Undo turn and New chat. Enter sends,
// Ctrl+Enter starts a new line.

#define CHAT_PANEL_INPUT_MAX 4096

typedef struct chat_panel
{
    char   input[CHAT_PANEL_INPUT_MAX];
    char   error[256];        // why the last send/undo failed; cleared on the next success
    uint64 seen_version;      // the session version last drawn
    b8     focus_input;
} chat_panel;

// What the user asked for that the panel can't do itself.
typedef enum chat_panel_action
{
    CHAT_PANEL_NONE,
    CHAT_PANEL_START_AGENT    // the "Start agent" button (shown while no agent is connected)
} chat_panel_action;

void chat_panel_init(chat_panel* panel);

// Draws the window; turns are sent against `ctx`. `agent_note`, if not NULL,
// is shown while no agent is connected (e.g. why the sidecar exited).
chat_panel_action chat_panel_draw(chat_panel* panel, chat_session* chat, command_context* ctx,
                                  const char* window_title, const char* agent_note);
