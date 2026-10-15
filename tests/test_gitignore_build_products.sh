#!/usr/bin/env bash
# test_gitignore_build_products.sh — every binary `make -C tests` builds must
# be .gitignore'd, and no SOURCE file may be.
#
# Why this exists. scripts/tree-hash.sh (used by scripts/check.sh and the push
# guard) hashes tracked PLUS untracked-but-unignored files. So a build product
# that .gitignore does not cover changes the tree hash the moment anyone runs
# `make -C tests` — the `make check` stamp stops matching, and a
# demo-rehearsal attempt loses its pairing. That is not hypothetical: counted
# attempt 2 hashed c47f9d0f against attempt 1's d645b0ee for exactly this
# reason, and at the time this test was written 33 of the 64 products
# tests/Makefile builds were missing from the per-file list in .gitignore.
#
# The fix was to ignore them as a CLASS (`tests/test_*` + `!tests/*.*`, see
# .gitignore). A class pattern still cannot see a product with a brand-new
# prefix, and nothing about a pattern announces that it has stopped matching.
# This test is that announcement.
#
# Run from tests/ (the way tests/unit_tests.list entries are invoked) or from
# anywhere; it locates the repo itself.
#
#   ./test_gitignore_build_products.sh              # audit + self-test
#   PGWT_GUARD_AUDIT_ONLY=1 ./test_...sh            # audit only
#   ./test_gitignore_build_products.sh <repo-dir>   # audit a scratch repo
#                                                   # (used by the self-test)
set -uo pipefail

checks=0
fails=0

ok()   { checks=$((checks + 1)); printf '  ok: %s\n' "$1"; }
bad()  { checks=$((checks + 1)); fails=$((fails + 1)); printf '  FAIL: %s\n' "$1"; }
note() { printf '    %s\n' "$1"; }

# ── the audit ────────────────────────────────────────────────────────────
# Prints its findings and returns 0 (clean) / 1 (a product is unignored or a
# source file is ignored) / 2 (REFUSED: it could not establish what to check).
# 2 is distinct on purpose: "I looked and found nothing wrong" and "I could
# not look" must never report the same way, which is how an unreachable gate
# ends up approving.
audit() {
    local repo="$1" quiet="${2:-}"
    local out=0

    command -v git >/dev/null 2>&1 || {
        [[ -n "$quiet" ]] || note "REFUSE: no usable git on PATH"
        return 2
    }
    [[ -d "$repo" ]] || {
        [[ -n "$quiet" ]] || note "REFUSE: no such directory: $repo"
        return 2
    }
    local top
    top=$(git -C "$repo" rev-parse --show-toplevel 2>/dev/null) || {
        [[ -n "$quiet" ]] || note "REFUSE: not a git work tree: $repo"
        return 2
    }
    [[ -f "$top/tests/Makefile" ]] || {
        [[ -n "$quiet" ]] || note "REFUSE: no tests/Makefile under $top"
        return 2
    }

    # $(TESTS) is a backslash-continued assignment. Parse it textually rather
    # than via `make -p`: `make` would try to BUILD things (and on macOS the
    # top-level Makefile cannot even configure), and a gate that needs a
    # working toolchain to run is a gate that skips on the machine where the
    # drift is introduced.
    local prods
    # [[:space:]], never [ \t]: a bracket expression is NOT an escape
    # context in POSIX/BSD sed, so [ \t] there means "space, backslash or the
    # letter t" and silently ate the leading t of every product name
    # (test_wait_event -> est_wait_event). The self-test below caught it;
    # a name mangled this way is unignored, so the guard would have been
    # permanently red for a reason that has nothing to do with .gitignore.
    prods=$(awk '/^TESTS[[:space:]]*=/{f=1} f{print} f && !/\\[[:space:]]*$/{exit}' \
                "$top/tests/Makefile" \
            | sed 's/^TESTS[[:space:]]*=[[:space:]]*//; s/\\[[:space:]]*$//')
    prods=$(printf '%s map_reader_core.o\n' "$prods" | tr -s '[:space:]' '\n' \
            | sed '/^$/d' | sort -u)

    local nprods
    nprods=$(printf '%s\n' "$prods" | sed '/^$/d' | wc -l | tr -d ' ')
    # An empty or implausibly short product list means the parse broke (a
    # renamed variable, a reformatted Makefile). Refuse: a zero-length list
    # makes "every product is ignored" vacuously true, which is precisely the
    # false negative this file is about.
    if [[ "$nprods" -lt 10 ]]; then
        [[ -n "$quiet" ]] || note "REFUSE: parsed only $nprods products from tests/Makefile \$(TESTS)"
        return 2
    fi

    # Tracked files under tests/ — the other half of the invariant, and also a
    # non-vacuity check on the repo itself.
    local tracked ntracked
    tracked=$(git -C "$top" ls-files tests) || {
        [[ -n "$quiet" ]] || note "REFUSE: git ls-files tests failed"
        return 2
    }
    ntracked=$(printf '%s\n' "$tracked" | sed '/^$/d' | wc -l | tr -d ' ')
    if [[ "$ntracked" -lt 1 ]]; then
        [[ -n "$quiet" ]] || note "REFUSE: no tracked files under tests/"
        return 2
    fi

    [[ -n "$quiet" ]] || note "$nprods build products, $ntracked tracked files under tests/"

    local p unignored=""
    for p in $prods; do
        if ! git -C "$top" check-ignore -q "tests/$p"; then
            unignored="$unignored tests/$p"
        fi
    done
    if [[ -n "$unignored" ]]; then
        out=1
        [[ -n "$quiet" ]] || for p in $unignored; do note "UNIGNORED PRODUCT: $p"; done
    fi

    local f ignored_src=""
    for f in $tracked; do
        if git -C "$top" check-ignore -q "$f"; then
            ignored_src="$ignored_src $f"
        fi
    done
    if [[ -n "$ignored_src" ]]; then
        out=1
        [[ -n "$quiet" ]] || for f in $ignored_src; do note "IGNORED SOURCE: $f"; done
    fi

    return "$out"
}

# ── explicit scratch-repo mode (the self-test calls this) ────────────────
if [[ $# -ge 1 ]]; then
    audit "$1"
    exit $?
fi

echo "=== test_gitignore_build_products ==="

repo=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "  FAIL: not inside a git work tree -- cannot audit anything"
    exit 1
}

echo "--- 1. this repository ---"
audit "$repo"
rc=$?
case "$rc" in
    0) ok "every tests/Makefile build product is ignored, and no tracked file under tests/ is" ;;
    2) bad "the audit REFUSED on this repository (rc=2) -- it could not establish what to check" ;;
    *) bad "the audit found a drifted entry on this repository (rc=$rc, detail above)" ;;
esac

# A build product named outside every class pattern would be invisible to the
# .gitignore classes; this is the one check that would catch it, so say which
# prefixes are currently load-bearing rather than leaving it implicit.
echo "--- 2. the tree-hash consequence, measured ---"
before=$("$repo/scripts/tree-hash.sh" 2>/dev/null)
if [[ -z "$before" ]]; then
    bad "scripts/tree-hash.sh produced nothing -- section 2 cannot measure anything"
else
    stand_in="$repo/tests/test_gitignore_guard_stand_in"
    rm -f "$stand_in"
    printf 'stand-in for a freshly built test binary\n' > "$stand_in"
    after=$("$repo/scripts/tree-hash.sh" 2>/dev/null)
    rm -f "$stand_in"
    restored=$("$repo/scripts/tree-hash.sh" 2>/dev/null)
    if [[ "$before" == "$after" ]]; then
        ok "a stand-in binary at tests/test_* does not move the tree hash ($before)"
    else
        bad "a stand-in binary at tests/test_* moved the tree hash: $before -> $after"
    fi
    if [[ "$before" == "$restored" ]]; then
        ok "and the hash is unchanged after removing it (no side effect on the tree)"
    else
        bad "removing the stand-in left the tree hash changed: $before -> $restored"
    fi
    # Non-vacuity: the same mechanism MUST move the hash for a file the
    # patterns do not cover. Without this leg, section 2 would pass just as
    # happily if tree-hash.sh always returned a constant.
    src_in="$repo/tests/test_gitignore_guard_stand_in.c"
    rm -f "$src_in"
    printf 'int main(void){return 0;}\n' > "$src_in"
    moved=$("$repo/scripts/tree-hash.sh" 2>/dev/null)
    rm -f "$src_in"
    if [[ "$moved" != "$before" ]]; then
        ok "a SOURCE file at tests/test_*.c does move it ($moved) -- the hash is not a constant"
    else
        bad "a source file did not move the tree hash either: tree-hash.sh is not measuring the tree"
    fi
fi

if [[ -n "${PGWT_GUARD_AUDIT_ONLY:-}" ]]; then
    echo ""
    echo "$checks checks, $fails failures (audit only; self-test skipped by request)"
    [[ "$fails" -eq 0 ]] || exit 1
    exit 0
fi

# ── 3. self-test: every way this guard could stop checking ───────────────
# Each leg builds a scratch git repo and runs THIS script against it. A leg
# that is supposed to go red and does not is itself a failure.
echo "--- 3. self-test: the guard must refuse rather than approve ---"

self="$repo/tests/test_gitignore_build_products.sh"
scratch=$(mktemp -d) || { echo "  FAIL: no scratch dir"; exit 1; }
trap 'rm -rf "$scratch"' EXIT

# Minimal fixture: a $(TESTS) list long enough to clear the >=10 floor, one
# tracked source per product name, and a .gitignore we vary per leg.
mk_repo() {
    local d="$1" ignore="$2" n=14 i
    mkdir -p "$d/tests" "$d/scripts"
    {
        printf 'TESTS     = '
        for ((i = 1; i <= n; i++)); do
            printf 'test_f%02d ' "$i"
            [[ $i -lt $n ]] && printf '\\\n            '
        done
        printf '\n\nall: $(TESTS)\n'
    } > "$d/tests/Makefile"
    for ((i = 1; i <= n; i++)); do
        printf 'int main(void){return 0;}\n' > "$(printf '%s/tests/test_f%02d.c' "$d" "$i")"
    done
    printf '%s\n' "$ignore" > "$d/.gitignore"
    git -C "$d" init -q 2>/dev/null
    git -C "$d" config user.email t@t >/dev/null
    git -C "$d" config user.name t >/dev/null
    git -C "$d" add -A >/dev/null 2>&1
    git -C "$d" commit -qm fixture >/dev/null 2>&1
}

leg() {  # leg <name> <want-rc> <repo-dir>
    local name="$1" want="$2" dir="$3" got
    "$self" "$dir" >/dev/null 2>&1
    got=$?
    if [[ "$got" -eq "$want" ]]; then
        ok "$name (rc=$got)"
    else
        bad "$name -- rc=$got, want $want"
    fi
}

# 3a. the class pattern: clean.
mk_repo "$scratch/good" 'tests/test_*
!tests/*.*
tests/*.o'
leg "3a. class pattern + source negation is CLEAN" 0 "$scratch/good"

# 3b. RED: the per-file style this change replaced, with three names missing.
#     This is the exact defect that broke rehearsal attempt 2, reproduced on
#     demand -- without it, 3a would pass just as well against a guard that
#     never looks at .gitignore at all.
mk_repo "$scratch/drifted" 'tests/test_f01
tests/test_f02
tests/test_f03
tests/test_f04
tests/test_f05
tests/test_f06
tests/test_f07
tests/test_f08
tests/test_f09
tests/test_f10
tests/test_f11'
leg "3b. three products missing from a per-file list is RED" 1 "$scratch/drifted"

# 3c. RED the other way: a pattern so broad it swallows the sources. An
#     over-broad fix is as bad as the drift -- an ignored .c silently drops
#     out of the tree hash, which is the same bug wearing the other hat.
mk_repo "$scratch/overbroad" 'tests/test_*'
leg "3c. a pattern that also ignores tests/*.c is RED" 1 "$scratch/overbroad"

# 3d. REFUSE: not a git work tree. A gate that cannot ask git anything must
#     not report "clean".
mkdir -p "$scratch/nogit/tests"
cp "$scratch/good/tests/Makefile" "$scratch/nogit/tests/Makefile"
leg "3d. a directory that is not a git work tree REFUSES" 2 "$scratch/nogit"

# 3e. REFUSE: no such directory.
leg "3e. a missing directory REFUSES" 2 "$scratch/does-not-exist"

# 3f. REFUSE: tests/Makefile absent -- nothing to enumerate.
mkdir -p "$scratch/nomk"
git -C "$scratch/nomk" init -q 2>/dev/null
leg "3f. a repo with no tests/Makefile REFUSES" 2 "$scratch/nomk"

# 3g. REFUSE: $(TESTS) unparseable (renamed variable). The product list comes
#     back empty, which would make "every product is ignored" VACUOUSLY true.
mk_repo "$scratch/unparseable" 'tests/test_*
!tests/*.*
tests/*.o'
sed -i.bak 's/^TESTS/TEST_PROGRAMS/' "$scratch/unparseable/tests/Makefile"
rm -f "$scratch/unparseable/tests/Makefile.bak"
leg "3g. an unparseable \$(TESTS) REFUSES instead of passing on an empty list" 2 \
    "$scratch/unparseable"

# 3h. REFUSE: a $(TESTS) list too short to be real (the floor), same reason.
mk_repo "$scratch/tiny" 'tests/test_*
!tests/*.*
tests/*.o'
printf 'TESTS     = test_f01 test_f02\n\nall: $(TESTS)\n' > "$scratch/tiny/tests/Makefile"
leg "3h. a two-entry \$(TESTS) is below the plausibility floor and REFUSES" 2 \
    "$scratch/tiny"

# 3i. REFUSE: no tracked files under tests/ -- the source half of the
#     invariant has nothing to check, so the whole audit is uninformative.
mkdir -p "$scratch/untracked"
git -C "$scratch/untracked" init -q 2>/dev/null
mkdir -p "$scratch/untracked/tests"
cp "$scratch/good/tests/Makefile" "$scratch/untracked/tests/Makefile"
printf 'tests/test_*\n!tests/*.*\ntests/*.o\n' > "$scratch/untracked/.gitignore"
leg "3i. a repo with nothing tracked under tests/ REFUSES" 2 "$scratch/untracked"

# 3j. REFUSE: git itself unusable (the 126/127 class). PATH is emptied for one
#     call, so `command -v git` fails exactly as it would on a box where git
#     is missing or non-executable.
rc=0
env PATH=/nonexistent "$BASH" "$self" "$scratch/good" >/dev/null 2>&1 || rc=$?
if [[ "$rc" -eq 2 ]]; then
    ok "3j. an unusable git REFUSES (rc=2), it does not approve"
else
    bad "3j. an unusable git gave rc=$rc, want 2"
fi

echo ""
echo "$checks checks, $fails failures"
[[ "$fails" -eq 0 ]] || exit 1
exit 0
