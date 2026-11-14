#!/usr/bin/env python3
"""Compare transcribed outputs with both frozen answers, without running the core.

Only notation is expanded: K / FORK_D10, D12 / TRANS_D12, and [U] / [T].
Expected order, timestamps, flags, vectors and reasons come from the answer files.
"""
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent


def read_answers(path):
    answers = {}
    for section in re.split(r'^## Row ', path.read_text(), flags=re.M)[1:]:
        row_id = int(section.splitlines()[0])
        modes = {}
        for mode, text in re.findall(r'^Output ([NQ]): `([^`]+)`', section, re.M):
            modes[mode] = [answer_token(token.strip()) for token in text.split('→')]
        exit_match = re.search(r'^Exit: (yes|no)\s*$', section, re.M)
        if set(modes) != {'N', 'Q'} or exit_match is None:
            raise ValueError(f'{path.name}: incomplete row {row_id}')
        answers[row_id] = (modes, exit_match[1] == 'yes')
    return answers


def answer_token(token):
    if token == 'K':
        token = 'FORK_D10{fork_seed_proved=true}'
    match = re.fullmatch(r'VOID(\d+)\[(C0|Ci),([a-z_]+)\]', token)
    if match:
        return ('VOID', int(match[1]), match[2], match[3])
    match = re.fullmatch(r'F(\d+)', token)
    if match:
        return ('FRONTIER', int(match[1]), 'C0')
    match = re.fullmatch(r'FORK_D(\d+)\{fork_seed_proved=(true|false)\}', token)
    if match:
        return ('DATA', 'FORK', 'D', int(match[1]), match[2] == 'true')
    match = re.fullmatch(r'INIT_D(\d+)', token)
    if match:
        return ('DATA', 'INIT', 'D', int(match[1]))
    match = re.fullmatch(r'(TRANS_D|MARKER_D|D)(\d+)\[([UT])\]', token)
    if match:
        rec = 'MARKER' if match[1] == 'MARKER_D' else 'TRANS'
        return ('DATA', rec, 'D', int(match[2]), match[3] == 'T')
    raise ValueError(f'unrecognised answer token: {token!r}')


def json_output(row, output):
    kind = output['kind']
    if kind == 'VOID':
        return (kind, output['d'], output['C'], output['reason'])
    if kind == 'FRONTIER':
        return (kind, output['F'], output['C'])
    if kind != 'DATA':
        raise ValueError(f'unsupported JSON output: {output!r}')
    # Labels identify the input payload; output metadata is compared separately.
    inputs = [item for item in row['inputs']
              if item.get('label') == output['label']]
    if len(inputs) != 1:
        raise ValueError(f'row {row["id"]}: output label is not unique')
    rec = inputs[0]['rec']
    value = (kind, rec, output['who'], output['t'])
    if rec in ('TRANS', 'MARKER'):
        flag = output['prior_interval_certified']
        if type(flag) is not bool:
            raise ValueError('prior_interval_certified must be a boolean')
        value += (flag,)
    elif rec == 'FORK':
        flag = output['fork_seed_proved']
        if type(flag) is not bool:
            raise ValueError('fork_seed_proved must be a boolean')
        value += (flag,)
    return value


def main():
    doc = json.loads((HERE.parent / 'examples.json').read_text())
    selected = [row for row in doc['rows'] if 32 <= row['id'] <= 43]
    rows = {row['id']: row for row in selected}
    if len(selected) != 12 or len(rows) != 12:
        raise ValueError('expected rows 32-43 exactly once')
    sources = {name: read_answers(HERE / f'answers-{name}.md')
               for name in ('sol', 'sonnet')}
    failed = False
    for row_id in range(32, 44):
        row = rows[row_id]
        verdicts = []
        for name, answers in sources.items():
            expected, exit_nonzero = answers[row_id]
            modes = []
            for mode in ('N', 'Q'):
                actual = row['expected'][mode]
                outputs = [json_output(row, item) for item in actual['outputs']]
                matches = (outputs == expected[mode]
                           and actual['exit_nonzero'] is exit_nonzero
                           and not actual.get('epoch_invalid', False)
                           and not actual.get('statuses', []))
                modes.append(f'{mode}={"MATCH" if matches else "MISMATCH"}')
                if not matches:
                    failed = True
                    print(f'row {row_id} {name} {mode}: JSON={outputs!r}; '
                          f'answer={expected[mode]!r}', file=sys.stderr)
            verdicts.append(f'{name}: {" ".join(modes)}')
        print(f'row {row_id}: {"; ".join(verdicts)}')
    return int(failed)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (KeyError, ValueError) as error:
        sys.exit(f'ERROR: {error}')
