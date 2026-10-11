"""Tests for pbr_agent.EngineClient against the real command server.

Runs tests/server_host.c's ServerHost (a headless World behind the command
server; built with the engine) -- set PBR_SERVER_HOST to its path if it
isn't in the default build folder.

    agent/.venv/Scripts/python -m unittest discover -s agent/tests
"""

import asyncio
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pbr_agent import EngineClient, EngineError, EngineNotRunning, read_discovery  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
DEFAULT_HOST = REPO / "build" / "tests" / ("Debug/ServerHost.exe" if sys.platform == "win32" else "ServerHost")
SERVER_HOST = Path(os.environ.get("PBR_SERVER_HOST", DEFAULT_HOST))


class Host:
    """A ServerHost process writing its discovery file to `discovery_path`
    (and saving scenes to `scenes_dir`, if given)."""

    def __init__(self, discovery_path: Path, scenes_dir: Path | None = None):
        command = [str(SERVER_HOST), str(discovery_path)] + ([str(scenes_dir)] if scenes_dir else [])
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
        self.url = self.process.stdout.readline().strip()

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.process.stdout.close()


@unittest.skipUnless(SERVER_HOST.exists(), f"build ServerHost first ({SERVER_HOST})")
class EngineClientTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.discovery = Path(self.dir.name) / "server.json"
        self.host = Host(self.discovery)

    def tearDown(self):
        self.host.stop()
        self.dir.cleanup()

    async def test_calls_and_results(self):
        async with EngineClient(discovery_path=self.discovery) as engine:
            created = await engine.call("entity.create", {"name": "lamp", "components": {"Light": {"intensity": 3}}})
            self.assertEqual(created["name"], "lamp")
            self.assertEqual(created["changes"][0]["op"], "created")

            entity = await engine.call("entity.get", {"entity": "lamp"})
            self.assertEqual(entity["components"]["Light"]["intensity"], 3)

            # Concurrent calls on one connection each get their own answer.
            names = [f"e{i}" for i in range(20)]
            results = await asyncio.gather(*(engine.call("entity.create", {"name": n}) for n in names))
            self.assertEqual([r["name"] for r in results], names)

    async def test_errors_carry_the_engine_code(self):
        async with EngineClient(discovery_path=self.discovery) as engine:
            with self.assertRaises(EngineError) as caught:
                await engine.call("entity.get", {"entity": "sofa"})
            self.assertEqual(caught.exception.command_code, "not_found")
            self.assertEqual(caught.exception.code, -32001)
            self.assertIn("no entity named", caught.exception.message)

            with self.assertRaises(EngineError) as caught:
                await engine.call("entity.fly")
            self.assertEqual(caught.exception.code, -32601)

    async def test_change_events(self):
        async with EngineClient(discovery_path=self.discovery) as watcher, \
                   EngineClient(discovery_path=self.discovery) as actor:
            seen = asyncio.Queue()
            watcher.add_change_listener(seen.put_nowait)

            async def async_listener(params):   # async listeners work too
                await asyncio.sleep(0)
            watcher.add_change_listener(async_listener)

            await watcher.call("scene.describe")   # make sure the watcher is connected first
            await actor.call("entity.create", {"name": "crate"})

            event = await asyncio.wait_for(seen.get(), 5)
            self.assertEqual(event["source"], "entity.create crate")
            self.assertEqual(event["changes"][0]["name"], "crate")

    async def test_notify(self):
        async with EngineClient(discovery_path=self.discovery) as engine:
            await engine.notify("entity.create", {"name": "quiet"})
            entity = await engine.call("entity.get", {"entity": "quiet"})   # sent after, answered after
            self.assertEqual(entity["name"], "quiet")

    async def test_reconnects_after_app_restart(self):
        engine = EngineClient(discovery_path=self.discovery)
        await engine.call("entity.create", {"name": "before"})

        # Restart the app: a new process with a new token (and possibly port).
        old_url = self.host.url
        self.host.stop()
        self.host = Host(self.discovery)
        self.assertNotEqual(self.host.url, old_url)

        result = await engine.call("scene.describe")   # transparently reconnects
        self.assertEqual(result["count"], 0)            # a fresh world
        await engine.close()

    async def test_not_running(self):
        missing = Path(self.dir.name) / "nope.json"
        with self.assertRaises(EngineNotRunning) as caught:
            await EngineClient(discovery_path=missing).call("scene.describe")
        self.assertIn("isn't running", str(caught.exception))

        # A discovery file left behind by an app that died.
        stale = Path(self.dir.name) / "stale.json"
        info = json.loads(self.discovery.read_text())
        info["pid"] = 2 ** 22 + 12345   # not a running process
        stale.write_text(json.dumps(info))
        with self.assertRaises(EngineNotRunning) as caught:
            read_discovery(stale)
        self.assertIn("has exited", str(caught.exception))

    async def test_wrong_token(self):
        url = self.host.url.split("token=")[0] + "token=" + "0" * 32
        with self.assertRaises(EngineNotRunning) as caught:
            await EngineClient(url).call("scene.describe")
        self.assertIn("403", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
