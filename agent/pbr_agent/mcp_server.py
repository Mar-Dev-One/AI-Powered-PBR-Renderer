"""MCP server: lets Claude Desktop, Claude Code -- any MCP client -- build
scenes in the running PBR Renderer.

It speaks MCP over stdio and turns each tool call (pbr_agent.tools) into a
command on the app's command server. The app doesn't need to be running when
the client starts this server: it connects on the first tool call, and says
so clearly if the app isn't there.

    pbr-mcp                        # installed console script
    python -m pbr_agent.mcp_server [--discovery PATH]

Registering it: see agent/README.md.
"""

from __future__ import annotations

import argparse
import json
import sys

import anyio
import mcp_types as types
from mcp.server import Server
from mcp.server.stdio import stdio_server

from . import __name__ as _package
from .engine_client import EngineClient, EngineError, EngineNotRunning
from .prompt import SCENE_INSTRUCTIONS
from .tools import TOOLS, TOOLS_BY_NAME, check_against_engine

SERVER_NAME = "pbr-renderer"
SERVER_VERSION = "0.1.0"
SCENE_RESOURCE_URI = "scene://current"

# Tools that throw away work (undo still brings it back).
DESTRUCTIVE_TOOLS = {"delete_entity", "clear_scene", "load_scene"}


def _log(message: str) -> None:
    # stdout carries the MCP protocol; anything human-readable goes to stderr.
    print(f"{_package}: {message}", file=sys.stderr, flush=True)


def _mcp_tool(tool) -> types.Tool:
    return types.Tool(
        name=tool.name,
        description=tool.description,
        input_schema=tool.input_schema,
        annotations=types.ToolAnnotations(
            read_only_hint=tool.read_only,
            destructive_hint=tool.name in DESTRUCTIVE_TOOLS,
            idempotent_hint=tool.read_only,
            open_world_hint=False,
        ),
    )


def _error(message: str) -> types.CallToolResult:
    return types.CallToolResult(content=[types.TextContent(type="text", text=message)], is_error=True)


class SceneServer:
    """The MCP handlers, around one EngineClient."""

    def __init__(self, engine: EngineClient):
        self.engine = engine
        self.checked = False

    async def _check_tools_once(self) -> None:
        """On first contact with the app: log any tool the app doesn't support."""
        if self.checked:
            return
        self.checked = True
        try:
            problems = check_against_engine(await self.engine.call("commands.list"))
        except (EngineError, EngineNotRunning) as error:
            _log(f"could not check the tools against the app: {error}")
            return
        for problem in problems:
            _log(f"tool mismatch: {problem}")

    async def list_tools(self, ctx, params) -> types.ListToolsResult:
        return types.ListToolsResult(tools=[_mcp_tool(tool) for tool in TOOLS])

    async def call_tool(self, ctx, params: types.CallToolRequestParams) -> types.CallToolResult:
        tool = TOOLS_BY_NAME.get(params.name)
        if tool is None:
            return _error(f"Unknown tool '{params.name}'.")

        try:
            method, command_params = tool.to_command(params.arguments)
        except ValueError as error:
            return _error(str(error))

        try:
            result = await self.engine.call(method, command_params or None)
        except EngineError as error:
            return _error(error.message)
        except EngineNotRunning as error:
            self.checked = False   # check again once the app is back
            return _error(str(error))

        await self._check_tools_once()

        if tool.returns_image:
            caption = f"{result.get('width')}x{result.get('height')} screenshot of the current scene"
            return types.CallToolResult(content=[
                types.ImageContent(type="image", data=result["data"], mime_type="image/png"),
                types.TextContent(type="text", text=caption),
            ])

        text = json.dumps(result, separators=(",", ":"), ensure_ascii=False)
        return types.CallToolResult(content=[types.TextContent(type="text", text=text)])

    async def list_resources(self, ctx, params) -> types.ListResourcesResult:
        return types.ListResourcesResult(resources=[
            types.Resource(
                name="scene",
                title="Current scene",
                uri=SCENE_RESOURCE_URI,
                description="Every entity in the running app's scene, with its components (JSON).",
                mime_type="application/json",
            ),
        ])

    async def read_resource(self, ctx, params: types.ReadResourceRequestParams) -> types.ReadResourceResult:
        if params.uri != SCENE_RESOURCE_URI:
            raise ValueError(f"Unknown resource {params.uri}")
        try:
            scene = await self.engine.call("scene.describe")
            text = json.dumps(scene, indent=2, ensure_ascii=False)
        except (EngineError, EngineNotRunning) as error:
            text = json.dumps({"error": str(error)})
        return types.ReadResourceResult(contents=[
            types.TextResourceContents(uri=SCENE_RESOURCE_URI, mime_type="application/json", text=text),
        ])


def build_server(engine: EngineClient) -> Server:
    handlers = SceneServer(engine)
    return Server(
        SERVER_NAME,
        version=SERVER_VERSION,
        title="PBR Renderer scene builder",
        instructions=SCENE_INSTRUCTIONS,
        on_list_tools=handlers.list_tools,
        on_call_tool=handlers.call_tool,
        on_list_resources=handlers.list_resources,
        on_read_resource=handlers.read_resource,
    )


async def serve(discovery_path: str | None = None) -> None:
    engine = EngineClient(discovery_path=discovery_path)
    server = build_server(engine)
    try:
        async with stdio_server() as (read_stream, write_stream):
            await server.run(read_stream, write_stream, server.create_initialization_options())
    finally:
        await engine.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="MCP server for the PBR Renderer's scene.")
    parser.add_argument("--discovery", help="Path to the app's server.json (default: ~/.pbr-renderer/server.json)")
    args = parser.parse_args()
    anyio.run(serve, args.discovery)


if __name__ == "__main__":
    main()
