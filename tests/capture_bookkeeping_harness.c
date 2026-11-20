/* Synthetic wire replay for byte compatibility, pool reuse and host timing.
 * Include the binding to test its actual diagnostics and exact observation;
 * no Linux loader, kernel program or PostgreSQL process is executed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define PGWT_TRANSPORT_EMBEDDED
#include "test_percpu_transport.c"
#undef main
#define put_le capture_put_le
#include "../src/merge_capture.c"
#undef put_le

static bool file_write(void *ctx, const void *bytes, size_t n)
{ return fwrite(bytes, 1, n, ctx) == n; }
static bool discard_write(void *ctx, const void *bytes, size_t n)
{ (void)ctx; (void)bytes; (void)n; return true; }

static bool replay(unsigned rounds, const char *path, bool benchmark)
{
    struct tb_driver d; TB(tb_setup(&d));
    if (benchmark) {
        /* Match the deployed arrival-pool footprint; active bursts remain
         * identical in both binaries. Identity/reuse uses the small pool. */
        struct pgwt_capture_config cfg=d.capture->config;
        cfg.capacity=PGWT_CAPTURE_LIFETIME_CAPACITY;
        size_t owner=0;
        while(owner<64 && tb_live[owner].capture!=d.capture)++owner;
        TB(owner<64);
        pgwt_capture_destroy(d.capture);
        tb_live[owner].capture=NULL;
        d.capture=pgwt_capture_create(&cfg);TB(d.capture);
        tb_live[owner].capture=d.capture;
    }
    /* Retain the same callback context and replace only the consumer. */
    FILE *file = path ? fopen(path, "wb") : NULL;
    TB(!path || file);
    struct pgwt_capture_dump *dump = pgwt_capture_dump_create(128,
        file ? file_write : discard_write, file);
    TB(dump);
    d.capture->dump = dump;
    d.capture->config.sink = (struct pgwt_merge_capture_sink){
        .emit=pgwt_capture_dump, .ctx=dump, .flush=pgwt_capture_dump_flush};
    uint32_t cpus[] = {0,1}; TB(pgwt_capture_dump_header(dump,cpus,2));
    unsigned char fork[24] = {0};
    tb_le(fork,1,4); tb_le(fork+4,30,4); tb_le(fork+16,10,8);
    TB(tb_emit(&d,0,2,PGWT_WIRE_LIFECYCLE,123,fork,24)==0);
    uint64_t timestamp = 11;
    struct timespec start, end;
    TB(clock_gettime(CLOCK_MONOTONIC,&start)==0);
    for (unsigned round=0; round<rounds; ++round) {
        d.now = UINT64_C(10000) + (uint64_t)round*UINT64_C(10000000);
        for (unsigned i=0; i<64; ++i) {
            unsigned char trace[48]; memcpy(trace,golden_T,48);
            tb_le(trace,timestamp++,8); tb_le(trace+8,30,4);
            /* Distinct byte patterns must survive the internal slot token. */
            tb_le(trace+24,(uint64_t)round*64+i,8);
            TB(tb_emit(&d,0,0,PGWT_WIRE_TRACE,123,trace,48)==0);
        }
        pgwt_merge_diagnostics before_release=pgwt_capture_diagnostics(d.capture);
        if(before_release.allocation.pool) {
            fprintf(stderr,"POOL_EXHAUSTION");
            pgwt_pool_print(stderr,before_release.allocation.pool,before_release.allocation.usage);
            fputc('\n',stderr);
        }
        TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
        if (!benchmark) {
            unsigned char text[272];
            for (size_t i=0;i<sizeof(text);++i) text[i]=(unsigned char)(i+round);
            tb_le(text,4,4); tb_le(text+8,7,8);
            TB(tb_emit(&d,0,2,PGWT_WIRE_TEXT,0,text,sizeof(text))==0);
        }
        TB(pgwt_transport_poll(d.transport,false)==0);
        unsigned r = d.ctl.round;
        TB(tb_boundary(&d,0,1,r,timestamp+1)==0);
        TB(tb_boundary(&d,0,2,r,timestamp+1)==0);
        TB(pgwt_transport_poll(d.transport,false)==0);
        TB(d.ctl.round==r+1);
        d.now += 1234; /* exact phase/held/event times, independent of clock-call count */
        TB(tb_boundary(&d,0,0,r+1,timestamp+1)==0);
        TB(tb_boundary(&d,1,0,r+1,timestamp+1)==0);
        TB(pgwt_transport_poll(d.transport,false)==0);
        TB(pgwt_capture_report(d.capture).status==PGWT_MERGE_OK);
        pgwt_merge_diagnostics diagnostics=pgwt_capture_diagnostics(d.capture);
        TB(diagnostics.pools[PGWT_POOL_CAPTURE_ARRIVAL].occupancy==0);
        timestamp+=2; /* next DATA is after the accepted boundaries */
    }
#ifndef PGWT_CAPTURE_LEGACY
    TB(d.capture->arrival_probes==0);
#endif
    TB(clock_gettime(CLOCK_MONOTONIC,&end)==0);
    if (benchmark) {
        double elapsed=(double)(end.tv_sec-start.tv_sec)+(double)(end.tv_nsec-start.tv_nsec)/1e9;
        uint64_t records=pgwt_transport_stats(d.transport).records;
        printf("%.0f records/s (%llu records)\n",(double)records/elapsed,(unsigned long long)records);
    } else {
        struct pgwt_daemon *daemon=calloc(1,sizeof(*daemon)); TB(daemon);
        daemon->exact_perf=true; daemon->merge_capture=d.capture; daemon->percpu_transport=d.transport;
        TB(pgwt_merge_capture_finish(daemon)==0);
        free(daemon);
        struct pgwt_capture_stats stats=pgwt_capture_stats(d.capture);
        TB(stats.released_data==1+rounds*64ULL && stats.released_text==rounds);

    }
    pgwt_capture_dump_destroy(dump);
    tb_cleanup(&d);
    TB(!file || fclose(file)==0);
    return true;
}

static bool local_snapshot(void)
{
    struct tb_driver d;TB(tb_setup(&d));
    d.capture->config.writer_failed=NULL;
    d.lifecycle_error=-EIO;
    unsigned char payload[24]={0};tb_le(payload,1,4);
    ma_envelope e={.version=1,.kind=MA_LIFECYCLE,.source=3,.position=1,.bytes=payload,.size=24};
    TB(pgwt_capture_received(d.capture,&e,20)==-EIO);
    ma_snapshot s=pgwt_capture_last_snapshot(d.capture);
    TB(!s.valid && s.component_valid[0] && s.component_valid[14] && s.value[14]==0);
    tb_cleanup(&d);return true;
}

#ifndef PGWT_CAPTURE_LEGACY
static bool duplicate_checks(void)
{
    struct tb_driver d;TB(tb_setup(&d));
    ma_envelope e={.version=1,.kind=MA_TRACE,.source=0,.position=2,.cookie=123,
        .cookie_present=true,.bytes=golden_T,.size=48};
    size_t token;
    TB(pgwt_capture_received_token(d.capture,&e,20,&token)==0 && token);
    e.position=1;TB(pgwt_capture_received_token(d.capture,&e,20,&token)==0);
    e.position=2;TB(pgwt_capture_received_token(d.capture,&e,20,&token)==-EIO);
    TB(pgwt_capture_report(d.capture).faults[PGWT_MERGE_DUPLICATE_IDENTITY]==1);
    tb_cleanup(&d);return true;
}
static bool published_clock(void)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d)); TB(d);
    struct pgwt_merge_capture c={.observation={.d=1},.budget={.started=1}};
    d->merge_capture=&c;
    pgwt_merge_observation o={.d=1,.counters={.known=true}};
    struct timespec ts;
    TB(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
    uint64_t before=(uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
    TB(daemon_observe(d,&o)==0);
    TB(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
    uint64_t after=(uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
    free(d);
    TB(o.d>=before && o.d<=after);
    return true;
}
#endif
static int bookkeeping_main(int argc,char **argv)
{
    tb_guard=argc>1?argv[1]:"identity";
    printf("REACHED bookkeeping %s\n",tb_guard);fflush(stdout);
    if(argc>1 && !strcmp(argv[1],"local_snapshot"))return local_snapshot()?0:1;
    if(argc>1 && !strcmp(argv[1],"bench"))return replay(20000,NULL,true)?0:1;
#ifndef PGWT_CAPTURE_LEGACY
    if(argc>1 && !strcmp(argv[1],"clock"))return published_clock()?0:1;
    if(argc>1 && !strcmp(argv[1],"duplicate"))return duplicate_checks()?0:1;
#endif
    return replay(8,argc>2?argv[2]:NULL,false)?0:1;
}

int main(int argc,char **argv)
{
    int rc=bookkeeping_main(argc,argv);
    /* Failure mutants return through TB; release the ordinary fixture's
     * retained allocations before returning the intended assertion status. */
    for(size_t i=0;i<64;++i) {
        if(tb_live[i].capture)pgwt_capture_dump_destroy(tb_live[i].capture->dump);
        pgwt_transport_destroy(tb_live[i].transport);
        pgwt_capture_destroy(tb_live[i].capture);
    }
    return rc;
}
