#!/usr/bin/env bash
# Offline #861 regression/negative controls. Run the real build helper with
# the REAL clean targets; only compilation/network/identity calls are shimmed.
# shellcheck disable=SC2016
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; return 1; }
real_git=$(command -v git)
real_make=$(command -v make)
export REAL_MAKE="$real_make"
mkdir -p "$tmp/bin" "$tmp/remote/tests" "$tmp/remote/scripts" "$tmp/local/scripts" "$tmp/local/tests"
cp "$ROOT/Makefile" "$tmp/remote/Makefile"
cp "$ROOT/tests/Makefile" "$tmp/remote/tests/Makefile"
cp "$ROOT/scripts/box-source-hash.py" "$tmp/remote/scripts/"
git init -q --bare "$tmp/objects"
printf 'synced source\n' > "$tmp/source.c"
blob=$(git --git-dir="$tmp/objects" hash-object -w "$tmp/source.c")
printf '100644 blob %s\tsource.c\n' "$blob" > "$tmp/tree-input"
tree_hash=$(git --git-dir="$tmp/objects" mktree < "$tmp/tree-input")
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
    printf '100644 blob %s\tsource.c\0' "$blob" > "$tmp/remote/.pgwt-box-source-manifest"
    rm -f "$tmp/remote/.pgwt-box-built-from"
    [[ "$1" == missing ]] || printf '%s\n' "$1" > "$tmp/remote/.pgwt-box-built-from"
}
run_build() {
    (cd "$tmp/remote"; EXPECT_CACHE="$2" FAIL_STAGE="${3:-}" bash "$1" "$tree_hash" cccc) > "$tmp/build.log" 2>&1
}
assert_cache() {
    local script="$1" previous="$2" expected="$3"
    seed "$previous"
    if ! run_build "$script" "$expected"; then
        cat "$tmp/build.log"; fail "$previous marker cache $expected"; return 1
    fi
    [[ $(cat "$tmp/remote/.pgwt-box-built-from") == "$tree_hash" ]] || { fail 'successful build marker'; return 1; }
    grep -Fq "box-check: commit=cccc tree-hash=$tree_hash built-from=$tree_hash" "$tmp/build.log" || { fail 'built-from header'; return 1; }
    if [[ "$expected" == reset ]]; then
        cmp "$tmp/remote/source.c" "$tmp/remote/pgwt-server" || { fail 'rebuilt server content'; return 1; }
        cmp "$tmp/remote/source.c" "$tmp/remote/build/main.o" || { fail 'rebuilt object content'; return 1; }
        [[ ! -e "$tmp/remote/include/vmlinux.h" && ! -e "$tmp/remote/include/pg_wait_tracer.skel.h" ]] || { fail 'generated BPF headers cleaned'; return 1; }
    fi
    echo "PASS: $previous marker -> $expected before build (NEWER binary, objects, libraries, ASan, Go)"
}
assert_failure() {
    local script="$1" stage="$2" rc=0
    seed "$tree_hash"
    run_build "$script" reset "$stage" || rc=$?
    [[ $rc == 42 ]] || { cat "$tmp/build.log"; fail "failed $stage build exit (got $rc)"; return 1; }
    [[ ! -e "$tmp/remote/.pgwt-box-built-from" ]] || { fail "failed $stage build left marker"; return 1; }
    if grep -q '^box-check: commit=' "$tmp/build.log"; then fail 'failed build emitted certified header'; return 1; fi
    echo "PASS: failed $stage build leaves no marker or certified header"
}
helper="$ROOT/scripts/box-build.sh"
assert_cache "$helper" bbbb reset
assert_cache "$helper" missing reset
assert_cache "$helper" "$tree_hash" reset
for stage in daemon build-tests pgwt-client; do assert_failure "$helper" "$stage"; done

# Mutation controls grade the exact same behavioral assertions by name.
sed '/^make clean-box$/d' "$helper" > "$tmp/no-clean.sh"
# Matching markers never authorise reuse, even after an out-of-band ASan build.
sed 's/^make clean-box$/[[ "$built_from" == "$tree_hash" ]] || make clean-box/' "$helper" > "$tmp/reuse.sh"
sed '/^rm -f "\$marker"$/d' "$helper" > "$tmp/keep-marker.sh"
for previous in bbbb missing "$tree_hash"; do
    mutant="$tmp/no-clean.sh"
    [[ "$previous" != "$tree_hash" ]] || mutant="$tmp/reuse.sh"
    if assert_cache "$mutant" "$previous" reset > "$tmp/control.log" 2>&1; then fail 'reuse/no-clean mutant escaped'; exit 1; fi
    grep -Fq 'FAIL: stale NEWER pgwt-server survived before build' "$tmp/control.log"
    echo "PASS: negative control $previous marker fails by name: stale NEWER pgwt-server survived before build"
done
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

# The announced tree is a real Git mktree result in a scratch object store,
# not a second implementation of the remote hash. Never run tree-hash.sh.
seed missing
[[ $(cd "$tmp/remote"; python3 scripts/box-source-hash.py .pgwt-box-source-manifest) == "$tree_hash" ]] || fail 'Git tree hash roundtrip'
echo 'PASS: synced bytes reproduce Git mktree hash without remote .git'
assert_source_refusal() {
    local script="$1" corruption="$2" rc=0
    seed "$tree_hash"
    case "$corruption" in
        bytes)
            cp -p "$tmp/remote/source.c" "$tmp/timestamp"
            printf 'forged source\n' > "$tmp/remote/source.c"
            touch -r "$tmp/timestamp" "$tmp/remote/source.c" ;;
        mode) chmod +x "$tmp/remote/source.c" ;;
        symlink) rm "$tmp/remote/source.c"; ln -s ../source.c "$tmp/remote/source.c" ;;
        missing) rm "$tmp/remote/source.c" ;;
        manifest) rm "$tmp/remote/.pgwt-box-source-manifest" ;;
    esac
    run_build "$script" reset || rc=$?
    if [[ $rc != 1 ]] || grep -q 'build cache reset' "$tmp/build.log"; then
        fail "$corruption source must be refused before clean/build"; return 1
    fi
    [[ ! -e "$tmp/remote/.pgwt-box-built-from" ]] || { fail 'source refusal left marker'; return 1; }
    grep -Fq "tree-hash=$tree_hash synced-tree-hash=" "$tmp/build.log" || { fail 'source refusal logs both hashes'; return 1; }
    if grep -q '^box-check: commit=' "$tmp/build.log"; then fail 'source refusal certified a build'; return 1; fi
    echo "PASS: $corruption source refused before clean/build with no certificate"
}
# Replace the whole verification command with the announced value, simulating
# the old trust-in-rsync behavior, including missing-file/manifest cases.
python3 - "$helper" "$tmp/unverified.sh" <<'PYCONTROL'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
start = text.index('if ! synced_hash=')
end = text.index('echo "box-check: build cache reset', start)
Path(sys.argv[2]).write_text(text[:start] + 'synced_hash="$tree_hash"\n' + text[end:])
PYCONTROL
for corruption in bytes mode symlink missing manifest; do
    assert_source_refusal "$helper" "$corruption"
    if assert_source_refusal "$tmp/unverified.sh" "$corruption" > "$tmp/control.log" 2>&1; then fail "$corruption verification mutant escaped"; exit 1; fi
    grep -Fq "FAIL: $corruption source must be refused before clean/build" "$tmp/control.log"
    echo "PASS: negative control $corruption source verification fails by name"
done

# Include symlinks, executable files, nested names, whitespace/newlines, and
# Git's special directory sort order in an independent Git plumbing roundtrip.
python3 -B - "$ROOT/scripts/box-source-hash.py" "$tmp" <<'PYHASH'
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
helper, temp = sys.argv[1:]
root = Path(temp, 'hash-fixture'); root.mkdir()
(root / 'a').mkdir()
(root / 'a' / 'nested').write_bytes(b'nested\0bytes')
(root / 'a.c').write_text('sort before directory a/')
(root / 'sp ace\nname').write_bytes(b'line\r\n')
(root / 'executable').write_text('#!/bin/sh\n'); (root / 'executable').chmod(0o755)
(root / 'link').symlink_to('a/nested')
env = dict(os.environ, GIT_DIR=str(Path(temp, 'objects')), GIT_INDEX_FILE=str(Path(temp, 'fixture-index')))
def git(*args, data=None):
    return subprocess.check_output(['git', *args], input=data, cwd=root, env=env).strip()
entries = []
for path in sorted(root.rglob('*')):
    if path.is_dir():
        continue
    content = os.fsencode(os.readlink(path)) if path.is_symlink() else path.read_bytes()
    mode = b'120000' if path.is_symlink() else b'100755' if path.stat().st_mode & 0o100 else b'100644'
    blob = git('hash-object', '-w', '--stdin', data=content)
    entries.append(mode + b' ' + blob + b'\t' + os.fsencode(str(path.relative_to(root))) + b'\0')
git('update-index', '-z', '--index-info', data=b''.join(entries))
expected = git('write-tree').decode()
manifest = root / 'manifest'
manifest.write_bytes(subprocess.check_output(['git', 'ls-tree', '-rz', expected], cwd=root, env=env))
spec = importlib.util.spec_from_file_location('box_source_hash', helper)
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
os.chdir(root)
assert module.source_hash(manifest) == expected, 'Git tree hash roundtrip: modes/symlinks/names'
print('PASS: Git hash roundtrip covers executable modes, symlinks, nested paths and unusual names')
PYHASH

# Invoke the WHOLE launcher with local transport shims. The shim runs the
# actual remote helper, not compilation/network. Two launchers are interleaved
# at barriers: X sync -> Y sync -> X build. No flock binary is needed on Mac;
# separate tests inspect that the real command still acquires the box flock.
real_rsync=$(command -v rsync)
export REAL_RSYNC="$real_rsync" FIXTURE_TMP="$tmp"
cat > "$tmp/bin/transport" <<'PYTRANSPORT'
#!/usr/bin/env python3
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
name = Path(sys.argv[0]).name
args = sys.argv[1:]
temp = Path(os.environ['FIXTURE_TMP'])
label = os.environ.get('RUN_LABEL', 'single')
with open(os.environ['CALL_LOG'], 'a') as log:
    log.write(name + ' ' + args[-1] + '\n')
if name == 'git':
    if args == ['rev-parse', '--abbrev-ref', 'HEAD']:
        print('HEAD')
    elif args == ['rev-parse', '--short', 'HEAD']:
        print('cccc')
    elif args == ['rev-parse', 'HEAD']:
        print('c' * 40)
    elif args[:2] == ['ls-tree', '-rz']:
        manifest = Path(os.environ['LAUNCH_MANIFEST']).read_bytes()
        if '--name-only' in args:
            manifest = b''.join(e.split(b'\t', 1)[1] + b'\0' for e in manifest.split(b'\0') if e)
        sys.stdout.buffer.write(manifest)
    else:
        sys.exit('unexpected git: ' + repr(args))
elif name == 'rsync':
    src, dst = args[-2:]
    if src == './tests/provision-runner.sh' or ':pgwt-provision/' in dst:
        sys.exit(0)
    if src.startswith('fixture:'):
        sys.exit(1)  # no live UI artifacts in this fixture
    dst = dst[len('fixture:'):] if dst.startswith('fixture:') else dst
    subprocess.run([os.environ['REAL_RSYNC'], *args[:-1], dst], check=True)
    if dst.endswith('/.pgwt-box-source-manifest'):
        if os.environ.get('TAMPER_SYNC') == '1':
            Path(dst).parent.joinpath('source.c').write_text('tampered source\n')
        (temp / (label + '.synced')).touch()
elif name == 'ssh':
    command = args[-1]
    if 'mktemp -d ' in command or 'FIXED-DIR-CONTROL' in command:
        subprocess.run(['bash', '-c', command], check=True)
    elif 'tests/run_all.sh --require-live' in command:
        directory = re.search(r"cd '([^']+)'", command).group(1)
        (temp / (label + '.directory')).write_text(directory)
        if os.environ.get('INTERLEAVE') == '1' and label == 'X':
            deadline = time.monotonic() + 8
            while not (temp / 'Y.synced').exists():
                if time.monotonic() > deadline:
                    sys.exit('FAIL: Y never synced before X build')
                time.sleep(0.01)
        build = re.search(r'bash scripts/box-build.sh ([0-9a-f]+) ([0-9a-f]+)', command)
        rc = subprocess.run(['bash', 'scripts/box-build.sh', *build.groups()], cwd=directory).returncode
        if rc == 0:
            shutil.copyfile(Path(directory, 'pgwt-server'), temp / (label + '.binary'))
        sys.exit(rc)
    elif command.startswith('sudo rm -rf -- '):
        directory = shlex.split(command)[4]
        shutil.rmtree(directory, ignore_errors=True)
    # Provisioning, host identity: no operations outside the scratch tree.
else:
    sys.exit('unexpected network tool: ' + name)
PYTRANSPORT
chmod +x "$tmp/bin/transport"
for command in git ssh rsync curl security hcloud; do ln -sf transport "$tmp/bin/$command"; done
for label in single X Y; do
    fixture="$tmp/local-$label"
    mkdir -p "$fixture/scripts" "$fixture/tests/results"
    cp "$helper" "$ROOT/scripts/box-source-hash.py" "$ROOT/scripts/box-check.sh" "$fixture/scripts/"
    cp "$ROOT/Makefile" "$fixture/Makefile"
    cp "$ROOT/tests/Makefile" "$fixture/tests/Makefile"
    cp "$tmp/remote/build-fixture.mk" "$fixture/"
    printf '%s source\n' "$label" > "$fixture/source.c"
    printf '%s tracked archive\n' "$label" > "$fixture/tests/results/archived.txt"
    printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$LAUNCH_TREE_HASH"\n' > "$fixture/scripts/tree-hash.sh"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$fixture/tests/hetzner-sweep.sh"
    chmod +x "$fixture/scripts/tree-hash.sh" "$fixture/tests/hetzner-sweep.sh"
done
# Use real Git plumbing before the transport's git stub, by absolute path.
for label in single X Y; do
    fixture="$tmp/local-$label"
    source_blob=$("$real_git" --git-dir="$tmp/objects" hash-object -w "$fixture/source.c")
    archive_blob=$("$real_git" --git-dir="$tmp/objects" hash-object -w "$fixture/tests/results/archived.txt")
    archive_tree=$(printf '100644 blob %s\tarchived.txt\n' "$archive_blob" | "$real_git" --git-dir="$tmp/objects" mktree)
    tests_tree=$(printf '040000 tree %s\tresults\n' "$archive_tree" | "$real_git" --git-dir="$tmp/objects" mktree)
    printf '100644 blob %s\tsource.c\n040000 tree %s\ttests\n' "$source_blob" "$tests_tree" > "$tmp/input-$label"
    "$real_git" --git-dir="$tmp/objects" mktree < "$tmp/input-$label" > "$tmp/hash-$label"
    printf '100644 blob %s\tsource.c\000100644 blob %s\ttests/results/archived.txt\0' "$source_blob" "$archive_blob" > "$tmp/manifest-$label"
done
launch() {
    local label="$1" script="$2" interleave="${3:-0}"
    cp "$script" "$tmp/local-$label/scripts/box-check.sh"
    CALL_LOG="$tmp/calls-$label" RUN_LABEL="$label" INTERLEAVE="$interleave" \
        LAUNCH_TREE_HASH="$(cat "$tmp/hash-$label")" LAUNCH_MANIFEST="$tmp/manifest-$label" \
        PGWT_BOX=fixture PGWT_BOX_DIR="$tmp/destination" EPHEMERAL=0 OS=ubuntu EXPECT_CACHE=reset \
        bash "$tmp/local-$label/scripts/box-check.sh" > "$tmp/launcher-$label.log" 2>&1
}
assert_launcher() {
    : > "$tmp/calls-single"
    launch single "$1" || { cat "$tmp/launcher-single.log"; return 1; }
    grep -Fq -- "-> $tmp/destination/cccc." "$tmp/launcher-single.log" || { fail 'detached HEAD directory must be short SHA'; return 1; }
    python3 - "$tmp/calls-single" <<'PY' || return 1
import sys
commands = [s for s in open(sys.argv[1]) if 'tests/run_all.sh --require-live' in s]
assert len(commands) == 1, 'missing/ambiguous build SSH'
line = commands[0]
assert line.index('flock 9 || exit') < line.index('bash scripts/box-build.sh') < line.index('tests/run_all.sh --require-live'), 'build provenance must run under the existing flock'
PY
    [[ ! -d "$(cat "$tmp/single.directory")" ]] || { fail 'private directory not pruned'; return 1; }
    echo 'PASS: detached HEAD uses/logs SHA plus unique suffix; helper inside existing lock; private directory pruned'
}
assert_launcher "$ROOT/scripts/box-check.sh"
echo 'PASS: full announced file set includes tracked tests/results archive bytes'
sed '/^rsync -azr --checksum /d' "$ROOT/scripts/box-check.sh" > "$tmp/no-overlay.sh"
if assert_launcher "$tmp/no-overlay.sh" > "$tmp/control.log" 2>&1; then fail 'missing announced file overlay escaped'; exit 1; fi
grep -Fq 'tests/results' "$tmp/control.log"
grep -Fq 'synced-tree-hash=unavailable; refusing build' "$tmp/control.log"
echo 'PASS: negative control missing tracked archive is refused by synced-byte hash'
rc=0
TAMPER_SYNC=1 launch single "$ROOT/scripts/box-check.sh" || rc=$?
[[ $rc == 1 ]] || fail 'tampered launcher must fail'
grep -Fq 'source hash mismatch; refusing build' "$tmp/launcher-single.log"
[[ ! -d "$(cat "$tmp/single.directory")" ]] || fail 'failed run private directory not pruned'
echo 'PASS: failed verification also prunes its private directory'
sed '/^\[\[ "\$branch" != HEAD \]\] || branch=/d' "$ROOT/scripts/box-check.sh" > "$tmp/shared-head.sh"
if assert_launcher "$tmp/shared-head.sh" > "$tmp/control.log" 2>&1; then fail 'shared-HEAD mutant escaped'; exit 1; fi
grep -Fq 'FAIL: detached HEAD directory must be short SHA' "$tmp/control.log"
echo 'PASS: negative control detached HEAD fails by name: directory must be short SHA'
sed "s/sudo rm -rf -- '\$remote_dir'/true/" "$ROOT/scripts/box-check.sh" > "$tmp/no-prune.sh"
if assert_launcher "$tmp/no-prune.sh" > "$tmp/control.log" 2>&1; then fail 'no-prune mutant escaped'; exit 1; fi
grep -Fq 'FAIL: private directory not pruned' "$tmp/control.log"
echo 'PASS: negative control private directory pruning fails by name'

assert_interleaved() {
    local script="$1" first_pid second_pid first_rc=0 second_rc=0
    rm -f "$tmp/X.synced" "$tmp/Y.synced" "$tmp/X.binary" "$tmp/Y.binary" "$tmp/X.directory" "$tmp/Y.directory"
    launch X "$script" 1 & first_pid=$!
    # Start Y only after X has synced, but before X is allowed to build.
    for _ in {1..200}; do [[ ! -e "$tmp/X.synced" ]] || break; sleep 0.01; done
    launch Y "$script" 1 & second_pid=$!
    wait "$first_pid" || first_rc=$?
    wait "$second_pid" || second_rc=$?
    [[ -e "$tmp/X.synced" && -e "$tmp/Y.synced" ]] || { fail 'interleave barriers not reached'; return 1; }
    [[ $(cat "$tmp/X.directory") != "$(cat "$tmp/Y.directory")" ]] || { fail 'interleaved runs shared a source directory'; return 1; }
    [[ $first_rc == 0 && $second_rc == 0 ]] || { cat "$tmp/launcher-X.log" "$tmp/launcher-Y.log"; fail 'interleaved launcher failed'; return 1; }
    cmp "$tmp/local-X/source.c" "$tmp/X.binary" || { fail 'X built Y sources'; return 1; }
    cmp "$tmp/local-Y/source.c" "$tmp/Y.binary" || { fail 'Y built X sources'; return 1; }
    [[ ! -d "$(cat "$tmp/X.directory")" && ! -d "$(cat "$tmp/Y.directory")" ]] || { fail 'interleaved private directories not pruned'; return 1; }
    echo 'PASS: interleaved X sync -> Y sync -> X build: both build their own bytes in distinct, pruned directories'
}
assert_interleaved "$ROOT/scripts/box-check.sh"
# Same branch/detached SHA with a fixed directory reproduces the race. The
# independent synced-byte verification refuses X rather than certifying Y.
python3 - "$ROOT/scripts/box-check.sh" "$tmp/fixed-directory.sh" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
before = "mktemp -d '$remote_root/$branch.XXXXXXXXXX'"
after = "mkdir -p '$remote_root/$branch.FIXED-DIR-CONTROL'; echo '$remote_root/$branch.FIXED-DIR-CONTROL'"
assert text.count(before) == 1
Path(sys.argv[2]).write_text(text.replace(before, after))
PY
if assert_interleaved "$tmp/fixed-directory.sh" > "$tmp/control.log" 2>&1; then fail 'shared directory mutant escaped'; exit 1; fi
grep -Fq 'FAIL: interleaved runs shared a source directory' "$tmp/control.log"
echo 'PASS: negative control fixed remote directory fails by name: interleaved runs shared a source directory'

# Check actual check.sh invocations, using the same assertion for the real
# driver and each mutant. Comments cannot satisfy the registration assertion.
python3 - "$ROOT/scripts/check.sh" <<'PY'
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text()
commands = ('run bash tests/test_box_check_build_provenance.sh',
            'run bash scripts/lead/selftest.sh')
def check_wiring(source):
    for command in commands:
        assert re.search(r'^' + re.escape(command) + r'$', source, re.M), 'missing Mac offline gate: ' + command
check_wiring(text)
print('PASS: check.sh invokes both offline self-tests')
for command in commands:
    mutant = text.replace(command, '# invocation removed')
    try:
        check_wiring(mutant)
    except AssertionError as error:
        assert str(error) == 'missing Mac offline gate: ' + command
        print('PASS: negative control missing Mac gate fails by name: ' + command)
    else:
        raise AssertionError('wiring negative control escaped: ' + command)
PY
echo 'OK: box build provenance offline checks'
