"""End-to-end test of the MCP server: a real MCP client (the SDK's) starts
`python -m pbr_agent.mcp_server` over stdio, exactly as Claude Desktop or
Claude Code would, and drives a headless ServerHost through it.
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

# The server runs as a child process and inherits this environment: keep the
# developer's own LangSmith settings from uploading the tests' tool calls.
os.environ["LANGSMITH_TRACING"] = "false"
os.environ["LANGSMITH_TRACING_V2"] = "false"
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import mcp_types as types  # noqa: E402
from mcp import ClientSession  # noqa: E402
from mcp.client.stdio import StdioServerParameters, stdio_client  # noqa: E402

from pbr_agent.tools import TOOLS  # noqa: E402
from test_engine_client import SERVER_HOST, Host  # noqa: E402

AGENT_DIR = Path(__file__).resolve().parents[1]


def _text(result) -> str:
    return "".join(c.text for c in result.content if isinstance(c, types.TextContent))


class McpServerTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.discovery = Path(self.dir.name) / "server.json"
        self.host = None

    def tearDown(self):
        if self.host:
            self.host.stop()
        self.dir.cleanup()

    def _params(self) -> StdioServerParameters:
        return StdioServerParameters(
            command=sys.executable,
            args=["-m", "pbr_agent.mcp_server", "--discovery", str(self.discovery)],
            cwd=str(AGENT_DIR),
        )

    async def test_without_the_app(self):
        # The server starts and lists its tools without the app; calls say why they can't run.
        async with stdio_client(self._params()) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                tools = (await session.list_tools()).tools
                self.assertEqual(len(tools), len(TOOLS))

                result = await session.call_tool("describe_scene", {})
                self.assertTrue(result.is_error)
                self.assertIn("isn't running", _text(result))

    @unittest.skipUnless(SERVER_HOST.exists(), f"build ServerHost first ({SERVER_HOST})")
    async def test_build_a_scene_through_mcp(self):
        self.host = Host(self.discovery, scenes_dir=Path(self.dir.name) / "scenes")

        async with stdio_client(self._params()) as (read, write):
            async with ClientSession(read, write) as session:
                init = await session.initialize()
                self.assertIn("meters", init.instructions or "")
                self.assertEqual(init.server_info.name, "pbr-renderer")

                # Tools carry their schemas and hints.
                tools = {t.name: t for t in (await session.list_tools()).tools}
                self.assertIn("place_model", tools)
                self.assertIn("relative_to", tools["place_model"].input_schema["properties"])
                self.assertTrue(tools["describe_scene"].annotations.read_only_hint)
                self.assertTrue(tools["clear_scene"].annotations.destructive_hint)

                # Build: a table, a lamp on it, a light aimed at it.
                result = await session.call_tool("place_model", {"model": "builtin:cube", "name": "table", "size": 1.0})
                self.assertFalse(result.is_error, _text(result))
                self.assertEqual(json.loads(_text(result))["name"], "table")

                result = await session.call_tool("place_model", {
                    "model": "builtin:sphere", "name": "lamp", "size": 0.3,
                    "relative_to": {"entity": "table", "side": "on_top"}})
                lamp = json.loads(_text(result))
                self.assertGreater(lamp["bounds"]["min"][1], 0.99)   # resting on the 1 m cube

                result = await session.call_tool("add_light", {"type": "spot", "name": "key", "position": [0, 3, 2],
                                                               "aim_at": [0, 1, 0], "intensity": 20})
                self.assertFalse(result.is_error, _text(result))

                # Engine errors come back as tool errors the model can read.
                result = await session.call_tool("get_entity", {"entity": "sofa"})
                self.assertTrue(result.is_error)
                self.assertIn("no entity named \"sofa\"", _text(result))

                result = await session.call_tool("no_such_tool", {})
                self.assertTrue(result.is_error)

                # App-only tools (not in the headless host) fail cleanly too.
                result = await session.call_tool("screenshot", {})
                self.assertTrue(result.is_error)

                # The scene resource.
                resources = (await session.list_resources()).resources
                self.assertEqual([r.uri for r in resources], ["scene://current"])
                contents = (await session.read_resource("scene://current")).contents
                scene = json.loads(contents[0].text)
                self.assertEqual(sorted(e["name"] for e in scene["entities"]), ["key", "lamp", "table"])

                # Undo through MCP.
                result = await session.call_tool("undo", {})
                self.assertFalse(result.is_error, _text(result))
                # Undid the light. History labels name the engine command
                # add_light is built on, not the tool.
                self.assertEqual(json.loads(_text(result))["undone"], "entity.create key")


if __name__ == "__main__":
    unittest.main()
