#!/bin/bash
# test_docs_summary_version.sh — the docs that name PGWT_SUMMARY_VERSION must
# name the one the code defines.
#
# This drifted twice in one night. docs/AAS_SEMANTICS_DECISION.md listed
# versions up to v6 while the constant was 8; the correction was written
# against a v8 tree and landed on a branch that had meanwhile merged v9, so
# docs/DEMO_RUNBOOK.md then told an operator to move aside anything "before
# v8" — leaving a v8 directory in place for a v9 server, which refuses it on
# the summary fast path and drops the 900 s Overview to the raw path with no
# explanation. The runbook exists to prevent exactly that, so a stale number
# in it is an operational defect, not a typo.
#
# Checked here, in milliseconds, instead of by whoever reads it next:
#   1. AAS_SEMANTICS_DECISION.md has a `- **vN**` bullet for the current N.
#   2. AAS_SEMANTICS_DECISION.md states the current N as the constant.
#   3. DEMO_RUNBOOK.md says "now **vN**" for the current N.
#   4. DEMO_RUNBOOK.md's `mv traces traces.pre-vN` names the current N.
#
# REFUSES (exit 2) rather than passing when it cannot establish the version
# or cannot read a file: a docs check that silently finds nothing to compare
# approves every stale number there will ever be.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
HDR="$REPO/src/summary_writer.h"
AAS="$REPO/docs/AAS_SEMANTICS_DECISION.md"
RUN="$REPO/docs/DEMO_RUNBOOK.md"

# A scratch mode so the self-test can drive this logic over fixtures.
if [[ $# -eq 3 ]]; then HDR="$1"; AAS="$2"; RUN="$3"; fi

checks=0; fails=0
ok()  { checks=$((checks+1)); echo "  ok: $1"; }
bad() { checks=$((checks+1)); fails=$((fails+1)); echo "  FAIL: $1"; }

for f in "$HDR" "$AAS" "$RUN"; do
    if [[ ! -r "$f" ]]; then
        echo "REFUSE: cannot read $f -- nothing to compare"
        exit 2
    fi
done

V=$(sed -n 's/^#define[[:space:]]\{1,\}PGWT_SUMMARY_VERSION[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' \
        "$HDR" | head -1)
if [[ -z "$V" ]]; then
    echo "REFUSE: no '#define PGWT_SUMMARY_VERSION <n>' in $HDR."
    echo "        Without the constant there is nothing to hold the docs to,"
    echo "        and every number below would pass by default."
    exit 2
fi

[[ $# -eq 3 ]] || echo "=== test_docs_summary_version (PGWT_SUMMARY_VERSION = $V) ==="

grep -q -- "- \*\*v$V\*\*" "$AAS" \
    && ok "AAS_SEMANTICS_DECISION.md has a v$V entry" \
    || bad "AAS_SEMANTICS_DECISION.md has no '- **v$V**' entry; its own rule is that the list tracks the constant"

grep -q "is \*\*$V\*\* as of" "$AAS" \
    && ok "AAS_SEMANTICS_DECISION.md states the constant as $V" \
    || bad "AAS_SEMANTICS_DECISION.md does not state the constant as **$V**"

grep -q "now \*\*v$V\*\*" "$RUN" \
    && ok "DEMO_RUNBOOK.md says summary files are now v$V" \
    || bad "DEMO_RUNBOOK.md does not say 'now **v$V**' -- an operator told the wrong version keeps a directory the server will refuse"

grep -q "traces\.pre-v$V" "$RUN" \
    && ok "DEMO_RUNBOOK.md's mv instruction names traces.pre-v$V" \
    || bad "DEMO_RUNBOOK.md's 'mv traces traces.pre-vN' does not name v$V -- this is the line that decides which directories an operator moves aside"

if [[ $# -eq 3 ]]; then
    [[ "$fails" -eq 0 ]] || exit 1
    exit 0
fi

# ── self-test: the ways this check could stop checking ──────────────────
echo "--- self-test ---"
self="$REPO/tests/test_docs_summary_version.sh"
scratch=$(mktemp -d) || { echo "REFUSE: no scratch dir"; exit 2; }
trap 'rm -rf "$scratch"' EXIT

mk() {  # mk <n> <aas-n> <runbook-now-n> <runbook-mv-n>
    printf '#define PGWT_SUMMARY_VERSION  %s\n' "$1" > "$scratch/h.h"
    printf -- '- **v%s** something\n\nwhich is **%s** as of today\n' "$2" "$2" > "$scratch/a.md"
    printf 'now **v%s**\n\nmv traces traces.pre-v%s\n' "$3" "$4" > "$scratch/r.md"
}
leg() {  # leg <name> <want-rc>
    "$self" "$scratch/h.h" "$scratch/a.md" "$scratch/r.md" >/dev/null 2>&1
    local got=$?
    if [[ "$got" -eq "$2" ]]; then ok "$1 (rc=$got)"; else bad "$1 -- rc=$got, want $2"; fi
}

mk 9 9 9 9;  leg "all four agree is CLEAN" 0
mk 9 8 9 9;  leg "a stale AAS list is RED" 1
mk 9 9 8 9;  leg "a stale runbook 'now vN' is RED" 1
mk 9 9 9 8;  leg "a stale traces.pre-vN is RED -- the operational line" 1
mk 10 9 9 9; leg "bumping the constant alone turns every doc RED" 1

printf 'no define here\n' > "$scratch/h.h"
leg "a header with no PGWT_SUMMARY_VERSION REFUSES, never approves" 2

mk 9 9 9 9; rm -f "$scratch/a.md"
leg "an unreadable doc REFUSES" 2

echo ""
echo "$checks checks, $fails failures"
[[ "$fails" -eq 0 ]] || exit 1
exit 0
