"""Tests for the in-app chat agent (pbr_agent.agent) together with the
engine's chat session (engine/src/Chat/ChatSession.c), end to end.

A scripted model stands in for Claude, so these run without an API key: the
agent connects to ServerHost (which has the chat commands), a second client
plays the chat panel -- chat.send, chat.stop, watching chat.event -- and the
tests check what reaches the scene, the undo history and the conversation.
"""

import asyncio
import copy
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pbr_agent import EngineClient, EngineError  # noqa: E402
from pbr_agent.agent import ROLE, ChatAgent, ModelReply, serve, tool_definitions  # noqa: E402

from test_engine_client import SERVER_HOST, Host  # noqa: E402

TIMEOUT = 5


def tool_use(tool_id: str, tool: str, /, **args) -> dict:
    return {"type": "tool_use", "id": tool_id, "name": tool, "input": args}


def text(value: str) -> dict:
    return {"type": "text", "text": value}


class ScriptedModel:
    """Plays back replies in order. A reply is a list of content blocks, or
    an async function returning one (to wait, or to look at the messages)."""

    name = "scripted"

    def __init__(self, *replies, problem=None):
        self.replies = list(replies)
        self.problem = problem
        self.calls: list[list] = []   # the messages each call was given

    async def respond(self, messages, *, on_text, on_progress):
        self.calls.append(copy.deepcopy(messages))
        reply = self.replies.pop(0)
        if callable(reply):
            reply = await reply()
        for block in reply:
            if block["type"] == "text":
                await on_text(block["text"])
        stop = "tool_use" if any(b["type"] == "tool_use" for b in reply) else "end_turn"
        return ModelReply(reply, stop)


async def forever():
    await asyncio.Event().wait()


@unittest.skipUnless(SERVER_HOST.exists(), f"build ServerHost first ({SERVER_HOST})")
class ChatAgentTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.discovery = Path(self.dir.name) / "server.json"
        self.host = Host(self.discovery)

    async def asyncSetUp(self):
        # The panel's side: sends messages and watches the conversation.
        self.panel = EngineClient(discovery_path=self.discovery)
        self.events: asyncio.Queue = asyncio.Queue()
        self.panel.add_notification_listener("chat.event", self.events.put_nowait)
        await self.panel.connect()
        self.agents: list[ChatAgent] = []

    async def asyncTearDown(self):
        for agent in self.agents:
            agent.cancel_turn()
            await agent.wait_idle()
            await agent.engine.close()
        await self.panel.close()

    def tearDown(self):
        self.host.stop()
        self.dir.cleanup()

    async def start_agent(self, model) -> ChatAgent:
        engine = EngineClient(discovery_path=self.discovery, hello={"client": "test-agent", "role": ROLE})
        agent = ChatAgent(engine, model)
        await engine.connect()
        await agent.announce()
        self.agents.append(agent)
        return agent

    async def until(self, event_type: str) -> list[dict]:
        """Chat events up to and including the first of `event_type`."""
        seen = []
        while True:
            event = await asyncio.wait_for(self.events.get(), TIMEOUT)
            seen.append(event)
            if event["type"] == event_type:
                return seen

    async def test_a_turn_builds_the_scene_as_one_undo_step(self):
        model = ScriptedModel(
            [text("Adding a box and a lamp."),
             tool_use("t1", "place_model", model="builtin:cube", name="box", size=0.5),
             tool_use("t2", "add_light", type="point", name="lamp", position=[0, 2, 0], intensity=10)],
            [text("Done: a box with a lamp above it.")],
        )
        await self.start_agent(model)

        result = await self.panel.call("chat.send", {"text": "a box with a lamp above it"})
        events = await self.until("done")
        turn = result["turn"]

        self.assertEqual([e["type"] for e in events],
                         ["user", "text", "tool_call", "tool_result", "tool_call", "tool_result", "text", "done"])
        self.assertTrue(all(e["turn"] == turn for e in events))
        self.assertTrue(all(e["ok"] for e in events if e["type"] == "tool_result"))

        # The scene changed, and the model saw the tools' results.
        scene = await self.panel.call("scene.describe", {"summary": True})
        names = {e["name"] for e in scene["entities"]}
        self.assertLessEqual({"box", "lamp"}, names)
        results = model.calls[1][-1]["content"]
        self.assertEqual([r["tool_use_id"] for r in results], ["t1", "t2"])
        self.assertNotIn("is_error", results[0])

        # The panel's view of the conversation.
        history = await self.panel.call("chat.history")
        self.assertEqual(history["active_turn"], 0)
        self.assertTrue(history["can_undo_turn"])
        kinds = [(i["kind"], i.get("state")) for i in history["items"]]
        self.assertEqual(kinds, [("user", None), ("assistant", None), ("tool", "ok"), ("tool", "ok"),
                                 ("assistant", None)])
        self.assertEqual(history["items"][-1]["text"], "Done: a box with a lamp above it.")

        # One undo step for the whole turn; one undo removes both.
        steps = (await self.panel.call("history.list"))["steps"]
        self.assertEqual([s["label"] for s in steps], ["chat: a box with a lamp above it"])
        await self.panel.call("history.undo")
        scene = await self.panel.call("scene.describe", {"summary": True})
        self.assertFalse({"box", "lamp"} & {e["name"] for e in scene["entities"]})

    async def test_a_failing_tool_is_reported_to_the_model_and_the_panel(self):
        model = ScriptedModel(
            [tool_use("t1", "get_entity", entity="sofa")],
            [text("There is no sofa.")],
        )
        await self.start_agent(model)

        await self.panel.call("chat.send", {"text": "what color is the sofa?"})
        events = await self.until("done")

        result = next(e for e in events if e["type"] == "tool_result")
        self.assertFalse(result["ok"])
        self.assertIn("no entity named", result["result"])
        self.assertTrue(model.calls[1][-1]["content"][0]["is_error"])
        # Nothing changed, so there is nothing for "Undo turn" to undo.
        self.assertFalse((await self.panel.call("chat.history"))["can_undo_turn"])

    async def test_undo_turn_reverts_the_turn_and_tells_the_agent(self):
        model = ScriptedModel(
            [tool_use("t1", "place_model", model="builtin:sphere", name="ball")],
            [text("Placed a ball.")],
            [text("Noted.")],
        )
        await self.start_agent(model)
        await self.panel.call("chat.send", {"text": "a ball"})
        await self.until("done")

        await self.panel.call("chat.undo_turn")   # the panel's "Undo turn"
        await self.until("undone")
        scene = await self.panel.call("scene.describe", {"summary": True})
        self.assertNotIn("ball", {e["name"] for e in scene["entities"]})
        self.assertFalse((await self.panel.call("chat.history"))["can_undo_turn"])
        with self.assertRaises(EngineError):
            await self.panel.call("chat.undo_turn")   # only once

        # The agent hears about it with the next message.
        await self.panel.call("chat.send", {"text": "thanks"})
        await self.until("done")
        note, message = model.calls[2][-1]["content"]
        self.assertIn("Undo turn", note["text"])
        self.assertEqual(message["text"], "thanks")

    async def test_a_plain_undo_also_retires_undo_turn(self):
        model = ScriptedModel(
            [tool_use("t1", "place_model", model="builtin:sphere", name="ball")],
            [text("Placed a ball.")],
        )
        await self.start_agent(model)
        await self.panel.call("chat.send", {"text": "a ball"})
        await self.until("done")

        await self.panel.call("history.undo")      # e.g. the console's Undo
        self.assertFalse((await self.panel.call("chat.history"))["can_undo_turn"])

    async def test_stop_ends_the_turn_and_keeps_the_conversation_valid(self):
        model = ScriptedModel(
            [tool_use("t1", "place_model", model="builtin:cube", name="crate")],
            forever,                      # still "thinking" when Stop is pressed
            [text("Okay, only one crate.")],
        )
        agent = await self.start_agent(model)

        await self.panel.call("chat.send", {"text": "build a pile of crates"})
        await self.until("tool_result")
        await asyncio.sleep(0.1)          # the agent is now waiting on the model
        self.assertTrue((await self.panel.call("chat.stop"))["stopped"])
        await self.until("stopped")
        await agent.wait_idle()

        # The turn is over and its change is kept, as one undoable step.
        history = await self.panel.call("chat.history")
        self.assertEqual(history["active_turn"], 0)
        self.assertTrue(history["can_undo_turn"])
        self.assertEqual((await self.panel.call("history.list"))["transaction_depth"], 0)

        # The next turn starts from a valid conversation that says what happened.
        await self.panel.call("chat.send", {"text": "never mind"})
        await self.until("done")
        messages = model.calls[2]
        self.assertEqual([m["role"] for m in messages], ["user", "assistant", "user", "user"])
        self.assertIn("stopped your previous turn", messages[-1]["content"][0]["text"])

    async def test_the_turn_ends_if_the_agent_disconnects(self):
        agent = await self.start_agent(ScriptedModel(
            [tool_use("t1", "place_model", model="builtin:cube", name="crate")],
            forever,
        ))
        await self.panel.call("chat.send", {"text": "crates"})
        await self.until("tool_result")

        agent.cancel_turn()
        await agent.engine.close()        # as if the agent crashed
        await self.until("agent_lost")

        history = await self.panel.call("history.list")
        self.assertEqual(history["transaction_depth"], 0)   # undo isn't left blocked
        self.assertEqual([s["label"] for s in history["steps"]], ["chat: crates"])

        with self.assertRaises(EngineError) as caught:
            await self.panel.call("chat.send", {"text": "anyone?"})
        self.assertIn("no chat agent", caught.exception.message)

    async def test_one_turn_at_a_time_and_one_agent_at_a_time(self):
        await self.start_agent(ScriptedModel(forever))
        await self.panel.call("chat.send", {"text": "think hard"})

        with self.assertRaises(EngineError) as caught:
            await self.panel.call("chat.send", {"text": "and this too"})
        self.assertIn("still working", caught.exception.message)

        second = EngineClient(discovery_path=self.discovery, hello={"client": "other", "role": ROLE})
        with self.assertRaises(EngineError) as caught:
            await second.connect()
        self.assertEqual(caught.exception.command_code, "conflict")
        self.assertFalse(second.connected)

    async def test_an_agent_that_cant_work_says_why(self):
        await self.start_agent(ScriptedModel(problem="no Claude API credentials"))
        await self.panel.call("chat.send", {"text": "hello"})
        events = await self.until("error")
        self.assertIn("no Claude API credentials", events[-1]["message"])
        self.assertEqual((await self.panel.call("chat.history"))["active_turn"], 0)

    async def test_serve_exits_with_the_app(self):
        model = ScriptedModel([text("hi")])
        task = asyncio.create_task(serve(model, discovery_path=str(self.discovery), exit_with_app=True,
                                         retry_seconds=0.1))
        for _ in range(100):
            if (await self.panel.call("chat.history"))["agent_online"]:
                break
            await asyncio.sleep(0.05)
        await self.panel.call("chat.send", {"text": "hello"})
        await self.until("done")

        self.host.stop()
        self.assertEqual(await asyncio.wait_for(task, TIMEOUT), 0)


class ToolDefinitionTests(unittest.TestCase):
    def test_undo_and_redo_are_left_out(self):
        # The app's turn transaction would refuse them.
        names = [t["name"] for t in tool_definitions()]
        self.assertNotIn("undo", names)
        self.assertNotIn("redo", names)
        self.assertIn("place_model", names)


if __name__ == "__main__":
    unittest.main()
