#!/usr/bin/env python3
"""Compare the working agreement with repository artifacts; never execute Codex.

House-style Markdown scanner, not a natural-language policy interpreter:
agent sanctions come from explicit .claude/agents/NAME.md references, bare
NAME.md references in the same paragraph, model assignments and quoted spawn instructions. Role
nouns such as 'implementer' sanction nothing. Plan and Explore are built-in
Claude types, not repository agent files. HTML comments do not sanction
artifacts; fenced spawn instructions still count.

Hooks are documented under '## Claude hooks' as a table with backtick-quoted
event, matcher ('*' for no matcher), and command columns. Compare the complete
multiset (event, matcher, command), including repeated commands at different
events. Unknown settings shapes/types, ambiguous JSON, and missing/unreadable
inputs refuse; an empty parse never means approval. The numbered coder/Astra
rules must retain their findable house spelling; every inline Codex command
must agree with its role. Intentional format/policy changes require updating
this scanner and its bypass tests together.

Usage: check-agent-agreement.py [repo-dir]
"""
from collections import Counter
import json
from pathlib import Path
import re
import sys


class Refusal(Exception):
    pass


def read(path):
    try:
        # Also fail under a privileged test runner that can read mode 000.
        if not path.is_file() or not path.stat().st_mode & 0o444:
            raise Refusal(f'{path.name} missing or unreadable')
        return path.read_text(encoding='utf-8')
    except (OSError, UnicodeError) as exc:
        raise Refusal(f'{path.name} missing or unreadable: {exc}') from exc


def prose(raw):
    raw = re.sub(r'<!--[\s\S]*?-->', '', raw)
    lines = []
    fence = None
    for line in raw.splitlines():
        marker = re.match(r'^\s*(`{3,}|~{3,})', line)
        if marker:
            if fence is None:
                fence = marker[1]
            elif marker[1][0] == fence[0] and len(marker[1]) >= len(fence):
                fence = None
            continue
        lines.append(line)
    if fence is not None or '<!--' in raw:
        raise Refusal('CLAUDE.md unparseable: unterminated fence/comment')
    return '\n'.join(lines)


def agents_in_agreement(doc):
    names = set(re.findall(r'\.claude/agents/([\w-]+)\.md\b', doc))
    # Short references such as `ui-reviewer.md` accompany a full path.
    for paragraph in re.split(r'\n\s*\n', doc):
        if '.claude/agents/' in paragraph:
            names.update(re.findall(r'`([\w-]+)\.md`', paragraph))
    names.update(re.findall(
        r'`([\w-]+)`\s*=\s*(?:\*\*)?(?:Sonnet|Opus|Haiku|Fable)\b', doc, re.I))
    names.update(re.findall(
        r'\bspawn\s+(?:(?:a|the|fresh)\s+)*`([\w-]+)`(?:\s+agent)?', doc, re.I))
    names.update(re.findall(
        r'\bsubagent_type["\x27`]?\s*[:=]\s*["\x27`]([\w-]+)["\x27`]', doc))
    names.difference_update({'Plan', 'Explore'})
    if not names:
        raise Refusal('CLAUDE.md unparseable: no sanctioned agent types')
    return names


def described_hooks(doc):
    section = re.search(r'^## Claude hooks\s*\n([\s\S]*?)(?=^## |\Z)', doc, re.M)
    if section is None:
        raise Refusal('check 3: CLAUDE.md has no Claude hooks description')
    hooks = Counter()
    for line in section[1].splitlines():
        if not line.lstrip().startswith('|'):
            continue
        columns = [part.strip() for part in line.strip().strip('|').split('|')]
        if columns == ['Event', 'Matcher', 'Command'] or all(
                re.fullmatch(r':?-+:?', col) for col in columns):
            continue
        if len(columns) != 3 or not all(re.fullmatch(r'`[^`]+`', col) for col in columns):
            raise Refusal(f'check 3: CLAUDE.md unparseable hook row: {line}')
        event, matcher, command = (col[1:-1] for col in columns)
        hooks[event, '' if matcher == '*' else matcher, command] += 1
    if not hooks:
        raise Refusal('check 3: CLAUDE.md unparseable: no described hooks')
    return hooks


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Refusal(f'settings.json unparseable: duplicate key {key!r}')
        result[key] = value
    return result


def invalid_constant(value):
    raise Refusal(f'settings.json unparseable: invalid JSON constant {value}')


def registered_hooks(raw):
    try:
        settings = json.loads(raw, object_pairs_hook=unique_object,
                              parse_constant=invalid_constant)
    except ValueError as exc:
        raise Refusal(f'settings.json unparseable: {exc}') from exc
    if not isinstance(settings, dict) or not isinstance(settings.get('hooks'), dict):
        raise Refusal('settings.json unparseable: expected hooks object')
    hooks = Counter()
    for event, groups in settings['hooks'].items():
        if not event or not isinstance(groups, list):
            raise Refusal(f'settings.json unparseable: invalid hook event {event!r}')
        for group in groups:
            if (not isinstance(group, dict) or set(group) - {'matcher', 'hooks'}
                    or not isinstance(group.get('matcher', ''), str)
                    or not isinstance(group.get('hooks'), list) or not group['hooks']):
                raise Refusal(f'settings.json unparseable: invalid hook group at {event}')
            for hook in group['hooks']:
                if (not isinstance(hook, dict) or set(hook) != {'type', 'command'}
                        or hook['type'] != 'command' or not isinstance(hook['command'], str)
                        or not hook['command'].strip()):
                    raise Refusal(f'settings.json unparseable: unsupported hook at {event}')
                hooks[event, group.get('matcher', ''), hook['command']] += 1
    return hooks


def check_invocations(doc, fail):
    for role, subject, expected in (
        ('coder', r'`codex` writes the code', 'codex exec'),
        ('Astra', r'Astra writes designs', 'codex exec -m gpt-6-astra'),
    ):
        found = re.findall(r'^\s*\d+\. ' + subject + r', invoked as `([^`]+)`', doc, re.M)
        if found != [expected]:
            fail(4, f'{role} invocation: expected `{expected}`, found {found!r}')
    for line in doc.splitlines():
        for command in re.findall(r'`(codex\s+[^`]+)`', line):
            role = 'Astra' if re.search(r'\bAstra\b', line) else 'coder'
            expected = 'codex exec -m gpt-6-astra' if role == 'Astra' else 'codex exec'
            if command != expected:
                fail(4, f'{role} invocation `{command}` must be `{expected}`')


def main():
    if len(sys.argv) > 2:
        print('usage: check-agent-agreement.py [repo-dir]', file=sys.stderr)
        return 1
    repo = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parent.parent
    failed = False

    def fail(number, reason):
        nonlocal failed
        failed = True
        print(f'FAIL: check {number}: {reason}', file=sys.stderr)

    try:
        doc = prose(read(repo / 'CLAUDE.md'))
        sanctioned = agents_in_agreement(doc)
        directory = repo / '.claude/agents'
        try:
            artifacts = list(directory.iterdir())
        except OSError as exc:
            raise Refusal(f'.claude/agents/ missing or unreadable: {exc}') from exc
        shipped = {path.stem for path in artifacts if path.suffix == '.md' and path.is_file()}
        for name in sorted(sanctioned - shipped):
            fail(1, f'CLAUDE.md sanctions missing .claude/agents/{name}.md')
        for path in sorted(artifacts):
            if path.suffix != '.md' or path.stem not in sanctioned or not path.is_file():
                fail(2, f'unsanctioned artifact .claude/agents/{path.name}')
            else:
                raw = read(path)
                header = re.match(r'\A---\n([\s\S]*?)\n---(?:\n|\Z)', raw)
                names = re.findall(r'^name:\s*([\w-]+)\s*$', header[1], re.M) if header else []
                if names != [path.stem]:
                    fail(2, f'.claude/agents/{path.name}: frontmatter name disagrees with filename')
        # Read and parse settings even when the agreement has no hook description.
        actual_hooks = registered_hooks(read(repo / '.claude/settings.json'))
        expected_hooks = described_hooks(doc)
        for key, count in sorted((expected_hooks - actual_hooks).items()):
            fail(3, f'described hook not registered: {key!r} (missing {count})')
        for key, count in sorted((actual_hooks - expected_hooks).items()):
            fail(3, f'registered hook not described: {key!r} (extra {count})')
        check_invocations(doc, fail)
    except (OSError, UnicodeError, Refusal) as exc:
        print(f'REFUSING: check-agent-agreement: {exc}', file=sys.stderr)
        return 1
    if failed:
        return 1
    print('OK: agent agreement checks 1-4 (agents, hooks, role invocations)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
