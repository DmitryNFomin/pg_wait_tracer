# Mutant: the ORIGINAL bug — rotation deletes the checkpoint instead of
# carrying it, so a kill in that window loses the open intervals.
d=$1
perl -0pi -e 's/        ckpt_handover\(w->trace_dir, new_path, &id, w->verbose\);/        ckpt_discard(w->trace_dir, "rotation", w->verbose);   \/* mutant *\//' $d/event_writer.c
grep -q 'ckpt_discard(w->trace_dir, "rotation"' $d/event_writer.c
