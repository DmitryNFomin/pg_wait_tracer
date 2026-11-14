/* Frozen answers stay in contracts/. These tests supply bytes and schedules. */
#include "../src/percpu_transport.h"
#include "../src/merge_capture.h"
#include "../src/daemon.h"
#include "../src/counter_vector.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void tb_le(unsigned char *p, uint64_t v, size_t n)
{ for (size_t i=0;i<n;++i) p[i]=(unsigned char)(v>>(8*i)); }
static size_t tb_packet(unsigned char *p, unsigned kind, unsigned cpu, uint64_t seq,
                         uint64_t cookie, const void *payload, size_t len)
{
    memset(p,0,32+len); tb_le(p,1,2); p[2]=(unsigned char)kind;
    tb_le(p+4,len,2); tb_le(p+6,cpu,2); tb_le(p+8,seq,8); tb_le(p+16,cookie,8);
    if (len) memcpy(p+32,payload,len);
    return 32+len;
}
/* Contract replay builds a real v1 header around each fixture input, decodes
 * it through B's reader and then calls the same production adapter entry as
 * the daemon. Q cases remain covered by the existing synthetic suite; B is N. */
static bool production_wire(const ma_envelope *e, ma_envelope *decoded, unsigned char *packet)
{
    *decoded=*e;
    if (e->kind!=MA_TRACE && e->kind!=MA_LIFECYCLE && e->kind!=MA_TEXT && e->kind!=MA_BOUNDARY) return true;
    uint32_t dest=e->source<2 ? PGWT_WIRE_DEST_CPU : (uint32_t)e->source-1;
    unsigned char boundary[16]={0};
    const void *payload=e->bytes; size_t len=e->size;
    unsigned kind=e->kind==MA_TRACE ? PGWT_WIRE_TRACE : e->kind==MA_LIFECYCLE ?
        PGWT_WIRE_LIFECYCLE : e->kind==MA_TEXT ? PGWT_WIRE_TEXT : PGWT_WIRE_BOUNDARY;
    if (e->kind==MA_BOUNDARY) {
        tb_le(boundary,e->w,8); tb_le(boundary+8,1,4); tb_le(boundary+12,dest,4);
        payload=boundary; len=16;
    }
    size_t n=tb_packet(packet,kind,e->source<2 ? (unsigned)e->source : 0,
                        e->position,e->cookie_present ? e->cookie : 0,payload,len);
    uint32_t cpus[]={0,1}; struct pgwt_wire_record record;
    if (pgwt_transport_decode(cpus,2,dest,e->position,packet,n,&record)<0) return false;
    *decoded=record.envelope;
    return true;
}
#define PGWT_ADAPTER_PRODUCTION_TEST 1
#define PGWT_ADAPTER_EMBEDDED 1
#include "test_core_merge_adapter.c"
#undef ma_create
#undef ma_destroy
#undef pgwt_merge_create
#undef pgwt_merge_destroy

static const char *tb_guard;
#define TB(expr) do { if (!(expr)) { fprintf(stderr,"transport ASSERT %s: %s (line %d)\n",tb_guard,#expr,__LINE__); return false; } } while (0)
struct tb_driver {
    struct pgwt_percpu_transport *transport;
    struct pgwt_merge_capture *capture;
    ma_snapshot snapshot;
    uint64_t attempts[6], seq[6], now, trace_t;
    unsigned controls, wakes, reads, drains, lifecycle, traces;
    struct pgwt_transport_ctl ctl;
    pgwt_merge_output outputs[64];
    unsigned char payloads[64][272];
    size_t count;
    bool topology, reject_data, reject_void, drained, prove, support, read_error, mutate_after_data;
    unsigned prepared, configured, opened, closed, consumed;
    int received_error, lifecycle_error;
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
static int tb_topology(void *ctx) { return ((struct tb_driver *)ctx)->topology ? 1:0; }
static int tb_control(void *ctx,const struct pgwt_transport_ctl *c)
{ struct tb_driver *d=ctx; ++d->controls; d->ctl=*c; return 0; }
static int tb_wake(void *ctx,uint32_t cpu)
{ struct tb_driver *d=ctx; if(cpu>1) return -EINVAL; ++d->wakes; return 0; }
static int tb_attempts(void *ctx,uint64_t *v,size_t n)
{ if(n!=6)return -EINVAL; memcpy(v,((struct tb_driver *)ctx)->attempts,sizeof(uint64_t)*6);return 0; }
static int tb_counters(void *ctx,ma_snapshot *s)
{ struct tb_driver *d=ctx; ++d->reads; *s=d->snapshot; return d->read_error ? -EIO:0; }
static int tb_drain(void *ctx)
{ struct tb_driver *d=ctx; ++d->drains;return d->drained ? 1:0; }
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
    d->outputs[d->count]=*out;
    if(out->record.payload_size)memcpy(d->payloads[d->count],out->record.payload,out->record.payload_size);
    d->outputs[d->count].record.payload=d->payloads[d->count]; ++d->count;
    return true;
}
static int tb_trace(void *ctx,const void *bytes,size_t n)
{
    (void)bytes; if(n!=48)return -EINVAL;
    struct tb_driver *d=ctx;++d->traces;
    if(d->mutate_after_data)d->snapshot.value[11]=1;
    return 0;
}
static int tb_post_observe(void *ctx,pgwt_merge_observation *o)
{
    struct tb_driver *d=ctx;o->d=d->now;o->counters.known=d->snapshot.valid;
    for(size_t i=0;i<15;++i){o->counters.value[i]=d->snapshot.value[i];o->counters.known&=d->snapshot.component_valid[i];}
    return d->read_error ? -EIO:0;
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
        .control=tb_control,.wake=tb_wake,.attempts=tb_attempts,.counters=tb_counters,
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
    TB(d.reads==2 && d.drains==1 && d.count==3);
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
       d.outputs[2].reason==PGWT_MERGE_COUNTER_CHANGE && d.outputs[2].counters.value[11]==1);
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
static bool tb_off(void)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TB(d);
    TB(pgwt_percpu_transport_prepare(d)==0 && pgwt_percpu_transport_configure(d)==0 &&
       pgwt_percpu_transport_open(d)==0 && pgwt_percpu_transport_poll(d)==0 &&
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
    TB(pgwt_percpu_transport_epoll_fd(d)==17 && pgwt_percpu_transport_poll(d)==0);
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
static bool tb_dump(void)
{
    FILE *f=tmpfile();TB(f);
    unsigned char data[]={0x00,0xff,0x12};
    pgwt_merge_output out={.kind=PGWT_MERGE_OUT_DATA,.prior_interval_certified=false,
        .record={.pid=30,.g=123,.source=1,.o=2,.t=12,.payload=data,.payload_size=3},
        .counters={.known=true}};
    pgwt_merge_observation post={0};TB(pgwt_capture_dump(f,&out,&post));
    rewind(f);char text[2048];TB(fgets(text,sizeof(text),f));
    TB(strstr(text,"\"prior_interval_certified\":false") && strstr(text,"\"g\":123") && strstr(text,"00ff12"));
    fclose(f);return true;
}
static const struct {const char *name;bool(*run)(void);} tb_guards[]={
    {"admission",tb_admission},{"all_acks",tb_all_acks},{"missing_ack",tb_missing_ack},
    {"stale_ack",tb_stale_ack},{"counter_timing",tb_counter_timing},{"post_snapshot",tb_post_snapshot},{"core_held_age",tb_core_held_age},
    {"attempt_window",tb_attempt_window},{"late_fork",tb_late_fork},{"unknown_init",tb_unknown_init},{"unknown",tb_unknown},
    {"gaps",tb_gaps},{"writer",tb_writer},{"decode_lost",tb_decode_and_lost},
    {"shutdown_topology",tb_shutdown_and_topology},{"flag_off",tb_off},{"injected_kernel",tb_injected_kernel},{"dump",tb_dump}
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
        if(!adapter_row(doc->root,row,"N",0)){free_document(doc);return 1;}
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

#if defined(__linux__) && !defined(PGWT_TRANSPORT_HOST_TEST)
/* Isolated Linux unit target links readers/libbpf, not the daemon. These
 * side effects are never invoked by portable schedules or flag-off tests. */
struct pgwt_counter_vector pgwt_daemon_counter_vector(struct pgwt_daemon *d)
{(void)d;return (struct pgwt_counter_vector){0};}
int pgwt_process_lifecycle_payload(struct pgwt_daemon *d,void *p,size_t n)
{(void)d;(void)p;(void)n;return -ENOSYS;}
int pgwt_process_trace_payload(struct pgwt_daemon *d,void *p,size_t n)
{(void)d;(void)p;(void)n;return -ENOSYS;}
int pgwt_sweep_bootstrap_backends(struct pgwt_daemon *d){(void)d;return 0;}
int pgwt_recover_unattached_backends(struct pgwt_daemon *d){(void)d;return 0;}
int pgwt_sweep_stale_state(struct pgwt_daemon *d){(void)d;return 0;}
void pgwt_escalation_check_budget(struct pgwt_daemon *d){(void)d;}
#endif
