# Mutant: a sidecar found at the destination is adopted without checking that
# it describes this trace — a gate approving whatever it finds.
d=$1
perl -0pi -e 's/    rc = pgwt_open_ckpt_load\(sidecar, id, &view, NULL\);/    rc = pgwt_open_ckpt_load(sidecar, NULL, &view, NULL);   \/* mutant *\//' $d/event_writer.c
grep -q 'pgwt_open_ckpt_load(sidecar, NULL' $d/event_writer.c
