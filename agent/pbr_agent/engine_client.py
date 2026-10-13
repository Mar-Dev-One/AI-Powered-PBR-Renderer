"""Async client for the PBR Renderer's command server.

The app publishes where it listens in a discovery file
(~/.pbr-renderer/server.json: url with its per-run token, port, pid). The
client reads it, connects over WebSocket and speaks JSON-RPC 2.0: a command
name is the method, its arguments are the params (see
engine/src/Commands/JsonRpc.h). Changes to the scene arrive as
"scene.changed" notifications; any other notification (the chat's, say) can
be listened to by method name.

    async with EngineClient() as engine:
        lamp = await engine.call("place_model", {"model": "builtin:sphere", "size": 0.3})
        engine.add_change_listener(print)
"""

from __future__ import annotations

import asyncio
import inspect
import itertools
import json
import os
import sys
from pathlib import Path
from typing import Any, Awaitable, Callable

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

DEFAULT_DISCOVERY_PATH = Path.home() / ".pbr-renderer" / "server.json"

# Screenshots and full scene descriptions can be large.
MAX_MESSAGE_BYTES = 64 * 1024 * 1024

ChangeListener = Callable[[dict], Any]
NotificationListener = Callable[[dict], Any]

CHANGE_NOTIFICATION = "scene.changed"

# Commands that only read: safe to send again if the connection dropped
# before their answer arrived (see EngineClient.call).
READ_ONLY_METHODS = frozenset({
    "commands.list", "components.describe", "scene.describe", "scene.list", "entity.get",
    "assets.list", "history.list", "environment.list", "view.get", "view.screenshot", "chat.history",
})


class EngineNotRunning(ConnectionError):
    """The app isn't running, or its command server can't be reached."""


class EngineError(Exception):
    """A command was rejected or failed. `message` says why and how to fix it.

    `code` is the JSON-RPC error code; `command_code` the engine's own
    ("not_found", "invalid_value", "batch_failed", ...).
    """

    def __init__(self, message: str, code: int, command_code: str | None):
        super().__init__(message)
        self.message = message
        self.code = code
        self.command_code = command_code


def _engine_error(error: dict) -> EngineError:
    return EngineError(error.get("message", "command failed"), error.get("code", 0),
                       (error.get("data") or {}).get("code"))


def _process_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if sys.platform == "win32":
        import ctypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            return bool(kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code))) and exit_code.value == STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True   # exists, owned by someone else
    return True


def read_discovery(path: str | os.PathLike | None = None) -> dict:
    """The running app's connection details, from its discovery file.

    Raises EngineNotRunning if there is no file, or if it was left behind by
    an app that has since exited (e.g. crashed).
    """
    path = Path(path) if path else DEFAULT_DISCOVERY_PATH
    try:
        info = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise EngineNotRunning(
            f"The PBR Renderer isn't running (no {path}). Start the app (the Editor) and try again."
        ) from None
    except (OSError, ValueError) as error:
        raise EngineNotRunning(f"Can't read {path}: {error}") from None

    if "url" not in info:
        raise EngineNotRunning(f"{path} has no server url")
    if "pid" in info and not _process_alive(int(info["pid"])):
        raise EngineNotRunning(
            f"The PBR Renderer isn't running (the app that wrote {path} has exited). Start it and try again."
        )
    return info


class EngineClient:
    """One connection to the app, shared by any number of concurrent calls.

    If the connection drops (the app was closed or restarted), the next call
    re-reads the discovery file -- a restarted app has a new token -- and
    reconnects once before giving up.

    `hello`, if given, is sent as the server's "server.hello" params on every
    (re)connection -- e.g. {"client": "pbr-agent", "role": "chat_agent"} to
    claim a role (engine/src/Net/CommandServer.h). If the server refuses it,
    connecting fails with that EngineError.
    """

    def __init__(
        self,
        url: str | None = None,
        *,
        discovery_path: str | os.PathLike | None = None,
        call_timeout: float = 60.0,
        hello: dict | None = None,
    ):
        self._fixed_url = url
        self._discovery_path = discovery_path
        self._call_timeout = call_timeout
        self._hello = hello

        self._ws: ClientConnection | None = None
        self._reader: asyncio.Task | None = None
        self._closed: asyncio.Event | None = None   # set once the current connection is gone
        self._pending: dict[int, asyncio.Future] = {}
        self._ids = itertools.count(1)
        self._listeners: dict[str, list[NotificationListener]] = {}
        self._connect_lock = asyncio.Lock()

    # --- connection -------------------------------------------------------------

    @property
    def connected(self) -> bool:
        return self._ws is not None

    async def connect(self) -> None:
        async with self._connect_lock:
            if self._ws is not None:
                return

            url = self._fixed_url or read_discovery(self._discovery_path)["url"]
            try:
                # Localhost only: never through a system proxy. No Origin header
                # (the server only accepts browsers from localhost), no
                # compression (the server doesn't negotiate it), no keep-alive
                # pings (the app polls once per frame and may be paused).
                self._ws = await connect(url, max_size=MAX_MESSAGE_BYTES, open_timeout=5, ping_interval=None,
                                         proxy=None, compression=None)
            except InvalidStatus as error:
                raise EngineNotRunning(f"The app refused the connection ({error.response.status_code}): "
                                       "the token may be out of date") from None
            except OSError as error:
                raise EngineNotRunning(f"Can't reach the app at {url.split('?')[0]}: {error}") from None

            self._closed = asyncio.Event()
            self._reader = asyncio.create_task(self._read_loop(self._ws, self._closed))

            if self._hello is not None:
                try:
                    reply = await self._roundtrip("server.hello", self._hello)
                except EngineNotRunning:
                    await self.close()
                    raise
                if "error" in reply:
                    await self.close()
                    raise _engine_error(reply["error"])

    async def wait_closed(self) -> None:
        """Returns once the current connection has closed (at once if there is none)."""
        closed = self._closed
        if self._ws is not None and closed is not None:
            await closed.wait()

    async def close(self) -> None:
        ws, self._ws = self._ws, None
        if self._reader:
            self._reader.cancel()
            self._reader = None
        if ws is not None:
            await ws.close()
        self._fail_pending(EngineNotRunning("connection closed"))

    async def __aenter__(self) -> "EngineClient":
        await self.connect()
        return self

    async def __aexit__(self, *exc_info) -> None:
        await self.close()

    # --- calls ------------------------------------------------------------------

    async def call(self, method: str, params: dict | None = None) -> Any:
        """Runs one command and returns its result; raises EngineError if the
        engine rejected it, EngineNotRunning if the app can't be reached.

        A request that couldn't be sent (stale connection: the app was
        restarted) is retried once on a fresh connection, and so is a
        read-only one whose connection dropped before it was answered. A
        command that changes the scene and was already sent is not retried
        -- it may have run, and running e.g. place_model twice would create
        two entities."""
        retry_after_send = method in READ_ONLY_METHODS

        for attempt in range(2):
            sent: list[int] = []
            try:
                # Let the reader notice a connection that has already been
                # closed (app quit or restarted) before we reuse it.
                await asyncio.sleep(0)
                await self.connect()
                reply = await self._roundtrip(method, params, sent)
                break
            except EngineNotRunning:
                await self.close()
                if sent and not retry_after_send:
                    raise EngineNotRunning(
                        f"The connection to the app was lost after sending {method}; it may or may not have "
                        "run. Check the scene (scene.describe) before trying again."
                    ) from None
                if attempt == 1:
                    raise

        if "error" in reply:
            raise _engine_error(reply["error"])
        return reply.get("result")

    async def _roundtrip(self, method: str, params: dict | None, sent: list | None = None) -> dict:
        """Sends one request on the current connection and returns the raw
        reply. Appends to `sent` once the request has gone out."""
        request_id = next(self._ids)
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params

        future: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[request_id] = future
        try:
            await self._send(message)
            if sent is not None:
                sent.append(request_id)
            return await asyncio.wait_for(future, self._call_timeout)
        finally:
            self._pending.pop(request_id, None)

    async def notify(self, method: str, params: dict | None = None) -> None:
        """Runs a command without waiting for (or getting) a result."""
        await self.connect()
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        await self._send(message)

    async def _send(self, message: dict) -> None:
        ws = self._ws
        if ws is None:
            raise EngineNotRunning("not connected")
        try:
            await ws.send(json.dumps(message))
        except ConnectionClosed:
            raise EngineNotRunning("the app closed the connection") from None

    # --- incoming ------------------------------------------------------------------

    def add_change_listener(self, listener: ChangeListener) -> None:
        """Calls `listener(params)` for every "scene.changed" notification:
        {"revision", "source", "changes": [...], "world"}. May be async."""
        self.add_notification_listener(CHANGE_NOTIFICATION, listener)

    def remove_change_listener(self, listener: ChangeListener) -> None:
        self.remove_notification_listener(CHANGE_NOTIFICATION, listener)

    def add_notification_listener(self, method: str, listener: NotificationListener) -> None:
        """Calls `listener(params)` for every notification named `method` that
        the app pushes, e.g. "chat.event". May be async; listeners run one at
        a time in arrival order, so a slow one holds up the rest."""
        self._listeners.setdefault(method, []).append(listener)

    def remove_notification_listener(self, method: str, listener: NotificationListener) -> None:
        self._listeners[method].remove(listener)

    async def _read_loop(self, ws: ClientConnection, closed: asyncio.Event) -> None:
        try:
            async for raw in ws:
                try:
                    message = json.loads(raw)
                except ValueError:
                    continue

                if "id" in message and message["id"] is not None:
                    future = self._pending.get(message["id"])
                    if future and not future.done():
                        future.set_result(message)
                elif isinstance(message.get("method"), str):
                    await self._dispatch(message["method"], message.get("params") or {})
        except ConnectionClosed:
            pass
        finally:
            if self._ws is ws:
                self._ws = None
            self._fail_pending(EngineNotRunning("the app closed the connection"))
            closed.set()

    async def _dispatch(self, method: str, params: dict) -> None:
        for listener in list(self._listeners.get(method, ())):
            try:
                result = listener(params)
                if inspect.isawaitable(result):
                    await result
            except Exception as error:   # a listener's bug mustn't kill the connection
                print(f"pbr_agent: {method} listener failed: {error!r}", file=sys.stderr)

    def _fail_pending(self, error: Exception) -> None:
        for future in self._pending.values():
            if not future.done():
                future.set_exception(error)
        self._pending.clear()
