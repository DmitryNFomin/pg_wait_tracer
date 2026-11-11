#!/usr/bin/env bash
# Offline #861 regression/negative controls. Run the real build helper with
# the REAL clean targets; only compilation/network/identity calls are shimmed.
# shellcheck disable=SC2016
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; return 1; }
real_make=$(command -v make)
export REAL_MAKE="$real_make"
mkdir -p "$tmp/bin" "$tmp/remote/tests" "$tmp/remote/scripts" "$tmp/local/scripts" "$tmp/local/tests"
cp "$ROOT/Makefile" "$tmp/remote/Makefile"
cp "$ROOT/tests/Makefile" "$tmp/remote/tests/Makefile"
cat > "$tmp/remote/build-fixture.mk" <<'MAKE'
.PHONY: all
all: pgwt-server build/main.o
pgwt-server: source.c
	cp $< $@
build/main.o: source.c
	mkdir -p build
	cp $< $@
MAKE
cat > "$tmp/bin/make" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
case "$*" in
    clean-box) exec "$REAL_MAKE" SYSTEM_LIBBPF_HAS_USDT=yes clean-box ;;
    '-C tests clean') exec "$REAL_MAKE" -C tests clean ;;
    -j*)
        for path in pgwt-server build/main.o foreign.o nested/foreign.a nested/foreign.so pg_wait_tracer pgwt-server-asan pg_wait_tracer-asan web/pgwt tests/map_reader_core.o; do
            if [[ "$EXPECT_CACHE" == reset && -e "$path" ]]; then
                echo "FAIL: stale NEWER $path survived before build"; exit 91
            elif [[ "$EXPECT_CACHE" == reuse && ! -e "$path" ]]; then
                echo "FAIL: same-tree $path was not kept before build"; exit 92
            fi
        done
        "$REAL_MAKE" -f build-fixture.mk -j2
        [[ "${FAIL_STAGE:-}" != daemon ]] || exit 42
        ;;
    build-tests|pgwt-client)
        [[ "${FAIL_STAGE:-}" != "$1" ]] || exit 42
        ;;
    *) echo "FAIL: unexpected make $*"; exit 93 ;;
esac
SH
chmod +x "$tmp/bin/make"
printf '#!/usr/bin/env bash\necho 2\n' > "$tmp/bin/nproc"
chmod +x "$tmp/bin/nproc"
export PATH="$tmp/bin:$PATH"

seed() {
    rm -rf "$tmp/remote/build" "$tmp/remote/nested" "$tmp/remote/web"
    mkdir -p "$tmp/remote/build" "$tmp/remote/nested" "$tmp/remote/web" "$tmp/remote/include"
    printf 'synced source\n' > "$tmp/remote/source.c"
    touch -t 200001010000 "$tmp/remote/source.c"
    for path in pgwt-server build/main.o foreign.o nested/foreign.a nested/foreign.so pg_wait_tracer pgwt-server-asan pg_wait_tracer-asan web/pgwt tests/map_reader_core.o include/vmlinux.h include/pg_wait_tracer.skel.h; do
        printf 'foreign newer output\n' > "$tmp/remote/$path"
        touch -t 203701010000 "$tmp/remote/$path"
    done
    rm -f "$tmp/remote/.pgwt-box-built-from"
    [[ "$1" == missing ]] || printf '%s\n' "$1" > "$tmp/remote/.pgwt-box-built-from"
}
run_build() {
    (cd "$tmp/remote"; EXPECT_CACHE="$2" FAIL_STAGE="${3:-}" bash "$1" aaaa cccc) > "$tmp/build.log" 2>&1
}
assert_cache() {
    local script="$1" previous="$2" expected="$3"
    seed "$previous"
    if ! run_build "$script" "$expected"; then
        cat "$tmp/build.log"; fail "$previous marker cache $expected"; return 1
    fi
    [[ $(cat "$tmp/remote/.pgwt-box-built-from") == aaaa ]] || { fail 'successful build marker'; return 1; }
    grep -Fq 'box-check: commit=cccc tree-hash=aaaa built-from=aaaa' "$tmp/build.log" || { fail 'built-from header'; return 1; }
    if [[ "$expected" == reset ]]; then
        cmp "$tmp/remote/source.c" "$tmp/remote/pgwt-server" || { fail 'rebuilt server content'; return 1; }
        cmp "$tmp/remote/source.c" "$tmp/remote/build/main.o" || { fail 'rebuilt object content'; return 1; }
        [[ ! -e "$tmp/remote/include/vmlinux.h" && ! -e "$tmp/remote/include/pg_wait_tracer.skel.h" ]] || { fail 'generated BPF headers cleaned'; return 1; }
    fi
    echo "PASS: $previous marker -> $expected before build (NEWER binary, objects, libraries, ASan, Go)"
}
assert_failure() {
    local script="$1" stage="$2" rc=0
    seed aaaa
    run_build "$script" reuse "$stage" || rc=$?
    [[ $rc == 42 ]] || { cat "$tmp/build.log"; fail "failed $stage build exit (got $rc)"; return 1; }
    [[ ! -e "$tmp/remote/.pgwt-box-built-from" ]] || { fail "failed $stage build left marker"; return 1; }
    if grep -q '^box-check: commit=' "$tmp/build.log"; then fail 'failed build emitted certified header'; return 1; fi
    echo "PASS: failed $stage build leaves no marker or certified header"
}
helper="$ROOT/scripts/box-build.sh"
assert_cache "$helper" bbbb reset
assert_cache "$helper" missing reset
assert_cache "$helper" aaaa reuse
for stage in daemon build-tests pgwt-client; do assert_failure "$helper" "$stage"; done

# Mutation controls grade the exact same behavioral assertions by name.
sed 's/if \[\[ "$built_from" != "$tree_hash" \]\]; then/if false; then/' "$helper" > "$tmp/no-clean.sh"
sed 's/if \[\[ "$built_from" != "$tree_hash" \]\]; then/if true; then/' "$helper" > "$tmp/always-clean.sh"
sed '/^rm -f "\$marker"$/d' "$helper" > "$tmp/keep-marker.sh"
for previous in bbbb missing; do
    if assert_cache "$tmp/no-clean.sh" "$previous" reset > "$tmp/control.log" 2>&1; then fail 'no-clean mutant escaped'; exit 1; fi
    grep -Fq 'FAIL: stale NEWER pgwt-server survived before build' "$tmp/control.log"
    echo "PASS: negative control $previous marker fails by name: stale NEWER pgwt-server survived before build"
done
if assert_cache "$tmp/always-clean.sh" aaaa reuse > "$tmp/control.log" 2>&1; then fail 'always-clean mutant escaped'; exit 1; fi
grep -Fq 'FAIL: same-tree pgwt-server was not kept before build' "$tmp/control.log"
echo 'PASS: negative control same marker fails by name: same-tree pgwt-server was not kept before build'
if assert_failure "$tmp/keep-marker.sh" daemon > "$tmp/control.log" 2>&1; then fail 'keep-marker mutant escaped'; exit 1; fi
grep -Fq 'FAIL: failed daemon build left marker' "$tmp/control.log"
echo 'PASS: negative control failed build fails by name: failed daemon build left marker'
# Removing only the recursive object/library sweep must fail on the object,
# independently of the daemon cleanup delegated to the existing clean rule.
sed '/^[[:space:]]*find \. -name \.git /d' "$ROOT/Makefile" > "$tmp/remote/Makefile"
if assert_cache "$helper" bbbb reset > "$tmp/control.log" 2>&1; then fail 'no object sweep mutant escaped'; exit 1; fi
grep -Fq 'FAIL: stale NEWER foreign.o survived before build' "$tmp/control.log"
cp "$ROOT/Makefile" "$tmp/remote/Makefile"
echo 'PASS: negative control object sweep fails by name: stale NEWER foreign.o survived before build'

# Invoke the WHOLE launcher with isolated helper scripts and network shims.
# The emitted SSH command must put the real helper after the existing flock
# and before run_all. No real tree-hash, SSH, cloud request or build is run.
cat > "$tmp/local/scripts/tree-hash.sh" <<'SH'
#!/usr/bin/env bash
echo aaaa
SH
printf '#!/usr/bin/env bash\nexit 0\n' > "$tmp/local/tests/hetzner-sweep.sh"
chmod +x "$tmp/local/scripts/tree-hash.sh" "$tmp/local/tests/hetzner-sweep.sh"
cat > "$tmp/bin/git" <<'SH'
#!/usr/bin/env bash
case "$*" in
    'rev-parse --abbrev-ref HEAD') echo HEAD ;;
    'rev-parse --short HEAD') echo cccc ;;
    'rev-parse HEAD') echo cccccccccccccccccccccccccccccccccccccccc ;;
    *) exit 99 ;;
esac
SH
cat > "$tmp/bin/ssh" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "${*: -1}" >> "$CALL_LOG"
SH
printf '#!/usr/bin/env bash\nexit 0\n' > "$tmp/bin/rsync"
chmod +x "$tmp/bin/git" "$tmp/bin/ssh" "$tmp/bin/rsync"
assert_launcher() {
    cp "$1" "$tmp/local/scripts/box-check.sh"
    : > "$tmp/calls"
    CALL_LOG="$tmp/calls" PGWT_BOX=fixture PGWT_BOX_DIR=custom-root EPHEMERAL=0 OS=ubuntu \
        bash "$tmp/local/scripts/box-check.sh" > "$tmp/launcher.log" 2>&1 || return 1
    grep -Fq -- '-> custom-root/cccc ' "$tmp/launcher.log" || { fail 'detached HEAD directory must be short SHA'; return 1; }
    grep -Fq "cd 'custom-root/cccc'" "$tmp/calls" || { fail 'SSH detached destination'; return 1; }
    python3 - "$tmp/calls" <<'PY' || return 1
import sys
commands = [s for s in open(sys.argv[1]) if 'tests/run_all.sh --require-live' in s]
assert len(commands) == 1, 'missing/ambiguous build SSH'
line = commands[0]
assert line.index('flock 9 || exit') < line.index('bash scripts/box-build.sh aaaa cccc') < line.index('tests/run_all.sh --require-live'), 'build provenance must run under the existing flock'
PY
    echo 'PASS: detached HEAD uses/logs custom-root/cccc; build provenance runs inside existing lock'
}
assert_launcher "$ROOT/scripts/box-check.sh"
sed '/^\[\[ "\$branch" != HEAD \]\] || branch=/d' "$ROOT/scripts/box-check.sh" > "$tmp/shared-head.sh"
if assert_launcher "$tmp/shared-head.sh" > "$tmp/control.log" 2>&1; then fail 'shared-HEAD mutant escaped'; exit 1; fi
grep -Fq 'FAIL: detached HEAD directory must be short SHA' "$tmp/control.log"
echo 'PASS: negative control detached HEAD fails by name: directory must be short SHA'
echo 'OK: box build provenance offline checks'
