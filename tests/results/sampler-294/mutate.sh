#!/bin/bash
# Prove each new gate can go RED. Each mutation is applied, the test run, the
# tree restored. A mutation that leaves the suite GREEN is reported as such --
# that is a blind gate, not a pass.
set -u
cd /root/pgwt-check/agent_sampler-cmd-gate-order || exit 1
L=src/backend_status_layout.c
S=src/sampler.c
cp $L $L.orig; cp $S $S.orig
restore() { cp $L.orig $L; cp $S.orig $S; }

run() { # name binary
  make -C tests "$2" >/dev/null 2>&1 || { echo "MUT $1: BUILD FAILED (red: gate unbuildable)"; return; }
  out=$(./tests/"$2" 2>&1)
  rc=$?
  n=$(printf '%s\n' "$out" | grep -c '  FAIL:')
  echo "MUT $1: rc=$rc failures=$n"
  printf '%s\n' "$out" | grep '  FAIL:' | head -4
}

echo "=== M1 second definition of in-a-command: 'not idle' instead of RUNNING/FASTPATH ==="
python3 - <<'PY'
p='src/backend_status_layout.c'; s=open(p).read()
s=s.replace("""    out->cmd_open = pgwt_pgbs_state_is_cmd_open(layout, snapshot->state);
    if (pgwt_pgbs_sampled_query_id_enabled(layout) &&""","""    out->cmd_open = snapshot->state != 1;   /* MUTANT: a second definition */
    if (pgwt_pgbs_sampled_query_id_enabled(layout) &&""",1)
open(p,'w').write(s)
PY
run M1 test_backend_status_layout; restore

echo "=== M2 always-closed predicate (the 'both agree because both are false' trap) ==="
python3 - <<'PY'
p='src/backend_status_layout.c'; s=open(p).read()
s=s.replace("""    return state == (uint32_t)layout->state_running ||
           state == (uint32_t)layout->state_fastpath;""","""    (void)state; return false;   /* MUTANT: nothing is ever in a command */""",1)
open(p,'w').write(s)
PY
run M2 test_backend_status_layout; restore

echo "=== M3 live gate re-read approves when it cannot see (no layout check) ==="
python3 - <<'PY'
p='src/backend_status_layout.c'; s=open(p).read()
s=s.replace("""    if (!pgwt_pgbs_sampled_attr_enabled(layout) || backend_pid <= 0)
        return -1;
    if (backend_status_addr == 0 && my_be_entry_addr == 0)
        return -1;""","""    if (backend_pid <= 0) return -1;
    if (backend_status_addr == 0 && my_be_entry_addr == 0) {
        out->cmd_open = true; return 0;   /* MUTANT: blind -> approve */
    }""",1)
open(p,'w').write(s)
PY
run M3 test_backend_status_layout; restore

echo "=== M4 a FAILED fresh read fabricates an open gate ==="
python3 - <<'PY'
p='src/sampler.c'; s=open(p).read()
s=s.replace("""                local.read_failed++;
                continue;             /* never fabricate an open gate */""","""                local.read_failed++;
                targets[i].cmd_open = 1;   /* MUTANT: fail-open */
                continue;""",1)
open(p,'w').write(s)
PY
run M4 test_sampler; restore

echo "=== M5 at-risk set widened to every client (recovers samples never dropped) ==="
python3 - <<'PY'
p='src/sampler.c'; s=open(p).read()
s=s.replace("""            if (pgwt_cpu_sample_recordable(targets[i].backend_type,
                                           targets[i].cmd_open))
                continue;             /* already admitted — nothing at risk */""","""            /* MUTANT: no 'about to be dropped' test at all */""",1)
open(p,'w').write(s)
PY
run M5 test_sampler; restore

echo "=== M6 recheck decides on UNREAD values (the pre-batch placement) ==="
python3 - <<'PY'
p='src/sampler.c'; s=open(p).read()
s=s.replace("""            if (!valid[i])
                continue;             /* unread: not an on-CPU observation */""","""            /* MUTANT: an unread target counts as on-CPU */""",1)
open(p,'w').write(s)
PY
run M6 test_sampler; restore

echo "=== M7 recheck with no fresh-read function silently approves ==="
python3 - <<'PY'
p='src/sampler.c'; s=open(p).read()
s=s.replace("""    if (targets && vals && valid && read_gate && n > 0) {""","""    if (targets && vals && valid && n > 0) {
        if (!read_gate) { for (int i = 0; i < n; i++) targets[i].cmd_open = 1;
                          if (stats) *stats = local; return n; }   /* MUTANT */""",1)
open(p,'w').write(s)
PY
run M7 test_sampler; restore

echo "=== M8 recovered sample loses its query id (silently unattributed) ==="
python3 - <<'PY'
p='src/sampler.c'; s=open(p).read()
s=s.replace("""                targets[i].query_id = qid;
                if (qid != 0)
                    targets[i].query_quality = PGWT_QUERY_QUALITY_REAL;""","""                /* MUTANT: drop the id that came with the gate */""",1)
open(p,'w').write(s)
PY
run M8 test_sampler; restore

echo "=== M9 conservation: an at-risk target counted in two outcomes ==="
python3 - <<'PY'
p='src/sampler.c'; s=open(p).read()
s=s.replace("""            local.recovered++;
            recovered++;""","""            local.recovered++;
            local.confirmed_closed++;   /* MUTANT: double-counted */
            recovered++;""",1)
open(p,'w').write(s)
PY
run M9 test_sampler; restore

echo "=== restored; confirming green ==="
run BASE test_sampler
run BASE test_backend_status_layout
rm -f $L.orig $S.orig
