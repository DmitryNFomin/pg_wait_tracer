#!/usr/bin/env python3
"""Live phase-counter injection, with an executed refusal on phase-less tiers.

The shared live test keeps all other injections on every tier. Here the
declared phase contract checks both the attached mask and the counter driven
only by phase markers; declared absence exits 78 with the shared marker.
"""
import argparse

from test_counter_vector_live import run_arm
from testutil import find_postmaster, resolve_declared_phase_mask


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    expected_phase = resolve_declared_phase_mask()
    pm_pid = args.pid or find_postmaster()
    assert pm_pid, "no postmaster for live phase counter test"
    run_arm(pm_pid, "exact_phase", "exact_phase_update_fail_total",
            expected_phase=expected_phase)


if __name__ == "__main__":
    main()
