#include "ChatPanel.h"

#include "cimgui.h"

#include <stdio.h>
#include <string.h>

static const ImVec4_c COLOR_USER     = { 0.55f, 0.80f, 1.00f, 1.0f };
static const ImVec4_c COLOR_TEXT     = { 0.92f, 0.92f, 0.92f, 1.0f };
static const ImVec4_c COLOR_DIM      = { 0.62f, 0.62f, 0.62f, 1.0f };
static const ImVec4_c COLOR_ERROR    = { 1.00f, 0.45f, 0.40f, 1.0f };
static const ImVec4_c COLOR_RUNNING  = { 1.00f, 0.82f, 0.40f, 1.0f };
static const ImVec4_c COLOR_OK       = { 0.55f, 0.85f, 0.55f, 1.0f };
static const ImVec4_c COLOR_WARNING  = { 1.00f, 0.75f, 0.35f, 1.0f };

void
chat_panel_init(chat_panel* panel)
{
    memset(panel, 0, sizeof(*panel));
    panel->focus_input = true;
}

static void
wrapped(ImVec4_c color, const char* text)
{
    igPushStyleColor_Vec4(ImGuiCol_Text, color);
    igTextWrapped("%s", text);
    igPopStyleColor(1);
}

static void
draw_tool(const chat_item* item, uint32 index)
{
    static const char* const STATE_TEXT[] = { "running...", "ok", "failed" };
    const ImVec4_c           colors[]     = { COLOR_RUNNING, COLOR_OK, COLOR_ERROR };

    char label[160];
    snprintf(label, sizeof(label), "%s  %s##tool%u", item->text, STATE_TEXT[item->tool_state], index);

    igPushStyleColor_Vec4(ImGuiCol_Text, colors[item->tool_state]);
    b8 open = igTreeNodeEx_Str(label, ImGuiTreeNodeFlags_SpanAvailWidth);
    igPopStyleColor(1);

    if (open)
    {
        igTextDisabled("input");
        wrapped(COLOR_DIM, item->tool_input ? item->tool_input : "{}");
        if (item->tool_result)
        {
            igTextDisabled("result");
            wrapped(item->tool_state == CHAT_TOOL_FAILED ? COLOR_ERROR : COLOR_DIM, item->tool_result);
        }
        igTreePop();
    }
}

static void
draw_items(const chat_session* chat)
{
    for (uint32 i = 0; i < chat->item_count; ++i)
    {
        const chat_item* item = &chat->items[i];
        switch (item->kind)
        {
            case CHAT_ITEM_USER:
                if (i > 0)
                    igSeparator();
                igPushStyleColor_Vec4(ImGuiCol_Text, COLOR_USER);
                igTextWrapped("> %s", item->text);
                igPopStyleColor(1);
                break;
            case CHAT_ITEM_ASSISTANT: wrapped(COLOR_TEXT, item->text);  break;
            case CHAT_ITEM_PROGRESS:  wrapped(COLOR_DIM, item->text);   break;
            case CHAT_ITEM_ERROR:     wrapped(COLOR_ERROR, item->text); break;
            case CHAT_ITEM_INFO:      wrapped(COLOR_DIM, item->text);   break;
            case CHAT_ITEM_TOOL:      draw_tool(item, i);               break;
        }
    }
}

// The agent's state, and the Start button when there is none.
static chat_panel_action
draw_status(const chat_session* chat, const char* agent_note)
{
    if (chat_agent_online(chat))
    {
        if (chat->agent_problem[0])
        {
            igPushStyleColor_Vec4(ImGuiCol_Text, COLOR_WARNING);
            igTextWrapped("Agent connected, but: %s", chat->agent_problem);
            igPopStyleColor(1);
        }
        else
        {
            igTextDisabled("Agent: %s", chat->agent_model[0] ? chat->agent_model : "connected");
        }
        return CHAT_PANEL_NONE;
    }

    igPushStyleColor_Vec4(ImGuiCol_Text, COLOR_WARNING);
    igText("Agent offline");
    igPopStyleColor(1);
    igSameLine(0.0f, -1.0f);
    chat_panel_action action = igSmallButton("Start agent") ? CHAT_PANEL_START_AGENT : CHAT_PANEL_NONE;
    if (agent_note && agent_note[0])
        wrapped(COLOR_DIM, agent_note);
    return action;
}

static void
send(chat_panel* panel, chat_session* chat, command_context* ctx)
{
    if (chat_send(chat, ctx, panel->input, panel->error, sizeof(panel->error)))
    {
        panel->input[0] = '\0';
        panel->error[0] = '\0';
    }
    panel->focus_input = true;
}

chat_panel_action
chat_panel_draw(chat_panel* panel, chat_session* chat, command_context* ctx, const char* window_title,
                const char* agent_note)
{
    // First use: left of the entity panel (the right-hand 310 px), above the console.
    const ImGuiViewport* viewport = igGetMainViewport();
    f32 width = viewport->WorkSize.x * 0.3f > 300.0f ? viewport->WorkSize.x * 0.3f : 300.0f;
    igSetNextWindowPos((ImVec2_c){ viewport->WorkPos.x + viewport->WorkSize.x - 320.0f - width, viewport->WorkPos.y + 10.0f },
                       ImGuiCond_FirstUseEver, (ImVec2_c){ 0.0f, 0.0f });
    igSetNextWindowSize((ImVec2_c){ width, viewport->WorkSize.y * 0.58f }, ImGuiCond_FirstUseEver);

    if (!igBegin(window_title, NULL, 0))
    {
        igEnd();
        return CHAT_PANEL_NONE;
    }

    chat_panel_action action = draw_status(chat, agent_note);
    b8 busy = chat_busy(chat);

    // Conversation, filling everything above the input box and buttons.
    f32 input_height = igGetTextLineHeight() * 3.5f;
    f32 footer       = input_height + igGetFrameHeightWithSpacing() * 2.0f;
    igBeginChild_Str("conversation", (ImVec2_c){ 0.0f, -footer }, ImGuiChildFlags_Borders, 0);

    // Follow new output only while already at the bottom, so reading back
    // through a long reply isn't interrupted.
    b8 at_bottom = igGetScrollY() >= igGetScrollMaxY() - 4.0f;

    if (chat->item_count == 0)
        igTextDisabled("Describe what to build or change, e.g. \"put a red sphere on a table and light it warmly\".");
    draw_items(chat);

    if (busy)
    {
        static const char SPINNER[] = "|/-\\";
        igTextDisabled("%c working...", SPINNER[(int)(igGetTime() * 8.0) % 4]);
    }

    if (chat->version != panel->seen_version && at_bottom)
        igSetScrollHereY(1.0f);
    panel->seen_version = chat->version;
    igEndChild();

    if (panel->error[0])
        wrapped(COLOR_ERROR, panel->error);
    else
        wrapped(COLOR_DIM, "Enter: send   Ctrl+Enter: new line");

    // Input.
    if (panel->focus_input)
    {
        igSetKeyboardFocusHere(0);
        panel->focus_input = false;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine;
    b8 entered = igInputTextMultiline("##chat_input", panel->input, sizeof(panel->input),
                                      (ImVec2_c){ -1.0f, input_height }, flags, NULL, NULL);

    b8 can_send = !busy && chat_agent_online(chat) && panel->input[0];
    if (entered)
    {
        if (can_send)
            send(panel, chat, ctx);
        else
            panel->focus_input = true;   // keep typing; Send is unavailable for now
    }

    // Buttons.
    if (busy)
    {
        if (igButton("Stop", (ImVec2_c){ 0, 0 }))
            chat_stop(chat);
    }
    else
    {
        igBeginDisabled(!can_send);
        if (igButton("Send", (ImVec2_c){ 0, 0 }))
            send(panel, chat, ctx);
        igEndDisabled();
    }

    igSameLine(0.0f, -1.0f);
    igBeginDisabled(!chat_can_undo_turn(chat));
    if (igButton("Undo turn", (ImVec2_c){ 0, 0 }))
    {
        if (chat_undo_turn(chat, panel->error, sizeof(panel->error)))
            panel->error[0] = '\0';
    }
    igEndDisabled();
    igSetItemTooltip("Reverts everything the agent changed in its last turn, in one step");

    igSameLine(0.0f, -1.0f);
    igBeginDisabled(busy || chat->item_count == 0);
    if (igButton("New chat", (ImVec2_c){ 0, 0 }))
        chat_clear(chat);
    igEndDisabled();
    igSetItemTooltip("Clears the conversation; the agent forgets it too");

    igEnd();
    return action;
}
