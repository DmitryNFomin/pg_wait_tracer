#!/usr/bin/env python3
"""check-section-registration.py -- per-section registration guard (#686).

tests/unit_tests.list and check-test-registration.sh verify whole binaries,
not the numbered sections dispatched by main() or a sub-dispatcher. Deleting
ONE dispatcher line left these PR #689 suites green (0 failed, exit 0):
  binary                    wired checks    unwired checks
  test_idle_summary                  397               367
  test_agg_raw_crosscheck             251               211
  test_cpu_split_parity              107                66

Static call-graph reachability makes every function owning a section printf
header reachable from main(), through actual call sites in the same file.
Each header's window runs from the end of its printf statement to the next header in the
same function (excluded), or to the end of its body. The first local function
call in that window is its delegate; both owner and delegate must be reachable,
and the delegate's call must supply an active edge in that particular window.
Windows without local calls but with other masked code contain inline sections,
checked via their owner. Whitespace-only windows fail: their dispatch site is
missing or the section is a stub.
No binaries, Linux box, section annotations, second registry, or exemptions
are needed: discover tests/test_*.c automatically. Comments, strings, chars,
preprocessor directives and #if 0 blocks cannot supply call edges. A call on
an if (0) line cannot supply one either. A gate that cannot parse its inputs
REFUSES rather than approving blind (the naive comment grep was already a
real failure: check-runner-registration.py, instance 3).

What this CANNOT see: a call site that exists but never executes at runtime
(for example a branch not taken), a reached section whose
assertions were gutted, or a suite whose headers match no supported label form.
Supported runtime banners: N. (suffixes, ranges and lists), -- N: title --,
=== N. title ===, §N (optionally wrapped), [N]/[A]/[A1], and --- title ---.
Only active printf first arguments supply markers; comments never do. Every
out-of-scope file is listed with a reason, distinguishing comment-organised
suites from files with no detected heading. Neither is claimed as verified.
The first local call in a window may be a helper, not the dispatcher.
Scope is append-only against the merge-base: a suite with supported headers
at base must retain at least one supported header on this tree.
This is a small house-style C scanner, not a C compiler or a runtime coverage check; it
follows direct calls, not function pointers or macro expansion, and does
not evaluate general preprocessor conditions. Definitions start at column 0.

usage: check-section-registration.py [repo-dir]
  repo-dir defaults to this script's own repository. Scratch repositories
  exercise refusal and mutation cases in test_check_section_registration.sh.
Exit: 0 all in-scope owners reachable, 1 unreachable or refused.
"""
import bisect
from collections import deque
from dataclasses import dataclass
from pathlib import Path
import re
import shutil
import subprocess
import sys


# Consume each lexical token once. Keep offsets unchanged in the masked copy,
# including for escaped quotes, continued literals, and multiline comments.
TOKEN = re.compile(
    r'(?P<directive>^[ \t]*\#(?:\\\n|[^\n])*)'
    r'|(?P<block>/\*[\s\S]*?\*/)'
    r'|(?P<comment>//(?:\\\n|[^\n])*)'
    r'|(?P<string>"(?:\\[\s\S]|[^"\\\n])*")'
    r"|(?P<char>'(?:\\[\s\S]|[^'\\\n])*')"
    r'|(?P<broken>/\*|["\'])', re.M,
)
LABEL = re.compile(r'^(?:\s|\\[nt])*(?:--+[ \t]+)?([0-9]+[a-z]*(?:-(?:[0-9]+[a-z]*|[a-z]+))?)\. +')
# Preserve LABEL's existing forms. New forms have explicit delimiters, not
# a loose search for numbers or prose that could match arbitrary comments.
NUMBER = r'[0-9]+[a-z]*(?:[-/,](?:[0-9]+[a-z]*|[a-z]+))*'
DOT_LABEL = re.compile(rf'^(?:(?:--+|===) +)?({NUMBER})\. +')
COLON_LABEL = re.compile(rf'^(?P<wrap>--+) +(?P<label>{NUMBER}): +'
                         r'[^\n]+? +(?P=wrap)(?:\\[nt]|\s)*$')
SECTION_LABEL = re.compile(r'^(?:(?:===|---) +)?§('
                           r'[0-9]+[a-z]*(?:[-/,]§?(?:[0-9]+[a-z]*|[a-z]+))*)[:.]? +')
BRACKET_LABEL = re.compile(r'^\[([0-9]+|[A-Z][0-9]*)\] +')
NAMED_LABEL = re.compile(r'^(?P<wrap>---+) +(?P<label>[A-Za-z#][^\n]+?) +'
                         r'(?P=wrap)(?:\\[nt]|\s)*$')
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
# No leading indentation: definitions in these suites start at column zero.
# A separate return-type line (int\nmain(...)) is allowed too.
DEFINITION = re.compile(
    r'^(?:[A-Za-z_]\w*[ \t*]+)*([A-Za-z_]\w*)[ \t]*\(', re.M,
)
DEAD_LINE = re.compile(r'\bif\s*\(\s*0\s*\)')


class Refusal(Exception):
    pass


class DuplicateDefinition(Refusal):
    pass


def mask(text):
    return ''.join('\n' if c == '\n' else ' ' for c in text)


def lex(raw):
    """Return offset-preserving code and active string tokens, in one pass."""
    parts, strings = [], {}
    last = 0
    disabled_depth = 0
    for token in TOKEN.finditer(raw):
        gap = raw[last:token.start()]
        parts.append(mask(gap) if disabled_depth else gap)
        value = token.group()
        kind = token.lastgroup
        if kind == 'directive':
            directive = re.sub(r'/\*.*?\*/|//.*', '', value).lstrip(' \t#').strip()
            if disabled_depth:
                if re.match(r'if(?:def|ndef)?\b', directive):
                    disabled_depth += 1
                elif re.match(r'endif\b', directive):
                    disabled_depth -= 1
            elif re.fullmatch(r'if\s+0\s*', directive):
                disabled_depth = 1
        elif kind == 'broken':
            if not disabled_depth:
                raise Refusal('unterminated comment or literal')
        elif kind == 'string' and not disabled_depth:
            strings[token.start()] = value[1:-1]
        parts.append(mask(value))
        last = token.end()
    if disabled_depth:
        raise Refusal('unterminated #if 0 block')
    parts.append(raw[last:])
    return ''.join(parts), strings


def matching(code, start, opening, closing):
    depth = 0
    for pos in range(start, len(code)):
        if code[pos] == opening:
            depth += 1
        elif code[pos] == closing:
            depth -= 1
            if depth == 0:
                return pos
    raise Refusal(f'unmatched {opening} at offset {start}')


@dataclass
class Function:
    start_line: int
    end_line: int
    body_start: int
    body_end: int
    returns_int: bool


def functions(code, line_at):
    found = {}
    previous_end = -1
    for candidate in DEFINITION.finditer(code):
        if candidate.start() <= previous_end:
            continue  # A column-zero call inside an already parsed body.
        name = candidate[1]
        paren = candidate.end() - 1
        end_params = matching(code, paren, '(', ')')
        body = end_params + 1
        while body < len(code) and code[body].isspace():
            body += 1
        if body == len(code) or code[body] != '{':
            continue  # Declaration/prototype, not a definition.
        end = matching(code, body, '{', '}')
        if name in found:
            raise DuplicateDefinition(f'duplicate function definition: {name}')
        # Inline helper bodies exist too; balanced braces identify their end.
        signature = code[candidate.start():paren]
        if signature.strip() == name:
            preceding = code[:candidate.start()].rstrip().rsplit('\n', 1)[-1]
            signature = preceding + ' ' + signature
        found[name] = Function(line_at(candidate.start()), line_at(end),
                               body + 1, end, bool(re.search(r'\bint\b', signature)))
        previous_end = end
    return found


def printf_literals(code, strings):
    """Yield first-argument literals at active printf sites."""
    for call in CALL.finditer(code):
        if call[1] != 'printf':
            continue
        first = call.end()
        # Whitespace (including newlines) and masked comments are allowed
        # between '(' and the first literal.
        while first < len(code) and code[first].isspace() and first not in strings:
            first += 1
        if first in strings:
            literal, pos = strings[first], first
            # A banner can continue across adjacent C string literals. Stop
            # at any code (comma, ')', macro, etc.), never another argument.
            while True:
                pos += len(strings[pos]) + 2
                while pos < len(code) and code[pos].isspace() and pos not in strings:
                    pos += 1
                if pos not in strings:
                    break
                literal += strings[pos]
            end = matching(code, call.end() - 1, '(', ')') + 1
            while end < len(code) and code[end].isspace():
                end += 1
            if end < len(code) and code[end] == ';':
                end += 1
            yield first, literal, call.start(), end


def section_label(literal):
    old = LABEL.match(literal.split('\n', 1)[0])
    if old:
        return old[1]
    text = re.sub(r'^(?:\s|\\[nt])*', '', literal)
    for pattern in (DOT_LABEL, SECTION_LABEL, BRACKET_LABEL):
        match = pattern.match(text)
        if match:
            return match[1].replace('§', '')
    for pattern in (COLON_LABEL, NAMED_LABEL):
        match = pattern.fullmatch(text)
        if match:
            return match['label']
    return None


def extract_markers(code, strings):
    """The same scope definition for working-tree files and base blobs."""
    return [(label, first) for label, first, _, _ in marker_spans(code, strings)]


def marker_spans(code, strings):
    markers = []
    for first, literal, start, end in printf_literals(code, strings):
        label = section_label(literal)
        if label:
            markers.append((label, first, start, end))
    return markers


def out_of_scope_reason(raw, code, strings):
    """Report evidence of unsupported organisation, without making it scope.

    Comments are hints only: a region heading can cover helpers, data and
    several tests, so it cannot identify a section owner or dispatch window.
    Never claim that zero recognised markers proves a file has no sections.
    """
    headings = []
    for token in TOKEN.finditer(raw):
        if token.lastgroup != 'block':
            continue
        if re.match(r'/\* [─═-]{2,}[ \t]+\S', token.group()):
            headings.append(str(raw.count('\n', 0, token.start()) + 1))
    hints = []
    if headings:
        hints.append('section-like comments at lines ' + ', '.join(headings))
    banners = [str(raw.count('\n', 0, pos) + 1)
               for pos, literal, _, _ in printf_literals(code, strings)
               if re.match(r'^(?:\s|\\[nt])*(?:--+ |=== [0-9]|§|'
                           r'\[[A-Z0-9]|[Ss]ection [0-9])', literal)]
    if banners:
        hints.append('unrecognised section-like printf text at lines ' + ', '.join(banners))
    unlabelled = [str(raw.count('\n', 0, pos) + 1)
                  for pos, literal, _, _ in printf_literals(code, strings)
                  if re.match(r'^(?:\s|\\[nt])*=== [^0-9§]', literal)]
    if unlabelled:
        hints.append('unlabelled === printf banners at lines ' + ', '.join(unlabelled) +
                     ' (suite/result or unsupported section title)')
    return ('no supported printf section banner; ' +
            ('; '.join(hints) if hints else 'no section-like heading detected'))


def conditional_alternatives(raw):
    """Split a top-level #if/else, preserving all source offsets.

    Only needed for mutually exclusive definitions (alloc_tune's two mains).
    Check both arms rather than choosing the host platform or merging their
    call edges. Nested alternatives are split on the next recursive pass.
    This is deliberately not a general preprocessor expression evaluator.
    """
    depth, start, middle = 0, None, None
    for token in TOKEN.finditer(raw):
        if token.lastgroup != 'directive':
            continue
        directive = re.sub(r'/\*.*?\*/|//.*', '', token.group()).lstrip(' \t#').strip()
        if re.match(r'if(?:def|ndef)?\b', directive):
            if depth == 0:
                start, middle = token, None
            depth += 1
        elif re.match(r'else\b', directive) and depth == 1:
            middle = token
        elif re.match(r'elif\b', directive) and depth == 1:
            # Refuse unsupported ambiguous duplicate definitions, never
            # silently discard an arm in a chain of conditions.
            start = None
        elif re.match(r'endif\b', directive):
            depth -= 1
            if depth == 0 and start is not None and middle is not None:
                first, second = (start.end(), middle.start()), (middle.end(), token.start())
                # Split only a conditional containing definitions in BOTH
                # arms, skipping include-only conditionals before the mains.
                if all(DEFINITION.search(lex(raw[a:b])[0]) for a, b in (first, second)):
                    prefix = raw[:start.start()] + mask(raw[start.start():start.end()])
                    suffix = mask(raw[token.start():token.end()]) + raw[token.end():]
                    return [prefix + raw[first[0]:first[1]] +
                            mask(raw[middle.start():second[1]]) + suffix,
                            prefix + mask(raw[first[0]:middle.end()]) +
                            raw[second[0]:second[1]] + suffix]
    return None


def check_file(raw, display, *, alternatives=0):
    try:
        return check_variant(raw, display)
    except DuplicateDefinition:
        branches = conditional_alternatives(raw) if alternatives < 8 else None
        if branches is None:
            raise
        print(f'CONDITIONAL SCOPE: {display}: checking both #if/else alternatives')
        results = [check_file(branch, f'{display} [alternative {index}]',
                              alternatives=alternatives + 1)
                   for index, branch in enumerate(branches, 1)]
        return (sum(r[0] for r in results), sum(r[1] for r in results),
                any(r[2] for r in results))


def check_variant(raw, display):
    code, strings = lex(raw)
    newlines = [pos for pos, c in enumerate(raw) if c == '\n']

    def line_at(pos):
        return bisect.bisect_left(newlines, pos) + 1

    markers = [(label, line_at(pos), start, end)
               for label, pos, start, end in marker_spans(code, strings)]
    if not markers:
        print(f'OUT OF SCOPE: {display} (0 markers; '
              f'{out_of_scope_reason(raw, code, strings)}; not verified)')
        return 0, 0, False

    definitions = functions(code, line_at)
    if 'main' not in definitions or not definitions['main'].returns_int:
        raise Refusal('section markers found but no findable int main definition')
    owners = {}
    for label, line, start, end in markers:
        matches = [name for name, fn in definitions.items()
                   if fn.start_line <= line <= fn.end_line]
        if len(matches) != 1:
            raise Refusal(f'section {label} at line {line}: cannot determine owner definition')
        owners.setdefault(matches[0], []).append((label, line, start, end))

    print(f'IN SCOPE: {display}: {len(markers)} markers, {len(owners)} owners')
    graph = {name: set() for name in definitions}
    local_calls = {name: [] for name in definitions}
    lines = code.splitlines()
    for caller, fn in definitions.items():
        for call in CALL.finditer(code, fn.body_start, fn.body_end):
            callee = call[1]
            if callee not in definitions:
                continue  # External/library calls are not local graph nodes.
            line = line_at(call.start())
            active = not (line == definitions[callee].start_line or
                          DEAD_LINE.search(lines[line - 1]))
            local_calls[caller].append((call.start(), callee, active))
            if active:
                graph[caller].add(callee)

    reached, queue = set(), deque(['main'])
    while queue:
        node = queue.popleft()
        if node in reached:
            continue
        reached.add(node)
        queue.extend(graph[node] - reached)
    failed = False
    for owner, sections in owners.items():
        if owner not in reached:
            labels = ', '.join(f'{label} (line {line})' for label, line, _, _ in sections)
            print(f'FAIL: {display}: unreachable owner {owner}: sections {labels}', file=sys.stderr)
            failed = True
        fn = definitions[owner]
        for index, (label, line, _, start) in enumerate(sections):
            end = sections[index + 1][2] if index + 1 < len(sections) else fn.body_end
            delegate = next(((callee, active) for pos, callee, active in local_calls[owner]
                             if start <= pos < end), None)
            if delegate is not None:
                callee, active = delegate
                if callee not in reached or not active:
                    print(f'FAIL: {display}: unreachable section-{label} delegate '
                          f'{callee} (header line {line})', file=sys.stderr)
                    failed = True
            elif not code[start:end].strip():
                print(f'FAIL: {display}: section {label} header at line {line} '
                      'has an empty body -- its dispatch site is missing', file=sys.stderr)
                failed = True
    return len(owners), len(markers), failed


def check_scope_ratchet(repo, current_scope):
    if not (repo / '.git').exists():
        print('SCOPE RATCHET: skipped (not a git repository)')
        return
    if shutil.which('git') is None:
        raise Refusal("SCOPE RATCHET: missing required tool 'git'; refusing to approve blind")

    def git(*args):
        try:
            return subprocess.run(['git', *args], cwd=repo, check=True,
                                  capture_output=True, encoding='utf-8').stdout
        except (OSError, UnicodeError, subprocess.CalledProcessError) as exc:
            raise Refusal(f'SCOPE RATCHET: git {" ".join(args)}: {exc}') from exc

    # Match check-test-registration.sh: never substitute HEAD for a missing base.
    base = None
    for ref in ('origin/master', 'master'):
        try:
            git('rev-parse', '--verify', '-q', ref)
            base = git('merge-base', 'HEAD', ref).strip()
            break
        except Refusal:
            continue
    if not base:
        raise Refusal('SCOPE RATCHET: no comparable base ref (origin/master or master) '
                      'resolvable from HEAD; refusing to approve blind')
    base_files = [name for name in git('ls-tree', '-r', '--name-only', '-z', base,
                                     '--', 'tests/').split('\0')
                  if Path(name).parent == Path('tests') and Path(name).match('test_*.c')]
    base_scope = set()
    for name in base_files:
        try:
            if extract_markers(*lex(git('show', f'{base}:{name}'))):
                base_scope.add(name)
        except Refusal as exc:
            raise Refusal(f'SCOPE RATCHET: {name} at base {base}: {exc}') from exc
    if base_files and not base_scope:
        raise Refusal(f'SCOPE RATCHET: found tests/test_*.c at base {base} but zero '
                      'files in scope; parser self-check failed; refusing to approve blind')
    missing = base_scope - current_scope
    if missing:
        raise Refusal(f'SCOPE RATCHET: {", ".join(sorted(missing))} left scope '
                      f'(in scope at base {base}; headers removed/reworded or file deleted)')
    print(f'SCOPE RATCHET: base {base}: {len(base_scope)} files in scope at base, '
          'all still in scope')


def main():
    if len(sys.argv) > 2:
        print('usage: check-section-registration.py [repo-dir]', file=sys.stderr)
        return 1
    repo = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parent.parent
    file_count = owner_count = marker_count = 0
    current_scope = set()
    failed = False
    try:
        # iterdir, unlike a glob that can suppress directory errors, refuses
        # unreadable directories. Read EVERY candidate, even out-of-scope ones.
        list(repo.iterdir())
        files = sorted(p for p in (repo / 'tests').iterdir()
                       if p.match('test_*.c'))
    except (OSError, Refusal) as exc:
        print(f'REFUSING: {repo}: {exc}', file=sys.stderr)
        return 1
    if not files:
        print('REFUSING: tests/test_*.c matched no files', file=sys.stderr)
        failed = True
    for path in files:
        display = f'tests/{path.name}'
        try:
            raw = path.read_text(encoding='utf-8')
            owners, markers, bad = check_file(raw, display)
            if markers:
                current_scope.add(display)
                file_count += 1
                owner_count += owners
                marker_count += markers
            failed |= bad
        except (OSError, UnicodeError, Refusal) as exc:
            print(f'REFUSING: {display}: {exc}', file=sys.stderr)
            failed = True
    try:
        check_scope_ratchet(repo, current_scope)
    except Refusal as exc:
        print(f'REFUSING: {exc}', file=sys.stderr)
        failed = True
    if failed:
        return 1
    print(f'OK: {file_count} of {len(files)} tests/test_*.c in scope '
          f'({len(files) - file_count} out of scope), '
          f'{owner_count} owners, {marker_count} markers; '
          'every section owner reachable from main')
    return 0


if __name__ == '__main__':
    sys.exit(main())
