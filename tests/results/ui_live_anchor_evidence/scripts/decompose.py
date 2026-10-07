"""Decompose ui_live blink-sweep drift into its measurable components.

achieved_offsets_ms[k] = (time of just-before-screenshot k) - mount_at_ms.
So:
  A    := achieved[0]                 -- mount -> first capture start ("pre-sweep overhead")
  cap1 := achieved[1] - achieved[0]   -- screenshot 1 cost + one Date.now() round trip
  cap2 := achieved[2] - achieved[1]   -- screenshot 2 cost + one round trip
  f3 drift = achieved[2] - 1000 = (A + cap1 + cap2) - 1000, when that is positive.
Verified against the recorded capture_ms list (cap_k ~= capture_ms[k-1] + ~6ms).
"""
import json, os, sys, datetime

HEAVY = ["scatter", "matrix", "timeline", "transitions", "concurrency"]
LIGHT = ["overview", "events", "sessions", "queries", "histogram", "waterfall"]


def load(p):
    with open(p) as f:
        return json.load(f)


def runmeta(path):
    d = os.path.dirname(path)
    rid = None
    rp = os.path.join(d, "run.id")
    if os.path.exists(rp):
        rid = open(rp).read().strip()
    # Chronology comes from run.id (unix epoch seconds written by the run
    # itself), NOT file mtime: every tracked/committed artifact shares one
    # checkout mtime, which would silently collapse the time series.
    if rid and rid.isdigit():
        mt = datetime.datetime.fromtimestamp(int(rid)).strftime("%Y-%m-%d %H:%M")
    else:
        mt = "NO-run.id-" + os.path.basename(os.path.dirname(path))[:6]
    return rid, mt


def tick1(tab):
    ticks = (tab.get("blink_sweep") or {}).get("ticks") or []
    return ticks[0] if ticks else None


rows = []
for p in sys.argv[1:]:
    try:
        d = load(p)
    except Exception as e:
        print(f"SKIP {p}: {e}", file=sys.stderr)
        continue
    rid, mt = runmeta(p)
    label = p.split("/.claude/worktrees/")[-1].split("/tests/")[0] if "/.claude/worktrees/" in p \
        else os.path.basename(os.path.dirname(p))
    tabs = d.get("tabs") or {}
    rows.append((mt, rid, label, tabs))

rows.sort()

print(f"{'when':<17}{'run.id':<12}{'worktree':<36} "
      f"{'A heavy med':>11}{'cap heavy med':>14}{'f3 heavy max':>13}"
      f"{'f45 all max':>12}{'ttfp heavy med':>15}")
for mt, rid, label, tabs in rows:
    As, caps, f3s, f45s, ttfps = [], [], [], [], []
    for name, tab in tabs.items():
        t = tick1(tab)
        allticks = (tab.get("blink_sweep") or {}).get("ticks") or []
        for tk in allticks:
            a = tk.get("achieved_offsets_ms") or []
            tg = tk.get("target_offsets_ms") or []
            if len(a) >= 5 and len(tg) >= 5:
                f45s.append(max(abs(a[3] - tg[3]), abs(a[4] - tg[4])))
        if t is None:
            continue
        a = t.get("achieved_offsets_ms") or []
        cm = t.get("capture_ms") or []
        if name in HEAVY:
            if len(a) >= 1:
                As.append(a[0])
            if len(a) >= 3:
                f3s.append(a[2] - 1000)
            if cm:
                caps.append(sum(cm) / len(cm))
            v = tab.get("ttfp_ms")
            if isinstance(v, (int, float)):
                ttfps.append(v)

    def med(xs):
        if not xs:
            return None
        xs = sorted(xs)
        return xs[len(xs) // 2]

    def fmt(v):
        return f"{v:.0f}" if isinstance(v, (int, float)) else "-"

    print(f"{mt:<17}{str(rid):<12}{label[:35]:<36} "
          f"{fmt(med(As)):>11}{fmt(med(caps)):>14}{fmt(max(f3s) if f3s else None):>13}"
          f"{fmt(max(f45s) if f45s else None):>12}{fmt(med(ttfps)):>15}")
