#!/usr/bin/env python3
"""Hand-calculated independent observer checks; no product timing input."""
from rt_runtime_measure import analyze


def event(ns, prev, state, nxt, cpu=0):
    return (f"task {prev} [{cpu:03}] 1.{ns:09}: sched:sched_switch: "
            f"task:{prev} [{'98' if prev == 7 else '120'}] {state} ==> "
            f"task:{nxt} [{'98' if nxt == 7 else '120'}]\n")


def main():
    # Initial partial run discarded. Preempted at 30, migrates, sleeps at 60:
    # (30 - 10) + (60 - 40) = 40 ns. Next complete run is 90 - 80 = 10 ns.
    text = (event(0, 7, "S", 8) + event(10, 8, "R+", 7) + event(30, 7, "R+", 8) +
            event(40, 8, "R", 7, 1) + event(60, 7, "D", 8, 1) +
            event(80, 8, "S", 7, 2) + event(90, 7, "S", 8, 2))
    assert analyze(text, 7) == dict(longest_ns=40, completed_runs=2,
                                   switch_events=7, measured_cpu_ns=50, lost_events=0)
    for label, invalid in (("loss", text + "PERF_RECORD_LOST 1 events\n"),
                           ("missing switch-in", text + event(100, 7, "S", 8)),
                           ("incomplete", text + event(100, 8, "R", 7)),
                           ("demoted", text.replace("[98]", "[120]"))):
        try:
            analyze(invalid, 7)
        except AssertionError:
            print(f"observer rejects {label}: PASS")
        else:
            raise AssertionError(f"accepted {label}")
    print("observer hand calculation, preemption and migration: PASS")


if __name__ == "__main__":
    main()
