#define _POSIX_C_SOURCE 200809L
/* Test-only JSON/hex transport. The Python harness materializes abstract models;
 * this process calls the real encoder/decoder/replay APIs. No expected answers. */
#include "core/evidence_codec.h"
#include "evidence_json.c"

static uint64_t number(const json_node *n) { uint64_t v=0;if (!integer(n,&v)) exit(2);return v; }
static pgwt_codec_bytes unhex(const json_node *n)
{
    if (!n || n->kind!=J_STRING || n->length%2) exit(2);
    pgwt_codec_bytes b={malloc(n->length/2+1),n->length/2};if (!b.data) exit(2);
    for (size_t i=0;i<b.size;++i) {
        unsigned v;if (sscanf(n->string+2*i,"%2x",&v)!=1) exit(2);b.data[i]=(unsigned char)v;
    }
    return b;
}
static void hexbytes(const unsigned char *p, size_t n)
{ putchar('"');for (size_t i=0;i<n;++i) { printf("%02x",p[i]); }putchar('"'); }
static void delivery_id(const pgwt_codec_decoded *out, size_t index)
{
    const pgwt_codec_delivery *d=&out->deliveries[index];printf("\"%zu:",d->bytes.size);
    for (size_t i=0;i<32;++i) { printf("%02x",d->sha256[i]); }putchar('"');
}
static void file_id(pgwt_codec_identity id)
{
    putchar('[');json_event(stdout,id.boot);putchar(',');json_event(stdout,id.capture);printf(",%" PRIu64 "]",id.file_no);
}
static void counts_json(pgwt_evidence_counters c)
{
    if (!c.known) { fputs("null",stdout);return; }
    putchar('[');for (size_t i=0;i<15;++i) { printf("%s%" PRIu64,i ? "," : "",c.value[i]); }putchar(']');
}
static void optional_json(bool has, uint64_t v)
{ if (has) printf("%" PRIu64,v);else fputs("null",stdout); }
static void nullable_event(pgwt_core_event e)
{ if (e.length) json_event(stdout,e);else fputs("null",stdout); }
static void record_json(const pgwt_evidence_record *r, pgwt_codec_transition meta)
{
    static const char *const kinds[]={"OTHER","LIFE","ATTACH_OK","TRANS","FRONTIER","END","VOID"};
    printf("{\"kind\":\"%s\",\"t\":%" PRIu64,kinds[r->kind],r->t);
    if (r->kind==PGWT_EVIDENCE_LIFE || r->kind==PGWT_EVIDENCE_ATTACH_OK || r->kind==PGWT_EVIDENCE_TRANS || r->kind==PGWT_EVIDENCE_END)
        printf(",\"pid\":%u",r->pid);
    if (r->kind==PGWT_EVIDENCE_LIFE || r->kind==PGWT_EVIDENCE_ATTACH_OK || r->kind==PGWT_EVIDENCE_END) {
        fputs(",\"fork\":",stdout);optional_json(r->has_fork,r->fork);
    }
    switch (r->kind) {
    case PGWT_EVIDENCE_LIFE:
        printf(",\"start\":%" PRIu64 ",\"start_known\":%s,\"exit\":",r->start,r->start_known ? "true" : "false");optional_json(r->has_exit,r->exit);break;
    case PGWT_EVIDENCE_ATTACH_OK:
        printf(",\"method\":\"%s\",\"seed\":",r->reread ? "reread" : "transition");nullable_event(r->seed);fputs(",\"state\":",stdout);nullable_event(r->state);
        fputs(",\"counters\":",stdout);counts_json(r->counters);break;
    case PGWT_EVIDENCE_TRANS:
        fputs(",\"old\":",stdout);nullable_event(r->old_event);fputs(",\"new\":",stdout);nullable_event(r->new_event);
        printf(",\"duration\":%" PRIu64 ",\"query_id\":%" PRIu64 ",\"cpu_ns\":%" PRIu64,r->duration,meta.query_id,meta.cpu_ns);break;
    case PGWT_EVIDENCE_FRONTIER: fputs(",\"counters\":",stdout);counts_json(r->counters);break;
    case PGWT_EVIDENCE_END: fputs(",\"reason\":",stdout);json_event(stdout,r->end_reason);break;
    case PGWT_EVIDENCE_VOID:
        fputs(",\"counters\":",stdout);counts_json(r->counters);fputs(",\"reason\":",stdout);json_event(stdout,r->void_reason);
        fputs(",\"invalid_from\":",stdout);optional_json(r->has_invalid_from,r->invalid_from);break;
    default: break;
    }
    putchar('}');
}
static void stop_json(const pgwt_codec_decoded *out, const pgwt_codec_stop *s)
{
    fputs("{\"file\":",stdout);if (s->group==SIZE_MAX) fputs("null",stdout);else file_id(out->groups[s->group].identity);
    printf(",\"position\":[%zu,",s->block_offset);optional_json(s->has_record_offset,s->record_offset);
    printf("],\"reason\":\"%s\",\"witnesses\":[",pgwt_codec_code_name(s->code));
    for (size_t i=0;i<s->witness_count;++i) {
        if (i) { putchar(','); }pgwt_codec_witness w=s->witnesses[i];
        if (w.second!=SIZE_MAX) { putchar('['); }delivery_id(out,w.first);
        if (w.second!=SIZE_MAX) { putchar(',');delivery_id(out,w.second);putchar(']'); }
    }
    fputs("]}",stdout);
}
static void replay_stop_json(const pgwt_codec_decoded *out, const pgwt_codec_capture *c)
{
    if (!c->replay.stop.stopped) { fputs("null",stdout);return; }
    fputs("{\"file\":",stdout);file_id(out->groups[c->replay_group].identity);
    printf(",\"position\":[%zu,",c->replay_position.block_offset);
    optional_json(!c->replay.stop.descriptor,c->replay_position.record_offset);
    printf("],\"reason\":\"%s\"",pgwt_evidence_stop_label(c->replay.stop.reason));
    if (c->replay.stop.reason==PGWT_REPLAY_STOP_BASELINE_MISMATCH) {
        fputs(",\"expected\":",stdout);counts_json(c->replay.stop.expected_baseline);
        fputs(",\"declared\":",stdout);counts_json(c->replay.stop.declared_baseline);
    }
    putchar('}');
}
static void decoded_json(const pgwt_codec_decoded *out)
{
    printf("{\"aggregate_status\":\"%s\",\"unassigned_header_failure\":%s,\"records\":[",out->failed ? "FAILED" : "PASS",out->unassigned_header_failure ? "true" : "false");
    bool comma=false;
    for (size_t i=0;i<out->file_count;++i) for (size_t j=0;j<out->files[i].record_count;++j) {
        if (comma) { putchar(','); }comma=true;
        pgwt_codec_position p=out->info[i].positions[j];
        pgwt_codec_identity id={out->files[i].boot,out->files[i].capture,out->files[i].file_no};
        fputs("{\"physical\":[",stdout);json_event(stdout,id.boot);putchar(',');json_event(stdout,id.capture);
        printf(",%" PRIu64 ",%zu,%zu],\"value\":",id.file_no,p.block_offset,p.record_offset);
        record_json(&out->files[i].records[j],out->info[i].descriptor.transitions[j]);putchar('}');
    }
    fputs("],\"stops\":[",stdout);
    for (size_t i=0;i<out->stop_count;++i) { if (i) putchar(',');stop_json(out,&out->stops[i]); }
    fputs("],\"diagnostics\":[",stdout);
    for (size_t i=0;i<out->diagnostic_count;++i) { if (i) putchar(',');stop_json(out,&out->diagnostics[i]); }
    fputs("],\"admissions\":[",stdout);
    static const char *const links[]={"root","witnessed","unavailable","seal_conflict"};
    for (size_t i=0;i<out->group_count;++i) {
        const pgwt_codec_group *g=&out->groups[i];if (i) putchar(',');
        fputs("{\"file\":",stdout);file_id(g->identity);printf(",\"admitted\":%s,\"sources\":[",g->admitted ? "true" : "false");
        for (size_t j=0;j<g->source_count;++j) { if (j) putchar(',');delivery_id(out,g->sources[j]); }
        fputs("],\"links\":[",stdout);comma=false;
        for (size_t j=0;j<out->delivery_count;++j) if (out->deliveries[j].group==i) {
            const pgwt_codec_delivery *d=&out->deliveries[j];if (comma) { putchar(','); }comma=true;
            fputs("{\"delivery\":",stdout);delivery_id(out,j);printf(",\"status\":\"%s\",\"D\":%" PRIu64 ",\"digest\":",links[d->link],d->previous_data_end);hexbytes(d->previous_sha256,32);
            fputs(",\"checks\":[",stdout);
            for (size_t k=0;k<d->check_count;++k) {
                const pgwt_codec_link_check *c=&d->checks[k];if (k) putchar(',');fputs("{\"predecessor\":",stdout);delivery_id(out,c->predecessor);
                printf(",\"validated_end\":%zu,\"clean\":%s,\"static_match\":%s,\"boundary\":%s,\"digest_match\":",c->validated_end,c->clean ? "true":"false",c->static_match ? "true":"false",c->boundary ? "true":"false");
                fputs(!c->boundary ? "null" : c->digest_match ? "true":"false",stdout);
                printf(",\"exact_end\":%s,\"extension\":%s,\"witness\":%s,\"seal_proof\":%s}",c->exact_end ? "true":"false",c->extension ? "true":"false",c->witness ? "true":"false",c->seal_proof ? "true":"false");
            }
            fputs("]}",stdout);
        }
        fputs("]}",stdout);
    }
    fputs("],\"captures\":[",stdout);
    for (size_t i=0;i<out->capture_count;++i) {
        const pgwt_codec_capture *c=&out->captures[i];if (i) putchar(',');fputs("{\"capture\":",stdout);json_event(stdout,c->capture);
        printf(",\"status\":\"%s\",\"ambiguous\":%s,\"boots\":[",c->failed ? "FAILED":"PASS",c->ambiguous ? "true":"false");
        for (size_t j=0;j<c->boot_count;++j) { if (j) putchar(',');json_event(stdout,c->boots[j]); }
        fputs("],\"witnesses\":[",stdout);
        for (size_t j=0;j<c->witness_count;++j) { if (j) putchar(',');delivery_id(out,c->witnesses[j]); }
        fputs("],\"replay\":",stdout);
        if (!c->has_replay) fputs("null",stdout);
        else {
            const pgwt_evidence_capture_report *r=&c->replay;
            fputs("{\"frontier\":",stdout);optional_json(r->has_frontier,r->frontier);
            fputs(",\"high_water\":",stdout);optional_json(r->has_high_water,r->high_water);
            fputs(",\"baseline\":",stdout);counts_json(r->baseline);
            fputs(",\"lifecycle_uncertainty\":",stdout);optional_json(r->has_lifecycle_uncertainty,r->lifecycle_uncertainty);
            fputs(",\"frontiers\":[",stdout);for (size_t j=0;j<r->frontier_count;++j) printf("%s%" PRIu64,j ? ",":"",r->frontiers[j]);
            fputs("],\"revoked\":[",stdout);for (size_t j=0;j<r->revoked_frontier_count;++j) printf("%s%" PRIu64,j ? ",":"",r->revoked_frontiers[j]);
            fputs("],\"stop\":",stdout);replay_stop_json(out,c);
            fputs(",\"projections\":[",stdout);bool first=true;
            /* Query every physical LIFE identity; replay decides whether it
             * survives the effective semantic cutoff. Duplicate LIFE collapses. */
            for (size_t f=0;f<out->file_count;++f) if (bytes_equal(out->files[f].capture.data,out->files[f].capture.length,c->capture.data,c->capture.length))
                for (size_t j=0;j<out->files[f].record_count;++j) {
                    const pgwt_evidence_record *life=&out->files[f].records[j];if (life->kind!=PGWT_EVIDENCE_LIFE) continue;
                    bool seen=false;
                    for (size_t a=0;a<=f;++a) for (size_t b=0;b<out->files[a].record_count;++b) {
                        if (a==f && b>=j) break;
                        const pgwt_evidence_record *l=&out->files[a].records[b];
                        if (l->kind==PGWT_EVIDENCE_LIFE && bytes_equal(out->files[a].capture.data,out->files[a].capture.length,c->capture.data,c->capture.length) && l->pid==life->pid && l->has_fork==life->has_fork && (!l->has_fork || l->fork==life->fork)) seen=true;
                    }
                    if (seen) continue;
                    pgwt_evidence_input in={.files=out->files,.file_count=out->file_count,.capture=c->capture,.pid=life->pid,.has_fork=life->has_fork,.fork=life->fork};
                    pgwt_core_query q={.event={"w",1},.from=0,.to=15};pgwt_evidence_output o={0};
                    if (pgwt_evidence_replay(&in,&q,&o,NULL)!=PGWT_REPLAY_OK) exit(2);
                    if (!(o.projection.invalid_fields&PGWT_CORE_LIFE_INVALID)) {
                        if (!first) { putchar(','); }first=false;
                        printf("{\"pid\":%u,\"fork\":",life->pid);optional_json(life->has_fork,life->fork);fputs(",\"projection\":",stdout);projection_json(stdout,&o.projection);putchar('}');
                    }
                    pgwt_evidence_output_free(&o);
                }
            fputs("]}",stdout);
        }
        putchar('}');
    }
    fputs("]}\n",stdout);
}
static void encode_request(const json_node *request)
{
    const json_node *h=member(request,"header"),*rows=member(request,"records");
    pgwt_evidence_file f={.version=5,.present=true};
    event_string(member(h,"boot"),&f.boot);event_string(member(h,"capture"),&f.capture);
    f.file_no=number(member(h,"file_no"));f.has_previous=f.file_no!=0;f.previous=f.file_no ? f.file_no-1 : 0;
    boolean(member(h,"qualified"),&f.qualified);counters(member(h,"baseline"),&f.baseline);
    size_t n=list_length(rows);pgwt_evidence_record *rs=calloc(n+1,sizeof(*rs));pgwt_codec_transition *meta=calloc(n+1,sizeof(*meta));
    if (!rs || !meta) exit(2);
    for (size_t i=0;i<n;++i) {
        const json_node *r=item(rows,i);record(r,&rs[i]);rs[i].unsupported_shape=false;
        const json_node *qid=member(r,"query_id"),*cpu=member(r,"cpu_ns");
        if (qid) { meta[i].query_id=number(qid); }if (cpu) { meta[i].cpu_ns=number(cpu); }
    }
    f.records=rs;f.record_count=n;
    const json_node *maps=member(h,"mapping");size_t nm=list_length(maps);pgwt_codec_mapping *map=calloc(nm+1,sizeof(*map));if (!map) exit(2);
    for (size_t i=0;i<nm;++i) { const json_node *m=item(maps,i);map[i].id=(uint32_t)number(item(m,0));map[i].role=(unsigned)number(item(m,1));event_string(item(m,2),&map[i].name); }
    pgwt_codec_descriptor d={.mapping=map,.mapping_count=nm,.transitions=meta,.block_records=4096,
        .flags=(uint16_t)number(member(h,"flags")),.start_time_ns=number(member(h,"creation"))};
    pgwt_codec_bytes prev={0};pgwt_codec_buffer previous={0};
    if (f.file_no) { prev=unhex(member(request,"previous"));previous=(pgwt_codec_buffer){prev.data,prev.size}; }
    pgwt_codec_bytes b={0};const char *why=NULL;
    if (pgwt_codec_encode(&f,&d,f.file_no ? &previous : NULL,&b,&why)) { fprintf(stderr,"encode: %s\n",why);exit(2); }
    hexbytes(b.data,b.size);putchar('\n');pgwt_codec_bytes_free(&b);pgwt_codec_bytes_free(&prev);free(rs);free(meta);free(map);
}
int main(void)
{
    char *line=NULL;size_t capacity=0;
    while (getline(&line,&capacity,stdin)>0) {
        char err[128];char *owned=strdup(line);if (!owned) return 2;
        json_document *d=parse_document(owned,err,sizeof(err));if (!d) { fprintf(stderr,"%s\n",err);return 2; }
        if (member(d->root,"header")) encode_request(d->root);
        else {
            const json_node *ds=member(d->root,"buffers");size_t n=list_length(ds);
            pgwt_codec_bytes *bs=calloc(n+1,sizeof(*bs));pgwt_codec_buffer *buffers=calloc(n+1,sizeof(*buffers));if (!bs || !buffers) return 2;
            for (size_t i=0;i<n;++i) { bs[i]=unhex(item(ds,i));buffers[i]=(pgwt_codec_buffer){bs[i].data,bs[i].size}; }
            pgwt_codec_decoded out={0};int rc=pgwt_codec_decode(buffers,n,&out);
            if (rc<0) { fputs("decode API error\n",stderr);return 2; }
            decoded_json(&out);pgwt_codec_decoded_free(&out);
            for (size_t i=0;i<n;++i) { pgwt_codec_bytes_free(&bs[i]); }free(bs);free(buffers);
        }
        free_document(d);fflush(stdout);
    }
    free(line);return ferror(stdin) ? 2 : 0;
}
