"""The in-app chat agent: builds scenes from the app's chat panel.

A Claude API tool-use loop over the same tools as the MCP server
(pbr_agent.tools). It connects to the running app as a command-server client
holding the role "chat_agent", and then works turn by turn:

    app  --chat.request {turn, text}-->  agent
    agent: Claude <-> tools (engine commands over the same connection)
    agent --chat.event {turn, type: text | progress | tool_call | tool_result | done | error}--> app

The app wraps each turn in one undo step and ends it on "done"/"error", on
Stop (it then sends chat.stop, which cancels the turn here) or when this agent
disconnects (engine/src/Chat/ChatSession.h). The conversation is kept here,
in memory, until the app sends chat.reset ("New chat").

    pbr-agent                       # installed console script
    python -m pbr_agent.agent [--provider P] [--model M] [--effort E] [--discovery PATH] [--exit-with-app]

The app starts it itself (with --exit-with-app); it can also be started by
hand, before or after the app. Claude API credentials come from the
environment (ANTHROPIC_API_KEY, or an `ant auth login` profile). Without
them, --provider (or $PBR_AGENT_PROVIDER) picks an OpenAI-compatible API
instead -- gemini, ollama or openai (pbr_agent.openai_model).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
import uuid
from dataclasses import dataclass
from typing import Any, Awaitable, Callable, Protocol

from . import settings as agent_settings
from . import tracing
from .engine_client import EngineClient, EngineError, EngineNotRunning
from .prompt import IN_APP_INSTRUCTIONS, SCENE_INSTRUCTIONS
from .tools import TOOLS, TOOLS_BY_NAME

ROLE = "chat_agent"
CLIENT_NAME = "pbr-agent"

DEFAULT_MODEL = "claude-opus-5-5"
DEFAULT_EFFORT = "medium"
MAX_TOKENS = 64000
MAX_STEPS = 40                       # model calls in one turn before giving up

# The app makes each turn one undo step (a transaction), and undo can't run
# inside one -- so these would only ever fail here.
EXCLUDED_TOOLS = frozenset({"undo", "redo"})

TEXT_FLUSH_SECONDS = 0.05            # streamed text is sent to the app at most this often
RESULT_SUMMARY_CHARS = 600           # what the panel shows of a tool's result

# A progress block that stands in for unfinished work (it isn't a real note).
INTERRUPTED_SENTINEL = "This part of the response was interrupted before it finished."

# The app's ImGui font only has ASCII; the model's typography would show as '?'.
_DISPLAY_REPLACEMENTS = str.maketrans({
    "‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": " - ",
    "…": "...", " ": " ", "→": "->", "←": "<-", "×": "x", "−": "-",
    "•": "*", "°": " deg",
})


def _log(message: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} pbr-agent: {message}", file=sys.stderr, flush=True)


def _for_display(text: str) -> str:
    return text.translate(_DISPLAY_REPLACEMENTS)


def _field(block: Any, name: str) -> Any:
    """A content block's field, whether it is an SDK object or a dict."""
    return block.get(name) if isinstance(block, dict) else getattr(block, name, None)


def _shorten(text: str, limit: int = RESULT_SUMMARY_CHARS) -> str:
    return text if len(text) <= limit else text[:limit] + f"... ({len(text)} characters)"


class TurnError(Exception):
    """A turn can't go on; the message is shown to the user."""


# --- The model ----------------------------------------------------------------------

@dataclass
class ModelReply:
    content: list          # the assistant message's content blocks, passed back unchanged
    stop_reason: str | None
    usage: dict | None = None   # token counts in LangSmith's usage_metadata shape, when known


TextCallback = Callable[[str], Awaitable[None]]


class Model(Protocol):
    name: str
    problem: str | None    # why it can't answer at all (e.g. no credentials); None = fine
    provider_name: str     # for traces: "anthropic", "gemini", ...

    async def respond(self, messages: list, *, on_text: TextCallback, on_progress: TextCallback) -> ModelReply:
        """One model call. Streams the reply's text through on_text and its
        notes between tool calls through on_progress."""
        ...


def tool_definitions() -> list[dict]:
    """The tools as the Claude API takes them, in a fixed order (prompt caching)."""
    return [
        {"name": t.name, "description": t.description, "input_schema": t.input_schema}
        for t in TOOLS if t.name not in EXCLUDED_TOOLS
    ]


class ClaudeModel:
    """Claude through the Anthropic SDK, streaming."""

    BETAS = [
        "server-side-fallback-2026-07-01",      # fallbacks="default": a declined request is retried on another model
        "thinking-display-updates-2026-08-18",  # thinking display "updates": notes between tool calls as text
        "context-management-2025-06-27",        # clear old tool results (screenshots) as the context grows
    ]

    def __init__(self, name: str = DEFAULT_MODEL, effort: str = DEFAULT_EFFORT):
        import anthropic

        self.name = name
        self.provider_name = "anthropic"
        self.effort = effort
        self.client = anthropic.AsyncAnthropic()
        self.system = [{"type": "text", "text": SCENE_INSTRUCTIONS + IN_APP_INSTRUCTIONS}]
        self.tools = tool_definitions()

        no_credentials = not (self.client.api_key or self.client.auth_token or getattr(self.client, "credentials", None))
        self.problem = ("no Claude API credentials: set ANTHROPIC_API_KEY (or run `ant auth login`) where the "
                        "app is started, then restart the agent") if no_credentials else None

    async def respond(self, messages: list, *, on_text: TextCallback, on_progress: TextCallback) -> ModelReply:
        progress: list[str] = []
        async with self.client.beta.messages.stream(
            model=self.name,
            max_tokens=MAX_TOKENS,
            system=self.system,
            tools=self.tools,
            messages=messages,
            thinking={"type": "adaptive", "display": "updates"},
            output_config={"effort": self.effort},
            cache_control={"type": "ephemeral"},
            context_management={"edits": [{"type": "clear_tool_uses_20250919"}]},
            fallbacks="default",
            betas=self.BETAS,
        ) as stream:
            async for event in stream:
                if event.type == "content_block_delta":
                    if event.delta.type == "text_delta":
                        await on_text(event.delta.text)
                    elif event.delta.type == "thinking_delta" and event.delta.thinking:
                        progress.append(event.delta.thinking)
                elif event.type == "content_block_stop":
                    note = "".join(progress).strip()
                    progress = []
                    if note and note != INTERRUPTED_SENTINEL:
                        await on_progress(note)
            message = await stream.get_final_message()
        return ModelReply(message.content, message.stop_reason, tracing.anthropic_usage(message.usage))


def describe_error(error: Exception) -> str:
    """What the user is told when a turn fails."""
    try:
        import anthropic
    except ImportError:   # pragma: no cover
        anthropic = None

    if anthropic is not None:
        if isinstance(error, anthropic.AuthenticationError):
            return "The Claude API rejected the credentials. Check ANTHROPIC_API_KEY and restart the agent."
        if isinstance(error, anthropic.PermissionDeniedError):
            return f"The Claude API refused the request: {error.message}"
        if isinstance(error, anthropic.NotFoundError):
            return f"The Claude API doesn't know that model (or endpoint): {error.message}"
        if isinstance(error, anthropic.RateLimitError):
            return "The Claude API is rate-limiting requests right now; try again in a moment."
        if isinstance(error, anthropic.APIStatusError):
            return f"The Claude API returned an error ({error.status_code}): {error.message}"
        if isinstance(error, anthropic.APIConnectionError):
            return "Couldn't reach the Claude API. Check the network connection."
    try:
        import openai
    except ImportError:
        openai = None

    if openai is not None:
        if isinstance(error, openai.AuthenticationError):
            return "The model API rejected the API key. Check it and restart the agent."
        if isinstance(error, openai.NotFoundError):
            return f"The model API doesn't know that model (set PBR_AGENT_MODEL): {error.message}"
        if isinstance(error, openai.RateLimitError):
            return ("The model API is rate-limiting requests (free tiers allow only a few per minute); "
                    "wait a moment and try again.")
        if isinstance(error, openai.APIStatusError):
            return f"The model API returned an error ({error.status_code}): {error.message}"
        if isinstance(error, openai.APIConnectionError):
            return "Couldn't reach the model API. Check the network connection (or that Ollama is running)."
    if isinstance(error, TypeError) and "authentication" in str(error):
        return ("No Claude API credentials: set ANTHROPIC_API_KEY (or run `ant auth login`) where the app is "
                "started, then restart the agent.")
    if isinstance(error, TurnError):
        return str(error)
    return f"The agent failed: {error!r}"


# --- The agent ------------------------------------------------------------------------

class ChatAgent:
    """Runs the app's chat turns, one at a time, over one EngineClient."""

    def __init__(self, engine: EngineClient, model: Model, *, max_steps: int = MAX_STEPS):
        self.engine = engine
        self.model = model
        self.max_steps = max_steps

        self.messages: list[dict] = []   # the whole conversation, append-only
        self.thread_id = uuid.uuid4().hex   # names the conversation in traces; New chat starts another
        self.last_trace = tracing.Span()    # the last turn's trace (a no-op span when tracing is off)
        self._task: asyncio.Task | None = None
        self._turn = 0
        self._interrupted = False        # the last turn was stopped part-way

        self._text = ""                  # streamed text not yet sent to the app
        self._text_sent_at = 0.0

        engine.add_notification_listener("chat.request", self._on_request)
        engine.add_notification_listener("chat.stop", self._on_stop)
        engine.add_notification_listener("chat.reset", self._on_reset)

    @property
    def busy(self) -> bool:
        return self._task is not None and not self._task.done()

    async def announce(self) -> None:
        """Tells the app which model this is, or why it can't work."""
        status = {"model": self.model.name}
        if self.model.problem:
            status["problem"] = self.model.problem
        await self.engine.call("chat.status", status)

    def cancel_turn(self) -> None:
        if self.busy:
            self._task.cancel()

    async def wait_idle(self) -> None:
        if self._task is not None:
            await asyncio.gather(self._task, return_exceptions=True)

    # --- from the app -------------------------------------------------------------

    def _on_request(self, params: dict) -> None:
        if self.busy:
            _log(f"ignoring turn {params.get('turn')}: turn {self._turn} is still running")
            return
        self._task = asyncio.create_task(self._run_turn(int(params["turn"]), str(params.get("text", "")),
                                                        params.get("notes")))

    def _on_stop(self, params: dict) -> None:
        if self.busy and params.get("turn") == self._turn:
            _log(f"turn {self._turn} stopped by the user")
            self._task.cancel()

    def _on_reset(self, params: dict) -> None:
        if self.busy:
            return
        self.messages = []
        self.thread_id = uuid.uuid4().hex
        self._interrupted = False
        _log("conversation cleared")

    # --- one turn -------------------------------------------------------------------

    async def _run_turn(self, turn: int, text: str, notes: str | None) -> None:
        self._turn = turn
        self._text = ""
        self._reply = []
        _log(f"turn {turn}: {text[:120]!r}")
        try:
            async with tracing.span("chat turn", "chain", inputs={"text": text, "notes": notes, "turn": turn},
                                    metadata={"thread_id": self.thread_id, "model": self.model.name,
                                              "provider": getattr(self.model, "provider_name", None)}) as trace:
                self.last_trace = trace
                await self._converse(turn, text, notes)
                trace.end({"reply": "".join(self._reply)})
            await self._post(turn, "done")
        except asyncio.CancelledError:
            self._interrupted = True     # the app already ended the turn
        except EngineNotRunning as error:
            _log(f"turn {turn}: lost the app ({error})")
        except Exception as error:
            _log(f"turn {turn} failed: {error!r}")
            await self._post(turn, "error", message=describe_error(error))

    async def _converse(self, turn: int, text: str, notes: str | None) -> None:
        if self.model.problem:
            raise TurnError(f"The agent can't work: {self.model.problem}.")

        app_notes = []
        if self._interrupted:
            app_notes.append("The user stopped your previous turn before you finished.")
        if notes:
            app_notes.append(notes)
        self._interrupted = False

        content = []
        if app_notes:
            content.append({"type": "text", "text": f"(Note from the app: {' '.join(app_notes)})"})
        content.append({"type": "text", "text": text})
        self.messages.append({"role": "user", "content": content})

        async def on_text(chunk: str) -> None:
            self._reply.append(chunk)
            self._text += chunk
            if time.monotonic() - self._text_sent_at >= TEXT_FLUSH_SECONDS:
                await self._flush_text(turn)

        async def on_progress(note: str) -> None:
            await self._post(turn, "progress", text=_for_display(note))

        for _ in range(self.max_steps):
            reply = await self._respond(on_text, on_progress)
            await self._flush_text(turn)
            self.messages.append({"role": "assistant", "content": reply.content})

            if reply.stop_reason == "refusal":
                raise TurnError("The model declined this request.")
            if reply.stop_reason == "pause_turn":
                continue

            tool_uses = [b for b in reply.content if _field(b, "type") == "tool_use"]
            if not tool_uses:
                return

            results: list[dict] = []
            try:
                for block in tool_uses:
                    if reply.stop_reason == "max_tokens":
                        # A tool call cut off mid-input: don't run a partial one.
                        results.append(_tool_result(_field(block, "id"), "The reply was cut off before this "
                                                    "call was complete; it was not run.", ok=False))
                    else:
                        results.append(await self._run_tool(turn, block))
            finally:
                # Every tool call needs its result, even when the turn was
                # stopped half-way; the history stays append-only and valid.
                answered = {r["tool_use_id"] for r in results}
                for block in tool_uses:
                    if _field(block, "id") not in answered:
                        results.append(_tool_result(_field(block, "id"), "Not run: the turn was stopped.", ok=False))
                self.messages.append({"role": "user", "content": results})

        raise TurnError(f"Stopped after {self.max_steps} steps without finishing. Ask again to continue.")

    async def _respond(self, on_text: TextCallback, on_progress: TextCallback) -> ModelReply:
        """One model call, traced as an llm run."""
        model = self.model
        inputs = lambda: tracing.model_inputs(getattr(model, "system", None), self.messages,
                                              getattr(model, "tools", None))
        async with tracing.span(model.name, "llm", inputs=inputs,
                                metadata={"ls_provider": getattr(model, "provider_name", None), "ls_model_name": model.name}) as call:
            reply = await model.respond(self.messages, on_text=on_text, on_progress=on_progress)
            call.end({"content": tracing.plain(reply.content), "stop_reason": reply.stop_reason}, usage=reply.usage)
        return reply

    async def _run_tool(self, turn: int, block: Any) -> dict:
        """One tool call, traced as a tool run (a screenshot attached as a PNG)."""
        name, tool_id, args = _field(block, "name"), _field(block, "id"), _field(block, "input")
        args = args if isinstance(args, dict) else {}
        async with tracing.span(name or "tool", "tool", inputs={"input": args}) as span:
            result = await self._call_tool(turn, tool_id, name, args)
            content = result["content"]
            if isinstance(content, list):   # a screenshot: the image, then its caption
                png = tracing.png_from_base64(content[0]["source"]["data"])
                if png:
                    span.attach("screenshot", "image/png", png)
                span.end({"output": content[-1]["text"]})
            else:
                span.end({"output": content}, error=content if result.get("is_error") else None)
        return result

    async def _call_tool(self, turn: int, tool_id: str, name: str, args: dict) -> dict:
        await self._post(turn, "tool_call", id=tool_id, name=name, input=args)

        tool = None if name in EXCLUDED_TOOLS else TOOLS_BY_NAME.get(name)
        if tool is None:
            message = f"Unknown tool '{name}'."
            await self._post(turn, "tool_result", id=tool_id, ok=False, result=message)
            return _tool_result(tool_id, message, ok=False)

        try:
            method, params = tool.to_command(args)
            result = await self.engine.call(method, params or None)
        except EngineError as error:
            await self._post(turn, "tool_result", id=tool_id, ok=False, result=_for_display(error.message))
            return _tool_result(tool_id, error.message, ok=False)
        except ValueError as error:
            await self._post(turn, "tool_result", id=tool_id, ok=False, result=_for_display(str(error)))
            return _tool_result(tool_id, str(error), ok=False)

        if tool.returns_image:
            caption = f"{result.get('width')}x{result.get('height')} screenshot of the current scene"
            await self._post(turn, "tool_result", id=tool_id, ok=True, result=caption)
            return _tool_result(tool_id, [
                {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": result["data"]}},
                {"type": "text", "text": caption},
            ])

        text = json.dumps(result, separators=(",", ":"), ensure_ascii=False)
        await self._post(turn, "tool_result", id=tool_id, ok=True, result=_for_display(_shorten(text)))
        return _tool_result(tool_id, text)

    # --- to the app -------------------------------------------------------------------

    async def _flush_text(self, turn: int) -> None:
        if self._text:
            text, self._text = self._text, ""
            await self._send_event(turn, "text", text=_for_display(text))
        self._text_sent_at = time.monotonic()

    async def _post(self, turn: int, event_type: str, **fields: Any) -> None:
        await self._flush_text(turn)   # keep the order the model produced things in
        await self._send_event(turn, event_type, **fields)

    async def _send_event(self, turn: int, event_type: str, **fields: Any) -> None:
        # A notification: nothing to wait for, and after Stop the app ignores
        # a turn's late events anyway.
        try:
            await self.engine.notify("chat.event", {"turn": turn, "type": event_type, **fields})
        except EngineNotRunning:
            pass


def _tool_result(tool_use_id: str, content: Any, *, ok: bool = True) -> dict:
    result = {"type": "tool_result", "tool_use_id": tool_use_id, "content": content}
    if not ok:
        result["is_error"] = True
    return result


# --- Running ---------------------------------------------------------------------------

async def serve(model: Model, *, discovery_path: str | None = None, exit_with_app: bool = False,
                retry_seconds: float = 2.0, give_up_seconds: float = 30.0) -> int:
    """Connects to the app and serves its chat until it goes away (with
    exit_with_app) or forever, reconnecting when the app restarts."""
    engine = EngineClient(discovery_path=discovery_path, hello={"client": CLIENT_NAME, "role": ROLE})
    agent = ChatAgent(engine, model)
    started = time.monotonic()
    connected_once = False
    waiting_logged = False

    try:
        while True:
            try:
                await engine.connect()
                connected_once, waiting_logged = True, False
                await agent.announce()
                _log(f"connected to the app; model {model.name}" + (f" -- {model.problem}" if model.problem else ""))
                await engine.wait_closed()
                _log("the app closed the connection")
            except EngineNotRunning as error:
                if not waiting_logged:
                    _log(f"waiting for the app: {error}")
                    waiting_logged = True
            except EngineError as error:
                _log(f"the app refused this agent: {error.message}")
                if exit_with_app and error.command_code == "conflict":
                    return 3

            agent.cancel_turn()
            await agent.wait_idle()
            await engine.close()

            if exit_with_app and (connected_once or time.monotonic() - started > give_up_seconds):
                return 0
            await asyncio.sleep(retry_seconds)
    finally:
        agent.cancel_turn()
        await engine.close()
        try:
            tracing.shutdown()
        except Exception as error:   # never keep the agent from exiting
            _log(f"couldn't send the last traces to LangSmith: {error!r}")


def _log_tracing() -> None:
    wanted = os.environ.get("LANGSMITH_TRACING", os.environ.get("LANGSMITH_TRACING_V2", "")).lower() == "true"
    if tracing.enabled():
        _log(f"LangSmith tracing on (project {os.environ.get('LANGSMITH_PROJECT', 'default')})")
    elif wanted:
        _log("LANGSMITH_TRACING is set but the langsmith package isn't installed: "
             "pip install -e agent[tracing] (see agent/README.md)")


def make_model(settings: dict[str, agent_settings.Setting]) -> Model:
    provider = settings["provider"].value
    if provider == "claude":
        return ClaudeModel(settings["model"].value or DEFAULT_MODEL, settings["effort"].value)
    from .openai_model import OpenAICompatibleModel
    return OpenAICompatibleModel(provider, settings["model"].value, base_url=settings["base_url"].value)


def main() -> None:
    settings_file = agent_settings.DEFAULT_SETTINGS_PATH
    parser = argparse.ArgumentParser(
        description="The PBR Renderer's in-app chat agent. Settings come from the command line, else the "
                    f"environment ($PBR_AGENT_*), else {settings_file}, else the defaults (see pbr_agent.settings).")
    parser.add_argument("--provider", choices=agent_settings.PROVIDERS,
                        help="Model API (default claude); the others are OpenAI-compatible, see pbr_agent.openai_model")
    parser.add_argument("--model", help=f"Model name (default {DEFAULT_MODEL} for Claude; each provider has its own)")
    parser.add_argument("--effort", choices=agent_settings.EFFORTS,
                        help=f"How hard the model thinks (default {DEFAULT_EFFORT})")
    parser.add_argument("--discovery", help="Path to the app's server.json (default: ~/.pbr-renderer/server.json)")
    parser.add_argument("--exit-with-app", action="store_true",
                        help="Exit when the app goes away (how the app starts it) instead of waiting for it")
    args = parser.parse_args()
    command_line = {"provider": args.provider, "model": args.model, "effort": args.effort}

    # A broken settings file still lets the agent connect, so the chat panel
    # can say what's wrong instead of the agent silently using another model.
    settings_problem = None
    try:
        settings = agent_settings.resolve(command_line)
    except agent_settings.SettingsError as error:
        settings_problem = str(error)
        settings = agent_settings.resolve(command_line, use_file=False, environ={})

    model = make_model(settings)
    if settings_problem:
        model.problem = f"the agent's settings are broken -- {settings_problem}"
    _log(f"provider {settings['provider'].value} (from {settings['provider'].source}), model {model.name}")

    # LangSmith uploads on a background thread. As a daemon it can't keep this
    # process alive retrying an unreachable server after the app has quit;
    # serve() still gives queued traces tracing.FLUSH_SECONDS to go out.
    os.environ.setdefault("LANGSMITH_USE_DAEMON", "true")
    _log_tracing()
    try:
        sys.exit(asyncio.run(serve(model, discovery_path=args.discovery,
                                   exit_with_app=args.exit_with_app)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
