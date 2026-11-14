/* Frozen answers stay in contracts/. These tests supply bytes and schedules. */
#include "../src/percpu_transport.h"
#include "../src/merge_capture.h"
#include "../src/daemon.h"
#include "../src/counter_vector.h"
/* Exercise the real 13-lookup vector reader with an injected map syscall. */
#include "../src/counter_vector.c"
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void tb_le(unsigned char *p, uint64_t v, size_t n)
{ for (size_t i=0;i<n;++i) p[i]=(unsigned char)(v>>(8*i)); }
static uint64_t tb_read(const unsigned char *p,size_t n)
{uint64_t v=0;for(size_t i=0;i<n;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
static size_t tb_packet(unsigned char *p, unsigned kind, unsigned cpu, uint64_t seq,
                         uint64_t cookie, const void *payload, size_t len)
{
    memset(p,0,32+len); tb_le(p,1,2); p[2]=(unsigned char)kind;
    tb_le(p+4,len,2); tb_le(p+6,cpu,2); tb_le(p+8,seq,8); tb_le(p+16,cookie,8);
    if (len) memcpy(p+32,payload,len);
    return 32+len;
}
#define PGWT_ADAPTER_PRODUCTION_TEST 1
#define PGWT_ADAPTER_EMBEDDED 1
#include "test_core_merge_adapter.c"
#undef ma_create
#undef ma_destroy
#undef pgwt_merge_create
#undef pgwt_merge_destroy
#undef ma_map

static const char *tb_guard;
#define TB(expr) do { if (!(expr)) { fprintf(stderr,"transport ASSERT %s: %s (line %d)\n",tb_guard,#expr,__LINE__); return false; } } while (0)
struct tb_driver {
    struct pgwt_percpu_transport *transport;
    struct pgwt_merge_capture *capture;
    ma_snapshot snapshot;
    uint64_t attempts[6], seq[6], now, trace_t;
    /* Producer accounting and shared-ring byte positions ([0]=E, [1]=L) as
     * the kernel would report them; every schedule sets them explicitly. */
    uint64_t failures[6], discards[6], ring_produced[2], ring_consumed[2];
    unsigned failure_reads, position_reads;
    bool position_error, accounting_error;
    unsigned controls, wakes, reads, drains, lifecycle, traces, topology_reads;
    struct pgwt_transport_ctl ctl;
    pgwt_merge_output outputs[64];
    unsigned char payloads[64][272];
    size_t count;
    bool topology, reject_data, reject_void, drained, prove, support, read_error, mutate_after_data;
    unsigned prepared, configured, opened, closed, consumed;
    int received_error, lifecycle_error;
    bool count_maps;
    size_t map_lookups, first_data_lookups, last_data_lookups, data_outputs, observes;
    unsigned shutdown_phase;
};
static struct {struct pgwt_percpu_transport *transport;struct pgwt_merge_capture *capture;} tb_live[64];
static int tb_prepare(void *ctx){++((struct tb_driver *)ctx)->prepared;return 0;}
static int tb_configure(void *ctx)
{struct tb_driver *d=ctx;++d->configured;return d->support?0:-ENOTSUP;}
static int tb_open(void *ctx){++((struct tb_driver *)ctx)->opened;return 0;}
static int tb_epoll(void *ctx){(void)ctx;return 17;}
static uint32_t tb_tgid(void *ctx){(void)ctx;return 1234;}
static void tb_close(void *ctx){++((struct tb_driver *)ctx)->closed;}
static int tb_consume(void *ctx,uint32_t dest,bool bounded)
{(void)bounded;if(dest>2)return -EINVAL;++((struct tb_driver *)ctx)->consumed;return 0;}
static uint64_t tb_now(void *ctx) { return ((struct tb_driver *)ctx)->now; }
static int tb_topology(void *ctx)
{ struct tb_driver *d=ctx;++d->topology_reads;return d->topology ? 1:0; }
static int tb_control(void *ctx,const struct pgwt_transport_ctl *c)
{ struct tb_driver *d=ctx; ++d->controls; d->ctl=*c; return 0; }
static int tb_wake(void *ctx,uint32_t cpu)
{ struct tb_driver *d=ctx; if(cpu>1) return -EINVAL; ++d->wakes; return 0; }
static int tb_attempts(void *ctx,uint64_t *v,size_t n)
{ if(n!=6)return -EINVAL; memcpy(v,((struct tb_driver *)ctx)->attempts,sizeof(uint64_t)*6);return 0; }
static int tb_failures(void *ctx,uint64_t *v,size_t n)
{ struct tb_driver *d=ctx;if(n!=6)return -EINVAL;++d->failure_reads;if(d->accounting_error)return -EIO;
  memcpy(v,d->failures,sizeof(uint64_t)*6);return 0; }
static int tb_discards(void *ctx,uint64_t *v,size_t n)
{ if(n!=6)return -EINVAL; memcpy(v,((struct tb_driver *)ctx)->discards,sizeof(uint64_t)*6);return 0; }
static int tb_positions(void *ctx,uint64_t produced[2],uint64_t consumed[2])
{
    struct tb_driver *d=ctx;++d->position_reads;
    if(d->position_error)return -EIO;
    memcpy(produced,d->ring_produced,sizeof(d->ring_produced));memcpy(consumed,d->ring_consumed,sizeof(d->ring_consumed));return 0;
}
static struct tb_driver *tb_lookup_driver;
static int tb_map_lookup(int fd,const void *key,void *out)
{
    (void)fd;(void)key;++tb_lookup_driver->map_lookups;
    memset(out,0,2*sizeof(uint64_t));return 0;
}
static int tb_counters(void *ctx,ma_snapshot *s)
{
    struct tb_driver *d=ctx;++d->reads;*s=d->snapshot;
    if(d->count_maps){
        tb_lookup_driver=d;
        struct pgwt_counter_sources sources={.fail_fd=1,.site_fd=2,.event_ring_fd=3,.ncpu=2,
            .writer_push_fail_total=d->snapshot.value[14]};
        struct pgwt_counter_vector v=pgwt_read_counter_vector(&sources,tb_map_lookup);
        s->valid=v.valid;
        for(size_t i=0;i<15;++i){s->value[i]=v.components[i].value;s->component_valid[i]=v.components[i].valid;}
    }
    return d->read_error?-EIO:0;
}
static int tb_emit(struct tb_driver *,unsigned,unsigned,unsigned,uint64_t,const void *,size_t);
static int tb_boundary(struct tb_driver *,unsigned,unsigned,unsigned,uint64_t);
static int tb_drain(void *ctx)
{
    struct tb_driver *d=ctx;++d->drains;
    if(d->shutdown_phase==1){
        /* These bytes have not reached userspace when finish() is called.
         * Model a held perf buffer plus its lifecycle ring. */
        unsigned char fork[24]={0},trace[48];
        tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,10,8);
        memcpy(trace,golden_T,48);tb_le(trace,d->trace_t,8);tb_le(trace+8,30,4);
        if(tb_emit(d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)<0 ||
            tb_emit(d,0,0,PGWT_WIRE_TRACE,123,trace,48)<0)return -EIO;
        d->shutdown_phase=2;return 0;
    }
    if(d->shutdown_phase==2 && d->ctl.el_request){
        if(tb_boundary(d,0,1,1,13)<0 || tb_boundary(d,1,2,1,13)<0)return -EIO;
        d->shutdown_phase=3;return 0;
    }
    if(d->shutdown_phase==3 && d->ctl.round==2){
        if(tb_boundary(d,0,0,2,14)<0 || tb_boundary(d,1,0,2,14)<0)return -EIO;
        d->shutdown_phase=4;return 0;
    }
    return d->drained?1:0;
}
static int tb_received(void *ctx,const ma_envelope *e,uint64_t now)
{
    struct tb_driver *d=ctx;
    return d->received_error ? d->received_error : pgwt_capture_received(d->capture,e,now);
}
static int tb_envelope(void *ctx,const ma_envelope *e,uint64_t now,ma_snapshot *s)
{
    struct pgwt_merge_capture *c=((struct tb_driver *)ctx)->capture;
    int rc=pgwt_capture_envelope(c,e,now,s);*s=pgwt_capture_last_snapshot(c);return rc;
}
static int tb_fault(void *ctx,pgwt_merge_reason reason,uint64_t now,const ma_snapshot *s)
{ return pgwt_capture_fault(((struct tb_driver *)ctx)->capture,reason,now,s); }
static bool tb_sink(void *ctx,const pgwt_merge_output *out,pgwt_merge_observation *post)
{
    struct tb_driver *d=ctx; (void)post;
    if ((out->kind==PGWT_MERGE_OUT_DATA && out->record.rank==PGWT_MERGE_TRANS && d->reject_data) ||
        (out->kind==PGWT_MERGE_OUT_VOID && d->reject_void)) return false;
    if(d->count==64 || out->record.payload_size>272)return false;
    if(out->kind==PGWT_MERGE_OUT_DATA && out->record.rank==PGWT_MERGE_TRANS){
        if(!d->data_outputs)d->first_data_lookups=d->map_lookups;
        d->last_data_lookups=d->map_lookups;++d->data_outputs;
    }
    d->outputs[d->count]=*out;
    if(out->record.payload_size)memcpy(d->payloads[d->count],out->record.payload,out->record.payload_size);
    d->outputs[d->count].record.payload=d->payloads[d->count]; ++d->count;
    return true;
}
static int tb_trace(void *ctx,const void *bytes,size_t n)
{
    (void)bytes; if(n!=48)return -EINVAL;
    struct tb_driver *d=ctx;++d->traces;
    if(d->mutate_after_data)d->snapshot.value[14]=1;
    return 0;
}
static int tb_post_observe(void *ctx,pgwt_merge_observation *o)
{
    struct tb_driver *d=ctx;++d->observes;
    o->d=d->now;
    o->counters.value[14]=d->snapshot.value[14];
    return 0;
}
static void tb_writer_failed(void *ctx){++((struct tb_driver *)ctx)->snapshot.value[14];}
static int tb_lifecycle(void *ctx,const void *bytes,size_t n)
{
    struct tb_driver *d=ctx; const unsigned char *p=bytes; ++d->lifecycle;
    if(d->lifecycle_error)return d->lifecycle_error;
    if(n==24 && p[0]==1 && d->prove) {
        struct pgwt_attach_report r={.pid=30,.cookie=123,.event=MA_BEGIN};
        if(pgwt_capture_attach(d->capture,&r,d->now,&d->snapshot)<0)return -EIO;
        r.event=MA_COMPLETE;r.facts_known=r.facts_true=MA_ALL_FACTS;
        return pgwt_capture_attach(d->capture,&r,d->now,&d->snapshot);
    }
    return 0;
}
static bool tb_setup(struct tb_driver *d)
{
    memset(d,0,sizeof(*d));d->snapshot=readable();d->topology=d->drained=d->prove=d->support=true;d->now=20;d->trace_t=12;
    struct pgwt_capture_config cfg={.cpu_count=2,.capacity=128,.ctx=d,
        .baseline={.counters={.known=true}},.lifecycle=tb_lifecycle,.trace=tb_trace,
        .observe=tb_post_observe,.writer_failed=tb_writer_failed,
        .sink={.emit=tb_sink,.ctx=d}};
    d->capture=pgwt_capture_create(&cfg);TB(d->capture);
    struct pgwt_transport_io io={.ctx=d,.prepare=tb_prepare,.configure=tb_configure,.open=tb_open,
        .epoll_fd=tb_epoll,.helper_tgid=tb_tgid,.close=tb_close,.consume=tb_consume,.now=tb_now,.cpu_set_unchanged=tb_topology,
        .control=tb_control,.wake=tb_wake,.attempts=tb_attempts,.failures=tb_failures,
        .discards=tb_discards,.positions=tb_positions,.counters=tb_counters,
        .drain=tb_drain,.received=tb_received,.envelope=tb_envelope,.fault=tb_fault};
    uint32_t cpus[]={0,1};
    d->transport=pgwt_transport_create(cpus,2,128,&io);
    for(size_t i=0;i<64;++i)if(!tb_live[i].capture){tb_live[i].capture=d->capture;tb_live[i].transport=d->transport;break;}
    TB(d->transport);return true;
}
static void tb_cleanup(struct tb_driver *d)
{
    for(size_t i=0;i<64;++i)if(tb_live[i].capture==d->capture){tb_live[i].capture=NULL;tb_live[i].transport=NULL;}
    pgwt_transport_destroy(d->transport);pgwt_capture_destroy(d->capture);
}
static int tb_emit(struct tb_driver *d,unsigned cpu,unsigned dest,unsigned kind,
                     uint64_t cookie,const void *p,size_t n)
{
    unsigned char packet[304];size_t k=cpu*3+dest;
    uint64_t seq=++d->seq[k];d->attempts[k]=seq;
    size_t len=tb_packet(packet,kind,cpu,seq,cookie,p,n);
    return pgwt_transport_record(d->transport,dest,packet,len);
}
/* FABRICATED: no kernel produced this boundary. On Linux only a registered,
 * pinned helper thread's getppid tracepoint publishes one, and only the
 * configured issuer thread publishes E/L, once per new request token. The Mac
 * cannot show that the daemon makes that happen; the registered live test
 * tests/test_transport_daemon_live.py runs the real path (helper registration,
 * issuer thread, token, both boundaries, every CPU's acknowledgement). */
static int tb_boundary(struct tb_driver *d,unsigned cpu,unsigned dest,unsigned round,uint64_t w)
{
    unsigned char p[16]={0};tb_le(p,w,8);tb_le(p+8,round,4);tb_le(p+12,dest,4);
    return tb_emit(d,cpu,dest,PGWT_WIRE_BOUNDARY,0,p,sizeof(p));
}
static bool tb_begin(struct tb_driver *d)
{
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,10,8);
    TB(tb_emit(d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    TB(d->lifecycle==1); /* attachment runs at consumption, before completion */
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,d->trace_t,8);tb_le(trace+8,30,4);
    TB(tb_emit(d,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);
    TB(pgwt_transport_poll(d->transport,false)==0);
    TB(d->controls==1 && d->wakes==2 && d->ctl.round==1 && d->ctl.el_request==1);
    TB(tb_boundary(d,0,1,1,13)==0);TB(tb_boundary(d,1,2,1,13)==0);
    TB(pgwt_transport_poll(d->transport,false)==0);
    TB(d->controls==2 && d->wakes==4 && d->ctl.round==2 && !d->ctl.el_request);
    TB(d->reads==1 && !d->count);return true;
}
static bool tb_all_acks(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    d.now=40;TB(pgwt_transport_poll(d.transport,false)==0);
    /* Completion needs no drain of its own and no position read. */
    TB(d.reads==2 && d.drains==0 && !d.position_reads && d.count==3);
    TB(d.outputs[0].record.rank==PGWT_MERGE_FORK);
    TB(d.outputs[1].kind==PGWT_MERGE_OUT_DATA && d.outputs[1].record.g==123 &&
       d.outputs[1].record.pid==30 && !d.outputs[1].prior_interval_certified);
    TB(d.outputs[2].kind==PGWT_MERGE_OUT_FRONTIER && d.outputs[2].F==12);
    TB(d.traces==1 && pgwt_transport_max_held_ns(d.transport)==20 &&
       pgwt_capture_max_held_ns(d.capture,40)==20);
    TB(!pgwt_capture_waiting(d.capture));
    TB(pgwt_transport_poll(d.transport,false)==0 && d.controls==2 && d.wakes==4);
    tb_cleanup(&d);return true;
}
static bool tb_missing_ack(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
    TB(tb_boundary(&d,0,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.reads==1 && d.drains==0 && d.count==0);
    TB(pgwt_transport_poll(d.transport,true)==0 && d.controls==2); /* coalesced */
    TB(pgwt_transport_finish(d.transport)==0);
    TB(d.count==1 && d.outputs[0].kind==PGWT_MERGE_OUT_VOID);
    tb_cleanup(&d);return true;
}
static bool tb_stale_ack(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,1,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.reads==1 && d.drains==0 && d.count==0);
    /* A second acknowledgement from the same CPU is not the other CPU's. */
    TB(tb_boundary(&d,0,0,2,15)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.reads==1 && d.count==0);
    TB(tb_boundary(&d,1,0,2,15)==0);
    TB(pgwt_transport_poll(d.transport,false)==0 && d.outputs[d.count-1].F==12);
    tb_cleanup(&d);return true;
}
static bool tb_counter_timing(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
    TB(tb_boundary(&d,0,0,2,14)==0);TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.reads==1 && !d.count);
    /* The failure becomes visible between the first and final CPU ack. */
    d.snapshot.value[11]=1;
    TB(tb_boundary(&d,1,0,2,14)==0);TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && d.outputs[0].kind==PGWT_MERGE_OUT_VOID &&
        d.outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE && d.outputs[0].counters.value[11]==1);
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    tb_cleanup(&d);return true;
}
static bool tb_post_snapshot(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));d.mutate_after_data=true;
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    TB(tb_boundary(&d,0,0,2,15)==0); /* another envelope after publication */
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==3 && d.outputs[0].record.rank==PGWT_MERGE_FORK &&
       d.outputs[1].kind==PGWT_MERGE_OUT_DATA && d.outputs[2].kind==PGWT_MERGE_OUT_VOID &&
       d.outputs[2].reason==PGWT_MERGE_COUNTER_CHANGE && d.outputs[2].counters.value[14]==1);
    TB(pgwt_capture_report(d.capture).faults[PGWT_MERGE_COUNTER_UNKNOWN]==0);
    tb_cleanup(&d);return true;
}
static bool tb_core_held_age(void)
{
    struct tb_driver d;TB(tb_setup(&d));d.trace_t=13;TB(tb_begin(&d));
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    d.now=40;TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==2 && d.outputs[0].record.rank==PGWT_MERGE_FORK &&
       d.outputs[1].kind==PGWT_MERGE_OUT_FRONTIER && d.outputs[1].F==12 && !d.traces);
    TB(pgwt_capture_waiting(d.capture) && pgwt_capture_max_held_ns(d.capture,100)==80);
    struct pgwt_daemon *daemon=calloc(1,sizeof(*daemon));TB(daemon);
    daemon->exact_perf=true;daemon->percpu_transport=d.transport;daemon->merge_capture=d.capture;
    /* Queue has emptied, but the core still holds the one TRANS at t=F+1. */
    TB(pgwt_transport_pending_records(d.transport)==0 && pgwt_capture_pending_records(d.capture)==1 &&
       pgwt_merge_capture_pending_records(daemon)==1);
    free(daemon);
    TB(pgwt_capture_finish(d.capture,100,&d.snapshot)==0 && d.count==3 &&
       d.outputs[2].kind==PGWT_MERGE_OUT_VOID && pgwt_capture_max_held_ns(d.capture,100)==80);
    tb_cleanup(&d);return true;
}
static bool tb_attempt_window(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
    /* A later attempt is outside the completed E/L prefix. Do not compare
     * its growing total against a snapshot of earlier consumption. */
    ++d.attempts[1];++d.seq[1];
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0 && d.count==3 && d.outputs[2].F==12);
    tb_cleanup(&d);return true;
}
static bool tb_late_fork(void)
{
    struct tb_driver d;TB(tb_setup(&d));
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,12,8);tb_le(trace+8,30,4);
    TB(tb_emit(&d,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,10,8);
    TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(tb_boundary(&d,0,1,1,13)==0);TB(tb_boundary(&d,1,2,1,13)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0 && d.count==3 && d.outputs[2].F==12);
    TB(d.outputs[0].record.rank==PGWT_MERGE_FORK && d.outputs[1].record.g==123);
    tb_cleanup(&d);return true;
}
static bool tb_unknown_init(void)
{
    struct tb_driver d;TB(tb_setup(&d));d.prove=false;TB(tb_begin(&d));
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && d.outputs[0].kind==PGWT_MERGE_OUT_VOID &&
       d.outputs[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE && !d.traces);
    tb_cleanup(&d);return true;
}
static bool tb_unknown(void)
{
    for(size_t i=0;i<17;++i) {
        struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
        if(i<15)d.snapshot.component_valid[i]=false;else if(i==15)d.snapshot.valid=false;else d.read_error=true;
        TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
        TB(pgwt_transport_poll(d.transport,false)==0);
        TB(d.count==1 && d.outputs[0].kind==PGWT_MERGE_OUT_VOID &&
            d.outputs[0].reason==PGWT_MERGE_COUNTER_UNKNOWN && !d.outputs[0].counters.known);
        tb_cleanup(&d);
    }
    return true;
}
static bool tb_gaps(void)
{
    for(unsigned dest=0;dest<3;++dest) {
        struct tb_driver d;TB(tb_setup(&d));
        d.seq[dest]=1;
        TB(tb_boundary(&d,0,dest,1,13)==0);
        TB(d.count==1 && d.outputs[0].kind==PGWT_MERGE_OUT_VOID && d.outputs[0].reason==PGWT_MERGE_SEQUENCE_GAP);
        tb_cleanup(&d);
    }
    /* A missing final attempt has no subsequent seq to reveal its hole. */
    struct tb_driver d;TB(tb_setup(&d));
    unsigned char trace[48];memcpy(trace,golden_T,48);
    TB(tb_emit(&d,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(tb_boundary(&d,0,1,1,13)==0);TB(tb_boundary(&d,1,2,1,13)==0);
    ++d.attempts[1]; /* failed E attempt before the fence */
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && d.outputs[0].reason==PGWT_MERGE_SEQUENCE_GAP);
    tb_cleanup(&d);return true;
}
static bool tb_writer(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));d.reject_data=true;
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==2 && d.outputs[0].record.rank==PGWT_MERGE_FORK && d.outputs[1].kind==PGWT_MERGE_OUT_VOID &&
        d.outputs[1].reason==PGWT_MERGE_WRITER_PUSH_FAIL && d.outputs[1].counters.value[14]==1);
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    tb_cleanup(&d);
    TB(tb_setup(&d));TB(tb_begin(&d));d.reject_data=d.reject_void=true;
    TB(tb_boundary(&d,0,0,2,14)==0);TB(tb_boundary(&d,1,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==-EIO && pgwt_capture_report(d.capture).void_failed);
    TB(d.count==1 && d.outputs[0].record.rank==PGWT_MERGE_FORK);
    tb_cleanup(&d);return true;
}
static bool tb_decode_and_lost(void)
{
    uint32_t cpus[]={2,7};struct pgwt_wire_record r;
    unsigned char packet[305],payload[48];memcpy(payload,golden_T,48);
    size_t n=tb_packet(packet+1,PGWT_WIRE_TRACE,7,1,123,payload,48);
    TB(pgwt_transport_decode(cpus,2,0,8,packet+1,n,&r)==0 && r.envelope.source==1 && r.envelope.position==8);
    packet[1]=2;TB(pgwt_transport_decode(cpus,2,0,8,packet+1,n,&r)<0);packet[1]=1;
    packet[5]=47;TB(pgwt_transport_decode(cpus,2,0,8,packet+1,n,&r)<0);packet[5]=48;
    packet[4]=1;TB(pgwt_transport_decode(cpus,2,0,8,packet+1,n,&r)<0);packet[4]=0;
    struct tb_driver d;TB(tb_setup(&d));
    unsigned char raw[88]={0};tb_le(raw,84,4);tb_packet(raw+4,PGWT_WIRE_TRACE,0,1,123,payload,48);
    TB(pgwt_transport_perf(d.transport,0,raw,88)==0);
    TB(pgwt_transport_lost(d.transport,1,2)==0 && d.count==1 && d.outputs[0].reason==PGWT_MERGE_PERF_LOST);
    uint64_t lost;TB(pgwt_transport_lost_total(d.transport,&lost) && lost==2);
    TB(pgwt_transport_poll(d.transport,true)==0 && !d.controls);
    tb_cleanup(&d);return true;
}
static bool tb_shutdown_and_topology(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
    d.topology=false;TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && d.outputs[0].kind==PGWT_MERGE_OUT_VOID);
    unsigned wakes=d.wakes;TB(pgwt_transport_poll(d.transport,true)==0 && d.wakes==wakes);
    tb_cleanup(&d);
    TB(tb_setup(&d));TB(tb_begin(&d));
    TB(pgwt_transport_finish(d.transport)==0 && d.count==1 && d.outputs[0].kind==PGWT_MERGE_OUT_VOID);
    TB(pgwt_transport_finish(d.transport)==0 && d.count==1);
    tb_cleanup(&d);
    TB(tb_setup(&d));d.received_error=-ENOMEM;
    TB(tb_boundary(&d,0,0,1,13)==0 && d.count==1 && d.outputs[0].reason==PGWT_MERGE_ALLOCATION_FAIL);
    tb_cleanup(&d);
    TB(tb_setup(&d));d.lifecycle_error=-EIO;
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,10,8);
    TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    TB(d.count==1 && d.outputs[0].reason==PGWT_MERGE_WRITER_PUSH_FAIL && d.outputs[0].counters.value[14]==1);
    tb_cleanup(&d);return true;
}
static bool tb_shutdown_raw(void)
{
    struct tb_driver d;TB(tb_setup(&d));d.shutdown_phase=1;
    TB(!d.traces && !d.count && !d.controls);
    TB(pgwt_transport_finish(d.transport)==0);
    TB(d.shutdown_phase==4 && d.drains>=4 && d.traces==1 && d.count==3 && d.controls==2);
    TB(d.outputs[1].kind==PGWT_MERGE_OUT_DATA && d.outputs[2].kind==PGWT_MERGE_OUT_FRONTIER &&
        pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    unsigned drains=d.drains;
    TB(pgwt_transport_finish(d.transport)==0 && d.drains==drains);
    tb_cleanup(&d);
    TB(tb_setup(&d));d.drained=false;
    TB(pgwt_transport_finish(d.transport)==0 && d.drains==256 && d.count==1 &&
        d.outputs[0].kind==PGWT_MERGE_OUT_VOID && d.outputs[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    tb_cleanup(&d);
    TB(tb_setup(&d));d.snapshot.value[11]=1;
    TB(pgwt_capture_finish(d.capture,40,&d.snapshot)==0 && d.count==1 &&
        d.outputs[0].kind==PGWT_MERGE_OUT_VOID && d.outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE);
    tb_cleanup(&d);
    printf("shutdown: unread perf DATA drained and released; nonempty/BUSY suffix explicitly VOID; final counters checked\n");
    return true;
}
static bool tb_off(void)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TB(d);
    TB(pgwt_percpu_transport_prepare(d)==0 && pgwt_percpu_transport_configure(d)==0 &&
       pgwt_percpu_transport_open(d)==0 && pgwt_percpu_transport_poll(d)==0 &&
       pgwt_percpu_transport_snapshot(d,NULL,NULL)==0 &&
       pgwt_percpu_transport_consume(d,1,true)==0 && pgwt_percpu_transport_record(d,0,NULL,0)==0);
    TB(pgwt_merge_capture_open(d)==0 && pgwt_merge_capture_poll(d)==0 && pgwt_merge_capture_tick(d)==0 &&
       pgwt_merge_capture_finish(d)==0 && pgwt_merge_capture_record(d,0,NULL,0)==0 && pgwt_merge_capture_attach(d,NULL)==0);
    TB(!d->percpu_transport && !d->merge_capture && !d->attach_report);
    pgwt_percpu_transport_close(d);pgwt_merge_capture_close(d);free(d);return true;
}
static bool tb_injected_kernel(void)
{
    struct tb_driver driver;TB(tb_setup(&driver));
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TB(d);
    d->exact_perf=true;d->percpu_transport=driver.transport;d->merge_capture=driver.capture;
    TB(pgwt_percpu_transport_prepare(d)==0 && pgwt_percpu_transport_prepare(d)==0 && driver.prepared==1);
    TB(pgwt_percpu_transport_helper_tgid(d)==1234 && pgwt_percpu_transport_cpu_count(d)==2);
    driver.support=false;
    TB(pgwt_percpu_transport_configure(d)==-ENOTSUP && pgwt_percpu_transport_open(d)==-ENOTSUP && !driver.opened);
    driver.support=true;
    TB(pgwt_percpu_transport_configure(d)==0 && pgwt_percpu_transport_open(d)==0 && driver.opened==1);
    ma_snapshot s;uint64_t now=0;driver.snapshot.value[11]=7;
    TB(pgwt_percpu_transport_snapshot(d,&s,&now)==0 && driver.reads==2 && now==20 && s.valid && s.value[11]==7);
    driver.read_error=true;
    TB(pgwt_percpu_transport_snapshot(d,&s,&now)==0 && !s.valid);driver.read_error=false;
    TB(pgwt_percpu_transport_epoll_fd(d)==17 && pgwt_percpu_transport_poll(d)==0);
    TB(!pgwt_transport_silent(driver.transport,29,10) && pgwt_transport_silent(driver.transport,30,10));
    driver.now=40;TB(tb_boundary(&driver,0,1,1,13)==0);
    TB(!pgwt_transport_silent(driver.transport,39,10) && !pgwt_transport_silent(driver.transport,49,10) &&
        pgwt_transport_silent(driver.transport,50,10) && !driver.controls && !driver.wakes);
    TB(pgwt_percpu_transport_consume(d,1,true)==0 && driver.consumed==1);
    pgwt_percpu_transport_close(d);TB(driver.closed==1 && !d->percpu_transport);
    driver.transport=NULL;tb_cleanup(&driver);free(d);return true;
}
static bool tb_admission(void)
{
    ma_source_decl sources[4];
    for(size_t i=0;i<4;++i)sources[i]=(ma_source_decl){.mechanism=i<2 ? MA_WATCHPOINT_PERF:MA_ORDERED_RING,.qualification_complete=true};
    ma_config cfg={.mode=PGWT_MERGE_N,.source_count=4,.sources=sources};
    ma *a=ma_create_production(&cfg,NULL);TB(a);ma_destroy(a);
    cfg.synthetic_premises=true;TB(!ma_create_production(&cfg,NULL));
    cfg.synthetic_premises=false;
    for(size_t i=0;i<4;++i) {sources[i].qualification_complete=false;TB(!ma_create_production(&cfg,NULL));sources[i].qualification_complete=true;}
    struct tb_driver d;TB(tb_setup(&d));tb_cleanup(&d);return true;
}
#ifdef PGWT_TRANSPORT_HOST_TEST
/* The exact same frozen-output comparator as the adapter migration replay;
 * expected answers are never derived from the transport or adapter. */
static bool tb_production_row(const json_node *root, const json_node *row, const char *mode,
                         unsigned cookie_test)
{
    replay_capture c={.f={.root=root,.row=row},.bytes_ok=true};
    premises p; MUST(setup(&p,!strcmp(mode,"N") ? PGWT_MERGE_N : PGWT_MERGE_Q));
    const json_node *start=member(row,"start"), *inputs=member(row,"inputs");
    const json_node *expected=member(member(row,"expected"),mode);
    MUST(list_length(inputs)<=256);
    for (const json_node *n=inputs->child;n;n=n->next) {
        if (string_is(member(n,"kind"),"WRITER_REJECT")) c.f.reject=n;
        if (string_is(member(n,"kind"),"DATA") || string_is(member(n,"kind"),"TEXT"))
            (void)make_wire(&c,&p,n);
        if (member(n,"fork_seed_proved") && boolean(member(n,"fork_seed_proved"))) {
            const json_node *id=identity(&c.f,string_value(member(n,"who")));
            for (size_t i=0;i<p.table.lifetime_count;++i)
                if (p.lifetimes[i].pid==number(member(id,"pid")) &&
                    p.lifetimes[i].g==number(member(id,"g"))) qualified_lifetime(&p.lifetimes[i]);
        }
    }
    for (size_t s=0;s<4;++s)
        p.sources[s].initial_cutoff=optional_number(member(member(start,"cutoffs"),
            string_value(item(member(root,"sources"),s))),0);
    tracked_adapter_destroy(p.adapter); p.adapter=tracked_adapter_create(&p.config,NULL); MUST(p.adapter);
    pgwt_merge_attachment proofs[2];
    const char *const attached[]={"A","B"};
    for (size_t i=0;i<2;++i) {
        const json_node *id=identity(&c.f,attached[i]);
        proofs[i]=(pgwt_merge_attachment){.pid=number(member(id,"pid")),.g=number(member(id,"g")),
            .next_q=optional_number(member(member(start,"next_q"),attached[i]),1),
            .identity_proved=true,.sequence_proved=true};
    }
    pgwt_merge_config base={.staged_capacity=128,.payload_capacity=272,.attachment_capacity=32,
        .attachments=proofs,.attachment_count=2,.sink=wire_sink,.sink_context=&c,
        .observation={.counters=named_counters(&c.f,"C0")},.last_observed=named_counters(&c.f,"C0")};
    if (member(start,"baseline")) base.observation.counters=counters(&c.f,member(start,"baseline"));
    if (member(start,"last_observed")) base.last_observed=counters(&c.f,member(start,"last_observed"));
    if (member(start,"suspended")) base.suspended=boolean(member(start,"suspended"));
    if (member(start,"last_frontier")) {
        base.has_frontier=true; base.frontier=number(member(start,"last_frontier"));
    }
    pgwt_merge_config config; MUST(ma_core_config(p.adapter,&base,&config));
    struct tb_driver driver={.snapshot=readable(),.topology=true,.drained=true};
    struct pgwt_capture_config cfg={.cpu_count=2,.capacity=128,.baseline=config.observation,
        .sink={.emit=wire_sink,.ctx=&c}};
    driver.capture=pgwt_capture_replay_create(&cfg,&config,&p.table);MUST(driver.capture);
    struct pgwt_transport_io io={.ctx=&driver,.now=tb_now,.cpu_set_unchanged=tb_topology,
        .control=tb_control,.wake=tb_wake,.attempts=tb_attempts,.failures=tb_failures,
        .discards=tb_discards,.positions=tb_positions,.counters=tb_counters,
        .drain=tb_drain,.received=tb_received,.envelope=tb_envelope,.fault=tb_fault};
    uint32_t cpus[]={0,1};driver.transport=pgwt_transport_create(cpus,2,128,&io);MUST(driver.transport);
    for(size_t i=0;i<64;++i)if(!tb_live[i].capture){
        tb_live[i].capture=driver.capture;tb_live[i].transport=driver.transport;break;
    }
    pgwt_merge_status status=pgwt_capture_report(driver.capture).status;
    pgwt_merge_status statuses[256]={0}; bool processed[256]={0};
    size_t index=0,record_index=0;
    for (const json_node *n=inputs->child;n;n=n->next,++index) {
        const json_node *kind=member(n,"kind");
        if (string_is(kind,"NOTE") || string_is(kind,"WRITER_REJECT")) continue;
        ma_envelope e=control(MA_CHECK);
        if (string_is(kind,"DATA") || string_is(kind,"TEXT")) {
            bool text=string_is(kind,"TEXT"); replay_wire *w=&c.records[record_index++];
            e=wire(text ? MA_TEXT : w->size==48 ? MA_TRACE : MA_LIFECYCLE,w->bytes,w->size);
            e.source=w->source; e.position=w->position;
            if (!text) {
                const json_node *id=identity(&c.f,string_value(member(n,"who")));
                e.cookie=number(member(id,"g"));
                if (cookie_test==1) e.cookie_present=false;
                if (cookie_test==2) e.cookie=0;
                e.has_q=member(n,"q") && member(n,"q")->kind==J_NUMBER;
                e.q=e.has_q ? number(member(n,"q")) : 0;
            }
        } else if (string_is(kind,"BOUNDARY")) {
            e.kind=MA_BOUNDARY; e.source=source(&c.f,member(n,"source"));
            e.position=number(member(n,"pos")); e.w=number(member(n,"w"));
            e.boundary=e.source<2 ? MA_IDLE_PERF : MA_RING_BOUNDARY;
        } else if (string_is(kind,"LOSS")) {
            e.kind=MA_LOSS; e.source=source(&c.f,member(n,"source")); e.lost=number(member(n,"n"));
        } else if (string_is(kind,"FAULT")) {
            /* Only row41 is adapter-driven: a concrete coverage interruption. */
            MUST(number(member(row,"id"))==41);
            e.kind=MA_ATTACH_EVENT; e.attach_event=MA_INTERRUPTED; e.control_pid=30;
        } else MUST(string_is(kind,"CHECK") || string_is(kind,"REARM"));
        ma_snapshot s=from_counters(member(n,"C") ? counters(&c.f,member(n,"C")) : named_counters(&c.f,"C0"));
        if (string_is(kind,"REARM")) {
            /* Explicit contract-fixture control, never a daemon wire input. */
            pgwt_merge_attachment fresh[32]; size_t nf=0;
            const json_node *ps=member(n,"proofs");
            MUST(ps && ps->kind==J_OBJECT);
            for (const json_node *q=ps->child;q;q=q->next) {
                MUST(nf<32);
                const json_node *id=identity(&c.f,q->key);
                fresh[nf++]=(pgwt_merge_attachment){.pid=number(member(id,"pid")),
                    .g=number(member(id,"g")),.next_q=number(member(q,"next_q")),
                    .identity_proved=true,.sequence_proved=true};
            }
            pgwt_merge_input in={.kind=PGWT_MERGE_REARM,
                .observation={.d=optional_number(member(n,"d"),0),.counters={.known=s.valid}},
                .attachments=fresh,.attachment_count=nf,.attachment_proofs_fresh=true,.identity_repaired=true};
            memcpy(in.observation.counters.value,s.value,sizeof(s.value));
            status=pgwt_capture_replay_control(driver.capture,&in);
        } else {
            driver.snapshot=s;driver.now=optional_number(member(n,"d"),0);
            int rc;
            if(e.kind==MA_TRACE || e.kind==MA_LIFECYCLE || e.kind==MA_TEXT || e.kind==MA_BOUNDARY) {
                /* Only the corpus cadence is injected: its frozen starting
                 * cutoffs, proofs, positions and explicit REARM controls.
                 * Actual wire validation, receipt, indexing, queue ownership,
                 * retained payloads, mapping and publication use B's code. */
                pgwt_transport_replay_position(driver.transport,e.source,e.position);
                unsigned dest=e.source<2?0:(unsigned)e.source-1;
                unsigned kind=e.kind==MA_TRACE?PGWT_WIRE_TRACE:e.kind==MA_LIFECYCLE?
                    PGWT_WIRE_LIFECYCLE:e.kind==MA_TEXT?PGWT_WIRE_TEXT:PGWT_WIRE_BOUNDARY;
                unsigned char boundary[16]={0};const void *payload=e.bytes;size_t len=e.size;
                if(e.kind==MA_BOUNDARY){
                    tb_le(boundary,e.w,8);tb_le(boundary+8,dest?1:2,4);tb_le(boundary+12,dest,4);
                    payload=boundary;len=16;
                }
                rc=tb_emit(&driver,e.source<2?(unsigned)e.source:0,dest,kind,
                    e.cookie_present?e.cookie:0,payload,len);
                if(rc>=0)rc=pgwt_transport_replay_release(driver.transport);
            } else if(e.kind==MA_LOSS) {
                rc=pgwt_transport_lost(driver.transport,(uint32_t)e.source,e.lost);
            } else rc=pgwt_capture_envelope(driver.capture,&e,driver.now,&s);
            status=pgwt_capture_report(driver.capture).status;
            MUST(rc>=0 || status==PGWT_MERGE_EXIT_REQUIRED);
        }
        statuses[index]=status; processed[index]=true;
        if (status==PGWT_MERGE_EXIT_REQUIRED) break;
    }
    bool pass=c.bytes_ok;
    if (cookie_test) {
        pass=pass && status==PGWT_MERGE_SUSPENDED && c.f.count==2;
        for (size_t i=0;i<c.f.count;++i)
            pass=pass && c.f.outputs[i].out.kind==PGWT_MERGE_OUT_VOID &&
                 c.f.outputs[i].out.reason==PGWT_MERGE_UNKNOWN_EVIDENCE;
    } else {
        const json_node *outputs=member(expected,"outputs");
        const char *field=NULL;
        if (c.f.count!=list_length(outputs)) field="output count";
        else for (size_t i=0;i<c.f.count;++i)
            if (!equal_output(&c.f,&c.f.outputs[i],item(outputs,i),&field)) break;
        pass=pass && !field && ((status==PGWT_MERGE_EXIT_REQUIRED)==boolean(member(expected,"exit_nonzero")));
        const json_node *invalid=member(expected,"epoch_invalid");
        pass=pass && pgwt_capture_report(driver.capture).epoch_invalid==(invalid ? boolean(invalid) : false);
        const json_node *checks=member(expected,"statuses");
        for (const json_node *s=checks ? checks->child : NULL;s;s=s->next) {
            size_t at=(size_t)number(member(s,"input_index"));
            pass=pass && at<256 && processed[at] && statuses[at]==status_value(member(s,"status"));
        }
        if (c.f.reject) pass=pass && c.f.rejected;
        if (!pass) fprintf(stderr,"adapter row %" PRIu64 "/%s MISMATCH %s (%zu outputs)\n",
            number(member(row,"id")),mode,field ? field : "status/bytes/invalidation",c.f.count);
    }
    tb_cleanup(&driver); tracked_adapter_destroy(p.adapter);
    MUST(pass);
    return true;
}

#endif

static bool tb_counter_cost(void)
{
    struct tb_driver d;TB(tb_setup(&d));d.count_maps=true;TB(tb_begin(&d));
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace+8,30,4);
    for(unsigned i=0;i<40;++i){
        tb_le(trace,13+i,8);TB(tb_emit(&d,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);
    }
    /* Update the first round's E/L cutoffs, preserving their positions. */
    TB(tb_boundary(&d,0,1,1,100)==0 && tb_boundary(&d,1,2,1,100)==0);
    TB(tb_boundary(&d,0,0,2,101)==0 && tb_boundary(&d,1,0,2,101)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.reads==2 && d.map_lookups==13 && d.data_outputs==41);
    TB(d.first_data_lookups==13 && d.last_data_lookups==13 && d.observes==d.count);
    printf("counter cost: 41 released DATA, 13 map lookups at 1 completion checkpoint; 0 map lookups/DATA\n");
    tb_cleanup(&d);return true;
}
static bool tb_terminal_attempts(void)
{
    for(unsigned fail=0;fail<2;++fail)for(size_t slot=0;slot<6;++slot){
        struct tb_driver d;TB(tb_setup(&d));
        /* A failed last publication has no following sequence number to
         * expose its gap. All destinations are empty, with no pending round. */
        if(fail)d.attempts[slot]=1;
        TB(pgwt_transport_finish(d.transport)==0 && d.drains==1);
        TB(d.count==(fail?1U:0U));
        if(fail)TB(d.outputs[0].kind==PGWT_MERGE_OUT_VOID &&
                   d.outputs[0].reason==PGWT_MERGE_SEQUENCE_GAP &&
                   pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
        tb_cleanup(&d);
    }
    puts("terminal attempts: empty CPU/E/L with last publication missing => VOID(sequence_gap) PASS");
    return true;
}
static bool tb_cadence(void)
{
    struct pgwt_transport_perf_attributes a=pgwt_transport_perf_attributes(16U*1024U*1024U);
    TB(a.type==1 && a.config==10 && a.sample_type==(1U<<10) && a.sample_period==1 &&
       a.watermark && a.wakeup_events==0 && a.wakeup_watermark==65536);
    a=pgwt_transport_perf_attributes(4096);
    TB(a.watermark && a.wakeup_events==0 && a.wakeup_watermark==512);
    TB(pgwt_transport_service_interval_ns()==10000000);
    TB(pgwt_transport_service_due(0,0,false) && !pgwt_transport_service_due(9999999,0,true) &&
       pgwt_transport_service_due(10000000,9999999,true) &&
       !pgwt_transport_service_due(19999999,10000000,true) &&
       pgwt_transport_service_due(1,20000000,true));
    struct tb_driver driver;TB(tb_setup(&driver));
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TB(d);
    d->exact_perf=true;d->percpu_transport=driver.transport;d->merge_capture=driver.capture;
    TB(pgwt_percpu_transport_prepare(d)==0 && pgwt_percpu_transport_configure(d)==0 &&
       pgwt_percpu_transport_open(d)==0);
    /* Continuous early readiness drains promptly, but does not multiply
     * topology sysfs/helper-health work or request helpers on an idle box. */
    for(unsigned i=0;i<100000;++i){driver.now=i*100;TB(pgwt_percpu_transport_poll(d)==0);}
    TB(driver.drains==100000 && driver.topology_reads==1 && !driver.wakes);
    driver.now=10000000;TB(pgwt_percpu_transport_poll(d)==0 && driver.topology_reads==2);
    driver.now=10000001;TB(pgwt_percpu_transport_poll(d)==0 && driver.topology_reads==2);
    /* A clock-anchored cadence does not accumulate delay from early wakes. */
    driver.now=29999999;TB(pgwt_percpu_transport_poll(d)==0 && driver.topology_reads==3);
    driver.now=30000000;TB(pgwt_percpu_transport_poll(d)==0 && driver.topology_reads==4);
    tb_cleanup(&driver);free(d);
    puts("perf cadence: byte watermark min(buffer/8,65536), poll 10 ms; 100000 early wakes, 1 topology service PASS");
    return true;
}
static bool tb_pending(void)
{
    struct tb_driver driver;TB(tb_setup(&driver));
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TB(d);
    d->exact_perf=true;d->percpu_transport=driver.transport;d->merge_capture=driver.capture;
    TB(pgwt_merge_capture_pending_records(d)==0);
    TB(tb_begin(&driver));
    TB(pgwt_transport_pending_records(driver.transport)==4 && pgwt_capture_pending_records(driver.capture)==0 &&
       pgwt_merge_capture_pending_records(d)==4);
    TB(tb_boundary(&driver,0,0,2,14)==0 && tb_boundary(&driver,1,0,2,14)==0);
    TB(pgwt_merge_capture_pending_records(d)==6);
    TB(pgwt_transport_poll(driver.transport,false)==0 && driver.traces==1 &&
       pgwt_merge_capture_pending_records(d)==0);
    /* The real, now portable tick entry must delegate even when its own
     * post-duty poll is skipped by --count/shutdown. */
    d->running=false;d->tick=0;
    TB(pgwt_merge_capture_tick(d)==0 && d->tick==1);
    tb_cleanup(&driver);
    TB(tb_setup(&driver));driver.prove=false;
    d->percpu_transport=driver.transport;d->merge_capture=driver.capture;
    TB(tb_begin(&driver));
    struct pgwt_attach_report r={.pid=30,.cookie=123,.event=MA_BEGIN};
    TB(pgwt_capture_attach(driver.capture,&r,driver.now,&driver.snapshot)==0);
    TB(tb_boundary(&driver,0,0,2,14)==0 && tb_boundary(&driver,1,0,2,14)==0);
    TB(pgwt_transport_poll(driver.transport,false)==0 && !driver.traces);
    /* Transport empty; adapter holds FORK and its following L boundary,
     * while the core stages TRANS. Do not lose or double-count either tier. */
    TB(pgwt_transport_pending_records(driver.transport)==0 && pgwt_capture_pending_records(driver.capture)==3 &&
       pgwt_merge_capture_pending_records(d)==3);
    r.event=MA_COMPLETE;r.facts_known=r.facts_true=MA_ALL_FACTS;
    TB(pgwt_capture_attach(driver.capture,&r,driver.now,&driver.snapshot)==0 && driver.traces==1 &&
       pgwt_merge_capture_pending_records(d)==0);
    tb_cleanup(&driver);free(d);return true;
}
static bool tb_poll_budget(void)
{
    struct tb_driver driver;TB(tb_setup(&driver));
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TB(d);
    d->exact_perf=true;d->percpu_transport=driver.transport;d->merge_capture=driver.capture;
    TB(pgwt_percpu_transport_prepare(d)==0 && pgwt_percpu_transport_configure(d)==0 &&
       pgwt_percpu_transport_open(d)==0);
    const uint64_t interval=pgwt_transport_service_interval_ns();
    driver.now=0;TB(pgwt_percpu_transport_poll(d)==0 && !driver.wakes);
    /* Model DATA emitted just after that poll, below the watermark. First
     * consume at 10 ms. t=13 is newer than round one's certified cutoff 12,
     * so it must wait for round two; the timer is not permission to publish. */
    uint64_t emitted=1;
    driver.shutdown_phase=1;driver.trace_t=13;
    for(unsigned slot=1;slot<=3;++slot){
        driver.now=slot*interval;
        TB(pgwt_percpu_transport_poll(d)==0 && !driver.traces);
    }
    TB(driver.shutdown_phase==4 && pgwt_capture_pending_records(driver.capture)==1);
    driver.now=4*interval;TB(pgwt_percpu_transport_poll(d)==0 && driver.ctl.round==2 && driver.ctl.el_request);
    TB(tb_boundary(&driver,0,1,2,14)==0 && tb_boundary(&driver,1,2,2,14)==0);
    driver.now=5*interval;TB(pgwt_percpu_transport_poll(d)==0 && !driver.traces);
    TB(tb_boundary(&driver,0,0,3,15)==0 && tb_boundary(&driver,1,0,3,15)==0);
    driver.now=6*interval;TB(pgwt_percpu_transport_poll(d)==0 && driver.traces==1 &&
       pgwt_merge_capture_pending_records(d)==0 && driver.now-emitted<70000000);
    printf("poll budget: empty drains and prompt helpers only: below-watermark DATA + two rounds, "
           "release by 60 ms; <=70 ms with poll lateness (not a sustained-load bound) PASS\n");
    tb_cleanup(&driver);free(d);return true;
}
static bool tb_bind(struct tb_driver *driver,struct pgwt_daemon **out)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TB(d);
    d->exact_perf=true;d->percpu_transport=driver->transport;d->merge_capture=driver->capture;
    TB(pgwt_percpu_transport_prepare(d)==0 && pgwt_percpu_transport_configure(d)==0 &&
       pgwt_percpu_transport_open(d)==0);
    *out=d;return true;
}
/* A round opened by hand, so a schedule can place attempts between the E/L
 * boundaries and the frozen targets. Ends before the fence request. */
static bool tb_open_round(struct tb_driver *d)
{
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,10,8);
    TB(tb_emit(d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,12,8);tb_le(trace+8,30,4);
    TB(tb_emit(d,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);
    TB(pgwt_transport_poll(d->transport,false)==0 && d->controls==1);
    TB(tb_boundary(d,0,1,1,13)==0);TB(tb_boundary(d,1,2,1,13)==0);
    return true;
}
static bool tb_acks(struct tb_driver *d,unsigned round,uint64_t w)
{ TB(tb_boundary(d,0,0,round,w)==0);TB(tb_boundary(d,1,0,round,w)==0);return true; }
static bool tb_void(const struct tb_driver *d,size_t at,pgwt_merge_reason reason)
{ return d->count>at && d->outputs[at].kind==PGWT_MERGE_OUT_VOID && d->outputs[at].reason==reason; }

/* T1 and T4: the completion rule under passes that are NEVER empty. */
static bool tb_never_empty(void)
{
    struct tb_driver driver;struct pgwt_daemon *d;
    TB(tb_setup(&driver));TB(tb_begin(&driver));TB(tb_bind(&driver,&d));
    driver.drained=false; /* every pass consumes something, from here on */
    /* T4: with one acknowledgement missing, no number of passes releases. */
    TB(tb_boundary(&driver,0,0,2,14)==0);
    for(unsigned pass=1;pass<=300;++pass){
        driver.now=20+UINT64_C(100000)*pass;TB(pgwt_percpu_transport_poll(d)==0);
    }
    TB(!driver.traces && !driver.count && driver.reads==1 && pgwt_merge_capture_pending_records(d)==5);
    /* T1: the pass that consumes the last acknowledgement completes the
     * round. No empty pass, no further drain, no position read. */
    TB(tb_boundary(&driver,1,0,2,14)==0);
    driver.now+=100000;TB(pgwt_percpu_transport_poll(d)==0);
    TB(driver.traces==1 && driver.count==3 && driver.outputs[2].kind==PGWT_MERGE_OUT_FRONTIER &&
       driver.outputs[2].F==12 && pgwt_capture_report(driver.capture).status==PGWT_MERGE_OK);
    TB(driver.reads==2 && !driver.position_reads && pgwt_merge_capture_pending_records(d)==0);
    struct pgwt_transport_stats s=pgwt_transport_stats(driver.transport);
    TB(s.rounds_requested==1 && s.rounds_completed==1 && s.rounds_completed_nonempty==1 && !s.holds && !s.cuts);
    /* The next round: a NEW nonzero E/L token (the producer serves a token
     * once), the fence on a pass that is not a service slot, and completion
     * on an empty pass, which the non-empty count must not include. */
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,20,8);tb_le(trace+8,30,4);
    TB(tb_emit(&driver,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);
    driver.drained=true;
    driver.now=UINT64_C(40000000);TB(pgwt_percpu_transport_poll(d)==0);
    TB(driver.ctl.round==2 && driver.ctl.el_request==2 && driver.controls==3);
    TB(tb_boundary(&driver,0,1,2,30)==0 && tb_boundary(&driver,1,2,2,30)==0);
    driver.now+=100000;TB(pgwt_percpu_transport_poll(d)==0);
    TB(driver.ctl.round==3 && !driver.ctl.el_request && driver.controls==4);
    TB(tb_acks(&driver,3,31));
    driver.now+=100000;TB(pgwt_percpu_transport_poll(d)==0);
    TB(driver.traces==2 && pgwt_capture_report(driver.capture).status==PGWT_MERGE_OK);
    s=pgwt_transport_stats(driver.transport);
    TB(s.rounds_requested==2 && s.rounds_completed==2 && s.rounds_completed_nonempty==1);
    tb_cleanup(&driver);free(d);
    puts("completion: never-empty passes hold without every ack (300 passes) and release on the pass that consumes the last ack PASS");
    return true;
}
/* T2: a snapshot-era record not yet consumed when the last ack arrives. */
static bool tb_snapshot_hold(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_open_round(&d));
    /* CPU 1 reserved an E record after the E boundary and before the targets
     * were frozen. The reader has not reached it. */
    d.attempts[4]=1;d.ring_produced[0]=88;
    TB(pgwt_transport_poll(d.transport,false)==0 && d.controls==2);
    TB(tb_acks(&d,2,14));
    TB(pgwt_transport_poll(d.transport,false)==0);
    /* Every ack is in and one target is unreached: a hold, not a fault. */
    TB(!d.count && !d.traces && d.position_reads==1 &&
       pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    /* Later traffic on the same key keeps arriving and is never consumed
     * here. Neither the cut nor the target may follow it. */
    d.attempts[4]=11;d.ring_produced[0]=88+10*88;
    for(unsigned pass=0;pass<50;++pass)TB(pgwt_transport_poll(d.transport,false)==0);
    TB(!d.count && pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    struct pgwt_transport_stats s=pgwt_transport_stats(d.transport);
    TB(s.holds==51 && s.cuts==1 && !s.rounds_completed);
    /* The reader consumes that one record (seq 1, t=13: not below the E
     * boundary) and stands exactly at the cut, with more behind it. */
    unsigned char trace[48],packet[304];memcpy(trace,golden_T,48);tb_le(trace,13,8);tb_le(trace+8,30,4);
    size_t len=tb_packet(packet,PGWT_WIRE_TRACE,1,1,123,trace,48);
    TB(pgwt_transport_record(d.transport,PGWT_WIRE_DEST_E,packet,len)==0);
    d.ring_consumed[0]=88;
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==3 && d.outputs[2].kind==PGWT_MERGE_OUT_FRONTIER && d.outputs[2].F==12 && d.traces==1);
    s=pgwt_transport_stats(d.transport);
    TB(s.rounds_completed==1 && !s.terminal_discards && !s.skipped);
    tb_cleanup(&d);
    puts("completion: an unconsumed snapshot-era record holds without a fault; the cut and the target stay frozen under later traffic PASS");
    return true;
}
/* T3: an attempt that failed is never delivered. */
static bool tb_terminal_failure(void)
{
    /* (a) Counted on any key while the fence is open: VOID as soon as every
     * ack is consumed, with no pass and no position read. */
    for(size_t k=0;k<6;++k){
        struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
        ++d.attempts[k];d.failures[k]=1;
        if(k!=0)TB(tb_boundary(&d,0,0,2,14)==0);
        if(k!=3)TB(tb_boundary(&d,1,0,2,14)==0);
        /* k==0 / k==3: that CPU's own acknowledgement is the failed attempt. */
        TB(pgwt_transport_poll(d.transport,false)==0);
        TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_SEQUENCE_GAP) && !d.traces && !d.position_reads);
        tb_cleanup(&d);
    }
    /* (b) Inside the frozen target window, on a key with no later record:
     * the failure counter decides, the cut is not needed. */
    struct tb_driver d;TB(tb_setup(&d));TB(tb_open_round(&d));
    d.attempts[4]=1;d.failures[4]=1;
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_SEQUENCE_GAP) && !d.position_reads && d.controls==1);
    tb_cleanup(&d);
    /* (c) The round's E boundary itself failed: it never arrives and the
     * fence never starts. The open round's failure poll finds it. */
    TB(tb_setup(&d));
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,10,8);
    TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    TB(pgwt_transport_poll(d.transport,false)==0 && d.controls==1);
    TB(tb_boundary(&d,1,2,1,13)==0);
    TB(pgwt_transport_poll(d.transport,false)==0 && !d.count);
    ++d.attempts[1];d.failures[1]=1;
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_SEQUENCE_GAP) && d.controls==1);
    tb_cleanup(&d);
    /* (d) Accounting that cannot be read is unknown, never zero. */
    TB(tb_setup(&d));TB(tb_begin(&d));TB(tb_acks(&d,2,14));
    d.accounting_error=true;
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_COUNTER_UNKNOWN) && !d.traces);
    tb_cleanup(&d);
    puts("completion: a counted failure on any key, a failed boundary and a failed ack VOID(sequence_gap) with no pass; unreadable accounting VOIDs PASS");
    return true;
}
/* T5: the control for the box falsifier. If completion ever released too
 * early, the record it should have waited for arrives afterwards below a
 * published frontier, and the coordinator must say so. */
static bool tb_seal_control(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));TB(tb_acks(&d,2,14));
    TB(pgwt_transport_poll(d.transport,false)==0 && d.count==3 && d.outputs[2].F==12);
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,5,8);tb_le(trace+8,30,4);
    TB(tb_emit(&d,1,1,PGWT_WIRE_TRACE,123,trace,48)==0);
    TB(pgwt_transport_poll(d.transport,false)==0 && d.ctl.round==2 && d.ctl.el_request==2);
    TB(tb_boundary(&d,0,1,2,20)==0 && tb_boundary(&d,1,2,2,20)==0);
    TB(pgwt_transport_poll(d.transport,false)==0 && d.ctl.round==3);
    TB(tb_acks(&d,3,21));
    TB(pgwt_transport_poll(d.transport,false)==0);
    pgwt_merge_report report=pgwt_capture_report(d.capture);
    TB(tb_void(&d,d.count-1,PGWT_MERGE_SEAL_VIOLATION) && report.faults[PGWT_MERGE_SEAL_VIOLATION]==1 &&
       report.epoch_invalid && report.status==PGWT_MERGE_SUSPENDED);
    tb_cleanup(&d);
    puts("completion control: a record below a published frontier arriving later => VOID(seal_violation), epoch invalid PASS");
    return true;
}
/* A discard is an attempt withdrawn by design, with nothing lost. */
static bool tb_interior_discards(void)
{
    for(unsigned dest=1;dest<3;++dest){
        struct tb_driver d;TB(tb_setup(&d));
        TB(tb_boundary(&d,0,dest,1,13)==0 && !d.count);            /* seq 1 */
        d.seq[dest]=2;d.discards[dest]=1;                          /* seq 2 withdrawn, counted */
        TB(tb_boundary(&d,0,dest,1,13)==0 && !d.count);            /* seq 3 */
        TB(pgwt_transport_stats(d.transport).skipped==1 &&
           pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
        d.seq[dest]=4;                                             /* seq 4 missing, counted nowhere */
        TB(tb_boundary(&d,0,dest,1,13)==0);
        TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_SEQUENCE_GAP));
        tb_cleanup(&d);
        /* A failure on the key is a loss, whatever else is counted. */
        TB(tb_setup(&d));
        TB(tb_boundary(&d,0,dest,1,13)==0);
        d.seq[dest]=2;d.discards[dest]=1;d.failures[dest]=1;
        TB(tb_boundary(&d,0,dest,1,13)==0);
        TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_SEQUENCE_GAP));
        tb_cleanup(&d);
        /* A repeated number is never a discard, however many are counted. */
        TB(tb_setup(&d));
        TB(tb_boundary(&d,0,dest,1,13)==0);
        d.seq[dest]=0;d.discards[dest]=UINT64_MAX;
        TB(tb_boundary(&d,0,dest,1,13)==0);
        TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_SEQUENCE_GAP));
        tb_cleanup(&d);
    }
    puts("discards: a counted discard explains a skipped number; an uncounted skip, a failure and a repeat VOID(sequence_gap) PASS");
    return true;
}
static bool tb_terminal_discard(void)
{
    /* The last snapshot-era attempt on (CPU 1, E) was withdrawn by design.
     * No later record reveals it: only the cut can decide. */
    struct tb_driver d;TB(tb_setup(&d));TB(tb_open_round(&d));
    d.attempts[4]=1;d.discards[4]=1;d.ring_produced[0]=88;
    TB(pgwt_transport_poll(d.transport,false)==0 && d.controls==2);
    TB(tb_acks(&d,2,14));
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(!d.count && d.position_reads==1);                /* not past the cut: hold */
    d.ring_consumed[0]=88;                              /* the reader passed the withdrawn reservation */
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==3 && d.outputs[2].kind==PGWT_MERGE_OUT_FRONTIER && d.outputs[2].F==12 && d.traces==1);
    struct pgwt_transport_stats s=pgwt_transport_stats(d.transport);
    TB(s.terminal_discards==1 && s.cuts==1 && s.rounds_completed==1);
    /* The key's next record carries seq 2: one skipped number, the same
     * counted discard. It was never recorded as delivered. */
    d.seq[4]=1;TB(tb_boundary(&d,1,1,2,20)==0);
    s=pgwt_transport_stats(d.transport);
    TB(d.count==3 && s.skipped==1 && pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    tb_cleanup(&d);

    /* The same schedule with NO counted discard is a terminal hole. It is a
     * hold while the reader is short of the cut, whatever else is reserved
     * meanwhile, then VOID(sequence_gap). Nothing is released or skipped. */
    TB(tb_setup(&d));TB(tb_open_round(&d));
    d.attempts[4]=1;d.ring_produced[0]=88;
    TB(pgwt_transport_poll(d.transport,false)==0);TB(tb_acks(&d,2,14));
    TB(pgwt_transport_poll(d.transport,false)==0 && !d.count);   /* the cut: 88 */
    for(unsigned pass=0;pass<200;++pass){
        d.ring_produced[0]+=88;TB(pgwt_transport_poll(d.transport,false)==0 && !d.count);
    }
    d.ring_consumed[0]=88;
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_SEQUENCE_GAP) && !d.traces);
    tb_cleanup(&d);

    /* The bound. A prefix that is never consumed (a reservation that never
     * finishes) is not waited for forever: after the stated number of
     * passes the round VOIDs. Positions that cannot be read VOID at once. */
    TB(tb_setup(&d));TB(tb_open_round(&d));
    d.attempts[4]=1;d.ring_produced[0]=88;
    TB(pgwt_transport_poll(d.transport,false)==0);TB(tb_acks(&d,2,14));
    const unsigned limit=88/(PGWT_TRANSPORT_MIN_RING_RECORD*4096)+1+PGWT_TRANSPORT_CUT_STALL_PASSES;
    for(unsigned pass=0;pass<limit;++pass)TB(pgwt_transport_poll(d.transport,false)==0 && !d.count);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_UNKNOWN_EVIDENCE) && !d.traces);
    tb_cleanup(&d);
    TB(tb_setup(&d));TB(tb_open_round(&d));
    d.attempts[4]=1;d.ring_produced[0]=88;d.position_error=true;
    TB(pgwt_transport_poll(d.transport,false)==0);TB(tb_acks(&d,2,14));
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==1 && tb_void(&d,0,PGWT_MERGE_UNKNOWN_EVIDENCE));
    tb_cleanup(&d);
    printf("terminal hole: a counted discard completes at the cut; an uncounted one holds until the cut then VOID(sequence_gap); "
           "an unreachable cut VOIDs after %u passes PASS\n",limit+1);
    return true;
}
/* A backend that existed before the capture has no lifetime cookie. */
static bool tb_uncovered(void)
{
    struct tb_driver d;TB(tb_setup(&d));TB(tb_begin(&d));
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,11,8);tb_le(trace+8,77,4);
    TB(tb_emit(&d,1,0,PGWT_WIRE_TRACE,0,trace,48)==0);
    unsigned char gone[24]={0};tb_le(gone,3,4);tb_le(gone+4,77,4);tb_le(gone+16,12,8);
    TB(tb_emit(&d,1,2,PGWT_WIRE_LIFECYCLE,0,gone,24)==0);
    /* Consumed and accounted; the daemon's own EXIT handling ran; not
     * queued, not held, not merge input. */
    TB(d.lifecycle==2 && !d.count && pgwt_transport_pending_records(d.transport)==4 &&
       !pgwt_capture_pending_records(d.capture));
    struct pgwt_transport_stats s=pgwt_transport_stats(d.transport);
    TB(s.uncovered==2 && s.records==6);
    TB(tb_acks(&d,2,14));
    d.now=40;TB(pgwt_transport_poll(d.transport,false)==0);
    TB(d.count==3 && d.outputs[2].kind==PGWT_MERGE_OUT_FRONTIER && d.outputs[2].F==12 && d.traces==1 &&
       pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    /* It never entered the held-age accounting either. */
    TB(pgwt_capture_max_held_ns(d.capture,1000000)==20);
    tb_cleanup(&d);
    puts("uncovered: cookie-less TRACE and EXIT are consumed, accounted and counted, never merge input PASS");
    return true;
}
#ifdef PGWT_TRANSPORT_HOST_TEST
static bool tb_index_cost(struct pgwt_merge_capture *capture,size_t n)
{
    ma_table t=pgwt_capture_replay_table(capture);
    size_t lcost=0,bcost=0;
    for(size_t i=1;i<=n;++i){
        size_t probes=0,slot=ma_lifetime_bucket(&t,30,i,&probes);
        TB(t.lifetime_index[slot] && t.lifetimes[t.lifetime_index[slot]-1].g==i);
        lcost+=probes;
        slot=ma_binding_bucket(&t,i*104729+1,&probes);
        TB(t.binding_index[slot] && t.bindings[t.binding_index[slot]-1].query_id==i*104729+1);
        bcost+=probes;
    }
    TB(lcost<2*n && bcost<2*n);
    printf("indexed lookups at %zu lifetimes: L %.3f, Q %.3f buckets/lookup; TRACE (L+2Q) %.3f buckets/record\n",
        n,(double)lcost/n,(double)bcost/n,(double)(lcost+2*bcost)/n);
    return true;
}
static bool tb_lifetime_history(void)
{
    struct tb_driver d;TB(tb_setup(&d));
    for(size_t i=0;i<64;++i)if(tb_live[i].capture==d.capture)tb_live[i].capture=NULL;
    pgwt_capture_destroy(d.capture);
    struct pgwt_capture_config cfg={.cpu_count=2,.capacity=PGWT_CAPTURE_LIFETIME_CAPACITY,
        .baseline={.counters={.known=true}},.ctx=&d,.sink={.emit=tb_sink,.ctx=&d}};
    d.capture=pgwt_capture_create(&cfg);TB(d.capture);
    for(size_t i=0;i<64;++i)if(!tb_live[i].capture){tb_live[i].capture=d.capture;tb_live[i].transport=d.transport;break;}
    /* Isolate B's table/adapter cost from the deliberately unchanged linear
     * core generation registry. Suspension still retains every identity and
     * proof; receipt and attachment updates take the ordinary capture path. */
    TB(pgwt_capture_fault(d.capture,PGWT_MERGE_UNKNOWN_EVIDENCE,20,&d.snapshot)==0);
    unsigned char fork[24]={0},exit[24]={0},trace[48];
    tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(exit,3,4);tb_le(exit+4,30,4);
    memcpy(trace,golden_T,48);tb_le(trace+8,30,4);
    for(size_t i=1;i<=PGWT_CAPTURE_LIFETIME_CAPACITY;++i){
        ma_envelope e={.version=1,.kind=MA_LIFECYCLE,.source=3,.position=2*i-1,
            .cookie=i,.cookie_present=true,.bytes=fork,.size=24};
        TB(pgwt_capture_received(d.capture,&e,20)==0);
        struct pgwt_attach_report r={.pid=30,.cookie=i,.event=MA_BEGIN};
        TB(pgwt_capture_attach(d.capture,&r,20,&d.snapshot)==0);
        r.event=MA_COMPLETE;r.facts_known=r.facts_true=MA_ALL_FACTS;
        TB(pgwt_capture_attach(d.capture,&r,20,&d.snapshot)==0);
        e.position=2*i;e.bytes=exit;TB(pgwt_capture_received(d.capture,&e,20)==0);
        tb_le(trace+32,i*104729+1,8);
        e.kind=MA_TRACE;e.source=0;e.position=i;e.bytes=trace;e.size=48;
        TB(pgwt_capture_received(d.capture,&e,20)==0);
        if(i==1000 || i==100000)TB(tb_index_cost(d.capture,i));
    }
    ma_table t=pgwt_capture_replay_table(d.capture);
    TB(t.lifetime_count==PGWT_CAPTURE_LIFETIME_CAPACITY && t.binding_count==PGWT_CAPTURE_LIFETIME_CAPACITY);
    size_t old=ma_lifetime_bucket(&t,30,1,NULL),newer=ma_lifetime_bucket(&t,30,100000,NULL);
    TB(old!=newer && t.lifetimes[t.lifetime_index[old]-1].proof==MA_PROVED &&
        t.lifetimes[t.lifetime_index[newer]-1].proof==MA_PROVED);
    struct pgwt_attach_report r={.pid=30,.cookie=1,.event=MA_INTERRUPTED};
    TB(pgwt_capture_attach(d.capture,&r,20,&d.snapshot)==0);
    TB(t.lifetimes[t.lifetime_index[old]-1].proof==MA_FAILED &&
        t.lifetimes[t.lifetime_index[newer]-1].proof==MA_PROVED);
    /* Re-presenting a historical FORK must preserve its failed proof. */
    ma_envelope e={.version=1,.kind=MA_LIFECYCLE,.source=3,.position=1,
        .cookie=1,.cookie_present=true,.bytes=fork,.size=24};
    TB(pgwt_capture_received(d.capture,&e,20)==0 &&
        t.lifetimes[t.lifetime_index[old]-1].proof==MA_FAILED);
    TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,PGWT_CAPTURE_LIFETIME_CAPACITY+1,fork,24)==0);
    TB(d.outputs[d.count-1].kind==PGWT_MERGE_OUT_VOID && d.outputs[d.count-1].reason==PGWT_MERGE_ALLOCATION_FAIL);
    printf("lifetime history: 100000 fork/exit cycles; PID reuse isolated; %u + 1 => VOID(allocation_fail)\n",
        PGWT_CAPTURE_LIFETIME_CAPACITY);
    tb_cleanup(&d);return true;
}
#endif
struct tb_binary {
    unsigned char bytes[65536];
    size_t size, writes;
    bool reject_next;
};
static bool tb_write(void *ctx,const void *bytes,size_t n)
{
    struct tb_binary *f=ctx;++f->writes;
    if(f->reject_next){f->reject_next=false;return false;}
    if(n>sizeof(f->bytes)-f->size)return false;
    memcpy(f->bytes+f->size,bytes,n);f->size+=n;return true;
}
static bool tb_dump(void)
{
    struct tb_binary file={0};
    struct pgwt_capture_dump *dump=pgwt_capture_dump_create(64,tb_write,&file);TB(dump);
    uint32_t cpus[]={0,1};TB(pgwt_capture_dump_header(dump,cpus,2));
    TB(file.writes==1 && file.size==32 && !memcmp(file.bytes,"PGWTQB02",8));
    TB(tb_read(file.bytes+16,4)==PGWT_CAPTURE_LIFETIME_CAPACITY && tb_read(file.bytes+20,4)==88);
    pgwt_merge_output out={.kind=PGWT_MERGE_OUT_DATA,.prior_interval_certified=true,.epoch=1,
        .record={.pid=30,.g=123,.source=1,.o=2,.t=12,.rank=PGWT_MERGE_TRANS,
            .identity_known=true,.payload=golden_T,.payload_size=48}};
    pgwt_merge_observation post={.counters={.known=true}};
    for(size_t i=0;i<41;++i)TB(pgwt_capture_dump(dump,&out,&post));
    TB(file.writes==1 && file.size==32); /* no per-DATA write */
    out=(pgwt_merge_output){.kind=PGWT_MERGE_OUT_FRONTIER,.epoch=1,.F=12,.counters={.known=true}};
    TB(pgwt_capture_dump(dump,&out,&post));
    TB(file.writes==2 && file.size==32+41*88+152);
    TB(tb_read(file.bytes+32,2)==88 && file.bytes[34]==PGWT_MERGE_OUT_DATA && file.bytes[35]==13);
    TB(tb_read(file.bytes+32+24,8)==123 && !memcmp(file.bytes+32+40,golden_T,48));
    TB(pgwt_capture_dump_flush(dump) && file.writes==2);
    printf("binary sink: TRACE DATA 88 bytes; 41 DATA + FRONTIER, 1 write/batch\n");
    pgwt_capture_dump_destroy(dump);return true;
}
static bool tb_dump_failure(void)
{
    struct tb_binary file={0};
    struct pgwt_capture_dump *dump=pgwt_capture_dump_create(128,tb_write,&file);TB(dump);
    struct tb_driver d;TB(tb_setup(&d));
    pgwt_capture_destroy(d.capture);
    for(size_t i=0;i<64;++i)if(tb_live[i].capture==d.capture)tb_live[i].capture=NULL;
    struct pgwt_capture_config cfg={.cpu_count=2,.capacity=128,.ctx=&d,
        .baseline={.counters={.known=true}},.lifecycle=tb_lifecycle,.trace=tb_trace,
        .observe=tb_post_observe,.writer_failed=tb_writer_failed,
        .sink={.emit=pgwt_capture_dump,.ctx=dump,.flush=pgwt_capture_dump_flush}};
    d.capture=pgwt_capture_create(&cfg);TB(d.capture);
    for(size_t i=0;i<64;++i)if(!tb_live[i].capture){tb_live[i].capture=d.capture;tb_live[i].transport=d.transport;break;}
    TB(tb_begin(&d));file.reject_next=true;
    TB(tb_boundary(&d,0,0,2,14)==0 && tb_boundary(&d,1,0,2,14)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED &&
       pgwt_capture_report(d.capture).faults[PGWT_MERGE_WRITER_PUSH_FAIL]==1);
    TB(file.writes==2 && file.size==152 && file.bytes[2]==PGWT_MERGE_OUT_VOID);
    TB(tb_read(file.bytes+4,4)==PGWT_MERGE_WRITER_PUSH_FAIL && tb_read(file.bytes+32+14*8,8)==1);
    TB(!pgwt_capture_report(d.capture).has_frontier);
    tb_cleanup(&d);pgwt_capture_dump_destroy(dump);return true;
}
static bool tb_bound(void)
{
    struct tb_driver d;TB(tb_setup(&d));
    unsigned char payload[16]={0},packet[48];tb_le(payload+12,1,4);
    size_t consumed=0;int rc=0;
    /* Model libbpf's documented convention: positive callback results do
     * NOT stop, negative results do. Re-enter with the unread suffix. */
    for(unsigned i=0;i<5000;++i){
        tb_packet(packet,PGWT_WIRE_BOUNDARY,0,i+1,0,payload,16);
        rc=pgwt_transport_ring_record(d.transport,1,packet,48,&consumed,4096);
        if(rc<0)break;
    }
    TB(consumed==4096 && rc==-EAGAIN);
    TB(pgwt_transport_ring_result(rc,consumed,4096)==4096);
    consumed=0;
    for(unsigned i=4096;i<5000;++i){
        tb_packet(packet,PGWT_WIRE_BOUNDARY,0,i+1,0,payload,16);
        rc=pgwt_transport_ring_record(d.transport,1,packet,48,&consumed,4096);
        TB(rc==0);
    }
    TB(consumed==904 && pgwt_transport_ring_result(rc,consumed,4096)==904);
    TB(pgwt_transport_ring_result(-EIO,1,4096)==-EIO && pgwt_transport_ring_result(-EAGAIN,1,4096)==-EAGAIN);
    tb_cleanup(&d);return true;
}
static const struct {const char *name;bool(*run)(void);} tb_guards[]={
    {"admission",tb_admission},{"all_acks",tb_all_acks},{"missing_ack",tb_missing_ack},
    {"stale_ack",tb_stale_ack},{"counter_timing",tb_counter_timing},{"post_snapshot",tb_post_snapshot},{"core_held_age",tb_core_held_age},
    {"attempt_window",tb_attempt_window},{"late_fork",tb_late_fork},{"unknown_init",tb_unknown_init},{"unknown",tb_unknown},
    {"gaps",tb_gaps},{"writer",tb_writer},{"decode_lost",tb_decode_and_lost},
    {"shutdown_topology",tb_shutdown_and_topology},{"shutdown_raw",tb_shutdown_raw},{"terminal_attempts",tb_terminal_attempts},
    {"cadence",tb_cadence},{"pending",tb_pending},{"poll_budget",tb_poll_budget},
    {"never_empty",tb_never_empty},{"snapshot_hold",tb_snapshot_hold},{"terminal_failure",tb_terminal_failure},
    {"seal_control",tb_seal_control},{"discards",tb_interior_discards},{"terminal_discard",tb_terminal_discard},
    {"uncovered",tb_uncovered},{"flag_off",tb_off},{"injected_kernel",tb_injected_kernel},{"dump",tb_dump},{"dump_failure",tb_dump_failure},{"bounded_ring",tb_bound},{"counter_cost",tb_counter_cost},
#ifdef PGWT_TRANSPORT_HOST_TEST
    {"lifetime_history",tb_lifetime_history},
#endif
};
static int tb_main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"--guard")) {
        for(size_t i=0;i<sizeof(tb_guards)/sizeof(tb_guards[0]);++i) if(!strcmp(argv[2],tb_guards[i].name)) {
            tb_guard=argv[2];printf("REACHED %s\n",tb_guard);fflush(stdout);return tb_guards[i].run()?0:1;
        }
        return 2;
    }
    for(size_t i=0;i<sizeof(tb_guards)/sizeof(tb_guards[0]);++i) {
        tb_guard=tb_guards[i].name;printf("REACHED %s\n",tb_guard);fflush(stdout);
        if(!tb_guards[i].run())return 1;
        printf("transport %s PASS\n",tb_guard);
    }
    char error[256];
    json_document *doc=read_document(argc>1?argv[1]:"contracts/merge-v1/examples.json",error,sizeof(error));
    if(!doc && argc==1)doc=read_document("../contracts/merge-v1/examples.json",error,sizeof(error));
    if(!doc){fprintf(stderr,"%s\n",error);return 2;}
    size_t count=0;active_guard="production_rows";
    for(const json_node *row=member(doc->root,"rows")->child;row;row=row->next) {
        if(!
#ifdef PGWT_TRANSPORT_HOST_TEST
            tb_production_row
#else
            adapter_row /* initial-state corpus injection is host-only */
#endif
            (doc->root,row,"N",0)){free_document(doc);return 1;}
        printf("production wire row %" PRIu64 "/N PASS\n",number(member(row,"id")));++count;
    }
    free_document(doc);if(count!=43)return 2;
    size_t guards=sizeof(tb_guards)/sizeof(tb_guards[0]);
    printf("production wire rows 1-43: 43/43 N PASS; completion/refusal guards: %zu/%zu PASS\n",guards,guards);
    return 0;
}

int main(int argc,char **argv)
{
    int rc=tb_main(argc,argv);
    for(size_t i=0;i<64;++i) {
        pgwt_transport_destroy(tb_live[i].transport);pgwt_capture_destroy(tb_live[i].capture);
        if(live_cores[i])pgwt_merge_destroy(live_cores[i]);
        if(live_adapters[i])ma_destroy(live_adapters[i]);
    }
    return rc;
}

/* Portable binding tests link the real capture binding, with daemon effects
 * supplied here. The shared-duty body has its own compiled host harness. */
struct pgwt_counter_vector pgwt_daemon_counter_vector(struct pgwt_daemon *d)
{(void)d;return (struct pgwt_counter_vector){0};}
int pgwt_process_lifecycle_payload(struct pgwt_daemon *d,void *p,size_t n)
{(void)d;(void)p;(void)n;return -ENOSYS;}
int pgwt_process_trace_payload(struct pgwt_daemon *d,void *p,size_t n)
{(void)d;(void)p;(void)n;return -ENOSYS;}
void pgwt_tick_duties(struct pgwt_daemon *d){++d->tick;}
int pgwt_recover_unattached_backends(struct pgwt_daemon *d){(void)d;return 0;}
int pgwt_sweep_stale_state(struct pgwt_daemon *d){(void)d;return 0;}
void pgwt_escalation_check_budget(struct pgwt_daemon *d){(void)d;}
