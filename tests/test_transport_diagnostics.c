/* Reuse the portable byte/schedule driver, not the existing test entry point.
 * Each expected capacity, pool name and duration below is supplied by this
 * test. No contract/answer/tolerance changes and no Linux/BPF execution. */
#define PGWT_TRANSPORT_EMBEDDED
#include "test_percpu_transport.c"
#undef main
#undef PGWT_TRANSPORT_EMBEDDED

static bool diagnostic_failure(pgwt_allocation_failure a, pgwt_pool pool,
                                 const char *name, uint64_t occupancy, uint64_t capacity,
                                 uint64_t high)
{
    TB(a.pool==pool && !strcmp(pgwt_pool_name(a.pool),name));
    TB(a.usage.occupancy==occupancy && a.usage.capacity==capacity && a.usage.high==high);
    FILE *f=tmpfile();TB(f);
    pgwt_pool_print(f,a.pool,a.usage);rewind(f);
    char actual[256]={0},expected[256];TB(fgets(actual,sizeof(actual),f));fclose(f);
    snprintf(expected,sizeof(expected)," pool=%s occupancy=%llu capacity=%llu high=%llu",name,
        (unsigned long long)occupancy,(unsigned long long)capacity,(unsigned long long)high);
    TB(!strcmp(actual,expected));
    f=tmpfile();TB(f);
    pgwt_merge_output record={.kind=PGWT_MERGE_OUT_VOID,.reason=PGWT_MERGE_ALLOCATION_FAIL,.d=123,.allocation=a};
    pgwt_suspension_print(f,&record);rewind(f);
    TB(fgets(actual,sizeof(actual),f));fclose(f);
    snprintf(expected,sizeof(expected),"exact-perf: suspended: allocation_fail at 123; automatic REARM %s pool=%s occupancy=%llu capacity=%llu high=%llu\n",
        pool==PGWT_POOL_TRANSPORT_QUEUE || pool==PGWT_POOL_CAPTURE_ARRIVAL || pool==PGWT_POOL_CORE_STAGING ?
            "pending fresh proofs" : "disabled", name,
        (unsigned long long)occupancy,(unsigned long long)capacity,(unsigned long long)high);
    TB(!strcmp(actual,expected));
    return true;
}
static bool diagnostic_queue(void)
{
    struct tb_driver d;TB(tb_setup(&d));
    TB(pgwt_transport_poll(d.transport,true)==0);
    for(unsigned i=0;i<128;++i)TB(tb_boundary(&d,0,1,1,13)==0);
    struct pgwt_transport_stats stats=pgwt_transport_stats(d.transport);
    TB(stats.queue.occupancy==128 && stats.queue.capacity==128 && stats.queue.high==128);
    TB(!stats.allocation.pool && !d.count);
    TB(tb_boundary(&d,0,1,1,13)==0);
    TB(d.count==1 && d.outputs[0].reason==PGWT_MERGE_ALLOCATION_FAIL);
    TB(diagnostic_failure(d.outputs[0].allocation,PGWT_POOL_TRANSPORT_QUEUE,"transport_queue",128,128,128));
    TB(diagnostic_failure(pgwt_transport_stats(d.transport).allocation,
        PGWT_POOL_TRANSPORT_QUEUE,"transport_queue",128,128,128));
    TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    unsigned wakes=d.wakes;TB(pgwt_transport_poll(d.transport,true)==0 && wakes==d.wakes);
    tb_cleanup(&d);return true;
}
static bool diagnostic_capture(pgwt_pool pool, const char *name)
{
    struct tb_driver d={.snapshot=readable()};
    struct pgwt_capture_config config={.cpu_count=1,.capacity=2,
        .baseline={.counters={.known=true}},.sink={.emit=tb_sink,.ctx=&d}};
    d.capture=pgwt_capture_create(&config);TB(d.capture);
    /* Identity and binding histories keep filling during suspension, while
     * arrival tracking intentionally stops. This isolates those two pools. */
    if(pool==PGWT_POOL_CAPTURE_LIFETIME || pool==PGWT_POOL_CAPTURE_BINDING)
        TB(pgwt_capture_fault(d.capture,PGWT_MERGE_UNKNOWN_EVIDENCE,10,&d.snapshot)==0);
    for(unsigned i=1;i<=3;++i){
        unsigned char payload[48]={0};
        ma_envelope e={.version=1,.origin=MA_EXACT,.source=0,.position=i,.cookie=7,
            .cookie_present=true,.kind=MA_TRACE,.bytes=payload,.size=48};
        tb_le(payload,10+i,8);tb_le(payload+8,30,4);
        if(pool==PGWT_POOL_CAPTURE_LIFETIME){
            e.kind=MA_LIFECYCLE;e.source=2;e.size=24;e.cookie=i;
            memset(payload,0,sizeof(payload));tb_le(payload,1,4);tb_le(payload+4,i+30,4);tb_le(payload+16,10+i,8);
        }else if(pool==PGWT_POOL_CAPTURE_BINDING)tb_le(payload+32,i,8);
        int rc;
        if(pool==PGWT_POOL_CAPTURE_RETAINED){
            e.kind=MA_BOUNDARY;e.bytes=NULL;e.size=0;e.boundary=MA_NO_BOUNDARY;
            rc=pgwt_capture_envelope(d.capture,&e,20,&d.snapshot);
        }else{
            rc=pgwt_capture_received(d.capture,&e,20);
            TB(rc==(i==3?-ENOMEM:0));
            if(rc==-ENOMEM)rc=pgwt_capture_fault(d.capture,PGWT_MERGE_ALLOCATION_FAIL,20,&d.snapshot);
        }
        TB(rc==0);
        if(i==2){
            pgwt_merge_diagnostics s=pgwt_capture_diagnostics(d.capture);
            TB(s.pools[pool].occupancy==2 && s.pools[pool].capacity==2 && s.pools[pool].high==2);
            TB(!s.allocation.pool);
        }
    }
    TB(d.count && d.outputs[d.count-1].reason==PGWT_MERGE_ALLOCATION_FAIL);
    TB(diagnostic_failure(d.outputs[d.count-1].allocation,pool,name,2,2,2));
    pgwt_merge_diagnostics s=pgwt_capture_diagnostics(d.capture);
    TB(diagnostic_failure(s.allocation,pool,name,2,2,2));
    TB(s.pools[pool].high==2 && pgwt_capture_report(d.capture).status==PGWT_MERGE_SUSPENDED);
    pgwt_capture_destroy(d.capture);return true;
}
static bool diagnostic_core(pgwt_pool pool, const char *name)
{
    struct tb_driver d={0};
    pgwt_merge_source sources[3]={{.qualified=true},{.qualified=true},{.qualified=true}};
    pgwt_merge_config config={.sources=sources,.source_count=3,.staged_capacity=8,
        .attachment_capacity=8,.payload_capacity=16,.dependency_capacity=8,.frontier_capacity=8,
        .observation={.counters={.known=true}},.last_observed={.known=true},
        .sink=tb_sink,.sink_context=&d};
    if(pool==PGWT_POOL_CORE_STAGING)config.staged_capacity=2;
    if(pool==PGWT_POOL_CORE_IDENTITY)config.attachment_capacity=2;
    if(pool==PGWT_POOL_CORE_DEPENDENCY)config.dependency_capacity=2;
    if(pool==PGWT_POOL_CORE_PAYLOAD)config.payload_capacity=4;
    if(pool==PGWT_POOL_CORE_FRONTIER){config.frontier_capacity=1;config.has_frontier=true;config.frontier=5;}
    pgwt_merge_status status;pgwt_merge *core=pgwt_merge_create(&config,&status);TB(core && status==PGWT_MERGE_OK);
    pgwt_merge_input in={.observation=config.observation};
    if(pool==PGWT_POOL_CORE_FRONTIER){
        pgwt_merge_diagnostics initial=pgwt_merge_get_diagnostics(core);
        TB(initial.pools[pool].occupancy==1 && initial.pools[pool].capacity==1 && initial.pools[pool].high==1);
        TB(!initial.allocation.pool);
        in.kind=PGWT_MERGE_FAULT;in.reason=PGWT_MERGE_UNKNOWN_EVIDENCE;
        TB(pgwt_merge_feed(core,&in)==PGWT_MERGE_SUSPENDED);
        in.kind=PGWT_MERGE_REARM;in.attachment_proofs_fresh=true;
        TB(pgwt_merge_feed(core,&in)==PGWT_MERGE_OK);
        for(size_t i=0;i<3;++i){
            in.kind=PGWT_MERGE_BOUNDARY;in.source=i;in.p=1;in.w=20;
            status=pgwt_merge_feed(core,&in);
        }
    }else{
        unsigned char bytes[8]={0};
        for(unsigned i=1;i<=3;++i){
            in.kind=pool==PGWT_POOL_CORE_DEPENDENCY?PGWT_MERGE_TEXT:PGWT_MERGE_DATA;
            in.record=(pgwt_merge_record){.source=pool==PGWT_POOL_CORE_DEPENDENCY?2:0,
                .o=i,.t=10+i,.pid=pool==PGWT_POOL_CORE_IDENTITY?30+i:30,.g=7,
                .rank=PGWT_MERGE_TRANS,.identity_known=true,.payload=bytes,.payload_size=4,
                .dependency_id=i};
            if(pool==PGWT_POOL_CORE_PAYLOAD && i==2)in.record.payload_size=8;
            status=pgwt_merge_feed(core,&in);
            if(status!=PGWT_MERGE_OK)break;
            pgwt_merge_diagnostics successful=pgwt_merge_get_diagnostics(core);
            uint64_t occupied=pool==PGWT_POOL_CORE_PAYLOAD?4:i;
            TB(successful.pools[pool].occupancy==occupied && successful.pools[pool].high==occupied);
            TB(!successful.allocation.pool);
        }
    }
    TB(status==PGWT_MERGE_SUSPENDED && d.count);
    pgwt_allocation_failure a=d.outputs[d.count-1].allocation;
    uint64_t capacity=pool==PGWT_POOL_CORE_PAYLOAD?4:pool==PGWT_POOL_CORE_FRONTIER?1:2;
    TB(diagnostic_failure(a,pool,name,capacity,capacity,capacity));
    TB(diagnostic_failure(pgwt_merge_get_diagnostics(core).allocation,pool,name,capacity,capacity,capacity));
    /* A VOID clears staging, but its pre-discard occupancy and high survive. */
    if(pool==PGWT_POOL_CORE_STAGING){
        pgwt_merge_diagnostics s=pgwt_merge_get_diagnostics(core);
        TB(s.pools[pool].occupancy==0 && s.pools[pool].high==2);
    }
    pgwt_merge_destroy(core);return true;
}
/* Release slot zero while slot one remains live, then reuse slot zero.
 * Current payload usage must follow live slots, not a dense queue prefix or
 * stale bytes in the free list. High water must survive every release. */
static bool diagnostic_sparse_slots(void)
{
    struct tb_driver d={0};
    pgwt_merge_source sources[3]={{.qualified=true},{.qualified=true},{.qualified=true}};
    pgwt_merge_attachment proof={.pid=30,.g=7,.identity_proved=true};
    pgwt_merge_config config={.sources=sources,.source_count=3,.staged_capacity=3,
        .attachment_capacity=1,.attachments=&proof,.attachment_count=1,.payload_capacity=16,
        .observation={.counters={.known=true}},.last_observed={.known=true},
        .sink=tb_sink,.sink_context=&d};
    pgwt_merge_status status;pgwt_merge *core=pgwt_merge_create(&config,&status);
    TB(core && status==PGWT_MERGE_OK);
    unsigned char bytes[16]={0};
    pgwt_merge_input in={.kind=PGWT_MERGE_DATA,.observation=config.observation,
        .record={.source=0,.o=1,.t=10,.pid=30,.g=7,.rank=PGWT_MERGE_TRANS,
            .identity_known=true,.payload=bytes,.payload_size=4}};
    TB(pgwt_merge_feed(core,&in)==PGWT_MERGE_OK);
    in.record.o=2;in.record.t=20;in.record.payload_size=12;
    TB(pgwt_merge_feed(core,&in)==PGWT_MERGE_OK);
    for(size_t source=1;source<3;++source){
        pgwt_merge_input boundary={.kind=PGWT_MERGE_BOUNDARY,.source=source,.p=1,.w=15,
            .observation=config.observation};
        TB(pgwt_merge_feed(core,&boundary)==PGWT_MERGE_OK);
    }
    pgwt_merge_diagnostics stats=pgwt_merge_get_diagnostics(core);
    TB(stats.pools[PGWT_POOL_CORE_STAGING].occupancy==1);
    TB(stats.pools[PGWT_POOL_CORE_STAGING].capacity==3 && stats.pools[PGWT_POOL_CORE_STAGING].high==2);
    TB(stats.pools[PGWT_POOL_CORE_PAYLOAD].occupancy==12);
    TB(stats.pools[PGWT_POOL_CORE_PAYLOAD].capacity==16 && stats.pools[PGWT_POOL_CORE_PAYLOAD].high==12);
    in.record.o=3;in.record.t=30;in.record.payload_size=8;
    TB(pgwt_merge_feed(core,&in)==PGWT_MERGE_OK);
    for(size_t source=1;source<3;++source){
        pgwt_merge_input boundary={.kind=PGWT_MERGE_BOUNDARY,.source=source,.p=2,.w=25,
            .observation=config.observation};
        TB(pgwt_merge_feed(core,&boundary)==PGWT_MERGE_OK);
    }
    stats=pgwt_merge_get_diagnostics(core);
    TB(stats.pools[PGWT_POOL_CORE_STAGING].occupancy==1 && stats.pools[PGWT_POOL_CORE_STAGING].high==2);
    TB(stats.pools[PGWT_POOL_CORE_PAYLOAD].occupancy==8 && stats.pools[PGWT_POOL_CORE_PAYLOAD].high==12);
    in.kind=PGWT_MERGE_FAULT;in.reason=PGWT_MERGE_UNKNOWN_EVIDENCE;
    TB(pgwt_merge_feed(core,&in)==PGWT_MERGE_SUSPENDED);
    stats=pgwt_merge_get_diagnostics(core);
    TB(stats.pools[PGWT_POOL_CORE_STAGING].occupancy==0 && stats.pools[PGWT_POOL_CORE_STAGING].high==2);
    TB(stats.pools[PGWT_POOL_CORE_PAYLOAD].occupancy==0 && stats.pools[PGWT_POOL_CORE_PAYLOAD].high==12);
    pgwt_merge_destroy(core);return true;
}
static bool diagnostic_distribution(struct pgwt_distribution d,uint64_t count,uint64_t total,uint64_t max)
{
    TB(d.count==count && d.total==total && d.max==max);
    uint64_t buckets=0;for(size_t i=0;i<8;++i)buckets+=d.histogram[i];TB(buckets==count);
    return true;
}
static bool diagnostic_phases(void)
{
    struct tb_driver d;TB(tb_setup(&d));
    unsigned char fork[24]={0};tb_le(fork,1,4);tb_le(fork+4,30,4);tb_le(fork+16,10,8);
    TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    d.now=100;TB(pgwt_transport_poll(d.transport,false)==0);
    d.now=150;TB(tb_boundary(&d,0,1,1,13)==0);
    TB(pgwt_transport_stats(d.transport).request_to_el.count==0);
    d.now=200;TB(tb_boundary(&d,1,2,1,13)==0);
    /* Duplicate E boundary does not produce another timing sample. */
    d.now=220;TB(tb_boundary(&d,0,1,1,13)==0);
    d.now=250;TB(pgwt_transport_poll(d.transport,false)==0);
    d.now=300;TB(tb_boundary(&d,0,0,2,14)==0);
    TB(pgwt_transport_stats(d.transport).fence_to_ack.count==0);
    d.now=400;TB(tb_boundary(&d,1,0,2,14)==0);
    d.now=420;TB(tb_boundary(&d,0,0,2,14)==0);
    d.now=500;d.envelope_cost_ns=10;
    /* First replay slice admits 3 of the 7 records, then yields at 530. */
    pgwt_transport_begin_pass(d.transport,530);
    TB(pgwt_transport_poll(d.transport,false)==0);
    struct pgwt_transport_stats s=pgwt_transport_stats(d.transport);
    TB(s.rounds_completed==0 && s.release_budget_yields==1);
    TB(s.ack_to_release_end.count==0 && s.release_replay.count==0);
    d.now=1000;pgwt_transport_begin_pass(d.transport,2000);
    TB(pgwt_transport_poll(d.transport,false)==0);
    s=pgwt_transport_stats(d.transport);
    TB(s.rounds_completed==1 && d.now==1040);
    TB(diagnostic_distribution(s.request_to_el,1,100,100));
    TB(diagnostic_distribution(s.fence_to_ack,1,150,150));
    TB(diagnostic_distribution(s.ack_to_release_end,1,640,640));
    TB(diagnostic_distribution(s.release_replay,1,70,70));
    TB(diagnostic_distribution(s.records_per_round,1,7,7));
    TB(s.round_ns_total==940 && s.release_replay.max<=s.ack_to_release_end.max);
    FILE *f=tmpfile();TB(f);pgwt_distribution_print(f,"request_to_el",&s.request_to_el,false);
    rewind(f);char line[256];TB(fgets(line,sizeof(line),f));fclose(f);
    TB(!strcmp(line,"exact-perf: phase name=request_to_el unit=ns count=1 mean=100 max=100 hist=1,0,0,0,0,0,0,0\n"));
    d.now=2000;TB(pgwt_transport_poll(d.transport,true)==0);
    d.now=2010;TB(tb_boundary(&d,0,1,2,30)==0);
    d.now=2020;TB(tb_boundary(&d,1,2,2,30)==0);
    d.now=2050;TB(pgwt_transport_poll(d.transport,false)==0);
    d.now=2060;TB(tb_boundary(&d,0,0,3,31)==0);
    d.now=2070;TB(tb_boundary(&d,1,0,3,31)==0);
    d.now=2100;pgwt_transport_begin_pass(d.transport,4000);
    TB(pgwt_transport_poll(d.transport,false)==0);
    s=pgwt_transport_stats(d.transport);
    TB(s.rounds_completed==2 && d.now==2140);
    TB(diagnostic_distribution(s.request_to_el,2,120,100));
    TB(diagnostic_distribution(s.fence_to_ack,2,170,150));
    TB(diagnostic_distribution(s.ack_to_release_end,2,710,640));
    TB(diagnostic_distribution(s.release_replay,2,110,70));
    TB(diagnostic_distribution(s.records_per_round,2,11,7));
    tb_cleanup(&d);return true;
}
static bool diagnostic_histogram_schedstat(void)
{
    struct pgwt_distribution d={0};
    const uint64_t values[]={0,100000,1000000,10000000,100000000,1000000000,UINT64_C(3000000000),UINT64_C(10000000000)};
    for(size_t i=0;i<8;++i)pgwt_distribution_add(&d,values[i],false);
    for(size_t i=0;i<8;++i)TB(d.histogram[i]==1);
    FILE *f=tmpfile();TB(f);fputs("111 222 333\n",f);rewind(f);uint64_t delay=999;
    TB(pgwt_schedstat_parse(f,&delay) && delay==222);fclose(f);
    f=tmpfile();TB(f);fputs("111 malformed\n",f);rewind(f);delay=999;
    TB(!pgwt_schedstat_parse(f,&delay) && delay==999);fclose(f);
    struct pgwt_schedstat_sample sample={.start=222,.end=555,.start_valid=true,.end_valid=true};
    f=tmpfile();TB(f);pgwt_schedstat_print(f,"helper",7,3,sample);rewind(f);
    char line[256];TB(fgets(line,sizeof(line),f));fclose(f);
    TB(!strcmp(line,"exact-perf: schedstat role=helper tid=7 cpu=3 start_valid=1 end_valid=1 start_ns=222 end_ns=555 delta_ns=333\n"));
    sample.end=111;
    f=tmpfile();TB(f);pgwt_schedstat_print(f,"reader",8,UINT32_MAX,sample);rewind(f);
    TB(fgets(line,sizeof(line),f));fclose(f);TB(strstr(line,"delta_ns=unknown"));
    sample.start_valid=false;
    f=tmpfile();TB(f);pgwt_schedstat_print(f,"helper",7,3,sample);rewind(f);
    TB(fgets(line,sizeof(line),f));fclose(f);TB(strstr(line,"start_ns=unknown") && strstr(line,"delta_ns=unknown"));
    return true;
}
int main(int argc,char **argv)
{
    tb_guard="diagnostics";
    if(argc==2 && !strcmp(argv[1],"phases")){puts("REACHED diagnostics phases");return diagnostic_phases()?0:1;}
    if(argc==2 && !strcmp(argv[1],"arrival")){puts("REACHED diagnostics arrival");return diagnostic_capture(PGWT_POOL_CAPTURE_ARRIVAL,"capture_arrival")?0:1;}
    if(argc==2 && !strcmp(argv[1],"sparse_slots")){puts("REACHED diagnostics sparse_slots");return diagnostic_sparse_slots()?0:1;}
    if(argc==2 && !strcmp(argv[1],"core_staging")){puts("REACHED diagnostics core_staging");return diagnostic_core(PGWT_POOL_CORE_STAGING,"core_staging")?0:1;}
    if(!diagnostic_queue() || !diagnostic_sparse_slots())return 1;
    const struct {pgwt_pool pool;const char *name;} capture[]={
        {PGWT_POOL_CAPTURE_ARRIVAL,"capture_arrival"},{PGWT_POOL_CAPTURE_LIFETIME,"capture_lifetime"},
        {PGWT_POOL_CAPTURE_BINDING,"capture_binding"},{PGWT_POOL_CAPTURE_RETAINED,"capture_retained"}};
    for(size_t i=0;i<4;++i)if(!diagnostic_capture(capture[i].pool,capture[i].name))return 1;
    const struct {pgwt_pool pool;const char *name;} core[]={
        {PGWT_POOL_CORE_STAGING,"core_staging"},{PGWT_POOL_CORE_IDENTITY,"core_identity"},
        {PGWT_POOL_CORE_DEPENDENCY,"core_dependency"},{PGWT_POOL_CORE_PAYLOAD,"core_payload"},
        {PGWT_POOL_CORE_FRONTIER,"core_frontier"}};
    for(size_t i=0;i<5;++i)if(!diagnostic_core(core[i].pool,core[i].name))return 1;
    if(!diagnostic_phases() || !diagnostic_histogram_schedstat())return 1;
    puts("transport diagnostics: 10 pool fills, phase times/replay yields, histograms and schedstat PASS");
    return 0;
}
