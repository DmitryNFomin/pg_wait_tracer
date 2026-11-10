# The #115 kernel-decomposition harness, copied into the repository

**Why this is here.** #194 (`tag-blocker`, "On-CPU time during the protocol tail
is credited to idle, not CPU") says to verify a fix against the kernel
decomposition rather than against another of our own tiers, because comparing
our tiers against each other only proves we are self-consistent. #795 is that
problem filed as a defect: the decomposition lived in two **untracked**
directories on one Mac, and #795's first remedy is "commit the results ...
under a tracked path, with a README". This directory is that remedy, done as the
last act of the handover.

The whole set is 180 KB of text: shell scripts, two `bpftrace` programs, one C
file, one Python script and the two evidence files.

## What is in it

| path | what it is |
|---|---|
| `issue115_arbiter/EVIDENCE.txt` | the unsaturated case, section `TRACED c2`: the kernel side (`schedstat` `sum_exec_runtime` and `run_delay`, with a `/proc/stat` cross-check) against our sampled and exact tiers |
| `issue115_arbiter/EVIDENCE_BIAS.txt` | the saturated case, section `sat` |
| `issue115_arbiter/arb_capture.sh`, `arb_analyze.sh`, `arb_control.sh`, `arb_ctl_analyze.sh`, `bias_capture.sh`, `bias_analyze.sh`, `sched2.bt` | the capture and analysis drivers that produced those two files, including the control arm |
| `issue115_oracle/capture*.sh`, `analyze*.sh`, `btwin.sh`, `frac.sh`, `run_offset*.sh` | the capture and analysis tooling of the sibling oracle set |
| `issue115_oracle/oracle_share.py`, `xp_gate.c`, `dbg.bt`, `offset.bt`, `offset2.bt` | the sampled-versus-exact share calculation, the gate's source, and the clock-offset probes |

## What is NOT here, and where it is. Read this before planning a re-run.

The drivers call files that lived on the capture box under `/root/arb/` and
`/root/xp/`: `kernel_cpu_arbiter.py` (the kernel sampler itself, the thing that
reads `schedstat`), `sched.bt`, `sched2.bt.tmpl` and `sleep.sql`. None of them is
in this directory.

**An earlier draft of this file said they were lost everywhere. That was wrong,
and an adviser caught it before it was published.** I had searched the working
trees of three gate boxes and this Mac, and the git index -- never git history.
Three of the four are in pushed history:

```
$ git log --all --diff-filter=A --name-only --format="%h %ad" --date=short \
    -- '*kernel_cpu_arbiter*' '*sched.bt' '*sched2.bt.tmpl' '*sleep.sql'
32580371 2026-09-27
tests/kernel_cpu_arbiter.py
tests/results/issue115_reconcile/sched2.bt.tmpl
tests/sched.bt
tests/test_kernel_cpu_arbiter.py
...
$ git branch -r --contains 32580371
  origin/preserve/observer-bias-study
```

So, verified rather than assumed:

| file | where it is |
|---|---|
| `kernel_cpu_arbiter.py` | four distinct versions, all committed 2026-09-27, all on pushed branches: blob `af13eb83` (14074 B) at `3c6ccb37`, `58b9f25d` (17736 B) at `080e8326` -- both on `origin/preserve/cpu-oracle-arbiter`; `2440a2b9` (20848 B) at `f876e16c` (`origin/preserve/pgws-bias-measurement`) and at `b540efa2` (`origin/preserve/exact-vs-kernel-gap`); `03773177` (23490 B) at `32580371` (`origin/preserve/observer-bias-study`) |
| `sched.bt` | `32580371:tests/sched.bt` |
| `sched2.bt.tmpl` | `32580371:tests/results/issue115_reconcile/sched2.bt.tmpl` |
| a unit test for the sampler | `32580371:tests/test_kernel_cpu_arbiter.py` |
| `sleep.sql` | **in no commit** (`git log --all -- '*sleep.sql'` is empty). It costs nothing: it appears once in the whole harness, in a comment in `capture2.sh` giving an example value for the optional `PGBARGS` variable |

Restore one with, for example:

```
$ git show 32580371:tests/kernel_cpu_arbiter.py > kernel_cpu_arbiter.py
```

**Which of those four sampler versions produced the evidence files here is
UNVERIFIED.** The timestamps permit a reading and no more: `EVIDENCE.txt`'s mtime
is 09:42 and `af13eb83` was committed at 09:43:06; `EVIDENCE_BIAS.txt`'s is 10:01
and `58b9f25d` was committed at 10:02:18, all on 2026-09-27 local. That is
suggestive, not proof, and nothing here records which one was actually run.

**What IS unique to this directory.** The two evidence files and the drivers
beside them are in no other commit, which is why this copy exists:

```
$ git log --all --format="%h" --name-only -- '*arb_capture.sh' '*EVIDENCE_BIAS.txt'
(only f5ad1796, this commit)
```

**Net position.** A new aligned capture needs `kernel_cpu_arbiter.py` restored
from a `preserve/*` ref above, and a choice of version that nothing in the
evidence settles. The oracle half retains some kernel collection of its own --
`capture2.sh` takes `schedstat` snapshots and computes deltas, and `sched2.bt`
carries scheduler accounting with hardcoded pids -- so "no kernel-side capture
can be produced" would also be too strong. What is genuinely unresolved is
reproducing *this* aligned pipeline with provenance.

The sampler's interface, from its call sites here: subcommands `sample`
(`--postmaster --out --hz --duration --extra-pid`), `report` (`--kernel --from
--to --pids`), `sched` (`--bt`), `sampbias` and `torn` (`--oracle --from --to`).
Those constrain a replacement; they do not specify its accounting semantics, so a
reconstruction would need validating against the restored original rather than
trusted.

## The directory #194 actually names

#194's body says the harness is in `tests/results/issue115_xtab/`. That
directory is **not** here and is not on this Mac -- but it is in pushed history,
ten files including an 80 KB `EVIDENCE.txt`:

```
$ git ls-tree -r --name-only 32580371 -- tests/results/issue115_xtab | head -3
tests/results/issue115_xtab/EVIDENCE.txt
tests/results/issue115_xtab/RUNLOG.txt
tests/results/issue115_xtab/SUMMARY.txt
```

It is reachable from `origin/preserve/observer-bias-study` (`32580371`) and
`origin/preserve/exact-vs-kernel-gap` (`b540efa2`). So #795's reading that the
harness "was evidently renamed and the issue never followed" is half right: the
path was never wrong about *a* directory, it is wrong about *this* one. Whoever
works #194 should read both.

## Provenance and limits

- These files are a **byte-identical copy** of `tests/results/issue115_arbiter/`
  and `tests/results/issue115_oracle/`, verified with `diff -r` at copy time on
  2026-11-10. Nothing was edited, reformatted or regenerated. The originals are
  left in place: this is a copy, not a move.
- **When:** the two evidence files' modification times on the outgoing lead's Mac
  are 2026-09-27 09:42 and 10:01 local. That is the only date evidence in the
  set; the files carry no capture timestamp of their own beyond the raw
  nanosecond window bounds in their `ALIGNMENT` lines.
- **Which box and which kernel: UNVERIFIED.** The files record no `uname`, no
  host and no PostgreSQL version. Captures of this kind were taken on the
  Hetzner gate boxes over ssh; the three that exist today report
  `6.8.0-139-generic`, `6.8.0-138-generic` and `6.8.0-139-generic`
  (read 2026-11-10), but whether the September capture box ran any of those is
  not recorded anywhere and must not be assumed.
- The scripts contain no host addresses and no credentials; that was checked
  before committing. Supply your own box.
- `EVIDENCE.txt` and `EVIDENCE_BIAS.txt` need `bpftrace` and root on a Linux box
  running PostgreSQL to reproduce. Never on macOS.

## What invalidates a capture (#795 item 3)

Treat these numbers as describing one box, one kernel and one workload, not as
constants:

- **A different kernel version.** The whole point of the decomposition is the
  kernel's own accounting (`schedstat` `sum_exec_runtime`/`run_delay`,
  `/proc/stat`). Scheduler accounting changes across versions, and the capture's
  own kernel is not recorded, so a comparison against a run on a different
  kernel is a comparison of two unknowns.
- **A different box class or CPU vendor.** The gate boxes are the same `cx33`
  class but not the same silicon, which is recorded as a known difference
  between them. The saturated case especially is a property of how many
  processors the work had.
- **A different PostgreSQL major version**, which moves which wait events are
  reported at all and therefore what the exact tier can see.
- **Any change to our own tiers' definitions** -- the idle/CPU boundary, the
  command-boundary straddle rule, the sampled share -- since the shares printed
  here were computed by the code of 2026-09-27, not by today's.

If any of those differs, the capture must be redone rather than trusted -- and
redoing it is blocked on the missing `kernel_cpu_arbiter.py` above.
