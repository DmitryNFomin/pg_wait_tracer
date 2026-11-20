#define PGWT_TRANSPORT_EMBEDDED
#include "test_percpu_transport.c"
#undef main
#define put_le discovery_capture_put_le
#include "../src/merge_capture.c"
#undef put_le
#include "coverage_record.h"
#include "coverage.h"
#include "core/pgwt_core.h"
#include "window_clip.h"
#include "discovery_oracle_rows.h"
#include "discovery_oracle_expected.h"
#include <stdarg.h>

static const char *discovery_case;
#define DC(x, message) do { if (!(x)) { fprintf(stderr, "ASSERT %s: %s (line %d)\n", discovery_case, message, __LINE__); exit(1); } } while (0)
#define DB UINT64_C(1000)
#define DG PGWT_DISCOVERY_COOKIE_MIN
#define DX UINT32_C(0x06000000)
#define DY UINT32_C(0x06000001)
#define DZ UINT32_C(0x06000002)
struct discovery_record {
    unsigned char bytes[48];
    uint64_t pid, cookie, position;
    size_t source, size;
    pgwt_merge_rank rank;
    bool certified;
};
struct dh {
    struct pgwt_merge_capture *capture;
    ma_snapshot snapshot;
    uint64_t pos[4];
    char output[1024]; size_t length;
    char full_output[2048]; size_t full_length;
    struct discovery_record input[32], released[32]; size_t ninput, nreleased;
    struct pgwt_capture_gap gaps[32]; size_t ngaps;
    struct pgwt_cov_rec coverage[32]; size_t coverage_count;
    bool reject_data, reject_close;
    size_t data_count;
};
static void full_append(struct dh *h, const char *fmt, ...)
{
    if(h->full_length) h->full_output[h->full_length++]=' ';
    va_list args;va_start(args,fmt);
    int n=vsnprintf(h->full_output+h->full_length,sizeof(h->full_output)-h->full_length,fmt,args);
    va_end(args);
    DC(n>=0 && (size_t)n<sizeof(h->full_output)-h->full_length,"full output bound");
    h->full_length+=(size_t)n;
}
static const char *counter_label(pgwt_merge_counters counters)
{
    static const char *labels[]={"C0","Ce","Cf","Cs","Cw"};
    static const size_t components[]={PGWT_MERGE_COUNTERS,0,10,12,14};
    DC(counters.known,"output counter vector known");
    for(size_t k=0;k<5;++k) {
        bool equal=true;
        for(size_t j=0;j<PGWT_MERGE_COUNTERS;++j)
            equal=equal && counters.value[j]==(j==components[k]?1:0);
        if(equal) return labels[k];
    }
    DC(false,"complete output counter vector matches oracle label");return NULL;
}
static void append(struct dh *h, const char *token, uint64_t t, const char *suffix)
{
    int n = snprintf(h->output+h->length, sizeof(h->output)-h->length,
                     "%s%s%llu%s", h->length?" ":"", token, (unsigned long long)t, suffix);
    DC(n>0 && (size_t)n<sizeof(h->output)-h->length,"output bounds");
    h->length+=(size_t)n;
}
static bool dsink(void *ctx, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    struct dh *h=ctx;
    DC(out->kind==PGWT_MERGE_OUT_DATA || out->kind==PGWT_MERGE_OUT_FRONTIER ||
       out->kind==PGWT_MERGE_OUT_VOID,"every output kind is specified by this oracle");
    if(out->kind==PGWT_MERGE_OUT_DATA && h->reject_data) {
        post->d=DB+20; post->counters.value[14]=1;
        return false;
    }
    if(out->kind==PGWT_MERGE_OUT_DATA) {
        const pgwt_merge_record *r=&out->record;
        ++h->data_count;
        DC(r->payload_size==48 || r->payload_size==24,"raw payload size");
        DC(get_le((const unsigned char *)r->payload+(r->rank==PGWT_MERGE_TRANS?0:16),8)==r->t,
           "raw payload preserved");
        size_t i;
        for(i=0;i<h->ninput;++i)
            if(h->input[i].source==r->source && h->input[i].position==r->o) break;
        DC(i<h->ninput,"released record has its original source/position");
        struct discovery_record copy=h->input[i];
        DC(copy.pid==r->pid && copy.cookie==r->g && copy.rank==r->rank &&
           r->identity_known && !r->has_q && !r->q,"released lifetime identity and rank");
        DC(copy.size==r->payload_size && !memcmp(copy.bytes,r->payload,copy.size),"all raw payload bytes preserved");
        copy.certified=out->prior_interval_certified;
        DC(h->nreleased<32,"released record bound");h->released[h->nreleased++]=copy;
        const char *name=r->rank==PGWT_MERGE_TRANS?
            (r->g==1?"R":(!strcmp(discovery_case,"D09") && r->t==DB+18)?"terminal_D":"D"):
            r->rank==PGWT_MERGE_EXIT?(r->pid==40?"EXIT_H":"EXIT_D"):
            r->rank==PGWT_MERGE_FORK?"FORK_R":"INIT";
        full_append(h,"%s%llu%s",name,(unsigned long long)(r->t-DB),
            r->rank==PGWT_MERGE_TRANS?(copy.certified?"[T]":"[U]"):"");
        append(h,r->rank==PGWT_MERGE_TRANS?"D":r->rank==PGWT_MERGE_EXIT?"EXIT":
               r->rank==PGWT_MERGE_FORK?"FORK":"INIT",r->t-DB,
               r->rank==PGWT_MERGE_TRANS?(out->prior_interval_certified?"T":"U"):"");
    } else if(out->kind==PGWT_MERGE_OUT_FRONTIER) {
        append(h,"F",out->F-DB,"");
        full_append(h,"F%llu[%s]",(unsigned long long)(out->F-DB),counter_label(out->counters));
    }
    else if(out->kind==PGWT_MERGE_OUT_VOID) {
        full_append(h,"VOID%llu[%s,%s]",(unsigned long long)(out->d-DB),
                    counter_label(out->counters),pgwt_merge_reason_name(out->reason));
        append(h,"VOID",out->d-DB,":");
        size_t n=strlen(pgwt_merge_reason_name(out->reason));
        DC(h->length+n<sizeof(h->output),"reason bounds");
        memcpy(h->output+h->length,pgwt_merge_reason_name(out->reason),n+1);h->length+=n;
    }
    return true;
}
static bool dgap(void *ctx,const struct pgwt_capture_gap *gap)
{
    struct dh *h=ctx;
    if(gap->kind==PGWT_CAPTURE_GAP_PREEXISTING_END && h->reject_close) return false;
    if(gap->kind!=PGWT_CAPTURE_GAP_PREEXISTING && gap->kind!=PGWT_CAPTURE_GAP_PREEXISTING_END) return true;
    DC(h->ngaps<32,"startup gap output bound");h->gaps[h->ngaps++]=*gap;
    struct pgwt_coverage_record r={.kind=gap->kind==PGWT_CAPTURE_GAP_PREEXISTING?
        PGWT_COVK_GAP_OPEN:PGWT_COVK_GAP_CLOSE,.a=gap->at,.c=1};
    unsigned char bytes[80];size_t n=pgwt_coverage_encode(&r,bytes);
    struct pgwt_coverage_record decoded;
    DC(pgwt_coverage_decode(bytes,n,&decoded)==PGWT_COVD_OK,"persisted gap decode");
    DC(h->coverage_count<32,"gap record bound");
    h->coverage[h->coverage_count++]=(struct pgwt_cov_rec){.kind=decoded.kind,.a=(int64_t)decoded.a,.c=(int64_t)decoded.c};
    return true;
}
static struct dh *dsetup_inventory(size_t count, uint64_t fence, bool qmode, bool census)
{
    struct dh *h=calloc(1,sizeof(*h));DC(h,"host fixture allocation");
    h->snapshot=readable();
    pgwt_merge_source sources[4]={{.qualified=true},{.qualified=true},{.qualified=true},{.qualified=true}};
    pgwt_merge_config initial={.mode=qmode?PGWT_MERGE_Q:PGWT_MERGE_N,
        .sources=sources,.source_count=4,.staged_capacity=64,.payload_capacity=272,
        .attachment_capacity=64,.frontier_capacity=128,.dependency_capacity=64,
        .observation={.d=DB+20,.counters={.known=true}},.last_observed={.known=true}};
    struct pgwt_capture_config cfg={.cpu_count=2,.capacity=64,.ctx=h,
        .sink={.emit=dsink,.ctx=h},.baseline=initial.observation,.publish_startup_gap=dgap,
        .test_attach_premise=true,.discovery_enabled=true};
    ma_table empty={0};
    h->capture=pgwt_capture_replay_create(&cfg,&initial,&empty);DC(h->capture,"capture create");
    DC(pgwt_capture_coverage_begin(h->capture,DB,NULL,0,NULL)==0,"begin startup gap");
    if(count) DC(pgwt_capture_discovery_inventory(h->capture,30,DG)==0,"D inventory");
    if(count>1) DC(pgwt_capture_discovery_inventory(h->capture,40,DG+1)==0,"H inventory");
    if(census) DC(pgwt_capture_discovery_census(h->capture,DB+fence,true,DB+20,&h->snapshot)==0,"complete census");
    return h;
}
static struct dh *dsetup(bool two, uint64_t fence, bool qmode)
{ return dsetup_inventory(two?2:1,fence,qmode,true); }
static pgwt_discovery_attempt admit(uint64_t pid,uint64_t cookie,uint64_t e,uint64_t gen)
{
    return (pgwt_discovery_attempt){.kind=PGWT_DISC_ADMIT,.pid=pid,.cookie=cookie,
        .e=DB+e,.attachment_generation=gen,
        .proof={.known=PGWT_DISC_ADMISSION_FACTS,.truth=PGWT_DISC_ADMISSION_FACTS}};
}
static pgwt_discovery_attempt bracket(uint64_t pid,uint64_t cookie,uint64_t t,uint64_t e,uint64_t gen)
{
    return (pgwt_discovery_attempt){.kind=PGWT_DISC_CONFIRM,.pid=pid,.cookie=cookie,
        .attachment_generation=gen,.witness={.t=DB+t,.e=DB+e,.wait_word=0x6000000,
        .v0=7,.v1=7,.handle0=1,.handle1=1,.armed_address=0x1000,.resolved_address=0x1000,
        .threads0=1,.threads1=1,.S0=PGWT_DISC_INACTIVE_SYSCALL,.S1=PGWT_DISC_INACTIVE_SYSCALL,
        .cpu_base_valid=true,.facts={.known=PGWT_DISC_QUIET_FACTS,.truth=PGWT_DISC_QUIET_FACTS}}};
}
static void dcontrol(struct dh *h,pgwt_discovery_attempt a)
{
    pgwt_discovery_attempt before=a;
    DC(pgwt_capture_discovery_control(h->capture,&a,DB+20,&h->snapshot)==0,"discovery control delivery");
    DC(!memcmp(&a,&before,sizeof(a)),"consumer preserves supplied producer attempt");
}
static void dadmit(struct dh *h,uint64_t e) {dcontrol(h,admit(30,DG,e,1));}
static void dquiet(struct dh *h,uint64_t t,uint64_t e) {dcontrol(h,bracket(30,DG,t,e,1));}
static void record(struct dh *h,uint64_t t,uint64_t pid,uint64_t cookie,size_t src,
                   pgwt_merge_rank rank,uint64_t gen,bool marker,bool refresh)
{
    unsigned char bytes[48]={0};size_t size=rank==PGWT_MERGE_TRANS?48:24;
    if(rank==PGWT_MERGE_TRANS) {
        uint32_t old=DY,next=DX;uint64_t duration=0,cpu=PGWT_CPU_NS_UNKNOWN;
        if(t==15) {old=DX;next=0;duration=3;}
        if(!strcmp(discovery_case,"D02c")) {
            old=t==9?DX:DY;next=t==9?DY:DX;duration=t==10?1:0;
        }
        if(!strcmp(discovery_case,"D09") && t==15) next=DY;
        if(!strcmp(discovery_case,"D09") && t==18) {old=DY;next=PGWT_EVENT_EXIT;duration=3;}
        if(!strcmp(discovery_case,"Q09")) {
            old=t==9?DY:DX;next=t==9?DZ:0;duration=t==9?1:4;
        }
        if(!strcmp(discovery_case,"Q01") || !strcmp(discovery_case,"Q02") ||
           !strcmp(discovery_case,"Q03") || !strcmp(discovery_case,"Q04") ||
           !strcmp(discovery_case,"Q04b") || !strcmp(discovery_case,"Q05") ||
           !strcmp(discovery_case,"Q06") || !strcmp(discovery_case,"Q07")) cpu=1;
        if(!strcmp(discovery_case,"Q07")) {old=0;next=DY;}
        tb_le(bytes,DB+t,8);tb_le(bytes+8,pid,4);tb_le(bytes+12,old,4);tb_le(bytes+16,next,4);
        tb_le(bytes+24,duration,8);tb_le(bytes+40,cpu,8);
    } else {
        tb_le(bytes,rank==PGWT_MERGE_FORK?1:rank==PGWT_MERGE_INIT?2:3,4);
        tb_le(bytes+4,pid,4);tb_le(bytes+16,DB+t,8);
    }
    ma_envelope e={.kind=rank==PGWT_MERGE_TRANS?MA_TRACE:MA_LIFECYCLE,.version=1,
        .origin=MA_EXACT,.source=src,.position=++h->pos[src],.cookie=cookie,.cookie_present=cookie!=0,
        .bytes=bytes,.size=size,.discovery_origin={.attachment_generation=gen,
            .membership_proved=gen!=0,.marker=marker,.baselines_refreshed=refresh}};
    DC(h->ninput<32,"input record bound");
    struct discovery_record *saved=&h->input[h->ninput++];
    *saved=(struct discovery_record){.pid=pid,.cookie=cookie,.position=e.position,.source=src,.size=size,.rank=rank};
    memcpy(saved->bytes,bytes,size);
    size_t token=0;
    int received=pgwt_capture_received_token(h->capture,&e,DB+20,&token);
    DC(received>=0,"record consumption");
    if(!received) DC(pgwt_capture_envelope_token(h->capture,&e,token,DB+20,&h->snapshot)==0,"record adapter delivery");
}
static void data(struct dh *h,uint64_t t,size_t source,uint64_t gen)
{record(h,t,30,DG,source,PGWT_MERGE_TRANS,gen,false,true);}
static void complete(struct dh *h,uint64_t w)
{
    for(size_t s=0;s<4;++s) {
        ma_envelope e={.kind=MA_BOUNDARY,.version=1,.source=s,.position=++h->pos[s],
            .w=DB+w,.boundary=s<2?MA_IDLE_PERF:MA_RING_BOUNDARY};
        DC(pgwt_capture_envelope(h->capture,&e,DB+20,&h->snapshot)==0,"source completeness");
    }
}
static void state_is(struct dh *h,const char *out,int64_t close,size_t pending,bool census)
{
    if(strcmp(h->output,out)) fprintf(stderr,"GOT %s; EXPECTED %s\n",h->output,out);
    DC(!strcmp(h->output,out),"exact merge outputs");
    pgwt_capture_discovery_report s=pgwt_capture_discovery_status(h->capture);
    DC(s.unresolved==pending,"unresolved lifetime count");
    DC(s.census_complete==census,"census completeness separate from pending");
    DC(s.closed==(close>=0),"startup closure commitment");
    if(close>=0) DC(s.closure==DB+(uint64_t)close,"max census/lower/terminal closure");
}
static int base_verdict(struct dh *h,uint64_t from,uint64_t to)
{
    struct pgwt_cov_rec facts[40]={{.kind=PGWT_COV_REC_FILE,.a=DB,.c=1}};
    memcpy(facts+1,h->coverage,h->coverage_count*sizeof(*facts));
    facts[h->coverage_count+1]=(struct pgwt_cov_rec){.kind=PGWT_COV_REC_WATERMARK,.a=DB+100,.b=DB+100,.c=DB+100};
    struct pgwt_cov_boot boot={.boot_start=DB,.closure_clean=1,.same_kernel_boot=1,
        .status=PGWT_COV_STATUS_MATCH,.last_obs=DB+100,.recs=facts,.n_recs=h->coverage_count+2};
    struct pgwt_cov_model model={.boots=&boot,.n_boots=1};
    struct pgwt_cov_span spans[40];
    struct pgwt_cov_result result={.uncovered=spans,.uncovered_cap=40};
    result.scratch_size=pgwt_cov_scratch_bytes(&model);result.scratch=malloc(result.scratch_size);
    DC(result.scratch,"coverage reader scratch");
    int v=pgwt_coverage_verdict(&model,DB+(int64_t)from,DB+(int64_t)to,
        DB+100,PGWT_COV_FLOOR_DB,&result);
    free(result.scratch);
    return v;
}
static void endpoint_verdict(struct dh *h,uint64_t closure)
{
    DC(base_verdict(h,closure,closure+1)==PGWT_COV_INCOMPLETE,
       "reader excludes completion exactly at C");
    DC(base_verdict(h,closure+1,closure+2)==PGWT_COV_COMPLETE,
       "reader permits interval after conservative endpoint");
}
static void full_state(struct dh *h, const char *id)
{
    const struct discovery_expected *expected=NULL;
    for(size_t i=0;i<sizeof(discovery_expected)/sizeof(*discovery_expected);++i)
        if(!strcmp(id,discovery_expected[i].id)) expected=&discovery_expected[i];
    DC(expected,"structured oracle row present");
    if(strcmp(h->full_output,expected->output))
        fprintf(stderr,"FULL GOT %s; EXPECTED %s\n",h->full_output,expected->output);
    DC(!strcmp(h->full_output,expected->output),"complete output sequence with counter labels");
    pgwt_capture_discovery_report gap=pgwt_capture_discovery_status(h->capture);
    DC(gap.closed==(expected->close>=0) && gap.unresolved==expected->pending &&
       gap.census_complete==expected->census,"full gap state and census");
    if(expected->close>=0) DC(gap.closure==DB+(uint64_t)expected->close,"semantic gap closing time");
    DC(gap.census_fence==DB+(!strcmp(id,"D07b")?25:5),"retained census fence");
    size_t ngaps=!strcmp(id,"D10")?3:expected->close>=0?2:1;
    DC(h->ngaps==ngaps,"complete startup gap publication sequence");
    DC(h->gaps[0].kind==PGWT_CAPTURE_GAP_PREEXISTING && h->gaps[0].at==DB &&
       h->gaps[0].detected==DB,"startup opens before any coverage");
    if(ngaps>=2) {
        uint64_t close=!strcmp(id,"D10")?12:(uint64_t)expected->close;
        DC(h->gaps[1].kind==PGWT_CAPTURE_GAP_PREEXISTING_END &&
           h->gaps[1].at==DB+close+1 && h->gaps[1].detected==DB+20,
           "persisted startup endpoint and publication time");
        DC(h->coverage[1].a==(int64_t)(DB+close+1),"encoded GAP_CLOSE time");
    }
    if(ngaps==3) DC(h->gaps[2].kind==PGWT_CAPTURE_GAP_PREEXISTING &&
        h->gaps[2].at==DB && h->gaps[2].detected==DB+40,"late gap revocation and detection times");
    pgwt_discovery_state d={0};
    (void)pgwt_merge_discovery_state(h->capture->core,30,DG,&d);
    DC(d.has_lower_bound==(expected->lower>=0),"lower bound presence");
    if(expected->lower>=0) {
        DC(d.lower_bound==DB+(uint64_t)expected->lower,"exact lifetime lower bound");
        DC(d.confirmed==(expected->route=='Q'),"lower bound route");
        if(expected->route=='F') DC(d.source==0 && d.position==1,"fresh lower bound ordered position");
        if(expected->route=='Q') {
            DC(d.baseline.t==d.lower_bound && d.baseline.e==d.lower_bound+1 &&
               d.baseline.cpu_base_valid && d.baseline.cpu_base==0 &&
               d.baseline.accumulator==0 && d.baseline.on_cpu_ts==0,
               "coherent final quiet seed and valid zero CPU base");
            DC(d.baseline.wait_word==(!strcmp(id,"Q07")?0:DX),"quiet seed wait state");
        }
    } else DC(!d.confirmed,"no accepted quiet confirmation");
    DC(!d.terminal_released,"D not discharged by an invented terminal witness");
    if(!strcmp(id,"D02b") || !strcmp(id,"D02e"))
        DC(d.baseline.t==0 && !d.has_lower_bound,"ordinary reread establishes no baseline");
    if(!strcmp(id,"Q08")) DC(!d.admitted,"address mismatch supplies no ADMIT");
    if(!strcmp(id,"Q05")) DC(!strcmp(counter_label(h->capture->observation.counters),"C0"),
        "optional witness denial causes no synthetic loss increment");
    if(!strcmp(id,"D07a") || !strcmp(id,"D07b") || !strcmp(id,"D08a") || !strcmp(id,"D08b")) {
        pgwt_discovery_state other={0};
        (void)pgwt_merge_discovery_state(h->capture->core,40,DG+1,&other);
        bool quiet=id[1]=='0' && id[2]=='7',terminal=!strcmp(id,"D08a");
        DC(other.has_lower_bound==quiet && other.confirmed==quiet &&
           other.terminal_released==terminal,"H lower bound or terminal route");
        if(quiet) DC(other.lower_bound==DB+20,"H lower bound is 20");
        if(terminal) DC(other.terminal_time==DB+18,"H discharge at released EXIT time");
    }
    pgwt_merge_report report=pgwt_capture_report(h->capture);
    DC(!report.epoch_invalid && !report.void_failed && report.status!=PGWT_MERGE_EXIT_REQUIRED,
       "oracle does not invalidate epochs or require process exit");
    bool fault=strstr(expected->output,"VOID")!=NULL;
    DC(report.status==(fault?PGWT_MERGE_SUSPENDED:PGWT_MERGE_OK),"final epoch status");
}
/* Feed ONLY certified incoming intervals to the existing numerical authority.
 * A frontier supplies release coverage, not an open-wait snapshot. For an
 * open target, the last transition establishes state only at its own time. */
static void released_metrics(struct dh *h,uint32_t event,uint64_t from,uint64_t to,
                      pgwt_core_result *duration,pgwt_core_result *count)
{
    pgwt_core_wait waits[32],scratch[32];size_t n=0;uint32_t last=0;uint64_t last_t=0;
    for(size_t i=0;i<h->nreleased;++i) {
        const struct discovery_record *r=&h->released[i];
        if(r->rank!=PGWT_MERGE_TRANS || r->cookie!=DG) continue;
        uint64_t t=get_le(r->bytes,8),dur=get_le(r->bytes+24,8);
        last=(uint32_t)get_le(r->bytes+16,4);last_t=t;
        if(r->certified && get_le(r->bytes+12,4)==event)
            waits[n++]=(pgwt_core_wait){.event={"selected",8},.start=t-dur,.end=t};
    }
    pgwt_discovery_state d={0};(void)pgwt_merge_discovery_state(h->capture->core,30,DG,&d);
    pgwt_merge_report report=pgwt_capture_report(h->capture);
    pgwt_core_span span={.start=d.lower_bound,.end=report.frontier+1};
    pgwt_core_evidence evidence={.life_start=DB,.coverage=&span,.coverage_count=d.has_lower_bound?1:0,
        .waits=waits,.wait_count=n};
    if(last==event) {
        evidence.has_open=true;evidence.open=(pgwt_core_open_wait){.event={"selected",8},.start=last_t,.as_of=last_t};
        span.end=last_t;
    }
    pgwt_core_prepared prepared;
    DC(pgwt_core_prepare(&evidence,scratch,32,&prepared)==0,"prepare released numerical evidence");
    pgwt_core_query query={.event={"selected",8},.from=DB+from,.to=DB+to};
    pgwt_core_wait_metrics(&prepared,&query,duration,count);
}
static void numerical_outputs(struct dh *h,const char *id)
{
    bool measured=!strcmp(id,"D01")||!strcmp(id,"D02a")||!strcmp(id,"D03")||
        !strcmp(id,"Q01")||!strcmp(id,"Q09");
    bool unavailable=!strcmp(id,"D02c")||!strcmp(id,"Q02")||!strcmp(id,"Q03");
    if(measured || unavailable) {
        pgwt_core_result time,count;
        released_metrics(h,DX,!strcmp(id,"D02c")?10:!strcmp(id,"Q09")?11:12,
                  !strcmp(id,"D02c")?13:16,&time,&count);
        if(measured) DC(time.value.present && time.value.value==(!strcmp(id,"Q09")?4:3) &&
            count.value.present && count.value.value==1,"X duration and completed count");
        else {
            DC(!time.value.present && !count.value.present,"incoming or open interval quantities unavailable");
            DC(time.diagnostic.recorded.present && time.diagnostic.recorded.value==0 &&
               count.diagnostic.recorded.present && count.diagnostic.recorded.value==0,
               "no invented X duration or completed count");
        }
    }
    if(!strcmp(id,"D01")) {
        pgwt_core_result time,count;released_metrics(h,DY,12,13,&time,&count);
        DC(count.value.present && count.value.value==0,"uncertified completion at 12 contributes no count");
    }
    if(!strcmp(id,"Q01") || !strcmp(id,"Q07") || !strcmp(id,"Q02") || !strcmp(id,"Q03")) {
        bool has_cpu=false;uint64_t cpu=0;
        for(size_t i=0;i<h->nreleased;++i) {
            const struct discovery_record *r=&h->released[i];
            if(r->rank!=PGWT_MERGE_TRANS || !r->certified) continue;
            struct pgwt_trace_event e;memcpy(&e,r->bytes,sizeof(e));
            if(e.cpu_ns==PGWT_CPU_NS_UNKNOWN) continue;
            has_cpu=true;cpu+=pgwt_cpu_clip_ns(&e,DB+12,DB+16,NULL);
        }
        bool expected=!strcmp(id,"Q01")||!strcmp(id,"Q07");
        DC(has_cpu==expected,"CPU availability follows interval certification");
        if(expected) DC(cpu==1,"measured CPU is one, not zero or elapsed time");
    }
}
static void a2_population(struct dh *h)
{
    /* D13b stipulates otherwise valid base coverage and certificates. An
     * invalid decoded member has precedence independently of A2 limits. */
    struct pgwt_cov_rec facts[]={{.kind=PGWT_COV_REC_FILE,.a=DB,.c=1},
        {.kind=PGWT_COV_REC_WATERMARK,.a=DB+100,.b=DB+100,.c=DB+100}};
    struct pgwt_cov_boot boot={.boot_start=DB,.closure_clean=1,.same_kernel_boot=1,
        .status=PGWT_COV_STATUS_MATCH,.last_obs=DB+100,.recs=facts,.n_recs=2};
    struct pgwt_cov_model model={.boots=&boot,.n_boots=1};
    struct pgwt_cov_span spans[4];
    struct pgwt_cov_result result={.uncovered=spans,.uncovered_cap=4};
    result.scratch_size=pgwt_cov_scratch_bytes(&model);result.scratch=malloc(result.scratch_size);
    DC(result.scratch,"A2 base coverage scratch");
    DC(pgwt_coverage_verdict(&model,DB,DB+10,DB+100,PGWT_COV_FLOOR_DB,&result)==PGWT_COV_COMPLETE,
       "D13b base coverage otherwise permits numbers");
    free(result.scratch);
    pgwt_a2_lifetime d;
    DC(pgwt_capture_a2_lifetime(h->capture,30,DG,&d),"discovery retained in A2 population view");
    DC(!pgwt_a2_observed_birth(&d,DB),"discovery is not an A2 birth");
    DC(pgwt_capture_a2_observed_births(h->capture,DB,DB+1000)==0,"discovery contributes zero to N_k");
    pgwt_a2_omission u={.pid=30,.cookie=DG,.fork_ns=DB-1,.exit_ns=DB+6,.receive_ns=DB+7};
    DC(pgwt_capture_a2_omission_population(h->capture,&u)==PGWT_A2_UNTRACED_INVALID,
       "A2 result Unavailable/untraced_invalid for discovery omission");
    /* A claimed in-boot timestamp cannot turn the same identity into U_k. */
    u.fork_ns=DB+1;
    DC(pgwt_capture_a2_omission_population(h->capture,&u)==PGWT_A2_UNTRACED_INVALID,
       "discovery contributes no A2 omission even with an in-boot timestamp");
}
static void run_row(const char *id)
{
    bool two=!strcmp(id,"D07a")||!strcmp(id,"D07b")||!strcmp(id,"D08a")||!strcmp(id,"D08b");
    struct dh *h=dsetup(two,!strcmp(id,"D07b")?25:5,!strcmp(id,"D13e"));
    if(!strcmp(id,"D01")||!strcmp(id,"D10")) {
        dadmit(h,8);data(h,12,0,1);data(h,15,0,1);complete(h,16);
        state_is(h,"D12U D15T F15",12,0,true);endpoint_verdict(h,12);
        if(!strcmp(id,"D10")) {
            complete(h,31);
            DC(base_verdict(h,13,16)==PGWT_COV_COMPLETE,"late revocation previously certified window");
            unsigned char bytes[48]={0};tb_le(bytes+8,99,4);
            ma_envelope e={.kind=MA_TRACE,.version=1,.bytes=bytes,.size=48};
            DC(pgwt_capture_received(h->capture,&e,DB+40)==1,"late observation outside merge population");
            state_is(h,"D12U D15T F15 F30",-1,1,false);
            DC(h->coverage[h->coverage_count-1].a==DB,"revoke from boot start");
            DC(base_verdict(h,13,16)==PGWT_COV_INCOMPLETE,"late revocation invalidates prior window verdict");
        }
    } else if(!strcmp(id,"D02a")) {
        dadmit(h,13);dquiet(h,12,13);complete(h,12);
        state_is(h,"F11",-1,1,true);complete(h,13);state_is(h,"F11 F12",12,0,true);
        data(h,15,0,1);complete(h,16);state_is(h,"F11 F12 D15T F15",12,0,true);
    } else if(!strcmp(id,"D02b")||!strcmp(id,"D02e")||!strcmp(id,"D04b")) {
        if(strcmp(id,"D04b")) dadmit(h,!strcmp(id,"D02e")?14:8);
        complete(h,16);state_is(h,"F15",-1,1,true);
    } else if(!strcmp(id,"D02c")) {
        dadmit(h,8);data(h,9,0,1);data(h,10,0,1);complete(h,13);
        state_is(h,"D9U D10T F12",9,0,true);
    } else if(!strcmp(id,"D02d")) {
        dadmit(h,13);pgwt_discovery_attempt a=bracket(30,DG,12,13,1);
        a.witness.facts.truth&=~(uint64_t)PGWT_DISC_FRESH_COHERENT_SEED;
        a.witness.claims_completed_coherence=true;dcontrol(h,a);
        state_is(h,"VOID20:unknown_evidence",-1,1,true);
        complete(h,13);
    } else if(!strcmp(id,"D03")) {
        data(h,15,1,1);data(h,12,0,1);complete(h,16);state_is(h,"",-1,1,true);
        dadmit(h,8);state_is(h,"D12U D15T F15",12,0,true);
    } else if(!strncmp(id,"D04a-",5)||!strcmp(id,"D04c")||!strcmp(id,"D13c")||!strcmp(id,"D13e")) {
        pgwt_discovery_attempt a=admit(30,DG,8,1);
        uint64_t bit=!strcmp(id,"D04a-install")?PGWT_DISC_INSTALL_BEFORE_ENABLE:
            !strcmp(id,"D04a-state")?PGWT_DISC_PRESERVED_STATE:
            !strcmp(id,"D04a-address")?PGWT_DISC_ADDRESS_CONTINUOUS:
            !strcmp(id,"D04a-cookie")?PGWT_DISC_COOKIE_AUTHORITY:PGWT_DISC_PINNED_BINDING;
        if(!strcmp(id,"D13c")) {
            record(h,10,31,1,3,PGWT_MERGE_FORK,0,false,false);
            a.pid=31;a.cookie=1;
        } else if(strcmp(id,"D13e")) {
            if(!strcmp(id,"D04a-binding") || !strcmp(id,"D04c")) a.proof.known&=~bit;
            else a.proof.truth&=~bit;
        }
        dcontrol(h,a);state_is(h,"VOID20:unknown_evidence",-1,1,true);
        if(!strncmp(id,"D04a-",5)) {data(h,12,0,1);complete(h,13);}
        if(!strcmp(id,"D13c")) complete(h,16);
    } else if(!strcmp(id,"D04d")) {
        state_is(h,"",-1,1,true); /* producer delivered no control or terminal witness */
    } else if(!strcmp(id,"D04e")||!strcmp(id,"D04g")||!strcmp(id,"D13a")) {
        if(!strcmp(id,"D13a")) {dadmit(h,8);data(h,12,0,1);}
        h->snapshot.value[!strcmp(id,"D04e")?10:!strcmp(id,"D04g")?12:0]=1;
        ma_envelope e={.version=1,.kind=MA_CHECK};
        DC(pgwt_capture_envelope(h->capture,&e,DB+20,&h->snapshot)==0,"producer failure checkpoint");
        state_is(h,"VOID20:counter_change",-1,1,true);
        if(!strcmp(id,"D13a")) complete(h,16);
    } else if(!strcmp(id,"D04f")) {
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,"allocation refusal");
        state_is(h,"VOID20:allocation_fail",-1,1,true);
    } else if(!strcmp(id,"D07a")||!strcmp(id,"D07b")) {
        dadmit(h,13);dquiet(h,12,13);dcontrol(h,admit(40,DG+1,21,1));dcontrol(h,bracket(40,DG+1,20,21,1));
        complete(h,21);state_is(h,"F20",!strcmp(id,"D07a")?20:-1,0,true);
        if(!strcmp(id,"D07b")) {complete(h,26);state_is(h,"F20 F25",25,0,true);}
    } else if(!strcmp(id,"D08a")||!strcmp(id,"D08b")) {
        dadmit(h,13);dquiet(h,12,13);
        if(!strcmp(id,"D08a")) {
            pgwt_discovery_attempt a={.kind=PGWT_DISC_TERMINAL,.pid=40,.cookie=DG+1,
                .terminal_time=DB+18,.terminal_binding_proved=true};dcontrol(h,a);
            record(h,18,40,DG+1,3,PGWT_MERGE_EXIT,0,false,false);
        }
        complete(h,18);state_is(h,"F17",-1,1,true);complete(h,19);
        state_is(h,!strcmp(id,"D08a")?"F17 EXIT18 F18":"F17 F18",!strcmp(id,"D08a")?18:-1,!strcmp(id,"D08a")?0:1,true);
    } else if(!strcmp(id,"D09")) {
        dadmit(h,8);data(h,12,0,1);data(h,18,2,1);
        record(h,18,30,DG,3,PGWT_MERGE_EXIT,1,false,false);
        record(h,20,30,1,3,PGWT_MERGE_FORK,0,false,false);
        record(h,22,30,1,0,PGWT_MERGE_TRANS,0,false,true);
        data(h,15,1,1);complete(h,23);
        state_is(h,"D12U D15T D18T EXIT18 FORK20 D22U F22",12,0,true);
    } else if(!strcmp(id,"D13b")) {
        /* Otherwise valid coverage/certificates cannot repair an invalid
         * population member. This is the population gate, not A2 limits. */
        a2_population(h);DC(h->data_count==0,"discovery contributes no lifecycle DATA");
        state_is(h,"",-1,1,true);
    } else if(!strcmp(id,"D13d")) {
        dadmit(h,8);data(h,12,0,1);h->reject_data=true;complete(h,13);
        state_is(h,"VOID20:writer_push_fail",-1,1,true);
        pgwt_discovery_state s;DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&s),"D retained");
        DC(!s.has_lower_bound,"rejected publication commits no lower bound");
    } else if(!strcmp(id,"Q08")) {
        pgwt_discovery_attempt a=bracket(30,DG,12,13,1);a.witness.resolved_address++;
        dcontrol(h,a);complete(h,16);state_is(h,"F15",-1,1,true);
        pgwt_discovery_state r;DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&r),"candidate lifetime retained");
        DC(r.baseline.t==0,"address mismatch rejects quiet candidate");
    } else if(!strcmp(id,"Q09")) {
        dcontrol(h,admit(30,DG,12,3));pgwt_discovery_attempt final=bracket(30,DG,11,12,3);
        final.witness.v0=final.witness.v1=9;dcontrol(h,final);data(h,9,0,1);
        complete(h,12);state_is(h,"D9U F11",11,0,true);
        pgwt_discovery_state after_first;
        DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&after_first) &&
           after_first.confirmed && after_first.lower_bound==DB+11 &&
           after_first.baseline.t==DB+11 && after_first.baseline.v0==9 &&
           after_first.attachment_generation==3,"D9 does not rebase final attachment seed");
        data(h,15,0,3);complete(h,16);state_is(h,"D9U F11 D15T F15",11,0,true);
        pgwt_discovery_state s;DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&s),"confirmed baseline retained");
        DC(s.baseline.t==DB+11,"earlier attempt cannot overwrite final baseline");
    } else if(id[0]=='Q') {
        dadmit(h,13);pgwt_discovery_attempt a=bracket(30,DG,12,13,1);
        bool good=!strcmp(id,"Q01")||!strcmp(id,"Q07");
        if(!strcmp(id,"Q02")) a.witness.v1=8;
        if(!strcmp(id,"Q03")) a.witness.S1=PGWT_DISC_RUNNING;
        if(!strcmp(id,"Q04")) a.witness.S0=a.witness.S1=PGWT_DISC_STATE_COUNTER_ONLY;
        if(!strcmp(id,"Q04b")) a.witness.S0=a.witness.S1=PGWT_DISC_SCHEDSTAT_ONLY;
        if(!strcmp(id,"Q05")) a.witness.S1=PGWT_DISC_DENIED;
        if(!strcmp(id,"Q06")) a.witness.threads0=2;
        if(!strcmp(id,"Q07")) a.witness.wait_word=0;
        dcontrol(h,a);data(h,15,0,1);complete(h,16);
        state_is(h,good?"D15T F15":"D15U F15",good?12:15,0,true);
        if(good) {
            pgwt_discovery_state s;DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&s),"quiet baseline stored");
            DC(s.baseline.cpu_base_valid && s.baseline.cpu_base==0 && s.baseline.on_cpu_ts==0,"valid zero CPU base and off-CPU seed");
        }
    } else DC(false,"row implemented");
    full_state(h,id);numerical_outputs(h,id);
    pgwt_capture_destroy(h->capture);free(h);
}
/* Additional adversarial schedules retain their controlling oracle row ID.
 * They distinguish guards whose physical facts overlap in the printed row. */
static void adversarial_guards(const char *id)
{
    struct dh *h;
    if(!strcmp(id,"D01")) {
        h=dsetup(false,5,false);dadmit(h,8);
        record(h,12,30,DG,0,PGWT_MERGE_TRANS,1,true,false);data(h,15,0,1);complete(h,16);
        DC(!strcmp(h->output,"D12U D15U F15"),"stale marker cannot establish lower bound");
        DC(pgwt_capture_discovery_status(h->capture).closure==DB+15,"fresh baseline follows stale marker");
    } else if(!strcmp(id,"Q09")) {
        h=dsetup(false,5,false);dcontrol(h,admit(30,DG,12,3));
        data(h,9,0,3);data(h,15,0,3);complete(h,16);
        DC(!strcmp(h->output,"D9U D15U F15"),"pre-enable record cannot establish lower bound");
        DC(pgwt_capture_discovery_status(h->capture).closure==DB+15,"pre-enable record leaves lifetime pending");
    } else if(!strcmp(id,"D07a")) {
        h=dsetup(true,5,false);dadmit(h,13);dquiet(h,12,13);complete(h,14);
        pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
        DC(!r.closed && r.unresolved==1,"pending candidate prevents startup closure");
    } else if(!strcmp(id,"D07b")) {
        h=dsetup(true,25,false);dadmit(h,13);dquiet(h,12,13);
        dcontrol(h,admit(40,DG+1,21,1));dcontrol(h,bracket(40,DG+1,20,21,1));
        complete(h,25);
        pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
        DC(!r.closed && r.unresolved==0,"census completeness must be strictly beyond closure");
        complete(h,26);
        DC(pgwt_capture_discovery_status(h->capture).closure==DB+25,"census fence controls C");
    } else if(!strcmp(id,"D08b")) {
        h=dsetup(true,5,false);dadmit(h,13);dquiet(h,12,13);
        pgwt_discovery_attempt a={.kind=PGWT_DISC_TERMINAL,.pid=40,.cookie=DG+1,
            .terminal_time=DB+18,.terminal_binding_proved=false};dcontrol(h,a);
        record(h,18,40,DG+1,3,PGWT_MERGE_EXIT,0,false,false);
        complete(h,18);complete(h,19);
        pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
        DC(!r.closed && r.unresolved==1 && !strcmp(h->output,"F17"),"ESRCH cannot authorize matching EXIT");
    } else if(!strcmp(id,"D13a")) {
        h=dsetup(false,5,false);h->snapshot.value[0]=1;
        ma_envelope check={.version=1,.kind=MA_CHECK};
        DC(pgwt_capture_envelope(h->capture,&check,DB+20,&h->snapshot)==0,"fault checkpoint");
        dadmit(h,8);
        DC(pgwt_capture_report(h->capture).status==PGWT_MERGE_SUSPENDED,"discovery cannot auto-REARM");
        DC(!strcmp(h->output,"VOID20:counter_change"),"discovery retains existing fault");
    } else if(!strcmp(id,"D13d")) {
        h=dsetup(false,5,false);dadmit(h,13);dquiet(h,12,13);h->reject_data=true;
        data(h,15,0,1);complete(h,16);
        pgwt_discovery_state r;DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&r),"quiet candidate retained");
        DC(!r.has_lower_bound,"quiet publication failure commits no lower bound");
        DC(!pgwt_capture_discovery_status(h->capture).closed,"failed supporting record leaves startup open");
    } else if(!strcmp(id,"D04a-binding")) {
        h=dsetup(false,5,false);pgwt_discovery_attempt a=admit(30,DG,8,1);
        a.proof.truth&=~(uint64_t)PGWT_DISC_PINNED_BINDING;dcontrol(h,a);
        DC(!strcmp(h->output,"VOID20:unknown_evidence"),"false required admission fact faults");
        record(h,10,30,DG,3,PGWT_MERGE_INIT,1,false,false);data(h,12,0,1);complete(h,13);
        DC(!strcmp(h->output,"VOID20:unknown_evidence"),"INIT cannot repair invalid discovery");
    } else if(!strcmp(id,"D13b")) {
        h=dsetup(false,5,false);
        record(h,10,50,1,3,PGWT_MERGE_FORK,0,false,false);
        DC(pgwt_capture_a2_observed_births(h->capture,DB,DB+1000)==1,
           "actual FORK contributes one observed birth");
        DC(pgwt_capture_a2_observed_births(h->capture,DB+11,DB+1000)==0,
           "observed birth belongs only to its event-time cohort");
        pgwt_a2_omission u={.pid=50,.cookie=1,.fork_ns=DB+10,.exit_ns=DB+12,.receive_ns=DB+20};
        DC(pgwt_capture_a2_omission_population(h->capture,&u)==PGWT_A2_POPULATION_OK,
           "observed in-boot FORK passes only the population gate");
        u.fork_ns=DB-1;
        DC(pgwt_capture_a2_omission_population(h->capture,&u)==PGWT_A2_UNTRACED_INVALID,
           "pre-boot omission is not clamped into cohort zero");
    } else return;
    pgwt_capture_destroy(h->capture);free(h);
}
static void empty_census(void)
{
    discovery_case="empty_census";
    struct dh *h=dsetup_inventory(0,5,false,false);
    complete(h,16);
    state_is(h,"F15",-1,0,false);
    DC(base_verdict(h,6,10)==PGWT_COV_INCOMPLETE,"unknown empty census cannot certify a window");
    DC(pgwt_capture_discovery_census(h->capture,DB+5,true,DB+20,&h->snapshot)==0,"explicit complete empty census");
    state_is(h,"F15",5,0,true);endpoint_verdict(h,5);
    DC(base_verdict(h,6,10)==PGWT_COV_COMPLETE,"complete empty census certifies ordinary windows");
    pgwt_capture_destroy(h->capture);free(h);

    h=dsetup_inventory(0,16,false,true);complete(h,16);
    state_is(h,"F15",-1,0,true);
    complete(h,17);state_is(h,"F15 F16",16,0,true);
    endpoint_verdict(h,16);
    pgwt_capture_destroy(h->capture);free(h);

    h=dsetup_inventory(0,5,false,true);h->reject_close=true;complete(h,16);
    state_is(h,"F15 VOID20:writer_push_fail",-1,0,true);
    DC(h->ngaps==1 && h->coverage_count==1,"rejected closure is not a published gap close");
    DC(base_verdict(h,6,10)==PGWT_COV_INCOMPLETE,"failed empty-census close cannot certify a window");
    DC(pgwt_capture_report(h->capture).status==PGWT_MERGE_SUSPENDED,"failed gap publication preserves writer fault");
    pgwt_capture_destroy(h->capture);free(h);
    puts("empty census: PASS (unknown versus complete, strict fence, publication failure, reader windows)");
}
int main(int argc,char **argv)
{
    size_t ran=0;
    for(size_t i=0;i<sizeof(oracle_rows)/sizeof(*oracle_rows);++i) {
        if(argc>1 && strcmp(argv[1],oracle_rows[i].id)) continue;
        discovery_case=oracle_rows[i].id;printf("REACHED discovery %s\n",discovery_case);fflush(stdout);
        run_row(discovery_case);adversarial_guards(discovery_case);printf("discovery %s: full consumer output PASS\n",discovery_case);++ran;
    }
    DC(ran>0,"named oracle row reached");
    if(argc==1) empty_census();
    printf("discovery full consumer outputs: %zu passed (including numerical and A2 population assertions)\n",ran);
    return 0;
}
