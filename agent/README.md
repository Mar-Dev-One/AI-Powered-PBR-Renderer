# pbr_agent

Python side of the PBR Renderer's AI scene builder (see `docs/ROADMAP.md`): talk to the running
app, and expose its commands to AI tools.

- `pbr_agent.EngineClient`: async client for the app's command server (a localhost WebSocket
  speaking JSON-RPC 2.0). It finds the app through `~/.pbr-renderer/server.json`, which the app
  writes at startup, and reconnects if the app is restarted.

## Setup

```sh
python -m venv agent/.venv
agent/.venv/Scripts/python -m pip install -e agent        # Windows
agent/.venv/bin/python -m pip install -e agent            # macOS / Linux
```

## Use

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
