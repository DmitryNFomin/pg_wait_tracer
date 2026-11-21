# Reconciliation decisions (Astra, 2026-11-20)

Where the two design advisers differed, the consolidated amendment chose as follows.


- Choose Fable’s `/proc/syscall` plus outer voluntary-count bracket as the concrete interface, retaining Astra’s `wait_task_inactive()` and no-wrap requirements because counters/state letters alone do not prove inactivity.
- Choose isolated provisional initialization over Fable’s conditional whole-struct replacement because rejecting a bracket cannot restore overwritten concurrent producer updates.
- Choose an explicit CPU-validity bit over a nonzero sentinel because a preserved accumulator of numeric zero is valid.
- Retain Astra’s strict completion and staged-record reconciliation alongside Fable’s inactivity witness because task silence does not prove publication completion.
- Choose Fable’s pre-ADMIT-only retries and retain Astra’s pending fallback because activity does not guarantee a future qualifying record.
- Retain lifecycle binding in addition to the quiet witness because neither task age nor a bracket excuses an FD/lifetime mismatch.
- Resolve and compare addresses inside the bracket, retaining post-bracket stability qualification because inactivity does not prevent later relocation.
- Correct quiet fixtures to place seed time before final enable; Q09 uses integer timestamps while preserving delayed earlier-attempt delivery.
- Treat failed optional quiet proofs as rejected candidates; retain faults for false admission/coherence claims and independently counted failures.
