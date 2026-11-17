#!/usr/bin/env python3
"""Coverage verdict mutants and fixture-drift controls (U2a; Refs #947, #871).

Each mutant is one edit of src/coverage.c, built in a temporary directory
with the real differential test; a kill needs a successful build, a failing
run and a FAIL line on the named window. The tree is never edited. CC and
CFLAGS come from the environment (an ASan/UBSan run passes them there).

Not a mutant here: C5 (W = the previous tick's publish_from) is a writer
rule. The verdict reads W from WATERMARK records and never sees a
publish_from, so the pure function has no line a C5 mutant could change; it
belongs to U1's writer test ("W lag of one tick").
"""
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SOURCE = ROOT / "src" / "coverage.c"
FIXTURES = HERE / "fixtures" / "coverage"

XF_GUARD = ("    if ((out->verdict == PGWT_COV_COMPLETE || out->verdict == PGWT_COV_LIVE_EDGE)"
            " && xb >= 0) {")
PRECEDENCE = ("        for (int r = 0; r < PGWT_COV_N_REASONS; r++)\n"
              "            if (mask & (1u << r))\n"
              "                out->reasons[out->n_reasons++] = r;\n")
TIME_ORDER = ("        for (size_t k = 0; k < out->n_uncovered; k++) {\n"
              "            int r = out->uncovered[k].reason;\n"
              "            if (mask & (1u << r)) {\n"
              "                mask &= ~(1u << r);\n"
              "                out->reasons[out->n_reasons++] = r;\n"
              "            }\n"
              "        }\n" + PRECEDENCE)
LEGACY_MEETS = ("        for (size_t i = 0; i < m->n_legacy; i++)\n"
                "            if (meets(m->legacy[i].from, m->legacy[i].to, from, to))\n"
                "                met++;\n")
APPEND_ORDER = "        while (j < ngo && go[j].start < r->a)\n"

# name, [(anchor, replacement)], window (or tuple of windows) that must fail
MUTANTS = [
    # The brief's list.
    ("c1-drop-open-floor", [("        s->E = s->fk;\n", "        s->E = s->W;\n")], "5b"),
    ("c2-hole-without-floor",
     [("    return w->have_w ? floor_of(w->kind, w->fd, w->fi) : w->b->boot_start;",
       "    return w->have_w ? w->w : w->b->boot_start;")], "14a"),
    ("a1.1-xf-on-incomplete", [(XF_GUARD, "    if (xb >= 0) {")], "7c"),
    ("a1.3-legacy-file-is-a-boot",
     [("        if (met >= 2) {\n", LEGACY_MEETS + "        if (met >= 2) {\n")], "16"),
    ("a1.4-time-ordered-reasons", [(PRECEDENCE, TIME_ORDER)], "X1"),
    # Spec r2 §5 U2a's list, where not already above.
    ("one-boot-rule-dropped", [("        if (met >= 2) {\n", "        if (met >= 2 && 0) {\n")], "3"),
    ("taint-ignored", [("#define COVER_EXCLUDES (BAD_TAINT | BAD_HOLE)",
                        "#define COVER_EXCLUDES (BAD_HOLE)")], "10c"),
    ("stale-is-live", [("              now - wall(b, b->last_obs) <= PGWT_COV_LIVE_L_NS;",
                        "              (now - wall(b, b->last_obs) <= PGWT_COV_LIVE_L_NS || 1);")],
     "8"),
    ("taint-from-append-order",
     [(APPEND_ORDER, APPEND_ORDER.replace("go[j].start < r->a", "go[j].pos < k"))], "11"),
    ("no-xf-when-live", [("        out->excludes_in_flight = s.live && s.fd < to;",
                          "        out->excludes_in_flight = 0;")], "7a"),
    ("floor-db-for-idle", [("    return kind == PGWT_COV_FLOOR_IDLE ? fi : fd;",
                            "    (void)kind;\n    (void)fi;\n    return fd;")], "29"),
    ("liveness-from-w-progress", [("now - wall(b, b->last_obs)", "now - wall(b, w.w)")], "26b"),
    ("has-exact-on-overlap", [("p->S <= from && to <= p->E &&", "meets(p->S, p->E, from, to) &&")],
     "2a"),
    # Rules the agreed table cannot reach (review 9d8bfac0 finding 1, nit 4,
    # finding 3 / A2 item 1). Each case lives in test_coverage_verdict.c's
    # rule_cases, outside the table; every refusal mutated here would
    # otherwise turn an incomplete window complete.
    ("gap-id-ignored", [("static int64_t pair_key(int64_t id) { return id; }",
                         "static int64_t pair_key(int64_t id) { (void)id; return 0; }")],
     "G1-gap-id"),
    ("gap-close-before-open-pairs",
     [("static int closes_after(size_t open_pos, size_t close_pos) { return close_pos > open_pos; }",
       "static int closes_after(size_t open_pos, size_t close_pos) "
       "{ (void)open_pos; (void)close_pos; return 1; }")], "G2-id-reused"),
    ("kernel-boot-ignored", [("    s->live = !s->sealed && b->same_kernel_boot &&\n",
                              "    s->live = !s->sealed &&\n")], "L1-other-kernel"),
    ("status-mismatch-ignored", [("              b->status != PGWT_COV_STATUS_MISMATCH &&\n", "")],
     "L2-status-mismatch"),
    ("unreadable-file-not-lost", [("int lost = !r->c || (", "int lost = (")], "H1-unreadable"),
    ("hole-closed-by-unreadable", [("if (r->c && w->hole_open) {", "if (w->hole_open) {")],
     "H2-unreadable-no-close"),
    ("nonmonotone-w-accepted",
     [("(r->kind == PGWT_COV_REC_WATERMARK && w->have_w && r->a < w->w)", "0")],
     "H3-nonmonotone-w"),
    ("restart-unknown-is-daemon", [("        return PGWT_COV_RESTART_UNKNOWN;",
                                    "        return PGWT_COV_RESTART_DAEMON;")], "C1-cause-unknown"),
    ("clock-step-dropped", [("    n_ov = overlaps(ws.sums, m->n_boots, ws.ev, ws.ov);",
                             "    n_ov = 0 * overlaps(ws.sums, m->n_boots, ws.ev, ws.ov);")],
     "S1-clock-step-inside"),
    # Rules round 2's review (655c3242) found unreached: findings 1-3.
    ("live-edge-exclusions-dropped",
     [("            to - p->W <= PGWT_COV_LIVE_L_NS && clean(&ws, p, n_ov, from, p->W)) {",
       "            to - p->W <= PGWT_COV_LIVE_L_NS && 1) {")],
     ("E1-live-edge-taint", "E2-live-edge-hole")),
    ("scratch-misalignment-accepted",
     [("        (off[7] && (!p || (uintptr_t)p % WS_ALIGN)))", "        (off[7] && !p))")],
     "api-misaligned"),
    ("unclosed-gap-takes-other-id-close",
     [("        go[j].hi = c < ngc && gc[c].key == go[j].key ? gc[c].end : PGWT_COV_INF;",
       "        go[j].hi = c < ngc ? gc[c].end : PGWT_COV_INF;")], "G4-other-id-close"),
]


def generate(tmp, fixtures):
    out = Path(tmp) / "coverage_cases.h"
    r = subprocess.run([sys.executable, "-B", str(HERE / "gen_coverage_cases.py"),
                        "--output", str(out), "--fixtures", str(fixtures)],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def build(tmp, name, source):
    src = Path(tmp) / f"{name}.c"
    src.write_text(source)
    binary = Path(tmp) / name
    cmd = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-g", "-O2", "-Wall",
           "-Wextra", "-Werror", *shlex.split(os.environ.get("CFLAGS", "")),
           "-I", str(ROOT / "src"), "-I", str(tmp), str(HERE / "test_coverage_verdict.c"),
           str(src), "-o", str(binary)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{name}: did not build:\n{r.stdout}{r.stderr}")
    return binary


def run(binary):
    r = subprocess.run([str(binary)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def failed_ids(output):
    return sorted({line.split()[1] for line in output.splitlines() if line.startswith("FAIL ")})


def mutate(source, edits):
    for before, after in edits:
        if source.count(before) != 1:
            raise SystemExit(f"stale mutation anchor: {before!r}")
        source = source.replace(before, after)
    return source


def drift(tmp, name, file_name, before, after):
    d = Path(tmp) / name
    shutil.copytree(FIXTURES, d)
    text = (d / file_name).read_text()
    if text.count(before) != 1:
        raise SystemExit(f"stale drift anchor: {before!r}")
    (d / file_name).write_text(text.replace(before, after))
    return d


def main():
    original = SOURCE.read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-coverage-mutant-") as tmp:
        rc, out = generate(tmp, FIXTURES)
        if rc:
            raise SystemExit(f"generation failed:\n{out}")
        rc, out = run(build(tmp, "baseline", original))
        if rc:
            raise SystemExit(f"baseline is not green:\n{out}")
        print("baseline: green")
        for name, edits, window in MUTANTS:
            rc, out = run(build(tmp, "mutant", mutate(original, edits)))
            ids = failed_ids(out)
            wanted = window if isinstance(window, tuple) else (window,)
            if rc == 0 or any(w not in ids for w in wanted):
                raise SystemExit(f"SURVIVED {name}: wanted FAIL {' '.join(wanted)}, rc={rc}\n{out}")
            print(f"KILLED {name}: {' '.join(wanted)} (failing windows: {' '.join(ids)})")
        print("NOT-A-MUTANT c5-this-tick-publish-from: writer rule (U1); the verdict never "
              "sees publish_from")

        # Drift controls: the table must follow the fixture files.
        d = drift(tmp, "defaults", "coverage-table-inputs.md", "L = 3, now = 10000,",
                  "L = 30, now = 20000,")
        rc, out = generate(d, d)
        if rc == 0 or "defaults line changed" not in out:
            raise SystemExit(f"defaults drift was not refused: {out}")
        print("REFUSED defaults-line drift (now, L)")
        d = drift(tmp, "setup", "coverage-table-inputs.md",
                  "| 5 | B1[0,100,unsealed], last wm 100/95 |",
                  "| 5 | B1[0,100,unsealed], last wm 100/96 |")
        rc, out = generate(d, d)
        if rc == 0 or "setup text changed" not in out:
            raise SystemExit(f"setup drift was not refused: {out}")
        print("REFUSED setup-text drift (row 5)")
        d = drift(tmp, "window", "coverage-table-inputs.md",
                  "| a [10,50) · b [10,100) ·", "| a [10,51) · b [10,100) ·")
        rc, out = generate(d, d)
        if rc == 0 or "differs from inputs" not in out:
            raise SystemExit(f"window drift was not refused: {out}")
        print("REFUSED window drift (row 1a)")
        d = drift(tmp, "answer", "coverage-answers-final.tsv",
                  "7\ta [50,100)\tcomplete\t[]\txf\t", "7\ta [50,100)\tcomplete\t[]\t\t")
        rc, out = generate(d, d)
        if rc:
            raise SystemExit(f"answer drift did not generate: {out}")
        rc, out = run(build(d, "answer", original))
        if rc == 0 or "7a" not in failed_ids(out):
            raise SystemExit(f"answer drift did not turn the test red: {out}")
        print("RED answer drift (7a mark removed)")
    print(f"coverage verdict mutants: {len(MUTANTS)} killed, 4 drift controls: PASS")


if __name__ == "__main__":
    main()
