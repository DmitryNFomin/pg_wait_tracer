#include "evidence_query.h"
#include "evidence_codec.h"
#include <stdlib.h>
#include <string.h>

static bool equal(pgwt_core_event a, pgwt_core_event b)
{ return a.length == b.length && (!a.length || !memcmp(a.data,b.data,a.length)); }

static bool trace_container(pgwt_evidence_buffer b)
{
    if (b.size < 6 || memcmp(b.data,"PGWT",4) || b.data[5] != 0) return false;
    switch (b.data[4]) {
    case 2: case 3: case 4: case 6: return true;
    default: return false;
    }
}

int pgwt_evidence_query(const pgwt_evidence_buffer *buffers, size_t n,
                        const pgwt_evidence_target *target,
                        const pgwt_core_query *query,
                        pgwt_core_result *wait_time, pgwt_core_result *completed,
                        pgwt_query_storage *storage, const char **reason)
{
    if (reason) *reason = "invalid pilot arguments";
    if (!wait_time || !completed || !storage) return -1;
    memset(wait_time,0,sizeof(*wait_time));
    memset(completed,0,sizeof(*completed));
    memset(storage,0,sizeof(*storage));
    if (!target || wait_time==completed || (target->capture.length && !target->capture.data) ||
        (n && !buffers) || n > PGWT_PILOT_MAX_FILES) return -1;
    storage->files = n;
    pgwt_codec_buffer v5[PGWT_PILOT_MAX_FILES];
    size_t count = 0;
    for (size_t i=0;i<n;++i) {
        if (buffers[i].size && !buffers[i].data) return -1;
        if (!trace_container(buffers[i])) v5[count++] = (pgwt_codec_buffer){buffers[i].data,buffers[i].size};
    }
    if (target->invalid) {
        pgwt_core_prepared prepared;
        pgwt_core_prepare(NULL,NULL,0,&prepared);
        pgwt_core_wait_metrics(&prepared,query,wait_time,completed);
        if (reason) *reason = NULL;
        return 0;
    }
    pgwt_codec_decoded decoded = {0};
    pgwt_evidence_output output = {0};
    int status = pgwt_codec_decode(v5,count,&decoded);
    if (status < 0) {
        if (reason) *reason = "evidence decode failed";
        return -1;
    }
    /* Invalid headers are unassigned diagnostics, not normalized codec stops.
     * Surface them here without changing the codec's reconciliation rules. */
    for (size_t pass=0;pass<2;++pass) {
        size_t stop_count = pass ? decoded.diagnostic_count : decoded.stop_count;
        for (size_t i=0;i<stop_count;++i) {
            const pgwt_codec_stop *s = pass ? &decoded.diagnostics[i] : &decoded.stops[i];
            if (pass && s->group != SIZE_MAX) continue;
            if (storage->codec_stop_count == PGWT_PILOT_MAX_FILES) {
                if (reason) *reason = "pilot storage stop limit exceeded";
                pgwt_codec_decoded_free(&decoded);
                return -1;
            }
            /* Codec indices name sorted, deduplicated deliveries. Map back to
             * the caller's files, including trace containers filtered above. */
            const pgwt_codec_buffer *b = &decoded.deliveries[s->input_file].bytes;
            size_t file=0;
            while (file<n && (buffers[file].data!=b->data || buffers[file].size!=b->size)) ++file;
            storage->codec_stops[storage->codec_stop_count++] = (pgwt_query_stop){.file=file,
                .block_offset=s->block_offset,.record_offset=s->record_offset,
                .has_record_offset=s->has_record_offset,.reason=pgwt_codec_code_name(s->code)};
        }
    }
    /* Capture ambiguity and replay stops have their own storage fields. */
    storage->codec_failed = storage->codec_stop_count != 0;
    for (size_t i=0;i<decoded.capture_count;++i) {
        const pgwt_codec_capture *c = &decoded.captures[i];
        if (!equal(c->capture,target->capture)) continue;
        if (c->ambiguous) {
            storage->capture_ambiguous = true;
            if (reason) *reason = "capture is ambiguous across boot identities";
            pgwt_codec_decoded_free(&decoded);
            return 1;
        }
        if (c->has_replay && c->replay.stop.stopped) {
            const pgwt_evidence_stop *s = &c->replay.stop;
            storage->has_replay_stop = true;
            storage->replay_stop = (pgwt_query_stop){.file=s->file_index,
                .record_offset=s->record_index,
                .has_record_offset=!s->descriptor,.descriptor=s->descriptor,
                .reason=pgwt_evidence_stop_label(s->reason)};
            if (s->reason == PGWT_REPLAY_STOP_BASELINE_MISMATCH) {
                storage->baseline_mismatch = true;
                storage->expected_baseline_known = s->expected_baseline.known;
                storage->declared_baseline_known = s->declared_baseline.known;
                memcpy(storage->expected_baseline,s->expected_baseline.value,sizeof(storage->expected_baseline));
                memcpy(storage->declared_baseline,s->declared_baseline.value,sizeof(storage->declared_baseline));
            }
        }
    }
    pgwt_evidence_file old = {.version=4,.capture=target->capture,.present=true};
    bool only_legacy = !count && n;
    pgwt_evidence_input input = {.files=only_legacy ? &old : decoded.files,.file_count=only_legacy ? 1 : decoded.file_count,
        .capture=target->capture,.pid=target->pid,.has_fork=target->has_fork,
        .fork=target->fork,.unsupported_shape=target->invalid};
    status = pgwt_evidence_replay(&input,query,&output,reason);
    if (status == PGWT_REPLAY_OK) {
        *wait_time = output.wait_time;
        *completed = output.completed_waits;
    }
    pgwt_evidence_output_free(&output);
    pgwt_codec_decoded_free(&decoded);
    return status;
}
