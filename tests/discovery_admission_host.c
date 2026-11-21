#define PGWT_TRANSPORT_EMBEDDED
#include "test_percpu_transport.c"
#undef main
#define put_le discovery_capture_put_le
#include "../src/merge_capture.c"
#undef put_le
#include "../src/core/merge.c"
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
    unsigned char bytes[272];
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
    bool reject_data, reject_close, reject_exit, unknown_identity;
    uint64_t detection;
    uint64_t boundaries;
    bool query_dependency;
    bool allow_shared_text;
    bool reject_support;
    size_t supporting;
    bool lifecycle_counter_after_exit;
    bool counter_after_exit;
    bool counter_after_frontier;
    bool reject_frontier;
    bool unaccounted_rejection;
    size_t reject_data_number;
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
    static const char *labels[]={"C0","Ce","Cf","Cs","Cw","Cr","Ck","Ci","Cx","Cb"};
    static const size_t components[]={PGWT_MERGE_COUNTERS,0,10,12,14,3,0,1,2,11};
    if(!counters.known) return "unknown";
    for(size_t k=0;k<10;++k) {
        bool equal=true;
        for(size_t j=0;j<PGWT_MERGE_COUNTERS;++j)
            equal=equal && counters.value[j]==(j==components[k]?1:0);
        if(equal) return k==1 && !strcmp(discovery_case,"J7")?"Ck":labels[k];
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
    if(out->kind==PGWT_MERGE_OUT_TEXT && h->allow_shared_text) {
        DC(out->record.payload_size==272 && !out->record.discovery.exclusive_text,
           "shared TEXT retains metadata semantics");
        full_append(h,"TEXTx");return true;
    }
    DC(out->kind!=PGWT_MERGE_OUT_TEXT,"exclusive abandoned TEXT is never published");
    DC(out->kind==PGWT_MERGE_OUT_DATA || out->kind==PGWT_MERGE_OUT_FRONTIER ||
       out->kind==PGWT_MERGE_OUT_VOID,"every output kind is specified by this oracle");
    if((out->kind==PGWT_MERGE_OUT_FRONTIER && h->reject_frontier) ||
       (out->kind==PGWT_MERGE_OUT_DATA && (h->reject_data ||
        (h->reject_data_number && h->data_count+1==h->reject_data_number) ||
        (h->reject_exit && out->record.rank==PGWT_MERGE_EXIT)))) {
        post->d=DB+20;
        if(!h->unaccounted_rejection) post->counters.value[14]=1;
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
        bool terminal_binding=!copy.cookie && r->rank==PGWT_MERGE_EXIT &&
            r->g==(copy.pid==40?DG+1:DG);
        DC(copy.pid==r->pid && (copy.cookie==r->g || terminal_binding) && copy.rank==r->rank &&
           r->identity_known && !r->has_q && !r->q,"released lifetime identity and rank");
        DC(copy.size==r->payload_size && !memcmp(copy.bytes,r->payload,copy.size),"all raw payload bytes preserved");
        copy.certified=out->prior_interval_certified;
        DC(h->nreleased<32,"released record bound");h->released[h->nreleased++]=copy;
        const char *name=r->rank==PGWT_MERGE_TRANS?
            (r->pid==10?"A":r->g==1?"R":r->pid==40?"H":r->pid==50?"U":r->t==DB+18?"terminal_D":"D"):
            r->rank==PGWT_MERGE_EXIT?(r->pid==40?"EXIT_H":r->pid==50?"EXIT_U":"EXIT_D"):
            r->rank==PGWT_MERGE_FORK?"FORK_R":"INIT";
        full_append(h,"%s%llu%s",name,(unsigned long long)(r->t-DB),
            r->rank==PGWT_MERGE_TRANS?(copy.certified?"[T]":"[U]"):"");
        append(h,r->rank==PGWT_MERGE_TRANS?"D":r->rank==PGWT_MERGE_EXIT?"EXIT":
               r->rank==PGWT_MERGE_FORK?"FORK":"INIT",r->t-DB,
               r->rank==PGWT_MERGE_TRANS?(out->prior_interval_certified?"T":"U"):"");
        if(h->counter_after_exit && r->rank==PGWT_MERGE_EXIT) {
            post->counters.value[h->lifecycle_counter_after_exit?10:0]=1;h->snapshot.value[h->lifecycle_counter_after_exit?10:0]=1;
        }
    } else if(out->kind==PGWT_MERGE_OUT_FRONTIER) {
        append(h,"F",out->F-DB,"");
        full_append(h,"F%llu[%s]",(unsigned long long)(out->F-DB),counter_label(out->counters));
        if(h->counter_after_frontier) {
            post->counters.value[0]=1;h->snapshot.value[0]=1;
        }
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
    DC(decoded.b==0 && decoded.d==0,"GAP_CLOSE reserved fields stay zero");
    DC(h->coverage_count<32,"gap record bound");
    h->coverage[h->coverage_count++]=(struct pgwt_cov_rec){.kind=decoded.kind,.a=(int64_t)decoded.a,.c=(int64_t)decoded.c};
    return true;
}
static bool dsupport(void *ctx, uint64_t pid, uint64_t cookie,
                       const pgwt_discovery_no_word *w, pgwt_merge_observation *post)
{
    struct dh *h=ctx;
    DC(pid==30 && cookie==DG && w->role && w->enable==DB+6,
       "supporting no-word evidence retains lifetime and role");
    if(h->reject_support) {post->d=DB+20;post->counters.value[14]=1;return false;}
    ++h->supporting;
    return true;
}
static struct dh *dsetup_start(size_t count, uint64_t fence, bool qmode, bool census, bool pid_only)
{
    struct dh *h=calloc(1,sizeof(*h));DC(h,"host fixture allocation");
    h->snapshot=readable();h->detection=20;
    pgwt_merge_source sources[4]={{.qualified=true},{.qualified=true},{.qualified=true},{.qualified=true}};
    pgwt_merge_attachment attached[2]={{.pid=10,.g=1,.identity_proved=true,.sequence_proved=true,.next_q=1},
        {.pid=20,.g=1,.identity_proved=true,.sequence_proved=true,.next_q=1}};
    pgwt_merge_config initial={.mode=qmode?PGWT_MERGE_Q:PGWT_MERGE_N,
        .sources=sources,.source_count=4,.staged_capacity=64,.payload_capacity=272,
        .attachments=attached,.attachment_count=2,
        .attachment_capacity=64,.frontier_capacity=128,.dependency_capacity=64,
        .observation={.d=DB+20,.counters={.known=true}},.last_observed={.known=true}};
    struct pgwt_capture_config cfg={.cpu_count=2,.capacity=64,.ctx=h,
        .sink={.emit=dsink,.ctx=h},.baseline=initial.observation,.publish_startup_gap=dgap,
        .test_attach_premise=true,.discovery_enabled=true,.publish_no_wait_word=dsupport};
    ma_lifetime lifetimes[2]={{.pid=10,.g=1,.identity_bound=true},
        {.pid=20,.g=1,.identity_bound=true}};
    ma_table premises={.lifetimes=lifetimes,.lifetime_count=2};
    h->capture=pgwt_capture_replay_create(&cfg,&initial,&premises);DC(h->capture,"capture create");
    const uint32_t listed[]={30};
    DC(pgwt_capture_coverage_begin(h->capture,DB,pid_only?listed:NULL,pid_only?1:0,NULL)==0,"begin startup gap");
    if(count) DC(pgwt_capture_discovery_inventory(h->capture,30,DG)==0,"D inventory");
    if(count && (discovery_case[0]=='A' || discovery_case[0]=='V' || discovery_case[0]=='G')) {
        uint64_t facts=PGWT_DISC_IDENTITY_VISIBLE | PGWT_DISC_EMISSION_IDENTITY;
        DC(pgwt_capture_discovery_installed(h->capture,30,DG,
            (pgwt_discovery_facts){facts,facts})==0,"D installed identity report");
    }
    if(count>1) DC(pgwt_capture_discovery_inventory(h->capture,40,DG+1)==0,"H inventory");
    if(census) DC(pgwt_capture_discovery_census(h->capture,DB+fence,true,DB+20,&h->snapshot)==0,"complete census");
    return h;
}
static struct dh *dsetup_inventory(size_t count, uint64_t fence, bool qmode, bool census)
{ return dsetup_start(count,fence,qmode,census,false); }
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
    DC(pgwt_capture_discovery_control(h->capture,&a,DB+h->detection,&h->snapshot)==0,"discovery control delivery");
    DC(!memcmp(&a,&before,sizeof(a)),"consumer preserves supplied producer attempt");
}
static void dadmit(struct dh *h,uint64_t e) {dcontrol(h,admit(30,DG,e,1));}
static void dquiet(struct dh *h,uint64_t t,uint64_t e) {dcontrol(h,bracket(30,DG,t,e,1));}
static void dabandon(struct dh *h,uint64_t pid,uint64_t cookie)
{dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_ABANDON,.pid=pid,.cookie=cookie});}
static void dterminal(struct dh *h)
{dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_TERMINAL,.pid=30,.cookie=DG,
    .terminal_time=DB+18,.terminal_binding_proved=true});}
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
        if(!strcmp(discovery_case,"D14c")) {
            old=t==11?DY:DX;next=t==11?DZ:0;duration=t==11?0:4;
        }
        if(!strcmp(discovery_case,"D14a") && pid==10) {old=DY;next=0;duration=2;}
        if(discovery_case[0]=='A') {
            old=t==12 || t==15?DX:DY;next=t==12?DY:DX;
            duration=t==12?3:t==15?3:t==17?5:t==18?6:0;
            if(t==18) next=PGWT_EVENT_EXIT;
        }
        if(!strcmp(discovery_case,"Q01") || !strcmp(discovery_case,"Q02") ||
           !strcmp(discovery_case,"Q03") || !strcmp(discovery_case,"Q04") ||
           !strcmp(discovery_case,"Q04b") || !strcmp(discovery_case,"Q05") ||
           !strcmp(discovery_case,"Q06") || !strcmp(discovery_case,"Q07")) cpu=1;
        if(!strcmp(discovery_case,"Q07")) {old=0;next=DY;}
        tb_le(bytes,DB+t,8);tb_le(bytes+8,pid,4);tb_le(bytes+12,old,4);tb_le(bytes+16,next,4);
        tb_le(bytes+24,duration,8);tb_le(bytes+40,cpu,8);
        if(h->query_dependency) tb_le(bytes+32,77,8);
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
    if(h->unknown_identity) {
        pgwt_merge_input in={.kind=PGWT_MERGE_DATA,
            .observation={.d=DB+h->detection,.counters={.known=true}},
            .record={.source=src,.o=e.position,.t=DB+t,.pid=pid,.g=cookie,.rank=rank,
                .identity_known=false,.payload=bytes,.payload_size=size}};
        DC(pgwt_capture_replay_control(h->capture,&in)==PGWT_MERGE_SUSPENDED,
           "uncertain record attribution rejected before discard");
        return;
    }
    int received=pgwt_capture_received_token(h->capture,&e,DB+h->detection,&token);
    DC(received>=0,"record consumption");
    if(!received) DC(pgwt_capture_envelope_token(h->capture,&e,token,DB+h->detection,&h->snapshot)==0,"record adapter delivery");
}
static void data(struct dh *h,uint64_t t,size_t source,uint64_t gen)
{record(h,t,30,DG,source,PGWT_MERGE_TRANS,gen,false,true);}
static void text_for_d(struct dh *h)
{
    unsigned char bytes[272]={0};tb_le(bytes,4,4);tb_le(bytes+4,30,4);tb_le(bytes+8,77,8);
    ma_envelope e={.kind=MA_TEXT,.version=1,.origin=MA_EXACT,.source=3,.position=++h->pos[3],
        .bytes=bytes,.size=sizeof(bytes),.cookie_present=true,.cookie=DG,
        .discovery_origin={.exclusive_text=!h->allow_shared_text}};
    size_t token=0;
    DC(pgwt_capture_received_token(h->capture,&e,DB+20,&token)==0,"TEXT receipt");
    ma_table t=table(h->capture);
    size_t slot=ma_binding_bucket(&t,77,NULL);
    h->capture->bindings[h->capture->binding_index[slot]-1]=(ma_binding){.query_id=77,
        .dependency_id=1,.kind=MA_WAIT};
    h->query_dependency=true;
    DC(pgwt_capture_envelope_token(h->capture,&e,token,DB+20,&h->snapshot)==0,"exclusive metadata delivery");
}
static void complete(struct dh *h,uint64_t w)
{
    for(size_t s=0;s<4;++s) {
        ma_envelope e={.kind=MA_BOUNDARY,.version=1,.source=s,.position=++h->pos[s],
            .w=DB+w,.boundary=s<2?MA_IDLE_PERF:MA_RING_BOUNDARY};
        ++h->boundaries;
        DC(pgwt_capture_envelope(h->capture,&e,DB+h->detection,&h->snapshot)==0,"source completeness");
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
static void endpoint_verdict(struct dh *h,uint64_t closure,bool increment)
{
    DC(base_verdict(h,closure,closure+1)==(increment?PGWT_COV_INCOMPLETE:PGWT_COV_COMPLETE),
       "reader availability at semantic C");
    DC(base_verdict(h,closure+1,closure+2)==PGWT_COV_COMPLETE,
       "reader permits interval after endpoint");
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
    DC(gap.census_fence==DB+(!strcmp(id,"D07b")?25:!strcmp(id,"D14b")?15:5),"retained census fence");
    DC(base_verdict(h,99,100)==(expected->persisted>=0?PGWT_COV_COMPLETE:PGWT_COV_INCOMPLETE),
       "persisted reader gap state");
    size_t ngaps=!strcmp(id,"D10")?3:expected->close>=0?2:1;
    DC(h->ngaps==ngaps,"complete startup gap publication sequence");
    DC(h->gaps[0].kind==PGWT_CAPTURE_GAP_PREEXISTING && h->gaps[0].at==DB &&
       h->gaps[0].detected==DB,"startup opens before any coverage");
    if(ngaps>=2) {
        uint64_t close=!strcmp(id,"D10")?13:(uint64_t)expected->persisted;
        DC(h->gaps[1].kind==PGWT_CAPTURE_GAP_PREEXISTING_END &&
           h->gaps[1].at==DB+close && h->gaps[1].detected==DB+20,
           "persisted startup endpoint and publication time");
        DC(h->coverage[1].a==(int64_t)(DB+close),"encoded GAP_CLOSE time");
        if(ngaps==2) {
            DC(base_verdict(h,close-1,close)==PGWT_COV_INCOMPLETE,"reader before persisted endpoint");
            DC(base_verdict(h,close,close+1)==PGWT_COV_COMPLETE,"reader at persisted endpoint");
        }
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
    bool terminal=!strcmp(id,"A03")||!strcmp(id,"A09")||!strcmp(id,"A17")||!strcmp(id,"A20");
    DC(d.terminal_released==terminal,"D matching terminal discharge");
    if(terminal) DC(d.terminal_time==DB+18,"D terminal discharge time");
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
    DC(report.epoch_invalid==!strcmp(id,"A11") && !report.void_failed && report.status!=PGWT_MERGE_EXIT_REQUIRED,
       "oracle does not invalidate epochs or require process exit");
    DC(report.discovery_abandoned_records==expected->abandoned,"cumulative consumed abandonment count");
    DC(pgwt_capture_coverage(h->capture,DB+100).discovery_abandoned_records==expected->abandoned,
       "capture diagnostic agrees with consumed discard count");
    bool abandoned=id[0]=='V' || (id[0]=='A' && strcmp(id,"A04") && strcmp(id,"A06") && strcmp(id,"A20"));
    DC(d.abandoned==abandoned,"retained lifetime abandonment disposition");
    bool fault=strstr(expected->output,"VOID")!=NULL && strcmp(id,"A12") && id[0]!='V';
    DC(report.status==(fault?PGWT_MERGE_SUSPENDED:PGWT_MERGE_OK),"final epoch status");
    if(!strcmp(id,"A01") || !strcmp(id,"A19")) {
        FILE *stream=tmpfile();DC(stream,"wire accounting output stream");
        uint64_t wire=0;for(size_t i=0;i<4;++i) wire+=h->pos[i];
        pgwt_capture_coverage_print(stream,h->capture,DB+100,wire,h->boundaries,0,0);
        rewind(stream);char output[2048]={0};size_t n=fread(output,1,sizeof(output)-1,stream);
        DC(n>0 && !ferror(stream),"wire accounting output read");fclose(stream);
        DC(strstr(output,!strcmp(id,"A01")?"discovery_abandoned_records=2 ok=1":"discovery_abandoned_records=3 ok=1"),
           "wire identity accounts for consumed abandonment separately");
    }
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
        if(r->rank!=PGWT_MERGE_TRANS || (r->cookie!=DG && r->pid!=10)) continue;
        uint64_t t=get_le(r->bytes,8),dur=get_le(r->bytes+24,8);
        last=(uint32_t)get_le(r->bytes+16,4);last_t=t;
        if(r->certified && get_le(r->bytes+12,4)==event)
            waits[n++]=(pgwt_core_wait){.event={"selected",8},.start=t-dur,.end=t};
    }
    bool available=base_verdict(h,from,to)==PGWT_COV_COMPLETE;
    pgwt_core_span span={.start=DB+from,.end=DB+to};
    pgwt_core_evidence evidence={.life_start=DB,.coverage=&span,.coverage_count=available?1:0,
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
    bool fresh=!strcmp(id,"D01")||!strcmp(id,"D03")||!strcmp(id,"D14a");
    bool measured=fresh||!strcmp(id,"D02a")||!strcmp(id,"Q01")||!strcmp(id,"Q09")||
        !strcmp(id,"D14b")||!strcmp(id,"D14c");
    bool unavailable=!strcmp(id,"D02c")||!strcmp(id,"Q02")||!strcmp(id,"Q03")||
        !strcmp(id,"Q04")||!strcmp(id,"Q04b")||!strcmp(id,"Q05")||!strcmp(id,"Q06");
    if(measured || unavailable) {
        pgwt_core_result time,count;
        released_metrics(h,DX,!strcmp(id,"D02c")?10:!strcmp(id,"Q09")?11:
                  fresh?13:!strcmp(id,"D14b")?15:12,
                  !strcmp(id,"D02c")?13:16,&time,&count);
        if(measured) DC(time.value.present && time.value.value==(!strcmp(id,"Q09")?4:
            fresh?2:!strcmp(id,"D14b")?0:3) &&
            count.value.present && count.value.value==1,"X duration and completed count");
        else {
            DC(!time.value.present && !count.value.present,"incoming or open interval quantities unavailable");
            DC(time.diagnostic.recorded.present && time.diagnostic.recorded.value==0 &&
               count.diagnostic.recorded.present && count.diagnostic.recorded.value==0,
               "no invented X duration or completed count");
        }
    }
    if(fresh || !strcmp(id,"D14c")) {
        pgwt_core_result time,count;released_metrics(h,DX,!strcmp(id,"D14c")?11:12,16,&time,&count);
        DC(!time.value.present && !count.value.present,"reader refuses window containing uncertified endpoint");
    }
    if(!strcmp(id,"D01") || !strcmp(id,"D14a")) {
        pgwt_core_result time,count;released_metrics(h,DY,12,13,&time,&count);
        DC(!count.value.present,"unknown completed count at C is unavailable");
    }
    if(!strcmp(id,"Q01") || !strcmp(id,"Q07") || !strcmp(id,"Q02") || !strcmp(id,"Q03")) {
        bool has_cpu=false;uint64_t cpu=0;
        for(size_t i=0;i<h->nreleased;++i) {
            const struct discovery_record *r=&h->released[i];
            if(r->rank!=PGWT_MERGE_TRANS || !r->certified) continue;
            struct pgwt_trace_event e;memcpy(&e,r->bytes,sizeof(e));
            if(e.cpu_ns==PGWT_CPU_NS_UNKNOWN) continue;
            if(base_verdict(h,12,!strcmp(id,"Q07")?15:16)!=PGWT_COV_COMPLETE) continue;
            has_cpu=true;cpu+=pgwt_cpu_clip_ns(&e,DB+12,!strcmp(id,"Q07")?DB+15:DB+16,NULL);
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
static void abandon_row(struct dh *h,const char *id)
{
    if(!strcmp(id,"A04")) {
        dadmit(h,8);data(h,12,0,1);complete(h,13);dabandon(h,30,DG);return;
    }
    if(!strcmp(id,"A06")) {dabandon(h,40,DG+1);return;}
    if(!strcmp(id,"A20")) {
        dterminal(h);record(h,18,30,DG,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        dabandon(h,30,DG);return;
    }
    if(!strcmp(id,"A19")) {
        text_for_d(h);data(h,9,0,1);record(h,10,30,DG,0,PGWT_MERGE_TRANS,1,true,true);
        dabandon(h,30,DG);complete(h,16);return;
    }
    h->unknown_identity=!strcmp(id,"A16");
    data(h,9,0,1);
    if(!strcmp(id,"A18"))
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,"fault before abandonment");
    if(!strcmp(id,"A07")) {
        dcontrol(h,admit(40,DG+1,13,1));dcontrol(h,bracket(40,DG+1,12,13,1));
        dabandon(h,30,DG);record(h,15,40,DG+1,1,PGWT_MERGE_TRANS,1,false,true);complete(h,16);
        pgwt_discovery_state state;
        DC(pgwt_merge_discovery_state(h->capture->core,40,DG+1,&state) && state.lower_bound==DB+12,
           "H quiet bound independent of D abandonment");return;
    }
    bool base=!strcmp(id,"A01")||!strcmp(id,"A02")||!strcmp(id,"A03")||!strcmp(id,"A09")||
        !strcmp(id,"A10")||!strcmp(id,"A11")||!strcmp(id,"A12")||!strcmp(id,"A15")||!strcmp(id,"A17");
    if(base) {
        data(h,12,0,1);complete(h,13);state_is(h,"",-1,1,true);
    }
    dabandon(h,30,DG);
    if(!strcmp(id,"A05")) {dadmit(h,8);complete(h,16);return;}
    if(!strcmp(id,"A08")||!strcmp(id,"A14")) {
        if(!strcmp(id,"A08")) h->snapshot.value[0]=1;
        else h->snapshot.valid=false;
        ma_envelope e={.version=1,.kind=MA_CHECK};
        DC(pgwt_capture_envelope(h->capture,&e,DB+20,&h->snapshot)==0,"abandoned failure checkpoint");
    }
    if(!strcmp(id,"A13")) {
        for(size_t s=0;s<4;++s) {
            if(s==2) continue;
            ma_envelope e={.kind=MA_BOUNDARY,.version=1,.source=s,.position=++h->pos[s],
                .w=DB+16,.boundary=s<2?MA_IDLE_PERF:MA_RING_BOUNDARY};
            DC(pgwt_capture_envelope(h->capture,&e,DB+20,&h->snapshot)==0,"partial source completeness");
        }
        return;
    }
    complete(h,16);
    if(!strcmp(id,"A02")) {data(h,17,0,1);complete(h,20);}
    if(!strcmp(id,"A10")) dquiet(h,12,13);
    if(!strcmp(id,"A11")) data(h,14,0,1);
    if(!strcmp(id,"A12")) {
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,"fault after discards");
        pgwt_merge_attachment proofs[2]={{.pid=10,.g=1,.identity_proved=true},
            {.pid=20,.g=1,.identity_proved=true}};
        pgwt_merge_input rearm={.kind=PGWT_MERGE_REARM,.observation={.d=DB+21,.counters={.known=true}},
            .attachments=proofs,.attachment_count=2,.attachment_proofs_fresh=true};
        DC(pgwt_capture_replay_control(h->capture,&rearm)==PGWT_MERGE_OK,"explicit REARM accepted");
        h->detection=22;data(h,17,0,1);complete(h,18);
    }
    if(!strcmp(id,"A03")||!strcmp(id,"A09")||!strcmp(id,"A15")||!strcmp(id,"A17")) {
        data(h,18,2,1);record(h,18,30,DG,3,PGWT_MERGE_EXIT,0,false,false);dterminal(h);
        complete(h,18);state_is(h,"F12 F15 F17",-1,1,true);
        DC(pgwt_capture_report(h->capture).discovery_abandoned_records==2,"terminal at W is not consumed");
        h->reject_exit=!strcmp(id,"A15");complete(h,19);
        if(!strcmp(id,"A09")) dabandon(h,30,DG);
        if(!strcmp(id,"A17")) {
            record(h,20,30,1,3,PGWT_MERGE_FORK,0,false,false);
            record(h,22,30,1,0,PGWT_MERGE_TRANS,0,false,true);complete(h,23);
        }
    }
}
static void terminal_at(struct dh *h, uint64_t at, bool unresolved)
{
    dcontrol(h,(pgwt_discovery_attempt){.kind=unresolved?PGWT_DISC_TERMINAL_UNRESOLVED:PGWT_DISC_TERMINAL,
        .pid=30,.cookie=DG,.terminal_time=DB+at,.terminal_binding_proved=!unresolved});
}
static void rearm_terminal(struct dh *h)
{
    pgwt_merge_attachment proofs[2]={{.pid=10,.g=1,.identity_proved=true},
        {.pid=20,.g=1,.identity_proved=true}};
    h->snapshot=pgwt_capture_last_snapshot(h->capture);
    pgwt_merge_input rearm={.kind=PGWT_MERGE_REARM,.observation={.d=DB+21},
        .attachments=proofs,.attachment_count=2,.attachment_proofs_fresh=true,.identity_repaired=true};
    rearm.observation.counters=h->capture->observation.counters;
    DC(pgwt_capture_replay_control(h->capture,&rearm)==PGWT_MERGE_OK,"terminal evidence explicit recovery");
    h->detection=22;
}
static void disposition_row(struct dh *h, const char *id)
{
    bool before=!strcmp(id,"T4"),missing=!strcmp(id,"T2");
    if(before) terminal_at(h,18,true);
    record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);
    complete(h,19);
    if(!before) DC(h->full_length==0,"cookie-less EXIT and frontier wait only for disposition");
    if(!before && !missing) terminal_at(h,18,true);
    if(!before) {
        record(h,20,10,1,0,PGWT_MERGE_TRANS,0,false,true);complete(h,21);
        if(missing) {
            complete(h,40);
            DC(h->full_length==0,"no timeout or boundary substitutes for disposition");
            DC(pgwt_capture_finish(h->capture,DB+50,&h->snapshot)==0,"missing disposition finish delivery");
        }
    }
    if(!strcmp(id,"T3")) terminal_at(h,18,false);
    if(!missing) {
        struct pgwt_capture_coverage c=pgwt_capture_coverage(h->capture,DB+100);
        DC(c.uncovered==1 && h->capture->payload_records==h->ninput,
           "unresolved EXIT counted exactly once as uncovered");
        DC(h->capture->inventory_count==1,"unresolved disposition preserves inventory");
        DC(h->capture->arrival_count==0 && pgwt_capture_pending_records(h->capture)==0,
           "final unresolved disposition releases EXIT and frontier");
    }
}
static void voided_terminal_row(struct dh *h, const char *id)
{
    uint64_t at=!strcmp(id,"V3")?17:18;
    dabandon(h,30,DG);
    if(strcmp(id,"V1")) terminal_at(h,at,false);
    h->counter_after_exit=!strcmp(id,"V1") || !strcmp(id,"V2");
    h->counter_after_frontier=!strcmp(id,"G_discharge_frontier_checkpoint");
    record(h,at,30,DG,3,PGWT_MERGE_EXIT,0,false,false);
    if(!strcmp(id,"V3")) {
        record(h,18,10,1,0,PGWT_MERGE_TRANS,0,false,true);h->reject_data_number=2;
    }
    h->reject_frontier=!strcmp(id,"V4");
    complete(h,19);
    pgwt_discovery_state state={0};
    DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&state) && !state.terminal_released,
       "VOIDed release attempt commits no terminal discharge");
    DC(!pgwt_capture_discovery_status(h->capture).closed && base_verdict(h,18,19)==PGWT_COV_INCOMPLETE,
       "VOIDed EXIT never closes startup gap");
    rearm_terminal(h);h->reject_frontier=false;h->counter_after_frontier=false;
    terminal_at(h,at,false);complete(h,20);
}
static void run_row(const char *id)
{
    bool two=!strcmp(id,"D07a")||!strcmp(id,"D07b")||!strcmp(id,"D08a")||!strcmp(id,"D08b")||
        !strcmp(id,"A04")||!strcmp(id,"A07");
    struct dh *h=dsetup(two,!strcmp(id,"D07b")?25:!strcmp(id,"D14b")?15:5,!strcmp(id,"D13e"));
    if(id[0]=='T') {
        disposition_row(h,id);
    } else if(id[0]=='V') {
        voided_terminal_row(h,id);
    } else if(id[0]=='A') {
        abandon_row(h,id);
    } else if(!strcmp(id,"D14a")||!strcmp(id,"D14b")) {
        dadmit(h,8);data(h,12,0,1);
        if(!strcmp(id,"D14a")) record(h,12,10,1,1,PGWT_MERGE_TRANS,0,false,true);
        data(h,15,0,1);complete(h,16);
    } else if(!strcmp(id,"D01")||!strcmp(id,"D10")) {
        dadmit(h,8);data(h,12,0,1);data(h,15,0,1);complete(h,16);
        state_is(h,"D12U D15T F15",12,0,true);endpoint_verdict(h,12,true);
        if(!strcmp(id,"D10")) {
            numerical_outputs(h,"D01");
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
        state_is(h,"VOID20:counter_change",-1,1,strcmp(id,"D04g")==0);
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
    } else if(!strcmp(id,"Q09")||!strcmp(id,"D14c")) {
        dcontrol(h,admit(30,DG,12,3));pgwt_discovery_attempt final=bracket(30,DG,11,12,3);
        final.witness.v0=final.witness.v1=9;dcontrol(h,final);data(h,!strcmp(id,"D14c")?11:9,0,!strcmp(id,"D14c")?2:1);
        complete(h,12);state_is(h,!strcmp(id,"D14c")?"D11U F11":"D9U F11",11,0,true);
        pgwt_discovery_state after_first;
        DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&after_first) &&
           after_first.confirmed && after_first.lower_bound==DB+11 &&
           after_first.baseline.t==DB+11 && after_first.baseline.v0==9 &&
           after_first.attachment_generation==3,"D9 does not rebase final attachment seed");
        data(h,15,0,3);complete(h,16);state_is(h,!strcmp(id,"D14c")?"D11U F11 D15T F15":"D9U F11 D15T F15",11,0,true);
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
    if(!strcmp(id,"A01")) {
        h=dsetup(false,5,false);data(h,9,0,1);data(h,12,0,1);dabandon(h,30,DG);complete(h,16);
        DC(pgwt_capture_pending_records(h->capture)==0 && h->capture->arrival_count==0,
           "consumed discards release all capture bookkeeping slots");
    } else if(!strcmp(id,"D01")) {
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
    } else if(!strcmp(id,"D08a") || !strcmp(id,"A15")) {
        h=dsetup(false,5,false);
        /* Identity insertion never completed. Retain the cookie-less kernel
         * witness; only TERM's matching census/lifecycle proof can bind it. */
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);
        pgwt_capture_discovery_report pending=pgwt_capture_discovery_status(h->capture);
        DC(pending.unresolved==1 && pending.census_complete,"cookie-less EXIT retains one census lifetime");
        complete(h,18);
        DC(h->nreleased==0,"unmatched cookie-less EXIT does not publish");
        h->reject_exit=!strcmp(id,"A15");dterminal(h);complete(h,19);
        pgwt_capture_discovery_report after=pgwt_capture_discovery_status(h->capture);
        bool rejected=!strcmp(id,"A15");
        DC(after.unresolved==(rejected?1:0) && after.closed==!rejected,
           "pre-install terminal discharge requires successful EXIT publication");
        DC(base_verdict(h,18,19)==(rejected?PGWT_COV_INCOMPLETE:PGWT_COV_COMPLETE),
           "pre-install terminal endpoint through reader");
        DC(h->nreleased==(rejected?0:1),"pre-install EXIT publication result");
    } else if(!strcmp(id,"A03")) {
        h=dsetup(false,5,false);
        dabandon(h,30,DG);record(h,18,30,DG,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        DC(h->nreleased==1 && pgwt_capture_discovery_status(h->capture).unresolved==1,
           "published EXIT awaits matching terminal proof");
        dterminal(h);
        DC(pgwt_capture_discovery_status(h->capture).closed && base_verdict(h,18,19)==PGWT_COV_COMPLETE,
           "later terminal proof matches retained successful EXIT");
        pgwt_capture_destroy(h->capture);free(h);
        h=dsetup(false,5,false);dabandon(h,30,DG);
        record(h,18,30,DG,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        data(h,19,2,1);dterminal(h);
        DC(!pgwt_capture_discovery_status(h->capture).closed &&
           pgwt_capture_discovery_status(h->capture).unresolved==1 &&
           pgwt_capture_report(h->capture).status==PGWT_MERGE_SUSPENDED,
           "terminal proof reconciles staged records beyond EXIT before discharge");
        pgwt_capture_destroy(h->capture);free(h);
        h=dsetup(false,5,false);dabandon(h,30,DG);h->counter_after_exit=true;
        record(h,18,30,DG,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        DC(h->nreleased==1 && pgwt_capture_report(h->capture).status==PGWT_MERGE_SUSPENDED,
           "successful EXIT publication precedes independent counter fault");
        pgwt_merge_attachment proofs[2]={{.pid=10,.g=1,.identity_proved=true},
            {.pid=20,.g=1,.identity_proved=true}};
        pgwt_merge_input rearm={.kind=PGWT_MERGE_REARM,.observation={.d=DB+21,.counters={.known=true,.value={1}}},
            .attachments=proofs,.attachment_count=2,.attachment_proofs_fresh=true,.identity_repaired=true};
        DC(pgwt_capture_replay_control(h->capture,&rearm)==PGWT_MERGE_OK,"terminal evidence explicit recovery");
        h->detection=22;dterminal(h);complete(h,20);
        DC(!pgwt_capture_discovery_status(h->capture).closed &&
           pgwt_capture_discovery_status(h->capture).unresolved==1 &&
           pgwt_capture_report(h->capture).faults[PGWT_MERGE_COUNTER_CHANGE]==1,
           "VOIDed EXIT never discharges after later terminal reconciliation");
        record(h,20,30,1,3,PGWT_MERGE_FORK,0,false,false);
        record(h,22,30,1,0,PGWT_MERGE_TRANS,0,false,true);complete(h,23);
        DC(pgwt_capture_report(h->capture).status==PGWT_MERGE_OK && h->nreleased==3 &&
           h->released[2].cookie==1 && !h->released[2].certified,
           "retained published EXIT permits distinct replacement after recovery");
    } else if(!strcmp(id,"A06")) {
        h=dsetup(false,5,false);h->capture->inventory[0].identity_installed=false;
        dabandon(h,30,DG);
        DC(!strcmp(h->output,"VOID20:unknown_evidence"),"reserved cookie alone cannot authorize abandonment");
    } else if(!strcmp(id,"A04")) {
        h=dsetup(true,5,false);dadmit(h,8);data(h,12,0,1);complete(h,13);
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,"admitted epoch fault");
        pgwt_merge_attachment proofs[2]={{.pid=10,.g=1,.identity_proved=true},
            {.pid=20,.g=1,.identity_proved=true}};
        pgwt_merge_input rearm={.kind=PGWT_MERGE_REARM,.observation={.d=DB+21,.counters={.known=true}},
            .attachments=proofs,.attachment_count=2,.attachment_proofs_fresh=true};
        DC(pgwt_capture_replay_control(h->capture,&rearm)==PGWT_MERGE_OK,"admitted-history REARM");
        h->detection=22;dabandon(h,30,DG);
        pgwt_discovery_state state;
        DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&state) && state.admitted && !state.abandoned &&
           pgwt_capture_report(h->capture).status==PGWT_MERGE_SUSPENDED,
           "admission history forbids abandonment across REARM");
    } else if(!strcmp(id,"A09")) {
        h=dsetup(false,5,false);dabandon(h,30,DG);
        h->snapshot.value[0]=1;dabandon(h,30,DG);
        DC(!strcmp(h->output,"VOID20:counter_change"),"duplicate abandonment still observes counters");
    } else if(!strcmp(id,"A19")) {
        h=dsetup(false,5,false);dabandon(h,30,DG);h->allow_shared_text=true;
        text_for_d(h);complete(h,16);
        DC(!strcmp(h->full_output,"TEXTx F15[C0]") &&
           pgwt_capture_report(h->capture).discovery_abandoned_records==0,
           "shared metadata is published and not counted as abandoned");
        pgwt_capture_destroy(h->capture);free(h);
        h=dsetup(false,5,false);
        ma_table t=table(h->capture);size_t slot=ma_binding_bucket(&t,77,NULL);
        h->capture->bindings[h->capture->binding_count++]=(ma_binding){.query_id=77,.dependency_id=1,.kind=MA_WAIT};
        h->capture->binding_index[slot]=h->capture->binding_count;
        h->query_dependency=true;data(h,9,0,1);dabandon(h,30,DG);complete(h,16);
        DC(h->nreleased==0 && h->full_length==0 && pgwt_capture_report(h->capture).discovery_abandoned_records==0,
           "abandonment cannot bypass missing metadata dependency");
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
    state_is(h,"F15",5,0,true);endpoint_verdict(h,5,false);
    DC(base_verdict(h,6,10)==PGWT_COV_COMPLETE,"complete empty census certifies ordinary windows");
    pgwt_capture_destroy(h->capture);free(h);

    h=dsetup_inventory(0,16,false,true);complete(h,16);
    state_is(h,"F15",-1,0,true);
    complete(h,17);state_is(h,"F15 F16",16,0,true);
    endpoint_verdict(h,16,false);
    pgwt_capture_destroy(h->capture);free(h);

    h=dsetup_inventory(0,5,false,true);h->reject_close=true;complete(h,16);
    state_is(h,"F15 VOID20:writer_push_fail",-1,0,true);
    DC(h->ngaps==1 && h->coverage_count==1,"rejected closure is not a published gap close");
    DC(base_verdict(h,6,10)==PGWT_COV_INCOMPLETE,"failed empty-census close cannot certify a window");
    DC(pgwt_capture_report(h->capture).status==PGWT_MERGE_SUSPENDED,"failed gap publication preserves writer fault");
    pgwt_capture_destroy(h->capture);free(h);
    puts("empty census: PASS (unknown versus complete, strict fence, publication failure, reader windows)");
}
/* Decision 41: even a successfully output anchor commits no lower bound
 * when a later publication in the same release attempt is rejected. */
static void lower_bound_failure(const char *id)
{
    struct dh *h=dsetup(false,5,false);dadmit(h,8);
    data(h,12,0,1);
    bool frontier=!strcmp(id,"bound_rejected_frontier");
    if(frontier) h->reject_frontier=true;
    else {data(h,15,0,1);h->reject_data_number=2;}
    complete(h,16);
    DC(!strcmp(h->full_output,"D12[U] VOID20[Cw,writer_push_fail]"),
       "failed release complete output");
    pgwt_discovery_state s={0};
    DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&s) && !s.has_lower_bound,
       "VOIDed release commits no fresh lower bound");
    state_is(h,"D12U VOID20:writer_push_fail",-1,1,true);
    DC(h->ngaps==1 && base_verdict(h,12,16)==PGWT_COV_INCOMPLETE,
       "VOIDed release leaves reader startup gap open");
    pgwt_capture_destroy(h->capture);free(h);
}
/* Each case specifies the entire output and final reader gap state. */
static void discovery_guard_case(const char *id)
{
    if(!strcmp(id,"G_endpoint_overflow")) {
        uint64_t endpoint=77;
        DC(!discovery_endpoint(UINT64_MAX,true,&endpoint) && endpoint==77,
           "overflow supplies no persisted endpoint");
        DC(discovery_endpoint(UINT64_MAX,false,&endpoint) && endpoint==UINT64_MAX &&
           discovery_endpoint(UINT64_MAX-1,true,&endpoint) && endpoint==UINT64_MAX,
           "largest representable endpoints remain valid");
        return;
    }
    struct dh *h=dsetup(false,5,false);
    const char *expected="VOID20[C0,unknown_evidence]";
    uint64_t abandoned=0;
    int corrupt_drop_result=0;
    if(!strcmp(id,"G_after_terminal_arrival")) {
        dabandon(h,30,DG);terminal_at(h,18,false);data(h,19,0,1);
    } else if(!strcmp(id,"G_local_writer_once")) {
        h->unaccounted_rejection=true;h->reject_frontier=true;complete(h,17);
        h->reject_frontier=false;
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);terminal_at(h,18,true);
        DC(h->capture->local_writer_fail==1 && h->capture->observation.counters.value[14]==1 &&
           pgwt_capture_snapshot(h->capture)->value[14]==0,
           "unresolved disposition counts local writer failure once");
        expected="VOID20[Cw,writer_push_fail]";
    } else if(!strcmp(id,"G_untimed_text_no_discharge")) {
        dabandon(h,30,DG);terminal_at(h,18,false);
        unsigned char bytes[272]={0};
        pgwt_merge_input text={.kind=PGWT_MERGE_TEXT,
            .observation={.d=DB+20,.counters={.known=true}},
            .record={.source=3,.o=++h->pos[3],.pid=30,.g=DG,.identity_known=true,
                .payload=bytes,.payload_size=sizeof(bytes),.dependency_id=77,
                .rank=PGWT_MERGE_EXIT,.t=DB+18,.discovery={.exclusive_text=true}}};
        DC(pgwt_capture_replay_control(h->capture,&text)==PGWT_MERGE_OK,"untimed metadata delivery");
        complete(h,19);expected="F18[C0]";abandoned=1;
        DC(base_verdict(h,19,20)==PGWT_COV_INCOMPLETE,
           "untimed TEXT never closes startup reader coverage");
    } else if(!strcmp(id,"G_missing_disposition_after_fault")) {
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,
           "suspension before final disposition");
        DC(pgwt_capture_finish(h->capture,DB+50,&h->snapshot)==0,"suspended capture finish");
        expected="VOID20[C0,allocation_fail] VOID50[C0,unknown_evidence]";
    } else if(!strcmp(id,"G_missing_disposition_during_suspension")) {
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,
           "suspension before EXIT receipt");
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);
        DC(pgwt_capture_finish(h->capture,DB+50,&h->snapshot)==0,"suspended raw EXIT finish");
        expected="VOID20[C0,allocation_fail] VOID50[C0,unknown_evidence]";
    } else if(!strcmp(id,"G_unresolved_after_fault") ||
              !strcmp(id,"G_unresolved_received_suspended") ||
              !strcmp(id,"G_unresolved_reused_token") ||
              !strcmp(id,"G_unresolved_reused_position") ||
              !strcmp(id,"G_unresolved_bad_drop_count")) {
        bool received_suspended=!strcmp(id,"G_unresolved_received_suspended");
        bool same_source=!strcmp(id,"G_unresolved_reused_position");
        bool reused=same_source || !strcmp(id,"G_unresolved_reused_token");
        if(!received_suspended) record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,
           "unresolved disposition after suspension");
        if(received_suspended) record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);
        if(reused) {
            rearm_terminal(h);
            record(h,20,same_source?30:10,1,same_source?3:0,
                   same_source?PGWT_MERGE_FORK:PGWT_MERGE_TRANS,0,false,true);
        }
        bool corrupt=!strcmp(id,"G_unresolved_bad_drop_count");
        if(corrupt) {
            h->capture->dropped_records=0;
            pgwt_discovery_attempt a={.kind=PGWT_DISC_TERMINAL_UNRESOLVED,
                .pid=30,.cookie=DG,.terminal_time=DB+18};
            corrupt_drop_result=pgwt_capture_discovery_control(h->capture,&a,DB+20,&h->snapshot);
        } else terminal_at(h,18,true);
        if(reused) {
            DC(h->capture->arrival_count==1,"unresolved EXIT preserves reused arrival token");
            complete(h,21);
        }
        if(!corrupt) {
            DC(h->capture->uncovered_records==1 && h->capture->dropped_records==0 &&
               h->capture->arrival_count==0,"unresolved EXIT counted once after fault");
            DC(pgwt_capture_finish(h->capture,DB+50,&h->snapshot)==0,"disposed suspended capture finish");
        }
        expected=corrupt?"VOID20[C0,allocation_fail] VOID20[C0,queue_ownership]":
            same_source?"VOID20[C0,allocation_fail] FORK_R20 F20[C0]":
            reused?"VOID20[C0,allocation_fail] A20[T] F20[C0]":
            "VOID20[C0,allocation_fail]";
    } else if(!strcmp(id,"G_discharge_frontier_checkpoint")) {
        voided_terminal_row(h,id);
        expected="EXIT_D18 F18[C0] VOID20[Ce,counter_change] F19[Ce]";
    } else if(!strcmp(id,"G_consumed_past_terminal")) {
        dabandon(h,30,DG);data(h,12,0,1);
        record(h,10,30,DG,3,PGWT_MERGE_EXIT,0,false,false);
        complete(h,13);terminal_at(h,10,false);complete(h,14);
        expected="EXIT_D10 F12[C0] VOID20[C0,unknown_evidence]";abandoned=1;
    } else if(!strcmp(id,"G_confirm_staged_attempt")) {
        dadmit(h,13);data(h,20,0,2);dquiet(h,12,13);
    } else if(!strcmp(id,"G_batch_staged_attempt")) {
        dadmit(h,8);data(h,12,0,1);data(h,20,1,2);complete(h,16);
    } else if(!strcmp(id,"G_exit_time_conflict")) {
        dabandon(h,30,DG);record(h,17,30,DG,3,PGWT_MERGE_EXIT,0,false,false);
        complete(h,19);terminal_at(h,18,false);
        expected="EXIT_D17 F18[C0] VOID20[C0,unknown_evidence]";
    } else if(!strcmp(id,"G_abandoned_fork") || !strcmp(id,"G_abandoned_init")) {
        dabandon(h,30,DG);
        record(h,10,30,DG,3,!strcmp(id,"G_abandoned_fork")?PGWT_MERGE_FORK:PGWT_MERGE_INIT,0,false,false);
        complete(h,16);
    } else if(!strcmp(id,"G_terminal_control_conflict")) {
        DC(pgwt_capture_fault(h->capture,PGWT_MERGE_ALLOCATION_FAIL,DB+20,&h->snapshot)==0,
           "terminal conflict during suspension");
        terminal_at(h,18,false);terminal_at(h,19,false);
        expected="VOID20[C0,allocation_fail] VOID20[C0,unknown_evidence]";
    } else if(!strcmp(id,"G_ambiguous_terminal_owner")) {
        terminal_at(h,18,false);
        dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_TERMINAL,.pid=30,.cookie=DG+1,
            .terminal_time=DB+18,.terminal_binding_proved=true});
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);
    } else if(!strcmp(id,"G_cookie_less_time_match")) {
        dabandon(h,30,DG);terminal_at(h,18,false);
        record(h,17,30,0,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        DC(h->full_length==0,"cookie-less EXIT must match terminal time before binding");
        DC(pgwt_capture_finish(h->capture,DB+50,&h->snapshot)==0,"held mismatching EXIT finish");
        expected="VOID50[C0,unknown_evidence]";
    } else if(!strcmp(id,"G_exclusive_text_binding")) {
        dabandon(h,30,DG);
        ma_table t=table(h->capture);size_t slot=ma_lifetime_bucket(&t,30,DG,NULL);
        h->capture->lifetimes[h->capture->lifetime_index[slot]-1].identity_bound=false;
        text_for_d(h);
    } else if(!strcmp(id,"G_unresolved_duplicate")) {
        terminal_at(h,18,true);terminal_at(h,18,true);
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        terminal_at(h,18,true);expected="F18[C0]";
        DC(h->capture->uncovered_records==1,"duplicate unresolved disposition never recounts EXIT");
    } else if(!strcmp(id,"G_matched_then_unresolved")) {
        terminal_at(h,18,false);terminal_at(h,18,true);
    } else if(!strcmp(id,"G_matched_duplicate")) {
        terminal_at(h,18,false);terminal_at(h,18,false);
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);terminal_at(h,18,false);
        expected="EXIT_D18 F18[C0]";
    } else DC(false,"named guard implemented");
    if(strcmp(h->full_output,expected)) fprintf(stderr,"FULL GOT %s; EXPECTED %s\n",h->full_output,expected);
    DC(!strcmp(h->full_output,expected),"guard complete expected output");
    if(!strcmp(id,"G_unresolved_bad_drop_count"))
        DC(corrupt_drop_result<0 && pgwt_capture_report(h->capture).status==PGWT_MERGE_EXIT_REQUIRED,
           "inconsistent dropped bookkeeping requires exit");
    bool matched=!strcmp(id,"G_matched_duplicate");
    bool ambiguous=!strcmp(id,"G_ambiguous_terminal_owner");
    pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
    DC(r.closed==matched && r.unresolved==(matched?0:ambiguous?2:1) && r.census_complete==(!ambiguous && strcmp(id,"G_discharge_frontier_checkpoint")) &&
       h->capture->inventory_count==(ambiguous?2:1),
       "guard inventory and gap state");
    DC(h->ngaps==(matched?2:1) && base_verdict(h,19,20)==(matched?PGWT_COV_COMPLETE:PGWT_COV_INCOMPLETE),
       "guard complete persisted reader output");
    DC(pgwt_capture_report(h->capture).discovery_abandoned_records==abandoned,
       "guard complete abandonment count");
    pgwt_capture_destroy(h->capture);free(h);
}
static pgwt_discovery_attempt nw_attempt(uint32_t role)
{
    return (pgwt_discovery_attempt){.kind=PGWT_DISC_TRY_NO_WAIT_WORD,.pid=30,.cookie=DG,
        .route=PGWT_DISC_NO_WAIT_WORD,.e=DB+6,.attachment_generation=1,
        .no_word={.enable=DB+6,.read_at=DB+7,.fence=DB+7,.handle0=1,.handle1=1,
            .role=role,.permanent_role_qualified=true,.visible_before_enable=true,
            .continuous=true,.read_ok=true,.completed=true,.counters_unchanged=true,
            .ownership=true,.binding=true}};
}
static void nw_deliver(struct dh *h, pgwt_discovery_attempt a, bool force_admit)
{
    dcontrol(h,a);
    if(no_word_valid(&a.no_word) || force_admit) {
        a.kind=PGWT_DISC_ADMIT;
        a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};
        dcontrol(h,a);
        a.kind=PGWT_DISC_CONFIRM_NO_WAIT_WORD;dcontrol(h,a);
    }
}
static void pointer(struct dh *h,uint64_t t,uint64_t value)
{
    dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_POINTER,.pid=30,.cookie=DG,
        .pointer_time=DB+t,.pointer_value=value,.pointer_read_ok=true,.pointer_continuous=true});
}
static void nrow(struct dh *h,const char *id)
{
    pgwt_discovery_attempt a=nw_attempt(!strcmp(id,"N10")?12:13);
    if(!strcmp(id,"N2") || !strcmp(id,"N7") || !strcmp(id,"N8") || !strcmp(id,"N16")) {
        a.no_word.role=1;a.no_word.permanent_role_qualified=false;
        if(strcmp(id,"N2")) {a.e=a.no_word.enable=DB+4;a.no_word.read_at=a.no_word.fence=DB+5;}
        if(!strcmp(id,"N16")) a.authentication_deadline=DB+14;
    }
    if(!strcmp(id,"N3")) a.no_word.read_at=DB+5;
    if(!strcmp(id,"N4")) {a.no_word.fires=1;a.no_word.read_at=DB+8;a.no_word.fence=DB+9;}
    if(!strcmp(id,"N9")) {a.no_word.counters_unchanged=false;h->snapshot.value[10]=1;}
    if(!strcmp(id,"N11")) {a.no_word.role=11;a.no_word.permanent_role_qualified=false;}
    if(!strcmp(id,"N12")) a.no_word.continuous=false;
    if(!strcmp(id,"N15")) {a.no_word.read_ok=false;h->snapshot.value[3]=1;}
    if(!strcmp(id,"N14")) h->reject_support=true;
    if(!strcmp(id,"N15")) DC(!no_word_valid(&a.no_word),"failed required pointer read is not a NULL candidate");
    nw_deliver(h,a,!strcmp(id,"N12"));
    if(!strcmp(id,"N7")) {
        pointer(h,!strcmp(id,"N7")?9:12,0x2000);
        pgwt_discovery_state before={0};(void)pgwt_merge_discovery_state(h->capture->core,30,DG,&before);
        DC(!before.has_lower_bound,"initialization is not a lower bound");
        dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_WAIT_ATTEMPT,.pid=30,.cookie=DG});
        dadmit(h,12);dquiet(h,11,12);complete(h,13);
    } else if(!strcmp(id,"N8")) {
        terminal_at(h,9,false);record(h,9,30,DG,3,PGWT_MERGE_EXIT,0,false,false);complete(h,10);
    } else if(!strcmp(id,"N16")) {
        dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_WAIT_LIMIT,.pid=30,.cookie=DG});complete(h,16);
    } else if(!strcmp(id,"N13")) {
        dcontrol(h,admit(40,DG+1,8,1));record(h,10,40,DG+1,0,PGWT_MERGE_TRANS,1,false,true);complete(h,11);
    } else {
        if(!strcmp(id,"N1") || !strcmp(id,"N5") || !strcmp(id,"N6") || !strcmp(id,"N10")) {
            complete(h,6);
            pgwt_discovery_state state={0};(void)pgwt_merge_discovery_state(h->capture->core,30,DG,&state);
            DC(!state.has_lower_bound && !pgwt_capture_discovery_status(h->capture).closed,
               "no-word confirmation waits beyond enable");
        }
        complete(h,!strcmp(id,"N4")?10:8);
        if(!strcmp(id,"N5")) {pointer(h,!strcmp(id,"N7")?9:12,0x2000);complete(h,14);}
        if(!strcmp(id,"N6")) {
            pointer(h,12,0);terminal_at(h,12,false);record(h,12,30,DG,3,PGWT_MERGE_EXIT,0,false,false);complete(h,13);
        }
    }
    pgwt_discovery_state state={0};(void)pgwt_merge_discovery_state(h->capture->core,30,DG,&state);
    DC(state.wait_word_attempts==(!strcmp(id,"N7")?1:0),"NULL consumes no wait-word attempts");
    if(!strcmp(id,"N5")) DC(state.pointer_nonnull,"non-NULL evidence survives its own fault");
    if(!strcmp(id,"N16")) DC(state.waiting_limit_exceeded,"authentication waiting limit exceeded");
    if(!strcmp(id,"N2") || !strcmp(id,"N11")) DC(state.pointer_watched && !state.admitted,
        "unqualified NULL remains pending with its pointer watch");
    bool no_word=!strcmp(id,"N1")||!strcmp(id,"N5")||!strcmp(id,"N6")||!strcmp(id,"N10")||
        !strcmp(id,"N13")||!strcmp(id,"N14");
    DC(state.no_wait_word==no_word,"qualified no-word route retained");
    if(no_word) {
        DC(!state.confirmed && !state.baseline.cpu_base_valid && !state.baseline.wait_word,
           "no-word route creates no numerical or CPU seed");
        DC(state.no_word_witness.role==(!strcmp(id,"N10")?12:13),"present not measured role retained");
        pgwt_capture_discovery_measurement measurement;
        DC(pgwt_capture_discovery_measurement_state(h->capture,30,DG,&measurement) &&
           measurement.present && measurement.role==state.no_word_witness.role &&
           !measurement.numerical_available && !measurement.measured_population_member,
           "individual quantities unavailable and measured population excluded");
        DC(h->supporting==(!strcmp(id,"N14")?0:1),"no-word supporting publication before commitment");
        for(size_t i=0;i<h->nreleased;++i) DC(h->released[i].pid!=30 || h->released[i].rank==PGWT_MERGE_EXIT,
            "no-word lifetime supplies no numerical records");
        DC(pgwt_capture_a2_observed_births(h->capture,DB,DB+100)==0,"no-word population contribution unavailable");
        pgwt_a2_omission omission={.pid=30,.cookie=DG,.fork_ns=DB-100,.exit_ns=DB+12,.receive_ns=DB+20};
        DC(pgwt_capture_a2_omission_population(h->capture,&omission)==PGWT_A2_UNTRACED_INVALID,
           "no-word lifetime contributes no A2 omission");
    }
}
static pgwt_discovery_attempt journal(void)
{
    return (pgwt_discovery_attempt){.kind=PGWT_DISC_JOURNAL,.pid=50,.cookie=DG,
        .terminal_time=DB+4,.names_exit=true,.exit_source=3,.exit_position=1,
        .resolution={.F0=DB,.start=DB-100,.fence=DB+5,.failed_open=DB+3,.gone_at=DB+4,
            .handle=1,.tid=50,.tgid=50,.parent=1,.postmaster=1,.exit_time=DB+4,
            .start_known=true,.clocks_qualified=true,.namespaces=true,.history_complete=true,
            .producers_complete=true,.counters_unchanged=true,.journal_complete=true,
            .journal_eligible=true,.binding=true,.gone_esrch=true,.claims_match=true}};
}
static void jrow(struct dh *h,const char *id)
{
    pgwt_discovery_attempt a=journal();
    if(!strcmp(id,"J2")||!strcmp(id,"J3")||!strcmp(id,"J4")||!strcmp(id,"J5")||
       !strcmp(id,"J20")||!strcmp(id,"J21")) {
        a.kind=PGWT_DISC_GONE;a.resolution.journal_eligible=false;
        if(!strcmp(id,"J3")||!strcmp(id,"J4")) a.resolution.gone_esrch=false;
        if(!strcmp(id,"J5")) {h->snapshot.value[10]=1;a.resolution.counters_unchanged=false;}
        if(!strcmp(id,"J21")) a.resolution.gone_at=DB+2;
        dcontrol(h,a);complete(h,8);
        if(!strcmp(id,"J20")) {
            DC(base_verdict(h,5,8)==PGWT_COV_COMPLETE,"G initially permits reader coverage");
            unsigned char bytes[48]={0};tb_le(bytes+8,50,4);
            ma_envelope e={.version=1,.kind=MA_TRACE,.bytes=bytes,.size=sizeof(bytes)};
            DC(pgwt_capture_received(h->capture,&e,DB+40)==1,"late unlisted target observation");
        }
    } else if(!strcmp(id,"J6")) {
        a.resolution.producers_complete=false;dcontrol(h,a);
        for(size_t src=0;src<3;++src) {
            ma_envelope b={.version=1,.kind=MA_BOUNDARY,.source=src,.position=++h->pos[src],
                .w=DB+8,.boundary=src<2?MA_IDLE_PERF:MA_RING_BOUNDARY};
            DC(!pgwt_capture_envelope(h->capture,&b,DB+20,&h->snapshot),"incomplete L remains unbounded");
        }
    } else if(!strcmp(id,"J7")||!strcmp(id,"J8")||!strcmp(id,"J9")||!strcmp(id,"J10")||!strcmp(id,"J11")) {
        a.kind=PGWT_DISC_EXIT_OBSERVATION;
        if(!strcmp(id,"J7")) a.resolution.start=DB;
        if(!strcmp(id,"J8")) {a.resolution.tgid=60;a.resolution.discovery_map_cookie=DG;}
        if(!strcmp(id,"J9")||!strcmp(id,"J10")) {a.resolution.parent=2;a.resolution.start=DB+2;}
        if(!strcmp(id,"J9")) a.resolution.discovery_map_tombstone=true;
        if(!strcmp(id,"J10")) a.resolution.discovery_map_cookie=DG;
        if(!strcmp(id,"J11")) {a.resolution.start=0;a.resolution.start_known=false;}
        /* Actual adapter classification, with a producer counter checkpoint.
         * The consumer must refuse nonmembers/unknowns even if the map has an
         * identity or tombstone. No synthetic EXIT is manufactured here. */
        if(ma_discovery_exit(&a.resolution)==PGWT_DISC_EXIT_MISSING_FORK) h->snapshot.value[0]=1;
        if(ma_discovery_exit(&a.resolution)==PGWT_DISC_EXIT_PREEXISTING) {
            record(h,4,50,0,3,PGWT_MERGE_EXIT,0,false,false);
        }
        DC(!h->ninput,"three-fact exit filter emits no EXIT");
        dcontrol(h,a);complete(h,8);
    } else if(!strcmp(id,"J12")||!strcmp(id,"J13")||!strcmp(id,"J14")||!strcmp(id,"J22")) {
        a.kind=PGWT_DISC_REBIND;a.resolution.journal_eligible=false;
        if(!strcmp(id,"J12")||!strcmp(id,"J22")) {
            a.cookie=1;a.resolution.fork_cookie=1;
            a.resolution.start=!strcmp(id,"J22")?DB-100:DB+2;
        }
        if(!strcmp(id,"J14")) a.resolution.parent=2;
        a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};
        if(!strcmp(id,"J12")||!strcmp(id,"J22"))
            DC(ma_discovery_exit(&a.resolution)==PGWT_DISC_EXIT_FORK,"observed fork provenance wins before age comparison");
        dcontrol(h,a);
        if(!strcmp(id,"J13")) {
            dcontrol(h,admit(50,DG,6,1));record(h,7,50,DG,0,PGWT_MERGE_TRANS,1,false,true);
        } else if(strcmp(id,"J14")) {
            record(h,!strcmp(id,"J22")?1:3,50,1,3,PGWT_MERGE_FORK,0,false,false);
            record(h,6,50,1,0,PGWT_MERGE_TRANS,0,false,true);
        }
        complete(h,8);
    } else {
        if(!strcmp(id,"J16")) {
            DC(!pgwt_capture_discovery_inventory(h->capture,50,DG),"bound U inventory");
            DC(!pgwt_capture_discovery_start_identity(h->capture,50,DG,DB-200),"retained U start identity");
            DC(!pgwt_capture_discovery_census(h->capture,DB+5,true,DB+20,&h->snapshot),"bound U census");
        }
        if(!strcmp(id,"J17")) a.resolution.exit_time=DB+5;
        dcontrol(h,a);
        if(!strcmp(id,"J17")) DC(pgwt_capture_report(h->capture).status==PGWT_MERGE_SUSPENDED &&
            h->capture->disposition_count==0,"timestamp contradiction faults before matched disposition");
        if(!strcmp(id,"J15")) {
            /* Producer-side replacement history may already exist when the
             * retained older EXIT reaches the adapter. This observation has
             * no transport position and cannot supply that EXIT's identity. */
            ma_table t=table(h->capture);size_t slot=ma_lifetime_bucket(&t,50,1,NULL);
            h->capture->lifetimes[h->capture->lifetime_count++]=(ma_lifetime){.pid=50,.g=1,.identity_bound=true,.proof=MA_PROVED,.facts_known=MA_ALL_FACTS,.facts_true=MA_ALL_FACTS};
            h->capture->lifetime_index[slot]=h->capture->lifetime_count;
        }
        h->reject_exit=!strcmp(id,"J18");
        h->counter_after_exit=!strcmp(id,"J19");h->lifecycle_counter_after_exit=h->counter_after_exit;
        record(h,!strcmp(id,"J17")?5:4,50,0,3,PGWT_MERGE_EXIT,0,false,false);
        if(!strcmp(id,"J1")) {
            complete(h,4);
            pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
            DC(r.unresolved_lifetimes==1 && !r.closed && r.census_complete,"journal waits strict EXIT release");
        }
        if(!strcmp(id,"J15")) {
            record(h,6,50,1,3,PGWT_MERGE_FORK,0,false,false);
            record(h,8,50,1,0,PGWT_MERGE_TRANS,0,false,true);
        }
        complete(h,!strcmp(id,"J15")?9:8);
    }
}
static void amended_row(const char *id)
{
    bool numeric=id[0]=='J' && strcmp(id,"J8") && strcmp(id,"J9") && strcmp(id,"J10") && strcmp(id,"J16");
    struct dh *h=dsetup_inventory(id[0]=='N'?(!strcmp(id,"N13")?2:1):0,5,false,id[0]=='N');
    if(numeric) {
        DC(!pgwt_capture_discovery_numeric(h->capture,50,DB+2,0),"unbound numeric observation");
        DC(!pgwt_capture_discovery_numeric(h->capture,50,DB+2,DB+3) && h->capture->numeric_count==1 &&
           !h->capture->inventory_count,"repeated numeric observations allocate no lifetime or cookie");
    }
    if(id[0]=='J' && strcmp(id,"J16")) DC(!pgwt_capture_discovery_census(h->capture,DB+5,true,DB+20,&h->snapshot),"proposed census fence");
    if(id[0]=='N') nrow(h,id);else jrow(h,id);
    const struct discovery_amended_expected *e=NULL;
    for(size_t i=0;i<sizeof(discovery_amended_expected)/sizeof(*discovery_amended_expected);++i)
        if(!strcmp(id,discovery_amended_expected[i].id)) e=&discovery_amended_expected[i];
    DC(e,"amended row transcription");
    if(strcmp(h->full_output,e->output)) fprintf(stderr,"FULL GOT %s; EXPECTED %s\n",h->full_output,e->output);
    DC(!strcmp(h->full_output,e->output),"complete amended output sequence");
    pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
    DC(r.unresolved_lifetimes==e->lifetimes && r.unresolved_numeric==e->numeric &&
       r.unresolved==e->lifetimes+e->numeric && r.census_complete==e->census,"separate pending and census");
    DC(r.closed==(e->close>=0),"amended closure commitment");
    DC(r.census_fence==DB+5,"amended census fence retained");
    pgwt_merge_report report=pgwt_capture_report(h->capture);
    DC(report.status==(strstr(e->output,"VOID")?PGWT_MERGE_SUSPENDED:PGWT_MERGE_OK) &&
       !report.epoch_invalid && !report.void_failed,"amended final epoch state");
    DC(h->gaps[0].kind==PGWT_CAPTURE_GAP_PREEXISTING && h->gaps[0].at==DB && h->gaps[0].detected==DB,
       "amended startup opens before coverage");
    if(e->close>=0) DC(r.closure==DB+(uint64_t)e->close,"amended semantic closure");
    bool revoked=!strcmp(id,"N5")||!strcmp(id,"J20");
    DC(h->ngaps==(revoked?3:e->persisted>=0?2:1),"complete amended gap publication sequence");
    if(e->persisted>=0 || revoked) {
        uint64_t end=e->persisted>=0?(uint64_t)e->persisted:!strcmp(id,"N5")?6:5;
        DC(h->gaps[1].kind==PGWT_CAPTURE_GAP_PREEXISTING_END && h->gaps[1].at==DB+end && h->gaps[1].detected==DB+20,
           "amended persisted endpoint");
        if(!revoked) {
            DC(base_verdict(h,end-1,end)==PGWT_COV_INCOMPLETE && base_verdict(h,end,end+1)==PGWT_COV_COMPLETE,
               "amended endpoint through reader");
        } else DC(h->gaps[2].at==DB && h->gaps[2].detected==DB+(!strcmp(id,"N5")?20:40),
                  "revocation starts at boot and retains detection time");
    }
    DC(base_verdict(h,99,100)==(e->persisted>=0?PGWT_COV_COMPLETE:PGWT_COV_INCOMPLETE),"amended final reader state");
    pgwt_discovery_state state={0};(void)pgwt_merge_discovery_state(h->capture->core,id[0]=='N'?30:50,DG,&state);
    DC(state.has_lower_bound==(e->lower>=0),"amended lower bound presence");
    if(e->lower>=0) DC(state.lower_bound==DB+(uint64_t)e->lower,"amended lower bound time");
    bool terminal=!strcmp(id,"N6")||!strcmp(id,"N8")||!strcmp(id,"J1")||!strcmp(id,"J15");
    DC(state.terminal_released==terminal,"amended committed terminal discharge");
    if(terminal) DC(state.terminal_time==DB+(!strcmp(id,"N6")?12:!strcmp(id,"N8")?9:4),"amended terminal time");
    if(!strcmp(id,"J17")) DC(h->capture->inventory_count==1 && h->capture->numeric[0].resolved,
        "timestamp contradiction retains journal identity and numeric resolution");
    if(!strcmp(id,"J12")||!strcmp(id,"J15")||!strcmp(id,"J22")) {
        pgwt_a2_lifetime birth;
        DC(pgwt_capture_a2_lifetime(h->capture,50,1,&birth) && birth.fork_observed && !birth.discovery,
           "replacement retains fork provenance");
        DC(pgwt_capture_a2_observed_births(h->capture,DB,DB+100)==1,"replacement contributes one observed birth");
        DC(h->nreleased>=2 && !h->released[h->nreleased-1].certified,"fork first numerical interval unavailable");
    } else DC(pgwt_capture_a2_observed_births(h->capture,DB,DB+100)==0,"discovery contributes no observed births");
    DC(pgwt_capture_report(h->capture).discovery_abandoned_records==0,"amended rows manufacture no abandoned records");
    pgwt_capture_destroy(h->capture);free(h);
}

static void census_counter_class(void)
{
    static const size_t components[]={0,1,2,10,3,14,11};
    static const char *labels[]={"Ce","Ci","Cx","Cf","Cr","Cw","Cb"};
    for(size_t i=0;i<sizeof(components)/sizeof(*components);++i) {
        struct dh *h=dsetup(false,5,false);dadmit(h,8);data(h,12,0,1);complete(h,13);
        h->snapshot.value[components[i]]=1;
        ma_envelope check={.version=1,.kind=MA_CHECK};
        DC(!pgwt_capture_envelope(h->capture,&check,DB+20,&h->snapshot),"counter class checkpoint");
        char expected[128];snprintf(expected,sizeof(expected),"D12[U] F12[C0] VOID20[%s,counter_change]",labels[i]);
        DC(!strcmp(h->full_output,expected),"counter class complete expected output");
        pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
        DC(r.census_complete==(i>=4) && !r.unresolved && r.closed,"only named lifecycle losses invalidate census");
        DC(h->ngaps==2 && h->gaps[1].at==DB+13 && base_verdict(h,13,16)==PGWT_COV_COMPLETE,
           "counter class retains established identity and startup endpoint");
        pgwt_capture_destroy(h->capture);free(h);
    }
}
static void round5_guard(const char *id)
{
    bool two=!strcmp(id,"G_unresolved_does_not_block_closure");
    bool qmode=!strcmp(id,"G_numeric_resolution_in_Q");
    bool after_late=!strcmp(id,"G_resolution_after_late_revocation");
    struct dh *h=qmode?dsetup_inventory(0,5,true,false):after_late?dsetup_inventory(0,5,false,false):dsetup(two,5,false);
    const char *out="";size_t pending=1,expected_gaps=1;bool closed=false,census=true;
    if(!strcmp(id,"G_disposition_uninventoried")) {
        dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_TERMINAL_UNRESOLVED,.pid=40,.cookie=DG+1,.terminal_time=DB+18});
        out="VOID20[C0,unknown_evidence]";
    } else if(!strcmp(id,"G_discovery_after_finish")) {
        DC(!pgwt_capture_finish(h->capture,DB+20,&h->snapshot),"finish before control");
        pgwt_discovery_attempt a={.kind=PGWT_DISC_TERMINAL_UNRESOLVED,.pid=30,.cookie=DG,.terminal_time=DB+18};
        DC(pgwt_capture_discovery_control(h->capture,&a,DB+20,&h->snapshot)==-EINVAL,
           "finished capture refuses discovery control");
        DC(!h->capture->disposition_count,"finished capture retains no new disposition");
    } else if(!strcmp(id,"G_unresolved_reuse")) {
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);terminal_at(h,18,true);
        record(h,25,30,0,3,PGWT_MERGE_EXIT,0,false,false);complete(h,26);terminal_at(h,25,true);
        out="F18[C0] F25[C0]";
        DC(h->capture->uncovered_records==2 && h->capture->disposition_count==2,
           "reused PID EXITs each consume their own disposition");
    } else if(!strcmp(id,"G_unresolved_resolved_lifetime")) {
        dadmit(h,8);data(h,12,0,1);complete(h,13);
        record(h,25,30,0,3,PGWT_MERGE_EXIT,0,false,false);terminal_at(h,25,true);complete(h,26);
        out="D12[U] F12[C0] F25[C0]";pending=0;closed=true;
        DC(h->capture->uncovered_records==1,"replacement unresolved EXIT counted once");
    } else if(!strcmp(id,"G_mapped_hold_reset")) {
        terminal_at(h,18,false);
        dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_TERMINAL,.pid=30,.cookie=DG+1,
            .terminal_time=DB+18,.terminal_binding_proved=true});
        bool hold=true;const ma_snapshot *snapshot=&h->snapshot;
        unsigned char bytes[24]={0};tb_le(bytes,3,4);tb_le(bytes+4,30,4);tb_le(bytes+16,DB+18,8);
        ma_envelope e={.version=1,.kind=MA_LIFECYCLE,.source=3,.position=1,.bytes=bytes,.size=24};
        DC(!mapped(h->capture,&e,0,DB+20,&snapshot,&hold,false) && !hold,
           "mapped clears stale caller hold");
        out="VOID20[C0,unknown_evidence]";pending=2;census=false;
    } else if(!strcmp(id,"G_unresolved_does_not_block_closure")) {
        dadmit(h,8);data(h,12,0,1);complete(h,13);
        record(h,25,30,0,3,PGWT_MERGE_EXIT,0,false,false);terminal_at(h,25,true);
        dcontrol(h,admit(40,DG+1,21,1));dcontrol(h,bracket(40,DG+1,20,21,1));complete(h,26);
        out="D12[U] F12[C0] F25[C0]";pending=0;
    } else if(!strcmp(id,"G_journal_unknown_start")) {
        pgwt_discovery_attempt a=journal();a.pid=30;a.resolution.tid=a.resolution.tgid=30;
        dcontrol(h,a);out="VOID20[C0,unknown_evidence]";
    } else if(!strcmp(id,"G_numeric_resolution_in_Q")) {
        DC(!pgwt_capture_discovery_numeric(h->capture,50,DB+2,DB+3),"Q numeric observation");
        DC(!pgwt_capture_discovery_census(h->capture,DB+5,true,DB+20,&h->snapshot),"Q proposed census fence");
        pgwt_discovery_attempt a=journal();a.kind=PGWT_DISC_GONE;a.resolution.journal_eligible=false;
        dcontrol(h,a);out="VOID20[C0,unknown_evidence]";census=false;
    } else if(after_late) {
        DC(!pgwt_capture_discovery_numeric(h->capture,50,DB+2,DB+3),"late resolution numeric observation");
        DC(!pgwt_capture_discovery_census(h->capture,DB+5,true,DB+20,&h->snapshot),"late resolution census");
        jrow(h,"J20");
        pgwt_discovery_attempt a=journal();a.kind=PGWT_DISC_GONE;a.resolution.journal_eligible=false;
        dcontrol(h,a);out="F7[C0]";census=false;expected_gaps=3;
    } else if(!strcmp(id,"G_uncovered_position")) {
        pgwt_merge_input input={.kind=PGWT_MERGE_UNCOVERED,.source=3,.p=1,
            .observation={.d=DB+20,.counters={.known=true}}};
        DC(pgwt_capture_replay_control(h->capture,&input)==PGWT_MERGE_OK,"uncovered position consumed");
        input.kind=PGWT_MERGE_BOUNDARY;input.w=DB+19;
        (void)pgwt_capture_replay_control(h->capture,&input);
        out="VOID20[C0,duplicate_identity]";
    } else if(!strcmp(id,"G_endpoint_call_overflow")) {
        /* The strict frontier cannot naturally exceed UINT64_MAX. Inject the
         * largest representable retained frontier/anchor to exercise this
         * defensive call site, independently of the helper's direct test. */
        pgwt_merge_record key={.pid=30,.g=DG};
        struct generation_state *g=remember_generation(h->capture->core,&key);
        DC(g,"overflow retained generation");
        g->discovery.has_lower_bound=g->discovery.has_uncertified=true;
        g->discovery.lower_bound=g->discovery.uncertified_through=UINT64_MAX;
        h->capture->core->report.has_frontier=true;h->capture->core->report.frontier=UINT64_MAX;
        DC(!discovery_progress(h->capture),"overflow close attempt handled");pending=0;
    } else DC(false,"round five guard named");
    if(strcmp(h->full_output,out)) fprintf(stderr,"FULL GOT %s; EXPECTED %s\n",h->full_output,out);
    DC(!strcmp(h->full_output,out),"round five guard complete output");
    pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
    DC(r.unresolved==pending && r.closed==closed && r.census_complete==census,"round five guard inventory state");
    DC(h->ngaps==(closed?2:expected_gaps) && base_verdict(h,99,100)==(closed?PGWT_COV_COMPLETE:PGWT_COV_INCOMPLETE),
       "round five guard complete reader output");
    pgwt_capture_destroy(h->capture);free(h);
}

/* Round 6 expectations are literal contract consequences, including every
 * published DATA/FRONTIER/VOID and the complete persisted startup interval.
 * Negative endpoints mean the gap remains open / no bound was committed. */
struct round6_expected {
    const char *id, *output;
    size_t lifetimes, numeric;
    bool census;
    int close, endpoint, lower, terminal;
    size_t gaps, support, uncovered;
};
static const struct round6_expected round6_cases[]={
    {"G_pointer_before_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_NULL_numerical_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_NULL_quiet_confirm_first", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_pointer_before_try", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_pointer_break_before_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_try_cannot_erase_watch", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_quiet_before_no_word_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_nw_fence_before_read", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_nw_handles_differ", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_nw_no_binding", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_nw_no_ownership", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_nw_confirm_role", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_nw_enable_mismatch", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_pointer_break_after_bound", "F7[C0] VOID20[C0,unknown_evidence]",1,0,false,-1,-1,-1,-1,3,1,0},
    {"G_nw_TRANS", "F7[C0] VOID20[C0,unknown_evidence]",0,0,true,6,6,6,-1,2,1,0},
    {"G_nw_TRANS_before_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_quiet_after_no_word", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_try_after_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_wait_after_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_wait_limit_before_deadline", "F7[C0]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_resolution_delta_J", "VOID20[Cr,counter_change]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_resolution_delta_R", "VOID20[Cr,counter_change]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_resolution_delta_G", "VOID20[Cr,counter_change]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_gone_fence_before_gone", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_gone_journal_precedence", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_rebind_journal_precedence", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_rebind_no_binding", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_rebind_no_handle", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_journal_unnamed", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_journal_no_position", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_journal_wrong_tgid", "VOID20[C0,unknown_evidence]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_numeric_EXIT_before_J", "EXIT_U4 F7[C0]",0,0,true,5,5,-1,4,2,0,0},
    {"G_resolution_F0_after_boot", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_resolution_fence_before_census", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_discovery_cookie_not_fork", "F7[C0]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_unqualified_clocks", "F7[C0]",0,1,false,-1,-1,-1,-1,1,0,0},
    {"G_late_inventory_census", "",2,0,false,-1,-1,-1,-1,1,0,0},
    {"G_named_exit_wrong_source", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_named_exit_wrong_time", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_terminal_before_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_admit_before_terminal", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_terminal_before_quiet", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_quiet_before_terminal", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_terminal_before_nw_admit", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_nw_admit_before_terminal", "VOID20[C0,unknown_evidence]",1,0,true,-1,-1,-1,-1,1,0,0},
    {"G_PID_only_binding", "D12[U] F12[C0]",0,0,true,12,13,12,-1,2,0,0},
    {"G_unresolved_then_bound", "F18[C0] D22[U] F23[C0]",0,0,true,-1,-1,22,-1,1,0,1},
    {"G_unresolved_before_EXIT_then_bound", "F18[C0] D22[U] F23[C0]",0,0,true,-1,-1,22,-1,1,0,1},
};
static void round6_case(const struct round6_expected *e)
{
    const char *id=e->id;
    bool numeric=!strncmp(id,"G_resolution_",13) || !strncmp(id,"G_gone_",7) ||
        !strncmp(id,"G_rebind_",9) || !strncmp(id,"G_journal_",10) ||
        !strcmp(id,"G_numeric_EXIT_before_J") || !strcmp(id,"G_discovery_cookie_not_fork") ||
        !strcmp(id,"G_unqualified_clocks");
    bool pid_only=!strcmp(id,"G_PID_only_binding");
    struct dh *h=pid_only?dsetup_start(0,5,false,false,true):dsetup_inventory(numeric?0:1,5,false,!numeric);
    if(numeric) {
        DC(!pgwt_capture_discovery_numeric(h->capture,50,DB+2,DB+3),"numeric census observation");
        DC(!pgwt_capture_discovery_census(h->capture,DB+5,true,DB+20,&h->snapshot),"proposed census");
    }
    pgwt_discovery_attempt a=nw_attempt(13);
    if(!strcmp(id,"G_pointer_before_admit") || !strcmp(id,"G_pointer_before_try")) {
        if(!strcmp(id,"G_pointer_before_try")) pointer(h,9,0x2000);
        dcontrol(h,a);
        if(!strcmp(id,"G_pointer_before_admit")) pointer(h,9,0x2000);
        a.kind=PGWT_DISC_ADMIT;a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};
        dcontrol(h,a);
        DC(!strcmp(h->full_output,"VOID20[C0,unknown_evidence]"),"round six complete output");
        a.kind=PGWT_DISC_CONFIRM_NO_WAIT_WORD;dcontrol(h,a);complete(h,10);
    } else if(!strcmp(id,"G_NULL_numerical_admit") || !strcmp(id,"G_NULL_quiet_confirm_first")) {
        dcontrol(h,a);
        if(!strcmp(id,"G_NULL_quiet_confirm_first")) dquiet(h,11,12);
        else {
            dadmit(h,12);
            DC(!strcmp(h->full_output,"VOID20[C0,unknown_evidence]"),"round six complete output");
            dquiet(h,11,12);complete(h,13);
        }
    } else if(!strcmp(id,"G_pointer_break_before_admit")) {
        dcontrol(h,a);dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_POINTER,.pid=30,.cookie=DG,.pointer_read_ok=true});
        a.kind=PGWT_DISC_ADMIT;a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};dcontrol(h,a);
    } else if(!strcmp(id,"G_try_cannot_erase_watch")) {
        dcontrol(h,a);a.no_word.continuous=false;dcontrol(h,a);dadmit(h,12);
    } else if(!strcmp(id,"G_quiet_before_no_word_admit")) {
        dquiet(h,5,6);a.kind=PGWT_DISC_ADMIT;
        a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};dcontrol(h,a);
    } else if(!strcmp(id,"G_nw_fence_before_read") || !strcmp(id,"G_nw_handles_differ") ||
              !strcmp(id,"G_nw_no_binding") || !strcmp(id,"G_nw_no_ownership")) {
        if(!strcmp(id,"G_nw_fence_before_read")) a.no_word.fence=DB+6;
        if(!strcmp(id,"G_nw_handles_differ")) a.no_word.handle1=2;
        if(!strcmp(id,"G_nw_no_binding")) a.no_word.binding=false;
        if(!strcmp(id,"G_nw_no_ownership")) a.no_word.ownership=false;
        a.kind=PGWT_DISC_ADMIT;a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};dcontrol(h,a);
    } else if(!strcmp(id,"G_nw_confirm_role") || !strcmp(id,"G_nw_enable_mismatch")) {
        a.kind=PGWT_DISC_ADMIT;a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};
        if(!strcmp(id,"G_nw_enable_mismatch")) a.e=DB+8;
        dcontrol(h,a);
        if(!strcmp(id,"G_nw_confirm_role")) {a.kind=PGWT_DISC_CONFIRM_NO_WAIT_WORD;a.no_word.role=12;dcontrol(h,a);}
    } else if(!strcmp(id,"G_pointer_break_after_bound") || !strcmp(id,"G_nw_TRANS")) {
        nw_deliver(h,a,false);complete(h,8);
        if(!strcmp(id,"G_pointer_break_after_bound")) dcontrol(h,(pgwt_discovery_attempt){.kind=PGWT_DISC_POINTER,.pid=30,.cookie=DG,.pointer_read_ok=true});
        else data(h,12,0,1);
    } else if(!strcmp(id,"G_nw_TRANS_before_admit")) {
        data(h,12,0,1);nw_deliver(h,a,false);
    } else if(!strcmp(id,"G_quiet_after_no_word")) {
        a.kind=PGWT_DISC_ADMIT;a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};dcontrol(h,a);dquiet(h,5,6);
    } else if(!strcmp(id,"G_try_after_admit") || !strcmp(id,"G_wait_after_admit")) {
        dadmit(h,8);if(!strcmp(id,"G_wait_after_admit")) a.kind=PGWT_DISC_WAIT_ATTEMPT;dcontrol(h,a);
    } else if(!strcmp(id,"G_wait_limit_before_deadline")) {
        a.authentication_deadline=DB+30;dcontrol(h,a);a.kind=PGWT_DISC_WAIT_LIMIT;dcontrol(h,a);
        complete(h,8);
        pgwt_discovery_state state={0};DC(pgwt_merge_discovery_state(h->capture->core,30,DG,&state) && !state.waiting_limit_exceeded,
            "deadline not exceeded before detection reaches it");
    } else if(numeric) {
        a=journal();
        if(!strcmp(id,"G_numeric_EXIT_before_J")) {
            record(h,4,50,0,3,PGWT_MERGE_EXIT,0,false,false);
            DC(h->capture->uncovered_records==0 && pgwt_capture_pending_records(h->capture)==1,
               "numeric EXIT retained before journal");
            complete(h,8);DC(!h->full_length,"numeric EXIT holds frontier before journal");
            dcontrol(h,a);
        } else {
            if(strstr(id,"delta_")) {
                if(strstr(id,"_R")) {a.kind=PGWT_DISC_REBIND;a.resolution.journal_eligible=false;}
                if(strstr(id,"_G")) {a.kind=PGWT_DISC_GONE;a.resolution.journal_eligible=false;}
                h->snapshot.value[3]=1;
            } else if(!strncmp(id,"G_gone_",7)) {
                a.kind=PGWT_DISC_GONE;a.resolution.journal_eligible=!strcmp(id,"G_gone_journal_precedence");
                if(!strcmp(id,"G_gone_fence_before_gone")) a.resolution.gone_at=DB+6;
            } else if(!strncmp(id,"G_rebind_",9) || !strcmp(id,"G_discovery_cookie_not_fork") || !strcmp(id,"G_unqualified_clocks")) {
                a.kind=PGWT_DISC_REBIND;a.resolution.journal_eligible=!strcmp(id,"G_rebind_journal_precedence");
                if(!strcmp(id,"G_rebind_no_binding")) a.resolution.binding=false;
                if(!strcmp(id,"G_rebind_no_handle")) a.resolution.handle=0;
                if(!strcmp(id,"G_discovery_cookie_not_fork")) a.resolution.fork_cookie=DG+8;
                if(!strcmp(id,"G_unqualified_clocks")) a.resolution.clocks_qualified=false;
            } else if(!strcmp(id,"G_journal_unnamed")) a.names_exit=false;
            else if(!strcmp(id,"G_journal_no_position")) a.exit_position=0;
            else if(!strcmp(id,"G_journal_wrong_tgid")) a.resolution.tid=a.resolution.tgid=60;
            else if(!strcmp(id,"G_resolution_F0_after_boot")) a.resolution.F0=DB+1;
            else if(!strcmp(id,"G_resolution_fence_before_census")) a.resolution.fence=DB+4;
            dcontrol(h,a);complete(h,8);
        }
    } else if(!strcmp(id,"G_late_inventory_census")) {
        DC(!pgwt_capture_discovery_inventory(h->capture,40,DG+1),"late unlisted inventory");
        /* Resolving an unrelated listed number cannot restore the declaration. */
        DC(!pgwt_capture_discovery_numeric(h->capture,50,DB+2,DB+3),"unrelated numeric observation");
        a=journal();a.kind=PGWT_DISC_GONE;a.resolution.journal_eligible=false;dcontrol(h,a);
    } else if(!strcmp(id,"G_named_exit_wrong_source")) {
        a=(pgwt_discovery_attempt){.kind=PGWT_DISC_TERMINAL,.pid=30,.cookie=DG,.terminal_time=DB+18,
            .terminal_binding_proved=true,.names_exit=true,.exit_source=2,.exit_position=1};dcontrol(h,a);
    } else if(!strcmp(id,"G_named_exit_wrong_time")) {
        a=(pgwt_discovery_attempt){.kind=PGWT_DISC_TERMINAL,.pid=30,.cookie=DG,.terminal_time=DB+18,
            .terminal_binding_proved=true,.names_exit=true,.exit_source=3,.exit_position=1};dcontrol(h,a);
        record(h,19,30,0,3,PGWT_MERGE_EXIT,0,false,false);
    } else if(!strcmp(id,"G_terminal_before_admit")) {
        terminal_at(h,18,false);dadmit(h,20);
    } else if(!strcmp(id,"G_admit_before_terminal")) {
        dadmit(h,20);terminal_at(h,18,false);
    } else if(!strcmp(id,"G_terminal_before_quiet")) {
        terminal_at(h,18,false);dquiet(h,19,20);
    } else if(!strcmp(id,"G_quiet_before_terminal")) {
        dquiet(h,19,20);terminal_at(h,18,false);
    } else if(!strcmp(id,"G_terminal_before_nw_admit") || !strcmp(id,"G_nw_admit_before_terminal")) {
        a.kind=PGWT_DISC_ADMIT;a.proof=(pgwt_discovery_facts){PGWT_DISC_ADMISSION_FACTS,PGWT_DISC_ADMISSION_FACTS};
        if(!strcmp(id,"G_terminal_before_nw_admit")) terminal_at(h,4,false);
        dcontrol(h,a);
        if(!strcmp(id,"G_nw_admit_before_terminal")) terminal_at(h,4,false);
    } else if(pid_only) {
        pgwt_capture_discovery_report pending=pgwt_capture_discovery_status(h->capture);
        DC(pending.unresolved_numeric==1 && !pending.unresolved_lifetimes && !pending.census_complete,"PID-only is numeric until binding");
        DC(!pgwt_capture_discovery_inventory(h->capture,30,DG),"bind PID-only observation");
        DC(!pgwt_capture_discovery_census(h->capture,DB+5,true,DB+20,&h->snapshot),"bound census");
        dadmit(h,8);data(h,12,0,1);complete(h,13);
    } else if(!strcmp(id,"G_unresolved_then_bound") || !strcmp(id,"G_unresolved_before_EXIT_then_bound")) {
        if(!strcmp(id,"G_unresolved_before_EXIT_then_bound")) terminal_at(h,18,true);
        record(h,18,30,0,3,PGWT_MERGE_EXIT,0,false,false);complete(h,19);
        if(!strcmp(id,"G_unresolved_then_bound")) terminal_at(h,18,true);
        dadmit(h,20);data(h,22,0,1);complete(h,24);
    } else DC(false,"round six case implemented");
    if(!strcmp(id,"G_nw_TRANS") || !strcmp(id,"G_nw_TRANS_before_admit")) complete(h,14);
    if(strcmp(h->full_output,e->output)) fprintf(stderr,"FULL GOT %s; EXPECTED %s\n",h->full_output,e->output);
    DC(!strcmp(h->full_output,e->output),"round six complete output");
    pgwt_capture_discovery_report r=pgwt_capture_discovery_status(h->capture);
    DC(r.unresolved_lifetimes==e->lifetimes && r.unresolved_numeric==e->numeric &&
       r.unresolved==e->lifetimes+e->numeric && r.census_complete==e->census && r.closed==(e->close>=0),
       "round six complete census and pending");
    if(e->close>=0) DC(r.closure==DB+(uint64_t)e->close,"round six closure time");
    DC(h->ngaps==e->gaps && h->gaps[0].kind==PGWT_CAPTURE_GAP_PREEXISTING && h->gaps[0].at==DB,
       "round six complete gap publication");
    if(e->gaps>=2) DC(h->gaps[1].kind==PGWT_CAPTURE_GAP_PREEXISTING_END &&
        h->gaps[1].at==DB+(uint64_t)(e->endpoint<0?6:e->endpoint),"round six persisted endpoint");
    if(e->gaps==3) DC(h->gaps[2].kind==PGWT_CAPTURE_GAP_PREEXISTING && h->gaps[2].at==DB &&
        h->gaps[2].detected==DB+20 && h->gaps[2].revoked,"round six boot revocation");
    DC(base_verdict(h,99,100)==(e->endpoint>=0?PGWT_COV_COMPLETE:PGWT_COV_INCOMPLETE),"round six reader output");
    if(e->endpoint>=0) DC(base_verdict(h,e->endpoint-1,e->endpoint)==PGWT_COV_INCOMPLETE &&
        base_verdict(h,e->endpoint,e->endpoint+1)==PGWT_COV_COMPLETE,"round six reader endpoint");
    pgwt_discovery_state state={0};(void)pgwt_merge_discovery_state(h->capture->core,numeric?50:30,DG,&state);
    DC(state.has_lower_bound==(e->lower>=0) && (!state.has_lower_bound || state.lower_bound==DB+(uint64_t)e->lower) &&
       state.terminal_released==(e->terminal>=0) && (!state.terminal_released || state.terminal_time==DB+(uint64_t)e->terminal),
       "round six committed lower and terminal bounds");
    DC(h->supporting==e->support && h->capture->uncovered_records==e->uncovered &&
       !pgwt_capture_report(h->capture).discovery_abandoned_records,"round six complete accounting");
    pgwt_capture_destroy(h->capture);free(h);
}

int main(int argc,char **argv)
{
    size_t ran=0;
    for(size_t i=0;i<sizeof(oracle_rows)/sizeof(*oracle_rows);++i) {
        if(argc>1 && strcmp(argv[1],oracle_rows[i].id)) continue;
        discovery_case=oracle_rows[i].id;printf("REACHED discovery %s\n",discovery_case);fflush(stdout);
        if(discovery_case[0]=='N' || discovery_case[0]=='J') amended_row(discovery_case);
        else {run_row(discovery_case);adversarial_guards(discovery_case);}printf("discovery %s: full consumer output PASS\n",discovery_case);++ran;
    }
    size_t oracle_count=ran;
    const char *cases[]={"bound_rejected_later_record","bound_rejected_frontier",
        "G_after_terminal_arrival","G_consumed_past_terminal","G_confirm_staged_attempt",
        "G_batch_staged_attempt","G_exit_time_conflict","G_abandoned_fork","G_abandoned_init",
        "G_terminal_control_conflict","G_ambiguous_terminal_owner","G_cookie_less_time_match",
        "G_exclusive_text_binding","G_endpoint_overflow","G_unresolved_duplicate",
        "G_matched_then_unresolved","G_matched_duplicate","G_discharge_frontier_checkpoint",
        "G_missing_disposition_after_fault","G_missing_disposition_during_suspension",
        "G_unresolved_after_fault","G_unresolved_received_suspended",
        "G_unresolved_reused_token","G_unresolved_reused_position","G_unresolved_bad_drop_count",
        "G_untimed_text_no_discharge","G_local_writer_once"};
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        if(argc>1 && strcmp(argv[1],cases[i])) continue;
        discovery_case=cases[i];printf("REACHED discovery %s\n",discovery_case);fflush(stdout);
        if(discovery_case[0]=='G') discovery_guard_case(discovery_case);else lower_bound_failure(discovery_case);printf("discovery %s: PASS\n",discovery_case);++ran;
    }
    const char *round5[]={"G_disposition_uninventoried","G_discovery_after_finish","G_unresolved_reuse",
        "G_unresolved_resolved_lifetime","G_unresolved_does_not_block_closure","G_mapped_hold_reset","G_census_counter_class","G_uncovered_position","G_endpoint_call_overflow","G_journal_unknown_start","G_numeric_resolution_in_Q","G_resolution_after_late_revocation"};
    for(size_t i=0;i<sizeof(round5)/sizeof(*round5);++i) {
        if(argc>1 && strcmp(argv[1],round5[i])) continue;
        discovery_case=round5[i];printf("REACHED discovery %s\n",discovery_case);fflush(stdout);
        if(!strcmp(discovery_case,"G_census_counter_class")) census_counter_class();else round5_guard(discovery_case);printf("discovery %s: PASS\n",discovery_case);++ran;
    }
    for(size_t i=0;i<sizeof(round6_cases)/sizeof(*round6_cases);++i) {
        if(argc>1 && strcmp(argv[1],round6_cases[i].id)) continue;
        discovery_case=round6_cases[i].id;printf("REACHED discovery %s\n",discovery_case);fflush(stdout);
        round6_case(&round6_cases[i]);printf("discovery %s: PASS\n",discovery_case);++ran;
    }
    DC(ran>0,"named oracle row reached");
    if(argc==1) empty_census();
    printf("discovery full consumer outputs: %zu oracle rows, %zu named cases passed (including numerical and A2 population assertions)\n",oracle_count,ran-oracle_count);
    return 0;
}
