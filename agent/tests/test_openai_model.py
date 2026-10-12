"""Tests for the OpenAI-compatible model (pbr_agent.openai_model): the schema
and conversation translation, and reading a streamed reply. No network: the
client is replaced by one that plays back chunks."""

import asyncio
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pbr_agent.agent import tool_definitions  # noqa: E402
from pbr_agent.openai_model import OpenAICompatibleModel, compatible_schema, to_chat_messages  # noqa: E402


def _walk(schema):
    if isinstance(schema, dict):
        yield schema
        for value in schema.values():
            yield from _walk(value)
    elif isinstance(schema, list):
        for value in schema:
            yield from _walk(value)


class SchemaTests(unittest.TestCase):
    def test_tool_schemas_lose_what_gemini_rejects(self):
        for tool in tool_definitions():
            for node in _walk(compatible_schema(tool["input_schema"])):
                self.assertNotIn("additionalProperties", node)
                self.assertNotIn("exclusiveMinimum", node)
                if "type" in node and not isinstance(node.get("type"), dict):
                    self.assertNotIsInstance(node["type"], list)

    def test_a_nullable_union_becomes_nullable(self):
        self.assertEqual(compatible_schema({"type": ["string", "null"]}), {"type": "string", "nullable": True})


def _screenshot_turn(tool_id):
    return [
        {"role": "assistant", "content": [{"type": "tool_use", "id": tool_id, "name": "screenshot", "input": {}}]},
        {"role": "user", "content": [{"type": "tool_result", "tool_use_id": tool_id, "content": [
            {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": f"PNG-{tool_id}"}},
            {"type": "text", "text": "256x256 screenshot"}]}]},
    ]


class ConversationTests(unittest.TestCase):
    def test_tool_calls_and_results(self):
        messages = [
            {"role": "user", "content": [{"type": "text", "text": "a red cube"}]},
            {"role": "assistant", "content": [
                {"type": "text", "text": "Adding it."},
                {"type": "tool_use", "id": "c1", "name": "create_entity", "input": {"name": "Cube"},
                 "extra_content": {"google": {"thought_signature": "sig"}}}]},
            {"role": "user", "content": [
                {"type": "tool_result", "tool_use_id": "c1", "content": "nope", "is_error": True}]},
        ]
        chat = to_chat_messages("SYSTEM", messages)
        self.assertEqual(chat[0], {"role": "system", "content": "SYSTEM"})
        self.assertEqual(chat[1], {"role": "user", "content": "a red cube"})
        self.assertEqual(chat[2]["content"], "Adding it.")
        call = chat[2]["tool_calls"][0]
        self.assertEqual((call["id"], call["function"]["name"], call["function"]["arguments"]),
                         ("c1", "create_entity", '{"name": "Cube"}'))
        self.assertEqual(call["extra_content"], {"google": {"thought_signature": "sig"}})
        self.assertEqual(chat[3], {"role": "tool", "tool_call_id": "c1", "content": "Error: nope"})

    def test_screenshots_follow_the_tool_message_and_old_ones_are_dropped(self):
        messages = _screenshot_turn("a") + _screenshot_turn("b") + _screenshot_turn("c")
        chat = to_chat_messages("S", messages, keep_screenshots=2)
        images = [p["image_url"]["url"] for m in chat if isinstance(m["content"], list)
                  for p in m["content"] if p["type"] == "image_url"]
        self.assertEqual(images, ["data:image/png;base64,PNG-b", "data:image/png;base64,PNG-c"])
        self.assertEqual(chat[2], {"role": "tool", "tool_call_id": "a", "content": "256x256 screenshot"})
        self.assertEqual(chat[3]["role"], "user")


def _chunk(content=None, tool_calls=None, finish=None):
    return NS(choices=[NS(finish_reason=finish, delta=NS(content=content, tool_calls=tool_calls))])


def _call_part(index, id=None, name=None, arguments=None, extra=None):
    return NS(index=index, id=id, function=NS(name=name, arguments=arguments),
              model_extra={"extra_content": extra} if extra else {})


class StreamTests(unittest.IsolatedAsyncioTestCase):
    async def respond(self, chunks):
        model = OpenAICompatibleModel("ollama")

        async def stream():
            for chunk in chunks:
                yield chunk

        async def create(**kwargs):
            self.request = kwargs
            return stream()

        model.client = NS(chat=NS(completions=NS(create=create)))
        texts = []

        async def on_text(t):
            texts.append(t)

        async def on_progress(_):
            pass

        reply = await model.respond([{"role": "user", "content": "hi"}], on_text=on_text, on_progress=on_progress)
        return reply, texts

    async def test_text_reply(self):
        reply, texts = await self.respond([_chunk("Hel"), _chunk("lo"), _chunk(finish="stop")])
        self.assertEqual(texts, ["Hel", "lo"])
        self.assertEqual(reply.content, [{"type": "text", "text": "Hello"}])
        self.assertEqual(reply.stop_reason, "end_turn")
        self.assertTrue(self.request["stream"])

    async def test_streamed_tool_calls_are_assembled(self):
        reply, _ = await self.respond([
            _chunk(tool_calls=[_call_part(0, id="t1", name="list_entities", arguments="")]),
            _chunk(tool_calls=[_call_part(0, arguments="{}")]),
            _chunk(tool_calls=[_call_part(1, id="t2", name="add_light", arguments='{"type": ',
                                          extra={"google": {"thought_signature": "s"}})]),
            _chunk(tool_calls=[_call_part(1, arguments='"point"}')]),
            _chunk(finish="stop"),   # Gemini says "stop" even after tool calls
        ])
        self.assertEqual(reply.stop_reason, "tool_use")
        self.assertEqual(reply.content, [
            {"type": "tool_use", "id": "t1", "name": "list_entities", "input": {}},
            {"type": "tool_use", "id": "t2", "name": "add_light", "input": {"type": "point"},
             "extra_content": {"google": {"thought_signature": "s"}}},
        ])

    async def test_a_missing_key_is_a_problem(self):
        import os
        saved = {v: os.environ.pop(v, None) for v in ("GEMINI_API_KEY", "GOOGLE_API_KEY", "PBR_AGENT_API_KEY")}
        try:
            self.assertIn("GEMINI_API_KEY", OpenAICompatibleModel("gemini").problem)
            self.assertIsNone(OpenAICompatibleModel("gemini", api_key="k").problem)
        finally:
            os.environ.update({k: v for k, v in saved.items() if v is not None})


if __name__ == "__main__":
    unittest.main()
