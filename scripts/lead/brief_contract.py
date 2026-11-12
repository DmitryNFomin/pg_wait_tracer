"""Shared brief structure and budget parser; CLI prints wall-clock seconds."""
from decimal import Decimal
import math
from pathlib import Path
import re
import sys

SECTIONS = ('Authorised by', 'Task', 'Excluded', 'Done', 'Budget', 'Report by')


def validate(text):
    # Headings inside pasted evidence cannot supply the dispatch contract.
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r'^ {0,3}(`{3,}|~{3,})(.*)$', line)
        if fence is None and marker:
            fence = marker[1]
        elif fence is not None:
            if re.fullmatch(r' {0,3}' + re.escape(fence[0]) +
                            r'{' + str(len(fence)) + r',}\s*', line):
                fence = None
        else:
            lines.append(line)
    if fence is not None:
        raise ValueError('unterminated evidence fence')
    prose = re.sub(r'<!--[\s\S]*?-->', '', '\n'.join(lines))
    if '<!--' in prose:
        raise ValueError('unterminated comment')
    headings = list(re.finditer(r'^## ([^\n]+)\s*$', prose, re.M))
    sections = {}
    for index, heading in enumerate(headings):
        name = heading[1].strip()
        if name not in SECTIONS:
            continue
        if name in sections:
            raise ValueError(f'duplicate section: {name}')
        end = headings[index + 1].start() if index + 1 < len(headings) else len(prose)
        sections[name] = prose[heading.end():end].strip()
    for name in SECTIONS:
        if not sections.get(name):
            raise ValueError(f'missing or empty section: {name}')
    budget = sections['Budget']
    rounds = re.findall(r'^Rounds:[ \t]*(.*)$', budget, re.M)
    minutes = re.findall(r'^Wall-clock minutes:[ \t]*(.*)$', budget, re.M)
    if (len(rounds) != 1 or len(minutes) != 1
            or not re.fullmatch(r'[1-9][0-9]*', rounds[0].strip())
            or not re.fullmatch(r'[0-9]+(?:\.[0-9]+)?', minutes[0].strip())
            or Decimal(minutes[0]) <= 0):
        raise ValueError('Budget needs one positive Rounds: integer and Wall-clock minutes: number')
    seconds = Decimal(minutes[0]) * 60
    # The process supervisor consumes a float; refuse overflow/underflow here.
    if not math.isfinite(float(seconds)) or float(seconds) <= 0:
        raise ValueError('wall-clock budget is outside the supported range')
    return str(seconds)


if __name__ == '__main__':
    try:
        if len(sys.argv) != 2:
            raise ValueError('usage: brief_contract.py <brief-file>')
        print(validate(Path(sys.argv[1]).read_text(encoding='utf-8')))
    except (OSError, UnicodeError, ValueError) as exc:
        print(f'CHECK BRIEF BLOCK: {exc}', file=sys.stderr)
        sys.exit(1)
