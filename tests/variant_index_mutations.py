#!/usr/bin/env python3
"""Demonstrated red for tests/test_variant_index.c (paint-latency 3b).

src/variant_index.c is a NEW module, so "run the new test against the parent
tree" is not available: the parent tree has no module to test. The equivalent
evidence is this — every silent-wrong risk the index was written to avoid is
INJECTED into a copy of the module, one at a time, and the unit test must go
RED on it. A mutation that leaves the test GREEN is a blind spot in the test,
and this script reports it as a FAILURE.

Modelled on tests/exec_index_mutations.py (Phase 3, 16/16 detected).

WHY THE THREE OUTCOMES ARE NAMED SEPARATELY. This script's own false-negative
path is a mutation that never reached the binary:

  BUILD-FAILED  the mutant did not compile -> FAILURE (proves nothing)
  NOT-APPLIED   the needle matched 0 or >1 times -> FAILURE (did not apply)
  STAYED-GREEN  the mutant built, ran, and the test passed -> FAILURE (blind)

and a pristine baseline that is not green makes every "red" below meaningless,
so it is checked first and aborts the run.

Beyond the correctness mutations there is a GATE-BLINDING section: each entry
there makes the test's own detection unreachable rather than making the module
wrong — a refusal that approves, a ledger counter that cannot be driven, a
comparator field dropped. Those must go red too, because "the gate could not
see" is how a gate fails in practice.

Run from tests/:  python3 variant_index_mutations.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
# Mutant build trees are throwaway BINARIES: a temp dir, never tests/results
# (which box-check rsyncs and a reviewer reads).
WORK = os.path.join(tempfile.gettempdir(), "pgwt-variant-index-mutants")
# Only the text output of each mutant run is kept as evidence.
OUT = os.path.join(HERE, "results", "variant-index", "mutations")

MUTATIONS = [
    # ── the headline risk: a sequence truncated at a block boundary ──
    ("M01 no partial-sequence carry-over (open sequences reset each block)",
     "    int first_row = idx->n_rows;",
     "    for (int z = 0; z < idx->n_pids; z++)\n"
     "        for (int y = 0; y < 2; y++)\n"
     "            ((struct vi_pid_carry *)idx->pids)[z].ph[y].active = 0;\n"
     "    int first_row = idx->n_rows;"),

    ("M02 the partial sequence survives but its CONTENT is dropped at a "
     "block edge (truncation to the last block's steps only)",
     "    struct pgwt_variant_index_chunk *ch = &idx->chunks[idx->n_chunks++];",
     "    for (int z = 0; z < idx->n_pids; z++)\n"
     "        for (int y = 0; y < 2; y++) {\n"
     "            struct vi_phase_carry *zc =\n"
     "                &((struct vi_pid_carry *)idx->pids)[z].ph[y];\n"
     "            if (zc->active) { zc->exec.len = 0; zc->exec.total_ns = 0; }\n"
     "        }\n"
     "    struct pgwt_variant_index_chunk *ch = &idx->chunks[idx->n_chunks++];"),

    ("M03 a row is appended TWICE (duplication across blocks)",
     "    idx->n_steps += cp.num_steps;\n    idx->n_rows++;",
     "    idx->n_steps += cp.num_steps;\n    idx->n_rows++;\n"
     "    { struct pgwt_variant_index_entry dup = idx->rows[idx->n_rows - 1];\n"
     "      if (vi_grow_rows(idx) == 0) {\n"
     "          idx->rows[idx->n_rows] = dup; idx->n_rows++; } }"),

    # ── window semantics: selection is keyed and inclusive at BOTH ends ──
    ("M04 selection half-open at to_ns (an execution closing exactly at `to` "
     "is dropped)",
     "    return start_ns >= from_ns && close_ns <= to_ns;",
     "    return start_ns >= from_ns && close_ns < to_ns;"),

    ("M05 selection half-open at from_ns (an execution starting exactly at "
     "`from` is dropped)",
     "    return start_ns >= from_ns && close_ns <= to_ns;",
     "    return start_ns > from_ns && close_ns <= to_ns;"),

    ("M06 selection keyed on the CLOSE only, ignoring whether the opening "
     "marker was in the window",
     "    return start_ns >= from_ns && close_ns <= to_ns;",
     "    return close_ns >= from_ns && close_ns <= to_ns;"),

    ("M07 chunk prefilter narrowed, so a chunk that could contribute is "
     "skipped",
     "        if (ch->n_rows == 0 || ch->max_close_ns < from_ns ||\n"
     "            ch->min_start_ns > to_ns)",
     "        if (ch->n_rows == 0 || ch->max_close_ns <= from_ns ||\n"
     "            ch->min_start_ns >= to_ns)"),

    ("M08 prefilter bounds computed from the chunk's FIRST row only, so "
     "later rows in the same block are prefiltered away",
     "        ch->max_close_ns = 0;\n"
     "        for (int r = ch->first_row; r < ch->first_row + ch->n_rows; r++) {",
     "        ch->max_close_ns = 0;\n"
     "        for (int r = ch->first_row;\n"
     "             r < ch->first_row + (ch->n_rows ? 1 : 0); r++) {"),

    # ── unbalanced markers ──
    ("M09 #222: a second opening marker CONTINUES the first sequence instead "
     "of discarding it",
     "                if (pc->active)\n"
     "                    idx->n_discarded++;\n"
     "                memset(&pc->exec, 0, sizeof(pc->exec));\n"
     "                pc->active = 1;\n"
     "                pc->start_ns = ev->timestamp_ns;\n"
     "                pc->exec.query_id = ev->query_id;",
     "                if (pc->active) {\n"
     "                    idx->n_discarded++;\n"
     "                } else {\n"
     "                    memset(&pc->exec, 0, sizeof(pc->exec));\n"
     "                    pc->start_ns = ev->timestamp_ns;\n"
     "                    pc->exec.query_id = ev->query_id;\n"
     "                }\n"
     "                pc->active = 1;"),

    ("M10 start_ns kept STALE across executions of the same pid (the first "
     "opening marker's timestamp wins forever)",
     "                pc->start_ns = ev->timestamp_ns;\n"
     "                pc->exec.query_id = ev->query_id;",
     "                if (!pc->start_ns)\n"
     "                    pc->start_ns = ev->timestamp_ns;\n"
     "                pc->exec.query_id = ev->query_id;"),

    ("M11 a closing marker with nothing open emits a row anyway",
     "            if (ev->old_event == vi_marker_end[phase] && pc->active) {",
     "            if (ev->old_event == vi_marker_end[phase]) {"),

    ("M12 an execution still open at a BLOCK EDGE is closed and emitted as "
     "a truncated row instead of being carried over",
     "    idx->last_event_ns = last_ts;",
     "    for (int z = 0; z < idx->n_pids; z++)\n"
     "        for (int y = 0; y < 2; y++) {\n"
     "            struct vi_pid_carry *zz =\n"
     "                &((struct vi_pid_carry *)idx->pids)[z];\n"
     "            if (zz->ph[y].active)\n"
     "                vi_close_row(idx, &zz->ph[y], zz->pid, y, last_ts);\n"
     "        }\n"
     "    idx->last_event_ns = last_ts;"),

    # ── sequence identity and ordering ──
    ("M13 step ORDER folded out of the hash (so [A,B] and [B,A] collide)",
     "    for (int i = 0; i < p->num_steps; i++) {\n"
     "        h ^= p->steps[i];\n"
     "        h *= 0x100000001b3ULL;",
     "    uint64_t acc = 0;\n"
     "    for (int i = 0; i < p->num_steps; i++) acc += p->steps[i];\n"
     "    for (int i = 0; i < (p->num_steps ? 1 : 0); i++) {\n"
     "        h ^= acc;\n"
     "        h *= 0x100000001b3ULL;"),

    ("M14 the loop flag is not folded into the hash",
     "        if (p->is_loop[i]) {\n"
     "            h ^= 0xDEADBEEF;\n"
     "            h *= 0x100000001b3ULL;\n"
     "        }",
     "        /* mutant: loop flag not hashed */"),

    ("M15 rows merged in ROW order per chunk but chunks visited backwards "
     "(close order broken: collision chains, query_id sets, p95 sample)",
     "    for (int c = 0; c < idx->n_chunks && !alloc_failed; c++) {\n"
     "        const struct pgwt_variant_index_chunk *ch = &idx->chunks[c];",
     "    for (int c = idx->n_chunks - 1; c >= 0 && !alloc_failed; c--) {\n"
     "        const struct pgwt_variant_index_chunk *ch = &idx->chunks[c];"),

    ("M16 idle records enter the sequence",
     "                if (pgwt_is_idle_event(ev->old_event))\n"
     "                    continue;",
     "                /* mutant: idle records kept */"),

    ("M17 the all-zero filter treated as a no-op (an impossible duration "
     "enters the sequence)",
     "                if (ev->duration_ns > ev->timestamp_ns)\n"
     "                    continue;",
     "                /* mutant: impossible durations kept */"),

    ("M18 the raw cap set to 3, truncating longer sequences",
     "            if (pc->active && pc->exec.len < PGWT_VARIANT_INDEX_MAX_RAW) {",
     "            if (pc->active && pc->exec.len < 3) {"),

    ("M19 the CPU-only fill-in moved AFTER loop_n would see it (raw_len 0)",
     "    if (re->len == 0) {\n"
     "        re->events[0] = 0;\n"
     "        re->durations[0] = re->total_ns;\n"
     "        re->len = 1;\n"
     "    }",
     "    /* mutant: no CPU-only fill-in */"),

    ("M20 per-step accumulators not clamped to the last step (the oracle's "
     "si quirk 'fixed')",
     "            if (si >= cp.num_steps) si = cp.num_steps - 1;",
     "            /* mutant: no clamp */"),

    ("M21 query_id taken from the opening marker only, never from an inner "
     "record",
     "                if (ev->query_id)\n                    re->query_id = ev->query_id;",
     "                /* mutant: inner query_id ignored */"),

    ("M22 avg_loop_n pre-aggregated per variant instead of summed per "
     "execution in close order",
     "            va->loop_n_sum += loop_n;\n            va->loop_n_count++;",
     "            va->loop_n_sum = loop_n * (va->loop_n_count + 1);\n"
     "            va->loop_n_count++;"),

    ("M23 CMD_END closes an exec here (it does not, for variants)",
     "            if (PGWT_IS_MARKER(ev->old_event) || PGWT_IS_MARKER(ev->new_event))\n"
     "                continue;",
     "            if (ev->old_event == PGWT_MARKER_CMD_END && pc->active) {\n"
     "                if (vi_close_row(idx, pc, ev->pid, phase,\n"
     "                                 ev->timestamp_ns) != 0)\n"
     "                    return -1;\n"
     "                continue;\n"
     "            }\n"
     "            if (PGWT_IS_MARKER(ev->old_event) || PGWT_IS_MARKER(ev->new_event))\n"
     "                continue;"),

    ("M24 phases conflated (an exec row answers a plan query)",
     "            if (e->phase != (uint8_t)phase)\n                continue;",
     "            /* mutant: phase ignored */"),

    ("M25 max_variants truncation dropped",
     "    int nr = vi < max_variants ? vi : max_variants;",
     "    int nr = vi;"),

    # ── GATE-BLINDING: the gate cannot see rather than the module being wrong ──
    ("G01 an empty index answers 'no variants' instead of refusing",
     "    if (idx->n_chunks == 0)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_EMPTY);",
     "    if (idx->n_chunks == 0) {\n"
     "        out->refused = PGWT_VARIANT_INDEX_OK;\n"
     "        out->sequences_indexed = 1;\n"
     "        return 0;\n"
     "    }"),

    ("G02 coverage check is an INTERSECTION, not containment",
     "    if (from_ns < idx->cover_from_ns || to_ns > idx->cover_to_ns)",
     "    if (to_ns < idx->cover_from_ns || from_ns > idx->cover_to_ns)"),

    ("G03 a refusal leaves the counts at 0 instead of poisoning them",
     "    out->res.num_variants = PGWT_VARIANT_INDEX_NOT_INDEXED;\n"
     "    out->res.total_executions = PGWT_VARIANT_INDEX_NOT_INDEXED;\n"
     "    out->res.failed = 1;",
     "    out->res.num_variants = 0;\n"
     "    out->res.total_executions = 0;\n"
     "    out->res.failed = 0;"),

    ("G04 a refusal poisons the counts but does NOT set failed/indexed (the "
     "'-1 is truthy' trap: the caller sails past it)",
     "    out->res.failed = 1;\n    out->sequences_indexed = 0;",
     "    out->res.failed = 0;\n    out->sequences_indexed = 1;"),

    ("G05 a build allocation failure does not make the index unusable",
     "    idx->failed = 1;\n"
     "    if (idx->build_refusal == PGWT_VARIANT_INDEX_OK)",
     "    idx->failed = 0;\n"
     "    if (idx->build_refusal == PGWT_VARIANT_INDEX_OK)"),

    ("G06 a skipped block in the feed is accepted",
     "    if (block_idx != idx->next_block_idx)\n"
     "        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);",
     "    if (block_idx < 0)\n"
     "        return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);"),

    ("G07 out-of-order records are accepted silently",
     "        if (have_last && ev->timestamp_ns < last_ts)\n"
     "            return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);",
     "        if (have_last && ev->timestamp_ns + 1 == 0)\n"
     "            return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);"),

    ("G08 a record outside its block's bounds is clamped, not refused",
     "        if (ev->timestamp_ns < block_first_ns || ev->timestamp_ns > block_last_ns)\n"
     "            return vi_fail_build(idx, PGWT_VARIANT_INDEX_REFUSE_GAP);",
     "        /* mutant: out-of-bounds records accepted */"),

    ("G09 the version stamp is not checked on query",
     "    if (idx->version != PGWT_VARIANT_INDEX_VERSION)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_VERSION);",
     "    if (idx->version == 0)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_VERSION);"),

    ("G10 an unsealed index is queried anyway (chunk bounds still UINT64_MAX/0)",
     "    if (!idx->sealed)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_UNSEALED);",
     "    if (0)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_UNSEALED);"),

    ("G11 a NON-EMPTY filter is answered from the unfiltered index",
     "    if (!pgwt_variant_index_filter_ok(f))\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_FILTER);",
     "    if (0)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_FILTER);"),

    ("G12 a NULL filter is accepted as 'no filter'",
     "    if (!f)\n"
     "        return 0;   /* NULL means \"no filter at all\" to the oracle, which",
     "    if (!f)\n"
     "        return 1;   /* NULL means \"no filter at all\" to the oracle, which"),

    ("G13 an inverted window is answered instead of refused",
     "    if (from_ns > to_ns)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_BAD_WINDOW);",
     "    if (0)\n"
     "        return vi_query_refuse(out, PGWT_VARIANT_INDEX_REFUSE_BAD_WINDOW);"),

    ("G14 the ledger's carry-over counter cannot be driven: n_discarded is "
     "never incremented, so the #222 shape cannot be proven to have occurred",
     "                if (pc->active)\n                    idx->n_discarded++;",
     "                /* mutant: discards not counted */"),

    ("G15 the measured-work counters stop counting, so the complexity claim "
     "becomes vacuously true",
     "            out->rows_examined++;",
     "            /* mutant: work not counted */"),

    ("G16 open_at_end always reports 0, so 'no sequence was left open' cannot "
     "be distinguished from 'the counter is broken'",
     "    return vi_open_at_end(idx);",
     "    return 0;"),
]


def mac_stub(path):
    with open(path, "w") as fh:
        fh.write("#include <stdio.h>\n"
                 "FILE *pgwt_proc_open(char *const argv[], int *pid);\n"
                 "int pgwt_proc_close(FILE *fp, int pid);\n"
                 "FILE *pgwt_proc_open(char *const a[], int *p)"
                 " { (void)a; (void)p; return NULL; }\n"
                 "int pgwt_proc_close(FILE *f, int p)"
                 " { (void)f; (void)p; return -1; }\n")


def build(workdir, binary, extra_sources):
    cmd = ["cc", "-g", "-O1", "-w",
           "-I", workdir, "-I", SRC, "-DPGWT_SERVER",
           "-o", binary,
           os.path.join(HERE, "test_variant_index.c"),
           os.path.join(workdir, "variant_index.c"),
           os.path.join(SRC, "compute.c"),
           os.path.join(SRC, "wait_event.c"),
           os.path.join(SRC, "idle_rule.c"),
           os.path.join(SRC, "cJSON.c")] + extra_sources + ["-lm"]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    os.makedirs(OUT, exist_ok=True)

    # src/spawn.c uses pipe2(), which Darwin lacks; wait_event.c needs its two
    # symbols. Linux links the real file, macOS a stub. Neither path SKIPS: a
    # build failure below is a FAILURE, because a mutation harness that could
    # not build proves nothing.
    if sys.platform == "darwin":
        stub = os.path.join(WORK, "mac_stub.c")
        mac_stub(stub)
        extra = [stub]
    else:
        extra = [os.path.join(SRC, "spawn.c")]

    pristine = {}
    for name in ("variant_index.c", "variant_index.h"):
        with open(os.path.join(SRC, name)) as fh:
            pristine[name] = fh.read()

    failures = 0

    # 0. The baseline MUST be green, or every "red" below proves nothing.
    base = os.path.join(WORK, "base")
    os.makedirs(base)
    for n, t in pristine.items():
        with open(os.path.join(base, n), "w") as fh:
            fh.write(t)
    b = build(base, os.path.join(base, "t"), extra)
    if b.returncode != 0:
        print("FAIL: pristine build failed\n%s" % b.stderr[-2000:])
        return 1
    r = subprocess.run([os.path.join(base, "t")], capture_output=True,
                       text=True, timeout=1800)
    if r.returncode != 0:
        print("FAIL: pristine test is NOT green — nothing below is evidence")
        print(r.stdout[-3000:])
        return 1
    lines = [l for l in r.stdout.strip().splitlines() if l.strip()]
    print("baseline: pristine module, test PASSED (%s)" % lines[-2])
    print()

    for i, (name, needle, repl) in enumerate(MUTATIONS, 1):
        tag = "m%02d" % i
        d = os.path.join(WORK, tag)
        os.makedirs(d)
        for n, t in pristine.items():
            with open(os.path.join(d, n), "w") as fh:
                fh.write(t)

        target = os.path.join(d, "variant_index.c")
        with open(target) as fh:
            text = fh.read()
        hits = text.count(needle)
        if hits != 1:
            print("NOT-APPLIED  %s: needle matched %d times, must match "
                  "exactly 1 (this script cannot see)" % (name, hits))
            failures += 1
            continue
        with open(target, "w") as fh:
            fh.write(text.replace(needle, repl))

        binary = os.path.join(d, "t")
        b = build(d, binary, extra)
        if b.returncode != 0:
            print("BUILD-FAILED  %s: a mutation that cannot build is not "
                  "evidence\n%s" % (name, b.stderr[-800:]))
            failures += 1
            continue
        try:
            r = subprocess.run([binary], capture_output=True, text=True,
                               timeout=1800)
        except subprocess.TimeoutExpired:
            print("BUILD-FAILED  %s: mutant HUNG (a gate that stalls is not a "
                  "gate that fails)" % name)
            failures += 1
            continue
        with open(os.path.join(OUT, "%s.txt" % tag), "w") as fh:
            fh.write("%s\n\n%s" % (name, r.stdout))
        fails = [l.strip() for l in r.stdout.splitlines()
                 if l.strip().startswith("FAIL")]
        tail = re.findall(r"^(\d+) checks, (\d+) failed", r.stdout, re.M)
        nfail = int(tail[0][1]) if tail else -1
        if r.returncode == 0:
            print("STAYED-GREEN  %s: BLIND SPOT" % name)
            failures += 1
        else:
            first = fails[0][:160] if fails else "(crash/abort)"
            print("RED   %s\n        -> %d failing checks; first: %s"
                  % (name, nfail, first))

    print()
    shutil.rmtree(WORK, ignore_errors=True)
    print("%d mutations, %d not detected" % (len(MUTATIONS), failures))
    print("FAILED" if failures else "PASSED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
