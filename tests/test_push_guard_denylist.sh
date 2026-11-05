#!/usr/bin/env bash
# All five rules examine invocation operands. Lexical cases feed JSON only;
# the integration section runs pushes against an isolated local bare fixture.
# Data mentioning a forbidden invocation must remain discussable.
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
denied 'git config core.hooksPath /tmp/somewhere' "core.hooksPath"
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

# Hook registrations must survive Bash writes and reverts, while reads and
# discussion remain usable. These commands are JSON data, never executed.
control settings-checkout 2 'git checkout -- .claude/settings.json'
control settings-sed 2 "sed -i '' 's/Stop/Disabled/g' .claude/settings.json"
control settings-redirect 2 'printf "%s" "{}" > .claude/settings.json'
control settings-read 0 'cat .claude/settings.json'
denied 'git -C . restore --source=HEAD -- .claude/settings.json' "restore settings registrations"
allowed 'git reset HEAD -- .claude/settings.json' "index-only reset preserves working hook registrations"
denied 'sed -i.bak -e "s/Stop/Disabled/g" .claude/settings.json' "GNU sed in-place settings write"
denied 'sed --in-place=.bak -e "s/Stop/Disabled/g" .claude/settings.json' "long sed in-place settings write"
denied ': >>".claude/settings.json"' "append settings redirect"
denied 'cp /tmp/settings.json .claude/settings.json' "overwrite settings by copy"
denied 'rm .claude/settings.json' "remove settings registrations"
denied 'tee .claude/settings.json' "overwrite settings through tee"
allowed 'grep -n Stop .claude/settings.json' "grep settings is a read"
allowed "jq '.hooks' .claude/settings.json" "jq settings is a read"
allowed 'sed -n "1,20p" .claude/settings.json' "sed without in-place only reads settings"
allowed 'git show HEAD:.claude/settings.json' "git show settings is a read"
allowed 'cp .claude/settings.json /tmp/settings-backup.json' "copy settings out only reads it"
allowed 'cat .claude/settings.json; rm /tmp/other' "settings read and unrelated mutation"
allowed 'echo "Do not git checkout -- .claude/settings.json or sed -i on it"' "settings prose is data"
allowed 'gh issue comment 719 --body "Do not redirect > .claude/settings.json"' "settings inline body is data"
printf '%s\n' 'Do not git checkout -- .claude/settings.json' > "$body_file"
allowed "gh issue comment 719 --body-file $body_file" "settings body file is data"
allowed "cat <<'EOF'
Do not git checkout -- .claude/settings.json or redirect > .claude/settings.json
EOF" "settings heredoc prose is data"

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
    allowed "printf '%s' '$term'" "quoted string is data: $term"
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
# Correct the unsafe round-2 expectation: this write permanently disarms hooks.
denied 'git config core.hooksPath /tmp/fixture' "config write can disarm later pushes"
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

# #719 round 3: preserve queries and config values while refusing key writes.
for command in \
    'git -C /tmp config --local core.hooksPath /dev/null' \
    'git config --global --add core.hookspath /dev/null' \
    'git config --replace-all core.hooksPath /dev/null' \
    'git config --unset core.hooksPath' \
    'git config set core.hooksPath /dev/null' \
    'git config unset core.hooksPath' \
    'K=core.hooksPath; git config $K /dev/null'; do
    denied "$command" "hooksPath config write: $command"
done
for command in \
    'git config core.hooksPath' \
    'git config --get-all core.hooksPath' \
    'git config get core.hooksPath' \
    'git config --get-regexp core.hooksPath' \
    'git config --file core.hooksPath user.name example' \
    'git config user.name core.hooksPath' \
    'git config --get user.name core.hooksPath'; do
    allowed "$command" "config read or data operand: $command"
done
for wrapper in 'timeout 30' 'timeout -k 1 30' 'nohup' 'nice -n 5' 'stdbuf -o L' 'setsid'; do
    denied "$wrapper git push --no-verify" "wrapped bypass: $wrapper"
    denied "$wrapper git config core.hooksPath /dev/null" "wrapped config write: $wrapper"
    allowed "$wrapper grep -- '--no-verify' evidence.md" "wrapped grep pattern: $wrapper"
done
for command in \
    'V=--no-verify; git push $V' \
    'V=--no-verify; git push "$V"' \
    'K=core.hooksPath; git -c $K=/dev/null push' \
    'K=core.hooksPath; git -c "$K=/dev/null" push' \
    'K=core.hooksPath; git -c$K=/dev/null push' \
    'K=core.hooksPath; git --config-env=$K=VALUE push' \
    'K=core.hooksPath; git --config-env $K=VALUE push'; do
    denied "$command" "ambiguous executable expansion: $command"
done
allowed 'V=--no-verify; git push -o "$V"' "expanded push-option payload remains data"
allowed 'V=--no-verify; git push -- origin "$V"' "expanded operand after -- remains data"
for flag in --no-v --no-ve --no-ver --no-veri --no-verif; do
    denied "git push $flag" "git unique-prefix bypass: $flag"
    allowed "git push -o $flag" "abbreviation in push-option payload: $flag"
done

allowed 'gh issue create --title "git send-pack bypasses the pre-push hook" --body "Explain the guard"' \
    "issue title mentioning git is data"
allowed 'timeout 900 codex exec "Explain git config core.hooksPath and git push --no-verify"' \
    "embedded prose does not create a git command token"

# #719 round 4: unknown launchers fail closed without enumerating them in
# the guard. Include an invented name so adding known names cannot satisfy it.
for wrapper in 'flock /tmp/lock' 'xargs' 'script -q /dev/null' 'taskset -c 0' \
    'ionice -c 3' 'strace' 'su -c' 'chroot /tmp/root' 'busybox timeout 30' \
    'arbitrary-launcher'; do
    denied "$wrapper git push --no-verify" "unknown launcher bypass: $wrapper"
    denied "$wrapper git config core.hooksPath /dev/null" "unknown launcher config: $wrapper"
done
denied 'GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0=/dev/null git push' \
    "environment-config hook override"
denied 'PGWT_SKIP_PUSH_GUARD=1 flock /tmp/l git push' "skip assignment through unknown launcher"
denied "su -c 'git push --no-verify'" "quoted su command bypass"
denied "su -c 'git config core.hooksPath /dev/null'" "quoted su config write"
for reader in echo printf cat head tail grep rg sed awk less; do
    allowed "$reader git -c core.hooksPath=/dev/null push" "reader arguments are data: $reader"
done

# #687: exact plumbing names deny even without git or another denylist operand.
for name in git-send-pack git-push git-receive-pack; do
    denied "$name origin HEAD" "direct plumbing binary: $name"
    denied "command $name origin HEAD" "recognized wrapper plumbing binary: $name"
    denied "env /usr/libexec/git-core/$name origin HEAD" "recognized wrapper absolute plumbing binary: $name"
    denied "nohup $name origin HEAD" "nohup plumbing binary: $name"
    denied "timeout 30 $name origin HEAD" "timeout plumbing binary: $name"
    denied "xargs $name origin HEAD" "xargs plumbing binary: $name"
    denied "nohup /usr/libexec/git-core/$name" "absolute plumbing binary: $name"
    for reader in echo printf cat head tail grep rg sed awk less; do
        allowed "$reader $name origin HEAD" "reader plumbing name is data: $reader $name"
    done
done

# Exercise an actual pre-push executable in an isolated repository, with a
# local bare destination. Denied commands never execute. The following plain
# push must run the hook, including after every attempted persistent write.
python3 - "$HOOK" "$REPO_ROOT/scripts/git-hooks/pre-push" <<'PYPROOF'
import json, os, pathlib, subprocess, sys, tempfile

hook = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="push-guard-proof-") as temporary:
    root = pathlib.Path(temporary)
    repo, remote, marker = root / "repo", root / "remote.git", root / "hook-ran"
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
               GIT_TERMINAL_PROMPT="0", HOOK_MARKER=str(marker))
    def git(*args, cwd=repo):
        return subprocess.run(["git", *args], cwd=cwd, env=env, text=True, capture_output=True)
    def setup(*args, cwd=repo):
        result = git(*args, cwd=cwd)
        assert result.returncode == 0, result.stderr
    setup("init", "--bare", "-q", str(remote), cwd=root)
    setup("init", "-q", str(repo), cwd=root)
    setup("-c", "user.name=Guard test", "-c", "user.email=guard@example.invalid",
          "commit", "--allow-empty", "-qm", "fixture")
    setup("remote", "add", "origin", str(remote))
    pre_push = repo / ".git/hooks/pre-push"
    # Trace entry, then execute the repository's real, unmodified pre-push.
    real_pre_push = pre_push.with_name("real-pre-push")
    real_pre_push.write_bytes(pathlib.Path(sys.argv[2]).read_bytes())
    pre_push.write_text('#!/bin/sh\necho HOOK_RAN >> "$HOOK_MARKER"\nexec bash "$(dirname "$0")/real-pre-push" "$@"\n')
    pre_push.chmod(0o755)
    def probe():
        marker.unlink(missing_ok=True)
        result = git("push", "origin", "HEAD:refs/heads/proof")
        assert result.returncode != 0 and marker.read_text() == "HOOK_RAN\n", result.stderr
        assert "push blocked: no .pgwt-check.stamp" in result.stderr, result.stderr
    probe()
    # Negative control: prove Git really skips this same hook with the flag.
    marker.unlink()
    bypass = git("push", "--no-verify", "origin", "HEAD:refs/heads/negative-control")
    assert bypass.returncode == 0 and not marker.exists(), bypass.stderr
    print("PROOF baseline: plain push HOOK_RAN; unguarded --no-verify hook SKIPPED")
    commands = [
        "git config core.hooksPath /tmp/somewhere",
        "git -C . config --local core.hooksPath /dev/null",
        "git -c core.hooksPath=/dev/null push",
        *[f"{wrapper} git push --no-verify" for wrapper in
          ("timeout 30", "nohup", "nice -n 5", "stdbuf -o L", "setsid")],
        "V=--no-verify; git push $V",
        "K=core.hooksPath; git -c $K=/dev/null push",
        *[f"git push {flag}" for flag in ("--no-v", "--no-ve", "--no-ver", "--no-veri", "--no-verif")],
    ]
    # 20 launcher cases plus the two environment regressions: 22 proofs.
    round4 = [f"{wrapper} {command}" for wrapper in
              ("flock /tmp/lock", "xargs", "script -q /dev/null", "taskset -c 0",
               "ionice -c 3", "strace", "su -c", "chroot /tmp/root", "busybox timeout 30",
               "arbitrary-launcher")
              for command in ("git push --no-verify", "git config core.hooksPath /dev/null")]
    round4 += ["GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0=/dev/null git push",
               "PGWT_SKIP_PUSH_GUARD=1 flock /tmp/l git push"]
    quoted_su = ["su -c 'git push --no-verify'", "su -c 'git config core.hooksPath /dev/null'"]
    for command in commands + round4 + quoted_su:
        result = subprocess.run(["bash", hook], cwd=repo, env=env, text=True, capture_output=True,
                                input=json.dumps({"tool_input": {"command": command}}))
        assert result.returncode == 2, (command, result.returncode, result.stderr)
        probe()
        print(f"PROOF {command} => DENY(2); following plain push HOOK_RAN (real pre-push: missing-stamp refusal)")
PYPROOF
proof_rc=$?
check "$([[ $proof_rc -eq 0 ]] && echo true || echo false)" "all reviewer bypasses preserve the executable pre-push hook"

echo
echo "test_push_guard_denylist: $tests_passed/$tests_run passed"
[[ $tests_failed -eq 0 ]]
