## 1. Verdict on the current sampling section

**Build the argument around missing information, not inaccurate profiles.** Your strongest answer to “the profile is right on average” is: **“Yes. Now tell me whether this execution suffered one long wait or 4,000 short ones.”**

The current section mixes three different problems:

- Information that state sampling cannot reconstruct reliably.
- Information absent from `wait_event_info`, even when traced.
- A suspected implementation or scheduling bias that your evidence does not establish for standard tools.

Keep the first. Explain the second separately. Remove the third from the main talk unless the new experiments produce convincing evidence.

### `rate`: correct arithmetic, weak argument

The arithmetic checks out:

| Quantity | Calculation | Result |
|---|---:|---:|
| Backend observations at nominal 100 Hz | 8 × 100 | 800/s |
| TPC-B transitions per observation | 40,000 ÷ 800 | 50 |
| SELECT-only transitions per observation | 220,000 ÷ 800 | 275 |
| TPC-B transitions per observation at 1 Hz | 40,000 ÷ 8 | 5,000 |
| SELECT-only equivalent at 1 Hz | 220,000 ÷ 8 | 27,500 |

The default periods are indeed 10 ms in pg_wait_sampling 1.1.11. These are **configured periods**, not guaranteed achieved rates. [Versioned README](https://github.com/postgrespro/pg_wait_sampling/blob/v1.1.11/README.md)

Problems:

- **A transition-to-sample ratio says nothing by itself about time-share accuracy.** An expert can dismiss this slide immediately: sampling does not need to observe every transition to estimate occupancy.
- **Transitions are not waits.** A normal enter/exit pair produces two transitions. Do not turn “40,000 changes” into “40,000 waits.”
- The supplied README reports 40K/220K, but the supplied sweep and cross-validation files do not independently establish those transition rates. Treat them as benchmark summaries awaiting the underlying counters.
- The rates come from the **8-vCPU cx43 benchmark**, whereas the proposed experiments use a **4-vCPU box**, a different scale, and different conditions. New measurements will characterize the new setup, not reproduce those exact numbers.
- Clarify whether the counts include only the eight client backends or background processes too.

**Recommendation:** cut this standalone slide. Keep the transition rate for the overhead explanation, where it directly matters.

### `odds`: useful, but state the probability model

For strictly periodic instantaneous observations, interval \(\Delta\), and an isolated wait with uniformly random phase relative to the observation grid:

\[
P(\text{at least one observation})=\min(1,d/\Delta).
\]

The corrected table is:

| Wait duration | At 100 Hz: \(\Delta=10\) ms | At 1 Hz: \(\Delta=1,000\) ms |
|---|---:|---:|
| 0.1 ms | 1% | 0.01% |
| 1 ms | 10% | 0.1% |
| 5 ms | 50% | 0.5% |
| 20 ms | 100% | 2% |

Corrections:

- The footer needs the `min(1, …)` cap.
- “Polling once a second: divide by 100” works for the first three rows, **not for the displayed, capped 100% in the last row**.
- Under ideal periodic sampling, a 20 ms wait yields two observations, apart from endpoint conventions. Actual scheduling gaps remove that guarantee.
- “A sample lands at a random moment” is imprecise. These tools usually sample periodically; the **wait’s phase** is assumed random.
- For Poisson observation times, the formula is instead \(1-e^{-d/\Delta}\). A 5 ms wait at a mean 100 Hz has a 39.3% detection probability. Do not interchange the models.
- Repeated waits can be correlated with the sampling clock. “Thousands of waits” does not automatically guarantee convergence in one run.

The line “a sample never says how long the wait was” also needs narrowing. A plain PostgreSQL wait-state observation does not contain an event duration, but Oracle ASH has `SEQ#`, `TIME_WAITED`, execution identifiers, and cumulative-delta fields. Its captured waits still do not constitute a complete event population. [Oracle ASH reference](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/V-ACTIVE_SESSION_HISTORY.html)

**Recommendation:** keep, shortened. Say **“A state sample is not a complete wait record.”**

### `sowhat`: good questions, overclaimed consequences

| Current argument | Review |
|---|---|
| “One slow wait, or many short ones?” | Strong. Keep. |
| “Wrong fix, wrong team: storage or your index” | Unsupported causal leap. Wait duration and count do not establish that division of responsibility. |
| “The tail is invisible” | Wrong as a general statement. Long waits are more likely to be sampled. What is missing is a complete event-duration distribution and reliable reconstruction of a particular execution. |
| “No proof, no controlled experiment” | Wrong. Sampling can support excellent controlled experiments about time shares and bottlenecks. |
| “CPU, or waiting for a core?” | Real distinction, but not fundamentally a sampling limitation. A trace of the same four bytes cannot distinguish those states either. |

Replace the third question’s consequence with:

> “The total changed; the mechanism is still unknown.”

For CPU versus run queue, explain that **additional scheduler instrumentation** provides the distinction. Even “off CPU while no PostgreSQL wait is registered” is not necessarily runnable time: it can include uninstrumented blocking. Do not label a residual as run-queue delay without validating its scheduler state.

### `same`: strongest concept, flawed example and interpretation

The totals are correct:

\[
1\times800\text{ ms}=40\times20\text{ ms}=800\text{ ms}.
\]

Over one second, both have 80% wait occupancy.

However:

- The illustration’s HTML description says observations are **100 ms apart: 10 Hz**, following slides discussing 100 Hz.
- Forty 20 ms waits are individually long enough to be observed by an ideal 100 Hz sampler. This weakens the short-event argument.
- The two histories have the same **aggregate share**, but can have distinguishable sampled histories.
- If the forty waits alternate regularly with 5 ms gaps, their period is 25 ms. Observing every 100 ms hits the same phase repeatedly: the result can be 0% or 100%, not necessarily 80%.
- “800 ms means storage; forty reads means a bloated index” is not defensible. Both patterns can arise from several causes. `DataFileRead` elapsed time is not a direct device-service-time measurement.

The embedded image itself is unavailable as a local asset; these checks cover its HTML description and speaker notes.

**Recommendation:** keep the concept, redraw it explicitly as an **illustration of equal aggregate occupancy**. Use, for example, one 800 ms wait versus 4,000 waits of 0.2 ms. Label them “different event histories,” without assigning a root cause.

### `sched`: remove from the main talk

There is a plausible hypothesis here, but the slide presents it as a universal result.

- “Every sampler is a process” is false broadly: interrupt-driven and kernel-based sampling exist.
- A userspace collector can run late, but **lateness does not imply a particular bias direction**.
- The scheduler can preempt a running backend; it does not have to wait for that backend to block.
- An external sampler may execute on another CPU.
- State-dependent observation timing can bias a profile, but you must demonstrate that dependence.
- “The timer fires on time” is also too absolute, particularly on a shared VM.
- The supplied measurements concern **pg_wait_tracer’s sampler**, not pg_wait_sampling, pgsentinel, or AWS’s collector.

The supplied post-fix numbers are:

| Requested rate | Exact CPU-labelled share | Sampled share | Sampled/exact CPU-labelled time | Sampled/exact total DB time |
|---|---:|---:|---:|---:|
| 10 Hz | 38.5% | 31.3% | 0.768 | 0.946 |
| 50 Hz | 36.7% | 31.3% | 0.826 | 0.970 |
| 100 Hz | 40.9% | 34.4% | 0.795 | 0.946 |
| 200 Hz | 36.6% | 32.7% | 0.881 | 0.987 |

Thus:

- **77–88% is a ratio of accumulated CPU-labelled nanoseconds**, not a CPU-share ratio and not measured physical CPU execution time.
- CPU-share disagreement is **3.9–7.2 percentage points**.
- Total DB-time/AAS under-read is approximately **1.3–5.4%**, not a tenth to a quarter.
- Non-CPU time is approximately **4.9–5.7% higher** in the sampled result.
- The four results are not monotonic with requested frequency.
- `PASS` means passing a **±10 percentage-point tolerance**.
- Top-five **overlap** is not, by itself, proof of matching order or accurate magnitudes.

The README’s opening “within ~1 percentage point” statement contradicts its later results. Its recommendation to treat sampled CPU as a lower bound is also stronger than the evidence: an empirical tendency is not a bound.

**Recommendation:** retain this investigation in backup material. Title it “An observed discrepancy; cause under investigation.”

### Two claims elsewhere that affect the sampling argument

**“Exact” needs a defined scope.** Say “records registered wait transitions during an attached tracing window,” conditional on successful attachment, absence of loss/throttling, correct decoding, and measured instrumentation effects. It does not discover uninstrumented waits or retroactively recover events before escalation.

**The storage case needs its underlying trace.** Nine × 120 ms = 1.08 s, so the arithmetic works. But a one-second timeout may interrupt the ninth wait; show complete versus interrupted intervals and correlate the actual cancellation. “The trace said storage” requires more than observing long `DataFileRead` intervals.

## 2. The strongest arguments, ranked

### 1. An accurate time share does not identify event count or duration distribution

**Slide claim:** “The profile can be correct while the number and duration of waits remain unknown.”

**Why:** for an event type,

\[
T_{\text{wait}}=\sum_{i=1}^{N}D_i=N\bar D.
\]

Occupancy estimates \(T_{\text{wait}}/T\). It does not separately identify \(N\), \(\bar D\), or the duration distribution. Many event histories produce the same occupancy; aggregation loses their order too.

**Most damaging objection:** “I have `pg_stat_io`, `pg_stat_statements`, `EXPLAIN`, and other counters.”

**Answer:** use them. Existing counters can answer many count-and-total questions more cheaply. The case for tracing is the remaining question: **per-execution wait episodes, their distribution, and their sequence**, especially where suitable counters or attribution do not exist. Do not claim every read or lock-acquisition count requires this tool.

### 2. A particular execution can lose the evidence needed to explain it

**Slide claim:** “At 100 Hz, an isolated 1 ms wait has a 90% chance of leaving no state sample.”

**Why:** under the periodic/random-phase assumptions, detection probability is \(d/\Delta\) for \(d<\Delta\). Both entry and exit can occur between observations. More observations of other executions cannot reconstruct the missing history of this execution.

For example, an isolated 5 ms lock wait can matter greatly to a request normally taking 0.5 ms, despite contributing little to a cluster-wide profile.

**Most damaging objection:** “If it matters operationally, it repeats; the aggregate will reveal it.”

**Answer:** often true for identifying the dominant bottleneck. It does not establish whether **this execution** waited, how long, or what happened immediately before it failed. Long timeouts are often visible in samples; the missing information is their detailed decomposition.

**Limit:** exact tracing must already be active. An anomaly-triggered escalation may capture subsequent occurrences and miss the first one.

### 3. Time-weighted observations are not an event-weighted latency population

**Slide claim:** “A wait-share chart cannot give you the p99 of individual waits.”

**Why:** sampling at a random time preferentially encounters longer events. For event duration density \(f(d)\), the duration distribution encountered at random occupied instants is

\[
g(d)=\frac{d f(d)}{E[D]}.
\]

Its mean is \(E[D^2]/E[D]\), not \(E[D]\). For distinct events captured at least once by periodic sampling, inclusion probability is \(\min(1,d/\Delta)\), again unequal across durations.

This weighting is **appropriate for time shares**. It becomes misleading when interpreted as event counts or ordinary event-latency percentiles.

**Most damaging objection:** “Sampling can estimate percentiles.”

**Answer:** absolutely. Randomly selecting completed events and recording their full durations can estimate percentiles. That is different from periodically reading a backend’s current state. Likewise, built-in latency histograms may eliminate the need for full traces.

### 4. Temporal order matters when investigating a mechanism

**Slide claim:** “The same wait profile can describe different execution paths.”

**Why:** a profile is invariant under rearranging intervals. State histories preserve more information, but transitions inside each sampling gap remain ambiguous. Several adjacent samples with the same event cannot prove that one continuous wait occurred.

This matters when distinguishing repeated reacquisition, a single stalled operation, or a sequence of different waits surrounding a latency spike.

**Most damaging objection:** “A timestamped sequence still does not prove causation.”

**Answer:** correct. A trace establishes observed ordering and durations. Identifying the blocker, relation, buffer, syscall, or device may require additional instrumentation. The four-byte field alone does not provide those identities.

### 5. Periodic sampling can alias; collector delays can invalidate assumptions

**Slide claim:** “A configured sampling frequency is not a guarantee of representative observations.”

**Why:** suppose a process waits for 1 ms every 10 ms. A phase-locked 100 Hz sampler can repeatedly observe either the wait or the non-wait state, despite true occupancy of 10%. Randomization and phase drift can improve this.

Separately, observation times correlated with workload state can bias occupancy estimates. Delays alone do not establish the sign.

**Most damaging objection:** “Real scheduling adds jitter; this is a contrived example.”

**Answer:** that limits its practical importance. Present aliasing as a failure mode to test, not an explanation for your existing discrepancy. A randomized sampler can estimate occupancy well while still lacking individual event records.

**Ranking decision:** arguments 1–3 belong in the talk. Argument 4 supports the timeline demonstration. Argument 5 belongs in backup unless independently measured.

## 3. Verified situations and sources

**I did not verify a published PostgreSQL incident showing that default 100 Hz pg_wait_sampling systematically missed a sustained, dominant bottleneck that your tracer then found.** Do not imply that such evidence exists. The sources below support narrower claims, with their limits stated.

### Direct investigations and demonstrations

| Source | One-line summary | Exactly what it supports |
|---|---|---|
| [Piotr Sajda: TEMP on Flash, 2024](https://piotrsajda.com/oracle-blog/temp-tablespace-on-flash/) | ASH identified TEMP I/O as the expensive area; a duration average derived from ASH was about **19.305 ms**, whereas a targeted 10046 trace measured about **8.999 ms**. | Strong practical example of **correct broad attribution, insufficient event-duration statistics**. The public example reproduces an underlying production problem. The trace covered a selected execution phase, not the identical ASH window, so it is not a controlled demonstration that ASH always overestimates by 2×. |
| [Egor Rogov: PostgreSQL 14 Internals, §15.5, pp. 244–247](https://edu.postgrespro.com/postgresql_internals-14_en.pdf#page=245) | A pgbench demonstration compares normal operation with deliberately slowed filesystem operations; the author discusses short `WALWrite` LWLock waits receiving few or no samples. | Direct PostgreSQL demonstration that **absence or scarcity of samples does not establish absence of short waits**. It is a controlled example, not a production postmortem. The displayed profile is top-four output, so do not independently infer an exact zero count from that table. |
| [Brendan Gregg: Subsecond Offset Heat Maps](https://www.brendangregg.com/HeatMaps/subsecondoffset.html) | Coarse process monitoring missed short-lived `zoneadmd` activity associated with Riak stalls; 99 Hz visualization and DTrace led to an address-space-lock diagnosis and kernel changes. | A genuine production example where **coarse observations concealed the relevant actor and timing**. Crucially, higher-frequency sampling helped find it; this is not evidence that sampling at approximately 100 Hz failed. |
| [pgsql-general: Excessive and slow `fsync()` within one transaction, 2009](https://www.postgresql.org/message-id/51549ea20912081628u2b5450beo59fb4f549ac85d09@mail.gmail.com) | DTrace exposed thousands of mostly fast fsync calls interspersed with calls lasting approximately **22, 22, and 61 seconds**. | Supports the value of **individual operation counts and latency distributions** when bulk storage throughput looks healthy. It does not document an ASH failure; these long stalls would be easy to sample if correctly instrumented. |

The Sajda case is the strongest external illustration for your central argument: **the sampled profile was useful, and tracing answered the next question.**

### PostgreSQL discussions and product limitations

| Source | One-line summary | Exactly what it supports |
|---|---|---|
| [Drouvot/Haas: Adding wait-event statistics, July 2025](https://www.postgresql.org/message-id/aIIeX7p2cKUO6KTa@ip-10-97-1-34.eu-west-3.compute.internal) | Drouvot asks whether a wait increased through frequency or duration; Haas challenges the value and overhead of generic timing and requests more specific instrumentation. | This is almost your anticipated conference exchange. It supports **choosing a concrete unanswered question**, while warning that even a complete wait trace lacks acquisition counts, object identities, and other useful context. It is a design discussion, not a solved incident. |
| [PostgreSQL’s 2019 statement-logging change and reversion](https://www.postgresql.org/message-id/20190811025735.GA24786@fetter.org) | A statement-sampling feature was reverted because even the longest statements could escape logging; the design needed a separate threshold. | A concrete PostgreSQL engineering decision that **statistical coverage and guaranteed outlier retention serve different purposes**. This is statement selection, not periodic wait-state sampling. |
| [PGConf.dev 2025 developer unconference notes](https://wiki.postgresql.org/wiki/PGConf.dev_2025_Developer_Unconference#The_Future_of_pg_stat_statements) | Developers explicitly discuss missing cancelled-query statistics and the desire for latency percentiles. | Supports the operational need to retain failure evidence. **It is not an argument against wait sampling:** `pg_stat_statements` is completion-oriented accounting, and a running query may be sampled before cancellation. |
| [AWS: Database load](https://docs.aws.amazon.com/AmazonRDS/latest/UserGuide/USER_PerfInsights.Overview.ActiveSessions.html) | AWS documents one-second session sampling for AAS, alongside separately calculated execution-time metrics. | Establishes the actual observation model. A particular short execution can fall between observations, but it is wrong to describe the entire service as consisting only of sampled wait shares. |
| [AWS: `MetricQuery`](https://docs.aws.amazon.com/performance-insights/latest/APIReference/API_MetricQuery.html) | Above an internal active-session threshold, AWS samples sessions and scales `db.load.avg`; `db.sampledload.avg` exposes the raw sampled load. | Establishes another source of statistical estimation. It does **not** establish CPU under-counting or a broken load estimate. |
| [Microsoft: PostgreSQL troubleshooting-guide limitations](https://learn.microsoft.com/en-us/azure/postgresql/troubleshoot/concepts-troubleshooting-guides#limitations) | Microsoft explicitly documents that brief session/wait spikes can leave no sampled record. | Vendor confirmation of the **short-incident coverage limitation**, not proof of any particular missed incident. |
| [David Kurtz: Obtaining database trace files, 2022](https://blog.go-faster.co.uk/2022/09/obtaining-database-trace-files.html) | Kurtz starts with ASH but describes using SQL trace when short-lived behavior produces too few samples. | Experienced practitioner support for **sample first, trace selectively**. This article is methodological, not a quantified case study. |

### Counterevidence you should acknowledge

[Indrek Ots’s Aurora LockManager experiment](https://blog.indrek.io/articles/dealing-with-lwlock-lockmanager-wait-events-in-aurora-postgres/) clearly detects `LWLock:LockManager` in Performance Insights and shows it disappearing after reducing relation-lock demand.

This directly undermines “short LWLocks are invisible to sampling.” Individual episodes may be short while their **combined occupancy is large and readily measurable**.

[Gregg’s off-CPU analysis](https://www.brendangregg.com/offcpuanalysis.html) discusses both sampling and tracing, including tracing overhead at high event rates. Use it to justify choosing the appropriate measurement, not to imply tracing is universally superior.

[Gil Tene’s coordinated-omission discussion](https://groups.google.com/g/mechanical-sympathy/c/WnOTco6ek4I) concerns observation schedules becoming coupled to the system under test. It is relevant to your load generator and to certain polling designs, but **“the collector ran late” is not sufficient evidence of coordinated omission or directional bias**.

**Unverified lead:** Martin Berger’s “Invisible IOs” article surfaced in search, but direct opening returned HTTP 429. I would not use it as verified evidence. Its indexed excerpt also contains a correction from 40% to 4%, making it particularly unsuitable for an unchecked slide claim.

## 4. Review of `TEST_PLAN_DRAFT.md`

**The plan currently spends most of its effort testing the weakest slide.** Retain the collector investigation, but prioritize demonstrating useful information that a correct sampled profile lacks.

### Fix the collector assumptions first

For **pg_wait_sampling v1.1.11**, the source establishes:

- `probe_waits()` takes **one timestamp before acquiring `ProcArrayLock`**.
- It scans `PGPROC` entries under that shared lock.
- Each retained row receives that same timestamp.
- Wait values and query IDs are read separately; the lock does not freeze backend wait transitions.
- Overdue rounds are deliberately skipped rather than replayed.
- History/profile requests are serviced by the collector itself.
- `sample_cpu` defaults to true.
- The sampling predicate does not apply your tracer’s `cmd_open` predicate.

Therefore, “reads PGPROC without locks” is inaccurate for this version. The correct concern is **lack of a simultaneous, coherent snapshot of changing wait and attribution fields**. Check the installed package source for downstream differences. [Collector](https://github.com/postgrespro/pg_wait_sampling/blob/v1.1.11/collector.c), [sampling predicate](https://github.com/postgrespro/pg_wait_sampling/blob/v1.1.11/pg_wait_sampling.c)

### Common controls

1. **Record the actual machine conditions.** Four vCPUs do not establish four independent physical cores. Capture guest topology, visible SMT relationships, affinity, CPU quota/throttling, steal time, migrations, governor/frequency information where available, and competing work. A guest cannot fully control its cloud host.

2. **Measure actual saturation.** `pgbench -c 4` does not imply CPU saturation. Include a CPU-only load control and record run-queue delay, CPU usage, and I/O activity. Run the client elsewhere where feasible.

3. **Separate cache regimes.** Scale 10 is roughly 128 MiB of account heap before indexes and other relations; “~150 MB total” needs actual relation sizes. Uniform versus skewed access changes the working set. Record physical reads and PostgreSQL buffer misses rather than inferring them from database size.

4. **Use repeated, randomized conditions.** Run at least five independent repetitions, preferably ten, with consistent warmup, table state, checkpoint policy, and autovacuum treatment. Report variability across runs; individual samples within a run are correlated.

5. **Measure observer interaction.** Include neither tool, pg_wait_sampling only, tracer only, and both. Report throughput and latency changes. Agreement while both observe an instrumented workload does not establish that tracing leaves the original workload unchanged.

6. **Validate the reference trace.** Record attachment coverage, failures, ring-buffer loss, throttling, backend lifecycle gaps, and incomplete boundary intervals. Distinguish watchpoint firings, actual value changes, and completed wait episodes.

7. **Define the population and denominator.** Your existing evidence includes checkpointer and autovacuum events. For client AAS, select the same client sessions and active intervals in both datasets. First compare ungated state occupancy; then separately validate command gating and query attribution.

8. **Treat clocks explicitly.** Map PostgreSQL timestamps to monotonic time using repeated paired readings, account for the PostgreSQL epoch, quantify offset uncertainty, and detect wall-clock adjustments. Nanosecond output precision is not nanosecond accuracy.

9. **Use latency measurements appropriate to the claim.** For response under an offered arrival rate, use scheduled arrivals and include schedule lag, failures, and skipped work. PostgreSQL documents that `pgbench -R` uses a Poisson schedule and measures latency from scheduled start times. Ordinary closed-loop throughput tests remain useful for capacity comparisons. [pgbench documentation](https://www.postgresql.org/docs/18/pgbench.html)

### E1 — Bare timer lateness

**Verdict:** valid control for userspace scheduling delay; insufficient evidence for biased wait shares.

Changes:

- Record deadline, actual wake time, and whether intervening deadlines were missed.
- Define catch-up behavior. An absolute-deadline loop can issue several immediate iterations after a stall and still report roughly 100 Hz over the whole run.
- Use preallocated memory and write results after measurement.
- Include CPU-only saturation, and an exploratory condition with the timer assigned a less-contended CPU.
- At 100 Hz for 60 seconds there are only 6,000 deadlines. A p99.9 estimate rests on approximately six upper-tail observations; repeat runs before emphasizing it.

**Metrics:** lateness quantiles, maximum inter-observation gap, percentage over 1 ms and 10 ms, missed deadlines, catch-up iterations, achieved observations per second.

**One chart:** complementary CDF of deadline lateness, one curve per load, with 1 ms and 10 ms reference lines.

**Disconfirming result:** lateness remains small relative to the event durations under investigation and does not materially worsen under load. Report that. Even substantial lateness does not prove claim (b).

### E2 — Actual pg_wait_sampling collector cadence

**Verdict:** worthwhile and directly relevant to a standard tool.

Changes:

- Distinct history timestamps identify **rounds that retained rows**, not necessarily every attempted round.
- Use persistent target backends and confirm their inclusion. Empty rounds cannot be reconstructed from missing history rows.
- Preallocate enough history before the run. Avoid changing its size during capture.
- Read history after the measured interval, with enough retained guard time. Fetching a large history during capture can itself delay the collector.
- Report gaps as gaps. The history timestamp alone cannot separate scheduler delay, lock acquisition, collector work, and servicing requests.
- Remember that the shared timestamp precedes the actual backend reads.

The ring-size budget is approximately

\[
H \ge f \times N_{\text{recorded processes}} \times T_{\text{retained}},
\]

plus margin. At 100 Hz, 16 client backends alone produce **96,000 rows in 60 seconds**. The default 5,000 rows cover at most **3.125 seconds** of that workload, less when background processes contribute. Eight clients alone give 6.25 seconds.

**Metrics:** represented rounds, mean achieved frequency, p50/p99/max interval, fraction of intervals over 20 ms, longest uncovered interval, complete retention span.

**One chart:** inter-round interval survival curve by load, with the nominal 10 ms interval marked.

**Disconfirming result:** the collector maintains adequate coverage at the relevant loads. That removes a practical objection to this collector under these conditions.

### E3 — Share accuracy against the trace

**Verdict:** useful, provided it compares identical semantics. It may demonstrate that sampling works well.

Start with a fixed set of client PIDs and ungated wait-state occupancy. Label zero as **“no registered wait”**, not measured running CPU. Then add a separately validated active-command definition.

For event \(e\), report:

\[
T_e=\sum_b\int_W I_{b,e}(t)\,dt,
\qquad
AAS_e=T_e/|W|,
\qquad
p_e=T_e/T_{\text{active}}.
\]

For samples, distinguish:

- Occupancy estimated from observations.
- Time estimated using the nominal 10 ms period.
- Any alternative estimator using actual observation intervals.

They answer different questions when rounds are missed. Weighting a sample by a long surrounding interval does not reconstruct hidden transitions.

**Metrics:**

- Signed share error in percentage points.
- Absolute event-time and AAS error.
- No-wait-labelled time ratio.
- Top-event membership and ordering separately.
- Transitions and completed waits per second, and per transaction.
- Throughput and latency overhead.
- Capture loss and attribution failures.

**One chart:** signed per-event AAS error against the reference, grouped by load, with intervals across repetitions.

**Disconfirming result:** errors are small, practically unimportant, and consistent with expected variation; no systematic CPU deficit appears. Say **“the profile is accurate.”** That strengthens the argument that event records provide different information.

### E4 — State consistency and lateness-dependent bias

**Verdict:** invalid as currently specified; split it into two analyses.

**First: consistency at the actual read time.**

A history row’s timestamp is not its backend-read timestamp. Instrument a validation build to bracket each read, or at least the entire scan and lock acquisition. Compare against a trace state only when it is stable throughout the relevant uncertainty interval.

Do **not** simply discard everything within ±0.5 ms of a transition. That removes the short, rapidly changing states central to the hypothesis and can leave a heavily selected population of long waits. Report all ambiguous observations and their distribution.

**Second: bias in when observations occur.**

A perfectly accurate sample can still be taken at an unrepresentative time. Compare observed occupancy against uniformly selected instants from the same reference trace, matched by PID and approximately stationary workload interval. Use randomized grid phases or time shifts to assess sensitivity without pretending backend observations in the same round are independent.

To call an observation “late,” instrument its intended deadline. A long interval between stored timestamps is not a complete lateness measurement.

**Metrics:** stable-interval mismatch rate, ambiguous fraction, read-time uncertainty, actual deadline lateness, and excess wait probability over a matched time-occupancy baseline.

**One chart:** excess wait probability versus measured lateness, with uncertainty intervals and a zero reference line.

**Disconfirming result:** no meaningful excess, an opposite-direction effect, or an effect that disappears after correcting timestamps and admission rules. A positive association still needs controls before you attribute it specifically to backends freeing CPUs by blocking.

### Add E5 — Same total wait time, different event histories

**Priority: highest.** This demonstrates the strongest argument without requiring sampling to malfunction.

Use a controlled workload with two patterns, for example:

- A small number of longer waits.
- Many shorter waits of the same event type.
- Approximately equal total wait time per execution.

A small test extension can generate labelled wait intervals and maintain an independent count. Use **measured durations**, not requested sleep times. Follow with a real lock or I/O workload if you want operational relevance.

Also downsample a validated trace offline at 1, 10, 100, and optionally 1,000 Hz over many phases. That removes the live sampler implementation from the mathematical comparison. Label it explicitly as resampling the recorded execution.

**Metrics:** occupancy error, episode-count recovery, event-duration quantiles, captured-event fraction, and ambiguity in reconstructed episodes.

**One chart:** two event-duration histograms with total wait time and the sampled share printed above each. The desired result is **similar shares, clearly different event populations**.

**Disconfirming result:** the standard tool plus already available counters answers the proposed diagnostic question adequately. Then sharpen the question to the missing distribution or sequence, or drop the example. Do not manufacture a failure by withholding useful standard metrics.

### Add E6 — A brief wait causing an execution-level regression

**Priority: highest.** This gives the audience a reason to care.

Create a reproducible query normally completing quickly, with a controlled minority of executions encountering brief lock waits. Randomize their phase relative to sampling. Include durations around 0.1, 1, 5, and 20 ms where the system can actually produce them.

Record request identity, backend identity, execution boundaries, outcomes, and client latency independently. Add cancellations only after ordinary execution attribution is validated.

**Metrics:** client p50/p99, failure rate, realized wait duration, fraction of affected executions with a matching wait sample, and event-sequence completeness during tracing.

**One chart:** probability of detecting the injected wait versus its **measured duration**, with the ideal \(\min(1,d/\Delta)\) curve and confidence intervals.

**Disconfirming result:** the real waits are long enough that sampling captures essentially all relevant executions, or existing counters/logs already explain the regression. Also reject the demonstration if tracing materially changes the phenomenon or cannot attribute its events reliably.

**Experiment order:** E5 and E6 first, E2 and E3 next. E1 and E4 are supporting research. A ten-minute talk does not need a new theory of Linux scheduling bias.

## 5. Proposed final sampling section

Four slides, approximately **180 seconds total**. All illustrative numbers below should be labelled as illustrations.

### Slide 1 — “80% waiting. How many waits?” — 45 seconds

**On-slide content**

Two one-second timelines:

| Execution A | Execution B |
|---|---|
| 1 wait × 800 ms | 4,000 waits × 0.2 ms |
| 800 ms total waiting | 800 ms total waiting |

**Same 80% time share. Different counts and durations.**

Small label: “Illustration; aggregate occupancy.”

**Speaker note**

“Sampling can give the right answer: both executions spent 80 percent of their time waiting. But that answer does not tell me whether I need to investigate one stalled operation or thousands of repeated waits. I need the event count and duration distribution to choose the next investigation.”

### Slide 2 — “At 100 Hz, a 5 ms wait is a coin flip” — 40 seconds

**On-slide content**

| One wait lasts | Chance of being observed |
|---|---:|
| 0.1 ms | 1% |
| 1 ms | 10% |
| 5 ms | 50% |

**No sample does not mean no wait.**

Footnote: “Periodic 10 ms observations; random wait phase; no missed rounds.”

**Speaker note**

“These are probabilities for one particular wait, not errors in the aggregate profile. Repeated activity can produce an excellent profile while the evidence for this execution is missing. That matters when five milliseconds breaks the latency budget.”

### Slide 3 — “What changed: fewer waits, or shorter waits?” — 50 seconds

**On-slide content**

\[
\text{Total wait time}=\text{wait count}\times\text{mean duration}
\]

For a fix, measure:

- Waits per execution.
- Duration distribution, including the tail.
- The sequence in affected executions.

Visual: a small pair of illustrative duration histograms, labelled “fewer events” and “shorter events.”

**Speaker note**

“A sampled profile can show that a fix reduced waiting, and that is useful evidence. Counts and durations show how it helped; the execution timeline shows where the remaining delay occurred. Existing counters may answer part of this, but a wait-share chart alone cannot answer all three questions.”

### Slide 4 — “Sample continuously. Trace a bounded window.” — 45 seconds

**On-slide content**

**Sampling:** locate the workload and dominant waits.

**Tracing:** record registered wait transitions, durations, and execution timelines.

**pg_wait_tracer:** cheap sampling by default → bounded hardware-watchpoint tracing.

Small footer: “Tracing has measurable overhead; capture must cover the event.”

**Speaker note**

“This is why pg_wait_tracer samples by default: sampling is useful and cheap. When the question requires individual events, it can escalate for a bounded window; it cannot reconstruct an event that happened before tracing started. The next step is how a hardware watchpoint on PostgreSQL’s four-byte wait field makes that possible.”