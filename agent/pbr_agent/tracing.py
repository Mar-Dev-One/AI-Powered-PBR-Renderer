"""Optional LangSmith tracing of the in-app agent (https://smith.langchain.com).

Off unless the `langsmith` package is installed (`pip install -e agent[tracing]`)
and LangSmith's own environment variables turn it on, where the app is
started:

    LANGSMITH_TRACING=true
    LANGSMITH_API_KEY=lsv2_...
    LANGSMITH_PROJECT=pbr-agent        # optional; "default" otherwise

Each chat turn is then one trace: the turn (a chain run) holding every model
call (llm runs: the conversation so far, the reply, token usage) and every
tool call (tool runs: arguments and the engine's result). Screenshots are
attached to their tool run as PNG files; in the model calls' inputs they are
replaced by a short placeholder, so a long conversation isn't uploaded with
every screenshot again on every call. Runs of one conversation share a
thread_id, which LangSmith's Threads view groups by; "New chat" starts a new
one.

Without langsmith, or with tracing off, span() hands out a span whose methods
do nothing, and nothing is sent anywhere.
"""

from __future__ import annotations

import base64
import contextlib
from typing import Any, AsyncIterator, Callable

try:
    from langsmith import run_helpers as _run_helpers
    from langsmith import utils as _utils
except ImportError:   # tracing is an optional extra
    _run_helpers = None
    _utils = None

SCREENSHOT_PLACEHOLDER = "[screenshot: attached to the tool run that took it]"


def available() -> bool:
    """The langsmith package is installed."""
    return _run_helpers is not None


def enabled() -> bool:
    """Traces are being recorded (sent, or kept locally in tests)."""
    return _utils is not None and bool(_utils.tracing_is_enabled())


class Span:
    """One run. Set what it produced with end(); attach files with attach().
    `run` is langsmith's RunTree, or None when tracing is off."""

    def __init__(self, run: Any = None):
        self.run = run

    def end(self, outputs: dict | None = None, *, error: str | None = None, usage: dict | None = None) -> None:
        if self.run is None:
            return
        if usage:
            self.run.add_metadata({"usage_metadata": usage})
        self.run.end(outputs=outputs, error=error)

    def attach(self, name: str, mime_type: str, data: bytes) -> None:
        """`name` without periods -- LangSmith drops an attachment named "x.png"."""
        if self.run is None:
            return
        if self.run.attachments is None:
            self.run.attachments = {}
        self.run.attachments[name] = (mime_type, data)


@contextlib.asynccontextmanager
async def span(name: str, run_type: str = "chain", *, inputs: dict | Callable[[], dict] | None = None,
               metadata: dict | None = None, tags: list[str] | None = None) -> AsyncIterator[Span]:
    """A run nested under the current one. `inputs` may be a function, only
    called when tracing is on. An exception leaving the block (cancellation
    included) marks the run as failed."""
    if not enabled():
        yield Span()
        return
    if callable(inputs):
        inputs = inputs()
    async with _run_helpers.trace(name, run_type, inputs=plain(inputs or {}), metadata=metadata,
                                  tags=tags) as run:
        yield Span(run)


FLUSH_SECONDS = 5.0   # unreachable, LangSmith's client would retry for most of a minute


def shutdown() -> None:
    """Sends what's still queued (the uploads run in the background) and
    stops the upload thread, giving up after FLUSH_SECONDS so an unreachable
    LangSmith can't hold the agent open after the app has quit."""
    if enabled() is True:
        from langsmith.run_trees import get_cached_client

        get_cached_client().cleanup(timeout=FLUSH_SECONDS)


@contextlib.contextmanager
def local():
    """For tests: records traces in memory, uploading nothing. A turn's
    trace is then ChatAgent.last_trace.run, with its model and tool calls
    as child_runs."""
    if _run_helpers is None:
        raise RuntimeError("langsmith isn't installed")
    from langsmith.run_trees import configure

    configure(enabled="local")
    try:
        yield
    finally:
        configure(enabled=None)


# --- What gets recorded -----------------------------------------------------------------

def plain(value: Any) -> Any:
    """JSON-ready data from SDK objects (the Claude SDK's content blocks are pydantic models)."""
    if hasattr(value, "model_dump"):
        return plain(value.model_dump(mode="json", exclude_none=True))
    if isinstance(value, dict):
        return {k: plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    return value


def without_images(value: Any) -> Any:
    """`value` (plain()ed) with every image block replaced by a placeholder."""
    if isinstance(value, dict):
        if value.get("type") == "image":
            return {"type": "text", "text": SCREENSHOT_PLACEHOLDER}
        return {k: without_images(v) for k, v in value.items()}
    if isinstance(value, list):
        return [without_images(v) for v in value]
    return value


def model_inputs(system: Any, messages: list, tools: list | None = None) -> dict:
    """An llm run's inputs: the conversation as the model gets it, without screenshots."""
    inputs = {"messages": without_images(plain(messages))}
    if system:
        inputs["system"] = plain(system)
    if tools is not None:
        inputs["tools"] = [t.get("name") for t in tools]
    return inputs


def anthropic_usage(usage: Any) -> dict | None:
    """LangSmith's usage_metadata from a Claude API message's usage."""
    if usage is None:
        return None
    read = getattr(usage, "cache_read_input_tokens", None) or 0
    written = getattr(usage, "cache_creation_input_tokens", None) or 0
    fresh = getattr(usage, "input_tokens", None) or 0
    output = getattr(usage, "output_tokens", None) or 0
    total_input = fresh + read + written   # the API counts cached tokens apart from input_tokens
    return {
        "input_tokens": total_input,
        "output_tokens": output,
        "total_tokens": total_input + output,
        "input_token_details": {"cache_read": read, "cache_creation": written},
    }


def png_from_base64(data: str) -> bytes | None:
    try:
        return base64.b64decode(data, validate=True)
    except (ValueError, TypeError):
        return None
