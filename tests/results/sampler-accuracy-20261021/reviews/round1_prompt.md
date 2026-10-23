You are a senior PostgreSQL performance engineer and a sharp, skeptical conference reviewer. Be direct.

Context. Dmitry Fomin (author of pg_wait_tracer, https://github.com/DmitryNFomin/pg_wait_tracer, and of a proposed
PostgreSQL patch for in-core wait event timing, Commitfest 6984) is preparing a 10-minute talk:
"Beyond Sampling: True Wait Event Tracing with pg_wait_tracer". pg_wait_tracer puts a CPU hardware watchpoint on each
backend's 4-byte wait_event_info and records every wait transition with eBPF; its default mode is a cheap
userspace sampler that escalates to exact tracing for bounded windows.

Files in this directory (read them):
- slides/ORDER.txt and slides/*.html: the current deck, one HTML file per slide; speaker notes are in <aside>.
  The sampling section is slides rate, odds, sowhat, same, sched.
- evidence/: measured data. sweep294.log, *.xval2.txt, analyze_round2.out: pg_wait_tracer's own sampler vs its exact
  tier on the same windows (CPU share under-read; sampled/exact CPU 0.77-0.88 after a read-order bug fix).
  README_how_and_perf.md: how it works and overhead. AAS_semantics_runqueue.md: run-queue note. gate2_env.txt: test box.
- TEST_PLAN_DRAFT.md: experiments we intend to run on a 4-vCPU test box.

The speaker's problem. He needs VERY STRONG points for why sampling (pg_stat_activity polling, pg_wait_sampling at its
100 Hz default, pgsentinel, AWS Performance Insights / RDS, Oracle-ASH-style tools) is not good enough. The audience
will push back with "so what? the profile is right on average". Comparisons against his own sampler invite
"that's a bug in your sampler", so arguments must be about sampling by nature, or about the standard tools.

Tasks. Write your answer as markdown with these sections:

1. Verdict on the current sampling section (slides rate, odds, sowhat, same, sched): what is weak, wrong, overclaimed
   or likely to be challenged by an expert PostgreSQL audience; what to cut. Check every number and formula.
2. The strongest arguments, ranked. For each: one-sentence claim for a slide, why it is true by the nature of
   sampling (math or mechanism), and the most damaging audience objection with the answer.
3. Real situations where sampling missed or misled an investigation or a fix, WITH LINKS. Use web search. Look for:
   pgsql-hackers / pgsql-performance threads, PostgreSQL bug reports, blog posts and conference talks (e.g. on
   LWLock / lock manager contention, short lock spikes, fsync/WAL stalls, cancelled statements, p99 latency regressions,
   Performance Insights or pg_wait_sampling limitations, Oracle ASH vs 10046 trace / SQL trace arguments, Brendan Gregg
   on sampling vs tracing, coordinated omission, aliasing). For each: link, one-line summary, and exactly how it
   supports a claim. Only include links you actually opened; mark anything you could not verify. Prefer primary sources.
4. Review of TEST_PLAN_DRAFT.md: is each experiment valid and convincing; confounders (CPU frequency, SMT, the tracer's
   own overhead, clock alignment, pg_wait_sampling collector design: one timestamp per round, reads PGPROC without
   locks, history ring size); what to add or drop; the exact metrics and the one chart per experiment that would
   make the strongest slide. Say what result would DISPROVE the claim, so we report honestly either way.
5. A proposed final sampling section: 3-5 slides, each with title, on-slide content and a 2-3 sentence speaker note,
   fitting about 3 minutes of a 10-minute talk.

Do not modify any files. Your final message is the full markdown answer.
