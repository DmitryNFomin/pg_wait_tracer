/* Compiled around the actual pgwt_tick_duties body by the registered mutant
 * runner. No BPF/Linux loader: replace external effects, retain real duty
 * order, guards, rate arithmetic, --count, and writer retention conditions. */
#define _POSIX_C_SOURCE 200809L
#include "daemon.h"
#include "capture_counters.h"
#include "event_stream.h"
#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
static char calls[1024];
static size_t used;
static bool dead, bad_drain;
static uint64_t pending_records = 7;
static unsigned backlog_warnings;
static int tick_fprintf(FILE *stream,const char *format,...)
{
    if(stream==stderr && strstr(format,"WARN: exact-perf per-tick drain left ")==format)++backlog_warnings;
    va_list args;va_start(args,format);int rc=vfprintf(stream,format,args);va_end(args);return rc;
}
#define fprintf tick_fprintf
static uint64_t service_now = 1;
static int tick_clock_gettime(clockid_t clock, struct timespec *ts)
{
    (void)clock;ts->tv_sec=(time_t)(service_now/1000000000);
    ts->tv_nsec=(long)(service_now%1000000000);return 0;
}
#define clock_gettime tick_clock_gettime
/* Only the fields used by the verbatim capture poll body below. */
struct pgwt_merge_capture {
    pgwt_merge_observation observation;
    uint64_t last_checkpoint_at,last_service_at;
    bool serviced;
};
static unsigned core_checks;
static bool transport_silent;
static void called(const char *name)
{ size_t n=strlen(name);if(used+n+2>=sizeof(calls))abort();memcpy(calls+used,name,n);used+=n;calls[used++]=' ';calls[used]=0; }
#define TICK(expr) do {if(!(expr)){fprintf(stderr,"transport ASSERT tick_duties: %s (line %d)\n",#expr,__LINE__);return false;}}while(0)
uint64_t pgwt_debug_monotonic_ns(void){return 123456789;}
void pgwt_debug_block_report(const char *op,pid_t pid,uint64_t at){(void)op;(void)pid;(void)at;}
static int tick_kill(pid_t pid,int sig){(void)sig;if(pid==10){called("health");return dead?-1:0;}called("gc_probe");return -1;}
#define kill tick_kill
struct pgwt_effective_cores_result pgwt_effective_cores_refresh(struct pgwt_effective_cores_resolver *r,pid_t pid)
{(void)r;(void)pid;called("capacity");return (struct pgwt_effective_cores_result){0};}
int pgwt_sweep_bootstrap_backends(struct pgwt_daemon *d){(void)d;called("init");return 0;}
int pgwt_recover_unattached_backends(struct pgwt_daemon *d){(void)d;called("recovery");return 0;}
int pgwt_sweep_stale_state(struct pgwt_daemon *d){(void)d;called("stale");return 0;}
void pgwt_escalation_check_budget(struct pgwt_daemon *d){(void)d;called("budget");}
void pgwt_read_accum_map(struct pgwt_daemon *d){(void)d;called("accum_read");}
static void drain_event_ring_for_tick(struct pgwt_daemon *d){called("drain");d->event_accum->tm.db_time_ns+=7;}
/* Production consumes into the transport/coordinator, not event_accum. */
int pgwt_percpu_transport_poll(struct pgwt_daemon *d){(void)d;called("transport_drain");return bad_drain?-1:0;}
uint64_t pgwt_merge_capture_pending_records(const struct pgwt_daemon *d)
{(void)d;called("backlog");return pending_records;}
int pgwt_merge_capture_poll(struct pgwt_daemon *d){(void)d;called("check");return 0;}
void pgwt_accum_copy_used(struct pgwt_accumulator *dst,const struct pgwt_accumulator *src)
{called("copy");dst->tm=src->tm;}
void pgwt_read_state_map(struct pgwt_daemon *d){called("state");d->accum.tm.db_time_ns+=3;}
void pgwt_ring_push(struct pgwt_ring *r,const struct pgwt_accumulator *a){(void)r;(void)a;called("snapshot");}
#define VIEW(name) void name(struct pgwt_daemon *d){(void)d;called("view");}
VIEW(pgwt_print_time_model) VIEW(pgwt_print_system_event) VIEW(pgwt_print_session_event)
VIEW(pgwt_print_histogram) VIEW(pgwt_print_query_event) VIEW(pgwt_print_active)
int pgwt_handle_exit(struct pgwt_daemon *d,pid_t pid){(void)d;(void)pid;called("exit");return 0;}
int pgwt_writer_check_rotation(struct pgwt_event_writer *w){(void)w;called("rotate_raw");return 0;}
int pgwt_summary_flush_completed(struct pgwt_summary_writer *w,uint64_t at){(void)w;(void)at;called("summary_flush");return 0;}
int pgwt_summary_check_rotation(struct pgwt_summary_writer *w){(void)w;called("rotate_summary");return 0;}
int pgwt_writer_cleanup_old_files(struct pgwt_event_writer *w){(void)w;called("retain_raw");return 0;}
int pgwt_summary_cleanup_old_files(struct pgwt_summary_writer *w){(void)w;called("retain_summary");return 0;}
struct pgwt_counter_value pgwt_read_bpf_fail_counter(struct pgwt_daemon *d,uint32_t slot)
{(void)d;(void)slot;called("fail_read");return (struct pgwt_counter_value){0,true};}
static int daemon_observe(void *,pgwt_merge_observation *);
static unsigned publication_lookups;
int pgwt_merge_capture_snapshot(struct pgwt_daemon *d,ma_snapshot *s,uint64_t *now)
{
    publication_lookups+=13;memset(s,0,sizeof(*s));s->valid=true;*now=service_now;
    if(d->merge_capture)d->merge_capture->last_checkpoint_at=*now;
    for(size_t i=0;i<15;++i)s->component_valid[i]=true;
    return 0;
}
int pgwt_percpu_transport_snapshot(struct pgwt_daemon *d,ma_snapshot *s,uint64_t *now)
{return pgwt_merge_capture_snapshot(d,s,now);}
ma_snapshot pgwt_capture_last_snapshot(const struct pgwt_merge_capture *c)
{(void)c;ma_snapshot s={.valid=true};for(size_t i=0;i<15;++i)s.component_valid[i]=true;return s;}
int pgwt_capture_envelope(struct pgwt_merge_capture *c,const ma_envelope *e,uint64_t now,const ma_snapshot *s)
{(void)c;(void)now;(void)s;if(e->kind!=MA_CHECK)abort();++core_checks;return 0;}
bool pgwt_transport_silent(const struct pgwt_percpu_transport *t,uint64_t now,uint64_t interval)
{(void)t;(void)now;(void)interval;return transport_silent;}
static int capture_poll_under_test(struct pgwt_daemon *);
static bool cadence_cases(void)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TICK(d);
    struct pgwt_merge_capture c={.observation={.counters={.known=true}}};
    d->exact_perf=true;d->merge_capture=&c;core_checks=publication_lookups=0;
    /* Continuous DATA/watermark readiness: standalone core CHECK is once
     * per clock slot, with full counter reads reserved for silence. */
    for(unsigned pass=0;pass<100000;++pass){service_now=pass*100;TICK(capture_poll_under_test(d)==0);}
    TICK(core_checks==1 && publication_lookups==0);
    service_now=10000000;TICK(capture_poll_under_test(d)==0 && core_checks==2 && publication_lookups==0);
    transport_silent=true;service_now=20000000;
    TICK(capture_poll_under_test(d)==0 && core_checks==3 && publication_lookups==13 && c.last_checkpoint_at==service_now);
    service_now++;TICK(capture_poll_under_test(d)==0 && core_checks==3 && publication_lookups==13);
    service_now=30000000;TICK(capture_poll_under_test(d)==0 && core_checks==4 && publication_lookups==26);
    transport_silent=false;free(d);
    puts("actual capture poll: 100000 early calls, 1 core CHECK; silent full vector once/10 ms PASS");
    return true;
}
static bool publication_counters(void)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TICK(d);
    pgwt_merge_observation o={.counters={.known=true}};
    for(size_t i=0;i<14;++i)o.counters.value[i]=100+i;
    for(unsigned record=0;record<1000;++record){
        d->counters.writer_push_fail_total=record;
        TICK(daemon_observe(d,&o)==0 && o.counters.known && o.counters.value[14]==record && o.d>0);
        for(size_t i=0;i<14;++i)TICK(o.counters.value[i]==100+i);
    }
    TICK(publication_lookups==0);free(d);
    puts("actual daemon publication callback: 1000 refreshes, writer component only, 0 map lookups/DATA PASS");
    return true;
}
/* Only the fields accessed by the actual Linux drain body are required.
 * libbpf's successful consume returns ZERO even after delivering samples. */
struct pgwt_percpu_transport { uint64_t *positions; struct pgwt_transport_budget budget; void *platform; };
struct perf_buffer { struct pgwt_percpu_transport *transport;size_t source,records; bool hint; };
struct ring_context {struct pgwt_daemon *d;uint32_t dest;size_t consumed,limit;};
struct ring_buffer {struct ring_context *ctx;size_t records;};
struct linux_transport {struct pgwt_daemon *d;size_t ncpu,next_reader;struct perf_buffer **perf,**hints;
    struct ring_buffer *rings[2];struct ring_context ring_ctx[2];int callback_error;};
enum bpf_perf_event_ret {LIBBPF_PERF_EVENT_ERROR=-1,LIBBPF_PERF_EVENT_CONT=0,LIBBPF_PERF_EVENT_DONE=1};
struct perf_event_header {uint32_t type;uint16_t misc,size;};
enum {PERF_RECORD_SAMPLE=9,PERF_RECORD_LOST=2};
static uint64_t record_cost;
bool pgwt_transport_pass_expired(struct pgwt_percpu_transport *t)
{return pgwt_budget_expired(&t->budget,service_now);}
static struct linux_transport *linux_state(struct pgwt_daemon *d)
{return d->percpu_transport->platform;}
static int fault(struct pgwt_percpu_transport *t,pgwt_merge_reason r){(void)t;(void)r;return -EINVAL;}
int pgwt_transport_perf(struct pgwt_percpu_transport *t,uint32_t cpu,const void *p,size_t n)
{(void)p;(void)n;++t->positions[cpu];service_now+=record_cost;return 0;}
int pgwt_transport_lost(struct pgwt_percpu_transport *t,uint32_t cpu,uint64_t n)
{(void)t;(void)cpu;(void)n;return 0;}
int pgwt_transport_ring_record(struct pgwt_percpu_transport *t,uint32_t dest,const void *p,size_t n,size_t *consumed,size_t limit)
{(void)t;(void)dest;(void)p;(void)n;++*consumed;service_now+=record_cost;return limit && *consumed>=limit?-EAGAIN:0;}
static enum bpf_perf_event_ret receive_perf(void *,int,struct perf_event_header *);
static enum bpf_perf_event_ret receive_hint(void *,int,struct perf_event_header *);
static int receive_ring(void *,void *,size_t);
static int perf_buffer__consume(struct perf_buffer *p)
{
    while(p->records){
        --p->records;
        struct perf_event_header event={.type=PERF_RECORD_SAMPLE,.size=sizeof(event)};
        enum bpf_perf_event_ret rc=p->hint?receive_hint(p->transport->platform,(int)p->source,&event):
            receive_perf(p->transport->platform,(int)p->source,&event);
        if(p->hint)++p->transport->positions[p->source];
        if(rc==LIBBPF_PERF_EVENT_DONE)break;
    }
    return 0;
}
static int ring_buffer__consume(struct ring_buffer *r)
{
    size_t n=0;
    while(r->records){
        --r->records;++n;
        int rc=receive_ring(r->ctx,NULL,0);
        if(rc<0)return rc;
    }
    return (int)n;
}
static int consume_all(struct linux_transport *,bool *);
static int linux_consume_io(void *,uint32_t,bool);
static bool consume_event_ring(struct pgwt_daemon *,const char *,bool);
#define PGWT_DEBUG_BLOCK_NS (50ULL * 1000ULL * 1000ULL)
#define PGWT_EVENT_DRAIN_CALLBACK_BUDGET 64U
static int tick_consume(struct pgwt_daemon *d,uint32_t dest,bool bounded)
{return linux_consume_io(linux_state(d),dest,bounded);}
static bool drain_cases(void)
{
    struct pgwt_daemon *d=calloc(1,sizeof(*d));TICK(d);
    uint64_t positions[2]={0};struct pgwt_percpu_transport t={.positions=positions};d->percpu_transport=&t;
    struct perf_buffer cpu0={.transport=&t,.source=0,.records=7},cpu1={.transport=&t,.source=1};
    struct perf_buffer *perf[]={&cpu0,&cpu1};
    /* INIT wake-up hints are drained on the same pass but are not transport
     * records: they move no source position and never make a pass non-empty. */
    uint64_t woken[2]={0};struct pgwt_percpu_transport w={.positions=woken};
    struct perf_buffer hint0={.transport=&w,.source=0,.hint=true},hint1={.transport=&w,.source=1,.hint=true};
    struct perf_buffer *hints[]={&hint0,&hint1};
    struct linux_transport l={.d=d,.ncpu=2,.perf=perf,.hints=hints};
    t.platform=w.platform=&l;
    for(size_t i=0;i<2;++i){l.ring_ctx[i].d=d;l.ring_ctx[i].dest=(uint32_t)i+1;}
    struct ring_buffer e={.ctx=&l.ring_ctx[0]},life={.ctx=&l.ring_ctx[1]};l.rings[0]=&e;l.rings[1]=&life;
    bool empty=true;TICK(consume_all(&l,&empty)==0 && !empty && positions[0]==7);
    TICK(consume_all(&l,&empty)==0 && empty);
    /* Exercise the verbatim raw perf and ring callbacks too: libbpf consumes
     * the yielding record and leaves the suffix for the next service pass. */
    record_cost=UINT64_C(10000000);cpu0.records=20;service_now=0;
    pgwt_budget_begin(&t.budget,service_now,0);
    TICK(consume_all(&l,&empty)==0 && !empty);
    TICK(service_now<=PGWT_TRANSPORT_PASS_NS && cpu0.records==15);
    pgwt_budget_begin(&t.budget,service_now,0);
    TICK(consume_all(&l,&empty)==0 && !empty && cpu0.records==10);
    cpu0.records=0;e.records=20;l.next_reader=4;
    pgwt_budget_begin(&t.budget,service_now,0);uint64_t started=service_now;
    TICK(consume_all(&l,&empty)==0 && !empty);
    TICK(service_now-started<=PGWT_TRANSPORT_PASS_NS && e.records==15 && !l.callback_error);
    d->exact_perf=true;d->transport.consume=tick_consume;
    /* The main-loop drain at the end of an already exhausted pass must
     * report backlog without consuming; the next wait uses timeout zero. */
    TICK(consume_event_ring(d,"test",true) && e.records==15);
    pgwt_budget_begin(&t.budget,service_now,0);
    TICK(consume_event_ring(d,"test",true) && e.records==10);
    pgwt_budget_begin(&t.budget,service_now,0);
    TICK(linux_consume_io(&l,PGWT_WIRE_DEST_L,true)==0);
    record_cost=0;t.budget.active=false;e.records=5000;
    TICK(consume_event_ring(d,"test",true) && e.records==904);
    TICK(!consume_event_ring(d,"test",true) && e.records==0);
    TICK(!consume_event_ring(d,"test",true));
    l.callback_error=-EIO;e.records=5000;
    TICK(linux_consume_io(&l,PGWT_WIRE_DEST_E,true)==-EIO);
    l.callback_error=0;
    record_cost=0;e.records=0;t.budget.active=false;
    hint1.records=3;TICK(consume_all(&l,&empty)==0 && empty && woken[1]==3 && !positions[1]);
    e.records=5000;TICK(consume_all(&l,&empty)==0 && !empty && e.records==904 && l.ring_ctx[0].consumed==4096);
    TICK(consume_all(&l,&empty)==0 && !empty && e.records==0 && l.ring_ctx[0].consumed==904);
    TICK(consume_all(&l,&empty)==0 && empty);
    l.callback_error=-EIO;TICK(consume_all(&l,&empty)==-EIO);free(d);
    puts("actual Linux drain/callbacks: perf DONE and ring negative yield within 50 ms; suffix retained; 4096 record guard PASS");
    puts("actual Linux consume + daemon backlog: expired pass, timed yield and record cap keep next wait awake; errors preserved PASS");
    return true;
}
static bool tick_cases(void)
{
    for(unsigned flag=0;flag<2;++flag)for(unsigned view=0;view<6;++view){
        struct pgwt_daemon *d=calloc(1,sizeof(*d));TICK(d);
        d->event_accum=calloc(1,sizeof(*d->event_accum));TICK(d->event_accum);
        struct pgwt_event_writer *w=calloc(1,sizeof(*w));TICK(w);
        struct pgwt_summary_writer *sw=calloc(1,sizeof(*sw));TICK(sw);
        d->exact_perf=flag;d->running=true;d->daemon_mode=true;d->postmaster_pid=10;
        d->mode=PGWT_MODE_FULL;d->view=view;d->interval=2;d->tick=60;d->count=61;
        d->event_writer=w;d->summary_writer=sw;sw->fp=stdout;sw->clock_offset_wall_ns=91;
        d->ring.slots=(void *)1;d->backends.count=1;
        d->backends.entries[0].pid=20;d->backends.entries[0].is_alive=true;
        d->accum.tm.db_time_ns=4;d->event_accum->tm.db_time_ns=9;
        d->counters.events_total=40;d->counters.prev_events_total=10;
        d->counters.samples_total=12;d->counters.prev_samples_total=2;
        used=0;calls[0]=0;backlog_warnings=0;
        if(flag)TICK(pgwt_merge_capture_tick(d)==0);else pgwt_tick_duties(d);
        TICK(!strcmp(calls,flag
            ? "health capacity init recovery stale budget transport_drain backlog copy state snapshot view gc_probe exit rotate_raw summary_flush rotate_summary retain_raw retain_summary fail_read "
            : "health capacity init recovery stale budget drain copy state snapshot view gc_probe exit rotate_raw summary_flush rotate_summary retain_raw retain_summary fail_read "));
        TICK(d->tick==61 && !d->running && d->counters.events_per_sec==15 && d->counters.samples_per_sec==5);
        TICK(d->accum.tm.db_time_ns==(flag?12U:19U) && d->accum.prev_tm.db_time_ns==4);
        TICK(d->counters.tick_drain_pending_records==(flag?7U:0U) &&
             d->counters.tick_drain_truncated_total==flag && d->tick_drain_truncated_logged==(bool)flag);
        TICK(backlog_warnings==flag);
        if(flag)TICK(d->counters.tick_drain_last_bound==PGWT_TICK_DRAIN_BOUND_MERGE);
        TICK(w->retention_live_summary_ns==91);
        /* Guard preservation: lightweight reads its accumulator, skips
         * recovery/sweep; sampled mode skips open state-map intervals. */
        d->tick=1;d->count=0;d->running=true;d->lightweight_mode=true;d->mode=PGWT_MODE_SAMPLED;
        d->quiet=true;d->ring.slots=NULL;d->event_writer=NULL;d->summary_writer=NULL;
        used=0;calls[0]=0;
        if(flag)TICK(pgwt_merge_capture_tick(d)==0);else pgwt_tick_duties(d);
        TICK(!strcmp(calls,flag ? "health capacity init budget accum_read transport_drain backlog copy fail_read check "
                               : "health capacity init budget accum_read drain copy fail_read "));
        TICK(d->tick==2 && d->running);
        TICK(backlog_warnings==flag);
        d->lightweight_mode=false;d->mode=PGWT_MODE_FULL;
        setenv("PGWT_TEST_NO_RECOVERY","1",1);setenv("PGWT_TEST_NO_STALE_SWEEP","1",1);
        used=0;calls[0]=0;
        if(flag)TICK(pgwt_merge_capture_tick(d)==0);else pgwt_tick_duties(d);
        TICK(!strcmp(calls,flag ? "health capacity init budget transport_drain backlog copy state fail_read check "
                               : "health capacity init budget drain copy state fail_read "));
        TICK(backlog_warnings==flag);
        unsetenv("PGWT_TEST_NO_RECOVERY");unsetenv("PGWT_TEST_NO_STALE_SWEEP");
        dead=true;used=0;calls[0]=0;int before=d->tick;pgwt_tick_duties(d);dead=false;
        TICK(!strcmp(calls,"health ") && d->tick==before && !d->running && d->exit_reason==PGWT_EXIT_PG_DEAD);
        if(flag){
            /* The gauge clears on a later empty copy. A successful tick
             * drains without inventing publication into event_accum. */
            pending_records=0;d->running=true;used=0;calls[0]=0;
            TICK(pgwt_merge_capture_tick(d)==0 && d->counters.tick_drain_pending_records==0 &&
                 d->counters.tick_drain_truncated_total==3 && d->tick_drain_truncated_logged);
            before=d->tick;uint64_t saved=d->accum.tm.db_time_ns;
            bad_drain=true;d->running=true;used=0;calls[0]=0;
            TICK(pgwt_merge_capture_tick(d)==0);bad_drain=false;
            TICK(!d->running && d->tick==before && d->accum.tm.db_time_ns==saved);
            TICK(!strcmp(calls,"health capacity init recovery stale budget transport_drain "));
            pending_records=7;
        }
        free(sw);free(w);free(d->event_accum);free(d);
    }
    return true;
}
int main(void)
{
    puts("REACHED tick_duties");fflush(stdout);
    unsetenv("PGWT_TEST_NO_RECOVERY");unsetenv("PGWT_TEST_NO_STALE_SWEEP");
    if(!publication_counters() || !cadence_cases() || !drain_cases() || !tick_cases())return 1;
    puts("shared tick: full duty order, 6 views, rates/count, GC, rotations/retention, lightweight/test/death guards PASS");
    return 0;
}
