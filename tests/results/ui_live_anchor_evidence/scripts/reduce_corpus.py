"""Build the reduced corpus committed next to this script.

The analysis reads only four fields per tab per tick -- target_offsets_ms,
achieved_offsets_ms, capture_ms, and the per-tab ttfp_ms. Full ui_live
summary.json files for 31 runs are 3.3 MB (frame dims, ratios, notes, leak
probes, per-tab artifacts); reduced to those four they are a few tens of KB,
and every script in this directory reads them UNCHANGED, because the reduced
file keeps the same nesting.

Run against live artifacts to regenerate:

    python3 reduce_corpus.py OUT_DIR SUMMARY_JSON...

Each input's sibling run.id file supplies the run identity (unix epoch seconds,
which is also the only reliable chronology -- the committed artifacts all share
one checkout mtime). An input without a readable run.id is REFUSED, not given a
synthetic name: a run that cannot be placed in time cannot support any of the
before/after claims this corpus exists to carry.
"""
import json
import os
import sys


def reduce_summary(d):
    out = {"ok": d.get("ok"), "tabs": {}}
    for name, tab in (d.get("tabs") or {}).items():
        ticks = []
        for t in (tab.get("blink_sweep") or {}).get("ticks") or []:
            # capture_ms is recorded to ~14 significant figures by
            # time.monotonic(); rounded to 1e-3 ms here, which is five orders
            # of magnitude below the 1 ms the analysis ever resolves and keeps
            # the committed corpus small. The offsets are already integers.
            cm = t.get("capture_ms")
            ticks.append({
                "target_offsets_ms": t.get("target_offsets_ms"),
                "achieved_offsets_ms": t.get("achieved_offsets_ms"),
                "capture_ms": ([round(x, 3) for x in cm] if cm else cm),
            })
        out["tabs"][name] = {
            "tab": name,
            "ttfp_ms": tab.get("ttfp_ms"),
            "blink_sweep": {"ticks": ticks},
        }
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out_dir, inputs = argv[0], argv[1:]
    written = 0
    for p in inputs:
        rid_path = os.path.join(os.path.dirname(p), "run.id")
        if not os.path.exists(rid_path):
            print(f"REFUSED (no run.id, cannot place in time): {p}", file=sys.stderr)
            continue
        rid = open(rid_path).read().strip()
        if not rid.isdigit():
            print(f"REFUSED (run.id {rid!r} is not epoch seconds): {p}", file=sys.stderr)
            continue
        with open(p) as f:
            d = json.load(f)
        dest = os.path.join(out_dir, rid)
        os.makedirs(dest, exist_ok=True)
        with open(os.path.join(dest, "summary.json"), "w") as f:
            json.dump(reduce_summary(d), f, sort_keys=True,
                      separators=(",", ":"))
            f.write("\n")
        with open(os.path.join(dest, "run.id"), "w") as f:
            f.write(rid + "\n")
        # Provenance: which artifact this run was reduced from. Most of these
        # paths are sibling agent worktrees that will be deleted once their PRs
        # land -- which is exactly why the corpus is committed here. The eight
        # runs under tests/results/ui_live_gate*/ are in-repo and can be
        # re-reduced from the originals at any time to check this reduction.
        with open(os.path.join(dest, "origin.txt"), "w") as f:
            f.write(os.path.abspath(p) + "\n")
        written += 1
    print(f"wrote {written} reduced runs to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
