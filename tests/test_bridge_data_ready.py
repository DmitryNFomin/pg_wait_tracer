#!/usr/bin/env python3
"""#683: exercise the real probe CLI against local HTTP/WebSocket stubs.

No box, root, ssh or pgwt-server. Every subprocess has a hard outer bound:
a gate that hangs instead of refusing is a failed control, never a skip.
"""
import asyncio
import json
import logging
import os
from pathlib import Path
import signal
import socket
import sys
import tempfile
import time
import unittest
import urllib.parse

try:
    from websockets.asyncio.server import serve
    from websockets.datastructures import Headers
    from websockets.exceptions import ConnectionClosed
    from websockets.http11 import Response
except ImportError as exc:
    # The controls themselves cannot see without this dependency. RED,
    # not a skipped suite that would silently approve the new list entry.
    print("REFUSE bridge data path controls: install websockets: %s" % exc)
    print("test_bridge_data_ready: checks=1 failures=1")
    sys.exit(1)

HERE = Path(__file__).resolve().parent
PROBE = HERE / "bridge_data_ready.py"
TOKEN = "local token +/&?"


class Stub:
    def __init__(self, mode="ready", prefix="/"):
        self.mode = mode
        self.prefix = prefix
        self.sessions = 0
        self.infos = 0
        self.info_seen = asyncio.Event()
        self.requests = []
        self.errors = []
        self.warm = asyncio.Event()
        self.timer = None

    async def __aenter__(self):
        # Silence expected upgrade refusals; the probe's own captured
        # diagnostic and the counters below are the evidence we assert.
        logger = logging.getLogger("bridge-data-ready-stub")
        logger.disabled = True
        self.server = await serve(self.handle, "127.0.0.1", 0,
                                  process_request=self.http, logger=logger,
                                  close_timeout=0.1)
        port = self.server.sockets[0].getsockname()[1]
        self.base = "http://127.0.0.1:%s%s" % (port, self.prefix)
        if self.mode == "late":
            self.timer = asyncio.get_running_loop().call_later(2, self.warm.set)
        return self

    async def __aexit__(self, *args):
        if self.timer is not None:
            self.timer.cancel()
        self.server.close()
        await asyncio.wait_for(self.server.wait_closed(), timeout=2)

    async def http(self, connection, request):
        if request.path == self.prefix + "session":
            self.sessions += 1
            status, body = 200, json.dumps({"token": TOKEN}).encode()
            if self.mode == "http500":
                status, body = 500, b"upstream unavailable"
            elif self.mode == "session_bad_json":
                body = b"not JSON"
            elif self.mode == "session_no_token":
                body = b"{}"
            elif self.mode == "session_empty_token":
                body = b'{"token":""}'
            elif self.mode == "session_wrong_shape":
                body = b"[]"
            elif self.mode == "session_stalls":
                # HTTP itself can hang too; only the short overall probe
                # deadline may bound this, not the stub or a real service.
                await connection.wait_closed()
            return Response(status, "OK" if status == 200 else "Unavailable",
                            Headers({"Content-Type": "application/json",
                                     "Content-Length": str(len(body)),
                                     "Connection": "close"}), body)
        if self.mode == "upgrade_refused":
            return connection.respond(403, "upgrade refused")
        return None

    async def handle(self, ws):
        path = urllib.parse.urlsplit(ws.request.path)
        if (path.path != self.prefix + "ws"
                or urllib.parse.parse_qs(path.query) != {"token": [TOKEN]}):
            self.errors.append("wrong WS path or token: " + ws.request.path)
            await ws.close()
            return
        if self.mode == "closes":
            await ws.close()
            return
        try:
            request = json.loads(await ws.recv())
            self.requests.append(request)
            if request != {"id": 1, "cmd": "info"}:
                self.errors.append("wrong info wire envelope: %r" % request)
                await ws.close()
                return
            self.infos += 1
            self.info_seen.set()
            if self.mode == "silent" or (self.mode == "late" and not self.warm.is_set()):
                # THE CAUSE: /session is 200 and WS opens, but an upstream
                # accepting stdin without replying must NEVER green a walk.
                await ws.wait_closed()
                return
            replies = {
                "error": {"id": 1, "error": "server unavailable (reconnecting)"},
                "null_error": {"id": 1, "num_cpus": 4, "error": None},
                "no_cpus": {"id": 1, "version": "fixture"},
                "wrong_id": {"id": 2, "num_cpus": 4},
                "no_id": {"num_cpus": 4},
                "boolean_id": {"id": True, "num_cpus": 4},
                "array": [1, {"num_cpus": 4}],
            }
            raw = ("not JSON" if self.mode == "bad_json" else
                   json.dumps(replies.get(self.mode, {"id": 1, "num_cpus": 4})))
            await ws.send(raw)
            await ws.wait_closed()
        except ConnectionClosed:
            # Probe expiry closes the local fixture's connection. This is
            # teardown, never the criterion used to approve a test case.
            pass


class Controls(unittest.IsolatedAsyncioTestCase):
    async def command(self, argv, bound):
        started = time.monotonic()
        process = await asyncio.create_subprocess_exec(
            *map(str, argv), stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT, start_new_session=True)
        try:
            out, _ = await asyncio.wait_for(process.communicate(), timeout=bound)
        except asyncio.TimeoutError:
            # A hanging gate is RED. Kill/reap it so the controls themselves
            # cannot hang, then fail rather than disguising it as a refusal.
            # Include a shell gate's probe child: an inherited output pipe
            # must not keep communicate hanging after only its shell died.
            os.killpg(process.pid, signal.SIGKILL)
            await asyncio.wait_for(process.communicate(), timeout=1)
            self.fail("gate hung beyond %.1fs: %r" % (bound, argv))
        elapsed = time.monotonic() - started
        self.assertLess(elapsed, bound, "refusal must have a bounded wall time")
        return process.returncode, out.decode(), elapsed

    async def run_probe(self, base, timeout=0.35, extra=()):
        bound = max(2, timeout + 1.5) if isinstance(timeout, (int, float)) else 2
        return await self.command(
            [sys.executable, *extra, PROBE, "--base-url", base,
             "--timeout", str(timeout)], bound)

    async def refusal(self, mode, code, diagnostic):
        async with Stub(mode) as stub:
            rc, out, _ = await self.run_probe(stub.base)
            self.assertEqual(rc, code, out)
            self.assertIn("REFUSE bridge data path:", out)
            self.assertIn(diagnostic, out)
            self.assertGreater(stub.sessions, 0, "must really GET /session")
            self.assertEqual(stub.errors, [])
            if mode in {"silent", "error", "null_error", "no_cpus", "wrong_id",
                        "no_id", "boolean_id", "bad_json", "array"}:
                self.assertGreater(stub.infos, 0, "must really send info")

    async def test_cause_http200_and_open_ws_with_silent_info_refuses(self):
        await self.refusal("silent", 1, "no reply to info")

    async def test_happy_path(self):
        async with Stub() as stub:
            rc, out, _ = await self.run_probe(stub.base, 1)
            self.assertEqual(rc, 0, out)
            self.assertEqual(stub.sessions, 1)
            self.assertEqual(stub.infos, 1)
            self.assertEqual(stub.errors, [])

    async def test_becomes_ready_after_two_seconds(self):
        async with Stub("late") as stub:
            rc, out, elapsed = await self.run_probe(stub.base, 4)
            self.assertEqual(rc, 0, out)
            self.assertTrue(stub.warm.is_set())
            self.assertGreaterEqual(stub.infos, 2, "must poll beyond the silent attempt")
            self.assertGreaterEqual(elapsed, 2)
            self.assertLess(elapsed, 4)
            self.assertEqual(stub.errors, [])

    async def test_session_http500(self):
        await self.refusal("http500", 2, "/session HTTP 500")

    async def test_session_bad_json(self):
        await self.refusal("session_bad_json", 2, "bad JSON from /session")

    async def test_session_missing_token(self):
        await self.refusal("session_no_token", 2, "missing usable token")

    async def test_session_empty_token(self):
        await self.refusal("session_empty_token", 2, "missing usable token")

    async def test_session_wrong_shape(self):
        await self.refusal("session_wrong_shape", 2, "missing usable token")

    async def test_session_stalls(self):
        await self.refusal("session_stalls", 1, "no reply from /session")

    async def test_ws_refuses_upgrade(self):
        await self.refusal("upgrade_refused", 1, "transport error")

    async def test_ws_closes_immediately(self):
        await self.refusal("closes", 1, "transport error")

    async def test_bridge_reconnecting_error(self):
        await self.refusal("error", 1, "server unavailable (reconnecting)")

    async def test_error_field_even_when_null_and_cpus_present(self):
        await self.refusal("null_error", 1, "transport error in info reply")

    async def test_info_without_num_cpus(self):
        await self.refusal("no_cpus", 1, "lacks num_cpus")

    async def test_info_mismatched_id(self):
        await self.refusal("wrong_id", 1, "mismatched id")

    async def test_info_missing_id(self):
        await self.refusal("no_id", 1, "mismatched id")

    async def test_boolean_id_does_not_equal_request_id(self):
        await self.refusal("boolean_id", 1, "mismatched id")

    async def test_info_bad_json(self):
        await self.refusal("bad_json", 1, "bad JSON in info reply")

    async def test_info_non_object(self):
        await self.refusal("array", 1, "bad JSON info envelope")

    async def test_nothing_listening(self):
        # Reserve an ephemeral port WITHOUT listening, so no other local
        # process can race in and accidentally provide the missing evidence.
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            base = "http://127.0.0.1:%s/" % reservation.getsockname()[1]
            rc, out, _ = await self.run_probe(base)
        self.assertEqual(rc, 1, out)
        # A bound but non-listening port may reject immediately or leave
        # connect pending until expiry (macOS). Both are observed refusals.
        self.assertRegex(out, r"last seen: (transport error|no reply from /session)")

    async def test_zero_deadline(self):
        rc, out, _ = await self.run_probe("http://127.0.0.1:1/", 0)
        self.assertEqual(rc, 2, out)
        self.assertIn("--timeout must be finite and > 0", out)

    async def test_negative_deadline(self):
        rc, out, _ = await self.run_probe("http://127.0.0.1:1/", -1)
        self.assertEqual(rc, 2, out)
        self.assertIn("--timeout must be finite and > 0", out)

    async def test_bad_arguments(self):
        # Non-finite numbers and malformed URLs cannot silently pick a
        # default budget/endpoint; every uninterpretable invocation is RED.
        for timeout in ("nan", "inf", "not-a-number"):
            rc, out, _ = await self.run_probe("http://127.0.0.1:1/", timeout)
            self.assertEqual(rc, 2, out)
            self.assertIn("REFUSE bridge data path:", out)
        for base in ("not-a-url", "http://localhost:bad/", "http://localhost/?query=1"):
            rc, out, _ = await self.run_probe(base)
            self.assertEqual(rc, 2, out)
        rc, out, _ = await self.command([sys.executable, PROBE], 2)
        self.assertEqual(rc, 2, out)
        self.assertIn("bad arguments", out)

    async def test_missing_websockets(self):
        # -S removes site packages from the CHILD only: this tests a real
        # unavailable import, not a mocked successful transport.
        rc, out, _ = await self.run_probe("http://127.0.0.1:1/", extra=("-S",))
        self.assertEqual(rc, 2, out)
        self.assertIn("missing websockets module", out)

    async def test_base_path_and_escaped_token(self):
        async with Stub(prefix="/bridge/") as stub:
            rc, out, _ = await self.run_probe(stub.base.rstrip("/"), 1)
            self.assertEqual(rc, 0, out)
            self.assertEqual(stub.errors, [])
            self.assertEqual(stub.infos, 1)

    async def shell_gate(self, base, pid, timeout):
        with tempfile.TemporaryDirectory(prefix="bridge-data-ready-") as temp:
            log = Path(temp) / "bridge.log"
            log.write_text("bridge-log-sentinel-683\n")
            return await self.command([
                "bash", "-c", 'source "$1"; wait_bridge_data_ready "$2" "$3" "$4" "$5"',
                "bridge-data-ready-control", HERE / "live_daemon_lib.sh",
                base, pid, log, timeout], 3)

    async def test_shell_gate_success(self):
        async with Stub() as stub:
            rc, out, _ = await self.shell_gate(stub.base, os.getpid(), 1)
            self.assertEqual(rc, 0, out)
            self.assertEqual(stub.infos, 1)

    async def test_shell_gate_refusal_tails_log(self):
        async with Stub("silent") as stub:
            rc, out, _ = await self.shell_gate(stub.base, os.getpid(), 0.35)
            self.assertEqual(rc, 1, out)
            self.assertIn("REFUSE bridge data path:", out)
            self.assertIn("bridge-log-sentinel-683", out)

    async def test_shell_dead_bridge_before_wait(self):
        dead = await asyncio.create_subprocess_exec(sys.executable, "-c", "pass")
        await dead.wait()
        async with Stub() as stub:
            rc, out, elapsed = await self.shell_gate(stub.base, dead.pid, 20)
            self.assertEqual(rc, 1, out)
            self.assertIn("exited before info probe", out)
            self.assertIn("bridge-log-sentinel-683", out)
            self.assertEqual(stub.sessions, 0)
            self.assertLess(elapsed, 2, "must not wait for the 20s deadline")

    async def test_shell_bridge_dies_during_wait(self):
        async with Stub("silent") as stub:
            bridge = await asyncio.create_subprocess_exec(
                sys.executable, "-c", "import sys; sys.stdin.buffer.read()",
                stdin=asyncio.subprocess.PIPE)

            async def die_after_info():
                # Kill only AFTER the real probe sends info; process startup
                # speed cannot turn this into the before-wait control.
                await asyncio.wait_for(stub.info_seen.wait(), timeout=2)
                bridge.terminate()
                await bridge.wait()

            reaper = asyncio.create_task(die_after_info())
            try:
                rc, out, elapsed = await self.shell_gate(stub.base, bridge.pid, 20)
                self.assertEqual(rc, 1, out)
                self.assertIn("exited while waiting for info", out)
                self.assertIn("bridge-log-sentinel-683", out)
                self.assertGreater(stub.infos, 0)
                self.assertLess(elapsed, 2, "must not wait for the 20s deadline")
            finally:
                if bridge.returncode is None:
                    bridge.kill()
                reaper.cancel()
                await asyncio.gather(reaper, return_exceptions=True)
                await bridge.wait()


if __name__ == "__main__":
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    failures = len(result.failures) + len(result.errors)
    print("test_bridge_data_ready: checks=%s failures=%s" % (result.testsRun, failures))
    sys.exit(0 if result.wasSuccessful() and result.testsRun > 0 else 1)
