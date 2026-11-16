"""Talk to the running PBR Renderer from Python.

`EngineClient` connects to the app's command server (a localhost WebSocket
speaking JSON-RPC 2.0, see engine/src/Net/CommandServer.h), runs commands
and receives scene change events.
"""

from .engine_client import (
    DEFAULT_DISCOVERY_PATH,
    EngineClient,
    EngineError,
    EngineNotRunning,
    read_discovery,
)

__all__ = [
    "DEFAULT_DISCOVERY_PATH",
    "EngineClient",
    "EngineError",
    "EngineNotRunning",
    "read_discovery",
]
