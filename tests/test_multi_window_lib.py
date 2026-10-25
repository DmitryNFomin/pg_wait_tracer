#!/usr/bin/env python3
"""test_multi_window_lib.py — offline multi-window CLI expectations and parsers.

No PostgreSQL, root or running tracer. Requires a C compiler: the scripted
snapshot fixture compiles the real ring and output code in a temporary directory.
Also covers the pure parsers: Test 3's conservation check depends on
what it parses, and the parser had a defect that MANUFACTURED positive
excess inside that very check (issue #202 review).

The defect. src/output.c prints the multi-window time model per tick and
OMITS a class row whose first-window value is 0
(`if (classes[i].sort_ns == 0) continue`, output.c ~line 241), and omits
Off-CPU* when no window has one (output.c ~line 197). So the row SET
changes from tick to tick. parse_first_window() used to scan the whole
output with last-writer-wins, which kept a class's tick-2 milliseconds
alongside a DB Time read from tick 3 — the identity was checked against a
mixture of two windows. Some unknown part of #202's reported 20.2%
(18330 ms vs 15255 ms) was this, not the accounting bug.

Usage: python3 tests/test_multi_window_lib.py
"""
import os
import sys
import argparse
import re
import shlex
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_multi_window as live
from test_multi_window import parse_first_window, split_ticks

tests_run = 0
tests_passed = 0


def check(cond, msg):
    global tests_run, tests_passed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        print(f"  FAIL: {msg}")


# Two ticks in the exact shape src/output.c emits. Tick 1 has a Lock row
# and no Off-CPU*; tick 2 has neither Lock (it went to 0, so output.c drops
# the row) but does have Off-CPU*. Each tick is internally conservative:
#   tick 1:  600 + 300 + 100          = 1000 = DB Time
#   tick 2:  500 + 200 +  50 (Off-CPU) = 750 = DB Time
# Merged last-writer-wins you get DB Time 750 with Lock's 100 from tick 1
# still in the dict: 500 + 200 + 50 + 100 = 850 vs 750 = +13.3% excess out
# of thin air.
TWO_TICKS = """
--- 2026-09-28 00:00:01 ---
================================================================================
pg_wait_tracer — Time Model    Backends: 4    Interval: 5s
================================================================================

  Stat Name                          Last 5s    % DB
  -------------------------------- --------- -------
  DB Time                             1000.0  100.0%
    CPU*                               600.0   60.0%
    IO                                 300.0   30.0%
      IO:WalSync                       280.0   28.0%
    Lock                               100.0   10.0%
      Lock:transactionid                90.0    9.0%

  (Activity/Idle)                     5000.0       -


--- 2026-09-28 00:00:06 ---
================================================================================
pg_wait_tracer — Time Model    Backends: 4    Interval: 5s
================================================================================

  Stat Name                          Last 5s    % DB
  -------------------------------- --------- -------
  DB Time                              750.0  100.0%
    CPU*                               500.0   66.7%
    Off-CPU*                            50.0    6.7%
    IO                                 200.0   26.7%
      IO:WalSync                       180.0   24.0%

  (Activity/Idle)                     5200.0       -
"""

EXCLUDE = {'DB Time', 'Idle'}


def top_level(model):
    return {k: v for k, v in model.items()
            if ':' not in k and k not in EXCLUDE}


def test_single_tick():
    print("--- one tick, never a mixture of two ---")
    check(len(split_ticks(TWO_TICKS)) == 2,
          f"fixture splits into 2 ticks (got {len(split_ticks(TWO_TICKS))})")

    last = parse_first_window(TWO_TICKS)
    check(last.get('DB Time') == 750.0,
          f"the LAST tick's DB Time is parsed (got {last.get('DB Time')})")
    # The whole point: a row that tick 2 does not print must NOT be carried
    # over from tick 1. This is the assertion the old parser failed.
    check('Lock' not in last,
          f"a class tick 2 omitted is absent, not inherited from tick 1 "
          f"(rows {sorted(top_level(last))})")
    check(last.get('Off-CPU*') == 50.0,
          f"a row only tick 2 has is present (got {last.get('Off-CPU*')})")
    s = sum(top_level(last).values())
    check(abs(s - 750.0) < 0.05,
          f"tick 2 conserves: sum(top-level) = {s} vs DB Time 750.0")

    first = parse_first_window(TWO_TICKS, tick=0)
    check(first.get('DB Time') == 1000.0,
          f"tick 0 is reachable explicitly (got {first.get('DB Time')})")
    check(first.get('Lock') == 100.0 and 'Off-CPU*' not in first,
          f"…with tick 0's OWN row set (rows {sorted(top_level(first))})")
    s0 = sum(top_level(first).values())
    check(abs(s0 - 1000.0) < 0.05,
          f"tick 0 conserves too: sum(top-level) = {s0} vs DB Time 1000.0")

    # The number the old parser produced, spelled out, so the regression is
    # named rather than merely prevented.
    merged = dict(first)
    merged.update(last)
    bad = sum(top_level(merged).values())
    check(abs(bad - 850.0) < 0.05 and bad > 750.0,
          f"the merged-ticks parse would have read {bad}ms against a DB "
          f"Time of 750ms (+{100 * (bad - 750) / 750:.1f}%) — manufactured "
          f"excess, which is what this test exists to stop")


def test_cannot_see_refuses():
    """A parser that cannot see must return nothing, never a plausible dict.

    Each of these used to (or could) yield a dict that the caller would
    then happily 'verify', so they are asserted explicitly.
    """
    print("--- blind inputs yield {} , never a half-parsed model ---")
    check(parse_first_window("") == {},
          "empty output -> {} (Test 3 then fails on 'DB Time found')")
    check(parse_first_window("no tick separators here at all\n") == {},
          "output with no tick separator -> {}")
    # "waiting for data" is a REAL first tick: it has a separator but no
    # rows. It must parse to an empty model, not to a partial one.
    waiting = ("\n--- 2026-09-28 00:00:01 ---\n"
               "pg_wait_tracer — Time Model    Backends: 4    Interval: 5s\n"
               "  (waiting for data)\n")
    check(parse_first_window(waiting) == {},
          "a 'waiting for data' tick -> {} , not a partial model")
    # A tick whose DB Time row is present but whose classes are all absent
    # must still parse DB Time — absent rows are the caller's problem to
    # fail on, not something the parser should paper over.
    only_db = ("\n--- 2026-09-28 00:00:01 ---\n"
               "  DB Time                              750.0  100.0%\n")
    m = parse_first_window(only_db)
    check(m == {'DB Time': 750.0},
          f"a DB-Time-only tick parses to exactly that (got {m})")
    check(top_level(m) == {},
          "…and its top-level row set is empty, so Test 3 fails it loudly")


# Cumulative inputs, not precomputed deltas. Compile production output.c and
# snapshot.c in a temporary directory; no daemon, BPF, PG, root or network.
# Four slots + five pushes deliberately exercise wraparound. All allocations
# are on the heap: pgwt_daemon is much too large for the stack.
SCRIPT = r"""
#include "output.c"
#define MS(n) ((uint64_t)(n) * 1000000ULL)
int main(int argc, char **argv) {
    const int cpu[] = {0, 100, 300, 350, 500};
    const int io[] = {0, 300, 400, 650, 700};
    const int off[] = {0, 0, 20, 20, 40};
    const int cpu_count[] = {0, 2, 6, 7, 10};
    const int low[] = {0, 1, 4, 4, 5};
    const int high[] = {0, 3, 4, 8, 9};
    struct pgwt_daemon *d = calloc(1, sizeof(*d));
    if (!d || pgwt_ring_init(&d->ring, 4)) return 2;
    pgwt_init_event_names(18);
    uint32_t event = 0;
    for (int id = 0; id < 512; id++) {
        char name[64];
        pgwt_event_full_name(WEI(PG_WAIT_IO, id), name, sizeof(name));
        if (!strcmp(name, "IO:WalSync")) { event = WEI(PG_WAIT_IO, id); break; }
    }
    if (!event) return 3;
    d->format = PGWT_FMT_TEXT;
    d->interval = 1; d->num_windows = 2;
    d->windows[0] = 1; d->windows[1] = 3;
    int ticks = argc > 1 ? atoi(argv[1]) : 5;
    for (int i = 0; i < ticks; i++) {
        struct pgwt_accumulator *a = &d->accum;
        a->tm.cpu_time_ns = MS(cpu[i]);
        a->tm.io_time_ns = MS(io[i]);
        a->tm.offcpu_time_ns = MS(off[i]);
        a->tm.db_time_ns = MS(cpu[i] + io[i] + off[i]);
        a->num_system_events = 2;
        a->system_events[0] = (struct pgwt_event_stats){
            .wait_event = 0, .count = cpu_count[i], .total_ns = MS(cpu[i])};
        a->system_events[1] = (struct pgwt_event_stats){
            .wait_event = event, .count = low[i] + high[i],
            .total_ns = MS(io[i]), .histogram = {[14] = low[i], [15] = high[i]}};
        a->num_query_events = 3;
        a->query_events[0] = (struct pgwt_query_event_stats){
            .query_id = 42, .wait_event = 0,
            .count = cpu_count[i], .total_ns = MS(cpu[i])};
        a->query_events[1] = (struct pgwt_query_event_stats){
            .query_id = 42, .wait_event = event,
            .count = (low[i] + high[i]) / 2, .total_ns = MS(io[i]) / 2};
        a->query_events[2] = a->query_events[1];
        a->query_events[2].query_id = 84;
        pgwt_ring_push(&d->ring, a);
        puts("@@ time_model"); pgwt_print_time_model(d);
        puts("@@ system_event"); pgwt_print_system_event(d);
        puts("@@ query_a"); pgwt_print_query_event(d);
        d->event_filter = "IO:WalSync";
        puts("@@ query_b"); pgwt_print_query_event(d);
        puts("@@ histogram"); pgwt_print_histogram(d);
        d->event_filter = NULL; d->query_id_filter = 42;
        puts("@@ query_c"); pgwt_print_query_event(d);
        d->query_id_filter = 0;
    }
    pgwt_ring_free(&d->ring); free(d); return 0;
}
"""


def scripted_output(mutant=None):
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/output.c').read_text()
    replacements = {
        'copy-short': ('int ticks = d->windows[w] / d->interval;',
                       'int ticks = d->windows[w] / d->interval;\n'
                       '    if (w > 0 && d->ring.count > ticks)\n'
                       '        ticks = d->windows[0] / d->interval;'),
        'zero-histogram': ('100.0 * cnt / targets[w]->count : 0;',
                           '(w == 1 ? 0.0 : 100.0 * cnt / targets[w]->count) : 0;'),
    }
    if mutant in replacements:
        old, new = replacements[mutant]
        assert source.count(old) == 1, f'mutation anchor missing: {mutant}'
        source = source.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='pgwt-mwin-') as tmp:
        work = Path(tmp)
        (work / 'output.c').write_text(source)
        (work / 'driver.c').write_text(SCRIPT)
        dead = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        cmd = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-ffunction-sections', '-fdata-sections',
            '-Werror=implicit-function-declaration', '-I' + str(root / 'src'),
            '-I' + str(root / 'include'), str(work / 'driver.c')]
        cmd += [str(root / 'src' / name) for name in
                ('snapshot.c', 'wait_event.c', 'idle_rule.c', 'cJSON.c')]
        build = subprocess.run(cmd + [dead, '-lm', '-o', str(work / 'driver')],
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise RuntimeError('offline renderer build failed:\n' + build.stderr)
        result = subprocess.run([str(work / 'driver'),
                                 '3' if mutant == 'drop-later' else '5'],
                                check=True, capture_output=True, text=True, timeout=15)
    views = {name: [] for name in
             ('time_model', 'system_event', 'query_a', 'query_b', 'query_c', 'histogram')}
    for name, text in re.findall(r'^@@ (\w+)\n(.*?)(?=^@@ |\Z)', result.stdout,
                                 re.MULTILINE | re.DOTALL):
        views[name].append(text)
    return views


# Literal independent expectations: db/cpu/io/offcpu milliseconds; their
# printed percentages; CPU/IO counts; the two occupied histogram buckets;
# IO's half-share of %DB; and Mode C's CPU/IO %Query. None means INSUFFICIENT
# HISTORY, never "do not check". Off-CPU is not attributed to query events.
S1 = (400, 100, 300, 0, 25.0, 75.0, 0, 2, 4, 1, 3, 37.5, 40.0, 60.0)
S2 = (320, 200, 100, 20, 62.5, 31.2, 6.2, 4, 4, 3, 1, 15.6, 80.0, 20.0)
S3 = (300, 50, 250, 0, 16.7, 83.3, 0, 1, 4, 0, 4, 41.7, 28.6, 71.4)
L3 = (1020, 350, 650, 20, 34.3, 63.7, 2.0, 7, 12, 4, 8, 31.9, 51.9, 48.1)
S4 = (220, 150, 50, 20, 68.2, 22.7, 9.1, 3, 2, 1, 1, 11.4, 85.7, 14.3)
L4 = (840, 400, 400, 40, 47.6, 47.6, 4.8, 8, 10, 4, 6, 23.8, 66.7, 33.3)
EXPECTED = ((None, None), (S1, None), (S2, None), (S3, L3), (S4, L4))
BUCKETS = ('<1', '1-2', '2-4', '4-8', '8-16', '16-32', '32-64', '64-128',
           '128-256', '256-512', '512-1K', '1K-2K', '2K-4K', '4K-8K',
           '8K-16K', '>=16K')
HISTOGRAMS = {
    S1: ((1, 25.0), (3, 75.0)), S2: ((3, 75.0), (1, 25.0)),
    S3: ((0, 0.0), (4, 100.0)), L3: ((4, 33.3), (8, 66.7)),
    S4: ((1, 50.0), (1, 50.0)), L4: ((4, 40.0), (6, 60.0)),
}


def require_equal(actual, expected, context):
    if actual != expected:
        raise AssertionError(f'{context}: got {actual!r}, expected {expected!r}')


def table_rows(section):
    """Keep complete row tokens so changed or missing cells fail equality."""
    rows = []
    for line in section.splitlines():
        cells = line.split()
        if len(cells) >= 2 and re.fullmatch(r'\d+(?:\.\d+)?%', cells[-1]):
            rows.append(tuple(cells))
    return sorted(rows)


def assert_scripted_horizons(views):
    for name, ticks in views.items():
        require_equal(len(ticks), 5, f'{name} tick count')
    for ti, horizons in enumerate(EXPECTED):
        model_text = views['time_model'][ti]
        if ti > 0:
            require_equal(re.findall(r'Last \d+[smh]', model_text),
                          ['Last 1s', 'Last 3s'], f'tick {ti + 1} model horizons')
        require_equal(re.findall(r'Last \d+[smh]', views['histogram'][ti]),
                      ['Last 1s', 'Last 3s'], f'tick {ti + 1} histogram horizons')
        model = {}
        for line in model_text.splitlines():
            m = re.match(r'^\s*(DB Time|CPU\*|Off-CPU\*|IO|IO:WalSync)\s+(.*)$', line)
            if m:
                cells = m[2].split()
                require_equal(len(cells), 4, f'tick {ti + 1} model row width')
                model[m[1]] = [None if cells[w * 2:w * 2 + 2] == ['-', '-'] else
                               (float(cells[w * 2]), float(cells[w * 2 + 1].rstrip('%')))
                               for w in range(2)]
        if ti == 0:
            require_equal(model, {}, 'initial model rows')
            assert 'waiting for data' in model_text, 'initial model must say waiting for data'
        hist = [row for line in views['histogram'][ti].splitlines()
                if (row := live.parse_histogram_row(line))]
        require_equal([row['label'] for row in hist], list(BUCKETS), f'tick {ti + 1} buckets')
        for row in hist:
            require_equal(len(row['values']), 2, f'tick {ti + 1} histogram width')
        for wi in (1, 0):
            expected = horizons[wi]
            context = f'tick {ti + 1} Last {(1, 3)[wi]}s'
            wanted = {}
            if expected:
                db, cpu, io, off, cp, ip, op, cn, n, low, high, half, qc, qi = expected
                wanted = {'DB Time': (db, 100.0), 'CPU*': (cpu, cp),
                          'IO': (io, ip), 'IO:WalSync': (io, ip)}
                if any(e and e[3] for e in horizons):
                    wanted['Off-CPU*'] = (off, op)
            actual = {name: cells[wi] for name, cells in model.items()}
            require_equal(actual, wanted if expected else dict.fromkeys(model), context + ' model')
            if expected:
                require_equal(sum(value[0] for name, value in actual.items()
                                  if name not in ('DB Time', 'IO:WalSync')), db,
                              context + ' conservation')
            hist_wanted = ([{'count': 0, 'pct': 0.0}] * 14 +
                           [dict(zip(('count', 'pct'), cell)) for cell in HISTOGRAMS[expected]]
                           if expected else [None] * 16)
            for row, cell in zip(hist, hist_wanted):
                require_equal(row['values'][wi], cell,
                              context + ' histogram ' + row['label'])
            # The Total row is a separate count, not another bucket percentage.
            total_line = next((line for line in views['histogram'][ti].splitlines()
                               if line.strip().startswith('Total ')), '')
            require_equal(total_line.split()[wi + 1:wi + 2],
                          [str(n)] if expected else ['-'], context + ' histogram Total')
            for view in ('system_event', 'query_a', 'query_b', 'query_c'):
                sections = re.findall(r'^---- Last (\d+)s -+\n(.*?)(?=^---- Last |\Z)',
                                      views[view][ti], re.MULTILINE | re.DOTALL)
                require_equal([label for label, _ in sections], ['1', '3'], context + ' sections')
                section = sections[wi][1]
                if not expected:
                    assert 'waiting for data' in section, context + ' must say waiting for data'
                    require_equal(table_rows(section), [], context + ' unavailable ' + view)
                    continue
                def event_row(name, count, ms, pct):
                    return (name, str(count), f'{ms:.1f}', f'{ms * 1000 / count:.1f}', f'{pct:.1f}%')
                cpu_row = event_row('CPU*', cn, cpu, cp)
                io_row = event_row('IO:WalSync', n, io, ip)
                half_row = event_row('IO:WalSync', n // 2, io / 2, half)
                if view == 'system_event':
                    wanted_rows = [cpu_row, io_row]
                    if off:
                        wanted_rows.append(('Off-CPU*', '—', f'{off:.1f}', '—', f'{op:.1f}%'))
                elif view == 'query_a':
                    wanted_rows = [('42',) + cpu_row, ('42',) + half_row, ('84',) + half_row]
                elif view == 'query_b':
                    wanted_rows = [(qid,) + half_row[1:-1] + ('50.0%', half_row[-1])
                                   for qid in ('42', '84')]
                else:
                    wanted_rows = [cpu_row[:-1] + (f'{qc:.1f}%', cpu_row[-1]),
                                   half_row[:-1] + (f'{qi:.1f}%', half_row[-1])]
                require_equal(table_rows(section), sorted(wanted_rows), context + ' ' + view)


def test_scripted_horizons():
    views = scripted_output()
    assert_scripted_horizons(views)
    check(True, 'five production snapshots: own totals, percentages, conservation and histogram per horizon')
    # Drive the actual live Test 10 offline as well: three truncated ticks
    # and zero percentages must change ITS verdict, not just our oracle's.
    with patch.object(live, 'check') as verdict:
        live.check_histogram_population(''.join(views['histogram']))
        check(all(call.args[0] for call in verdict.call_args_list) and
              verdict.call_count == 4, 'live histogram checker accepts the complete fixture')
    for mutant in ('copy-short', 'drop-later', 'zero-histogram'):
        broken = scripted_output(mutant)
        try:
            assert_scripted_horizons(broken)
        except AssertionError as exc:
            check(True, f'RED {mutant}: {exc}')
        else:
            check(False, f'RED {mutant}: incorrectly accepted')
        if mutant != 'copy-short':
            with patch.object(live, 'check') as verdict:
                live.check_histogram_population(''.join(broken['histogram']))
                check(any(not call.args[0] for call in verdict.call_args_list),
                      f'live histogram checker rejects {mutant}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutant', choices=('copy-short', 'drop-later', 'zero-histogram'),
                        help='run a broken production fixture; must exit nonzero')
    args = parser.parse_args()
    if args.mutant:
        try:
            assert_scripted_horizons(scripted_output(args.mutant))
        except AssertionError as exc:
            print(f'FAIL {args.mutant}: {exc}')
            return 1
        print(f'PASS {args.mutant}: mutant escaped')
        return 0
    test_single_tick()
    test_cannot_see_refuses()
    test_scripted_horizons()
    print(f"\n{tests_passed}/{tests_run} checks passed")
    return 0 if tests_passed == tests_run else 1


if __name__ == "__main__":
    sys.exit(main())
