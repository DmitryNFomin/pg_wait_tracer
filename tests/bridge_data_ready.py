#!/usr/bin/env python3
"""#683: /session is HTTP readiness; only an answered info is DATA readiness.

Exit 0 only for our info reply with num_cpus and no error field; 1 means
the deadline expired, 2 means we cannot interpret the session or run the
probe. A probe that cannot see must REFUSE, never approve.
"""
import argparse
import asyncio
import json
import math
import queue
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


class Refusal(Exception):
    """Uninterpretable evidence is exit 2, never a readiness signal."""


class Parser(argparse.ArgumentParser):
    def error(self, message):
        # Missing/malformed arguments cannot define a gate; never default
        # them into a vacuous pass (argparse's help still exits normally).
        raise Refusal("bad arguments: " + message)


async def session_token(url, timeout):
    # urllib handles HTTP framing; a daemon worker keeps even a stalled
    # body/DNS lookup inside the overall asyncio deadline. Cancelling a
    # default-executor worker would still hang asyncio.run's shutdown.
    outcome = queue.Queue()

    def fetch():
        try:
            with urllib.request.urlopen(url, timeout=timeout) as response:
                if response.status != 200:
                    raise Refusal("/session HTTP %s" % response.status)
                body = response.read()
            outcome.put((body, None))
        except Exception as exc:
            # Forward EVERY worker failure; a dead worker must not look
            # like a successful empty session or swallow an exit-2 refusal.
            outcome.put((None, exc))

    threading.Thread(target=fetch, daemon=True).start()
    while outcome.empty():
        await asyncio.sleep(0.01)
    body, error = outcome.get_nowait()
    if error is not None:
        raise error
    try:
        session = json.loads(body)
    except (ValueError, UnicodeError) as exc:
        # HTTP 200 with unreadable JSON proves nothing about a token.
        raise Refusal("bad JSON from /session: %s" % exc) from exc
    if (not isinstance(session, dict) or not isinstance(session.get("token"), str)
            or not session["token"]):
        # Missing/empty/non-string tokens cannot authenticate our request.
        raise Refusal("/session missing usable token")
    return session["token"]


async def info_answered(connect, ws_url, seen):
    seen["last"] = "transport error: WebSocket upgrade did not complete"
    async with connect(ws_url, open_timeout=1, close_timeout=0.1,
                       ping_interval=None) as ws:
        await ws.send(json.dumps({"id": 1, "cmd": "info"}))
        seen["last"] = "no reply to info"
        while True:
            raw = await ws.recv()
            try:
                reply = json.loads(raw)
            except (ValueError, UnicodeError) as exc:
                # A received frame alone is not an interpreted info reply.
                seen["last"] = "bad JSON in info reply: %s" % exc
                continue
            if not isinstance(reply, dict):
                # Parseable arrays/scalars are not info response envelopes.
                seen["last"] = "bad JSON info envelope: expected an object"
            elif type(reply.get("id")) is not int or reply["id"] != 1:
                # An unsolicited/other request's answer cannot green ours.
                seen["last"] = "info reply has mismatched id: %r" % reply.get("id")
            elif "error" in reply:
                # b.up=false sends this envelope even over a healthy WS;
                # presence of error refuses, including null/empty errors.
                seen["last"] = "transport error in info reply: %r" % reply["error"]
            elif "num_cpus" not in reply:
                # A matching JSON reply without the UI's required field
                # cannot establish #status.connected.
                seen["last"] = "info reply lacks num_cpus"
            else:
                return True


async def probe(base, timeout, connect, transport_error):
    deadline = time.monotonic() + timeout
    session_url = base + "session"
    ws_url = None
    seen = {"last": "no reply from /session"}
    while time.monotonic() < deadline:
        try:
            if ws_url is None:
                token = await asyncio.wait_for(
                    session_token(session_url, min(1, deadline - time.monotonic())),
                    timeout=max(0, deadline - time.monotonic()))
                parts = urllib.parse.urlsplit(base)
                ws_url = urllib.parse.urlunsplit((
                    "wss" if parts.scheme == "https" else "ws", parts.netloc,
                    parts.path + "ws", urllib.parse.urlencode({"token": token}), ""))
            ready = await asyncio.wait_for(info_answered(connect, ws_url, seen),
                                          timeout=min(1, max(0, deadline - time.monotonic())))
            # The sole GREEN path: a validated answer before the deadline.
            # A late answer must not turn expiry into success.
            if ready and time.monotonic() < deadline:
                return 0
        except urllib.error.HTTPError as exc:
            # A missing/non-200 session is uninterpretable, not ready.
            raise Refusal("/session HTTP %s" % exc.code) from exc
        except (asyncio.TimeoutError, TimeoutError):
            # Preserve the last observation (silent info, bad JSON, etc.);
            # timing out is NEVER the success path.
            pass
        except (OSError, urllib.error.URLError, transport_error) as exc:
            seen["last"] = "transport error: %s: %s" % (type(exc).__name__, exc)
        await asyncio.sleep(min(0.1, max(0, deadline - time.monotonic())))
    # Exhaustion is RED even if /session and the WS upgrade were green.
    print("REFUSE bridge data path: deadline expired; last seen: " + seen["last"],
          file=sys.stderr)
    return 1


def main():
    try:
        parser = Parser(description=__doc__)
        parser.add_argument("--base-url", required=True)
        parser.add_argument("--timeout", required=True, type=float)
        args = parser.parse_args()
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            # Zero/negative/NaN/infinite deadlines cannot define a bounded
            # observation; refusing prevents an empty poll from passing.
            raise Refusal("bad arguments: --timeout must be finite and > 0")
        parts = urllib.parse.urlsplit(args.base_url)
        if (parts.scheme not in ("http", "https") or not parts.hostname
                or parts.username is not None or parts.query or parts.fragment):
            # An ambiguous endpoint cannot establish which bridge answered.
            raise Refusal("bad arguments: --base-url must be an HTTP(S) base URL")
        parts.port  # Invalid port syntax is an argument refusal too.
        try:
            from websockets.asyncio.client import connect
            from websockets.exceptions import WebSocketException
        except ImportError as exc:
            # Missing tooling cannot see the data path; it must fail closed.
            raise Refusal("missing websockets module (pip install websockets): %s" % exc) from exc
        return asyncio.run(probe(args.base_url.rstrip("/") + "/", args.timeout,
                                 connect, WebSocketException))
    except Exception as exc:
        # Includes unexpected inspection failures: no crash or malformed
        # session can fall through to exit 0. SystemExit is not swallowed.
        print("REFUSE bridge data path: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
