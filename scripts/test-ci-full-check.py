#!/usr/bin/env python3
"""Real workflow mutants plus execution of its verdict/exit-status shell."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import importlib.util

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("guard", HERE / "check-ci-full-check.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def main():
    workflow = (HERE.parent / ".github/workflows/ci.yml").read_text()
    guard.validate(workflow)
    job = guard.full_check_job(workflow)
    mutants = [
        ("removed job", workflow.replace("  full-check:" + job, ""), "exist exactly once"),
        ("--fast", workflow.replace("scripts/check.sh |", "scripts/check.sh --fast |"), "must not pass --fast"),
        ("self-hosted", workflow.replace(job, job.replace("runs-on: ubuntu-24.04", "runs-on: self-hosted")), "runner must be hosted"),
    ]
    with tempfile.TemporaryDirectory(prefix="pgwt-ci-full-check-") as directory:
        root = Path(directory)
        for name, mutant, reason in mutants:
            path = root / "ci.yml"
            path.write_text(mutant)
            result = subprocess.run([sys.executable, "-B", str(HERE / "check-ci-full-check.py"), str(path)],
                                    capture_output=True, text=True)
            assert result.returncode == 1 and reason in result.stderr, (name, result)
            print(f"mutant {name}: REACHED workflow guard; RED: {result.stderr.strip()}")

        (root / "scripts").mkdir()
        lock = root / "scripts/check-lock.sh"
        lock.write_text('#!/usr/bin/env bash\nexec "$@"\n')
        lock.chmod(0o755)
        check = root / "scripts/check.sh"
        run = guard.full_check_run(job)
        for name, output, code, expected in [
            ("full pass", "CHECK PASSED (full)", 0, 0),
            ("suite failure after full line", "CHECK PASSED (full)", 7, 7),
            ("fast pass", "CHECK PASSED (fast)", 0, 1),
            ("both verdicts", "CHECK PASSED (full)\\nCHECK PASSED (fast)", 0, 1),
            ("missing verdict", "suite output without verdict", 0, 1),
        ]:
            check.write_text(f'#!/usr/bin/env bash\nprintf "{output}\\n"\nexit {code}\n')
            check.chmod(0o755)
            result = subprocess.run(["bash", "-c", run], cwd=root,
                                    env={**os.environ, "RUNNER_TEMP": str(root)},
                                    capture_output=True, text=True)
            assert result.returncode == expected, (name, result)
            if name == "full pass":
                assert output in result.stdout, result
            print(f"workflow shell {name}: PASS (exit {result.returncode})")
    print("CI full-check self-test: PASS")


if __name__ == "__main__":
    main()
