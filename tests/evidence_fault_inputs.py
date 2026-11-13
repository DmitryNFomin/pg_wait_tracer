"""R3 physical-prefix faults and readable counter-loss recovery witnesses."""
import copy
import random


def named(fixture):
    cases = []
    base = fixture("01")
    # A prior certificate makes a middle cut observably different from rejecting
    # the capture or accepting its later suffix. Fault timestamps are unrelated
    # to append positions.
    base["files"][0]["records"].insert(5, {
        "kind": "FRONTIER", "t": 8, "counters": [0] * 15})
    faults = {
        "malformed": {"kind": "TRANS", "pid": 101},
        "unknown": {"kind": "FUTURE", "t": 1},
        "structural": {"kind": "END", "t": 0, "pid": 999, "fork": None, "reason": "stop"},
    }
    for kind, fault in faults.items():
        for position, at in (("start", 0), ("middle", 6), ("end", len(base["files"][0]["records"]))):
            c = copy.deepcopy(base)
            c["id"] = f"fault_{kind}_{position}"
            r = c["files"][0]["records"]
            r.insert(at, copy.deepcopy(fault))
            # No later revocation, LIFE exit or certificate may survive a cut.
            r.extend([{"kind": "VOID", "t": 30, "counters": [0] * 15,
                       "reason": "must not revoke prefix", "invalid_from": 0},
                      {"kind": "LIFE", "t": 40, "pid": 101, "fork": 0,
                       "start": 0, "start_known": True, "exit": 40},
                      {"kind": "FRONTIER", "t": 50, "counters": [0] * 15}])
            cases.append(c)
            for target_faulty in (False, True):
                two = copy.deepcopy(c)
                other = copy.deepcopy(base["files"][0])
                other["capture"] = "independent"
                other["boot"] = "independent-boot"
                two["files"].append(other)
                if not target_faulty:
                    two["target"]["capture"] = "independent"
                two["id"] += "_two_faulty" if target_faulty else "_two_good"
                cases.append(two)
            rotated = copy.deepcopy(c)
            first = rotated["files"][0]
            # Keep the fault in the successor, even for a fault at position 0.
            rotated["files"] = [dict(first, records=first["records"][:at]),
                                  dict(first, file_no=1, previous=0, records=first["records"][at:])]
            rotated["id"] += "_rotated"
            cases.append(rotated)
    for prefix in ("38", "40", "42", "44", "46", "48", "50", "55", "56", "57", "58", "59", "60", "61", "62", "63", "64"):
        c = fixture(prefix)
        c["id"] = "fault_rule_" + c["id"]
        cases.append(c)
    def add(prefix, label, edit):
        c = fixture(prefix)
        c["id"] = "fault_" + label
        edit(c)
        cases.append(c)
    def rec(c): return c["files"][0]["records"]
    def nonadvancing_changed(c):
        changed = [0] * 15
        changed[11] = 1
        rec(c).extend([{"kind":"FRONTIER", "t":14, "counters":changed},
                       {"kind":"VOID", "t":30, "counters":changed,
                        "reason":"later revocation", "invalid_from":0}])
    add("01", "nonadvancing_changed_counter", nonadvancing_changed)
    def decrease(c):
        f = c["files"][0]
        f["loss_baseline"][11] = 1
        for r in rec(c):
            if "counters" in r: r["counters"][11] = 1
        rec(c).extend([{"kind":"FRONTIER", "t":16, "counters":[0]*15},
                       dict(rec(c)[1], t=17, counters=[0]*15),
                       {"kind":"FRONTIER", "t":25, "counters":[0]*15}])
        c["query"].update(**{"from":17, "to":25})
    add("01", "counter_decrease", decrease)
    add("01", "unknown_baseline", lambda c: c["files"][0].update(loss_baseline=None))
    def delayed_attach(c):
        r = rec(c)
        end = next(r.pop(i) for i in range(len(r)) if r[i]["kind"] == "END")
        r.insert(1, end)
    add("18", "delayed_END_attach", delayed_attach)
    add("03", "END_after_exit", lambda c: rec(c).insert(-1,
        {"kind":"END", "t":11, "pid":101, "fork":0, "reason":"stop"}))
    def eof_fault(c):
        r = rec(c)
        r.insert(-2, {"kind":"FRONTIER", "t":8, "counters":[0]*15})
        r[-2].update(t=100, old="inconsistent", duration=0)
    add("01", "EOF_semantic", eof_fault)
    add("18", "unattached_after_END", lambda c: rec(c).insert(-1,
        {"kind":"TRANS", "t":13, "pid":101, "old":None, "new":"$EXIT", "duration":0}))
    add("01", "unattached_before_anchor", lambda c: rec(c).insert(1,
        {"kind":"TRANS", "t":0, "pid":101, "old":None, "new":"$EXIT", "duration":0}))
    add("03", "unattached_after_exit", lambda c: rec(c).insert(-1,
        {"kind":"TRANS", "t":11, "pid":101, "old":None, "new":"Lock", "duration":0}))
    def frozen_gap(c):
        changed = [0]*15
        changed[11] = 1
        rec(c).extend([{"kind":"VOID", "t":16, "counters":changed, "reason":"ring", "invalid_from":None},
                       {"kind":"LIFE", "t":20, "pid":101, "fork":20, "start":20, "start_known":True, "exit":None},
                       {"kind":"TRANS", "t":25, "pid":101, "old":None, "new":"$EXIT", "duration":0},
                       dict(rec(c)[1], t=30, fork=20, counters=changed),
                       {"kind":"FRONTIER", "t":40, "counters":changed}])
        c["target"]["fork"] = 20
        c["query"].update(**{"from":30, "to":40})
    add("01", "frozen_run_gap", frozen_gap)
    def earliest_fault(c):
        r = rec(c)
        r[:] = r[:3] + [dict(r[1], pid=202, fork=5),
                        {"kind":"FRONTIER", "t":8, "counters":[0]*15},
                        {"kind":"TRANS", "t":6, "pid":202, "old":"bad", "new":"Lock", "duration":0},
                        {"kind":"LIFE", "t":5, "pid":202, "fork":5, "start":5, "start_known":True, "exit":None},
                        {"kind":"FRONTIER", "t":15, "counters":[0]*15}]
    add("01", "earliest_dependency", earliest_fault)
    def duplicate_descriptor(c):
        f = c["files"][0]
        c["files"].append(dict(f, records=[{"kind":"VOID", "t":30,
            "counters":[0]*15, "reason":"duplicate must not revoke", "invalid_from":0}]))
    add("01", "duplicate_descriptor_revocation", duplicate_descriptor)
    def observed_without_fork(c):
        c["target"]["fork"] = None
        rec(c)[0]["fork"] = None
        rec(c)[1]["fork"] = None
    add("01", "observed_without_fork", observed_without_fork)
    def exit_before_start(c):
        c["target"]["fork"] = 2
        rec(c)[0].update(start=2, fork=2, t=3, exit=1)
        rec(c)[1]["fork"] = 2
    add("01", "exit_before_start", exit_before_start)
    def future_ambiguous(c):
        r = rec(c)
        r[:] = r[:3] + [dict(r[0], t=20, fork=20, start=20),
                        dict(r[1], t=21, fork=20),
                        {"kind":"TRANS", "t":25, "pid":101, "old":None, "new":"Lock", "duration":0},
                        {"kind":"FRONTIER", "t":10, "counters":[0]*15},
                        {"kind":"FRONTIER", "t":30, "counters":[0]*15}]
    add("01", "future_ambiguous_assignment", future_ambiguous)
    def invalid_provisional(c):
        r = rec(c)
        r[:] = [dict(r[0], fork=None, start_known=False),
                dict(r[0], fork=None, start_known=False, start=1, t=1),
                dict(r[0], fork=5, start=5, t=5), dict(r[1], fork=5, t=6),
                {"kind":"TRANS", "t":8, "pid":101, "old":None, "new":"PgSleep", "duration":0},
                {"kind":"FRONTIER", "t":10, "counters":[0]*15}]
        c["target"]["fork"] = 5
        c["query"].update(**{"from":6, "to":10})
    add("01", "invalid_provisional_assignment", invalid_provisional)
    def zero_exit(c):
        r = rec(c)
        r[:] = [r[0], dict(r[1], t=0),
                {"kind":"TRANS", "t":0, "pid":101, "old":None, "new":"$EXIT", "duration":0},
                {"kind":"FRONTIER", "t":5, "counters":[0]*15}]
    add("01", "zero_exit_without_life_exit", zero_exit)
    for kind in ("ATTACH_OK", "FRONTIER"):
        def bad_counter(c, kind=kind):
            record = (dict(rec(c)[1], t=20, counters=[0]*14) if kind == "ATTACH_OK"
                      else {"kind":"FRONTIER", "t":20, "counters":[0]*14})
            rec(c).extend([record, {"kind":"VOID", "t":30, "counters":[0]*15,
                           "reason":"later revocation", "invalid_from":0}])
        add("01", "malformed_" + kind.lower() + "_counters", bad_counter)
    def first_discovery(c):
        r = rec(c)
        r[:] = [r[0], dict(r[0], t=5), dict(r[1], t=6),
                {"kind":"TRANS", "t":7, "pid":101, "old":None, "new":"Lock", "duration":0},
                {"kind":"FRONTIER", "t":20, "counters":[0]*15}]
    add("06", "first_physical_discovery", first_discovery)
    def touching_consistent(c):
        r = rec(c)
        r[:] = r[:3] + [
            {"kind":"END", "t":8, "pid":101, "fork":0, "reason":"stop"},
            dict(r[1], t=8, seed="B", state="B"),
            {"kind":"TRANS", "t":8, "pid":101, "old":"PgSleep", "new":"B", "duration":5},
            {"kind":"FRONTIER", "t":10, "counters":[0]*15}]
        c["query"].update(**{"from":1, "to":8})
    add("01", "touching_consistent", touching_consistent)
    def invalid_life_end(c):
        rec(c)[-1:-1] = [
            {"kind":"LIFE", "t":0, "pid":202, "fork":None, "start":0, "start_known":False, "exit":None},
            {"kind":"LIFE", "t":1, "pid":202, "fork":None, "start":1, "start_known":False, "exit":None},
            {"kind":"END", "t":2, "pid":202, "fork":None, "reason":"stop"}]
    add("01", "invalid_LIFE_pending_END", invalid_life_end)
    for version in (0, 6):
        def invalid_version_boots(c, version=version):
            f = c["files"][0]
            f["version"] = version
            c["files"].append(dict(f, boot="second-boot"))
        add("01", f"invalid_version_{version}_boots", invalid_version_boots)
    add("22", "boot_conflicting_successor", lambda c: c["files"][1].update(boot="conflicting-boot"))
    def duplicate_life(c):
        f = c["files"][0]
        later = dict(rec(c)[0], t=40, exit=40)
        c["files"].append(dict(f, records=[copy.deepcopy(later) for _ in rec(c)]))
    add("01", "duplicate_descriptor_LIFE", duplicate_life)
    def unrelated_blocked_pid(c):
        r = rec(c)
        r[-1:-1] = [dict(r[0], pid=202), dict(r[1], pid=202),
                     {"kind":"TRANS", "t":5, "pid":202, "old":None, "new":"$EXIT", "duration":0}]
    add("01", "blocked_pid_is_local", unrelated_blocked_pid)
    # A schema-valid END earlier than the latest VOID is allowed. If it were
    # treated as a recovery attach it would cut before the fresh target LIFE.
    def end_before_void(c):
        r = rec(c)
        r.insert(7, {"kind":"END", "t":0, "pid":101, "fork":0, "reason":"stop"})
    add("64", "END_before_VOID_fresh_target", end_before_void)
    for unknown_successor in (False, True):
        def baseline_boundary(c, unknown_successor=unknown_successor):
            f = c["files"][0]
            f.update(qualified=False, loss_baseline=None, records=rec(c)[:1])
            c["files"].append(dict(f, file_no=1, previous=0,
                loss_baseline=None if unknown_successor else [0]*15,
                records=[dict(f["records"][0], t=20, exit=20)]))
        add("01", "descriptor_unknown_" + str(unknown_successor), baseline_boundary)
    def reverse_attach(c):
        r = rec(c)
        r[:] = [r[0], dict(r[1], t=8), r[1],
                {"kind":"END", "t":7, "pid":101, "fork":0, "reason":"stop"},
                {"kind":"FRONTIER", "t":15, "counters":[0]*15}]
        c["query"].update(**{"from":8, "to":15})
    add("01", "reverse_attach_time", reverse_attach)
    # Validate the remaining explicit refusal: no validated descriptor and two
    # possible boots. A later valid-looking descriptor cannot repair the gap.
    c = fixture("01")
    c["id"] = "fault_ambiguous_target_boot"
    first = c["files"][0]
    first["present"] = False
    c["files"].append(dict(first, present=True, boot="other-boot"))
    cases.append(c)
    return cases


def generate(fixture, count=1200, seed=0x868003):
    rng = random.Random(seed)
    templates = named(fixture)
    cases = []
    for i in range(count):
        c = copy.deepcopy(rng.choice(templates))
        c["id"] = f"fault_generated_{i}"
        # Change queries without changing any physical stop or dependency.
        left = rng.randrange(0, 24)
        c["query"].update(event=rng.choice(["PgSleep", "Lock", "absent"]),
                          **{"from": left, "to": left + rng.randrange(1, 24)})
        cases.append(c)
    return cases
