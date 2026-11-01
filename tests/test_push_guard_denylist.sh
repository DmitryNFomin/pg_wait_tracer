#!/usr/bin/env bash
# All five rules examine invocation operands. Feed JSON only: these commands
# never execute. Data mentioning a forbidden invocation must remain discussable.
# PGWT_TEST_PUSH_GUARD_DENYLIST can supply an alternate hook script, to
# show these checks go red against a regressed one (same convention as
# tests/test_pre_push_hook.sh's PGWT_TEST_PRE_PUSH_HOOK).
#
# Usage: tests/test_push_guard_denylist.sh (run from anywhere)
# Commands below are literal test data, never shell-expanded.
# shellcheck disable=SC2016
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOK="${PGWT_TEST_PUSH_GUARD_DENYLIST:-$REPO_ROOT/scripts/hooks/push-guard.sh}"

# Replay top-level project transcripts (the supplied *.jsonl corpus), accepting
# both hook records and Claude transcript tool_use records normalized to the
# same tool_name/tool_input shape. --replay PATH also accepts a saved snapshot.
if [[ "${1:-}" == "--replay" ]]; then
    exec python3 - "$HOOK" "${2:-$HOME/.claude/projects/-Users-dmitryfomin-work-git-pg-wait-tracer}" <<'PYREPLAY'
import concurrent.futures, glob, json, pathlib, subprocess, sys
hook, source = sys.argv[1:]
paths = sorted(glob.glob(str(pathlib.Path(source) / "*.jsonl"))) if pathlib.Path(source).is_dir() else [source]
if not paths:
    sys.exit("no transcript files found")
commands, malformed = [], 0
for path in paths:
    with open(path) as stream:
        for line_no, line in enumerate(stream, 1):
            try:
                record = json.loads(line)
            except ValueError:
                malformed += 1
                continue
            entries = [record] if record.get("tool_name") == "Bash" else []
            content = record.get("message", {}).get("content", [])
            if isinstance(content, list):
                entries += [{"tool_name": "Bash", "tool_input": c.get("input", {})}
                            for c in content if c.get("type") == "tool_use" and c.get("name") == "Bash"]
            for entry in entries:
                command = entry.get("tool_input", {}).get("command")
                if not isinstance(command, str):
                    sys.exit(f"missing Bash command at {path}:{line_no}")
                commands.append((record.get("source", path), record.get("line", line_no), command))

def replay(item):
    path, line_no, command = item
    result = subprocess.run(["bash", hook], input=json.dumps({"tool_name": "Bash", "tool_input": {"command": command}}), text=True, capture_output=True)
    if result.returncode not in (0, 2):
        raise RuntimeError(f"guard failed at {path}:{line_no}: {result.stderr}")
    return item, result

allowed = denied = 0
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    for (path, line_no, command), result in pool.map(replay, commands):
        if result.returncode == 0:
            allowed += 1
            continue
        denied += 1
        print(f"DENIAL {denied}: {path}:{line_no} [exit {result.returncode}]")
        print("COMMAND BEGIN\n" + command + "\nCOMMAND END")
        print(result.stdout + result.stderr, end="")
print(f"REPLAY files={len(paths)} commands={len(commands)} allowed={allowed} denied={denied} malformed_json_lines={malformed}")
PYREPLAY
fi

tests_run=0
tests_passed=0
tests_failed=0

report() {
    local ok="$1" msg="$2"
    tests_run=$((tests_run + 1))
    if [[ "$ok" == "true" ]]; then
        tests_passed=$((tests_passed + 1))
        echo "  PASS: $msg"
    else
        tests_failed=$((tests_failed + 1))
        echo "  FAIL: $msg"
    fi
}
check() { if [[ "$1" == "true" ]]; then report true "$2"; else report false "$2"; fi; }

# Builds the real Claude Code hook JSON ({"tool_input":{"command": ...}})
# for an arbitrary command string, including one containing newlines and
# apostrophes -- json.dumps handles the escaping, not this script.
hook_json() {
    python3 -c 'import json, sys; print(json.dumps({"tool_input": {"command": sys.argv[1]}}))' "$1"
}

run_hook() {
    printf '%s' "$(hook_json "$1")" | bash "$HOOK" 2>&1
}

denied() {
    local command="$1" label="$2"
    local out rc
    out=$(run_hook "$command"); rc=$?
    check "$([[ $rc -ne 0 ]] && echo true || echo false)" "$label: denied (nonzero exit)"
    check "$([[ -n "$out" ]] && echo true || echo false)" "$label: a message was printed, not a silent refusal"
}

allowed() {
    local command="$1" label="$2"
    local rc
    run_hook "$command" >/dev/null 2>&1; rc=$?
    check "$([[ $rc -eq 0 ]] && echo true || echo false)" "$label: allowed (exit 0)"
}

# ── Each of the five rules denies ────────────────────────────────────────
denied 'PGWT_SKIP_PUSH_GUARD=1 git push origin main' "PGWT_SKIP_PUSH_GUARD=1"
denied 'git -c core.hooksPath=/tmp/somewhere push' "core.hooksPath"
denied 'chmod -x scripts/git-hooks/pre-push' "chmod + git-hooks together"
denied 'git push --no-verify origin main' "--no-verify"
denied 'git send-pack origin HEAD' "git send-pack bypasses pre-push entirely (#687)"

# ── Write-shaped operations together with git-hooks deny ────────────────
denied 'rm -f scripts/git-hooks/pre-push' "removal + git-hooks"
denied ': > scripts/git-hooks/pre-push' "truncating redirection + git-hooks"
denied 'install -m 000 /dev/null scripts/git-hooks/pre-push' "permission-stripping install + git-hooks"
denied 'echo exit 0 > scripts/git-hooks/pre-push' "overwrite via redirection + git-hooks"
allowed 'git commit -m "it'"'"'s fine, just discussing scripts/git-hooks/pre-push here"' \
    "apostrophe-bearing commit message mentioning git-hooks, no write-shaped operation: allowed"

# ── Write-shaped operations and git-hooks only deny TOGETHER ─────────────
allowed 'chmod +x my-script.sh' "chmod alone (no git-hooks mention)"
allowed 'ls -la scripts/git-hooks' "git-hooks alone (no chmod)"
allowed 'cat scripts/git-hooks/pre-push' "reading git-hooks"
allowed 'cat scripts/git-hooks/pre-push 2>&1' \
    "benign read with > in stderr redirection, without writing to the hook"
allowed 'echo "see scripts/git-hooks/pre-push -> it skips a non-executable hook"' \
    "benign prose with ->, without writing to the hook"
allowed 'diff <(cat a) <(cat scripts/git-hooks/pre-push) > /dev/null' \
    "benign comparison with > targeting /dev/null, without writing to the hook"
allowed 'git log -- scripts/git-hooks/pre-push' "history query for git-hooks"
allowed 'git commit -m "doc: explain git-hooks in README"' \
    "a commit message merely mentioning git-hooks with no write-shaped operation"

# ── A command with none of the five terms passes untouched ───────────────
allowed 'git push origin main' "an ordinary push with none of the five terms"
allowed 'git status && git log --oneline -5' "an unrelated command with none of the five terms"

# ── THE control that matters: odd apostrophe count, no push, none of the
#    five denylist terms -- must pass. The retired parser rejected exactly
#    this shape outright, blocking an unrelated command for a quoting
#    reason that had nothing to do with pushing or disarming anything. ──
IFS= read -r -d '' odd_apostrophe_heredoc <<'INNER'
cat > f.txt <<'EOF'
it can't be a push; neither can 'tests' alone
EOF
INNER
allowed "$odd_apostrophe_heredoc" \
    "odd-apostrophe heredoc, no push, no denylist term: passes (the shape the retired parser rejected)"

# The former accepted false positive is now a regression: prose is data.
quiet_allowed() {
    local out rc
    out=$(run_hook "$1"); rc=$?
    check "$([[ $rc -eq 0 ]] && echo true || echo false)" "$2: allowed (exit 0)"
    check "$([[ -z "$out" ]] && echo true || echo false)" "$2: no refusal message"
}
quiet_allowed 'git commit -m "doc: explain core.hooksPath in README"' \
    "commit message merely mentioning core.hooksPath"

# Required #719 controls: paste both the exact command and actual hook output.
control() {
    local label="$1" expected="$2" command="$3" out rc
    out=$(run_hook "$command"); rc=$?
    printf 'CONTROL %s: %s\nexit=%s output=%s\n' "$label" "$command" "$rc" "${out:-<empty>}"
    check "$([[ $rc -eq $expected ]] && echo true || echo false)" "control $label"
}
control 1 0 'git show origin/master:scripts/git-hooks/pre-push 2>/dev/null | grep -n stamp'
body_file=$(mktemp)
trap 'rm -f "$body_file"' EXIT
printf '%s\n' 'The guard denies rm scripts/git-hooks/pre-push.' > "$body_file"
control 2 0 "gh issue comment 719 --body-file $body_file"
# The guard does not open the file: tool payload files are data.
control 3 0 'gh issue close 719 --comment "The guard blocks rm scripts/git-hooks/pre-push and 2>/dev/null near it"'
control 4a 0 'ls -l scripts/git-hooks'
control 4b 0 'cat scripts/git-hooks/pre-push'
control 5 2 'rm scripts/git-hooks/pre-push'
control 6 2 ': > scripts/git-hooks/pre-push'
control 7 2 'install /dev/null scripts/git-hooks/pre-push'
control 8 2 'cp /tmp/x scripts/git-hooks/pre-push'
control 9 2 'chmod -x scripts/git-hooks/pre-push'
control 10 2 'echo x > scripts/git-hooks/pre-push'
control 11a 2 'PGWT_SKIP_PUSH_GUARD=1 git push'
control 11b 2 'git -c core.hooksPath=/dev/null push'
control 11c 2 'git push --no-verify'
control 11d 2 'git send-pack'

# Payloads and invocation boundaries, including commands following data.
allowed 'echo "rm scripts/git-hooks/pre-push"' "quoted mutation is data"
allowed "gh issue comment 719 --body 'chmod -x scripts/git-hooks/pre-push'" "inline body is data"
allowed 'git commit -m "rm scripts/git-hooks/pre-push"' "commit message is data"
allowed 'cat scripts/git-hooks/pre-push; rm /tmp/other' "unrelated mutation in separate invocation"
allowed 'cp scripts/git-hooks/pre-push /tmp/backup' "copying hook out only reads it"
allowed 'ln -s scripts/git-hooks/pre-push /tmp/link' "linking hook as source only reads it"
allowed 'echo ok # rm scripts/git-hooks/pre-push' "shell comment is data"
allowed 'cat "$(printf %s scripts/git-hooks/pre-push)"' "read with command substitution"
allowed 'diff <(cat scripts/git-hooks/pre-push) <(cat /tmp/backup)' "read with process substitution"
IFS= read -r -d '' hook_prose <<'INNER'
gh issue comment 719 --body-file - <<'EOF'
it can't rm scripts/git-hooks/pre-push; chmod -x is dangerous
EOF
INNER
allowed "$hook_prose" "heredoc hook discussion is data"
denied "$hook_prose"$'\nrm scripts/git-hooks/pre-push' "mutation following heredoc"
for verb in mv truncate tee ln unlink touch; do
    denied "$verb /tmp/x scripts/git-hooks/pre-push" "real $verb operand"
done
denied '/bin/rm -- "scripts/git-hooks/pre-push"' "absolute mutator and quoted path"
denied 'env X=1 command rm scripts/git-hooks/pre-push' "wrapped invocation"
denied 'echo "$(rm scripts/git-hooks/pre-push)"' "executed command substitution"
denied 'bash -c "rm scripts/git-hooks/pre-push"' "explicit shell command operand"
denied 'cp -t scripts/git-hooks /tmp/x' "target-directory operand"
denied 'echo x >>"scripts/git-hooks/pre-push"' "quoted append target"
denied 'dd if=/dev/null of=scripts/git-hooks/pre-push' "dd output operand"
denied 'eval "rm scripts/git-hooks/pre-push"' "ambiguous executable payload"
denied 'echo "scripts/git-hooks/pre-push' "ambiguous unfinished quote"

IFS= read -r -d '' quoted_heredoc <<'INNER'
gh issue close 719 --comment "$(cat <<'EOF'
it can't rm scripts/git-hooks/pre-push; a ) is prose here
EOF
)"
INNER
allowed "$quoted_heredoc" "heredoc in a quoted body substitution is data"
denied '2>/dev/null rm scripts/git-hooks/pre-push' "redirect before command preserves command position"
denied '{ rm scripts/git-hooks/pre-push; }' "mutation inside command group"
denied 'time rm scripts/git-hooks/pre-push' "mutation with time keyword"

allowed 'cp -t /tmp/backup scripts/git-hooks/pre-push' "target-directory copy reads hook"
allowed 'ln --target-directory=/tmp/backup scripts/git-hooks/pre-push' "target-directory link reads hook"
denied 'cp /tmp/x scripts/git-hooks/pre-push -f' "trailing copy option preserves destination"
denied 'install -d scripts/git-hooks /tmp/other' "install directory form mutates every operand"

IFS= read -r -d '' shell_heredoc <<'INNER'
bash <<'EOF'
rm scripts/git-hooks/pre-push
EOF
INNER
denied "$shell_heredoc" "shell interpreter executes its heredoc"

# #719 part 2: matched forbidden-invocation / harmless-mention pairs.
control skip-denied 2 'env PGWT_SKIP_PUSH_GUARD=1 git push'
control skip-allowed 0 'grep -n "PGWT_SKIP_PUSH_GUARD=1" evidence.md'
control config-denied 2 'git -c core.hooksPath=/dev/null push'
control config-allowed 0 'gh issue comment 719 --body "Explain core.hooksPath=/dev/null"'
control verify-denied 2 'git push --no-verify origin main'
control verify-allowed 0 'grep -n -- "--no-verify" evidence.md'
control plumbing-denied 2 'git send-pack origin HEAD'
control plumbing-allowed 0 'gh issue comment 719 --body "Explain git send-pack"'

# Every term must be safe across each data context, without exemptions.
for term in 'PGWT_SKIP_PUSH_GUARD=1' 'core.hooksPath=/dev/null' '--no-verify' 'git send-pack'; do
    allowed "rg -n -- '$term' evidence.md" "rg pattern is data: $term"
    allowed "git commit -m 'Explain $term'" "commit body is data: $term"
    allowed "gh issue comment 719 --body 'Explain $term'" "inline body is data: $term"
    printf '%s\n' "$term" > "$body_file"
    allowed "gh issue comment 719 --body-file $body_file" "body file is data: $term"
    allowed "cat <<'EOF'
$term
EOF" "quoted heredoc is data: $term"
    allowed "cat <<EOF
$term
EOF" "unquoted literal heredoc is data: $term"
    allowed "echo ok # $term" "shell comment is data: $term"
done

# Recognize actual assignments, global options, wrappers and executable contexts.
denied 'export PGWT_SKIP_PUSH_GUARD=1; git push' "export before git invocation"
denied 'export PGWT_SKIP_PUSH_GUARD="1" && command git push' "quoted export before wrapped git"
denied 'PGWT_SKIP_PUSH_GUARD="1" /usr/bin/git push' "quoted assignment before absolute git"
denied 'env X=1 PGWT_SKIP_PUSH_GUARD=1 command git push' "skip assignment among env operands"
denied 'export PGWT_SKIP_PUSH_GUARD=1; bash -c "git push"' "export inherited by explicit shell"
denied 'PGWT_SKIP_PUSH_GUARD=$value git push' "ambiguous executable assignment value"
allowed 'env PGWT_SKIP_PUSH_GUARD=0 git push' "known inert skip assignment"
allowed 'export PGWT_SKIP_PUSH_GUARD=1; unset PGWT_SKIP_PUSH_GUARD; git push' "unset before git invocation"
allowed 'export PGWT_SKIP_PUSH_GUARD=1; env -u PGWT_SKIP_PUSH_GUARD git push' "env unsets skip assignment"
allowed 'printf "%s" PGWT_SKIP_PUSH_GUARD=1' "assignment spelling outside command prefix is data"
allowed 'git config --get core.hooksPath' "config query is not a git -c override"
allowed 'git config core.hooksPath /tmp/fixture' "config operand is not a git -c override"
denied 'git -C /tmp -c "core.hooksPath=/dev/null" push' "quoted git config after global option"
denied 'git -ccore.hookspath=/dev/null push' "attached git config with case-insensitive key"
denied 'git -c core.hooksPath= status' "empty hook directory override"
allowed 'echo git -c core.hooksPath=/dev/null push' "git override outside command position"
denied 'git -C /tmp push origin --no-verify' "push flag after global option and remote"
allowed 'git commit -m "--no-verify"' "bypass spelling in commit message"
allowed 'git push -o --no-verify origin main' "push option payload is data"
allowed 'git push -- origin --no-verify' "push operands after end of options are data"
denied 'command /usr/bin/git --git-dir /tmp/repo send-pack origin HEAD' "wrapped plumbing after global option"
allowed 'printf "%s" "git send-pack"' "plumbing spelling outside command position"
for command in 'env PGWT_SKIP_PUSH_GUARD=1 git push' 'git -c core.hooksPath=/dev/null push' 'git push --no-verify' 'git send-pack'; do
    denied "bash -c '$command'" "explicit shell executes: $command"
    denied "echo \"\$( $command )\"" "command substitution executes: $command"
    denied "cat <<'EOF'
$command
EOF
$command" "command following data heredoc executes: $command"
done
allowed 'cat <<EOF
Explain --no-verify and $(printf %s core.hooksPath)
EOF' "heredoc substitution that only prints prose is data"
denied 'cat <<EOF
$(git push --no-verify)
EOF' "heredoc executable substitution is checked"
denied 'eval "git push --no-verify"' "ambiguous executable bypass payload"
denied 'git "$subcommand" --no-verify' "ambiguous expanded git subcommand"

allowed '(export PGWT_SKIP_PUSH_GUARD=1); git push' "subshell export does not affect parent git"
allowed 'export PGWT_SKIP_PUSH_GUARD=1 | cat; git push' "pipeline export does not affect later git"
allowed 'env PGWT_SKIP_PUSH_GUARD=1; git push' "env without command only prints environment"
allowed 'export PGWT_SKIP_PUSH_GUARD=1; env -i git push' "env clears exported skip variable"
denied 'export PGWT_SKIP_PUSH_GUARD=1; env --unset=PGWT_SKIP_PUSH_GUARD_OTHER git push' "unsetting another variable leaves skip enabled"
denied 'export PGWT_SKIP_PUSH_GUARD=1; (git push)' "subshell inherits exported skip variable"
denied 'env -S "PGWT_SKIP_PUSH_GUARD=1 git push"' "ambiguous env split-string executes"
denied 'git -c core.hooks"Path"=/dev/null push' "split quoted config key"
denied 'PGWT_SKIP_PUSH_GUARD=""1 git push' "split quoted skip value"
allowed 'bash script.sh --comment "git send-pack"' "shell script arguments are data"
allowed 'bash --norc -c "echo git send-pack"' "long shell option is not a command-string flag"
denied 'bash --norc -c "git send-pack"' "shell command-string flag after long option"

echo
echo "test_push_guard_denylist: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
