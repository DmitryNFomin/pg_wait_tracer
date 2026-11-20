/* Portable daemon recovery, with small injectable capacities and virtual time.
 * Core/adapter rules and frozen answers are linked unchanged. */
#define PGWT_TRANSPORT_EMBEDDED
#include "test_percpu_transport.c"
#undef main
#define put_le capture_put_le
#include "../src/merge_capture.c"
#undef put_le

static bool recovery_paused(void *ctx)
{ return pgwt_capture_capacity_paused(((struct tb_driver *)ctx)->capture); }
static void recovery_discard(void *ctx)
{ pgwt_capture_discard_backlog(((struct tb_driver *)ctx)->capture); }
static int recovery_rearm(void *ctx,uint64_t now,const ma_snapshot *s)
{ return pgwt_capture_rearm(((struct tb_driver *)ctx)->capture,now,s); }
static bool recovery_setup(struct tb_driver *d,size_t arrivals,size_t queue,size_t staging)
{
    memset(d,0,sizeof(*d));d->snapshot=readable();d->topology=d->drained=true;d->now=100;
    pgwt_merge_source sources[4]={{.qualified=true},{.qualified=true},{.qualified=true},{.qualified=true}};
    pgwt_merge_config initial={.mode=PGWT_MERGE_N,.sources=sources,.source_count=4,
        .staged_capacity=staging,.payload_capacity=272,.attachment_capacity=arrivals,
        .frontier_capacity=128,.dependency_capacity=arrivals,
        .observation={.d=100,.counters={.known=true}},.last_observed={.known=true}};
    struct pgwt_capture_config cfg={.cpu_count=2,.capacity=arrivals,.ctx=d,
        .baseline=initial.observation,.lifecycle=tb_lifecycle,.trace=tb_trace,
        .observe=tb_post_observe,.now=tb_now,.writer_failed=tb_writer_failed,
        .sink={.emit=tb_sink,.ctx=d,.flush=tb_flush},.test_attach_premise=true};
    ma_table empty={0};
    d->capture=pgwt_capture_replay_create(&cfg,&initial,&empty);TB(d->capture);
    struct pgwt_transport_io io={.ctx=d,.now=tb_now,.cpu_set_unchanged=tb_topology,
        .control=tb_control,.wake=tb_wake,.attempts=tb_attempts,.failures=tb_failures,
        .discards=tb_discards,.positions=tb_positions,.counters=tb_counters,
        .drain=tb_drain,.budget=tb_budget,.pending=tb_pending_count,
        .received_token=tb_received_token,.envelope_token=tb_envelope_token,
        .fault=tb_fault,.suspended=tb_suspended,.waiting=tb_waiting,
        .capacity_paused=recovery_paused,.discard_backlog=recovery_discard,.rearm=recovery_rearm};
    uint32_t cpus[]={0,1};d->transport=pgwt_transport_create(cpus,2,queue,&io);TB(d->transport);
    return true;
}
static void recovery_cleanup(struct tb_driver *d)
{ pgwt_transport_destroy(d->transport);pgwt_capture_destroy(d->capture); }
static bool recovery_trace(struct tb_driver *d,uint64_t ts)
{
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,ts,8);tb_le(trace+8,30,4);
    TB(tb_emit(d,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);return true;
}
static bool recovery_identity(struct tb_driver *d)
{
    struct pgwt_capture_coverage c=pgwt_capture_coverage(d->capture,d->now);
    struct pgwt_capture_stats released=pgwt_capture_stats(d->capture);
    struct pgwt_transport_stats wire=pgwt_transport_stats(d->transport);
    TB(c.payload_records==released.released_data+released.released_text+c.dropped+c.uncovered+c.pending);
    TB(wire.records==released.released_data+released.released_text+c.dropped+c.uncovered+c.pending+wire.controls+wire.rejected+wire.dropped_controls);
    pgwt_capture_coverage_print(stdout,d->capture,d->now,wire.records,
        wire.controls,wire.rejected,wire.dropped_controls);
    TB(c.covered_ns+c.unavailable_ns==c.window_ns);
    return true;
}
/* The same real completion protocol as the daemon. Cutoffs use the virtual
 * monotonic clock, and every requested E/L pair needs every higher-round ack. */
static bool recovery_round(struct tb_driver *d,uint64_t w)
{
    TB(pgwt_transport_poll(d->transport,pgwt_capture_waiting(d->capture))==0);
    unsigned r=d->ctl.round;
    TB(tb_boundary(d,0,1,r,w)==0);TB(tb_boundary(d,0,2,r,w)==0);
    TB(pgwt_transport_poll(d->transport,false)==0);
    TB(d->ctl.round==r+1);
    TB(tb_boundary(d,0,0,r+1,w)==0);TB(tb_boundary(d,1,0,r+1,w)==0);
    TB(pgwt_transport_poll(d->transport,false)==0);
    return true;
}
static bool recovery_fork(struct tb_driver *d)
{
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,101,8);
    TB(tb_emit(d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    d->now=110;TB(recovery_round(d,105));
    TB(pgwt_capture_report(d->capture).status==PGWT_MERGE_OK);
    TB(pgwt_capture_stats(d->capture).released_data==1);return true;
}
static bool recovery_retry(struct tb_driver *d)
{
    TB(pgwt_transport_poll(d->transport,false)==0); /* reclaim, schedule 10 ms */
    d->now+=UINT64_C(10000000);
    TB(pgwt_transport_poll(d->transport,false)==0);return true;
}
static bool recovery_cycles(pgwt_pool pool)
{
    struct tb_driver d;
    size_t arrivals=pool==PGWT_POOL_CAPTURE_ARRIVAL?8:32;
    size_t queue=pool==PGWT_POOL_TRANSPORT_QUEUE?8:64;
    size_t staging=pool==PGWT_POOL_CORE_STAGING?3:32;
    TB(recovery_setup(&d,arrivals,queue,staging));TB(recovery_fork(&d));
    for(unsigned cycle=0;cycle<3;++cycle){
        size_t count=pool==PGWT_POOL_CORE_STAGING?4:9;
        uint64_t first=d.now+1;d.now+=100;
        uint64_t before=pgwt_capture_stats(d.capture).released_data;
        for(size_t i=0;i<count;++i)TB(recovery_trace(&d,first+i));
        if(pool==PGWT_POOL_CORE_STAGING){
            /* Release to the core with only a CPU cutoff; E/L keep W=0. */
            TB(pgwt_transport_replay_release(d.transport)==0);
        }
        TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
        TB(pgwt_capture_diagnostics(d.capture).allocation.pool==pool);
        TB(pgwt_capture_stats(d.capture).released_data==before);
        TB(recovery_identity(&d));
        TB(recovery_retry(&d));
        uint64_t proof_at=d.now;
        TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
        TB(pgwt_capture_report(d.capture).epoch==cycle+2);
        TB(pgwt_transport_pending_records(d.transport)==0);
        TB(pgwt_capture_pending_records(d.capture)==0);
        pgwt_merge_diagnostics pools=pgwt_capture_diagnostics(d.capture);
        TB(pools.pools[PGWT_POOL_CAPTURE_ARRIVAL].occupancy==0);
        TB(pools.pools[PGWT_POOL_CAPTURE_RETAINED].occupancy==0);
        TB(pools.pools[PGWT_POOL_CORE_STAGING].occupancy==0);
        TB(d.capture->arrival_free!=SIZE_MAX && d.capture->free);
        TB(pgwt_capture_coverage(d.capture,d.now).proof_acquisitions==cycle+2);
        /* Old buffered DATA, even if it first arrives after REARM, is lost.
         * An old round's E/L/acks must not qualify the new epoch either. */
        TB(recovery_trace(&d,first+count-1));
        TB(tb_boundary(&d,0,1,1,d.now-1)==0);
        TB(tb_boundary(&d,0,2,1,d.now-1)==0);
        TB(pgwt_capture_stats(d.capture).released_data==before);
        TB(pgwt_capture_coverage(d.capture,d.now).open);
        /* New DATA is held until the new completed-round boundaries. */
        uint64_t ts=d.now+1;d.now+=10;TB(recovery_trace(&d,ts));
        TB(pgwt_capture_stats(d.capture).released_data==before);
        TB(recovery_round(&d,d.now-1));
        TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
        TB(pgwt_capture_stats(d.capture).released_data==before+1);
        struct pgwt_capture_coverage cov=pgwt_capture_coverage(d.capture,d.now);
        TB(cov.pauses==cycle+1 && cov.rearms==cycle+1 && cov.attempts==cycle+1);
        TB(!cov.open && cov.last_end>=cov.last_start && cov.pause_ns>0);
        TB(cov.last_end==proof_at && cov.covered_ns==4+8*(cycle+1));
        TB(cov.pause_ns==cov.unavailable_ns-(d.now-d.capture->coverage_cursor));
        TB(recovery_identity(&d));
    }
    uint64_t prior=0;unsigned frontiers=0;
    for(size_t i=0;i<d.count;++i)if(d.outputs[i].kind==PGWT_MERGE_OUT_FRONTIER){
        TB(d.outputs[i].F>prior);prior=d.outputs[i].F;++frontiers;
    }
    TB(frontiers==4);
    pgwt_capture_coverage_print(stdout,d.capture,d.now,pgwt_transport_stats(d.transport).records,
        pgwt_transport_stats(d.transport).controls,pgwt_transport_stats(d.transport).rejected,
        pgwt_transport_stats(d.transport).dropped_controls);
    recovery_cleanup(&d);return true;
}
static bool recovery_retained(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));
    d.capture->config.test_attach_premise=false;
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,101,8);
    TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    struct pgwt_attach_report begin={.pid=30,.cookie=123,.event=MA_BEGIN};
    TB(pgwt_capture_attach(d.capture,&begin,d.now,&d.snapshot)==0);
    TB(pgwt_transport_replay_release(d.transport)==0);
    TB(pgwt_capture_pending_records(d.capture)==1);
    /* A held FORK also holds its later source boundary, so both payload and
     * control storage must be reclaimed and separately accounted. */
    TB(pgwt_transport_poll(d.transport,true)==0);
    TB(tb_boundary(&d,0,2,d.ctl.round,105)==0);
    TB(pgwt_transport_replay_release(d.transport)==0);
    TB(pgwt_capture_pending_records(d.capture)==2);
    d.now=1000;for(unsigned i=0;i<8;++i)TB(recovery_trace(&d,111+i));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    d.capture->config.test_attach_premise=true;
    TB(recovery_retry(&d));
    TB(pgwt_capture_pending_records(d.capture)==0);
    TB(pgwt_capture_coverage(d.capture,d.now).dropped_boundaries==1);
    TB(recovery_identity(&d));
    uint64_t ts=d.now+1;d.now+=10;TB(recovery_trace(&d,ts));
    TB(recovery_round(&d,d.now-1));
    TB(pgwt_capture_stats(d.capture).released_data==1);
    TB(recovery_identity(&d));recovery_cleanup(&d);return true;
}
static bool recovery_counter_change(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    d.now=1000;for(unsigned i=0;i<9;++i)TB(recovery_trace(&d,111+i));
    d.capture->config.test_attach_premise=false;
    TB(recovery_retry(&d)); /* one failed proof attempt; next is 20 ms away */
    ++d.snapshot.value[0];d.now+=UINT64_C(10000000);
    TB(pgwt_transport_poll(d.transport,false)==0); /* checkpoint during backoff */
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    TB(pgwt_capture_report(d.capture).faults[PGWT_MERGE_COUNTER_CHANGE]==1);
    TB(!pgwt_capture_capacity_paused(d.capture));
    d.now+=UINT64_C(5000000000);TB(pgwt_transport_poll(d.transport,false)==0);
    TB(pgwt_capture_coverage(d.capture,d.now).attempts==1);
    TB(pgwt_capture_coverage(d.capture,d.now).rearms==0);
    recovery_cleanup(&d);return true;
}
static bool recovery_retained_seal(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    d.now=1000;for(unsigned i=0;i<9;++i)TB(recovery_trace(&d,111+i));
    TB(recovery_retry(&d));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    /* Below the preceding epoch's F104, even while fresh boundaries are
     * pending: core arrival validation must invalidate that prefix. */
    TB(recovery_trace(&d,103));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    TB(pgwt_capture_report(d.capture).faults[PGWT_MERGE_SEAL_VIOLATION]==1);
    TB(pgwt_capture_report(d.capture).epoch_invalid);
    TB(pgwt_capture_coverage(d.capture,d.now).covered_ns==0);
    TB(pgwt_capture_coverage(d.capture,d.now).unavailable_ns==d.now-100);
    TB(!pgwt_capture_capacity_paused(d.capture));
    TB(recovery_identity(&d));recovery_cleanup(&d);return true;
}
static bool recovery_classes(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    unsigned char trace[48];memcpy(trace,golden_T,48);tb_le(trace,111,8);tb_le(trace+8,30,4);
    TB(tb_emit(&d,0,0,PGWT_WIRE_TRACE,0,trace,48)==0); /* uncovered */
    TB(recovery_trace(&d,112)); /* pending */
    TB(pgwt_capture_coverage(d.capture,d.now).pending==1);
    TB(pgwt_capture_coverage(d.capture,d.now).uncovered==1);
    TB(recovery_identity(&d));
    /* A malformed wire record is a rejected-fault class, not missing from
     * wire totals and never a fabricated payload arrival. */
    unsigned char wire[80];tb_packet(wire,PGWT_WIRE_TRACE,0,99,123,trace,48);wire[0]=0;
    TB(pgwt_transport_record(d.transport,0,wire,sizeof(wire))<0);
    TB(pgwt_transport_stats(d.transport).rejected==1);
    TB(recovery_identity(&d));
    recovery_cleanup(&d);return true;
}
static bool recovery_proof_time(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    d.now=1000;for(unsigned i=0;i<9;++i)TB(recovery_trace(&d,111+i));
    TB(pgwt_transport_poll(d.transport,false)==0);
    d.now+=UINT64_C(10000000);uint64_t started=d.now;
    d.feed_cost_ns=100; /* CHECK + fresh BEGIN + COMPLETE each take time */
    TB(pgwt_transport_poll(d.transport,false)==0);
    uint64_t proof_end=d.now;TB(proof_end==started+300);
    TB(d.capture->rearm_at==proof_end);
    d.feed_cost_ns=0;
    uint64_t before=pgwt_capture_stats(d.capture).released_data;
    TB(recovery_trace(&d,started+1)); /* during acquisition: still unavailable */
    uint64_t ts=proof_end+1;d.now+=10;TB(recovery_trace(&d,ts));
    TB(recovery_round(&d,d.now-1));
    TB(pgwt_capture_stats(d.capture).released_data==before+1);
    TB(pgwt_capture_coverage(d.capture,d.now).last_end==proof_end);
    TB(recovery_identity(&d));recovery_cleanup(&d);return true;
}
static bool recovery_break_proof(void *ctx)
{
    struct tb_driver *d=ctx;
    /* This runs outside a core feed, in the consumer's end-of-input flush.
     * The second call is BEGIN; the following VOID's flush is the third. */
    if(++d->observes==2)
        (void)pgwt_capture_fault(d->capture,PGWT_MERGE_UNKNOWN_EVIDENCE,d->now,&d->snapshot);
    return true;
}
static bool recovery_proof_fault(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    d.now=1000;for(unsigned i=0;i<9;++i)TB(recovery_trace(&d,111+i));
    TB(pgwt_transport_poll(d.transport,false)==0); /* initial checkpoint */
    d.capture->config.sink.flush=recovery_break_proof;
    /* tb_post_observe shares this diagnostic counter; it runs for VOID and
     * is not part of this injected flush schedule. Disable that observer. */
    d.capture->config.observe=NULL;d.observes=0;
    TB(recovery_retry(&d));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    TB(pgwt_capture_report(d.capture).faults[PGWT_MERGE_UNKNOWN_EVIDENCE]==1);
    TB(pgwt_capture_coverage(d.capture,d.now).rearms==0);
    TB(!pgwt_capture_capacity_paused(d.capture));
    recovery_cleanup(&d);return true;
}
static bool recovery_baseline(void)
{
    /* Both the attempt read and a backoff checkpoint must report unknown;
     * a partially unreadable vector is just as invalid as a failed syscall. */
    for(unsigned mode=0;mode<4;++mode){
        struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
        d.now=1000;for(unsigned i=0;i<9;++i)TB(recovery_trace(&d,111+i));
        TB(pgwt_transport_poll(d.transport,false)==0);
        unsigned reads=d.reads;
        if(mode==2)d.snapshot.component_valid[7]=false;else d.read_error=true;
        d.now+=mode==1?UINT64_C(9999999):UINT64_C(10000000);
        if(mode==3){
            struct pgwt_daemon *daemon=calloc(1,sizeof(*daemon));TB(daemon);
            daemon->exact_perf=true;daemon->merge_capture=d.capture;daemon->percpu_transport=d.transport;
            int rc=pgwt_merge_capture_poll(daemon);free(daemon);TB(rc==0);
        }else TB(pgwt_transport_poll(d.transport,false)==0);
        TB(d.reads>reads && pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
        TB(pgwt_capture_report(d.capture).faults[PGWT_MERGE_COUNTER_UNKNOWN]==1);
        TB(!pgwt_capture_capacity_paused(d.capture));
        TB(d.outputs[d.count-1].kind==PGWT_MERGE_OUT_VOID &&
           d.outputs[d.count-1].reason==PGWT_MERGE_COUNTER_UNKNOWN &&
           !d.outputs[d.count-1].counters.known);
        uint64_t attempts=pgwt_capture_coverage(d.capture,d.now).attempts;
        TB(attempts==(mode==1 || mode==3?0:1));
        d.read_error=false;d.snapshot=readable();d.now+=UINT64_C(5000000000);
        TB(pgwt_transport_poll(d.transport,false)==0);
        TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
        TB(pgwt_capture_coverage(d.capture,d.now).attempts==attempts);
        TB(pgwt_capture_coverage(d.capture,d.now).rearms==0);
        printf("counter-unknown mode=%u VOID=counter_unknown rearms=0 status=suspended\n",mode);
        TB(recovery_identity(&d));recovery_cleanup(&d);
    }
    return true;
}
static bool recovery_stale_round(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    d.now=200;TB(recovery_trace(&d,111));
    TB(pgwt_transport_poll(d.transport,true)==0);
    unsigned old=d.ctl.round;TB(old==2); /* outstanding E/L request */
    for(unsigned i=0;i<8;++i)TB(recovery_trace(&d,112+i));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    TB(recovery_retry(&d));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    uint64_t before=pgwt_capture_stats(d.capture).released_data;
    uint64_t ts=d.now+1;d.now+=10;TB(recovery_trace(&d,ts));
    TB(pgwt_transport_poll(d.transport,true)==0);
    /* Slow helpers now deliver the retired request's E/L pair. Their clock
     * values cover the new DATA, but their tokens cannot certify this epoch. */
    TB(tb_boundary(&d,0,1,old,d.now-1)==0);
    TB(tb_boundary(&d,0,2,old,d.now-1)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    unsigned ack=d.ctl.round;
    TB(tb_boundary(&d,0,0,ack,d.now-1)==0);
    TB(tb_boundary(&d,1,0,ack,d.now-1)==0);
    TB(pgwt_transport_poll(d.transport,false)==0);
    printf("stale-round old=%u request_or_ack=%u released_after_old_EL=%llu\n",
        old,ack,(unsigned long long)(pgwt_capture_stats(d.capture).released_data-before));
    TB(pgwt_capture_stats(d.capture).released_data==before);
    TB(recovery_round(&d,d.now-1));
    TB(pgwt_capture_stats(d.capture).released_data==before+1);
    TB(recovery_identity(&d));recovery_cleanup(&d);return true;
}
static bool recovery_time_identity(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    TB(recovery_identity(&d));
    /* Deliberate accounting error: even an overcount beyond the window is
     * visible in the public report and makes its printed identity red. */
    d.capture->covered_ns+=d.now-100;
    struct pgwt_capture_coverage c=pgwt_capture_coverage(d.capture,d.now);
    TB(c.covered_ns>c.window_ns);
    FILE *out=tmpfile();TB(out);
    pgwt_capture_coverage_print(out,d.capture,d.now,0,0,0,0);
    rewind(out);char line[2048];TB(fgets(line,sizeof(line),out));TB(fclose(out)==0);
    TB(strstr(line,"time_identity=0"));
    printf("corrupted coverage: %s",line);
    recovery_cleanup(&d);return true;
}
static bool recovery_proofs(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    d.now=1000;for(unsigned i=0;i<9;++i)TB(recovery_trace(&d,111+i));
    /* The run can no longer acquire its premise. Old MA_PROVED is insufficient. */
    d.capture->config.test_attach_premise=false;TB(recovery_retry(&d));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    TB(pgwt_capture_coverage(d.capture,d.now).proof_acquisitions==1);
    TB(d.capture->lifetimes[0].proof==MA_NOT_ATTEMPTED);
    d.capture->config.test_attach_premise=true;d.now+=UINT64_C(20000000);
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    TB(pgwt_capture_coverage(d.capture,d.now).proof_acquisitions==2);
    recovery_cleanup(&d);return true;
}
static bool recovery_excluded(bool lifetime)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    if(lifetime){
        for(unsigned i=0;i<8;++i){
            unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,31+i,4);tb_le(fork+16,111+i,8);
            TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,124+i,fork,24)==0);
        }
        TB(pgwt_capture_diagnostics(d.capture).allocation.pool==PGWT_POOL_CAPTURE_LIFETIME);
    }else TB(pgwt_capture_fault(d.capture,PGWT_MERGE_UNKNOWN_EVIDENCE,d.now,&d.snapshot)==0);
    for(unsigned i=0;i<30;++i){d.now+=UINT64_C(5000000000);TB(pgwt_transport_poll(d.transport,false)==0);}
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    TB(pgwt_capture_coverage(d.capture,d.now).attempts==0);
    TB(pgwt_capture_coverage(d.capture,d.now).rearms==0);
    recovery_cleanup(&d);return true;
}
static bool recovery_backoff(void)
{
    struct tb_driver d;TB(recovery_setup(&d,8,64,32));TB(recovery_fork(&d));
    d.now=1000;for(unsigned i=0;i<9;++i)TB(recovery_trace(&d,111+i));
    d.capture->config.test_attach_premise=false;
    TB(pgwt_transport_poll(d.transport,false)==0);
    uint64_t delay=UINT64_C(10000000),attempts=0;
    for(unsigned i=0;i<20;++i){
        d.now+=delay-1;TB(pgwt_transport_poll(d.transport,false)==0);
        TB(pgwt_capture_coverage(d.capture,d.now).attempts==attempts);
        ++d.now;TB(pgwt_transport_poll(d.transport,false)==0);++attempts;
        TB(pgwt_capture_coverage(d.capture,d.now).attempts==attempts);
        delay=delay>=UINT64_C(2500000000)?UINT64_C(5000000000):delay*2;
        TB(pgwt_transport_recovery_delay(delay)<=UINT64_C(5000000000));
    }
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    /* No give-up threshold: acquire after twenty failed attempts. */
    d.capture->config.test_attach_premise=true;d.now+=delay;
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
    TB(pgwt_capture_coverage(d.capture,d.now).attempts==21);
    recovery_cleanup(&d);return true;
}
int main(int argc,char **argv)
{
    const char *cases[]={"arrival","transport","staging","baseline","proofs","lifetime","noncapacity","backoff","retained","counter_change","retained_seal","classes","proof_time","proof_fault","stale_round","time_identity"};
    unsigned ran=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        if(argc>1 && strcmp(argv[1],cases[i]))continue;
        tb_guard=cases[i];printf("REACHED recovery %s\n",tb_guard);fflush(stdout);
        bool ok=i<3?recovery_cycles(i==0?PGWT_POOL_CAPTURE_ARRIVAL:i==1?PGWT_POOL_TRANSPORT_QUEUE:PGWT_POOL_CORE_STAGING):
            i==3?recovery_baseline():i==4?recovery_proofs():i<7?recovery_excluded(i==5):i==7?recovery_backoff():i==8?recovery_retained():i==9?recovery_counter_change():i==10?recovery_retained_seal():i==11?recovery_classes():i==12?recovery_proof_time():i==13?recovery_proof_fault():i==14?recovery_stale_round():recovery_time_identity();
        if(!ok){return 1;} printf("recovery %s PASS\n",tb_guard); ++ran;
    }
    printf("capacity recovery: %u/%u PASS\n",ran,ran);return ran?0:2;
}
