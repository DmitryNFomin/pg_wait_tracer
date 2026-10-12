#!/usr/bin/env python3
"""Check README.txt's evidence table against the logs it cites, BOTH ways.

The defect this guards against actually happened on this branch: the table
was retyped after the fx_cover fixture was added, and six rows silently
disagreed with the committed logs -- one of them reading as a speedup the
logs contradicted. A table is evidence only if it is transcription, so this
makes the transcription checkable instead of trusted.

THREE CHECKS, because any one of them alone has a false negative:

  1. LOG SIDE   -- the exact phrase each row quotes must appear verbatim in
                   the log named in that row's source column. Alone, this
                   passes a table that quotes a *different* number: the log
                   still contains the right phrase, nobody asked whether the
                   table does.
  2. TABLE SIDE -- the value each row publishes must appear in the table
                   itself. This is the check whose absence let probe A's
                   injected 8412 coexist with a "31/31 verified" verdict in
                   the first version of this script.
  3. STALE      -- none of the six values the review caught may appear in the
                   table (they are allowed in the correction note above it,
                   which quotes them on purpose). Catches a revert.

And it REFUSES rather than approves when it cannot see: a missing log, an
empty log, a README whose correction note or table header it cannot locate,
and an empty parse of either are all nonzero exits, never "nothing wrong".

Run from this directory:  python3 verify_table.py
"""
import sys

SRCS = ('unit-test-mac.txt', 'unit-test-linux.txt',
        'oneblock-falsification-mac.txt', 'mutations.txt',
        'mutation-selftest.txt', 'selftest-assertion-red.txt',
        'box-check-summary.txt', 'make-check-tail.txt')

# (label, phrase that must be in the LOG, log file, text that must be in the
#  README TABLE)
CLAIMS = [
    ('unit test green (Mac)',         '121 checks, 0 failed',
     'unit-test-mac.txt',                   '121 passed / 0 failed'),
    ('bit-exact answers compared',    '8476 (cut, window, resolution)',
     'unit-test-mac.txt',                   '| 8476 |'),
    ('block-cut sets built',          '802 block-cut sets',
     'unit-test-mac.txt',                   '| 802 |'),
    ('of them multi-block',           '794 indexes had MORE THAN ONE',
     'unit-test-mac.txt',                   '794 of the 802 were multi-block'),
    ('intervals compared',            '44266 intervals were actually',
     'unit-test-mac.txt',                   '| 44266 |'),
    ('cross-block intervals',         '2714 intervals straddled',
     'unit-test-mac.txt',                   '| 2714 |'),
    ('intervals before coverage',     '545 intervals started before',
     'unit-test-mac.txt',                   '| 545  |'),
    ('open-past-`to` exclusions',     '3052 intervals were excluded',
     'unit-test-mac.txt',                   '| 3052 |'),
    ('end-before-`from` exclusions',  '4020 intervals were excluded',
     'unit-test-mac.txt',                   '| 4020 |'),
    ('same pid, 2 blocks, 1 bucket',  '1058 answers had the SAME pid',
     'unit-test-mac.txt',                   '| 1058 |'),
    ('answers with peak > 1',         '5286 answers had a bucket peak',
     'unit-test-mac.txt',                   '| 5286 |'),
    ('bursts spanning >= 2 blocks',   '4694 answers contained a burst',
     'unit-test-mac.txt',                   '| 4694 |'),
    ('burst onsets detected',         '6214 burst onsets',
     'unit-test-mac.txt',                   '| 6214 |'),
    ('windows that selected nothing', '1148 windows selected nothing',
     'unit-test-mac.txt',                   '| 1148 |'),
    ('prefilter skips across sweep',  'a block 20134 times',
     'unit-test-mac.txt',                   '| 20134 |'),
    ('storage compaction 6.0x',       '960000 index bytes for 5760000',
     'unit-test-mac.txt',                   '960000 B index / 5760000 B raw'),
    ('materialised compaction 3.0x',  '960048 bytes vs 2880048',
     'unit-test-mac.txt',                   '960048 B / 2880048 B in window'),
    ('blocks prefiltered away',       'SKIPPED 19 of 40',
     'unit-test-mac.txt',                   '| 19 of 40 |'),
    ('Mac timing (index SLOWER)',     'index_us=1475.0 raw_us=1266.0',
     'unit-test-mac.txt',                   '| 1475.0 vs 1266.0 |'),
    ('GATE RED: ledger',              '15 checks, 5 failed',
     'oneblock-falsification-mac.txt',      '5/15 checks'),
    ('GATE RED: mutations 27/27',     'detected (RED):   27',
     'mutations.txt',                       '| 27/27 |'),
    ('mutations, no blind spot',      'STAYED-GREEN:     0',
     'mutations.txt',                       '| 27 detected |'),
    ('harness self-test green',       'SELF-TEST PASSED',
     'mutation-selftest.txt',               '0 wrongly approved, 0 wrong-reason'),
    ('GATE RED: self-test assert',    'SELF-TEST FAILED',
     'selftest-assertion-red.txt',          'exit 1, case 7 FAIL'),
    ('...for the right reason',       'STAYED-GREEN is 0, not 1',
     'selftest-assertion-red.txt',          'selftest-assertion-red.txt'),
    ('box-check live tier',           'passed 102, failed 0',
     'box-check-summary.txt',               '102 passed / 102 executed, 0 failed'),
    ('live UI smoke',                 'overall=PASS',
     'box-check-summary.txt',               '| overall=PASS |'),
    ('unit test green (gate-1)',      '121 checks, 0 failed',
     'unit-test-linux.txt',                 '| 121/121 | unit-test-linux.txt'),
    ('gate-1 timing pair',            'index_us=8887.9 raw_us=9340.0',
     'unit-test-linux.txt',                 '| 8887.9 vs 9340.0 |'),
    ('make check verdict',            'CHECK PASSED (full)',
     'make-check-tail.txt',                 '| PASSED (full) |'),
    ('29 of 29 python checks',        'Ran 29, passed 29, failed 0',
     'make-check-tail.txt',                 '| 29/29 python checks |'),
]

# The six values the review caught. Allowed in the correction note (which
# quotes them deliberately), never in the table.
# Values no evidence file may publish. The first five are the stale numbers
# the review caught. The last two are make-check STAMPS: scripts/check.sh
# hashes the WHOLE tree, so any file written after the run -- including
# verify-table.txt -- makes a quoted stamp wrong. The push guard
# recomputes it live; evidence text must not freeze it.
STALE = ('8412', '44074', '2697', '4666', '803 vs 1051',
         'df0e1433', '5cb212c8')


def fail(msg):
    print("REFUSE: %s" % msg)
    return 1


def main():
    srcs = {}
    for n in SRCS:
        try:
            srcs[n] = open(n).read()
        except OSError as e:
            return fail("cannot read %s (%s) — a checker that cannot see "
                        "must refuse, never approve" % (n, e))
        if not srcs[n].strip():
            return fail("%s is empty — every claim against it would be "
                        "vacuously unverifiable" % n)
    try:
        readme = open('README.txt').read()
    except OSError as e:
        return fail("cannot read README.txt (%s)" % e)

    note_at = readme.find('The corrections were:')
    table_at = readme.find('\nclaim | value | unit |')
    if note_at < 0 or table_at < 0 or table_at < note_at:
        return fail("could not locate the correction note AND the table "
                    "header in README.txt, in that order — without the split "
                    "a stale value in the table is indistinguishable from one "
                    "quoted in the note")
    table = readme[table_at:]
    if len(table.splitlines()) < len(CLAIMS):
        return fail("the table parsed to %d lines for %d claims — an empty or "
                    "truncated parse must never read as 'nothing wrong'"
                    % (len(table.splitlines()), len(CLAIMS)))
    if not CLAIMS:
        return fail("CLAIMS is empty — every comparison would be vacuous")

    bad = []
    for label, log_needle, src, tbl_needle in CLAIMS:
        in_log = log_needle in srcs[src]
        in_tbl = tbl_needle in table
        print("%-5s %-5s %-30s %-36s %s"
              % ("ok" if in_log else "MISS", "ok" if in_tbl else "MISS",
                 label, repr(log_needle), src))
        if not in_log:
            bad.append("log:%s" % label)
        if not in_tbl:
            bad.append("table:%s (%r not in the table)" % (label, tbl_needle))

    for s in STALE:
        if s in table:
            print("MISS  stale value %r is still in the TABLE (allowed only "
                  "in the correction note)" % s)
            bad.append("stale:%s" % s)

    print()
    print("%d claims; %d log-side mismatches, %d table-side mismatches, "
          "%d stale values in the table"
          % (len(CLAIMS),
             len([b for b in bad if b.startswith('log:')]),
             len([b for b in bad if b.startswith('table:')]),
             len([b for b in bad if b.startswith('stale:')])))
    print("VERIFIED" if not bad else "MISMATCH: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
