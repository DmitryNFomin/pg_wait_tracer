# Mutant: the checkpoint publishes (cuts a block boundary on every call).
d=$1
perl -0pi -e 's/    return pgwt_open_ckpt_write\(w->trace_dir,/    if (pgwt_writer_publish(w) != 0) return -1;   \/* mutant: flush *\/\n    return pgwt_open_ckpt_write(w->trace_dir,/' $d/event_writer.c
grep -q 'mutant: flush' $d/event_writer.c
