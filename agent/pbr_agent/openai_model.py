"""The in-app agent's model through an OpenAI-compatible chat API.

For when there is no Claude API access: Google Gemini (free tier with an API
key from https://aistudio.google.com/apikey), a local Ollama or LM Studio, or
any other server that speaks the OpenAI chat-completions protocol.

ChatAgent keeps the conversation in the Claude API's shape (pbr_agent.agent);
this model translates it to chat-completions messages on every call and
translates the reply back into text/tool_use blocks, so the agent itself
doesn't know which backend it talks to.
"""

from __future__ import annotations

import copy
import json
import os
import uuid
from dataclasses import dataclass
from typing import Any

from .agent import ModelReply, TextCallback, _field, _log, tool_definitions
from .prompt import IN_APP_INSTRUCTIONS, SCENE_INSTRUCTIONS

KEEP_SCREENSHOTS = 2      # older screenshots are dropped from the context (free tiers have small token budgets)


@dataclass(frozen=True)
class Provider:
    name: str
    base_url: str | None          # None: from $PBR_AGENT_BASE_URL
    key_variables: tuple[str, ...]
    default_model: str
    needs_key: bool = True
    key_help: str = ""


PROVIDERS = {
    "gemini": Provider("gemini", "https://generativelanguage.googleapis.com/v1beta/openai/",
                       ("GEMINI_API_KEY", "GOOGLE_API_KEY"), "gemini-3.8-flash",
                       key_help="set GEMINI_API_KEY (free key: https://aistudio.google.com/apikey)"),
    "ollama": Provider("ollama", "http://localhost:11434/v1", (), "qwen3", needs_key=False),
    "openai": Provider("openai", None, ("PBR_AGENT_API_KEY", "OPENAI_API_KEY"), "gpt-5-mini", needs_key=False,
                       key_help="set PBR_AGENT_API_KEY"),
}


def compatible_schema(schema: Any) -> Any:
    """A tool's JSON Schema with the parts some providers reject (Gemini in
    particular) rewritten: type unions, exclusiveMinimum, additionalProperties."""
    if isinstance(schema, list):
        return [compatible_schema(s) for s in schema]
    if not isinstance(schema, dict):
        return schema
    out = {}
    for key, value in schema.items():
        if key == "additionalProperties":
            continue
        if key == "exclusiveMinimum":
            out.setdefault("minimum", value)
        elif key == "type" and isinstance(value, list):
            types = [t for t in value if t != "null"]
            out["type"] = types[0] if types else "string"
            if "null" in value:
                out["nullable"] = True
        elif key == "properties":
            out[key] = {name: compatible_schema(s) for name, s in value.items()}
        else:
            out[key] = compatible_schema(value)
    return out


def _tool_result_text(content: Any) -> tuple[str, list[str]]:
    """A tool_result's text and the base64 PNGs it carries."""
    if isinstance(content, str):
        return content, []
    texts, images = [], []
    for block in content or []:
        if block.get("type") == "text":
            texts.append(block["text"])
        elif block.get("type") == "image":
            images.append(block["source"]["data"])
    return "\n".join(texts), images


def to_chat_messages(system: str, messages: list, keep_screenshots: int = KEEP_SCREENSHOTS) -> list[dict]:
    """The conversation (Claude API shape) as chat-completions messages.

    Tool results become "tool" messages; a screenshot can't ride in one, so it
    follows them in a user message. Only the newest keep_screenshots are sent."""
    screenshots_total = sum(
        len(_tool_result_text(b.get("content"))[1])
        for m in messages if m["role"] == "user" and isinstance(m["content"], list)
        for b in m["content"] if isinstance(b, dict) and b.get("type") == "tool_result")
    screenshots_to_skip = max(0, screenshots_total - keep_screenshots)

    out: list[dict] = [{"role": "system", "content": system}]
    for message in messages:
        content = message["content"]
        if isinstance(content, str):
            out.append({"role": message["role"], "content": content})
            continue

        if message["role"] == "assistant":
            text = "".join(_field(b, "text") or "" for b in content if _field(b, "type") == "text")
            calls = []
            for b in content:
                if _field(b, "type") != "tool_use":
                    continue
                call = {"id": _field(b, "id"), "type": "function",
                        "function": {"name": _field(b, "name"),
                                     "arguments": json.dumps(_field(b, "input") or {})}}
                if _field(b, "extra_content"):
                    call["extra_content"] = _field(b, "extra_content")
                calls.append(call)
            entry: dict = {"role": "assistant", "content": text or None}
            if calls:
                entry["tool_calls"] = calls
            out.append(entry)
            continue

        user_parts: list[dict] = []
        for block in content:
            if block.get("type") == "tool_result":
                text, images = _tool_result_text(block.get("content"))
                if block.get("is_error"):
                    text = f"Error: {text}"
                out.append({"role": "tool", "tool_call_id": block["tool_use_id"], "content": text or "(no output)"})
                for data in images:
                    if screenshots_to_skip > 0:
                        screenshots_to_skip -= 1
                        user_parts.append({"type": "text", "text": "(An earlier screenshot, no longer shown.)"})
                    else:
                        user_parts.append({"type": "text", "text": "The screenshot from that tool call:"})
                        user_parts.append({"type": "image_url",
                                           "image_url": {"url": f"data:image/png;base64,{data}"}})
            elif block.get("type") == "text":
                user_parts.append({"type": "text", "text": block["text"]})
        if user_parts:
            if all(p["type"] == "text" for p in user_parts):
                out.append({"role": "user", "content": "\n\n".join(p["text"] for p in user_parts)})
            else:
                out.append({"role": "user", "content": user_parts})
    return out


class OpenAICompatibleModel:
    """A chat-completions model (Gemini, Ollama, ...), streaming."""

    def __init__(self, provider: str, name: str | None = None, *, base_url: str | None = None,
                 api_key: str | None = None):
        import openai

        spec = PROVIDERS[provider]
        self.provider = spec
        self.provider_name = spec.name
        self.name = name or spec.default_model
        base_url = base_url or os.environ.get("PBR_AGENT_BASE_URL") or spec.base_url
        api_key = api_key or os.environ.get("PBR_AGENT_API_KEY") or next(
            (os.environ[v] for v in spec.key_variables if os.environ.get(v)), None)

        self.problem = None
        if base_url is None:
            self.problem = "no API address: set PBR_AGENT_BASE_URL (e.g. http://localhost:1234/v1 for LM Studio)"
        elif spec.needs_key and not api_key:
            self.problem = f"no {spec.name} API key: {spec.key_help} where the app is started, then restart it"

        # Local servers ignore the key, but the SDK wants one.
        self.client = openai.AsyncOpenAI(base_url=base_url, api_key=api_key or "unused")
        self.system = SCENE_INSTRUCTIONS + IN_APP_INSTRUCTIONS
        self.tools = [{"type": "function", "function": {"name": t["name"], "description": t["description"],
                                                        "parameters": compatible_schema(t["input_schema"])}}
                      for t in tool_definitions()]

    async def respond(self, messages: list, *, on_text: TextCallback, on_progress: TextCallback) -> ModelReply:
        stream = await self.client.chat.completions.create(
            model=self.name,
            messages=to_chat_messages(self.system, messages),
            tools=self.tools,
            stream=True,
        )

        text: list[str] = []
        calls: dict[int, dict] = {}
        finish_reason = None
        async for chunk in stream:
            if not chunk.choices:
                continue
            choice = chunk.choices[0]
            finish_reason = choice.finish_reason or finish_reason
            delta = choice.delta
            if delta is None:
                continue
            if delta.content:
                text.append(delta.content)
                await on_text(delta.content)
            for position, part in enumerate(delta.tool_calls or []):
                index = part.index if part.index is not None else position
                call = calls.setdefault(index, {"id": None, "name": "", "arguments": "", "extra": None})
                if part.id:
                    call["id"] = part.id
                if part.function and part.function.name:
                    call["name"] += part.function.name
                if part.function and part.function.arguments:
                    call["arguments"] += part.function.arguments
                extra = (part.model_extra or {}).get("extra_content")   # Gemini's thought signatures
                if extra:
                    call["extra"] = copy.deepcopy(extra)

        content: list[dict] = []
        if text:
            content.append({"type": "text", "text": "".join(text)})
        for index in sorted(calls):
            call = calls[index]
            try:
                args = json.loads(call["arguments"]) if call["arguments"].strip() else {}
            except json.JSONDecodeError:
                _log(f"{call['name']}: arguments aren't valid JSON: {call['arguments'][:200]!r}")
                args = {}
            block = {"type": "tool_use", "id": call["id"] or f"call_{uuid.uuid4().hex[:12]}",
                     "name": call["name"], "input": args if isinstance(args, dict) else {}}
            if call["extra"]:
                block["extra_content"] = call["extra"]
            content.append(block)

        if calls:
            stop_reason = "max_tokens" if finish_reason == "length" else "tool_use"
        else:
            stop_reason = {"length": "max_tokens", "content_filter": "refusal"}.get(finish_reason, "end_turn")
        return ModelReply(content, stop_reason)
