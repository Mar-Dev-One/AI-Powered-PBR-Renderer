"""End-to-end tests for Net/CommandServer: a real socket, a real handshake.

Starts ServerHost (a headless World behind the command server), then talks
to it with a minimal WebSocket client written here on the standard library
alone -- so the test also checks the server against an independent
implementation of RFC 6455 (Sec-WebSocket-Accept is computed with hashlib).

    python server_test.py <path to ServerHost> <discovery file path>
"""

import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import urllib.parse

FAILURES = []
CHECKS = 0


def check(condition, what):
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(what)
        print("CHECK failed:", what, file=sys.stderr)


class WebSocket:
    """Just enough of a WebSocket client for these tests."""

    def __init__(self, host, port, path, origin=None, raw_headers=None):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.key = base64.b64encode(os.urandom(16)).decode()
        headers = raw_headers if raw_headers is not None else [
            "Upgrade: websocket",
            "Connection: Upgrade",
            "Sec-WebSocket-Key: " + self.key,
            "Sec-WebSocket-Version: 13",
        ]
        if origin:
            headers.append("Origin: " + origin)
        request = "GET %s HTTP/1.1\r\nHost: %s:%d\r\n%s\r\n\r\n" % (path, host, port, "\r\n".join(headers))
        self.sock.sendall(request.encode())

        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            response += chunk
        head, _, self.pending = response.partition(b"\r\n\r\n")
        self.notifications = []   # server-pushed messages received while waiting for responses
        self.status_line = head.split(b"\r\n")[0].decode()
        self.headers = {}
        for line in head.split(b"\r\n")[1:]:
            name, _, value = line.decode().partition(":")
            self.headers[name.strip().lower()] = value.strip()

    @property
    def upgraded(self):
        return self.status_line.startswith("HTTP/1.1 101")

    def expected_accept(self):
        digest = hashlib.sha1((self.key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        return base64.b64encode(digest).decode()

    def send_frame(self, opcode, payload, fin=True, masked=True):
        header = bytes([(0x80 if fin else 0) | opcode])
        length = len(payload)
        mask_bit = 0x80 if masked else 0
        if length < 126:
            header += bytes([mask_bit | length])
        elif length <= 0xFFFF:
            header += bytes([mask_bit | 126]) + struct.pack(">H", length)
        else:
            header += bytes([mask_bit | 127]) + struct.pack(">Q", length)
        if masked:
            mask = os.urandom(4)
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            header += mask
        self.sock.sendall(header + payload)

    def _read_exact(self, n):
        while len(self.pending) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            self.pending += chunk
        data, self.pending = self.pending[:n], self.pending[n:]
        return data

    def recv_frame(self):
        b0, b1 = self._read_exact(2)
        opcode = b0 & 0x0F
        length = b1 & 0x7F
        if b1 & 0x80:   # only counted when it fails: every frame passes through here
            check(False, "server frames are unmasked")
        if length == 126:
            length = struct.unpack(">H", self._read_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._read_exact(8))[0]
        return opcode, self._read_exact(length)

    def recv_message(self):
        opcode, payload = self.recv_frame()
        check(opcode == 0x1, "messages are text frames")
        return json.loads(payload)

    def call(self, method, params=None, id=1):
        """Sends a request and returns its response; notifications that
        arrive first are kept in self.notifications."""
        message = {"jsonrpc": "2.0", "id": id, "method": method}
        if params is not None:
            message["params"] = params
        self.send_frame(0x1, json.dumps(message).encode())
        while True:
            reply = self.recv_message()
            if "id" in reply:
                return reply
            self.notifications.append(reply)

    def wait_notification(self):
        """The next server-pushed message (one kept earlier, or a new one)."""
        if self.notifications:
            return self.notifications.pop(0)
        reply = self.recv_message()
        check("id" not in reply, "expected a notification, got a response")
        return reply

    def close(self):
        self.sock.close()


def main():
    host_exe, discovery_path = sys.argv[1], sys.argv[2]
    host = subprocess.Popen([host_exe, discovery_path], stdout=subprocess.PIPE, text=True)
    try:
        url = host.stdout.readline().strip()
        parsed = urllib.parse.urlparse(url)
        port = parsed.port
        token = urllib.parse.parse_qs(parsed.query)["token"][0]
        good_path = "/?token=" + token

        # --- Discovery file ---------------------------------------------------
        with open(discovery_path) as f:
            info = json.load(f)
        check(info["url"] == url and info["token"] == token and info["port"] == port, "discovery file matches")
        check(parsed.hostname == "127.0.0.1", "listens on localhost only")

        # --- Handshake refusals -------------------------------------------------
        ws = WebSocket("127.0.0.1", port, "/?token=" + "0" * 32)
        check(ws.status_line.startswith("HTTP/1.1 403"), "wrong token is refused: " + ws.status_line)
        ws.close()

        ws = WebSocket("127.0.0.1", port, "/")
        check(ws.status_line.startswith("HTTP/1.1 403"), "missing token is refused")
        ws.close()

        ws = WebSocket("127.0.0.1", port, good_path, origin="https://evil.example")
        check(ws.status_line.startswith("HTTP/1.1 403"), "foreign origin is refused")
        ws.close()

        ws = WebSocket("127.0.0.1", port, good_path, raw_headers=["Connection: close"])
        check(ws.status_line.startswith("HTTP/1.1 400"), "non-upgrade request is refused")
        ws.close()

        # --- Handshake ------------------------------------------------------------
        ws = WebSocket("127.0.0.1", port, good_path, origin="http://localhost:3000")
        check(ws.upgraded, "upgrade accepted: " + ws.status_line)
        check(ws.headers.get("sec-websocket-accept") == ws.expected_accept(), "Sec-WebSocket-Accept is correct")

        # --- JSON-RPC over the wire -------------------------------------------------
        r = ws.call("entity.create", {"name": "lamp", "components": {"Light": {"type": "point", "intensity": 4}}})
        check(r.get("result", {}).get("name") == "lamp", "entity.create works")
        check(r["result"]["changes"][0]["op"] == "created", "changes come with the result")

        r = ws.call("entity.get", {"entity": "sofa"}, id="x")
        check(r["id"] == "x" and r["error"]["code"] == -32001 and r["error"]["data"]["code"] == "not_found",
              "command errors map to JSON-RPC errors")

        r = ws.call("nope.nope", id=3)
        check(r["error"]["code"] == -32601, "unknown method")

        ws.send_frame(0x1, b"{not json")
        check(ws.recv_message()["error"]["code"] == -32700, "parse error")

        # A notification runs without a reply: the next frame answers id 9.
        ws.send_frame(0x1, json.dumps({"jsonrpc": "2.0", "method": "entity.create", "params": {"name": "quiet"}}).encode())
        r = ws.call("entity.get", {"entity": "quiet"}, id=9)
        check(r["id"] == 9 and r["result"]["name"] == "quiet", "notifications run, unanswered")

        # --- Framing ---------------------------------------------------------------------
        ws.send_frame(0x9, b"hello")                         # ping
        opcode, payload = ws.recv_frame()
        check(opcode == 0xA and payload == b"hello", "ping gets a matching pong")

        message = json.dumps({"jsonrpc": "2.0", "id": 10, "method": "entity.get", "params": {"entity": "lamp"}}).encode()
        ws.send_frame(0x1, message[:20], fin=False)          # fragmented
        ws.send_frame(0x0, message[20:40], fin=False)
        ws.send_frame(0x0, message[40:], fin=True)
        check(ws.recv_message().get("id") == 10, "fragmented messages are reassembled")

        # 64-bit length in, 16-bit+ lengths out: create many entities in one batch.
        commands = [{"cmd": "entity.create", "args": {"name": "e%d" % i}} for i in range(400)]
        r = ws.call("batch", {"commands": commands, "label": "padding " + "x" * 70000}, id=11)
        check(len(r["result"]["results"]) == 400, "large request and response")

        r = ws.call("scene.describe", id=12)
        check(r["result"]["count"] == 402, "scene sees everything: %s" % r.get("result", {}).get("count"))

        # --- Two clients ---------------------------------------------------------------------
        ws.notifications.clear()
        other = WebSocket("127.0.0.1", port, good_path)
        check(other.upgraded, "a second client connects")
        r = other.call("history.undo", id=1)
        check(r["result"]["undone"].startswith("padding"), "clients share the same world and history")
        check(len(other.notifications) == 1 and other.notifications[0]["params"]["source"].startswith("undo"),
              "the undoing client got its own event before the reply")
        other.notifications.clear()
        r = ws.call("scene.describe", {"summary": True}, id=13)
        check(r["result"]["count"] == 2, "the other client's undo is visible")

        # --- Change events ---------------------------------------------------------------------
        # The first client heard about the second one's undo, unprompted.
        event = ws.wait_notification()
        check(event.get("method") == "scene.changed", "changes are pushed as scene.changed")
        params = event.get("params", {})
        check(params.get("source", "").startswith("undo padding"), "the event says what caused it")
        check(len(params.get("changes", [])) == 400 and params["changes"][0]["op"] == "destroyed",
              "the event lists what changed")
        undo_revision = params.get("revision", 0)

        # A change from this client: the other one is told; this one gets
        # the event too, just before its own response.
        ws.call("entity.create", {"name": "news"}, id=14)
        mine = ws.notifications.pop(0) if ws.notifications else {}
        check(mine.get("params", {}).get("source") == "entity.create news", "the caller gets its own event first")
        check(mine.get("params", {}).get("revision") == undo_revision + 1, "revisions count up")

        theirs = other.wait_notification()
        check(theirs.get("params", {}).get("changes", [{}])[0].get("name") == "news", "other clients are told")

        # Reads and failed commands change nothing, so nothing is pushed:
        # the next message the other client sees is the reply to its own call.
        ws.call("scene.describe", id=15)
        ws.call("entity.get", {"entity": "nobody"}, id=16)
        other.call("scene.describe", id=2)
        check(not other.notifications, "no events for reads or failures")
        other.close()

        # --- Protocol errors close the connection -------------------------------------------
        bad = WebSocket("127.0.0.1", port, good_path)
        bad.send_frame(0x1, b"{}", masked=False)
        opcode, payload = bad.recv_frame()
        check(opcode == 0x8 and struct.unpack(">H", payload[:2])[0] == 1002, "unmasked client frame: close 1002")
        bad.close()

        # --- Clean close -------------------------------------------------------------------
        ws.send_frame(0x8, struct.pack(">H", 1000))
        opcode, payload = ws.recv_frame()
        while opcode == 0x1:   # a notification queued before the close
            opcode, payload = ws.recv_frame()
        check(opcode == 0x8, "close is answered")
        ws.close()

        last = WebSocket("127.0.0.1", port, good_path)
        last.call("test.shutdown", id=99)
        last.close()
        host.wait(timeout=10)
        check(not os.path.exists(discovery_path), "discovery file removed on stop")
    finally:
        if host.poll() is None:
            host.kill()

    print("server_test: %d checks, %d failed" % (CHECKS, len(FAILURES)))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
