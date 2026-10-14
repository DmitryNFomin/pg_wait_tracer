"""Recording fixture layered on the existing mock, without changing it.

Overview evolves: time-model CPU/IO are redistributed (total DB Time
unchanged), and the corresponding AAS layers change. The first session's PID
matches the execution fixture so the literal Sessions drill remains populated.
All served UI assets come from mock_server.py. This is mock evidence only.
"""
import copy
import time

import mock_server

original = mock_server.handle_request
tick = 0
phase = 0
_last_tick_at = None

# issue #411/#409: the real product only advances what the Overview shows
# once per live tick (app.js startAutoRefresh's setTimeout(.., 5000) --
# exactly 5 real seconds), but this mock used to flip `phase` on EVERY
# "aas" request regardless of how much wall-clock time had passed. A
# recording walk can legitimately issue more than one "aas" request within
# a single real 5s window (e.g. a cache-fill alongside the periodic poll),
# and each one used to toggle the mock's induced swing again -- producing
# smoother, MORE FREQUENT motion than the real daemon+bridge ever can. A
# gate calibrated only against that smoother mock is calibrated wrong, the
# same mock-versus-real gap that produced #409's frame starvation (the mock
# has no network latency either). Throttling the flip to real wall-clock
# time makes repeat requests inside one tick window return the SAME
# phase -- exactly like the real backend's per-second accounting does not
# change just because the UI happened to ask twice quickly.
TICK_INTERVAL_S = 5.0


def handle_request(msg):
    global tick, phase, _last_tick_at
    response = copy.deepcopy(original(msg))
    if msg.get("cmd") == "aas" and response.get("buckets"):
        now = time.monotonic()
        if _last_tick_at is None or now - _last_tick_at >= TICK_INTERVAL_S:
            phase = tick % 2
            tick += 1
            _last_tick_at = now
        factors = {"cpu": 1.6 if phase else 0.65, "io": 0.4 if phase else 1.5}
        for bucket in response["buckets"]:
            for key, factor in factors.items():
                bucket[key] *= factor
        response["max_aas"] = max(sum(v for k, v in b.items()
                                      if k != "t" and isinstance(v, (int, float)))
                                  for b in response["buckets"])
    if msg.get("cmd") == "time_model" and response.get("rows"):
        delta = 1800 if phase else -1800
        for row in response["rows"]:
            if row["name"] == "CPU (running)":
                row["ms"] += delta
            elif row["name"] == "IO":
                row["ms"] -= delta
            elif row["name"].startswith("IO:"):
                row["ms"] *= (3200-delta)/3200
            row["pct"] = row["ms"]/response["db_time_ms"]*100 if row["name"] != "Idle" else 0
            row["aas"] = row["ms"]/3600 if row["name"] != "Idle" else 0
        response["cpu_ms"] += delta
    return response


if __name__ == "__main__":
    mock_server._CANNED["top_sessions"]["rows"][0]["pid"] = 1000
    # The base fixture has no favicon. Serve a legitimate empty response so
    # Chrome's implicit request does not introduce an unrelated 404 error.
    original_get = mock_server.StaticHandler.do_GET
    def do_get(self):
        if self.path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
        else:
            original_get(self)
    mock_server.StaticHandler.do_GET = do_get
    mock_server.handle_request = handle_request
    mock_server.main()
