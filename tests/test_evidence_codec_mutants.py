#!/usr/bin/env python3
"""Codec mutations and named semantic-guard sweep, in disposable /tmp builds.

Every claimed kill builds, runs the full codec/scenario suite and fails the
named assertion. Compiler errors, harness errors, signals and sanitizer findings
are errors, never kills. The guard registry targets independent semantic checks;
allocation/overflow fail-closed guards are preserved, not weakened to provoke UB.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_evidence_codec_oracle import ROOT, build


# (name, original text, replacement, independently asserted witness)
MUTANTS = [
    ("C01_crc_polynomial", "UINT32_C(0xedb88320)", "UINT32_C(0xedb88321)", "crc_known_answers"),
    ("C02_sha_round", "0x428a2f98", "0x428a2f99", "sha256_known_answers"),
    ("C03_digest_includes_flags", "sha_update(&s,b.data,6);sha_update(&s,b.data+8,end-8)",
     "sha_update(&s,b.data,8);sha_update(&s,b.data+8,end-8)", "predecessor_mutable_flags"),
    ("C04_digest_includes_footer", "sha_update(&s,b.data+8,end-8)",
     "sha_update(&s,b.data+8,b.size-8+0*end)", "footer_digest_excluded"),
    ("C05_timestamp_delta", "uleb_write(&p,f->records[i+j].t-prev)",
     "uleb_write(&p,f->records[i+j].t+0*prev)", "late_rows_metadata_opaque"),
    ("C06_cpu_metadata", "uleb_write(&p,d->transitions ? d->transitions[i+j].cpu_ns : 0)",
     "uleb_write(&p,0)", "late_rows_metadata_opaque"),
    ("C07_counter_slots", "v->value[i]=get(r,8)", "v->value[14-i]=get(r,8)", "optionals_counter_slots"),
    ("C08_optional_zero", "*present=boolean(r);if (*present) *value=get(r,8);",
     "*present=boolean(r);if (*present) { *value=get(r,8);if (!*value) *present=false; }", "optionals_counter_slots"),
    ("C09_null_certificate_fault", "v->kind=PGWT_EVIDENCE_FRONTIER;counters_read(r,&v->counters);break;",
     "v->kind=PGWT_EVIDENCE_FRONTIER;counters_read(r,&v->counters);if (!v->counters.known) r->bad=true;break;", "row10_null_certificate_r3"),
    ("C10_publish_bad_segment", "if (reason) {\n            *stop=", "if (reason) {\n            if (reason==PGWT_CODEC_CRC) (void)append_rows(f,info,rows,n,off);\n            *stop=", "row5_second_block_atomic"),
]


def guard(name, expression, witness):
    return (name, expression, "(false && (" + expression + "))", witness)


GUARDS = [
    guard("G01_file_magic", "at(p,4)!=0x54574750", "row1_file_magic"),
    guard("G02_trace_version", "at(p+4,2)!=5", "row1_trace_version"),
    guard("G03_extension", 'memcmp(p+36,"EVD1",4)', "row1_extension_magic"),
    guard("G04_descriptor_schema", "at(p+44,2)!=1", "row1_descriptor_version"),
    guard("G05_header_reserved", "at(p+46,2)", "row1_header_reserved"),
    guard("G06_header_crc", "crc!=at(p+h-4,4)", "row5_header_crc"),
    guard("G07_boolean", "x>1", "row8_bad_optional"),
    guard("G08_header_mapping_id", "map[i].id==UINT32_MAX", "row1_mapping_exit_id"),
    guard("G09_mapping_order", "(i && map[i-1].id>=map[i].id)", "row1_mapping_order"),
    guard("G10_mapping_role", "(map[i].role!=1 && map[i].role!=2)", "row1_mapping_role"),
    guard("G11_mapping_unique", "equal(map[i].name,map[j].name)", "row1_duplicate_names"),
    guard("G12_payload_schema", "get(&r,2)!=1", "row2_payload_version"),
    ("G13_payload_reserved", "get(&r,2)!=1 || get(&r,2)", "get(&r,2)!=1 || (get(&r,2), false)", "row2_payload_reserved"),
    guard("G14_equal_counts", "get(&r,4)!=n", "row3_unequal_counts"),
    guard("G15_common_t", "len<8", "row3_missing_common_t"),
    guard("G16_body_bound", "len>65535", "row3_body_oversize"),
    guard("G17_payload_suffix", "r.at!=r.n) return PGWT_CODEC_FRAMING", "row4_unconsumed_suffix"),
    guard("G18_block_count", "n>RECORD_MAX", "row3_count_4097"),
    guard("G19_block_crc", "pgwt_codec_crc32(bh,64+s)!=at(bh+64+s,4)", "row5_block_crc"),
    guard("G20_self_offset", "at(bh+48,8)!=off", "row5_self_offset"),
    guard("G21_preceding_crc", "at(bh+56,4)!=preceding", "row5_preceding_crc"),
    guard("G22_block_type", "type>2", "row2_block_type"),
    guard("G23_codec", "codec>1", "row2_block_codec"),
    guard("G24_evidence_raw", "(type==2 && codec)", "row2_evidence_compressed"),
    guard("G25_block_reserved", "at(bh+30,2)", "row2_block_reserved"),
    guard("G26_codec_reserved", "bh[61] || bh[62] || bh[63]", "row2_codec_reserved"),
    guard("G27_raw_sizes", "(codec==0 && s!=u)", "row6_raw_size"),
    guard("G28_nonsample_weights", "(type!=1 && (at(bh+32,8) || at(bh+40,8)))", "row6_type0_weights"),
    guard("G29_timestamp_metadata", "rows[0].r.t!=at(bh,8) || rows[n-1].r.t!=at(bh+8,8)", "row6_type0_time"),
    guard("G30_sample_weight", "type==1 && maximum!=at(bh+40,8)", "row6_sample_weight"),
    guard("G31_leb_range", "(i==9 && b!=1)", "row6_uleb_overflow"),
    guard("G32_leb_minimal", "(i && b==0)", "row6_uleb_redundant"),
    guard("G33_lz4_end_literals", "(u<5 || last_literals>=5)", "row6_lz4_last_five"),
    guard("G34_lz4_end_match", "(!matched || (last_match<=u && u-last_match>=12))", "row6_lz4_last_match"),
    guard("G35_attach_method", "method>1", "row7_unknown_method"),
    guard("G36_end_reason", "!reason || reason>3", "row7_unknown_end"),
    guard("G37_pid", "!v->pid || (v->has_fork && v->fork>v->t)", "row8_bad_pid"),
    guard("G38_life_start", "v->start>v->t", "row8_bad_life_start"),
    guard("G39_row_duration", "v->duration>v->t", "row9_duration_gt_t"),
    guard("G40_same_event", "old[i]==new_id[i]", "row9_same_event"),
    guard("G41_row_old_mapping", "(old[i] && (!a || a->role!=1)) ||", "row9_unmapped_old"),
    guard("G42_sample_pid", "!v->pid || (old[i] && (!a || a->role!=1))", "row9_sample_pid"),
    guard("G43_link_digest", "memcmp(digest,info->previous_sha256,32)!=0", "row5_predecessor_digest"),
    guard("G44_link_data_end", "info->previous_data_end!=out->info[prev].data_end", "row5_predecessor_end"),
    ("G45_writer_validation", "if (!valid_record(&f->records[i],d))", "if (false && !valid_record(&f->records[i],d))", "writer_life_pid"),
    guard("G46_writer_mapping_unique", "equal(m->name,d->mapping[j].name)", "writer_duplicate_names"),
    guard("G47_writer_mapping_order", "(i && m->id<=d->mapping[i-1].id)", "writer_mapping_order"),
    guard("G48_sample_tick_weight", "rows[0].r.t==info->last_sample_time && at(bh+32,8)!=info->last_sample_weight", "row6_sample_split_weight"),
    guard("G49_sample_stream_order", "rows[0].r.t<info->last_sample_time", "row6_sample_stream_order"),
]
# These two registry entries include surrounding syntax to select one guard;
# keep the replacement syntactically valid rather than disabling adjacent code.
GUARDS[16] = ("G17_payload_suffix", "if (r.at!=r.n) return PGWT_CODEC_FRAMING;",
              "if (false && r.at!=r.n) return PGWT_CODEC_FRAMING;", "row4_unconsumed_suffix")
GUARDS[40] = ("G41_row_old_mapping", "(old[i] && (!a || a->role!=1)) || (new_id[i]",
              "(false && (old[i] && (!a || a->role!=1))) || (new_id[i]", "row9_unmapped_old")
# A positive end constraint must become true when removed, not false.
for i in (32, 33):
    name, before, _, witness = GUARDS[i]
    GUARDS[i] = (name, before, "(true || " + before + ")", witness)
GUARDS = [case for case in GUARDS if case[0] != "G18_block_count"]
GUARDS = [
    (name, before, "(get(&r,2), false)", witness)
    if name == "G12_payload_schema" else
    (name, before, "(get(&r,4), false)", witness)
    if name == "G14_equal_counts" else
    (name, "if (!reason || reason>3) r->bad=true;",
     'if (!reason || reason>3) v->end_reason=(pgwt_core_event){"stop",4};', witness)
    if name == "G36_end_reason" else
    (name, "|| old[i]==new_id[i] ||", "|| (false && old[i]==new_id[i]) ||", witness)
    if name == "G40_same_event" else case
    for case in GUARDS for name, before, after, witness in [case]
]


def run(binary, witness=None):
    args = [str(binary)]
    # A corrupt hash primitive makes the golden file unreadable; run its
    # independent arithmetic witness before attempting higher-level fixtures.
    if witness in ("crc_known_answers", "sha256_known_answers"):
        args += ["--case", witness]
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=60)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--guard-sweep", action="store_true")
    args = parser.parse_args()
    cases = GUARDS if args.guard_sweep else MUTANTS + GUARDS
    original = (ROOT / "src/core/evidence_codec.c").read_text()
    with tempfile.TemporaryDirectory(prefix="pgwt-codec-mutants-") as directory:
        directory = Path(directory)
        baseline = directory / "baseline"
        build(baseline)
        result = run(baseline)
        if result.returncode or result.stderr:
            raise RuntimeError("baseline failed:\n" + result.stdout + result.stderr)
        for name, before, after, witness in cases:
            if original.count(before) != 1:
                raise RuntimeError(f"{name}: expected one source anchor, got {original.count(before)}")
            if f"codec {witness} PASS\n" not in result.stdout:
                raise RuntimeError(f"{name}: baseline did not reach witness {witness}")
            source = directory / "codec.c"
            source.write_text(original.replace(before, after, 1))
            binary = directory / name
            try:
                build(binary, source)
            except subprocess.CalledProcessError as exc:
                raise RuntimeError(f"{name}: BUILD ERROR, not a kill:\n{exc.stderr}") from exc
            mutated = run(binary, witness)
            if mutated.returncode != 1 or f"codec {witness} FAIL assertion\n" not in mutated.stdout:
                raise RuntimeError(f"{name}: SURVIVED or harness error ({mutated.returncode}); "
                                   f"required witness {witness}\n{mutated.stdout}\n{mutated.stderr}")
            if any(s in mutated.stderr for s in ("Sanitizer", "runtime error:", "ERROR:")):
                raise RuntimeError(f"{name}: sanitizer error, not a kill:\n{mutated.stderr}")
            print(f"codec {name} KILLED by {witness} (built; intended assertion failed)", flush=True)
            binary.unlink()
        label = "guard sweep" if args.guard_sweep else "mutants and guard sweep"
        print(f"codec {label}: {len(cases)}/{len(cases)} killed; 0 survivors PASS")


if __name__ == "__main__":
    main()
