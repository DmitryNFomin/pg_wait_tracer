"""Physical file-chain perturbations; values come only from r3/reference/replay.py."""
import copy
import random


def split_capture(case, rng):
    """Rotate each independent capture, preserving physical record order."""
    c = copy.deepcopy(case)
    files = []
    for original in c["files"]:
        records = original["records"]
        cuts = sorted([0, len(records), rng.randrange(len(records)+1),
                       rng.randrange(len(records)+1)])
        baseline = copy.deepcopy(original.get("loss_baseline"))
        highest = None
        for no, (a, b) in enumerate(zip(cuts, cuts[1:])):
            f = dict(original, file_no=no, previous=no-1 if no else None,
                     loss_baseline=copy.deepcopy(baseline), records=records[a:b])
            files.append(f)
            for record in records[a:b]:
                kind = record.get("kind")
                vector = record.get("counters")
                if kind == "VOID":
                    baseline = copy.deepcopy(vector)
                elif original.get("qualified") is True and kind in ("FRONTIER", "ATTACH_OK"):
                    if kind == "FRONTIER" and highest is not None and record["t"] <= highest:
                        break  # This capture already stops at this record.
                    readable = (isinstance(vector, list) and len(vector) == 15
                                and all(type(v) is int and 0 <= v < 1 << 64 for v in vector))
                    if vector is not None and not readable:
                        break  # Malformed records cannot supply a baseline.
                    if baseline is None or vector != baseline:
                        if (readable and baseline is not None
                                and any(a < b for a, b in zip(vector, baseline))):
                            vector = None
                        baseline = copy.deepcopy(vector)
                    elif kind == "FRONTIER":
                        highest = record["t"]
    c["files"] = files
    return c


def perturb(case, rng, mode):
    c = split_capture(case, rng)
    files = c["files"]
    # Each original descriptor produces exactly three files. Touch only the
    # first capture; independent later captures must still be replayed.
    if mode == 1:
        files[1]["present"] = False
    elif mode == 2:
        files[1], files[2] = files[2], files[1]
    elif mode == 3:
        files.insert(1, copy.deepcopy(files[0]))
    elif mode == 4:
        del files[1]
    elif mode == 5:
        files[1]["previous"] = 77
    elif mode == 6:
        del files[0]
    elif mode == 7:
        files[1]["version"] = 6
    elif mode == 8:
        # Qualification stays capture-local; a changed successor baseline stops
        # at the descriptor rather than silently resetting loss history.
        files[1]["qualified"] = not files[0]["qualified"]
        files[1]["loss_baseline"] = [7]*15
    return c


def extend(cases, seed):
    rng = random.Random(seed ^ 0xF11E)
    result = []
    for i, case in enumerate(cases):
        c = perturb(case, rng, i % 9)
        c["id"] = "file_" + case["id"]
        result.append(c)
    return result


def named(fixture):
    cases = [fixture("22"), fixture("23")]
    def add(base, label, edit):
        c = fixture(base)
        c["id"] = "chain_" + label
        edit(c)
        cases.append(c)
    add("22", "wrong_previous", lambda c: c["files"][1].update(previous=99))
    add("22", "absent_previous", lambda c: c["files"][1].update(previous=None))
    add("22", "skipped_ordinal", lambda c: c["files"][1].update(file_no=2))
    add("22", "suffix_only", lambda c: c["files"].pop(0))
    add("22", "out_of_order", lambda c: c["files"].reverse())
    add("22", "duplicate", lambda c: c["files"].insert(1, copy.deepcopy(c["files"][0])))
    add("22", "future_version", lambda c: c["files"][1].update(version=6))
    add("22", "legacy_successor", lambda c: c["files"][1].update(version=4))
    add("22", "initial_legacy", lambda c: c["files"][0].update(version=4))
    add("22", "legacy_no_salvage", lambda c: [c["files"][0].update(version=4),
        c["files"][1].update(records=[{"kind":"SAMPLE", "t":99}])])
    add("22", "initial_version_zero", lambda c: c["files"][0].update(version=0))
    add("22", "version_zero", lambda c: c["files"][1].update(version=0))
    add("22", "successor_gates_ignored", lambda c: c["files"][1].update(
        qualified=False, loss_baseline=[9]*15))
    add("22", "unqualified_initial", lambda c: c["files"][0].update(qualified=False))
    add("23", "gap_inside_window", lambda c: c["query"].update(**{"from":4,"to":16}))
    add("23", "window_before_gap", lambda c: c["query"].update(**{"from":3,"to":8}))
    add("23", "window_after_gap", lambda c: c["query"].update(**{"from":12,"to":20}))
    add("23", "gap_no_salvage", lambda c: c["files"][2].update(file_no=1, previous=0))
    # VOID baseline and frontier high-water both survive rotation.
    for prefix in ("12", "13", "26", "31", "24", "32", "25"):
        c = split_capture(fixture(prefix), random.Random(int(prefix)))
        c["id"] = "chain_rotated_" + prefix
        cases.append(c)
    c = split_capture(fixture("24"), random.Random(24))
    c["id"] = "chain_restart_interleaved"
    c["files"] = [c["files"][i] for i in (0,3,1,4,2,5)]
    cases.append(c)
    c = split_capture(fixture("24"), random.Random(24))
    c["id"] = "chain_restart_after_gap"
    c["files"][1]["present"] = False
    cases.append(c)
    add("23", "malformed_after_gap", lambda c: c["files"][2].update(
        records=[{"kind":"UNKNOWN", "t":99}]))
    return cases


def generate(fixture, count=800, seed=0x868006):
    rng = random.Random(seed)
    cases = []
    for i in range(count):
        base = fixture(rng.choice(["22", "23", "24", "12", "31", "25", "32"]))
        # Start from independent unrotated descriptors before splitting.
        captures = {}
        for f in base["files"]:
            if f["capture"] not in captures:
                captures[f["capture"]] = dict(f, file_no=0, previous=None, records=[])
            if f.get("present", True):
                captures[f["capture"]]["records"].extend(f["records"])
        base["files"] = list(captures.values())
        # Duplicate a whole file with a frontier: r3 stops at the repeated
        # descriptor, preserving its earlier prefix.
        if i % 9 == 3:
            c = copy.deepcopy(base)
            c["files"].insert(1, copy.deepcopy(c["files"][0]))
        else:
            c = perturb(base, rng, i % 9)
        c["id"] = f"chain_generated_{i}"
        cases.append(c)
    return cases
