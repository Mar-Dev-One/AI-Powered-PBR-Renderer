"""Tests for the agent's LangSmith tracing (pbr_agent.tracing).

Traces are recorded in memory (tracing.local()), so nothing is uploaded and
no LangSmith account is needed. A stand-in engine answers the tools -- a
screenshot included, which the headless ServerHost can't take -- and the tests
look at the trace the turn left behind (ChatAgent.last_trace).
"""

import base64
import os
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

# The developer's own LangSmith settings must not reach these tests: test
# turns would be uploaded to their project. Tracing tests turn it on in memory
# only (pbr_agent.tracing.local()).
os.environ["LANGSMITH_TRACING"] = "false"
os.environ["LANGSMITH_TRACING_V2"] = "false"
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pbr_agent import tracing  # noqa: E402
from pbr_agent.agent import ChatAgent, ModelReply  # noqa: E402
from pbr_agent.engine_client import EngineError  # noqa: E402

from pbr_agent.mcp_server import SceneServer  # noqa: E402
import mcp_types as types  # noqa: E402

from test_agent import ScriptedModel, text, tool_use  # noqa: E402

PNG = b"\x89PNG\r\n\x1a\n" + b"pixels" * 10


class StubEngine:
    """Just enough of EngineClient for ChatAgent: tool calls get canned answers."""

    def __init__(self):
        self.calls = []
        self.events = []

    def add_notification_listener(self, method, listener):
        pass

    async def call(self, method, params=None):
        self.calls.append((method, params))
        if method == "view.screenshot":
            return {"width": 4, "height": 2, "data": base64.b64encode(PNG).decode()}
        if method == "entity.get":
            raise EngineError(f'no entity named "{params["entity"]}"', -32000, "not_found")
        return {"ok": True, "method": method}

    async def notify(self, method, params=None):
        self.events.append(params)


class UsageModel(ScriptedModel):
    """A scripted model that reports token usage, like the Claude model does."""

    provider_name = "anthropic"

    async def respond(self, messages, *, on_text, on_progress):
        reply = await super().respond(messages, on_text=on_text, on_progress=on_progress)
        return ModelReply(reply.content, reply.stop_reason,
                          {"input_tokens": 120, "output_tokens": 30, "total_tokens": 150})


def children(run, run_type=None):
    return [c for c in run.child_runs if run_type is None or c.run_type == run_type]


@unittest.skipUnless(tracing.available(), "langsmith isn't installed (pip install -e agent[tracing])")
class TracingTests(unittest.IsolatedAsyncioTestCase):
    async def run_turn(self, model, request="look at the scene"):
        agent = ChatAgent(StubEngine(), model)
        with tracing.local():
            await agent._run_turn(1, request, None)
        return agent, agent.last_trace.run

    async def test_a_turn_is_one_trace_of_model_and_tool_calls(self):
        model = UsageModel(
            [text("Looking."), tool_use("t1", "screenshot")],
            [text("It's a cube on a plane.")],
        )
        agent, turn = await self.run_turn(model)

        self.assertIsNotNone(turn)
        self.assertEqual(turn.name, "chat turn")
        self.assertEqual(turn.inputs["text"], "look at the scene")
        self.assertEqual(turn.outputs["reply"], "Looking.It's a cube on a plane.")
        self.assertEqual(turn.metadata["thread_id"], agent.thread_id)
        self.assertIsNone(turn.error)

        # In order: model, tool, model.
        self.assertEqual([c.run_type for c in turn.child_runs], ["llm", "tool", "llm"])
        first, shot, second = turn.child_runs

        self.assertEqual(first.name, "scripted")
        self.assertEqual(first.metadata["ls_provider"], "anthropic")
        self.assertEqual(first.metadata["usage_metadata"]["total_tokens"], 150)
        self.assertEqual(first.outputs["stop_reason"], "tool_use")
        self.assertEqual(first.inputs["messages"][0]["content"][-1]["text"], "look at the scene")

        # The screenshot is a PNG attached to its tool run...
        self.assertEqual(shot.name, "screenshot")
        self.assertEqual(shot.attachments["screenshot"], ("image/png", PNG))
        self.assertIn("4x2 screenshot", shot.outputs["output"])

        # ...and only a placeholder in the next model call's inputs.
        result = second.inputs["messages"][-1]["content"][0]
        self.assertEqual(result["type"], "tool_result")
        self.assertEqual(result["content"][0], {"type": "text", "text": tracing.SCREENSHOT_PLACEHOLDER})
        self.assertNotIn(base64.b64encode(PNG).decode(), repr(second.inputs))

    async def test_a_failing_tool_is_a_failed_tool_run(self):
        model = ScriptedModel(
            [tool_use("t1", "get_entity", entity="sofa")],
            [text("There is no sofa.")],
        )
        _, turn = await self.run_turn(model, "what color is the sofa?")

        tool = children(turn, "tool")[0]
        self.assertEqual(tool.name, "get_entity")
        self.assertEqual(tool.inputs["input"], {"entity": "sofa"})
        self.assertIn("no entity named", tool.error)
        self.assertIsNone(turn.error)   # the turn itself went fine: the model dealt with it

    async def test_a_failed_turn_is_a_failed_trace(self):
        model = ScriptedModel(problem="no credentials")
        _, turn = await self.run_turn(model)
        self.assertIn("no credentials", turn.error)

    async def test_new_chat_starts_a_new_thread(self):
        agent = ChatAgent(StubEngine(), ScriptedModel())
        first = agent.thread_id
        agent._on_reset({})
        self.assertNotEqual(agent.thread_id, first)

    async def test_mcp_tool_calls_are_traced_with_the_client(self):
        server = SceneServer(StubEngine())
        server.checked = True   # skip the commands.list check against the stub

        class Ctx:   # what the low-level MCP server hands a handler: the client from the handshake
            class session:
                class client_params:
                    client_info = types.Implementation(name="claude-ai", version="1.2.3")

        with tracing.local():
            shot = await server.call_tool(Ctx, types.CallToolRequestParams(name="screenshot", arguments={}))
            shot_run = server.last_trace.run
            failed = await server.call_tool(Ctx, types.CallToolRequestParams(name="get_entity",
                                                                             arguments={"entity": "sofa"}))
            failed_run = server.last_trace.run

        self.assertFalse(shot.is_error)
        self.assertEqual(shot_run.name, "screenshot")
        self.assertEqual(shot_run.run_type, "tool")
        self.assertEqual(shot_run.session_name, "pbr-mcp")
        self.assertEqual(shot_run.metadata["client"], "claude-ai")
        self.assertEqual(shot_run.metadata["thread_id"], server.session_id)
        self.assertEqual(shot_run.attachments["screenshot"], ("image/png", PNG))
        self.assertIn("4x2 screenshot", shot_run.outputs["output"])

        self.assertTrue(failed.is_error)
        self.assertEqual(failed_run.inputs["input"], {"entity": "sofa"})
        self.assertIn("no entity named", failed_run.error)
        self.assertEqual(failed_run.metadata["thread_id"], server.session_id)   # one session, one thread

    async def test_off_means_nothing_is_recorded(self):
        agent = ChatAgent(StubEngine(), ScriptedModel([text("Hi.")]))
        await agent._run_turn(1, "hello", None)   # no tracing.local(), and LANGSMITH_TRACING unset
        self.assertIsNone(agent.last_trace.run)


if __name__ == "__main__":
    unittest.main()
