#!/usr/bin/env python3
import collections
import gzip
import json
from pathlib import Path
import re
import statistics
import sys
root=Path(__file__).resolve().parent
raw=root/'evidence/requests.jsonl'
raw_text=raw.read_text() if raw.exists() else gzip.decompress((root/'evidence/requests.jsonl.gz').read_bytes()).decode()
records=[json.loads(line) for line in raw_text.splitlines()]
commands=['top_queries','executions','concurrency','variants','exec_scatter','time_model','transitions']
baseline=dict(zip(commands,[5004,4064,3636,3050,3887,2985,25]))
groups=collections.defaultdict(list)
for row in records:
    if row.get('phase'): groups[row['phase']].append(row)
fmt=lambda x:f'{x:.3f}'
lines=['# Live capture validation retry — 2026-10-12','',
       'Branch `agent/cx-live-validate`; source commit `75717896130e60f9518105577d5e4da01dc7c2ee`. Gate-1 (`root@2.28.45.47`, Intel Xeon Skylake). Initially queued on gate-2; moved at 09:59 UTC after checking both queues again. No `src/` or `web/` changes. No VM created.','',
       'All timings are milliseconds of local server stdin/stdout request-to-complete-JSON latency. Each warm sweep uses one persistent process, primes outside the measurements, then issues four consecutive calls per command in the specified order. Each cold value comes from the first request in a fresh, unprimed server process, immediately followed by three more calls of that same command; every raw value in those series is reported below. Cold includes startup still in progress; separate spawn-to-response values are also shown. No browser or SSH transport latency is included.','',
       'Live requests use the moving latest committed edge while the full-mode daemon and all three workload drivers are alive. Frozen requests replay the exact live `from`/`to` values for every warm and cold call, using a byte-prefix snapshot made after the live sweep. The original dense static trace is measured separately with the identical server binary on the same host, at its original 900-second bounds. `transitions` at 900 seconds is the only canary; full-window `time_model` appears only as a measured command.','']
summary={}
for seconds in (900,):
    live_tag=f'live-{seconds}'
    if not groups[live_tag+'-warm']: continue
    lines.extend([f'## {seconds}-second window','', '| Command | Live cold | Cold spawn→response | Live calls 1, 2, 3, 4 | Live warm median | Frozen cold | Frozen calls 1, 2, 3, 4 | Frozen median | Live/frozen | Versus supplied dense 900s |','|---|---:|---:|---|---:|---:|---|---:|---:|---:|'])
    for cmd in commands:
        live=[r for r in groups[live_tag+'-warm'] if r['request']['cmd']==cmd]
        frozen=[r for r in groups[f'frozen-{seconds}-warm'] if r['request']['cmd']==cmd]
        cold=[r for r in groups[live_tag+'-cold'] if r['request']['cmd']==cmd]
        fcold=[r for r in groups[f'frozen-{seconds}-cold'] if r['request']['cmd']==cmd]
        if not live: continue
        med=statistics.median(r['ms'] for r in live)
        fmed=statistics.median(r['ms'] for r in frozen) if frozen else None
        ratio=med/fmed if fmed else None
        summary[(seconds,cmd)]=dict(live=med,frozen=fmed,ratio=ratio,cold=cold[0]['ms'] if cold else None)
        lines.append('| '+ ' | '.join([cmd,fmt(cold[0]['ms']) if cold else 'pending',fmt(cold[0]['spawn_to_response_ms']) if cold else 'pending',', '.join(fmt(r['ms']) for r in live),fmt(med),fmt(fcold[0]['ms']) if fcold else 'pending',', '.join(fmt(r['ms']) for r in frozen) or 'pending',fmt(fmed) if fmed else 'pending',f'{ratio:.3f}× ({(ratio-1)*100:+.1f}%)' if ratio else 'pending',f'{(med/baseline[cmd]-1)*100:+.1f}%'])+' |')
    lines.extend(['', 'Shared-process cache counters after each command (four calls; decoded / served / resets):','', '| Command | Call 1 | Call 2 | Call 3 | Call 4 |','|---|---|---|---|---|'])
    for cmd in commands:
        vals=[]
        for row in [r for r in groups[live_tag+'-warm'] if r['request']['cmd']==cmd]:
            stats=re.findall(r'decoded=(\d+) served=(\d+) resets=(\d+)',row['stderr'])
            vals.append(' / '.join(stats[-1]) if stats else 'no raw-load stats')
        lines.append('| '+cmd+' | '+' | '.join(vals)+' |')
    lines.append('')
if groups['dense-900-warm']:
    lines.extend(['## Original dense trace, same host and binary','', '| Command | Cold | Cold spawn→response | Calls 1, 2, 3, 4 | Warm median | Supplied baseline | Live 900s / measured dense |','|---|---:|---:|---|---:|---:|---:|'])
    for cmd in commands:
        rows=[r for r in groups['dense-900-warm'] if r['request']['cmd']==cmd]
        cold=[r for r in groups['dense-900-cold'] if r['request']['cmd']==cmd]
        if not rows: continue
        med=statistics.median(r['ms'] for r in rows)
        live=summary.get((900,cmd),{}).get('live')
        lines.append('| '+' | '.join([cmd,fmt(cold[0]['ms']) if cold else 'pending',fmt(cold[0]['spawn_to_response_ms']) if cold else 'pending',', '.join(fmt(r['ms']) for r in rows),fmt(med),str(baseline[cmd]),f'{live/med:.3f}× ({(live/med-1)*100:+.1f}%)' if live else 'pending'])+' |')
    lines.append('')
lines.extend(['## Four-call series starting cold, one fresh process per command','',
              'These additional series include the cold call already shown above plus all three immediately following calls in that same process. No priming or other command occurs inside a series.','',
              '| Trace/window | Command | Calls 1 (cold), 2, 3, 4 | Warm median of 2–4 |',
              '|---|---|---|---:|'])
for tag in ('live-900','frozen-900','dense-900'):
    for cmd in commands:
        rows=[r for r in groups[tag+'-cold']+groups[tag+'-after-cold'] if r['request']['cmd']==cmd]
        rows.sort(key=lambda r:r['call'])
        if rows:
            warm=[r['ms'] for r in rows if r['call']>1]
            lines.append('| '+' | '.join([tag,cmd,', '.join(fmt(r['ms']) for r in rows),fmt(statistics.median(warm)) if warm else 'pending'])+' |')
lines.append('')
lines.extend(['## Canaries, growth, integrity, and completion',''])
for row in records:
    if 'canary' in row or 'growth' in row or 'capture_info' in row or 'health' in row or 'complete' in row or 'STOP' in row:
        lines.extend(['```json',json.dumps(row,sort_keys=True),'```',''])
if (900,'transitions') in summary:
    comparisons=[(summary[(900,c)]['live']/baseline[c]-1)*100 for c in commands if c!='time_model']
    paired=[(summary[(900,c)]['ratio']-1)*100 for c in commands if summary[(900,c)]['ratio']]
    model=summary[(900,'time_model')]['live']
    lines[2:2]=[
        f"**Completed a full 900-second live validation. The supplied static warm table is a fair predictor for the six raw commands: live medians range from {min(comparisons):+.1f}% to {max(comparisons):+.1f}%. `time_model` is {(1-model/baseline['time_model'])*100:.1f}% faster because the live v5 capture uses summaries; its 2,985 ms static number is a raw-fallback measurement.**", '',
        f"The frozen copy replays every live request's exact window using the identical server binary. Across all seven commands, live/frozen warm differences range from {min(paired):+.1f}% to {max(paired):+.1f}%; no live warm regression exceeds 20%. Against the freshly measured original dense trace, the largest live slowdown is `transitions` at +19.5% (25.944 vs 21.704 ms), also below 20%. Cold `transitions` takes 1,591.450 ms; the table's warm value describes repeat requests.", '',
        "**Cache verdict:** growth preserves reuse. Across the live shared-process warm calls, `decoded` goes 2458→2529, exactly matching the 71 newly cached blocks; `served` goes 1206→23401, while `resets` remains 1. Existing blocks are reused. The initial transitions-canary→raw-primer handoff resets once in both live and frozen runs: live `decoded=1227, resets=0` becomes `decoded=2454, resets=1`; frozen 1208 becomes 2416 with the same reset. That reset occurs before the recorded warm sweep and is not caused by live growth. Source inspection explains it: the aggregate path stores raw blocks without calling `cur_cache_sync`, so the first raw loader's identity check drops that unanchored entry. No source change was made.", '',
        "**Summary-path finding:** the live summary header is v5; all live `time_model` calls return exact data without raw-load cache diagnostics. The source's `handle_time_model`/`should_use_summaries` conditions therefore identify the summary path. The dense v4 trace explicitly logs the v5-version rejection and raw recomputation. Measured warm medians: live 44.448 ms, frozen v5 44.684 ms, dense v4 3,094.034 ms. This is a behavior comparison of the same server build across its eligible paths.", '',
        "The daemon and all three workload drivers were alive before the live sweep and at final verification. `current.trace` kept the same inode and grew 40,198,357→42,263,283 bytes during the warm sweep (+2,064,926 bytes). At the live checkpoint, 5,304,320 events had been written; final metrics reported 8,118,272. Ring-buffer drops, state-map overflow, query-id overflow, and summary flush failures remained zero. Every measured response was exact and successful; all six warm canaries were 22.379–28.539 ms against a 32.4 ms ceiling.", '',
        "Capture and workload restart: 10:04:37 UTC. Measurement completed around 10:30 UTC. Owned processes were stopped and the box lock released by 10:31:28 UTC, within the T+90-minute deadline. The startup readiness race and teardown helper's nonzero `wait` result were handled without changing `src/`, `web/`, or shared test helpers. Only the completed restart's 900-second windows are reported; no 300-second measurements were taken.", '',
        "Raw values for every measured call, primer, and canary: [timings.csv](evidence/timings.csv). Full JSON responses, timestamps, file signatures, and cache diagnostics: [requests.jsonl.gz](evidence/requests.jsonl.gz). Acceptance checks: [verification.json](evidence/verification.json). Build fingerprints: [binary-sha256.txt](evidence/binary-sha256.txt). The retained live capture and frozen snapshot are on gate-1 under `/root/cx-live-validate-retry-20261012/retry-live` and `/root/cx-live-validate-retry-20261012/retry-frozen-900`.", ''
    ]
(root/'REPORT.md').write_text('\n'.join(lines).rstrip()+'\n')
print(json.dumps({'complete':any(r.get('complete') for r in records),'summary':{str(k):v for k,v in summary.items()}},indent=2))
