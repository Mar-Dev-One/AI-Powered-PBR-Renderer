# pbr_agent

Python side of the PBR Renderer's AI scene builder (see `docs/ROADMAP.md`): talk to the running
app, and expose its commands to AI tools.

- `pbr_agent.EngineClient`: async client for the app's command server (a localhost WebSocket
  speaking JSON-RPC 2.0). It finds the app through `~/.pbr-renderer/server.json`, which the app
  writes at startup, and reconnects if the app is restarted.
- `pbr_agent.tools`: the tools an AI gets (24: look, build, change, view, files), defined once
  with JSON Schemas and mapped onto engine commands.
- `pbr_agent.prompt`: the conventions and working method an AI should follow in the scene.
- `pbr_agent.mcp_server` (`pbr-mcp`): an MCP server over stdio exposing those tools, so Claude
  Desktop, Claude Code or any MCP client can build scenes in the running app. Screenshots come back
  as images, and the live scene is the resource `scene://current`.

## Setup

```sh
python -m venv agent/.venv
agent/.venv/Scripts/python -m pip install -e agent        # Windows
agent/.venv/bin/python -m pip install -e agent            # macOS / Linux
```

## Use from Claude

Start the app (the Testbed) first, or at any time; the MCP server connects on the first tool
call. Both setups below run the console script `pbr-mcp` from the virtual environment. The path
shown is this repository on Windows; adjust it to yours (`agent/.venv/bin/pbr-mcp` on macOS and
Linux).

**Claude Code**:

```sh
claude mcp add pbr-renderer -- C:\Dev\My\PBR-Renderer\agent\.venv\Scripts\pbr-mcp.exe
```

**Claude Desktop**: add the server to `claude_desktop_config.json` (Settings -> Developer -> Edit
Config; on Windows it is `%APPDATA%\Claude\claude_desktop_config.json`), then restart Claude
Desktop:

```json
{
  "mcpServers": {
    "pbr-renderer": {
      "command": "C:\\Dev\\My\\PBR-Renderer\\agent\\.venv\\Scripts\\pbr-mcp.exe"
    }
  }
}
```

Then ask for a scene, e.g. "put a table in the middle with a lamp on it, light it warmly and
show me". The claude.ai website only reaches remote MCP servers over HTTPS, so it can't use this
local server directly.

## Use from Python

```python
import asyncio
from pbr_agent import EngineClient

async def main():
    async with EngineClient() as engine:
        engine.add_change_listener(lambda event: print(event["source"]))
        await engine.call("place_model", {"model": "builtin:sphere", "size": 0.3, "on_ground": True})
        print(await engine.call("scene.describe", {"summary": True}))

asyncio.run(main())
```

`call()` raises `EngineError` when a command is rejected. Its `message` says what to fix, and
`command_code` is the engine's code (`not_found`, `invalid_value`, ...). It raises
`EngineNotRunning` when the app can't be reached.

## Tests

The tests run the headless `ServerHost` built with the engine (`build/tests/...`; set
`PBR_SERVER_HOST` to use another path):

```sh
agent/.venv/Scripts/python -m unittest discover -s agent/tests
```

They also run under `ctest` when `agent/.venv` exists.
