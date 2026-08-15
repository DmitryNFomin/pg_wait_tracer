# Sampled-overhead regression gate

This is the permanent Stage 5 guard for the sampled tier. It is intentionally
two-layered: noisy timing catches catastrophic performance changes, while live
and synthetic structural assertions catch their mechanisms without depending
on timing.

## Root cause and test gap

The historical sampled path attached per-query query-id and activity uprobes.
Their trap cost ran in PostgreSQL backends and reduced throughput by roughly
20–30%, while the old overhead test measured only `--mode full` with ordinary
pgbench. A later unthrottled `pg_stat_statements(true)` resolver regression cost
about 28% only under many distinct query shapes, which ordinary pgbench also
hid. The permanent gate therefore tests sampled versus no tracer and includes a
256-shape workload.

## Noise characterization (2026-08-15)

Each number below is a paired TPS loss. Pairs alternate baseline→sampled and
sampled→baseline; the statistic is the paired median.

| Host / workload | Pairs | Median | IQR | Observed range |
|---|---:|---:|---:|---:|
| Rocky 8.10 dedicated 4-core box, RO | 9 | +0.63% | 1.44pp | -2.04% … +3.97% |
| Rocky 8.10 dedicated 4-core box, RW | 9 | +0.24% | 1.49pp | -2.31% … +2.96% |
| Rocky 8.10 dedicated 4-core box, 256 shapes | 9 | -0.36% | 2.03pp | -2.30% … +7.14% |
| GitHub `ubuntu-latest` 4-vCPU runner, RO | 7 | +1.99% | 11.02pp | -12.75% … +19.38% |
| GitHub `ubuntu-latest` 4-vCPU runner, RW | 7 | +3.47% | 9.84pp | -7.77% … +7.11% |
| GitHub `ubuntu-latest` 4-vCPU runner, 256 shapes | 7 | +3.25% | 5.78pp | -3.74% … +11.56% |

On the dedicated box the prefix median was already useful at seven pairs; the
largest seven-to-nine-pair movement was 1.33pp. Nine pairs are retained for the
manual precision profile. On the hosted runner, prefix medians through seven
pairs moved by as much as 3.74pp and one individual RO pair exceeded 15%.
Consequently a seven-pair candidate failure is never accepted as final: the
gate adds seven pairs and hard-fails only if the combined, exactly AB/BA-balanced
14-pair median remains at or above 15%. Normal PRs pay only for seven pairs.

The allocated hosted runner had four vCPUs, so no claim is made about a 2-vCPU
runner. Shared-runner noise is already too large for a credible sub-1% blocking
test even at four vCPUs.

## Permanent design

- Workloads: standard pgbench read-only, standard read-write, and 256 genuinely
  distinct high-cardinality point-query shapes. At least 231 shapes must be
  observed.
- Statistic: median of seven alternating paired A/B losses. A candidate 15%
  hard failure automatically expands to a balanced 14-pair median.
- Thresholds: 10% emits a nonblocking GitHub warning; 15% hard-fails. The worst
  hosted null median was 3.47%, leaving 11.53pp of headroom. A multiplicative
  20% regression applied to the observed null medians lands at 21.6–22.8%; a
  28% regression lands at 29.4–30.5%.
- Placement: every pull request runs the coarse GitHub-hosted blocking job.
  The dedicated-box command below is the manual precision profile for the
  sampled tier's sub-1% target; it reports rather than pretending that 1% is a
  reliable hard boundary.
- Safety: the harness cross-checks SQL port/version, `data_directory`, and
  `postmaster.pid`; refuses to replace an existing database; scopes
  `pg_stat_statements_reset` to its run-owned database; and force-drops only
  that unique database.

Every sampled timing run also requires a validated layout, the requested
postmaster PID, pure sampled mode/tier, a healthy `pgbackend_status` sampler,
positive samples, and zero attached/fired exact query/activity uprobes. The
high-cardinality run additionally requires actual resolver scans and resolved
keys, and bounds actual `pg_stat_statements(true)` scans to at most one per
second plus one edge allowance. Synthetic tests pin zero sampled probe links
for validated PG13–18, scheduler throttling/fairness, and PG13 persistence in
the async worker rather than the sampler tick.

## Reproduction

The CI command is explicit in `.github/workflows/ci.yml`. For a quieter manual
profile on the dedicated box, use longer samples and keep timing informational:

```bash
sudo env PATH=/usr/pgsql-17/bin:$PATH \
  BPFTOOL=/root/pgwt/build/bpftool/src/bpftool \
  python3 tests/sampled_overhead_gate.py \
    --pid "$(head -1 /var/lib/pgsql/17/data/postmaster.pid)" \
    --pg-version 17 --port 5417 --reps 9 --confirm-reps 0 \
    --duration 30 --warmup 2 --clients 4 --jobs 4 --scale 10 \
    --high-cardinality-shapes 256 --min-distinct-shapes 231 \
    --characterize --output-json sampled-overhead-box.json
```

`PGWT_TEST_SAMPLED_UPROBES=1` and `PGWT_TEST_PGSS_UNTHROTTLED=1` are loud,
test-only fault-injection hooks used to prove both the timing and structural
halves of the gate go red. They are not production options.
