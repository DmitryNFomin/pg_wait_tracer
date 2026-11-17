#!/usr/bin/env python3
"""Generate the coverage-verdict differential table (U2a; Refs #947, #871).

Inputs, committed copies of the frozen lead files:
  tests/fixtures/coverage/coverage-table-inputs.md   setups and windows
  tests/fixtures/coverage/coverage-answers-final.tsv expected answers
Output: a C header for tests/test_coverage_verdict.c (built into ../build,
never into the tree).

Windows, verdicts, reasons, the xf mark, uncovered ranges, covered_to,
restart_cause and the idle verdict are parsed from the two files. The setups
are prose, so MODELS encodes each one by hand and pins the exact setup text it
was encoded from: an edit to either file fails generation instead of
drifting. Uncovered-range brackets are not compared (a measure-zero endpoint
is not represented). restart_cause tokens follow Amendment A1 item 2, so the
answer file's "postgresql" is read as "postgres".

EXTRA holds the A1 item 4 case, where rule precedence and time order differ.
"""
import argparse
import csv
import re
import sys
from decimal import Decimal
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures" / "coverage"
INPUTS_NAME = "coverage-table-inputs.md"
ANSWERS_NAME = "coverage-answers-final.tsv"

REASONS = ["capture_loss", "missing_file", "unclean_end", "retention", "restart",
           "clock_step", "not_yet_covered", "not_recorded", "not_captured"]
VERDICTS = {"complete": "PGWT_COV_COMPLETE", "live_edge": "PGWT_COV_LIVE_EDGE",
            "incomplete": "PGWT_COV_INCOMPLETE", "error": "PGWT_COV_ERROR"}
ERRORS = {"invalid_window": "PGWT_COV_E_INVALID_WINDOW"}
RESTART = {"daemon": "PGWT_COV_RESTART_DAEMON", "postgres": "PGWT_COV_RESTART_POSTGRES",
           "postgresql": "PGWT_COV_RESTART_POSTGRES", "unknown": "PGWT_COV_RESTART_UNKNOWN"}
NOW = 10000


def die(msg):
    sys.exit(f"gen_coverage_cases: {msg}")


def ns(value):
    v = Decimal(str(value)) * 1000000000
    if v != v.to_integral_value():
        die(f"time {value} is not a whole nanosecond")
    return int(v)


# --- record and boot builders (mono seconds) --------------------------------
def F(obs, no=0, readable=1):
    return ("PGWT_COV_REC_FILE", obs, no, readable)


def WM(w, fd=None, fi=None):
    fd = w if fd is None else fd
    return ("PGWT_COV_REC_WATERMARK", w, fd, fd if fi is None else fi)


def GO(start, gid=1):
    return ("PGWT_COV_REC_GAP_OPEN", start, 0, gid)


def GC(end, gid=1):
    return ("PGWT_COV_REC_GAP_CLOSE", end, 0, gid)


def SEAL(w):
    return ("PGWT_COV_REC_SEAL", w, 0, 0)


def BAD():
    return ("PGWT_COV_REC_BAD", 0, 0, 0)


def boot(start, recs, obs=None, pm=1, offset=0, closure_clean=None):
    if obs is None:   # the last COVERAGE record written
        obs = max([r[1] for r in recs if r[0] in ("PGWT_COV_REC_WATERMARK",
                                                   "PGWT_COV_REC_SEAL")] + [start])
    if closure_clean is None:
        closure_clean = any(r[0] == "PGWT_COV_REC_SEAL" for r in recs)
    return dict(start=start, recs=recs, obs=obs, pm=pm, offset=offset,
                clean=int(closure_clean))


def sealed(start, w, pm=1, offset=0):
    return boot(start, [F(start), WM(w), SEAL(w)], pm=pm, offset=offset)


def model(boots, legacy=(), now=NOW):
    return dict(boots=boots, legacy=list(legacy), now=now)


# row -> (setup text exactly as in the inputs file, model)
MODELS = {
    "1": ("B1[0,100,sealed]", model([sealed(0, 100)])),
    "2": ("B1[0,100,sealed], B2[130,200,sealed]",
          model([sealed(0, 100), sealed(130, 200)])),
    "3": ("B1[0,100,sealed], B2[100,200,sealed]",
          model([sealed(0, 100), sealed(100, 200)])),
    "4": ("as 2, but B2's postmaster start differs from B1's",
          model([sealed(0, 100), sealed(130, 200, pm=2)])),
    "5": ("B1[0,100,unsealed], last wm 100/95", model([boot(0, [F(0), WM(100, 95)])])),
    "6": ("B1[0,100,unsealed], last wm 100/100", model([boot(0, [F(0), WM(100, 100)])])),
    "7": ("B1 live [0,100], last wm 100/40, obs 100.5, now 101",
          model([boot(0, [F(0), WM(100, 40)], obs="100.5")], now=101)),
    "8": ("as 7, but now 110 and no record after 100.5",
          model([boot(0, [F(0), WM(100, 40)], obs="100.5")], now=110)),
    "9": ("B1 live [0,60], wm 60/55 repeated every tick, GO(60), obs 79.5, now 80",
          model([boot(0, [F(0), WM(60, 55), GO(60), WM(60, 55), WM(60, 55)], obs="79.5")],
                now=80)),
    "10": ("B1[0,200,sealed], wm 58/55, G(60,70)",
           model([boot(0, [F(0), WM(58, 55), GO(60), GC(70), WM(200), SEAL(200)])])),
    "11": ("B1[0,200,sealed], wm 78/75; W reached 150, and only then G(80,160) was appended",
           model([boot(0, [F(0), WM(78, 75), WM(150), GO(80), GC(160), WM(200), SEAL(200)])])),
    "12": ("B1[0,200,sealed], wm 59/55, GO(60)",
           model([boot(0, [F(0), WM(59, 55), GO(60), WM(200), SEAL(200)])])),
    "13": ("B1[0,1000,sealed]; the earliest retained file of B1 has BOOT obs 400",
           model([boot(0, [F(400, no=1), WM(1000), SEAL(1000)])])),
    "14": ("B1 sealed: f0 (BOOT obs 0, last wm 100/90), f1 missing, f2 (BOOT obs 200, SEAL 300)",
           model([boot(0, [F(0, 0), WM(100, 90), F(200, 2), WM(300), SEAL(300)])])),
    "15": ("only v4 trace files, data spanning [0,100)", model([], legacy=[(0, 100)])),
    "16": ("v4 trace data spanning [0,100); B2[120,200,sealed]",
           model([sealed(120, 200)], legacy=[(0, 100)])),
    "17": ("B1 in generation 0 (wall = mono + 1000), mono [0,100), sealed; B2 in generation 1 "
           "(wall = mono + 5000), mono [10,50), sealed. Windows are wall time",
           model([sealed(0, 100, offset=1000), sealed(10, 50, offset=5000)])),
    "18": ("B1[0,100]: SEAL present but header closure UNCLEAN, last wm 100/90",
           model([boot(0, [F(0), WM(100, 90), SEAL(100)], closure_clean=False)])),
    "19": ("B1 live [0,100], wm 97.5/97, G(98,99), last wm 100/100, obs 100.5, now 101",
           model([boot(0, [F(0), WM("97.5", 97), GO(98), GC(99), WM(100)], obs="100.5")],
                 now=101)),
    "20": ("master path: B1[0,100,sealed], wm 49/45; the counter vector changed on the next "
           "tick, writing G(49,50)",
           model([boot(0, [F(0), WM(49, 45), GO(49), GC(50), WM(100), SEAL(100)])])),
    "21": ("B1[0,100,sealed]; GO(0.5) written before any wm",
           model([boot(0, [F(0), GO("0.5"), WM(100), SEAL(100)])])),
    "22": ("B1[0,100,sealed]", model([sealed(0, 100)])),
    "23": ("B1[100,200,sealed] is the only boot; no legacy data", model([sealed(100, 200)])),
    "24": ("B1[0,100,sealed]; B2 live [130,200], last wm 200/180, obs 200.5, now 201",
           model([sealed(0, 100), boot(130, [F(130), WM(200, 180)], obs="200.5")], now=201)),
    "25": ("B1 sealed: f0 (BOOT obs 0, wm 80/75, then a record with a bad CRC), f1 (BOOT obs 150, "
           "SEAL 200)",
           model([boot(0, [F(0, 0), WM(80, 75), BAD(), F(150, 1), WM(200), SEAL(200)])])),
    "26": ("B1 live [0,60], wm 60/58 written every tick (W stalled), obs 99.5, now 100",
           model([boot(0, [F(0), WM(60, 58), WM(60, 58), WM(60, 58)], obs="99.5")], now=100)),
    "27": ("B1 sampled tier [0,100,unsealed], last wm 100/100",
           model([boot(0, [F(0), WM(100, 100)])])),
    "28": ("B1 tiered mode [0,100,unsealed], escalated at 40, last wm 100/70",
           model([boot(0, [F(0), WM(40), WM(100, 70)])])),
    "29": ("B1[0,100,unsealed], last wm 100/95/20", model([boot(0, [F(0), WM(100, 95, 20)])])),
    "30": ("B1 live [0,100], last wm 100/100, obs 100.5, now 101",
           model([boot(0, [F(0), WM(100, 100)], obs="100.5")], now=101)),
}

# A1 item 4: time order (restart, then capture_loss) differs from precedence.
EXTRA = [dict(
    id="X1", window="[50,180)", frm=50, to=180,
    setup="B1[0,100,sealed]; B2[130,200,sealed] with wm 158/155, G(160,170)",
    model=model([sealed(0, 100),
                 boot(130, [F(130), WM(158, 155), GO(160), GC(170), WM(200), SEAL(200)])]),
    db=dict(verdict="incomplete", reasons=["capture_loss", "restart"], xf=False,
            unc=[(100, 130, "restart"), (155, 170, "capture_loss")], covered_to=None,
            restart="daemon"),
)]

WINDOW = re.compile(r"^(?:([a-z]) )?\[([0-9.]+),([0-9.]+)\)$")
RANGE = re.compile(r"\bu[\[(]([0-9.]+),([0-9.]+)[)\]] ([a-z_]+)")
BARE = re.compile(r"\bu[\[(]([0-9.]+),([0-9.]+)[)\]](?! [a-z_])")


def parse_window(text):
    m = WINDOW.match(text.strip())
    if not m:
        die(f"unparsed window {text!r}")
    return (m.group(1) or ""), m.group(2), m.group(3)


def read_inputs(path):
    rows = {}
    for line in path.read_text().splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 3 or not cells[0].isdigit():
            continue
        windows = [parse_window(w) for w in cells[2].split("·")]
        rows[cells[0]] = (cells[1], windows)
    return rows


def parse_reasons(text):
    text = text.strip()
    if not (text.startswith("[") and text.endswith("]")):
        die(f"unparsed reasons {text!r}")
    return [r.strip() for r in text[1:-1].split(",") if r.strip()]


def ranges(text, only=None):
    """u[a,b) ranges; a range written without a reason takes `only`, the
    verdict's single listed reason (row 29's idle range)."""
    found = [(a, b, r) for a, b, r in RANGE.findall(text)]
    bare = BARE.findall(text)
    if len(found) + len(bare) != len(re.findall(r"\bu[\[(]", text)) or (bare and not only):
        die(f"a range without a reason in {text!r}")
    return found + [(a, b, only) for a, b in bare]


def read_answers(path):
    out = {}
    with path.open(newline="") as fh:
        reader = csv.reader(fh, delimiter="\t")
        if next(reader) != ["row", "window", "verdict", "reasons", "mark", "note"]:
            die("answers header changed")
        for cells in reader:
            if not cells:
                continue
            row, window, verdict, reasons, mark, note = cells
            label, frm, to = parse_window(window)
            db_note, _, idle_note = note.partition("idle verdict")
            m = re.search(r"covered_to ([0-9.]+)", db_note)
            rc = re.search(r"restart_cause ([a-z]+)", db_note)
            db = dict(verdict=verdict, reasons=parse_reasons(reasons), xf=mark == "xf",
                      unc=ranges(db_note), covered_to=m.group(1) if m else None,
                      restart=rc.group(1) if rc else None)
            if mark not in ("", "xf"):
                die(f"row {row}{label}: unknown mark {mark!r}")
            idle = None
            if idle_note:
                v = re.search(r"\b(complete|incomplete|live_edge)\b", idle_note)
                rs = re.search(r"\[[a-z_, ]+\]", idle_note)
                if not v or not rs:
                    die(f"row {row}{label}: unparsed idle verdict")
                ireasons = parse_reasons(rs.group(0))
                idle = dict(verdict=v.group(1), reasons=ireasons,
                            unc=ranges(idle_note, ireasons[0] if len(ireasons) == 1 else None))
            out[(row, label)] = (window, frm, to, db, idle)
    return out


def check_expect(cid, e):
    """Internal consistency, so a parse that drops a field cannot pass."""
    if e["verdict"] not in VERDICTS:
        die(f"{cid}: verdict {e['verdict']!r}")
    names = REASONS + (list(ERRORS) if e["verdict"] == "error" else [])
    if any(r not in names for r in e["reasons"]):
        die(f"{cid}: reasons {e['reasons']}")
    if (e["verdict"] in ("incomplete", "error")) != bool(e["reasons"]):
        die(f"{cid}: reasons {e['reasons']} for {e['verdict']}")
    if e["verdict"] == "incomplete":
        if not {r for _, _, r in e["unc"]} <= set(e["reasons"]):
            die(f"{cid}: range reasons outside {e['reasons']}")
        if not e["unc"] and e["reasons"] != ["restart"]:
            die(f"{cid}: incomplete with no range")
    elif e["unc"]:
        die(f"{cid}: ranges on a {e['verdict']} window")
    if "covered_to" in e and (e["verdict"] == "live_edge") != (e["covered_to"] is not None):
        die(f"{cid}: covered_to {e['covered_to']} for {e['verdict']}")
    if "restart" in e and ("restart" in e["reasons"]) != (e["restart"] is not None):
        die(f"{cid}: restart_cause {e['restart']} for {e['reasons']}")


def c_expect(e):
    err = "PGWT_COV_E_NONE"
    reasons = e["reasons"]
    if e["verdict"] == "error":
        err, reasons = ERRORS[reasons[0]], []
    rs = ", ".join(f"PGWT_COV_R_{r.upper()}" for r in reasons) or "0"
    us = ", ".join(f"{{{ns(a)}LL, {ns(b)}LL, PGWT_COV_R_{r.upper()}}}" for a, b, r in e["unc"]) \
        or "{0, 0, 0}"
    ct = f"{ns(e['covered_to'])}LL" if e.get("covered_to") is not None else "PGWT_COV_NONE"
    rc = RESTART[e["restart"]] if e.get("restart") else "PGWT_COV_RESTART_NONE"
    return (f"{{{VERDICTS[e['verdict']]}, {err}, {len(reasons)}, {{{rs}}}, {len(e['unc'])}, "
            f"{{{us}}}, {ct}, {rc}, {int(bool(e.get('xf')))}}}")


def c_model(name, m, out):
    for i, b in enumerate(m["boots"]):
        recs = ", ".join(f"{{{k}, {ns(a)}LL, {ns(bb) if k != 'PGWT_COV_REC_FILE' else bb}LL, "
                         f"{ns(c) if k == 'PGWT_COV_REC_WATERMARK' else c}LL}}"
                         for k, a, bb, c in b["recs"])
        out.append(f"static const struct pgwt_cov_rec {name}_r{i}[] = {{{recs}}};")
    if m["boots"]:
        boots = ",\n    ".join(
            f"{{{ns(b['start'])}LL, {ns(b['offset'])}LL, {b['pm']}u, {b['clean']}, 1, "
            f"PGWT_COV_STATUS_NONE, {ns(b['obs'])}LL, {name}_r{i}, {len(b['recs'])}}}"
            for i, b in enumerate(m["boots"]))
        out.append(f"static const struct pgwt_cov_boot {name}_b[] = {{\n    {boots}}};")
    if m["legacy"]:
        leg = ", ".join(f"{{{ns(a)}LL, {ns(b)}LL}}" for a, b in m["legacy"])
        out.append(f"static const struct pgwt_cov_range {name}_l[] = {{{leg}}};")
    out.append(f"static const struct pgwt_cov_model {name} = {{"
               f"{name + '_b' if m['boots'] else 'NULL'}, {len(m['boots'])}, "
               f"{name + '_l' if m['legacy'] else 'NULL'}, {len(m['legacy'])}}};")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--fixtures", default=str(FIXTURES),
                    help="directory holding the two files (drift controls use a copy)")
    args = ap.parse_args()
    fixtures = Path(args.fixtures)
    inputs = read_inputs(fixtures / INPUTS_NAME)
    answers = read_answers(fixtures / ANSWERS_NAME)
    if sorted(inputs, key=int) != sorted(MODELS, key=int):
        die(f"rows differ: inputs {sorted(inputs, key=int)} models {sorted(MODELS, key=int)}")
    out = ["/* Generated by tests/gen_coverage_cases.py from tests/fixtures/coverage/; "
           "do not edit. */"]
    cases, seen = [], set()
    for row in sorted(inputs, key=int):
        setup, windows = inputs[row]
        pinned, m = MODELS[row]
        if setup != pinned:
            die(f"row {row}: setup text changed; re-encode MODELS[{row!r}]:\n  {setup!r}")
        c_model(f"cov_m{row}", m, out)
        for label, frm, to in windows:
            key = (row, label)
            if key not in answers:
                die(f"row {row}{label}: no answer")
            window, afrm, ato, db, idle = answers[key]
            if (afrm, ato) != (frm, to):
                die(f"row {row}{label}: window {window} differs from inputs [{frm},{to})")
            seen.add(key)
            cid = row + label
            check_expect(cid, db)
            if idle is None:  # equal to the DB verdict whenever floor_idle = floor_db
                idle = dict(verdict=db["verdict"], reasons=db["reasons"], unc=db["unc"])
            check_expect(cid + " idle", idle)
            cases.append((cid, window, f"cov_m{row}", frm, to, m["now"], db, idle))
    if seen != set(answers):
        die(f"answers without an input window: {sorted(set(answers) - seen)}")
    table = len(cases)
    for x in EXTRA:
        name = "cov_m" + x["id"]
        c_model(name, x["model"], out)
        check_expect(x["id"], x["db"])
        e = x["db"]
        if [r for r in REASONS if r in e["reasons"]] != e["reasons"]:
            die(f"{x['id']}: expected reasons are not in precedence order")
        cases.append((x["id"], x["window"], name, x["frm"], x["to"], x["model"]["now"], e,
                      dict(verdict=e["verdict"], reasons=e["reasons"], unc=e["unc"])))
    out.append(f"#define COV_TABLE_WINDOWS {table}")
    out.append("static const struct cov_case cov_cases[] = {")
    for cid, window, name, frm, to, now, db, idle in cases:
        out.append(f"    {{\"{cid}\", \"{window}\", &{name}, {ns(frm)}LL, {ns(to)}LL, "
                   f"{ns(now)}LL,\n     {c_expect(db)},\n     {c_expect(idle)}}},")
    out.append("};")
    Path(args.output).write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
