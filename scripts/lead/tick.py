#!/usr/bin/env python3
"""Lead tick: gh/git errors always wake the lead; only ack suppresses reports.

Follow-ups latch once fired and bypass ack until explicitly completed. Other
items are keyed by their entire line, so a changed reason is a new item. ack
acknowledges the last emitted snapshot, without rerunning checks or actions.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time
from datetime import datetime

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
STATE_BASE = Path(os.environ.get('PGWT_LEAD_STATE_DIR') or HERE)
STATE = STATE_BASE / 'tick-state'
OPEN_QUERY = '''query($owner:String!, $name:String!, $endCursor:String) {
  repository(owner:$owner, name:$name) {
    pullRequests(states:OPEN, first:100, after:$endCursor) {
      nodes { number createdAt updatedAt headRefOid mergeStateStatus state
              autoMergeRequest { enabledAt } }
      pageInfo { hasNextPage endCursor }
    }
  }
}'''
CHECK_QUERY = '''query($owner:String!, $name:String!, $number:Int!, $endCursor:String) {
  repository(owner:$owner, name:$name) {
    pullRequest(number:$number) { commits(last:1) { nodes { commit {
      statusCheckRollup { contexts(first:100, after:$endCursor) {
        nodes {
          ... on CheckRun { name conclusion isRequired(pullRequestNumber:$number) }
          ... on StatusContext { context state isRequired(pullRequestNumber:$number) }
        }
        pageInfo { hasNextPage endCursor }
      } }
    } } } }
  }
}'''


def command(*args):
    env = dict(os.environ)
    if args[0] == 'gh':
        # An unrelated caller's GH_REPO must not redirect this repository's tick.
        env.pop('GH_REPO', None)
    result = subprocess.run(args, cwd=REPO, capture_output=True, text=True,
                            timeout=60, env=env)
    if result.returncode:
        raise RuntimeError(f'{shlex.join(args)} failed (exit {result.returncode}): '
                           f'{result.stderr.strip() or result.stdout.strip()}')
    return result.stdout


def gh(*args):
    value = json.loads(command('gh', *args))
    if isinstance(value, dict) and value.get('errors'):
        raise RuntimeError(f'gh {shlex.join(args)}: {value["errors"]}')
    return value


def graphql(query, repo, number=None):
    owner, name = repo.split('/')
    args = ['api', 'graphql', '--paginate', '--slurp', '-f', f'query={query}',
            '-f', f'owner={owner}', '-f', f'name={name}']
    if number is not None:
        args += ['-F', f'number={number}']
    pages = gh(*args)
    for page in pages:
        if page.get('errors'):
            raise RuntimeError(f'gh graphql: {page["errors"]}')
    return pages


def load():
    path = STATE / 'state.json'
    if not path.exists():
        return {'next_id': 1, 'follows': [], 'seen': {}, 'reported': []}
    return json.loads(path.read_text())


def save(data):
    fd, name = tempfile.mkstemp(dir=STATE, prefix='.state-')
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(data, stream, sort_keys=True)
            stream.write('\n')
        os.replace(name, STATE / 'state.json')
    finally:
        if os.path.exists(name):
            os.unlink(name)


def line(item, reason):
    return f'NEEDS-LEAD {item} {" ".join(str(reason).split())}'


def running_implementers():
    count = 0
    for row in command('ps', '-ww', '-axo', 'pid=,stat=,args=').splitlines():
        fields = row.split(None, 2)
        if len(fields) != 3 or fields[1][0] in 'ZX':
            continue
        pid, _, args = fields
        try:
            words = shlex.split(args)
        except ValueError:
            continue
        if len(words) < 2 or Path(words[0]).name != 'codex' or words[1] != 'exec':
            continue
        try:
            if sys.platform.startswith('linux'):
                cwd = os.readlink(f'/proc/{int(pid)}/cwd')
            else:
                output = command('lsof', '-a', '-p', pid, '-d', 'cwd', '-Fn')
                cwd = next(row[1:] for row in output.splitlines() if row.startswith('n'))
        except (OSError, RuntimeError, StopIteration):
            # A process exiting during inspection is ordinary; an unreadable
            # live process is an error, never evidence of a free slot.
            try:
                os.kill(int(pid), 0)
            except ProcessLookupError:
                continue
            raise
        if cwd.startswith('/private/tmp/pgwt-codex/'):
            count += 1
    return count


def failed_checks(repo, number):
    failed = []
    for page in graphql(CHECK_QUERY, repo, number):
        nodes = page['data']['repository']['pullRequest']['commits']['nodes']
        rollup = nodes[0]['commit']['statusCheckRollup'] if nodes else None
        for check in rollup['contexts']['nodes'] if rollup else []:
            if check['isRequired'] and (check.get('conclusion') in
                    ('FAILURE', 'CANCELLED', 'TIMED_OUT', 'ACTION_REQUIRED',
                     'STARTUP_FAILURE', 'STALE') or check.get('state') in ('FAILURE', 'ERROR')):
                failed.append(check.get('name') or check['context'])
    return failed


def collect(data, now):
    items, acted = [], []

    # Already-fired follow-ups survive even an outage during repo discovery.
    for follow in data['follows']:
        if follow['fired'] and not follow['done']:
            items.append(line(follow['id'], f'{follow["trigger"]}; {follow["text"]}'))

    def error(exc):
        items.append(line('tick-error', exc))

    try:
        command('git', 'rev-parse', '--show-toplevel')
        repo = gh('repo', 'view', '--json', 'nameWithOwner')['nameWithOwner']
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as exc:
        error(exc)
        return items, acted
    try:
        prs = [pr for page in graphql(OPEN_QUERY, repo)
               for pr in page['data']['repository']['pullRequests']['nodes']
               if pr['autoMergeRequest'] is not None]
        behind = sorted((pr for pr in prs if pr['mergeStateStatus'] == 'BEHIND'),
                        key=lambda pr: (pr['createdAt'], pr['number']))
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as exc:
        error(exc)
        prs, behind = [], []
    for pr in prs:
        n = pr['number']
        try:
            if pr['mergeStateStatus'] == 'BEHIND':
                delta = gh('api', f'repos/{repo}/compare/{pr["headRefOid"]}...master?per_page=1')
                paths = sorted({path for file in delta['files']
                                for path in (file['filename'], file.get('previous_filename')) if path})
                risky = [path for path in paths if path.startswith('src/') or path == 'tests/run_all.sh']
                # Compare's first page contains at most 300 files. At the cap
                # the path list cannot prove safety, so request judgement.
                incomplete = len(delta['files']) >= 300
                if not risky and not incomplete and n == behind[0]['number']:
                    command('gh', 'pr', 'update-branch', str(n), '--repo', repo)
                    acted.append(f'ACTED update-branch {n}')
                    continue
                if risky or incomplete:
                    reason = ('behind; master delta touches ' + ', '.join(paths) +
                              (' (file list may be truncated)' if incomplete else '') +
                              '; box-check before update-branch')
                else:
                    reason = f'behind; waiting for oldest BEHIND PR {behind[0]["number"]} update-branch'
                items.append(line(f'pr-{n}-behind', reason))
            if pr['mergeStateStatus'] == 'DIRTY':
                items.append(line(f'pr-{n}-dirty', 'DIRTY; merge conflict'))
            failed = failed_checks(repo, n)
            if failed:
                items.append(line(f'pr-{n}-checks', 'failed required check: ' + ', '.join(failed)))
            updated = datetime.fromisoformat(pr['updatedAt'].replace('Z', '+00:00')).timestamp()
            if now - updated > 1800:
                pages = gh('api', f'repos/{repo}/actions/runs?head_sha={pr["headRefOid"]}&per_page=100',
                           '--paginate', '--slurp')
                active = any(run['status'] in ('queued', 'in_progress')
                             for page in pages for run in page['workflow_runs'])
                if not active and pr['state'] != 'MERGED':
                    items.append(line(f'pr-{n}-stalled', 'stalled'))
        except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as exc:
            error(exc)
    for follow in data['follows']:
        if follow['done']:
            continue
        try:
            if not follow['fired']:
                trigger = follow['trigger'].split(':')
                pr = gh('pr', 'view', trigger[1], '--repo', repo, '--json', 'state,mergeStateStatus')
                if ((trigger[0] == 'merged' and pr['state'] == 'MERGED') or
                        (trigger[0] == 'state' and pr['mergeStateStatus'] == trigger[2])):
                    follow['fired'] = True
                    save(data)
            if follow['fired']:
                items.append(line(follow['id'], f'{follow["trigger"]}; {follow["text"]}'))
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as exc:
            error(exc)
    try:
        running = running_implementers()
        ready = STATE_BASE / 'ready.txt'
        lines = ready.read_text().splitlines() if ready.exists() else []
        first = next((row for row in lines if row.strip()), None)
        if running < 3 and first is not None:
            items.append(line('slots', f'free slots {3 - running}; ready: {first}'))
    except (OSError, ValueError, RuntimeError, StopIteration, subprocess.SubprocessError) as exc:
        error(exc)
    return list(dict.fromkeys(items)), acted


def once(data):
    now = time.time()
    items, acted = collect(data, now)
    data['seen'] = {item: stamp for item, stamp in data['seen'].items() if item in items}
    reported = [item for item in items if item.startswith(('NEEDS-LEAD follow-', 'NEEDS-LEAD tick-error '))
                or item not in data['seen'] or now - data['seen'][item] >= 3600]
    data['reported'] = reported
    save(data)
    for item in acted + reported:
        print(item, flush=True)
    return 10 if reported else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('once')
    watch = sub.add_parser('watch')
    watch.add_argument('--interval', type=float, default=300)
    sub.add_parser('ack')
    follow = sub.add_parser('follow').add_subparsers(dest='action', required=True)
    add = follow.add_parser('add')
    add.add_argument('trigger')
    add.add_argument('text')
    follow.add_parser('list')
    done = follow.add_parser('done')
    done.add_argument('id')
    args = parser.parse_args()
    if args.command == 'watch' and not 0 < args.interval < float('inf'):
        parser.error('interval must be positive and finite')
    STATE.mkdir(parents=True, exist_ok=True)
    while True:
        with (STATE / '.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            data = load()
            if args.command in ('once', 'watch'):
                rc = once(data)
                if rc or args.command == 'once':
                    return rc
            elif args.command == 'ack':
                data['seen'].update({item: time.time() for item in data['reported']})
                save(data)
                return 0
            elif args.action == 'add':
                if not re.fullmatch(r'(merged:[1-9][0-9]*|state:[1-9][0-9]*:[A-Z_]+)', args.trigger):
                    parser.error('trigger must be merged:<pr> or state:<pr>:<STATE>')
                if not args.text.strip():
                    parser.error('follow-up text must not be empty')
                ident = f'follow-{data["next_id"]}'
                data['next_id'] += 1
                data['follows'].append(dict(id=ident, trigger=args.trigger, text=args.text,
                                            fired=False, done=False))
                save(data)
                print(ident)
                return 0
            elif args.action == 'list':
                for item in data['follows']:
                    status = 'done' if item['done'] else 'fired' if item['fired'] else 'pending'
                    print(f'{item["id"]} {status} {item["trigger"]} {item["text"]}')
                return 0
            else:
                match = next((item for item in data['follows'] if item['id'] == args.id), None)
                if match is None:
                    parser.error(f'unknown follow-up {args.id}')
                match['done'] = True
                save(data)
                return 0
        time.sleep(args.interval)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, IndexError, RuntimeError,
            StopIteration, subprocess.SubprocessError) as exc:
        print(line('tick-error', exc), flush=True)
        sys.exit(10)
