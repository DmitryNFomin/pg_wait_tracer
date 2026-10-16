#!/usr/bin/env bash
# Bypass cases for the allowlist, run against a synthetic repo whose HEAD is
# the integration tip's tree (53218a1) and whose base is 304a772's.
R=/tmp/tipsim/repo
G=/Users/dmitryfomin/work/git/pg_wait_tracer/.claude/worktrees/agent-a283b5c4ab073a48e/scripts/check-link-closure.sh
BASE=a010835
cd "$R" || exit 2
cp scripts/link-closure-allow.txt /tmp/allow.bak
cp tests/Makefile /tmp/mk.bak

run() {
    local out rc
    out=$(ROOT="$R" ALLOW=scripts/link-closure-allow.txt "$G" "$BASE" 2>&1)
    rc=$?
    echo "$out" | tail -3 | sed 's/^/    /'
    echo "    exit=$rc"
}

echo "=== BASELINE (tip tree, entry present, Makefile intact) ==="; run
echo "=== (a) the allowlist entry removed ==="
grep -v '^pgwt_summary_note_open_ex' /tmp/allow.bak > scripts/link-closure-allow.txt; run
echo "=== (b) the entry names a provider those targets do NOT link ==="
sed 's/^pgwt_summary_note_open_ex  summary_writer/pgwt_summary_note_open_ex  exact_probe/' \
    /tmp/allow.bak > scripts/link-closure-allow.txt; run
cp /tmp/allow.bak scripts/link-closure-allow.txt
echo "=== (c) a target stops linking summary_writer.o (entry still there) ==="
sed 's|$(BUILD)/map_reader.o $(BUILD)/summary_writer.o|$(BUILD)/map_reader.o|' \
    /tmp/mk.bak > tests/Makefile; run
cp /tmp/mk.bak tests/Makefile
echo "=== (d) the allowlist file does not exist ==="
out=$(ROOT="$R" ALLOW=scripts/nonexistent.txt "$G" "$BASE" 2>&1); rc=$?
echo "$out" | tail -3 | sed 's/^/    /'; echo "    exit=$rc"
echo "=== (e) tests/Makefile missing entirely ==="
mv tests/Makefile /tmp/mk.hidden
out=$(ROOT="$R" ALLOW=scripts/link-closure-allow.txt "$G" "$BASE" 2>&1); rc=$?
echo "$out" | tail -2 | sed 's/^/    /'; echo "    exit=$rc"
mv /tmp/mk.hidden tests/Makefile
echo "=== RESTORED ==="; run
echo "=== (f) the -DPGWT_SERVER targets must NOT be implicated ==="
out=$(ROOT="$R" ALLOW=scripts/link-closure-allow.txt "$G" "$BASE" 2>&1)
if grep -q 'test_replay_fidelity\|test_replay_window\|map_reader_core' <<<"$out"; then
    echo "    FAIL: a -DPGWT_SERVER consumer was implicated"
else
    echo "    OK: no -DPGWT_SERVER consumer implicated"
fi
