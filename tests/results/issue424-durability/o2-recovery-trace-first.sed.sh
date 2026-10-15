# Mutant: the OTHER original bug — recovery renames the trace BEFORE the
# sidecar, so a kill between them strands the checkpoint and the next start
# deletes it as having no current.trace.
d=$1
perl -0pi -e 's/    ckpt_handover\(w->trace_dir, dest, &id, true\);\n    pgwt_crash_point\(PGWT_CRASH_RECOVER_AFTER_CKPT\);\n\n    if \(rename\(cur, dest\) != 0\) \{/    if (rename(cur, dest) != 0) {/' $d/event_writer.c
perl -0pi -e 's/    pgwt_crash_point\(PGWT_CRASH_RECOVER_AFTER_TRACE\);\n    unlink\(meta\);/    pgwt_crash_point(PGWT_CRASH_RECOVER_AFTER_CKPT);\n    ckpt_handover(w->trace_dir, dest, &id, true);\n    pgwt_crash_point(PGWT_CRASH_RECOVER_AFTER_TRACE);\n    unlink(meta);/' $d/event_writer.c
grep -q 'pgwt_crash_point(PGWT_CRASH_RECOVER_AFTER_CKPT);\n    ckpt_handover' $d/event_writer.c || grep -A1 'PGWT_CRASH_RECOVER_AFTER_CKPT' $d/event_writer.c | grep -q ckpt_handover
