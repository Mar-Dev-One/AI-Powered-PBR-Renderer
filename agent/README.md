# pbr_agent

Python side of the PBR Renderer's AI scene builder (see `docs/ROADMAP.md`): talk to the running
app, and expose its commands to AI tools.

- `pbr_agent.EngineClient`: async client for the app's command server (a localhost WebSocket
  speaking JSON-RPC 2.0). It finds the app through `~/.pbr-renderer/server.json`, which the app
  writes at startup, and reconnects if the app is restarted.
- `pbr_agent.tools`: the tools an AI gets (36: look, build, change, view, files, project, input), defined once
  with JSON Schemas and mapped onto engine commands.
- `pbr_agent.prompt`: the conventions and working method an AI should follow in the scene.
- `pbr_agent.mcp_server` (`pbr-mcp`): an MCP server over stdio exposing those tools, so Claude
  Desktop, Claude Code or any MCP client can build scenes in the running app. Screenshots come back
  as images, and the live scene is the resource `scene://current`.
- `pbr_agent.agent` (`pbr-agent`): the app's in-app chat agent, a Claude API tool-use loop over
  the same tools. The app starts it; see below.

## Setup

```sh
python -m venv agent/.venv
agent/.venv/Scripts/python -m pip install -e agent        # Windows
agent/.venv/bin/python -m pip install -e agent            # macOS / Linux
```

## Use from Claude

Start the app (the Editor) first, or at any time; the MCP server connects on the first tool
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

### Seeing Claude's tool calls in LangSmith

With LangSmith tracing on (see [Tracing with LangSmith](#tracing-with-langsmith) for the install),
every tool call Claude makes through `pbr-mcp` becomes a trace in the LangSmith project
**pbr-mcp**: the tool, its arguments, the engine's result, screenshots as images, and failed calls
marked as errors. Each trace carries the client's name (`claude-ai` for Claude Desktop) and a
`thread_id` shared by one session's calls, and the tag `mcp`. Set `PBR_MCP_LANGSMITH_PROJECT` for
another project.

Only the calls are visible: Claude's prompts, reasoning and token use stay in Claude Desktop, since
MCP sends a server nothing else. For whole traces, use the in-app chat agent.

The server reads the settings from its environment. If `LANGSMITH_TRACING` and
`LANGSMITH_API_KEY` are user environment variables, Claude Desktop passes them on once it's
restarted; or set them in its config, next to the command:

```json
{
  "mcpServers": {
    "pbr-renderer": {
      "command": "C:\\Dev\\My\\PBR-Renderer\\agent\\.venv\\Scripts\\pbr-mcp.exe",
      "env": {
        "LANGSMITH_TRACING": "true",
        "LANGSMITH_API_KEY": "lsv2_..."
      }
    }
  }
}
```

## The in-app chat agent

The editor starts `python -m pbr_agent.agent --exit-with-app` from this virtual environment at
launch. The agent connects to the app's command server, claims the role `chat_agent`
(`server.hello`), and then serves the **Chat** window:

```
app  --chat.request {turn, text}-->  agent  <-->  Claude API + tools (engine commands)
agent  --chat.event {turn, type: text | progress | tool_call | tool_result | done | error}-->  app
```

The app makes each turn one undo step, and ends the turn itself on Stop or if the agent goes away
(`engine/src/Chat/ChatSession.h`). `undo` and `redo` aren't offered to the in-app model, since
they can't run inside the turn's transaction. Its conversation stays in memory until **New chat**.

It reads Claude API credentials from its environment: `ANTHROPIC_API_KEY`, or an `ant auth login`
profile. Options (or environment variables): `--model` (`PBR_AGENT_MODEL`, default
`claude-opus-5-5`), `--effort` (`PBR_AGENT_EFFORT`, default `medium`), `--discovery`. Without
`--exit-with-app` it waits for the app and reconnects when the app restarts, which is handy when
working on the agent:

```sh
PBR_AGENT_AUTOSTART=0 build/editor/Release/Editor.exe       # the app, without its own agent
agent/.venv/Scripts/pbr-agent                               # this one instead
```

### Settings file

The model settings can also live in `~/.pbr-renderer/agent.json` (`%USERPROFILE%\.pbr-renderer`
on Windows), which the agent reads every time it starts:

```json
{
  "provider": "gemini",
  "model": "gemini-3.8-flash"
}
```

The keys are `provider`, `model`, `effort` and `base_url`. The command line wins over the
environment variables (`PBR_AGENT_PROVIDER`, `PBR_AGENT_MODEL`, `PBR_AGENT_EFFORT`,
`PBR_AGENT_BASE_URL`), which win over the file. The file is the dependable choice: the app starts
the agent with its own environment, so a variable set after VS Code (or Explorer) started doesn't
reach it until that restarts. `agent.log` names where the provider came from:
`provider gemini (from C:\Users\you\.pbr-renderer\agent.json)`. A broken file is reported in the
chat panel. API keys aren't read from the file; they stay in the environment.

### Without a Claude API key

`--provider` (`PBR_AGENT_PROVIDER`, or `"provider"` in the settings file) switches the chat to an
OpenAI-compatible API (`pbr_agent/openai_model.py`); its key is set where the app is started:

| Provider | Setup | Default model (`PBR_AGENT_MODEL`) |
|----------|-------|-----------------------------------|
| `gemini` | `GEMINI_API_KEY`, free from <https://aistudio.google.com/apikey> | `gemini-3.8-flash` |
| `ollama` | a running Ollama with a tool-calling model pulled (`ollama pull qwen3`) | `qwen3` |
| `openai` | `PBR_AGENT_BASE_URL` (LM Studio: `http://localhost:1234/v1`, OpenRouter, ...) and `PBR_AGENT_API_KEY` | `gpt-5-mini` |

These lose the Claude-only features: no progress notes between tool calls, and instead of
server-side clearing only the two newest screenshots are kept in the context. Free tiers are
rate-limited; a turn that hits the limit ends with an error, and asking again continues.

Requests use streaming, adaptive thinking with progress notes (shown in the panel between tool
calls), prompt caching, server-side clearing of old tool results (screenshots), and server-side
refusal fallback (`fallbacks: "default"`).

### Tracing with LangSmith

The agent can record every chat turn in [LangSmith](https://smith.langchain.com): one trace per
turn, holding each model call (the conversation so far, the reply, token usage) and each tool call
(arguments, the engine's result, screenshots attached as images). Turns of one conversation share a
`thread_id`, so LangSmith's Threads view shows them together; **New chat** starts a new thread. It
works the same for every provider (token counts are recorded for Claude only).

It's off unless set up. Install the extra:

```sh
agent/.venv/Scripts/python -m pip install -e "agent[tracing]"
```

On Python 3.15, where `zstandard` (a langsmith dependency) has no prebuilt package yet, install
langsmith without it instead; traces are then uploaded uncompressed:

```sh
agent/.venv/Scripts/python -m pip install --no-deps "langsmith>=0.14,<1"
agent/.venv/Scripts/python -m pip install anyio distro httpx2 orjson packaging pydantic requests requests-toolbelt sniffio typing-extensions uuid-utils xxhash
```

Then set these where the app is started, like the API key, and restart the app:

```sh
LANGSMITH_TRACING=true
LANGSMITH_API_KEY=lsv2_...           # from LangSmith: Settings -> API Keys
LANGSMITH_PROJECT=pbr-agent          # optional; "default" otherwise
```

`~/.pbr-renderer/agent.log` says "LangSmith tracing on" when it is. Traces go to LangSmith's
servers, prompts, scene contents and screenshots included. If LangSmith can't be reached, turns
work as usual and the agent waits at most 5 seconds for queued traces when it exits.

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
`PBR_SERVER_HOST` to use another path). The chat agent's tests use a scripted model in place of
Claude, so they need no API key:

```sh
agent/.venv/Scripts/python -m unittest discover -s agent/tests
```

They also run under `ctest` when `agent/.venv` exists.
