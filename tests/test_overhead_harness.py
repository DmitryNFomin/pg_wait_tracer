#!/usr/bin/env python3
"""Exercise the real overhead gate with host-only processes, no PostgreSQL/BPF.

TRACER is the gate's test-only executable override. PATH stubs supply the
workload, SQL, /proc reads and short attach sleeps; the benchmark itself is
neither copied nor mocked. All generated files live in TemporaryDirectory.
"""
import csv
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ATTACHED = "INFO: capture provider: full (exact fidelity)"


class OverheadHarness(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pgwt-overhead-control-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        os.mkfifo(self.directory / "attach-ready")
        self.log = self.directory / "server.log"
        self.log.write_text("server log evidence sentinel\n")
        self.stat = self.directory / "stat"
        self.stat.write_text("cpu  1 2 3 4 5 6 7 100 9 10\n")
        self.mem = self.directory / "meminfo"
        self.mem.write_text("MemTotal: 4194304 kB\n")
        self.env = dict(os.environ, PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}",
                        TRACER=str(self.bin / "tracer"), TMPDIR=str(self.directory),
                        CONTROL_DIR=str(self.directory),
                        REAL_AWK=shutil.which("awk"), REAL_SLEEP=shutil.which("sleep"))
        # The existing benchmark uses associative arrays (bash >= 4).
        self.bash = next((candidate for candidate in dict.fromkeys(
            [shutil.which("bash"), "/opt/homebrew/bin/bash", "/usr/local/bin/bash"])
            if candidate and Path(candidate).is_file() and subprocess.run(
                [candidate, "-c", '[[ ${BASH_VERSINFO[0]} -ge 4 ]]'],
                capture_output=True).returncode == 0), None)
        self.assertIsNotNone(self.bash, "overhead gate requires bash >= 4")
        self.stub("nproc", "print(4)\n")
        self.stub("pgrep", "raise SystemExit(1)\n")
        self.stub("sleep", """
import os, sys
from pathlib import Path
if sys.argv[1] == '2':
    # Synchronize startup rather than relying on host process scheduling.
    (Path(os.environ['CONTROL_DIR']) / 'attach-ready').read_text()
""")
        self.stub("readlink", """
import os, sys
from pathlib import Path
base = Path(os.environ['CONTROL_DIR'])
if sys.argv[1].endswith('/exe'):
    print(base / 'bin' / 'postgres')
elif sys.argv[1].endswith('/fd/2') and os.environ['SCENARIO'] == 'log_stderr':
    print(base / 'server.log')
else:
    raise SystemExit(1)
""")
        self.stub("postgres", "print('postgres (PostgreSQL) stub')\n")
        self.stub("psql", """
import os, sys
from pathlib import Path
base = Path(os.environ['CONTROL_DIR'])
sql = sys.argv[-1]
if 'count(*)' in sql:
    print(10)
elif 'pg_current_logfile' in sql:
    if os.environ['SCENARIO'] not in ('log_missing', 'log_stderr'):
        print('server.log')
elif 'data_directory' in sql:
    print(base)
""")
        self.stub("awk", """
import os, sys
from pathlib import Path
base = Path(os.environ['CONTROL_DIR'])
args = sys.argv[1:]
if '/proc/stat' in args:
    if os.environ['SCENARIO'] == 'log_missing':
        raise SystemExit(1)
    stat = base / 'stat'
    fields = stat.read_text().split()
    fields[8] = str(int(fields[8]) + 7)
    stat.write_text(' '.join(fields) + '\\n')
    args[args.index('/proc/stat')] = str(stat)
if '/proc/meminfo' in args:
    args[args.index('/proc/meminfo')] = str(base / 'meminfo')
os.execv(os.environ['REAL_AWK'], [os.environ['REAL_AWK'], *args])
""")
        self.stub("tracer", f"""
import os, signal, sys
from pathlib import Path
base = Path(os.environ['CONTROL_DIR'])
(base / 'tracer.pid').write_text(str(os.getpid()))
assert '--verbose' in sys.argv and sys.argv[sys.argv.index('--mode') + 1] == 'full'
scenario = os.environ['SCENARIO']
if scenario == 'dead':
    for line in range(30):
        print('tracer diagnostic ' + str(line), file=sys.stderr)
    print('tracer startup failure sentinel', file=sys.stderr, flush=True)
    with (base / 'attach-ready').open('w') as ready:
        ready.write('ready')
        ready.flush()
        os._exit(7)
if scenario == 'sleep':
    (base / 'attach-ready').write_text('ready')
    os.execv(os.environ['REAL_SLEEP'], [os.environ['REAL_SLEEP'], '60'])
print({ATTACHED!r}, file=sys.stderr, flush=True)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
if scenario == 'bad_stop':
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(7))
(base / 'attach-ready').write_text('ready')
while True:
    signal.pause()
""")
        self.stub("pgbench", """
import os, signal, sys, time
from pathlib import Path
base = Path(os.environ['CONTROL_DIR'])
scenario = os.environ['SCENARIO']
if sys.argv[sys.argv.index('-T') + 1] == '2':
    print('warmup failure sentinel', file=sys.stderr)
    raise SystemExit(9)
assert sys.argv[sys.argv.index('-P') + 1] == '5', 'progress must be enabled'
counter = base / 'measured.count'
run = int(counter.read_text()) + 1 if counter.exists() else 1
counter.write_text(str(run))
print('progress: 5.0 s, progress evidence run ' + str(run), file=sys.stderr)
if scenario == 'no_tps' or (scenario == 'no_tps_tracer' and run == 4):
    print('no TPS sentinel', file=sys.stderr)
    raise SystemExit(0)
if scenario == 'dies_during_run' and run == 4:
    os.kill(int((base / 'tracer.pid').read_text()), signal.SIGTERM)
    time.sleep(0.15)
values = [1000, 1000, 1000, 800, 800, 800]
if scenario in ('stall', 'log_missing', 'log_stderr'):
    values = [1000, 1000, 100, 800, 800, 80]
elif scenario == 'stall_red':
    values = [1000, 1000, 1000, 400, 400, 10]
elif scenario == 'boundary':
    values = [1000, 1000, 250, 800, 800, 200]
elif scenario == 'other_median':
    values = [100, 300, 1000, 80, 240, 800]
print('tps = ' + str(values[run - 1]) + ' (without initial connection time)')
if scenario == 'pgbench_exit':
    raise SystemExit(8)
""")
        self.addCleanup(self.ensure_tracer_stopped)

    def stub(self, name, body):
        path = self.bin / name
        path.write_text(f"#!{sys.executable}\n{body.lstrip()}")
        path.chmod(0o755)

    def ensure_tracer_stopped(self):
        pidfile = self.directory / "tracer.pid"
        if pidfile.exists():
            pid = int(pidfile.read_text())
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            os.kill(pid, signal.SIGKILL)
            self.fail(f"gate left tracer {pid} running")

    def run_gate(self, scenario, warmup=0):
        self.env['SCENARIO'] = scenario
        result = subprocess.run(
            [self.bash, str(ROOT / 'tests/test_overhead.sh'), '--pid', '424242',
             '--clients', '4', '--runs', '3', '--duration', '1',
             '--warmup', str(warmup), '--output', str(self.directory / 'runs.csv')],
            env=self.env, capture_output=True, text=True, timeout=30)
        self.output = result.stdout + result.stderr
        match = re.search(r'^Run evidence: (.+)$', self.output, re.M)
        self.assertIsNotNone(match, self.output)
        self.evidence = Path(match[1])
        return result.returncode

    def assert_error(self, code, message, runs):
        self.assertNotEqual(code, 0, self.output)
        self.assertIn(message, self.output)
        self.assertNotIn('  PASS (', self.output)
        self.assertNotIn('gross regression, something is broken', self.output)
        counter = self.directory / 'measured.count'
        self.assertEqual(int(counter.read_text()) if counter.exists() else 0, runs,
                         'failed runs must abort immediately without retry')

    def test_a_normal_prints_table_and_retains_all_run_evidence(self):
        self.assertEqual(self.run_gate('normal'), 0, self.output)
        for text in ('Summary', 'Baseline TPS', 'Tracer TPS', 'Peak overhead: 20.00%', 'PASS (10-30%'):
            self.assertIn(text, self.output)
        self.assertNotIn('STALL:', self.output)
        for arm in ('baseline', 'tracer'):
            for run in range(1, 4):
                prefix = self.evidence / f'4-{arm}-{run}'
                self.assertIn('progress:', prefix.with_suffix('.pgbench').read_text())
                self.assertIn('Steal delta: 7 clock ticks', prefix.with_suffix('.steal').read_text())
                self.assertIn('server log evidence sentinel', prefix.with_suffix('.pglog').read_text())
                if arm == 'tracer':
                    self.assertIn(ATTACHED, prefix.with_suffix('.stderr').read_text())

    def test_b_dead_tracer_is_a_liveness_error_with_stderr(self):
        self.assert_error(self.run_gate('dead'), 'ERROR: tracer liveness: not running after attach sleep', 3)
        self.assertIn('Tracer stderr (last 20 lines):', self.output)
        self.assertIn('tracer startup failure sentinel', self.output)
        self.assertNotIn('tracer diagnostic 0\n', self.output)
        self.assertIn('tracer diagnostic 29\n', self.output)

    def test_c_sleep_replacement_without_attach_signal_is_an_error(self):
        self.assert_error(self.run_gate('sleep'), 'ERROR: tracer liveness: full-mode attach signal missing', 3)
        self.assertIn('Tracer stderr (last 20 lines):', self.output)

    def test_d_pgbench_without_tps_is_an_error(self):
        self.assert_error(self.run_gate('no_tps'), 'ERROR: pgbench failed to produce TPS output', 1)
        self.assertIn('no TPS sentinel', self.output)

    def test_e_stalls_print_evidence_keep_every_measurement_and_pass(self):
        self.assertEqual(self.run_gate('stall'), 0, self.output)
        for arm, run in (('baseline', 3), ('tracer', 6)):
            self.assertIn(f'STALL: {arm}, 4 clients, run 3:', self.output)
            self.assertIn(f'progress evidence run {run}', self.output)
        self.assertIn('Steal delta: 7 clock ticks', self.output)
        self.assertIn('server log evidence sentinel', self.output)
        self.assertIn('Mean: 700.00 TPS', self.output)
        self.assertIn('Mean: 560.00 TPS', self.output)
        self.assertIn('PASS (10-30%', self.output)
        with (self.directory / 'runs.csv').open() as stream:
            self.assertEqual([row['tps'] for row in csv.DictReader(stream)],
                             ['1000', '1000', '100', '800', '800', '80'])

    def test_f_stall_does_not_excuse_gross_regression(self):
        self.assertNotEqual(self.run_gate('stall_red'), 0, self.output)
        self.assertIn('STALL: tracer, 4 clients, run 3:', self.output)
        self.assertIn('Mean: 270.00 TPS', self.output)
        self.assertIn('Peak overhead: 73.00%', self.output)
        self.assertIn('FAIL (> 50%', self.output)

    def test_g_tracer_must_survive_pgbench(self):
        self.assert_error(self.run_gate('dies_during_run'),
                          'ERROR: tracer liveness: not running after pgbench, before stop', 4)
        self.assertIn(ATTACHED, self.output)

    def test_h_pgbench_nonzero_exit_with_tps_is_an_error(self):
        self.assert_error(self.run_gate('pgbench_exit'), 'ERROR: pgbench failed (exit 8)', 1)

    def test_i_pgbench_failure_cleans_up_active_tracer(self):
        self.assert_error(self.run_gate('no_tps_tracer'), 'ERROR: pgbench failed', 4)
        self.assertIn('Tracer stderr (last 20 lines):', self.output)

    def test_j_warmup_failure_is_an_error(self):
        self.assert_error(self.run_gate('warmup', warmup=2), 'ERROR: pgbench warmup failed (exit 9)', 0)

    def test_k_unavailable_evidence_is_explicit(self):
        self.assertEqual(self.run_gate('log_missing'), 0, self.output)
        self.assertIn('STALL: baseline', self.output)
        self.assertIn('PostgreSQL log unavailable:', self.output)
        self.assertIn('Steal delta: unavailable', self.output)

    def test_l_log_falls_back_to_postmaster_stderr(self):
        self.assertEqual(self.run_gate('log_stderr'), 0, self.output)
        self.assertIn('server log evidence sentinel', self.output)

    def test_m_exactly_25_percent_is_not_labelled_a_stall(self):
        self.assertEqual(self.run_gate('boundary'), 0, self.output)
        self.assertNotIn('STALL:', self.output)

    def test_n_unexpected_tracer_stop_exit_is_an_error(self):
        self.assert_error(self.run_gate('bad_stop'), 'ERROR: tracer liveness: unexpected exit 7 at stop', 4)
        self.assertIn('Tracer stderr (last 20 lines):', self.output)

    def test_o_stall_median_excludes_the_run_itself(self):
        self.assertEqual(self.run_gate('other_median'), 0, self.output)
        self.assertIn('STALL: baseline, 4 clients, run 1: 100 TPS < 25% of other-run median 650', self.output)
        self.assertIn('STALL: tracer, 4 clients, run 1: 80 TPS < 25% of other-run median 520', self.output)


if __name__ == '__main__':
    unittest.main(verbosity=2)
