#!/usr/bin/env bash
# Self-test and bypass suite for the per-section registration guard (#686).
# Pure bash fixtures + the checker; no compiler, binaries, network or Linux.
# Every mutation lives under mktemp, never in the three acceptance suites.
# Usage: tests/test_check_section_registration.sh [--mutants] (run from anywhere)
# --mutants runs the exhaustive legacy-scope dispatcher sweep instead of fixtures.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)" || exit 1
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)" || exit 1
GUARD="$REPO_DIR/scripts/check-section-registration.sh"
tests_run=0
tests_failed=0

report() {
    tests_run=$((tests_run + 1))
    if [[ "$1" == true ]]; then
        echo "  PASS: $2"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $2"
        printf '%s\n' "${out:-}" >&2
    fi
}

tmpdir="$(mktemp -d)" || exit 1
trap 'rm -rf "$tmpdir"' EXIT

if [[ "${1:-}" == --mutants ]]; then
    python3 - "$REPO_DIR" <<'PY'
import bisect
from collections import Counter
import contextlib
import importlib.util
import io
from pathlib import Path
import sys

repo = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('guard', repo / 'scripts/check-section-registration.py')
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)
total = false_greens = suites = multi_site = 0
for path in sorted((repo / 'tests').glob('test_*.c')):
    raw = path.read_text()
    code, strings = guard.lex(raw)
    # Freeze #700's label grammar, not the widened scope. New banner forms
    # must not make a legacy dispatcher deletion equivalent to the control.
    legacy = [(pos, guard.LABEL.match(literal))
              for pos, literal, _, _ in guard.printf_literals(code, strings)]
    legacy = [pos for pos, match in legacy if match]
    if not legacy:
        continue
    suites += 1
    newlines = [i for i, c in enumerate(raw) if c == '\n']
    line_at = lambda pos: bisect.bisect_left(newlines, pos) + 1
    definitions = guard.functions(code, line_at)
    owners = {name for pos in legacy for name, fn in definitions.items()
              if fn.body_start <= pos < fn.body_end} - {'main'}
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        assert not guard.check_file(raw, path.name)[2], f'control is not green: {path.name}'
    counts = Counter()
    for owner in sorted(owners):
        sites = {line_at(call.start()) for fn in definitions.values()
                 for call in guard.CALL.finditer(code, fn.body_start, fn.body_end)
                 if call[1] == owner}
        assert sites, f'no dispatcher for {path.name}:{owner}'
        lines = raw.splitlines(keepends=True)
        for line in sites:
            lines[line - 1] = guard.mask(lines[line - 1])
        mutant = ''.join(lines)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            _, markers, failed = guard.check_file(mutant, path.name)
        # Require the intended unreachable-owner finding, not a parse error,
        # lost marker, or an unrelated failure. Multiple incoming sites are
        # all removed to disconnect a shared owner, as reachability requires.
        red = markers > 0 and failed and f'unreachable owner {owner}:' in err.getvalue()
        total += 1
        multi_site += len(sites) > 1
        false_greens += not red
        counts['red' if red else 'FALSE GREEN'] += 1
        print(f'{"RED" if red else "FALSE GREEN"}: {path.name}: {owner}; deleted lines {sorted(sites)}')
        if not red:
            print(out.getvalue() + err.getvalue())
    print(f'SUITE: {path.name}: {dict(counts)}')
print(f'LEGACY DISPATCHER MUTANTS: {total} across {suites} suites; '
      f'{total - multi_site} single-site, {multi_site} shared-owner disconnect; '
      f'{false_greens} false greens')
assert suites >= 22 and total >= 172, 'legacy-scope mutation census unexpectedly shrank'
sys.exit(bool(false_greens))
PY
    exit $?
fi

# Two differently shaped headers in one owner, with a continued first
# literal. Escaped quotes, character braces and multiline comments must
# preserve source positions without manufacturing definitions/call edges.
write_suite() {
    local repo="$1" main_body="$2" dispatch_body="${3:-}"
    mkdir -p "$repo/tests" || exit 1
    cat > "$repo/tests/test_fixture.c" <<'EOF'
static void section(void)
{
    printf("--- 1. first section ---\n");
    assert(1);
    printf("  2ab. continued section "
           "title\n");
    char brace = '}';
    const char *quoted = "\" } { ";
    /* } fake(void) {
       printf("99. a comment is not a marker\n"); */
    // printf("98. neither is this comment\n");
    puts("printf(\"97. nor this string\");");
}
static void dispatch(void)
{
EOF
    printf '%s\n' "$dispatch_body" >> "$repo/tests/test_fixture.c"
    cat >> "$repo/tests/test_fixture.c" <<'EOF'
}
int main(void)
{
EOF
    printf '%s\n' "$main_body" >> "$repo/tests/test_fixture.c"
    printf '    return 0;\n}\n' >> "$repo/tests/test_fixture.c"
}

green() {
    out=$(bash "$GUARD" "$1" 2>&1); rc=$?
    if [[ $rc -eq 0 && "$out" == *'OK:'* ]]; then
        report true "$2"
    else
        report false "$2 (exit $rc)"
    fi
}

red() {
    out=$(bash "$GUARD" "$1" 2>&1); rc=$?
    if [[ $rc -eq 1 && "$out" != *'OK:'* && "$out" == *"$3"* &&
          ( "$out" == *'FAIL:'* || "$out" == *'REFUSING:'* ) ]]; then
        report true "$2"
    else
        report false "$2 (exit $rc; expected diagnostic: $3)"
    fi
}

repo="$tmpdir/direct"
write_suite "$repo" '    section();'
green "$repo" 'direct dispatcher control passes'
if [[ "$out" == *'2 markers, 1 owners'* &&
      "$out" == *'OK: 1 of 1 tests/test_*.c in scope (0 out of scope), 1 owners, 2 markers;'* ]]; then
    report true 'both label forms, continued literal, multiple labels per owner; fake markers ignored'
else
    report false 'marker and owner counts must be exact'
fi
write_suite "$repo" ''
red "$repo" 'deleted main dispatcher line goes RED' 'unreachable owner section: sections 1 (line 3), 2ab (line 5)'

write_suite "$repo" '    dispatch();' '    section();'
green "$repo" 'sub-dispatcher control passes'
write_suite "$repo" '    dispatch();' ''
red "$repo" 'deleted sub-dispatcher line goes RED' 'unreachable owner section'

write_suite "$repo" $'\n    /* section(); is no longer dispatched */'
red "$repo" 'next-line comment cannot replace deleted dispatcher' 'unreachable owner section'
write_suite "$repo" '    puts("section();");'
red "$repo" 'string literal cannot replace deleted dispatcher' 'unreachable owner section'
write_suite "$repo" '    if (0) section();'
red "$repo" 'if (0) call cannot satisfy reachability' 'unreachable owner section'
write_suite "$repo" $'#if 0\n    section();\n#endif'
red "$repo" '#if 0 call cannot satisfy reachability' 'unreachable owner section'
write_suite "$repo" $'#if 0\n#if SOMETHING\n    section();\n#endif\n#endif'
red "$repo" 'nested #if 0 block cannot supply a call edge' 'unreachable owner section'
write_suite "$repo" '' '    dispatch(); section();'
red "$repo" 'unreachable recursive dispatcher cannot satisfy reachability' 'unreachable owner section'

mkdir -p "$tmpdir/empty/tests" "$tmpdir/no-tests"
red "$tmpdir/empty" 'empty test_*.c glob refuses' 'matched no files'
red "$tmpdir/no-tests" 'missing tests directory refuses' 'REFUSING:'
red "$tmpdir/nonexistent" 'nonexistent repository refuses' 'REFUSING:'

mkdir -p "$tmpdir/no-main/tests"
cat > "$tmpdir/no-main/tests/test_fixture.c" <<'EOF'
static void section(void)
{
    printf("1. no main\n");
}
EOF
red "$tmpdir/no-main" 'markers without main refuse' 'no findable int main'

mkdir -p "$tmpdir/no-owner/tests"
cat > "$tmpdir/no-owner/tests/test_fixture.c" <<'EOF'
printf("1. above every function\n");
int main(void)
{
    return 0;
}
EOF
red "$tmpdir/no-owner" 'marker above every definition refuses' 'cannot determine owner definition'

# A prototype and call do not define an owner for the surviving header.
# With no body in this file, the header has no determinable owner: refuse.
mkdir -p "$tmpdir/undefined/tests"
cat > "$tmpdir/undefined/tests/test_fixture.c" <<'EOF'
static void section(void);
printf("1. section has no definition in this file\n");
int main(void)
{
    section();
    return 0;
}
EOF
red "$tmpdir/undefined" 'called but undefined owner (prototype only) refuses' 'cannot determine owner definition'

mkdir -p "$tmpdir/undecodable/tests" "$tmpdir/unreadable/tests"
printf '\377\n' > "$tmpdir/undecodable/tests/test_fixture.c"
red "$tmpdir/undecodable" 'undecodable file refuses, even without markers' 'REFUSING:'
# A dangling symlink is unreadable regardless of whether this runs as root.
ln -s "$tmpdir/no-such-file" "$tmpdir/unreadable/tests/test_fixture.c"
red "$tmpdir/unreadable" 'unreadable candidate refuses rather than skipping' 'REFUSING:'

mkdir -p "$tmpdir/no-python" "$tmpdir/shim"
out=$(PATH="$tmpdir/no-python" /bin/bash "$GUARD" "$repo" 2>&1); rc=$?
if [[ $rc -eq 1 && "$out" == *"missing required tool 'python3'"* && "$out" != *'OK:'* ]]; then
    report true 'wrapper refuses with python3 absent from PATH'
else
    report false "missing python3 refusal (exit $rc)"
fi
# dirname is the wrapper's only external bootstrap command after python3.
ln -s /usr/bin/dirname "$tmpdir/shim/dirname"
for status in 127 126; do
    printf '#!/bin/bash\nexit %s\n' "$status" > "$tmpdir/shim/python3"
    chmod +x "$tmpdir/shim/python3"
    out=$(PATH="$tmpdir/shim" /bin/bash "$GUARD" "$repo" 2>&1); rc=$?
    if [[ $rc -eq 1 && "$out" == *'REFUSING:'* && "$out" == *'could not run the checker'* &&
          "$out" == *"python3 exited $status"* && "$out" != *'OK:'* ]]; then
        report true "python3 exiting $status yields explicit could-not-run refusal"
    else
        report false "python3 shim $status refusal (exit $rc)"
    fi
done

mkdir -p "$tmpdir/no-markers/tests"
cat > "$tmpdir/no-markers/tests/test_unsectioned.c" <<'EOF'
int main(void)
{
    printf("there are no numbered sections\n");
    return 0;
}
EOF
green "$tmpdir/no-markers" 'unsectioned suite may be outside scope'
if [[ "$out" == *'OUT OF SCOPE: tests/test_unsectioned.c (0 markers; no supported printf section banner; no section-like heading detected; not verified)'* &&
      "$out" == *'OK: 0 of 1 tests/test_*.c in scope (1 out of scope), 0 owners, 0 markers;'* &&
      "$out" != *'IN SCOPE:'* ]]; then
    report true 'the-thing-is-absent is named OUT OF SCOPE and never counted as verified'
else
    report false 'zero-marker suite must be explicitly outside verified scope'
fi

# Each newly accepted header shape must register its owner, then fail when
# that owner's dispatcher line disappears. Keep the literal C source intact.
write_header_suite() {
    local repo="$1" header="$2" main_body="$3"
    mkdir -p "$repo/tests" || exit 1
    {
        printf 'static void section(void)\n{\n'
        printf '%s\n' "$header"
        printf '    assert(1);\n'
        printf '}\nint main(void)\n{\n'
        printf '%s\n' "$main_body"
        printf '    return 0;\n}\n'
    } > "$repo/tests/test_fixture.c"
}

# Comments can describe regions spanning helpers and data. List the evidence
# without pretending that such a region identifies a dispatched section.
repo="$tmpdir/comment-regions"
mkdir -p "$repo/tests" || exit 1
cat > "$repo/tests/test_comment_regions.c" <<'EOF'
/* ── Section 1: an example in a design note, not a runtime section ── */
/* printf("--- 2. commented-out example ---\n");
   printf("--- named example ---\n"); printf("[A1] example\n"); */
// printf("-- 3: another example --\n");
int main(void)
{
    const char *example = "printf(\"=== §4 example ===\");";
#if 0
    printf("-- 5/6: disabled example --\n");
    printf("\n§7 disabled example\n");
#endif
    return 0;
}
EOF
green "$repo" 'header-like comments and quoted examples do not create scope'
if [[ "$out" == *'OUT OF SCOPE: tests/test_comment_regions.c (0 markers; no supported printf section banner; section-like comments at lines 1; not verified)'* &&
      "$out" != *'IN SCOPE:'* ]]; then
    report true 'comment-only organisation is explicitly listed with its source line'
else
    report false 'comment-only organisation needs a distinct out-of-scope reason'
fi

repo="$tmpdir/unknown-header"
write_header_suite "$repo" '    printf("Section 1 > unsupported banner\n");' '    section();'
green "$repo" 'unfamiliar runtime header is visibly unverified'
if [[ "$out" == *'unrecognised section-like printf text at lines 3; not verified)'* &&
      "$out" != *'IN SCOPE:'* ]]; then
    report true 'unrecognised runtime header has a distinct out-of-scope reason'
else
    report false 'unrecognised runtime header must not look sectionless'
fi

# This inventory was surveyed on master before widening (#710): 22 suites
# used the old dot labels; 30 more have runtime banners in the forms below;
# 8 have only decorated comment regions (helpers, phases or sections), one
# has unlabelled === banners, and 9 have no formal headings. Comments remain hints,
# not markers. Named banners must have BOTH wrappers; suite titles (===
# without a numeric/§ label) and formatted diagnostic output are excluded.
header_shapes=(leading-escape dash-wrapper range next-line-literal colon
               colon-pair colon-list colon-range equals-dot section-sign
               section-pair bracket bracket-alpha bracket-alnum named named-split)
header_shape() {
    local shape="$1"
    label=1
    case "$shape" in
        leading-escape) header='    printf("\n\t  1. leading whitespace\n");' ;;
        dash-wrapper) header='    printf("-- 1. dash wrapper --\n");' ;;
        range) header='    printf("1-3. three sections\n");'; label=1-3 ;;
        next-line-literal) header=$'    printf(\n        "--- 1. next source line ---\\n");' ;;
        colon) header='    printf("-- 1: colon banner --\n");' ;;
        colon-pair) header='    printf("-- 1/2: paired sections --\n");'; label=1/2 ;;
        colon-list) header='    printf("-- 8h,i,l: listed sections --\n");'; label=8h,i,l ;;
        colon-range) header='    printf("-- 8b-d: ranged sections --\n");'; label=8b-d ;;
        equals-dot) header='    printf("=== 1. equals banner ===\n");' ;;
        section-sign) header='    printf("=== §1 section sign ===\n");' ;;
        section-pair) header='    printf("\n§1/§2 paired sections\n");'; label=1/2 ;;
        bracket) header='    printf("\n[1] bracketed section\n");' ;;
        bracket-alpha) header='    printf("\n[A] alphabetic section\n");'; label=A ;;
        bracket-alnum) header='    printf("\n[A1] alphanumeric section\n");'; label=A1 ;;
        named) header='    printf("--- named section ---\n");'; label='named section' ;;
        named-split) header=$'    printf("--- named " /* masked */\n           "section ---\\n");'; label='named section' ;;
    esac
}
for shape in "${header_shapes[@]}"; do
    header_shape "$shape"
    repo="$tmpdir/header-$shape"
    write_header_suite "$repo" "$header" '    section();'
    green "$repo" "$shape header control passes"
    if [[ "$out" == *'IN SCOPE: tests/test_fixture.c: 1 markers, 1 owners'* &&
          "$out" == *'OK: 1 of 1 tests/test_*.c in scope (0 out of scope), 1 owners, 1 markers;'* ]]; then
        report true "$shape header is IN SCOPE with exactly one marker"
    else
        report false "$shape header must register exactly one owner and marker"
    fi
    write_header_suite "$repo" "$header" ''
    red "$repo" "$shape header dispatcher deletion goes RED and names $label" \
        "unreachable owner section: sections $label (line "
done

# Each prose form is its own file so every one must be explicitly OUT OF
# SCOPE, and their combined fixture must have no registered owners/markers.
repo="$tmpdir/prose-controls"
prose_forms=(rows decimal done sub-assertion suite-title result-title bare-colon
             missing-wrapper format-title quoted-comment bracket-prose wrong-printf-argument)
for prose in "${prose_forms[@]}"; do
    case "$prose" in
        rows) header='    printf("3 rows\n");' ;;
        decimal) header='    printf("1.5 seconds elapsed\n");' ;;
        done) header='    printf("\n  done.\n");' ;;
        sub-assertion) header='    printf("ok: 5h. a sub-assertion message\n");' ;;
        suite-title) header='    printf("=== test_fixture ===\n");' ;;
        result-title) header='    printf("=== %d/%d checks passed ===\n", 1, 1);' ;;
        bare-colon) header='    printf("534: an assertion message\n");' ;;
        missing-wrapper) header='    printf("--- ordinary prose without a closing delimiter\n");' ;;
        format-title) header='    printf("--- %s ---\n", "fixture data");' ;;
        quoted-comment) header='    printf("/* ── Section 1: quoted design note ── */\n");' ;;
        bracket-prose) header='    printf("[index] an array index\n");' ;;
        wrong-printf-argument) header='    printf("%s", "--- named section ---\n");' ;;
    esac
    write_header_suite "$repo" "$header" '    section();'
    mv "$repo/tests/test_fixture.c" "$repo/tests/test_$prose.c" || exit 1
done
green "$repo" 'prose negative controls pass'
for prose in "${prose_forms[@]}"; do
    if [[ "$out" == *"OUT OF SCOPE: tests/test_$prose.c (0 markers; "* ]]; then
        report true "prose negative control stays OUT OF SCOPE: $prose"
    else
        report false "prose negative control must stay OUT OF SCOPE: $prose"
    fi
done
if [[ "$out" == *'OK: 0 of 12 tests/test_*.c in scope (12 out of scope), 0 owners, 0 markers;'* &&
      "$out" != *'IN SCOPE:'* ]]; then
    report true 'prose-only fixture has 0 files, 0 owners, 0 markers'
else
    report false 'prose-only fixture must produce no markers'
fi

# Dispatch-site headers live in main rather than in their helper. Windows
# are separate even when both sections call the same helper.
write_dispatch_site_suite() {
    local repo="$1" main_body="$2"
    mkdir -p "$repo/tests" || exit 1
    cat > "$repo/tests/test_fixture.c" <<'EOF'
static void test_tick_close_order(void)
{
    assert(1);
}
int main(void)
{
EOF
    printf '%s\n' "$main_body" >> "$repo/tests/test_fixture.c"
    # Falling off main returns 0. Leave the last window free of unrelated
    # code so deleting its only call really produces a whitespace-only body.
    printf '}\n' >> "$repo/tests/test_fixture.c"
}

# Every added form must also preserve dispatch windows in main. Deleting
# one site must not borrow the next section's call to the same delegate.
for shape in "${header_shapes[@]}"; do
    header_shape "$shape"
    repo="$tmpdir/window-$shape"
    write_dispatch_site_suite "$repo" "$header"$'\n    test_tick_close_order();\n    printf("99. next window\\n");\n    test_tick_close_order();'
    green "$repo" "$shape dispatch-window control passes"
    write_dispatch_site_suite "$repo" "$header"$'\n    printf("99. next window\\n");\n    test_tick_close_order();'
    red "$repo" "$shape deleted dispatch window goes RED" "section $label header at line "
done

repo="$tmpdir/dispatch-site"
write_dispatch_site_suite "$repo" $'    printf("\\n-- 4. tick and close --\\n");\n    test_tick_close_order();'
green "$repo" 'dispatch-site header and delegate control passes'
write_dispatch_site_suite "$repo" $'    printf("\\n-- 4. tick and close --\\n");\n'
red "$repo" 'deleted dispatch-site call goes RED with the empty-body diagnostic' \
    'section 4 header at line 7 has an empty body -- its dispatch site is missing'
write_dispatch_site_suite "$repo" $'    printf("4. masked comment\\n");\n    /* test_tick_close_order(); */'
red "$repo" 'comment-only section body is empty after masking' \
    'section 4 header at line 7 has an empty body -- its dispatch site is missing'
write_dispatch_site_suite "$repo" $'    printf("4. disabled call\\n");\n#if 0\n    test_tick_close_order();\n#endif'
red "$repo" '#if 0-only section body is empty after masking' \
    'section 4 header at line 7 has an empty body -- its dispatch site is missing'

repo="$tmpdir/shared-delegate"
write_dispatch_site_suite "$repo" $'    printf("1. first use\\n");\n    test_tick_close_order();\n    printf("2. second use\\n");\n    test_tick_close_order();'
green "$repo" 'two headers may delegate to the same helper in separate windows'
write_dispatch_site_suite "$repo" $'    printf("1. first use\\n");\n\n    printf("2. second use\\n");\n    test_tick_close_order();'
red "$repo" 'deleted first shared-delegate call cannot borrow the second window' \
    'section 1 header at line 7 has an empty body -- its dispatch site is missing'
write_dispatch_site_suite "$repo" $'    printf("1. first use\\n");\n    test_tick_close_order();\n    printf("2. second use\\n");\n'
red "$repo" 'deleted second shared-delegate call cannot borrow the first window' \
    'section 2 header at line 9 has an empty body -- its dispatch site is missing'
write_dispatch_site_suite "$repo" $'    printf("1. first use\\n");\n    if (0) test_tick_close_order();\n    printf("2. second use\\n");\n    test_tick_close_order();'
red "$repo" 'dead delegate site cannot borrow reachability from another window' \
    'unreachable section-1 delegate test_tick_close_order (header line 7)'

repo="$tmpdir/inline-section"
write_dispatch_site_suite "$repo" $'    printf("4. inline assertions\\n");\n    assert(1);'
green "$repo" 'inline assertions in main need no local delegate'

# Widening brings alloc_tune's mutually exclusive mains into scope. Prove
# each branch independently; the other main must not lend it a call edge.
repo="$tmpdir/conditional-main"
mkdir -p "$repo/tests" || exit 1
cat > "$repo/tests/test_fixture.c" <<'EOF'
static void section(void)
{
    printf("--- conditional section ---\n");
    assert(1);
}
#ifdef PLATFORM
int main(void)
{
    section(); /* first arm */
}
#else
int main(void)
{
    section(); /* second arm */
}
#endif
EOF
cp "$repo/tests/test_fixture.c" "$tmpdir/conditional-control.c" || exit 1
green "$repo" 'mutually exclusive mains are checked independently'
for arm in first second; do
    sed "/section(); .* $arm arm/d" "$tmpdir/conditional-control.c" > "$repo/tests/test_fixture.c" || exit 1
    red "$repo" "conditional $arm arm cannot borrow the other main's dispatcher" \
        'unreachable owner section: sections conditional section'
done
sed '/^#ifdef PLATFORM$/d; /^#else$/d; /^#endif$/d' "$tmpdir/conditional-control.c" > "$repo/tests/test_fixture.c" || exit 1
red "$repo" 'duplicate mains without mutually exclusive branches still refuse' \
    'duplicate function definition: main'

# Registration of this registration check. Match actual invocation lines,
# not comments/mentions; resolve from this test's location, never from PWD.
check_text=$(< "$REPO_DIR/scripts/check.sh")
for command in scripts/check-section-registration.sh tests/test_check_section_registration.sh; do
    wired=false
    while IFS= read -r line; do
        if [[ "$line" == "run $command" ]]; then wired=true; fi
    done <<< "$check_text"
    report "$wired" "scripts/check.sh invokes $command"
done

green "$REPO_DIR" 'real repository passes'
if [[ "$out" == 'SCOPE RATCHET: base '* || "$out" == *$'\nSCOPE RATCHET: base '* ]]; then
    report true 'real repository runs the scope ratchet against a base'
else
    report false 'real repository must print SCOPE RATCHET: base '
fi
for suite in test_idle_summary.c test_agg_raw_crosscheck.c test_cpu_split_parity.c \
             test_pid_cap.c test_probe_bounds.c test_straddler_accounting.c; do
    if [[ "$out" == *"IN SCOPE: tests/$suite:"* ]]; then
        report true "real acceptance suite remains in scope: $suite"
    else
        report false "real acceptance suite must remain in scope: $suite"
    fi
done
if [[ "$out" == *'IN SCOPE: tests/test_straddler_accounting.c: 5 markers, 1 owners'* ]]; then
    report true 'real straddler suite counts all 5 headers, including 6b-h'
else
    report false 'real straddler suite must count all 5 headers, including 6b-h'
fi

# Permanent issue-acceptance mutations use the real current sources, copied
# first. Refuse a stale sed: a renamed dispatcher must never approve blind.
while IFS='|' read -r suite dispatcher owner diagnostic; do
    repo="$tmpdir/acceptance-${suite%.c}"
    mkdir -p "$repo/tests" || exit 1
    copy="$repo/tests/$suite"
    cp "$REPO_DIR/tests/$suite" "$copy" || exit 1
    green "$repo" "real-file mutation control passes: $suite"
    sed "s|^$dispatcher\$||" "$copy" > "$tmpdir/mutated.c" || exit 1
    if cmp -s "$copy" "$tmpdir/mutated.c"; then
        out="REFUSING: mutation did not match any line in $suite: [$dispatcher]"
        report false "real-file $suite: mutation did not match any line"
        continue
    fi
    report true "real-file dispatcher mutation matched: $suite"
    mv "$tmpdir/mutated.c" "$copy" || exit 1
    red "$repo" "real-file dispatcher deletion goes RED: $suite ($owner)" \
        "${diagnostic:-unreachable owner $owner: sections }"
done <<'EOF'
test_idle_summary.c|    test_sessions_overflow_is_reported();|test_sessions_overflow_is_reported
test_agg_raw_crosscheck.c|    div553_measured_cpu_identity(origin);|div553_measured_cpu_identity
test_cpu_split_parity.c|        test_windows(&fx);|test_windows
test_straddler_accounting.c|    test_tick_close_order();|test_tick_close_order|section 4 header at line 636 has an empty body -- its dispatch site is missing
test_straddler_accounting.c|    test_placement_and_conservation(1);|test_placement_and_conservation|section 1-3 header at line 630 has an empty body -- its dispatch site is missing
test_open_cpu_accounting.c|    test_notes_counter();   /\* prints its own banner -- see #686 there \*/|test_notes_counter
test_open_cpu_accounting.c|    test_placement_split_conservation();|test_placement_split_conservation|section 1/2 header at line
EOF

# Commit actual baselines so a working-tree scope loss cannot silently turn
# a previously verified suite into an unverified one. No global git config.
init_repo() {
    git -C "$1" init -q || exit 1
}
commit_base() {
    git -C "$1" add . || exit 1
    git -C "$1" -c user.email=scope@example.test -c user.name='Scope self-test' \
        -c core.hooksPath=/dev/null commit -qm 'scope baseline' || exit 1
    git -C "$1" branch -M master || exit 1
}

repo="$tmpdir/scope-real-cpu"
mkdir -p "$repo/tests" || exit 1
copy="$repo/tests/test_cpu_split_parity.c"
cp "$REPO_DIR/tests/test_cpu_split_parity.c" "$copy" || exit 1
init_repo "$repo"
commit_base "$repo"
green "$repo" 'scope ratchet real cpu-split committed control passes'
sed -E 's/(--- )[0-9]+\. /\1/g' "$copy" > "$tmpdir/stripped.c" || exit 1
if cmp -s "$copy" "$tmpdir/stripped.c"; then
    out='REFUSING: cpu-split numeric-prefix mutation did not match any header'
    report false 'scope ratchet header-prefix mutation must match the real cpu-split source'
else
    report true 'scope ratchet header-prefix mutation matched the real cpu-split source'
    mv "$tmpdir/stripped.c" "$copy" || exit 1
    green "$repo" 'real cpu-split losing numeric prefixes retains named-banner coverage'
    sed 's|^        test_windows(&fx);$||' "$copy" > "$tmpdir/mutated.c" || exit 1
    if cmp -s "$copy" "$tmpdir/mutated.c"; then
        out='REFUSING: cpu-split test_windows dispatcher mutation did not match'
        report false 'scope ratchet dispatcher mutation must match the real cpu-split source'
    else
        report true 'scope ratchet dispatcher mutation matched the real cpu-split source'
        mv "$tmpdir/mutated.c" "$copy" || exit 1
        red "$repo" 'real cpu-split prefix loss plus deleted test_windows stays RED' \
            'unreachable owner test_windows: sections '
    fi
    # With named banners now supported, stripping only numbers is an
    # equivalent style change. Remove the wrappers too to exercise an
    # ACTUAL loss of scope, rather than weakening or deleting the ratchet.
    sed -E 's/--- //g; s/ ---//g' "$copy" > "$tmpdir/unheaded.c" || exit 1
    mv "$tmpdir/unheaded.c" "$copy" || exit 1
    red "$repo" 'real cpu-split losing every supported banner goes RED' \
        'tests/test_cpu_split_parity.c left scope'
fi

repo="$tmpdir/scope-growth"
write_suite "$repo" '    section();'
cp "$tmpdir/no-markers/tests/test_unsectioned.c" "$repo/tests/test_unsectioned.c" || exit 1
init_repo "$repo"
commit_base "$repo"
write_header_suite "$tmpdir/scope-new-header" '    printf("1. newly sectioned\n");' '    section();'
cp "$tmpdir/scope-new-header/tests/test_fixture.c" "$repo/tests/test_unsectioned.c" || exit 1
green "$repo" 'scope ratchet permits out-of-scope at base to become in-scope'
cp "$tmpdir/scope-new-header/tests/test_fixture.c" "$repo/tests/test_new.c" || exit 1
green "$repo" 'scope ratchet permits a brand-new sectioned file absent at base'
rm "$repo/tests/test_fixture.c" || exit 1
red "$repo" 'scope ratchet rejects deletion of a file in scope at base' \
    'tests/test_fixture.c left scope'

# Deleting the only suite must still name the scope loss, not just the empty glob.
repo="$tmpdir/scope-delete-only"
write_suite "$repo" '    section();'
init_repo "$repo"
commit_base "$repo"
rm "$repo/tests/test_fixture.c" || exit 1
red "$repo" 'scope ratchet names deletion even when no current suites remain' \
    'tests/test_fixture.c left scope'

repo="$tmpdir/scope-no-base"
write_suite "$repo" '    section();'
init_repo "$repo"
red "$repo" 'scope ratchet refuses a git repo without a resolvable base' 'no comparable base ref'

repo="$tmpdir/scope-no-git"
write_suite "$repo" '    section();'
init_repo "$repo"
commit_base "$repo"
mkdir -p "$tmpdir/shim-no-git" || exit 1
ln -s /bin/bash "$tmpdir/shim-no-git/bash" || exit 1
ln -s /usr/bin/dirname "$tmpdir/shim-no-git/dirname" || exit 1
ln -s "$(python3 -c 'import sys; print(sys.executable)')" "$tmpdir/shim-no-git/python3" || exit 1
PATH="$tmpdir/shim-no-git" red "$repo" 'scope ratchet refuses git absent from PATH in a git repo' \
    "missing required tool 'git'"

repo="$tmpdir/scope-empty-base-parser"
mkdir -p "$repo/tests" || exit 1
cp "$tmpdir/no-markers/tests/test_unsectioned.c" "$repo/tests/test_unsectioned.c" || exit 1
init_repo "$repo"
commit_base "$repo"
write_suite "$repo" '    section();'
red "$repo" 'scope ratchet refuses base suites with zero parsed in-scope files' 'parser self-check failed'

# Base blobs must be readable and decodable even if current sources are fine.
repo="$tmpdir/scope-bad-base-encoding"
write_suite "$repo" '    section();'
printf '\377\n' > "$repo/tests/test_bad.c"
init_repo "$repo"
commit_base "$repo"
cp "$tmpdir/no-markers/tests/test_unsectioned.c" "$repo/tests/test_bad.c" || exit 1
red "$repo" 'scope ratchet refuses an undecodable base blob and names the file' \
    'tests/test_bad.c at base'

repo="$tmpdir/scope-no-git"
mkdir -p "$tmpdir/shim-base-show" || exit 1
real_git="$(command -v git)"
{
    printf '#!/bin/bash\nif [[ "$1" == show ]]; then exit 1; fi\n'
    printf 'exec "%s" "$@"\n' "$real_git"
} > "$tmpdir/shim-base-show/git"
chmod +x "$tmpdir/shim-base-show/git" || exit 1
PATH="$tmpdir/shim-base-show:$PATH" red "$repo" 'scope ratchet refuses an unreadable base blob and names the file' \
    'tests/test_fixture.c at base'

echo
echo "$tests_run checks, $tests_failed failed"
[[ $tests_failed -eq 0 ]]
