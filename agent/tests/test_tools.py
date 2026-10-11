"""Tests for pbr_agent.tools: every tool is well-formed, lines up with the
engine's command table, and its example actually works against the engine.

Engine-core tools run against ServerHost (headless). App tools (camera,
environment, screenshot) exist only in the Testbed; they are checked live
when the Testbed is running and skipped otherwise.
"""

import base64
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pbr_agent import EngineClient, EngineNotRunning, read_discovery  # noqa: E402
from pbr_agent.tools import TOOLS, TOOLS_BY_NAME, check_against_engine  # noqa: E402

from test_engine_client import SERVER_HOST, Host  # noqa: E402

# Order in which the examples are run: each relies on what earlier ones built.
SCENARIO = [
    "describe_scene", "list_assets", "describe_components",
    "place_model", "get_entity", "add_light", "create_entity", "run_batch", "move_entity",
    "set_component", "remove_component", "update_entity", "delete_entity",
    "undo", "redo",
    "save_scene", "list_scenes", "load_scene",
    "clear_scene",
]

NAME_PATTERN = "^[a-zA-Z0-9_-]{1,64}$"


class ToolDefinitionTests(unittest.TestCase):
    def test_tools_are_well_formed(self):
        import re

        self.assertEqual(len(TOOLS), len(TOOLS_BY_NAME), "tool names are unique")
        for tool in TOOLS:
            with self.subTest(tool=tool.name):
                self.assertRegex(tool.name, NAME_PATTERN)
                self.assertGreater(len(tool.description), 20)
                schema = tool.input_schema
                self.assertEqual(schema["type"], "object")
                self.assertFalse(schema.get("additionalProperties", True), "unknown arguments are refused")
                for required in schema.get("required", []):
                    self.assertIn(required, schema["properties"])
                for name, prop in schema["properties"].items():
                    self.assertTrue(re.match(NAME_PATTERN, name))
                    self.assertTrue("type" in prop or "enum" in prop, f"{name} has a type")

    def test_every_core_tool_is_in_the_scenario(self):
        core = {t.name for t in TOOLS if not t.app_command}
        self.assertEqual(core, set(SCENARIO), "add new tools to SCENARIO so their examples run")

    def test_add_light_builds_entity_create(self):
        method, params = TOOLS_BY_NAME["add_light"].to_command(
            {"type": "spot", "position": [0, 2, 0], "aim_at": [0, 0, 0], "intensity": 10, "name": "s"})
        self.assertEqual(method, "entity.create")
        self.assertEqual(params["name"], "s")
        self.assertEqual(params["components"]["Light"]["direction"], [0, -2, 0])
        self.assertEqual(params["components"]["Transform"]["position"], [0, 2, 0])

        with self.assertRaises(ValueError):
            TOOLS_BY_NAME["add_light"].to_command({"type": "spot", "aim_at": [0, 0, 0]})   # aim_at needs position

    def test_null_parent_is_kept(self):
        _, params = TOOLS_BY_NAME["update_entity"].to_command({"entity": "a", "parent": None, "name": None})
        self.assertEqual(params, {"entity": "a", "parent": None})   # null parent = root; null name = not given


@unittest.skipUnless(SERVER_HOST.exists(), f"build ServerHost first ({SERVER_HOST})")
class ToolsAgainstEngineTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.discovery = Path(self.dir.name) / "server.json"
        self.host = Host(self.discovery, scenes_dir=Path(self.dir.name) / "scenes")

    def tearDown(self):
        self.host.stop()
        self.dir.cleanup()

    async def test_tools_match_the_command_table(self):
        async with EngineClient(discovery_path=self.discovery) as engine:
            problems = check_against_engine(await engine.call("commands.list"), include_app_commands=False)
        self.assertEqual(problems, [])

    async def test_every_example_runs(self):
        async with EngineClient(discovery_path=self.discovery) as engine:
            for name in SCENARIO:
                tool = TOOLS_BY_NAME[name]
                with self.subTest(tool=name):
                    method, params = tool.to_command(tool.example)
                    result = await engine.call(method, params or None)
                    self.assertIsNotNone(result)

            # The scenario did what it says: the batch-made lamp base ended up on the table.
            # (clear_scene ran last, so reload the saved copy to look.)
            await engine.call("scene.load", {"name": "tool_test"})
            base = await engine.call("entity.get", {"entity": "desk_lamp_base"})
            table = await engine.call("entity.get", {"entity": "table"})
            self.assertGreater(base["components"]["Transform"]["position"][1],
                               table["components"]["Transform"]["position"][1])


def _live_app_url():
    try:
        return read_discovery()["url"]
    except EngineNotRunning:
        return None


@unittest.skipUnless(_live_app_url(), "start the Testbed to check the app's tools live")
class AppToolsLiveTests(unittest.IsolatedAsyncioTestCase):
    async def test_app_tools(self):
        async with EngineClient() as engine:
            self.assertEqual(check_against_engine(await engine.call("commands.list")), [])
            for tool in TOOLS:
                if not tool.app_command:
                    continue
                with self.subTest(tool=tool.name):
                    method, params = tool.to_command(tool.example)
                    result = await engine.call(method, params or None)
                    if tool.returns_image:
                        png = base64.b64decode(result["data"])
                        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")


if __name__ == "__main__":
    unittest.main()
