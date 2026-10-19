#!/usr/bin/env python3
"""The Queries/Waterfall span fixture must stay equal to what the mock serves.

Why this exists (integration finding, release-integration-2). The two halves
of #455 were briefed from the issue text, not from each other. The backend
(src/backend_span.c) emits a Queries row's span as
`backend_activity: {source, span, ...}` with ns boundaries as JSON STRINGS,
counts as `exec_starts` and multi-statement as `multi_exec`/`multi_query`;
the UI half read `backend_activity_span`, `start_observed`,
`executions_count` and `multi_statement`. Every one of those reads missed,
so the Queries cell drew the string "Not measured" over a measured, closed,
420 ms span -- a dead cell, green in every Node unit test, because both
halves' tests used the UI half's own placeholder shape.

tests/web_unit/backend-span-wire.test.mjs now renders the REAL shape, from
the checked-in fixture below. A checked-in fixture can go stale, and a stale
fixture is exactly how this gate would go blind again -- so this guard
regenerates it from tests/mock_server.py and requires equality.

A guard that cannot see must REFUSE, never approve. An unimportable mock, a
fixture that is missing, empty or unparseable, and a fixture whose content is
vacuous (no measured span, no row envelope, no span_detail spans) each fail
instead of passing. Run with --selftest to exercise those paths.
"""

import json
import os
import sys

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(TEST_DIR, "web_unit", "fixtures",
                       "mock_backend_activity.json")

# The literal field names the UI reads. Named here so a rename on either side
# fails this guard rather than silently blanking a cell.
ROW_ENVELOPE_KEY = "backend_activity"
DETAIL_ENVELOPE_KEY = "backend_activity_span"


def build_from_mock(mock):
    """The fixture, rebuilt from a mock_server module.

    Raises ValueError when the mock cannot answer, so "the mock changed
    shape" can never read as "nothing to compare".
    """
    tq = mock.handle_request({"id": 1, "cmd": "top_queries"})
    rows = (tq or {}).get("rows") or []
    if not rows:
        raise ValueError("mock top_queries returned no rows")
    sd = mock.handle_request({"id": 2, "cmd": "span_detail",
                              "filters": {"pid": 1001}})
    if not isinstance(sd, dict) or "spans" not in sd:
        raise ValueError("mock span_detail returned no spans key")
    ed = mock.handle_request({"id": 3, "cmd": "execution_detail",
                              "filters": {"pid": 1001},
                              "start_ns": "1792316000000000000"})
    if not isinstance(ed, dict):
        raise ValueError("mock execution_detail returned no object")
    measured = None
    unmeasured = None
    for row in rows:
        act = row.get(ROW_ENVELOPE_KEY)
        span = (act or {}).get("span") if isinstance(act, dict) else None
        if span and span.get("measurement") == "measured":
            measured = measured or row
        elif act is None or not span or span.get("measurement") == "unmeasured":
            unmeasured = unmeasured or row
    if measured is None:
        raise ValueError("mock serves no MEASURED row span: the fixture would "
                         "not be able to tell a dead cell from a true absence")
    if unmeasured is None:
        raise ValueError("mock serves no unmeasured/absent row span: the "
                         "fixture could not prove absence still reads absent")
    return {
        "_source": "generated from tests/mock_server.py by "
                   "tests/test_mock_backend_activity_fixture.py -- "
                   "regenerate with --write, never by hand",
        "measured_row": measured,
        "unmeasured_row": unmeasured,
        "top_queries_root": {k: v for k, v in tq.items() if "backend" in k},
        "span_detail": sd,
        "execution_detail": {k: v for k, v in ed.items() if "backend" in k},
    }


def load_fixture(path):
    """The checked-in fixture. Raises on missing, empty, unparseable or
    vacuous content -- every one of those is "cannot see", not "agrees"."""
    if not os.path.exists(path):
        raise ValueError("fixture missing: %s" % path)
    with open(path) as fh:
        text = fh.read()
    if not text.strip():
        raise ValueError("fixture is empty: %s" % path)
    data = json.loads(text)          # JSONDecodeError on garbage: refuse
    if not isinstance(data, dict):
        raise ValueError("fixture is not an object")
    assert_non_vacuous(data)
    return data


def assert_non_vacuous(data):
    """The fixture must carry the three things the Node test asserts on."""
    row = data.get("measured_row")
    act = (row or {}).get(ROW_ENVELOPE_KEY)
    span = (act or {}).get("span") if isinstance(act, dict) else None
    if not span or span.get("measurement") != "measured":
        raise ValueError("fixture has no measured row span")
    if span.get("completed_duration_ms") in (None, 0):
        raise ValueError("fixture's measured span has no completed duration")
    if not (data.get("span_detail") or {}).get("spans"):
        raise ValueError("fixture has no span_detail spans")
    if DETAIL_ENVELOPE_KEY not in (data.get("execution_detail") or {}):
        raise ValueError("fixture has no execution_detail span envelope")


def import_mock():
    sys.path.insert(0, TEST_DIR)
    try:
        import mock_server
    except Exception as exc:                      # noqa: BLE001
        raise ValueError("cannot import tests/mock_server.py: %s" % exc)
    if not hasattr(mock_server, "handle_request"):
        raise ValueError("mock_server has no handle_request()")
    return mock_server


def _diff(a, b, path=""):
    """First differing path, or None. Keeps the failure message actionable."""
    if type(a) is not type(b):
        return "%s: %s vs %s" % (path or "<root>", type(a).__name__,
                                 type(b).__name__)
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a:
                return "%s.%s missing from the fixture" % (path, key)
            if key not in b:
                return "%s.%s missing from the mock" % (path, key)
            d = _diff(a[key], b[key], "%s.%s" % (path, key))
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return "%s: %d vs %d entries" % (path, len(a), len(b))
        for i, (x, y) in enumerate(zip(a, b)):
            d = _diff(x, y, "%s[%d]" % (path, i))
            if d:
                return d
        return None
    return None if a == b else "%s: %r vs %r" % (path, a, b)


def selftest():
    failures = 0

    def check(ok, msg):
        nonlocal failures
        if ok:
            print("  ok: %s" % msg)
        else:
            failures += 1
            print("  FAIL: %s" % msg)

    print("--- 1. the real tree: fixture == what the mock serves ---")
    mock = import_mock()
    fresh = build_from_mock(mock)
    stored = load_fixture(FIXTURE)
    d = _diff(stored, fresh)
    check(d is None, "fixture matches the mock (%s)" % (d or "no difference"))

    print("--- 2. the guard must REFUSE when it cannot see ---")
    bad_fixtures = [
        ("missing file", os.path.join(TEST_DIR, "no-such-fixture.json")),
        ("empty file", os.devnull),
    ]
    for name, path in bad_fixtures:
        try:
            load_fixture(path)
        except ValueError:
            check(True, "refuses a %s" % name)
        else:
            check(False, "APPROVED a %s -- it must raise" % name)

    import tempfile
    for name, text in (
        ("unparseable JSON", "{not json"),
        ("a JSON array", "[]"),
        ("an empty object", "{}"),
    ):
        with tempfile.NamedTemporaryFile("w", suffix=".json",
                                         delete=False) as fh:
            fh.write(text)
            tmp = fh.name
        try:
            load_fixture(tmp)
        except (ValueError, json.JSONDecodeError):
            check(True, "refuses %s" % name)
        else:
            check(False, "APPROVED %s -- it must raise" % name)
        finally:
            os.unlink(tmp)

    print("--- 3. vacuous content is refused, not approved ---")
    import copy
    vacuous = [
        ("no measured span", lambda f: f["measured_row"].pop(
            ROW_ENVELOPE_KEY)),
        ("measured span without a duration",
         lambda f: f["measured_row"][ROW_ENVELOPE_KEY]["span"].update(
             {"completed_duration_ms": None})),
        ("span_detail with no spans",
         lambda f: f["span_detail"].update({"spans": []})),
        ("execution_detail without the span envelope",
         lambda f: f["execution_detail"].pop(DETAIL_ENVELOPE_KEY)),
    ]
    for name, mutate in vacuous:
        f = copy.deepcopy(stored)
        mutate(f)
        try:
            assert_non_vacuous(f)
        except ValueError:
            check(True, "refuses a fixture with %s" % name)
        else:
            check(False, "APPROVED a fixture with %s" % name)

    print("--- 4. a broken mock is refused, not approved ---")

    class NoRows:
        @staticmethod
        def handle_request(msg):
            return {"rows": []}

    class NoSpanDetail:
        @staticmethod
        def handle_request(msg):
            if msg["cmd"] == "top_queries":
                return {"rows": [dict(stored["measured_row"]),
                                 dict(stored["unmeasured_row"])]}
            return {}

    class OnlyUnmeasured:
        @staticmethod
        def handle_request(msg):
            if msg["cmd"] == "top_queries":
                return {"rows": [dict(stored["unmeasured_row"])]}
            if msg["cmd"] == "span_detail":
                return dict(stored["span_detail"])
            return dict(stored["execution_detail"])

    for name, fake in (("a mock serving no rows", NoRows),
                       ("a mock whose span_detail lost its spans key",
                        NoSpanDetail),
                       ("a mock serving ONLY unmeasured rows",
                        OnlyUnmeasured)):
        try:
            build_from_mock(fake)
        except ValueError:
            check(True, "refuses %s" % name)
        else:
            check(False, "APPROVED %s -- it must raise" % name)

    print("--- 5. the guard DETECTS a real drift ---")
    drifted = copy.deepcopy(stored)
    drifted["measured_row"][ROW_ENVELOPE_KEY]["span"]["exec_starts"] = 999
    check(_diff(drifted, fresh) is not None,
          "spots a renamed/changed field value")
    renamed = copy.deepcopy(stored)
    renamed["measured_row"]["backend_activity_span"] = \
        renamed["measured_row"].pop(ROW_ENVELOPE_KEY)
    check(_diff(renamed, fresh) is not None,
          "spots the envelope key being renamed")
    check(_diff(copy.deepcopy(stored), fresh) is None,
          "and reports nothing when nothing drifted")

    print("\n%s" % ("FAILED" if failures else "PASSED"))
    return 1 if failures else 0


def main():
    if "--write" in sys.argv:
        data = build_from_mock(import_mock())
        os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
        with open(FIXTURE, "w") as fh:
            json.dump(data, fh, indent=1, sort_keys=True)
            fh.write("\n")
        print("wrote %s" % FIXTURE)
        return 0
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
