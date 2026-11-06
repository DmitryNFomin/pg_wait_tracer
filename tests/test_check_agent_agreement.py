#!/usr/bin/env python3
"""Hermetic red controls for check-agent-agreement; no caller files are changed.

Run by scripts/check.sh (make check), or directly with python3 -B.
Each case builds an independent repository-shaped
fixture and invokes the real shell entry point, including missing-python tests.
The synthetic agreement is deliberately smaller than the real CLAUDE.md.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
GUARD = ROOT / 'scripts/check-agent-agreement.sh'
BASH = shutil.which('bash')
DOC = '''# Fixture working agreement
## Who does what
1. `codex` writes the code, invoked as `codex exec` with no `-m` flag.
2. Claude agents review the code.
3. Astra writes designs, invoked as `codex exec -m gpt-6-astra`.
Get approval from `.claude/agents/reviewer.md` and `ui-reviewer.md`.
Fable is the standing adviser via `.claude/agents/adviser.md`.
`reviewer` = **Sonnet**; `ui-reviewer` = Sonnet. `Explore`/triage: Haiku.
Send large work to a `Plan` agent. One issue = one implementer.
Return findings to the implementer. Fable is not an implementer.
Consult Astra (`codex exec -m gpt-6-astra`) on uncertainty.
## Claude hooks
| Event | Matcher | Command |
|---|---|---|
| `PreToolUse` | `Bash` | `scripts/hooks/push-guard.sh` |
| `Stop` | `*` | `scripts/lead/sweep-hook.sh` |
| `SubagentStart` | `*` | `scripts/lead/agent-start-hook.sh` |
| `SubagentStop` | `*` | `scripts/lead/agent-stop-hook.sh` |
| `SubagentStop` | `*` | `scripts/lead/sweep-hook.sh` |
'''
SETTINGS = {'hooks': {
    'PreToolUse': [{'matcher': 'Bash', 'hooks': [
        {'type': 'command', 'command': 'scripts/hooks/push-guard.sh'}]}],
    'Stop': [{'hooks': [{'type': 'command', 'command': 'scripts/lead/sweep-hook.sh'}]}],
    'SubagentStart': [{'hooks': [
        {'type': 'command', 'command': 'scripts/lead/agent-start-hook.sh'}]}],
    'SubagentStop': [{'hooks': [
        {'type': 'command', 'command': 'scripts/lead/agent-stop-hook.sh'},
        {'type': 'command', 'command': 'scripts/lead/sweep-hook.sh'}]}],
}}


def main():
    if BASH is None:
        print('REFUSING: self-test requires bash', file=sys.stderr)
        return 1
    failed = total = 0
    with tempfile.TemporaryDirectory(prefix='pgwt-agent-agreement-') as scratch:
        scratch = Path(scratch)

        def case(label, mutation=lambda repo: None, expected='OK:', env=None):
            nonlocal total, failed
            total += 1
            repo = scratch / str(total)
            agents = repo / '.claude/agents'
            agents.mkdir(parents=True)
            for name in ('reviewer', 'ui-reviewer', 'adviser'):
                (agents / f'{name}.md').write_text(f'---\nname: {name}\n---\n', encoding='utf-8')
            (repo / 'CLAUDE.md').write_text(DOC, encoding='utf-8')
            (repo / '.claude/settings.json').write_text(json.dumps(SETTINGS), encoding='utf-8')
            mutation(repo)
            result = subprocess.run([BASH, str(GUARD), str(repo)], env=env,
                                    capture_output=True, text=True)
            output = result.stdout + result.stderr
            green = expected == 'OK:'
            good = (result.returncode == (0 if green else 1)
                    and expected in output and (green or 'OK:' not in output))
            if not good:
                failed += 1
            print(f'{"PASS" if good else "FAIL"}: {label} (exit {result.returncode}): '
                  + ' | '.join(output.strip().splitlines()))
            # Restore mode before TemporaryDirectory cleanup, including on root.
            path = repo / 'CLAUDE.md'
            if path.is_file():
                path.chmod(0o644)

        def doc_change(old, new):
            def mutate(repo):
                path = repo / 'CLAUDE.md'
                raw = path.read_text(encoding='utf-8')
                assert old in raw
                path.write_text(raw.replace(old, new), encoding='utf-8')
            return mutate

        def settings_change(change):
            def mutate(repo):
                path = repo / '.claude/settings.json'
                data = json.loads(path.read_text(encoding='utf-8'))
                change(data['hooks'])
                path.write_text(json.dumps(data), encoding='utf-8')
            return mutate

        case('clean fixture')
        for name in ('implementer', 'zzz'):
            case(f'restored {name}.md', lambda repo, name=name:
                 (repo / f'.claude/agents/{name}.md').write_text('stub\n'),
                 f'FAIL: check 2: unsanctioned artifact .claude/agents/{name}.md')
        case('removed SubagentStop sweep entry', settings_change(
            lambda hooks: hooks['SubagentStop'][0]['hooks'].pop()),
            "FAIL: check 3: described hook not registered: ('SubagentStop', '', 'scripts/lead/sweep-hook.sh')")
        case('wrong Astra model', doc_change('gpt-6-astra', 'gpt-wrong'),
             'FAIL: check 4: Astra invocation')
        case('CLAUDE.md missing', lambda repo: (repo / 'CLAUDE.md').unlink(),
             'CLAUDE.md missing or unreadable')
        case('CLAUDE.md unreadable', lambda repo: (repo / 'CLAUDE.md').chmod(0),
             'CLAUDE.md missing or unreadable')
        case('agents directory missing', lambda repo: shutil.rmtree(repo / '.claude/agents'),
             '.claude/agents/ missing or unreadable')
        case('settings.json missing', lambda repo: (repo / '.claude/settings.json').unlink(),
             'settings.json missing or unreadable')
        case('settings.json malformed', lambda repo:
             (repo / '.claude/settings.json').write_text('{broken'), 'settings.json unparseable')
        empty_path = scratch / 'empty-path'
        empty_path.mkdir()
        no_python = dict(os.environ, PATH=str(empty_path))
        case('python3 absent', expected="missing required tool 'python3'", env=no_python)

        case('sanctioned agent missing', lambda repo:
             (repo / '.claude/agents/reviewer.md').unlink(), 'FAIL: check 1:')
        case('spawned agent missing', doc_change('One issue', 'Spawn the `missing` agent. One issue'),
             'missing .claude/agents/missing.md')
        case('explicit subagent_type missing', doc_change('One issue',
             'Use subagent_type: "missing". One issue'), 'missing .claude/agents/missing.md')
        case('non-md orphan', lambda repo: (repo / '.claude/agents/notes.txt').write_text('x'),
             'unsanctioned artifact .claude/agents/notes.txt')
        case('nested directory orphan', lambda repo: (repo / '.claude/agents/nested').mkdir(),
             'unsanctioned artifact .claude/agents/nested')
        case('disguised agent name', lambda repo:
             (repo / '.claude/agents/reviewer.md').write_text('---\nname: other\n---\n'),
             'frontmatter name disagrees')
        case('extra registered hook', settings_change(lambda hooks:
             hooks.update({'Bogus': [{'hooks': [{'type': 'command', 'command': 'bogus.sh'}]}]})),
             'FAIL: check 3: registered hook not described')
        case('extra described hook', doc_change('| `Stop`',
             '| `Bogus` | `*` | `bogus.sh` |\n| `Stop`'),
             'FAIL: check 3: described hook not registered')
        case('wrong hook event', settings_change(lambda hooks:
             hooks.update({'Bogus': hooks.pop('Stop')})), 'FAIL: check 3:')
        case('wrong hook matcher', settings_change(lambda hooks:
             hooks['PreToolUse'][0].update(matcher='Read')), 'FAIL: check 3:')
        case('duplicate registered hook', settings_change(lambda hooks:
             hooks['Stop'].append(hooks['Stop'][0])), 'FAIL: check 3: registered hook not described')
        case('missing hook documentation', doc_change('## Claude hooks', '## Other'),
             'CLAUDE.md has no Claude hooks description')
        case('malformed hook row', doc_change('| `Stop` | `*`', '| Stop | `*`'),
             'CLAUDE.md unparseable hook row')
        case('coder model override', doc_change('`codex exec`', '`codex exec -m gpt-6.1-sol`'),
             'FAIL: check 4: coder invocation')
        case('wrong secondary Astra invocation', doc_change(
             'Astra (`codex exec -m gpt-6-astra`)', 'Astra (`codex exec -m gpt-wrong`)'),
             'FAIL: check 4: Astra invocation `codex exec -m gpt-wrong`')
        case('missing coder rule', doc_change('`codex` writes the code', '`codex` builds things'),
             'FAIL: check 4: coder invocation')
        case('settings wrong shape', lambda repo:
             (repo / '.claude/settings.json').write_text('[]'), 'settings.json unparseable')
        case('settings duplicate key', lambda repo:
             (repo / '.claude/settings.json').write_text('{"hooks": {}, "hooks": {}}'),
             'settings.json unparseable: duplicate key')
        case('settings non-JSON numeric constant', lambda repo:
             (repo / '.claude/settings.json').write_text('{"hooks": {}, "invalid": NaN}'),
             'settings.json unparseable: invalid JSON constant NaN')
        case('settings unsupported hook type', settings_change(lambda hooks:
             hooks['Stop'][0]['hooks'][0].update(type='prompt')), 'settings.json unparseable')
        case('settings invalid group', settings_change(lambda hooks:
             hooks.update(Stop=[{}])), 'settings.json unparseable')
        case('CLAUDE.md invalid UTF-8', lambda repo:
             (repo / 'CLAUDE.md').write_bytes(b'\xff'), 'CLAUDE.md missing or unreadable')
        case('CLAUDE.md empty', lambda repo: (repo / 'CLAUDE.md').write_text(''),
             'CLAUDE.md unparseable')
        case('CLAUDE.md unterminated fence', doc_change('# Fixture', '```\n# Fixture'),
             'CLAUDE.md unparseable')
        case('CLAUDE.md unterminated comment', doc_change('# Fixture', '<!--\n# Fixture'),
             'CLAUDE.md unparseable')
        case('comments do not sanction', doc_change('One issue',
             '<!-- Spawn the `zzz` agent. --> One issue'))
        case('fenced spawn instructions count', doc_change('## Who does what',
             '```\nsubagent_type: "zzz"\n```\n## Who does what'),
             'missing .claude/agents/zzz.md')
        case('artifact sanctioned only in comment', doc_change('`.claude/agents/adviser.md`',
             '<!-- `.claude/agents/adviser.md` -->'), 'unsanctioned artifact .claude/agents/adviser.md')
        def unrelated_doc(repo):
            path = repo / 'CLAUDE.md'
            path.write_text(DOC + '\nSee `zzz.md` for unrelated documentation.\n', encoding='utf-8')
            (repo / '.claude/agents/zzz.md').write_text('---\nname: zzz\n---\n', encoding='utf-8')
        case('unrelated doc reference cannot sanction orphan', unrelated_doc,
             'unsanctioned artifact .claude/agents/zzz.md')
        case('clean fixture after all mutations')
    print(f'{total - failed}/{total} passed')
    return int(bool(failed))


if __name__ == '__main__':
    sys.exit(main())
