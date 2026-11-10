#!/usr/bin/env python3
"""Generate the tag board from GitHub facts and an issue-body TSV plan.

Offline directory: board.json (gh issue view number,body,createdAt), issues.json
(issue objects: number,title,state,closedAt,labels), prs.json (GraphQL PR nodes),
optional now.txt (UTC ISO render time). Missing files/data fail, never mean zero.
The board issue's createdAt is the immutable start time. Missing plan blocks are
shown explicitly; only set bootstraps one. publish requires an existing block.
"""
import argparse
import csv
from datetime import datetime, timezone
import html
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

COLUMNS = ['item', 'what', 'group', 'state', 'who', 'started', 'est0', 'est', 'size', 'note']
BEGIN, END = '<!-- plan:begin -->', '<!-- plan:end -->'
GROUPS = [('progress', 'In progress'), ('next', 'Next'),
          ('repro', 'Need a reproduction first'), ('done', 'Done')]


def utc(value):
    result = datetime.fromisoformat(value.replace('Z', '+00:00'))
    if result.tzinfo is None or result.utcoffset().total_seconds() != 0:
        raise ValueError('times must be UTC ISO timestamps: ' + value)
    return result


def fmt(value):
    return utc(value).strftime('%m-%d %H:%M') if value else '—'


def gh(*args):
    return subprocess.check_output(['gh', *args], text=True)


def numbers(item):
    return [int(n) for n in item.split('+')] if re.fullmatch(r'[1-9]\d*(?:\+[1-9]\d*)*', item) else []


def plan(body):
    if BEGIN not in body and END not in body:
        return [], ''
    if body.count(BEGIN) != 1 or body.count(END) != 1:
        raise ValueError('expected exactly one pair of plan markers')
    start, end = body.index(BEGIN), body.index(END) + len(END)
    if end < start:
        raise ValueError('plan markers are reversed')
    block = body[start:end]
    match = re.fullmatch(re.escape(BEGIN) + r'\s*```tsv\r?\n(.*?)```\s*' + re.escape(END), block, re.S)
    if not match:
        raise ValueError('plan must contain a fenced TSV')
    records = list(csv.reader(io.StringIO(match[1]), delimiter='\t', quoting=csv.QUOTE_NONE))
    if not records or records[0] != COLUMNS:
        raise ValueError('plan columns must be: ' + '\t'.join(COLUMNS))
    rows = []
    for record in records[1:]:
        if len(record) != len(COLUMNS):
            raise ValueError('plan row must have ten TSV columns')
        rows.append(dict(zip(COLUMNS, record)))
    validate(rows)
    return rows, block


def validate(rows):
    seen = set()
    for row in rows:
        if not row['item'] or row['item'] in seen:
            raise ValueError('empty or duplicate plan item: ' + row['item'])
        seen.add(row['item'])
        if row['group'] not in dict(GROUPS):
            raise ValueError('group must be progress|next|repro|done')
        for value in row.values():
            if any(c in value for c in '\r\n\t') or BEGIN in value or END in value or '```' in value:
                raise ValueError('plan fields must be single-line plain TSV values')
        for field in ('started', 'est0', 'est'):
            if row[field]:
                utc(row[field])


def encode_plan(rows):
    lines = ['\t'.join(COLUMNS)] + ['\t'.join(row[c] for c in COLUMNS) for row in rows]
    return BEGIN + '\n```tsv\n' + '\n'.join(lines) + '\n```\n' + END


def update(rows, item, assignments):
    changes = {}
    for assignment in assignments:
        key, sep, value = assignment.partition('=')
        if not sep or key not in COLUMNS or key == 'item' or key in changes:
            raise ValueError('expected distinct editable key=value fields: ' + assignment)
        changes[key] = value
    row = next((r for r in rows if r['item'] == item), None)
    fresh = row is None
    row = dict(row or dict.fromkeys(COLUMNS, ''))
    row['item'] = item
    if fresh:
        row['group'] = 'next'
    if row['est0'] and 'est0' in changes and changes['est0'] != row['est0']:
        raise ValueError('est0 is write-once: ' + item)
    row.update(changes)
    if 'est' in changes and not row['est0']:
        row['est0'] = row['est']
    result = [row if r['item'] == item else r for r in rows]
    if fresh:
        result.append(row)
    validate(result)
    return result


def paged(connection, fields, owner, repo, extra):
    query = '''query($owner:String!, $repo:String!, $endCursor:String) {
      repository(owner:$owner, name:$repo) {
        %s(first:100, after:$endCursor, %s) {
          nodes { %s } pageInfo { hasNextPage endCursor }
        }
      }
    }''' % (connection, extra, fields)
    pages = json.loads(gh('api', 'graphql', '--paginate', '--slurp',
                          '-f', 'query=' + query, '-f', 'owner=' + owner, '-f', 'repo=' + repo))
    result = []
    for page in pages:
        if page.get('errors'):
            raise ValueError('GitHub GraphQL errors: ' + str(page['errors']))
        result.extend(page['data']['repository'][connection]['nodes'])
    return result


def closing_refs(owner, repo, number):
    query = """query($owner:String!, $repo:String!, $number:Int!, $endCursor:String) {
      repository(owner:$owner, name:$repo) {
        pullRequest(number:$number) {
          closingIssuesReferences(first:100, after:$endCursor) {
            nodes { number } pageInfo { hasNextPage endCursor }
          }
        }
      }
    }"""
    pages = json.loads(gh('api', 'graphql', '--paginate', '--slurp', '-f', 'query=' + query,
                          '-f', 'owner=' + owner, '-f', 'repo=' + repo, '-F', 'number=' + str(number)))
    result = []
    for page in pages:
        if page.get('errors'):
            raise ValueError('GitHub GraphQL errors: ' + str(page['errors']))
        result.extend(page['data']['repository']['pullRequest']['closingIssuesReferences']['nodes'])
    return result


def load(args):
    if args.offline:
        root = Path(args.offline)
        board, issues, prs = [json.loads((root / name).read_text())
                              for name in ('board.json', 'issues.json', 'prs.json')]
        clock = root / 'now.txt'
        now = utc(clock.read_text().strip()) if clock.exists() else datetime.now(timezone.utc)
    else:
        board = json.loads(gh('issue', 'view', str(args.issue), '--json', 'number,body,createdAt'))
        owner, repo = gh('repo', 'view', '--json', 'nameWithOwner', '-q', '.nameWithOwner').strip().split('/')
        issues = paged('issues', 'number title state closedAt labels(first:100) { nodes { name } }',
                       owner, repo, 'labels:["tag-blocker"], states:[OPEN,CLOSED]')
        prs = paged('pullRequests', '''number headRefName isDraft autoMergeRequest { enabledAt }
                    mergeStateStatus commits(last:1) { nodes { commit { statusCheckRollup { state } } } }
                    closingIssuesReferences(first:100) { nodes { number } pageInfo { hasNextPage } }''',
                    owner, repo, 'states:[OPEN]')
        for pr in prs:
            if pr['closingIssuesReferences']['pageInfo']['hasNextPage']:
                pr['closingIssuesReferences'] = closing_refs(owner, repo, pr['number'])
            commits = pr.pop('commits')['nodes']
            pr['statusCheckRollup'] = commits[-1]['commit']['statusCheckRollup'] if commits else None
        now = datetime.now(timezone.utc)
    utc(board['createdAt'])
    rows, block = plan(board['body'])
    # Fetch unlabelled/old issues named in the plan too: their closure is a fact,
    # not an inference from absence in the tag-blocker query.
    wanted = {n for r in rows for n in numbers(r['item'])}
    if args.command == 'set':
        wanted.update(numbers(args.item))
    found = {i['number'] for i in issues}
    for number in sorted(wanted - found):
        if args.offline:
            raise ValueError('offline issues.json lacks planned issue #' + str(number))
        issues.append(json.loads(gh('issue', 'view', str(number), '--json',
                                    'number,title,state,closedAt,labels')))
    return board, rows, block, issues, prs, now


def nodes(value):
    return value.get('nodes', []) if isinstance(value, dict) else value


def tagged(issue):
    return any(label['name'] == 'tag-blocker' for label in nodes(issue['labels']))


def pr_numbers(pr):
    refs = {i['number'] for i in nodes(pr['closingIssuesReferences'])}
    branch = re.match(r'^agent/((?:[1-9]\d*-)+)', pr['headRefName'])
    if branch:
        refs.update(int(n) for n in branch[1].strip('-').split('-'))
    return refs


def derive(row, issues, prs):
    ids = numbers(row['item'])
    linked = [p for p in prs if set(ids) & pr_numbers(p)]
    complete = bool(ids) and all(issues[n]['state'].upper() == 'CLOSED' for n in ids)
    actual = max((issues[n]['closedAt'] for n in ids), key=utc) if complete else ''
    if complete:
        state, group = 'done', 'done'
    elif linked:
        state = 'merging' if any(p['autoMergeRequest'] is not None for p in linked) else 'in review'
        group = 'progress'
    else:
        state = row['state'] or 'not started'
        group = row['group']
        if ids and group == 'done':
            group = 'progress'
            state += '; issues still open'
    details = []
    if any((p['statusCheckRollup'] or {}).get('state') in ('FAILURE', 'ERROR') for p in linked):
        details.append('checks failing')
    if any(p['mergeStateStatus'] == 'BEHIND' for p in linked):
        details.append('behind master')
    if any(p['isDraft'] for p in linked):
        details.append('draft')
    if details and not complete:
        state += '; ' + '; '.join(details)
    return state, group, actual, linked


def estimate(value, now, done=False):
    text = fmt(value)
    if value and utc(value) < now and not done:
        hours = (now - utc(value)).total_seconds() / 3600
        text += f' (LATE by {hours:.1f}h)'
    return text


def cell(value):
    return html.escape(str(value), quote=False).replace('|', '\\|').replace('\n', ' ')


def render(board, rows, block, issue_list, prs, now):
    issues = {i['number']: i for i in issue_list}
    blockers = [i for i in issue_list if tagged(i)]
    opened = [i for i in blockers if i['state'].upper() == 'OPEN']
    closed = [i for i in blockers if i['state'].upper() == 'CLOSED' and utc(i['closedAt']) >= utc(board['createdAt'])]
    derived = [(r, *derive(r, issues, prs)) for r in rows]
    moved = sum(r['est'] != r['est0'] for r in rows)
    late = sum(bool(r['est']) and utc(r['est']) < now and group != 'done'
               for r, state, group, actual, linked in derived)
    out = [f"Rendered {now.strftime('%m-%d %H:%M')} UTC. Open tag-blockers: {len(opened)}. "
           f"Closed since board started: {len(closed)}. Estimates moved: {moved}. Past estimate, not done: {late}.",
           '', f"Board started {fmt(board['createdAt'])} UTC (board issue creation). All times UTC.", '']
    if not block:
        out += ['**No plan block stored. The hand-written board has not been imported.**', '']
    for group, title in GROUPS:
        out += ['## ' + title, '']
        columns = ['Issue', 'What', 'State', 'Who', 'Started', 'First estimate', 'Current estimate', 'PR']
        if group == 'done':
            columns = ['Issue', 'What', 'State', 'Who', 'First estimate', 'Actual', 'PR']
        elif group in ('next', 'repro'):
            columns += ['Size']
        out += ['| ' + ' | '.join(columns) + ' |', '| ' + ' | '.join(['---'] * len(columns)) + ' |']
        for row, state, row_group, actual, linked in derived:
            if row_group != group:
                continue
            ids = numbers(row['item'])
            item = ' + '.join('#' + str(n) for n in ids) if ids else row['item']
            bad = [str(n) for n in ids if not tagged(issues[n])]
            if bad:
                item += ' [NOT tag-blocker: #' + ', #'.join(bad) + ']'
            what = row['what']
            if row['note']:
                what += ' — ' + row['note']
            current_estimate = estimate(row['est'], now, row_group == 'done')
            refs = ', '.join('#' + str(p['number']) for p in linked) or '—'
            values = [item, what, state, row['who'] or '—']
            if group == 'done':
                values += [fmt(row['est0']), fmt(actual), refs]
            else:
                values += [fmt(row['started']), estimate(row['est0'], now), current_estimate, refs]
                if group in ('next', 'repro'):
                    values.append(row['size'] or '—')
            out.append('| ' + ' | '.join(cell(v) for v in values) + ' |')
        out.append('')
    planned = {n for r in rows for n in numbers(r['item'])}
    out += ['## Not on the plan yet', '', '| Issue | What |', '| --- | --- |']
    for issue in sorted(opened, key=lambda i: i['number']):
        if issue['number'] not in planned:
            out.append(f"| #{issue['number']} | {cell(issue['title'])} |")
    if block:
        out += ['', block]
    return '\n'.join(out) + '\n'


def publish(issue, body):
    # newline='' preserves even CRLF inside the original plan block.
    with tempfile.TemporaryDirectory(prefix='pgwt-board-') as directory:
        path = Path(directory) / 'body.md'
        with path.open('w', encoding='utf-8', newline='') as stream:
            stream.write(body)
        subprocess.run(['gh', 'issue', 'edit', str(issue), '--body-file', str(path)], check=True)


def main():
    # Options may be placed before or after the subcommand/assignments.
    options = argparse.ArgumentParser(add_help=False)
    options.add_argument('--issue', type=int, default=os.environ.get('PGWT_BOARD_ISSUE', '850'))
    options.add_argument('--offline')
    opts, remaining = options.parse_known_args()
    parser = argparse.ArgumentParser(description=__doc__, parents=[options])
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('render')
    commands.add_parser('publish')
    setter = commands.add_parser('set')
    setter.add_argument('item')
    setter.add_argument('assignments', nargs='+')
    args = parser.parse_args(remaining, namespace=opts)
    board, rows, block, issues, prs, now = load(args)
    if args.command == 'set':
        rows = update(rows, args.item, args.assignments)
        block = encode_plan(rows)
    if args.command != 'render' and not block:
        raise ValueError('no plan block: initialize with set before publishing')
    body = render(board, rows, block, issues, prs, now)
    if args.command == 'render':
        sys.stdout.write(body)
    else:
        publish(args.issue, body)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as error:
        print('board: REFUSED: ' + str(error), file=sys.stderr)
        sys.exit(2)
