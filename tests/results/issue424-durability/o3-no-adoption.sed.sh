# Mutant: handover does not adopt a sidecar stranded at the destination, so an
# interrupted handover loses the evidence on the next start.
d=$1
perl -0pi -e 's/    if \(access\(sidecar, F_OK\) != 0\)\n        return 0;\n    rc = pgwt_open_ckpt_load\(sidecar, id, &view, NULL\);/    return 0;\n    rc = pgwt_open_ckpt_load(sidecar, id, &view, NULL);/' $d/event_writer.c
grep -q '    return 0;\n    rc = pgwt_open_ckpt_load' $d/event_writer.c || grep -B1 'rc = pgwt_open_ckpt_load(sidecar' $d/event_writer.c | grep -q 'return 0;'
