#!/bin/sh
# Run from any working directory; all fixture reads and artifact writes stay here.
set -eu
cd "$(dirname "$0")/.."
export PYTHONDONTWRITEBYTECODE=1
python3 - <<'PY'
import json
import pathlib
import subprocess
import sys

root = pathlib.Path.cwd()
out = root / 'reference' / 'out'
out.mkdir(parents=True, exist_ok=True)
rows = []
for path in sorted((root / 'inputs' / 'scenarios').glob('*.json')):
    scenario = json.loads(path.read_text(encoding='utf-8'))
    run = subprocess.run([sys.executable, str(root / 'reference' / 'replay.py'), str(path)],
                         check=True, capture_output=True, text=True)
    objects = [json.loads(line) for line in run.stdout.splitlines()]
    assert objects and all(isinstance(item, dict) for item in objects)
    results = [item for item in objects if 'metric' in item]
    ambiguities = [item['ambiguity'] for item in objects if 'ambiguity' in item]
    assert (len(results) == 2 and 'projection' in objects[0]) or ambiguities
    for result in results:
        assert result['metric'] in ('wait_time', 'completed_waits')
        assert (result['value'] is None) == (result['completeness'] == 'unavailable')
        assert ('detail' in result) == (result['metric'] == 'wait_time' and result['completeness'] == 'complete')
        assert len(result['diagnostic']) == 3
        assert result['reasons'] == sorted(set(result['reasons']))
        if result['completeness'] == 'complete':
            diagnostic = result['diagnostic']
            assert diagnostic['covered_ns'] == diagnostic['required_ns']
            if result['metric'] == 'wait_time':
                assert result['value'] == sum(result['detail'].values()) == diagnostic['recorded_ns']
            else:
                assert result['value'] == diagnostic['recorded_count']
    if 'projection' in objects[0]:
        for generation in objects[0]['projection']:
            core = generation['core']
            assert set(core) == {'life', 'coverage', 'waits', 'open'}
            assert len(core['coverage']) <= 1  # never combine escalation windows
            assert ('LIFE_INVALID' in generation['flags']) == (core['life'] is None)
            if core['life'] is None:
                assert core['coverage'] == core['waits'] == [] and core['open'] is None
                continue
            start, exit_at = core['life']
            assert exit_at is None or start <= exit_at
            for a, b in core['coverage']:
                assert start <= a <= b and (exit_at is None or b <= exit_at)
            for event, s, e in core['waits']:
                assert event and start <= s <= e and (exit_at is None or e <= exit_at)
            if core['open'] is not None:
                event, s, as_of = core['open']
                assert event and start <= s <= as_of
                assert exit_at is None or as_of < exit_at
                assert all(b <= as_of for a, b in core['coverage'])
                assert all(e <= s for event, u, e in core['waits'])
    (out / (scenario['id'] + '.jsonl')).write_text(run.stdout, encoding='utf-8')
    rows.append((scenario['id'], len(results), '; '.join(ambiguities) or 'none'))
    print('{}: {} result objects; ambiguity: {}'.format(*rows[-1]))

report = '''# Evidence-v1 reference replay report

All scenarios were replayed from their inputs, without comparison to expected answers.
Counts below include metric result objects only; the projection is a separate JSONL object.
The evaluator uses integer arithmetic and the lifetime-start clarification. It treats
qualified fixture descriptors as assertions, without claiming to verify kernel protocols.
It consumes merged storage records; it does not reimplement transport capture or merge.

| Scenario | Result objects | Ambiguity |
|---|---:|---|
'''
report += ''.join('| {} | {} | {} |\n'.format(*row) for row in rows)
report += '''
## Ambiguous contract sentences found

None of these questions is exercised by the supplied 33 scenarios. They remain
explicit ambiguity branches rather than alternative numerical answers.

1. Evidence-v1 §5: “An unexpected counter vector, malformed record, or unsupported
   evidence kind acts as global VOID at the preceding F; if framing/identity cannot
   be trusted, stop the file chain there instead.” For a malformed record without
   readable counters, which baseline and lifecycle-uncertainty detection instant
   does this synthetic VOID use? What precise field failures make identity untrusted
   enough to stop the chain instead? A malformed logical fixture triggers ambiguity.
2. Evidence-v1 §4: “Overlapping attachments are invalid.” Together with §5's
   invalid-record rule, this does not specify the counter baseline or the effective
   lifecycle-uncertainty time for the resulting synthetic VOID. The same question
   applies to an attachment violating §2's “Require attachment a <= END e and
   LIFE.start <= a; no attach at/after known exit.”
3. Evidence-v1 §3: “Assign transitions to runs by generation and timestamp, not
   position relative to ATTACH_OK or END.” An END with no uniquely addressable run
   has no specified invalid-record recovery baseline. Touching runs with tied
   transitions are explicitly resolved by withholding certification and are not
   treated as an ambiguity.
4. Evidence-v1 §4: “FRONTIER validates its dependencies and, if H exists, must
   exceed H; set H=F=t on acceptance.” A nonadvancing FRONTIER is not acceptable,
   but whether to ignore it, freeze active runs, or stop the chain is unspecified.
5. Evidence-v1 §7: “target is {capture,pid,fork}.” Together with §6's “A boot change
   creates a different clock domain; windows cannot cross it,” a target cannot
   disambiguate two boots that reuse the same capture identifier. The evaluator
   reports ambiguity if that occurs, rather than choosing a clock domain.
6. Evidence-v1 §5: “Run consistency failure discards the suffix back to its last
   valid certificate and acts as global VOID; contradictory LIFE invalidates that
   generation.” The retained certificate is specified, but the synthetic VOID's
   counter baseline and lifecycle-uncertainty instant are not. A run mismatch
   triggers ambiguity instead of choosing recovery behavior.

## Verification

`reference/run_all.sh` executed every scenario as a separate Python process,
parsed each emitted line as a JSON object, checked result counts, result shapes,
complete-metric arithmetic, single-candidate selection, lifetime containment,
and open-observation bounds, and wrote stdout directly to `reference/out/`.
No expected-answer files or outside repositories were used. Inputs were unchanged.
'''
(root / 'reference' / 'REPORT.md').write_text(report, encoding='utf-8')
PY
