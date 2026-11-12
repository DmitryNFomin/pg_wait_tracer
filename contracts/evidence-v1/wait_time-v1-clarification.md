# Proposed addition to wait_time v1

Insert immediately after the Lifetime bullet in section 1 of
`agent/868-core-wait-time:contracts/wait_time-v1/contracts/metrics/wait_time.md:18-19`:

> `life_start` may be a proven conservative lower bound on the actual process start, rather than the exact start. In that case, `L` and `R = W ∩ L` conservatively include the possibly nonexistent prefix; this prefix requires coverage under the same rules as every other part of `R`, and the lower bound alone supplies neither coverage nor wait observations. A collector must not use discovery or attachment time as that lower bound unless it independently proves that time is no later than the actual start.

No new field, reason, metric version, validation rule, arithmetic, exit rule, or open-wait rule
is proposed. Capture retains `start_known`; the frozen core still consumes the numeric `life_start`.
The wording does not permit unknown or approximate wait entry timestamps.

Existing-case impact: **none — wait_window_01 through wait_window_48**. All 48 input JSON files
were read from commit `cca90301853baaf4ca67dd6f03af77b38bc7e6ed`; none supplies a separate actual
start, a start-known flag, or a rule deriving start from capture. Every existing numeric lifetime,
validation outcome, R, coverage requirement, and formula remains exactly the same. Expected-answer
files were not consulted or changed.

Boundary cases checked explicitly: 10 (window before start), 11–12 (exit), 26 and 40 (uint64 limits),
31–32 and 44 (exit inclusion), 41–42 (open before known exit), 47 (coverage outside lifetime), and
48 (empty lifetime). The clarification does not change their supplied endpoints or validity.
