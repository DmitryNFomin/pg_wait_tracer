#!/usr/bin/env python3
"""summary_raw_parity_lib.py — the PURE comparison core of the summary-vs-raw
parity gate (tests/test_summary_raw_parity.py).

WHY THIS GATE EXISTS
─────────────────────────────────────────────────────────────────────────────
pgwt-server answers a window from EITHER raw events OR per-second summaries
(`should_use_summaries`, src/server.c): any unfiltered window >= 120 s with
EXACT fidelity goes to summaries. Nothing ever compared the two answers on the
same window, and that single gap is why #402, #403, #408, #379 and #319 all
survived three clean rehearsals — the rehearsal's conservation checks use a
60 s window, which takes the RAW path and therefore never looks at the code
that answers the demo's own 900 s window.

This module holds everything that can be checked without a server: field
extraction, comparison, the issue-numbered expected-divergence table, the
route proof and the verdict. The live half — building a synthetic capture,
running both routes, proving they ARE two routes — is
tests/test_summary_raw_parity.py; the false-negative suite over this module is
tests/test_summary_raw_parity_lib.py.

WHAT IS COMPARED
─────────────────────────────────────────────────────────────────────────────
Not a hand-picked list of numbers. For every endpoint the extraction walks
EVERY top-level scalar and EVERY scalar in EVERY row, plus three derived
fields the per-key walk cannot see on its own:

  * `<view>/rowset`      — the row SET and its ORDER. The summary path emits
                           one `CPU*` where raw emits `CPU (running)` +
                           `CPU (waiting for a core)` (#408); a check that
                           only compared per-row numbers would call that "no
                           difference", because the rows never meet.
  * `time_model/cpu_group_ms` — the CPU group total (every row whose name
                           starts with CPU) under either row shape, because
                           #408's real harm is a +3.54% CPU total that the
                           per-row walk cannot reach while the names differ.
  * `aas/integral_ms`    — the AAS chart's integral; bucket layout may
                           legitimately differ, the area a viewer reads as DB
                           time may not.

Absence is a value: a key present on one side only is a FAILURE (a blank
column in the UI), never "equal". A `top_wait` that is null on BOTH sides
agrees (an honest "unknown" is allowed); null on one side and a name on the
other does not.

TOLERANCE
─────────────────────────────────────────────────────────────────────────────
Both paths replay the same fixed on-disk file, so both are deterministic and
the comparison is EXACT up to float formatting (1e-9 relative, 1e-6 absolute).
Deliberately not a percentage allowance: #408 is +3.54% and #379 is +1.14% on
DB Time, so any "2% tolerance" would swallow exactly the defects this gate
exists to catch.

A GATE THAT CANNOT SEE MUST REFUSE, NEVER APPROVE
─────────────────────────────────────────────────────────────────────────────
Every known way this check could pass while measuring nothing is a FAIL here,
not a skip (tests/test_summary_raw_parity_lib.py drives each one):

  * both sides took the same route            -> FAIL. About to become live:
    the #408/#379 fixes bump the summary format v6 -> v7, after which a v6
    capture is REFUSED and silently recomputed from raw — both sides raw,
    perfect agreement, nothing compared. Version refusal is detected as its
    own named case of the same-route failure.
  * a route could not be determined at all    -> FAIL
  * either side is empty/vacuous              -> FAIL
  * a compared field is MISSING on one side   -> FAIL
  * a compared field is unparseable           -> FAIL
  * a dependency is missing or not executable -> FAIL (never a skip: 126/127
    and "the two paths agree" must not share an exit status)
  * an empty expected table                   -> every divergence FAILs
  * a KNOWN divergence changes magnitude      -> FAIL (each entry pins the
    measured pair, so "known issue" never licenses a new number)
  * a KNOWN divergence disappears             -> XPASS, reported, does NOT
    fail: that is what a fix for #408/#379/#319 looks like, and the entry is
    then deleted.
"""

import math
import os

# ── Statuses ────────────────────────────────────────────────────────────────

AGREE = "agree"
KNOWN = "known"
XPASS = "xpass"
NEW = "NEW-DIVERGENCE"
CHANGED = "CHANGED-DIVERGENCE"
MISSING = "MISSING-FIELD"
BAD = "UNPARSEABLE-FIELD"

FAIL_STATUSES = (NEW, CHANGED, MISSING, BAD)

REL_TOL = 1e-9
ABS_TOL = 1e-6

ABSENT = "<absent>"
UNPARSEABLE = "<unparseable>"


class Finding:
    def __init__(self, field, raw, summary, status, issue=None, note=""):
        self.field = field
        self.raw = raw
        self.summary = summary
        self.status = status
        self.issue = issue
        self.note = note

    @property
    def delta(self):
        if _num(self.raw) and _num(self.summary):
            return self.summary - self.raw
        return None

    @property
    def rel(self):
        d = self.delta
        if d is None or not self.raw:
            return None
        return d / self.raw

    def line(self):
        if self.delta is not None:
            what = "raw=%.6f summary=%.6f delta=%+.6f" % (
                self.raw, self.summary, self.delta)
            if self.rel is not None:
                what += " (%+.4f%%)" % (self.rel * 100.0)
        else:
            what = "raw=%r summary=%r" % (self.raw, self.summary)
        tag = self.status
        if self.issue is not None:
            tag += " #%s" % self.issue
        out = "  [%-22s] %-46s %s" % (tag, self.field, what)
        if self.note:
            out += "  -- " + self.note
        return out

    def as_dict(self):
        return {"field": self.field, "raw": self.raw, "summary": self.summary,
                "status": self.status, "issue": self.issue,
                "delta": self.delta, "rel": self.rel, "note": self.note}


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) \
        and not (isinstance(v, float) and math.isnan(v))


def _scalar(v):
    return v is None or isinstance(v, (int, float, bool, str))


def _same(a, b):
    # bool and number are different answers even when Python would equate
    # them: has_measured_cpu=False is not cpu_ms=0.
    if isinstance(a, bool) != isinstance(b, bool):
        return False
    if _num(a) and _num(b):
        return abs(a - b) <= max(ABS_TOL, REL_TOL * max(abs(a), abs(b)))
    return a == b


# ── Field extraction ────────────────────────────────────────────────────────

def _scalars(resp, prefix, skip=("id",)):
    """Every top-level scalar of a response, as <prefix>/<key>.

    Generic on purpose: hand-picking fields is how `has_measured_cpu` (#408's
    actual root) and `io_worker_busy_pct` stay uncompared for a year.
    """
    out = {}
    if not isinstance(resp, dict):
        out[prefix + "/<response>"] = UNPARSEABLE + ":%s" % type(resp).__name__
        return out
    for k, v in resp.items():
        if k in skip:
            continue
        if _scalar(v):
            out["%s/%s" % (prefix, k)] = v
    return out


def _rows(resp, prefix, key_field, limit=None, order_field="rowset"):
    """Row-set (ordered) + every scalar of every row, keyed by the row's
    identity (`key_field`: a class-row name, a pid, a query_id)."""
    out = {}
    rows = resp.get("rows", ABSENT) if isinstance(resp, dict) else ABSENT
    if rows is ABSENT:
        out["%s/%s" % (prefix, order_field)] = ABSENT
        return out
    if not isinstance(rows, list):
        out["%s/%s" % (prefix, order_field)] = UNPARSEABLE
        return out
    ident = []
    for r in (rows[:limit] if limit else rows):
        if not isinstance(r, dict) or key_field not in r:
            out["%s/%s" % (prefix, order_field)] = UNPARSEABLE
            return out
        k = r[key_field]
        tag = "%s@%s" % (k, r.get("indent")) if "indent" in r else str(k)
        ident.append(tag)
        for kk, vv in r.items():
            if _scalar(vv):
                out["%s/row/%s/%s" % (prefix, k, kk)] = vv
    out["%s/%s" % (prefix, order_field)] = " | ".join(ident)
    return out


def extract_time_model(resp):
    out = _scalars(resp, "time_model")
    out.update(_rows(resp, "time_model", "name"))
    # The CPU group under either row shape (#408).
    rows = resp.get("rows") if isinstance(resp, dict) else None
    if isinstance(rows, list):
        total, seen = 0.0, False
        for r in rows:
            if not isinstance(r, dict):
                continue
            n = r.get("name")
            if isinstance(n, str) and n.upper().startswith("CPU"):
                seen = True
                if _num(r.get("ms")):
                    total += r["ms"]
                else:
                    total, seen = UNPARSEABLE, True
                    break
        out["time_model/cpu_group_ms"] = total if seen else ABSENT
    else:
        out["time_model/cpu_group_ms"] = ABSENT
    return out


def extract_aas(resp):
    """Scalars plus the integral. Bucket boundaries may legitimately be laid
    out differently; the area may not."""
    out = _scalars(resp, "aas")
    buckets = resp.get("buckets", ABSENT) if isinstance(resp, dict) else ABSENT
    width = resp.get("bucket_ns", ABSENT) if isinstance(resp, dict) else ABSENT
    if buckets is ABSENT or width is ABSENT:
        out["aas/integral_ms"] = ABSENT
        return out
    if not isinstance(buckets, list) or not _num(width):
        out["aas/integral_ms"] = UNPARSEABLE
        return out
    total = 0.0
    for b in buckets:
        if not isinstance(b, dict):
            out["aas/integral_ms"] = UNPARSEABLE
            return out
        for k, v in b.items():
            if k in ("t", "cat"):
                continue
            if _num(v):
                total += v
    out["aas/integral_ms"] = total * width / 1e6
    return out


def extract_sessions(resp):
    """Sessions: the pid row set and every scalar column including `top_wait`
    (#319) — compared over ALL returned rows, so a session past the default
    display limit cannot hide a divergence."""
    out = _scalars(resp, "sessions")
    out.update(_rows(resp, "sessions", "pid"))
    return out


def extract_queries(resp, limit=8):
    """Queries: the top rows INCLUDING their order (the order is the answer to
    "what do I look at first") and every scalar column."""
    out = _scalars(resp, "queries")
    out.update(_rows(resp, "queries", "query_id", limit=limit,
                     order_field="order"))
    return out


def extract_all(answers):
    fields = {}
    fields.update(extract_time_model(answers.get("time_model", {})))
    fields.update(extract_aas(answers.get("aas", {})))
    fields.update(extract_sessions(answers.get("top_sessions", {})))
    fields.update(extract_queries(answers.get("top_queries", {})))
    return fields


# ── Comparison ──────────────────────────────────────────────────────────────

def compare(raw_fields, summary_fields, expected):
    """expected: {field: {"issue": <int|str>, "raw": v, "summary": v}}.

    A pinned pair per entry is what keeps "known issue" from licensing an
    arbitrary new number.
    """
    findings = []
    for field in sorted(set(raw_fields) | set(summary_fields)):
        rv = raw_fields.get(field, ABSENT)
        sv = summary_fields.get(field, ABSENT)
        exp = expected.get(field)
        iss = exp.get("issue") if exp else None

        if rv is ABSENT or sv is ABSENT or rv == ABSENT or sv == ABSENT:
            only = "summary" if (rv is ABSENT or rv == ABSENT) else "raw"
            if exp and _same(rv, exp.get("raw")) and _same(sv, exp.get("summary")):
                findings.append(Finding(field, rv, sv, KNOWN, issue=iss,
                                        note="present on %s only" % only))
            else:
                findings.append(Finding(field, rv, sv, MISSING, issue=iss,
                                        note="present on %s only" % only))
            continue
        if (isinstance(rv, str) and rv.startswith(UNPARSEABLE)) or \
           (isinstance(sv, str) and sv.startswith(UNPARSEABLE)):
            findings.append(Finding(field, rv, sv, BAD, issue=iss))
            continue

        if _same(rv, sv):
            if exp:
                findings.append(Finding(
                    field, rv, sv, XPASS, issue=iss,
                    note="recorded as divergent, now AGREES -- if #%s is "
                         "fixed, delete this entry from "
                         "tests/summary_raw_parity_expected.json. Removing "
                         "the pin is the last step of landing that fix, not "
                         "an edit to be nervous about" % iss))
            else:
                findings.append(Finding(field, rv, sv, AGREE))
            continue

        if not exp:
            findings.append(Finding(field, rv, sv, NEW))
        elif _same(rv, exp.get("raw")) and _same(sv, exp.get("summary")):
            findings.append(Finding(field, rv, sv, KNOWN, issue=iss))
        else:
            findings.append(Finding(
                field, rv, sv, CHANGED, issue=iss,
                note="recorded raw=%r summary=%r"
                     % (exp.get("raw"), exp.get("summary"))))
    order = {NEW: 0, CHANGED: 1, MISSING: 2, BAD: 3, XPASS: 4, KNOWN: 5,
             AGREE: 6}
    findings.sort(key=lambda f: (order.get(f.status, 9), f.field))
    return findings


# ── Route proof ─────────────────────────────────────────────────────────────

ROUTE_RAW = "raw"
ROUTE_SUMMARY = "summary"
ROUTE_UNKNOWN = "unknown"

# src/server.c's two raw-fallback diagnostics. The SECOND one is the v6->v7
# hazard: after an accounting-version bump every old summary file is refused
# and the window is silently recomputed from raw, which would otherwise read
# as "the summary path agrees perfectly".
FALLBACK_MARKERS = (
    "no summary file covers this window",
    "are not summary v",
)


def fallback_reason(stderr_text):
    for m in FALLBACK_MARKERS:
        if m in (stderr_text or ""):
            for line in (stderr_text or "").splitlines():
                if m in line:
                    return line.strip()
    return None


def classify_route(oracle_resp, stderr_text, marker_expected=True):
    """Which path did pgwt-server take for this (dir, window)?

    THE ORACLE: the same binary, same dir, same window, re-run with
    PGWT_LOAD_MAX_EVENTS=1. The RAW path must materialise the window's events
    and therefore refuses with code=window_too_large; the SUMMARY path loads
    no events at all and answers normally. That is intrinsic to the route —
    not a label the test assigned itself.

    CROSS-CHECKED against the server's own stderr: every raw fallback prints
    one of FALLBACK_MARKERS, which the summary route never prints. Two
    independent signals that must AGREE; a contradiction returns 'unknown',
    which the caller must treat as a refusal. In particular an oracle whose
    env var never took effect (no error, or max_events != 1) reads as
    'unknown', never as 'summary'.

    `marker_expected=False` is for a window SHORTER than the 120 s threshold:
    there the raw path is taken on length alone and src/server.c prints no
    fallback line, so the absence of a marker carries no information and the
    oracle decides alone. A marker that contradicts the oracle still forces
    'unknown' -- fail-closed in both modes.
    """
    marker = fallback_reason(stderr_text) is not None
    by_oracle = ROUTE_UNKNOWN
    if isinstance(oracle_resp, dict):
        if oracle_resp.get("code") == "window_too_large":
            if oracle_resp.get("max_events") == 1:
                by_oracle = ROUTE_RAW
        elif "rows" in oracle_resp and "error" not in oracle_resp:
            by_oracle = ROUTE_SUMMARY
    if by_oracle == ROUTE_UNKNOWN:
        return ROUTE_UNKNOWN
    if not marker_expected:
        return ROUTE_UNKNOWN if (marker and by_oracle == ROUTE_SUMMARY) \
            else by_oracle
    by_marker = ROUTE_RAW if marker else ROUTE_SUMMARY
    return by_oracle if by_oracle == by_marker else ROUTE_UNKNOWN


# ── Non-vacuity ─────────────────────────────────────────────────────────────

MIN_DB_TIME_MS = 1000.0
MIN_CLASS_ROWS = 3
MIN_SESSIONS = 2
MIN_QUERIES = 2


def vacuity_problems(fields, side):
    """Two paths that both return nothing agree perfectly. Content is
    asserted, never assumed."""
    bad = []
    db = fields.get("time_model/db_time_ms", ABSENT)
    if not _num(db) or db < MIN_DB_TIME_MS:
        bad.append("%s: DB Time is %r, need >= %.0f ms" % (side, db, MIN_DB_TIME_MS))
    rowset = fields.get("time_model/rowset", ABSENT)
    nclass = len([n for n in rowset.split(" | ") if n.endswith("@1")]) \
        if isinstance(rowset, str) else 0
    if nclass < MIN_CLASS_ROWS:
        bad.append("%s: %d class rows, need >= %d" % (side, nclass, MIN_CLASS_ROWS))
    sess = fields.get("sessions/rowset", ABSENT)
    nsess = len(sess.split(" | ")) if isinstance(sess, str) and sess \
        and sess != ABSENT else 0
    if nsess < MIN_SESSIONS:
        bad.append("%s: %d sessions, need >= %d" % (side, nsess, MIN_SESSIONS))
    qs = fields.get("queries/order", ABSENT)
    nq = len(qs.split(" | ")) if isinstance(qs, str) and qs and qs != ABSENT else 0
    if nq < MIN_QUERIES:
        bad.append("%s: %d queries, need >= %d" % (side, nq, MIN_QUERIES))
    return bad


def require_tools(paths):
    """A missing or non-executable dependency (the 126/127 family) FAILS this
    gate; 'could not run' and 'the paths agree' must not share an exit code."""
    bad = []
    for p in paths:
        if not os.path.exists(p):
            bad.append("missing dependency (would exit 127): %s" % p)
        elif not os.access(p, os.X_OK):
            bad.append("not executable (would exit 126): %s" % p)
    return bad


# ── Verdict ─────────────────────────────────────────────────────────────────

def verdict(window, routes, raw_fields, summary_fields, expected,
            tool_problems=(), fallbacks=None):
    """(ok, lines, findings) for one window — diagnostic first: every
    divergence prints with field name and magnitude whether or not it fails
    the gate."""
    lines = ["--- window %s ---" % window]
    findings = []
    ok = True

    for p in tool_problems:
        lines.append("  FAIL dependency: %s" % p)
        ok = False
    if tool_problems:
        return ok, lines, findings

    fallbacks = fallbacks or {}
    lines.append("  routes: raw-side=%s summary-side=%s"
                 % (routes.get("raw"), routes.get("summary")))
    for side, reason in sorted(fallbacks.items()):
        if reason:
            lines.append("  %s-side raw fallback: %s" % (side, reason))

    if ROUTE_UNKNOWN in (routes.get("raw"), routes.get("summary")):
        lines.append("  FAIL route: a side's path could not be determined "
                     "(oracle and stderr marker disagree, or the oracle never "
                     "took effect) -- refusing to compare")
        ok = False
    elif routes.get("raw") == routes.get("summary"):
        extra = ""
        if fallbacks.get("summary"):
            extra = (" The summary side fell back to raw: %s -- an accounting "
                     "version refusal (v6->v7) is exactly this case."
                     % fallbacks["summary"])
        lines.append("  FAIL route: BOTH sides took the %s path, so this "
                     "comparison is vacuous (the #408 sabotage control caught "
                     "exactly this).%s" % (routes.get("raw"), extra))
        ok = False
    elif routes.get("raw") != ROUTE_RAW or routes.get("summary") != ROUTE_SUMMARY:
        lines.append("  FAIL route: sides are swapped or mislabelled")
        ok = False

    for p in (vacuity_problems(raw_fields, "raw")
              + vacuity_problems(summary_fields, "summary")):
        lines.append("  FAIL vacuous: %s" % p)
        ok = False

    if not ok:
        return ok, lines, findings

    findings = compare(raw_fields, summary_fields, expected)
    counts = {}
    for f in findings:
        counts[f.status] = counts.get(f.status, 0) + 1
        if f.status != AGREE:
            lines.append(f.line())
        if f.status in FAIL_STATUSES:
            ok = False
    lines.append("  %d fields compared: %s" % (
        len(findings),
        ", ".join("%s=%d" % (k, counts[k]) for k in sorted(counts))))
    return ok, lines, findings


def record_table(findings):
    """The expected-divergence table for the divergences just measured, ready
    to paste into the gate. Used by `--record` after a deliberate fix or
    fixture change, never to silence a surprise."""
    out = {}
    for f in findings:
        if f.status in (KNOWN, CHANGED, NEW, MISSING):
            out[f.field] = {"issue": f.issue if f.issue is not None else "UNFILED",
                            "raw": f.raw, "summary": f.summary}
    return out
