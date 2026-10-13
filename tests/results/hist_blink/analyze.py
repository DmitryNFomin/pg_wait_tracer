import json
import sys

d = json.load(open(sys.argv[1]))
print("threshold", d["threshold"], "offsets", d["offsets_ms"])
rows = []
for w in d["walks"]:
    for t in w["ticks"]:
        dg = [r.get("digest") for r in t["frame_reads"]]
        ncells = [x["ncells"] if x else None for x in dg]
        nx = [x["nx"] if x else None for x in dg]
        ny = [x["ny"] if x else None for x in dg]
        prog = dg[0]["progressive"] if dg and dg[0] else None
        pthr = dg[0]["progressiveThreshold"] if dg and dg[0] else None
        paints = sum(p["paints_between"] for p in t["pairs"])
        sets = sum(p["setoptions_between"] for p in t["pairs"])
        unk = sum(1 for p in t["pairs"] if p["data_verdict"] == "UNKNOWN")
        nlog = len(t["log"]["paints"]) if t.get("log") else None
        rows.append((w["walk"], w.get("t_min"), t["tick"], t["gate_ratio"],
                     max(ncells), nx[0], ny[0], prog, pthr, paints, sets, unk, nlog))
print("walk t_min tick ratio ncells nx ny prog pthr paints_between setopts_between UNKNOWN n_paints_total")
for r in rows:
    print(" ".join(str(x) for x in r))
cells = [r[4] for r in rows if r[4] is not None]
print("ncells min/max:", min(cells), max(cells))
print("ratios distinct:", sorted({r[3] for r in rows}))
print("pairs total:", sum(len(t["pairs"]) for w in d["walks"] for t in w["ticks"]))
print("UNKNOWN pairs:", sum(1 for w in d["walks"] for t in w["ticks"]
                           for p in t["pairs"] if p["data_verdict"] == "UNKNOWN"))
# cross-tick data change detection sanity: digests must differ across ticks
w0 = d["walks"][0]
h = [t["frame_reads"][0]["digest"]["hash"] for t in w0["ticks"]]
print("walk1 per-tick frame0 hashes:", h, "distinct:", len(set(h)))
