The evidence supports benign CPU inside the PgSleep wait boundary at the
resolution of this capture. It does not prove a stale label or an unclosed
`on_cpu_ts`. The precise source of the extra CPU is not recorded. No source
fix is proposed; `src/` and `tests/measure_wait_cpu.py` are unchanged.

The input is the retained October 2 local-time PG18 full capture in the primary
checkout, not a new workload run. The outlier started at **2026-10-01
23:28:31.539822175 UTC**. Trace SHA256 is
`52d2dbd03c88c753589dedc6e674bb48c2b309f22a22646431278c57a5eca214`.
The raw reader reports format v3, PG18, 1052 blocks, and mono-to-wall offset
1790450964570007046 ns. Block and record offsets in the evidence are zero-based.

`backend-context.json` joins PID 1171002 to a local postgres client in database
postgres, and query ID -8886085649470066503 to **SELECT pg_sleep(3);**. The raw
record is block 777, record 3734: start 1790897311539822175, end
1790897314543859529, duration **3004037354 ns**, CPU **923834 ns**,
ratio **0.0307530796%**. Its name is independently exposed by execution detail.

The following records are from `pgsleep-raw.jsonl`. A transition's timestamp
closes its preceding interval, so the interval start is timestamp minus duration;
markers have zero duration and must not be mistaken for interval boundaries.

| Record or marker | Start ns | End ns | Duration ns | CPU ns |
|---|---:|---:|---:|---:|
| preceding ClientRead | 1790897306901856661 | 1790897311539638842 | 4637782181 | 37380 |
| CMD_START | 1790897311539669510 | same | 0 | 0 |
| PLAN_START | 1790897311539758416 | same | 0 | 0 |
| PLAN_END | 1790897311539791107 | same | 0 | 0 |
| EXEC_START | 1790897311539810433 | same | 0 | 0 |
| CPU → PgSleep | 1790897311539638842 | 1790897311539822175 | 183333 | 183333 |
| PgSleep → CPU | 1790897311539822175 | 1790897314543859529 | 3004037354 | 923834 |
| EXEC_END | 1790897314543881321 | same | 0 | 0 |
| CMD_END | 1790897314543925584 | same | 0 | 0 |
| CPU → ClientRead | 1790897314543859529 | 1790897314544139073 | 279544 | 279544 |
| following ClientRead | 1790897314544139073 | 1790897320640992812 | 6096853739 | 37839 |

There is no intervening same-PID transition or lifecycle marker inside the sleep.
The previous label closes at exactly the sleep start; the next interval starts
at exactly the sleep end. Execution begins 11.742 µs before the sleep, and ends
21.792 µs after it. The statement's measured execution duration is
3004.070888 ms. Preceding and following CPU work is separately recorded, rather
than disappearing into the sleep. A label extending through another statement,
or an on-CPU clock left running throughout the 3-second block, is not the
observed pattern.

The immediate previous and next identical sleeps on this backend carry
35.908 µs / 3003.065017 ms and 31.589 µs / 3003.073513 ms. Compared with the
next sleep, the outlier adds **963.841 µs wall** and **892.245 µs CPU**; the
duration-minus-CPU difference adds only **71.596 µs**. This comparison uses the
specific neighbours, not a ratio diluted across the whole run. It supports extra
CPU work inside the recorded wait boundary while the approximately 3.003-second
non-CPU component stays comparable. `comparison.json` retains the inputs.

The measured allowance the criterion needs to state is therefore **0.923834 ms
of combined wait entry/exit and wakeup CPU observed in this capture**, not a
claim that every wakeup gets that budget. We cannot split the 923834 ns among
wait entry, wait exit, intervening wakeups, signal handling, probe overhead,
or interrupt activity. There are no scheduler-edge records or stacks in this
retained input; a short scheduler-accounting error cannot be independently
excluded using it. The benign verdict is supported at the recorded interval
level, not a proof of a particular wakeup cause. The duration alone is not proof.
The millisecond signature remains a reason to investigate; there is no new pass
threshold, and this annotation certifies no rehearsal.

`Lock:transactionid`'s 100% maximum is a real ratio, not a millisecond CPU
anomaly. Exactly two records tie at 100%: PID 1170989 at 1790896365871005376,
**12974 ns duration = CPU**; PID 1170991 at 1790896467430891351,
**4909 ns duration = CPU**. Both belong to query 3886912043147135675, the
pgbench `UPDATE pgbench_branches SET bbalance = bbalance + ... WHERE bid = ...`.
Both enter and leave the transaction lock within that execution, with separate
CPU intervals on either side. These are short CPU-bearing lock boundaries,
with no measurable off-CPU portion, not CPU mislabeled across a later execution.
The maximum CPU event is different: PID 1171010 at 1790897518868330077,
112622 ns CPU in 1000145950 ns duration, query -8013378961575804280, the hot-row
UPDATE. It is followed by a 13986 ns CPU gap and a second transaction-lock
interval lasting 689489 ns with 27020 ns CPU, all in the same execution.
The original 590-event measurement remains 4.149202% arithmetic mean ratio;
its duration-weighted ratio is 0.0101601%. Neither ratio changes the records.

`Lock:extend` is **statistically empty (n=2)** for a class verdict. Both events
belong to PID 1170990, query 286264995346867541, INSERT into pgbench_history:
1790896554571596265, 62917 ns duration / 20118 ns CPU (31.97546%), and
1790896874975270119, 75692 ns duration / 22122 ns CPU (29.226338%). Each closes
before a short CPU gap and IO:DataFileExtend in the same INSERT, with no marker
inside the lock. Neither individual neighbourhood proves an attribution defect.
The original ratio mean stays 30.600899%; no small-sample class conclusion follows.

Reproduction uses the **unchanged committed measurement tool**:

```sh
bash tests/results/perclass-wait-cpu/build_server.sh
python3 tests/results/pgsleep-outlier/investigate.py /PATH/TO/demo_rehearsal/trace
```

The investigation imports `Measurement` and `replay`, repeats a 25-second window
around the outlier with the original **824430 ns** threshold, and confirms six
selected events against `Measurement.discover` / `detail`. All CPU values match
the raw records. Per-target protocol responses, query/backend context, and raw
neighbourhoods are retained. The small C helper only exposes raw records and
markers using the existing decoder; it implements no threshold or CPU estimator.
It scans every block and refuses decode/count failures. The retained raw
Lock/PgSleep census contains 326 PgSleep, 590 transactionid, 2 extend, and
436 relation records; this is not a replacement for the measurement's scope.

The repeated window still reports **REFUSED**, one PgSleep exceedance, one
truncated lane with 18510 omitted records, and six unrepresented relation waits.
The earlier full walk's 55 truncated lanes, 1018050 omitted events and 436
unrepresented relation waits remain genuine coverage limits. Raw neighbourhood
inspection supplies evidence for selected events; it does not certify the
unmeasured executions or any class as globally clean. No box-check, rehearsal,
PR or push was performed.
