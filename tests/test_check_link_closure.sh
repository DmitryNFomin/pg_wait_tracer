#!/usr/bin/env bash
# test_check_link_closure.sh — self-test for scripts/check-link-closure.sh.
#
# The guard answers one question: does this translation unit reference any
# symbol it did not reference at the base commit? A new reference is a link
# that breaks in every target which links the .o but not the new dependency —
# compiles clean everywhere, fails only at link, and only on the box. That is
# what it cost on 2026-10-15: two ephemeral VMs and two `box-check` exits with
# every test that ran passing.
#
# So this file is mostly about the ways the guard could fail to SEE that:
# no resolvable base, a base revision that will not compile, a missing
# compiler, the wrong translation unit, an empty symbol set (which compares
# equal to anything). Each must REFUSE — exit 2, "CANNOT VERIFY" — because a
# closure check that cannot establish a baseline approves every change ever
# made.
#
# Runs against a SYNTHETIC git repo in a temp dir: it never edits the real
# tree, so an interrupted run cannot leave a mutated source file behind.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
GUARD="$PWD/scripts/check-link-closure.sh"

pass=0; fail=0
report() {   # report <ok> <desc>
    if [[ "$1" == true ]]; then printf '  PASS: %s\n' "$2"; pass=$((pass+1))
    else printf '  FAIL: %s\n' "$2"; fail=$((fail+1)); fi
}

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
repo="$tmp/repo"; mkdir -p "$repo/src"
git -C "$repo" init -q
git -C "$repo" config user.email t@t; git -C "$repo" config user.name t

# base: a TU with no external references beyond libc.
cat > "$repo/src/tu.c" <<'EOF'
int tu_anchor(int x) { return x + 1; }
EOF
git -C "$repo" add -A; git -C "$repo" commit -qm base
base=$(git -C "$repo" rev-parse HEAD)

run() { (cd "$repo" && ROOT="$repo" TU=src/tu.c ANCHOR=_tu_anchor \
             "$GUARD" "$@" 2>&1); }
rc_of() { (cd "$repo" && ROOT="$repo" TU=src/tu.c ANCHOR=_tu_anchor \
               "$GUARD" "$@" >/dev/null 2>&1); echo $?; }

# ── 1. clean: the TU is unchanged ───────────────────────────────────────
[[ $(rc_of "$base") -eq 0 ]] && ok=true || ok=false
report "$ok" "an unchanged TU passes (rc=$(rc_of "$base"))"

# ── 2. TRUE POSITIVE: a new external reference ──────────────────────────
cat > "$repo/src/tu.c" <<'EOF'
extern int some_daemon_symbol(int);
int tu_anchor(int x) { return some_daemon_symbol(x) + 1; }
EOF
git -C "$repo" commit -qam head
out=$(run "$base"); rc=$(rc_of "$base")
[[ $rc -eq 1 ]] && ok=true || ok=false
report "$ok" "a NEW link-time dependency fails the guard (rc=$rc)"
grep -q 'some_daemon_symbol' <<<"$out" && ok=true || ok=false
report "$ok" "the failure NAMES the symbol that was added"

# ── 3. the same change, described as a comment, must NOT fail ───────────
#    (a guard that fires on any edit is a guard nobody keeps)
cat > "$repo/src/tu.c" <<'EOF'
/* a comment, a rename, a reformat: none of these change the closure */
int tu_anchor(int y) { int z = y; return z + 1; }
EOF
git -C "$repo" commit -qam cosmetic
[[ $(rc_of "$base") -eq 0 ]] && ok=true || ok=false
report "$ok" "a cosmetic edit does not fail the guard"

# ── 4. REFUSALS: every way it could lose its baseline ───────────────────
[[ $(rc_of definitely-not-a-ref) -eq 2 ]] && ok=true || ok=false
report "$ok" "an unresolvable base ref REFUSES (rc=$(rc_of definitely-not-a-ref))"

tree=$(git -C "$repo" rev-parse "HEAD^{tree}")
[[ $(rc_of "$tree") -eq 2 ]] && ok=true || ok=false
report "$ok" "a ref that is not a commit REFUSES"

rc=$( (cd "$repo" && ROOT="$repo" TU=src/tu.c ANCHOR=_tu_anchor \
           CC=/nonexistent-compiler "$GUARD" "$base" >/dev/null 2>&1); echo $?)
[[ $rc -eq 2 ]] && ok=true || ok=false
report "$ok" "a missing compiler REFUSES rather than passing (rc=$rc)"

rc=$( (cd "$repo" && ROOT="$repo" TU=src/nope.c ANCHOR=_tu_anchor \
           "$GUARD" "$base" >/dev/null 2>&1); echo $?)
[[ $rc -eq 2 ]] && ok=true || ok=false
report "$ok" "a TU absent at base REFUSES (rc=$rc)"

# Wrong TU: the anchor symbol is not defined in what was built. Without this
# control the guard would compare two unrelated symbol sets and call it clean.
cat > "$repo/src/other.c" <<'EOF'
int other_thing(void) { return 0; }
EOF
git -C "$repo" add -A; git -C "$repo" commit -qm other
rc=$( (cd "$repo" && ROOT="$repo" TU=src/other.c ANCHOR=_tu_anchor \
           "$GUARD" HEAD >/dev/null 2>&1); echo $?)
[[ $rc -eq 2 ]] && ok=true || ok=false
report "$ok" "a TU that does not define the anchor REFUSES (rc=$rc)"

# A base revision that no longer compiles: with no baseline symbol set, the
# honest answer is "cannot verify", not "clean".
cat > "$repo/src/broken.c" <<'EOF'
this is not C at all
EOF
git -C "$repo" add -A; git -C "$repo" commit -qm broken-base
brokenbase=$(git -C "$repo" rev-parse HEAD)
cat > "$repo/src/broken.c" <<'EOF'
int tu_anchor(void) { return 0; }
EOF
git -C "$repo" commit -qam broken-fixed
rc=$( (cd "$repo" && ROOT="$repo" TU=src/broken.c ANCHOR=_tu_anchor \
           "$GUARD" "$brokenbase" >/dev/null 2>&1); echo $?)
[[ $rc -eq 2 ]] && ok=true || ok=false
report "$ok" "a base revision that will not compile REFUSES (rc=$rc)"

# ── 5. the ALLOWLIST, and the four ways it could become a rubber stamp ──
#
# A new dependency is not automatically a break: if the object providing it is
# already linked by every target that consumes the TU, nothing fails. That is
# declarable in scripts/link-closure-allow.txt — and the declaration is
# CHECKED against tests/Makefile, which is the only reason it is not an
# escape hatch. These cases are the check checking itself.
mkdir -p "$repo/tests" "$repo/scripts"
cat > "$repo/tests/Makefile" <<'EOF'
BUILD = ../build

test_a: test_a.c $(BUILD)/tu.o $(BUILD)/prov.o
	$(CC) -o $@ $^

test_b: test_b.c $(BUILD)/tu.o \
        $(BUILD)/prov.o
	$(CC) -o $@ $^

# A -DPGWT_SERVER build of the same source: the daemon half is compiled out,
# so this target is NOT a consumer of the dependency and must never be named.
tu_core.o: ../src/tu.c
	$(CC) -DPGWT_SERVER -c -o $@ $<

test_c: test_c.c tu_core.o
	$(CC) -o $@ $^
EOF
cat > "$repo/scripts/link-closure-allow.txt" <<'EOF'
# symbol provider  # why
some_daemon_symbol  prov   # provided by prov.o, linked by every consumer
EOF
git -C "$repo" add -A; git -C "$repo" commit -qm makefile

# Back to the TU that references the external symbol.
cat > "$repo/src/tu.c" <<'EOF'
extern int some_daemon_symbol(int);
int tu_anchor(int x) { return some_daemon_symbol(x) + 1; }
EOF
git -C "$repo" commit -qam dep

out=$(run "$base"); rc=$(rc_of "$base")
[[ $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "a declared dependency whose provider IS linked everywhere passes (rc=$rc)"
grep -q 'ALLOWED: ' <<<"$out" && ok=true || ok=false
report "$ok" "the acceptance is PRINTED, not silent"
grep -q 'tu_core\|test_c' <<<"$out" && ok=false || ok=true
report "$ok" "a -DPGWT_SERVER consumer of the same source is not implicated"

# (a) the provider named does not exist in any link line.
sed -i.bak 's/^some_daemon_symbol  prov/some_daemon_symbol  somethingelse/' \
    "$repo/scripts/link-closure-allow.txt"
out=$(run "$base"); rc=$(rc_of "$base")
[[ $rc -eq 1 ]] && ok=true || ok=false
report "$ok" "an entry naming a provider nobody links FAILS (rc=$rc)"
grep -q 'test_a' <<<"$out" && ok=true || ok=false
report "$ok" "...and NAMES the target that would not link"
mv "$repo/scripts/link-closure-allow.txt.bak" "$repo/scripts/link-closure-allow.txt"

# (b) the entry is right, but a target drops the provider. This is the case
#     that makes the allowlist a check rather than a promise.
sed -i.bak 's|test_a: test_a.c $(BUILD)/tu.o $(BUILD)/prov.o|test_a: test_a.c $(BUILD)/tu.o|' \
    "$repo/tests/Makefile"
out=$(run "$base"); rc=$(rc_of "$base")
[[ $rc -eq 1 ]] && ok=true || ok=false
report "$ok" "a target that drops the provider FAILS even with the entry present (rc=$rc)"
grep -q 'test_a' <<<"$out" && ok=true || ok=false
report "$ok" "...and NAMES that target"
mv "$repo/tests/Makefile.bak" "$repo/tests/Makefile"

# (c) no allowlist at all: the dependency is undeclared, which is the
#     original failure, not a pass.
rm -f "$repo/scripts/link-closure-allow.txt"
[[ $(rc_of "$base") -eq 1 ]] && ok=true || ok=false
report "$ok" "with no allowlist the dependency is UNDECLARED and fails"
cat > "$repo/scripts/link-closure-allow.txt" <<'EOF'
some_daemon_symbol  prov
EOF

# (d) no Makefile to check the claim against: refuse, do not take its word.
mv "$repo/tests/Makefile" "$repo/tests/Makefile.hidden"
[[ $(rc_of "$base") -eq 2 ]] && ok=true || ok=false
report "$ok" "with no Makefile to verify against, an entry REFUSES (rc=$(rc_of "$base"))"
mv "$repo/tests/Makefile.hidden" "$repo/tests/Makefile"

# restore the clean TU for the remaining cases
cat > "$repo/src/tu.c" <<'EOF'
int tu_anchor(int x) { return x + 1; }
EOF
git -C "$repo" commit -qam restore

# ── 6. and the guard must still be wired to the REAL tree ───────────────
#    (a self-test that only ever exercises a synthetic repo would pass
#     happily while the production invocation pointed at nothing)
real=$(scripts/check-link-closure.sh 2>&1); rc=$?
[[ $rc -eq 0 ]] && ok=true || ok=false
report "$ok" "the real invocation (src/map_reader.c, no args) passes: ${real##*$'\n'}"
grep -q 'anchor _pgwt_read_state_map present' <<<"$real" && ok=true || ok=false
report "$ok" "the real invocation confirmed its anchor, so it compiled the right TU"

printf '%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
