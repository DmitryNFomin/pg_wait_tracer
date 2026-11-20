#define PGWT_TRANSPORT_EMBEDDED
#include "test_percpu_transport.c"
#undef main
#define put_le discovery_capture_put_le
#include "../src/merge_capture.c"
#undef put_le
#include "coverage_record.h"
#include "coverage.h"
#include "discovery_oracle_rows.h"

static const char *discovery_case;
#define DC(x, message) do { if (!(x)) { fprintf(stderr, "ASSERT %s: %s (line %d)\n", discovery_case, message, __LINE__); exit(1); } } while (0)
#define DB UINT64_C(1000)
#define DG PGWT_DISCOVERY_COOKIE_MIN
struct dh {
    struct pgwt_merge_capture *capture;
    ma_snapshot snapshot;
    uint64_t pos[4];
    char output[1024]; size_t length;
    struct pgwt_cov_rec coverage[32]; size_t coverage_count;
    bool reject_data, reject_close;
    size_t data_count;
};
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
        append(h,r->rank==PGWT_MERGE_TRANS?"D":r->rank==PGWT_MERGE_EXIT?"EXIT":
               r->rank==PGWT_MERGE_FORK?"FORK":"INIT",r->t-DB,
               r->rank==PGWT_MERGE_TRANS?(out->prior_interval_certified?"T":"U"):"");
    } else if(out->kind==PGWT_MERGE_OUT_FRONTIER) append(h,"F",out->F-DB,"");
    else if(out->kind==PGWT_MERGE_OUT_VOID) {
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
    struct pgwt_coverage_record r={.kind=gap->kind==PGWT_CAPTURE_GAP_PREEXISTING?
        PGWT_COVK_GAP_OPEN:PGWT_COVK_GAP_CLOSE,.a=gap->at,.c=1};
    unsigned char bytes[80];size_t n=pgwt_coverage_encode(&r,bytes);
    struct pgwt_coverage_record decoded;
    DC(pgwt_coverage_decode(bytes,n,&decoded)==PGWT_COVD_OK,"persisted gap decode");
    DC(h->coverage_count<32,"gap record bound");
    h->coverage[h->coverage_count++]=(struct pgwt_cov_rec){.kind=decoded.kind,.a=(int64_t)decoded.a,.c=(int64_t)decoded.c};
    return true;
}
static struct dh *dsetup(bool two, uint64_t fence, bool qmode)
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
        .test_attach_premise=true};
    ma_table empty={0};
    h->capture=pgwt_capture_replay_create(&cfg,&initial,&empty);DC(h->capture,"capture create");
    DC(pgwt_capture_coverage_begin(h->capture,DB,NULL,0,NULL)==0,"begin startup gap");
    DC(pgwt_capture_discovery_inventory(h->capture,30,DG)==0,"D inventory");
    if(two) DC(pgwt_capture_discovery_inventory(h->capture,40,DG+1)==0,"H inventory");
    DC(pgwt_capture_discovery_census(h->capture,DB+fence,true,DB+20,&h->snapshot)==0,"complete census");
    return h;
}
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
{ DC(pgwt_capture_discovery_control(h->capture,&a,DB+20,&h->snapshot)==0,"discovery control delivery"); }
static void dadmit(struct dh *h,uint64_t e) {dcontrol(h,admit(30,DG,e,1));}
static void dquiet(struct dh *h,uint64_t t,uint64_t e) {dcontrol(h,bracket(30,DG,t,e,1));}
static void record(struct dh *h,uint64_t t,uint64_t pid,uint64_t cookie,size_t src,
                   pgwt_merge_rank rank,uint64_t gen,bool marker,bool refresh)
{
    unsigned char bytes[48]={0};size_t size=rank==PGWT_MERGE_TRANS?48:24;
    if(rank==PGWT_MERGE_TRANS) {
        tb_le(bytes,DB+t,8);tb_le(bytes+8,pid,4);tb_le(bytes+12,0x6000000,4);
        tb_le(bytes+24,3,8);tb_le(bytes+40,1,8);
    } else {
        tb_le(bytes,rank==PGWT_MERGE_FORK?1:rank==PGWT_MERGE_INIT?2:3,4);
        tb_le(bytes+4,pid,4);tb_le(bytes+16,DB+t,8);
    }
    ma_envelope e={.kind=rank==PGWT_MERGE_TRANS?MA_TRACE:MA_LIFECYCLE,.version=1,
        .origin=MA_EXACT,.source=src,.position=++h->pos[src],.cookie=cookie,.cookie_present=cookie!=0,
        .bytes=bytes,.size=size,.discovery_origin={.attachment_generation=gen,
            .membership_proved=gen!=0,.marker=marker,.baselines_refreshed=refresh}};
    size_t token=0;
    DC(pgwt_capture_received_token(h->capture,&e,DB+20,&token)>=0,"record consumption");
    DC(pgwt_capture_envelope_token(h->capture,&e,token,DB+20,&h->snapshot)==0,"record adapter delivery");
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
static void endpoint_verdict(struct dh *h,uint64_t closure)
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
    int v=pgwt_coverage_verdict(&model,DB+(int64_t)closure,DB+(int64_t)closure+1,
        DB+100,PGWT_COV_FLOOR_DB,&result);
    DC(v==PGWT_COV_INCOMPLETE,"reader excludes completion exactly at C");
    v=pgwt_coverage_verdict(&model,DB+(int64_t)closure+1,DB+(int64_t)closure+2,
        DB+100,PGWT_COV_FLOOR_DB,&result);
    DC(v==PGWT_COV_COMPLETE,"reader permits interval after conservative endpoint");
    free(result.scratch);
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
            unsigned char bytes[48]={0};tb_le(bytes+8,99,4);
            ma_envelope e={.kind=MA_TRACE,.version=1,.bytes=bytes,.size=48};
            DC(pgwt_capture_received(h->capture,&e,DB+40)==1,"late observation outside merge population");
            state_is(h,"D12U D15T F15 F30",-1,1,false);
            DC(h->coverage[h->coverage_count-1].a==DB,"revoke from boot start");
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
        } else if(strcmp(id,"D13e")) a.proof.known&=~bit;
        dcontrol(h,a);state_is(h,"VOID20:unknown_evidence",-1,1,true);
    } else if(!strcmp(id,"D04d")) {
        state_is(h,"",-1,1,true); /* producer delivered no control or terminal witness */
    } else if(!strcmp(id,"D04e")||!strcmp(id,"D04g")||!strcmp(id,"D13a")) {
        if(!strcmp(id,"D13a")) {dadmit(h,8);data(h,12,0,1);}
        h->snapshot.value[!strcmp(id,"D04e")?10:!strcmp(id,"D04g")?12:0]=1;
        ma_envelope e={.version=1,.kind=MA_CHECK};
        DC(pgwt_capture_envelope(h->capture,&e,DB+20,&h->snapshot)==0,"producer failure checkpoint");
        state_is(h,"VOID20:counter_change",-1,1,true);
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
        /* No A2 evaluator for coverage-a2-v1 exists in this tree. This row's
         * consumer projection proves only that discovery emits no birth or
         * omission. The UNTRACED verdict remains an integration obligation. */
        dadmit(h,8);DC(h->data_count==0,"discovery contributes no lifecycle DATA");
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
        dcontrol(h,admit(30,DG,12,3));dcontrol(h,bracket(30,DG,11,12,3));data(h,9,0,1);
        complete(h,12);state_is(h,"D9U F11",11,0,true);
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
    } else return;
    pgwt_capture_destroy(h->capture);free(h);
}
int main(int argc,char **argv)
{
    size_t ran=0;
    for(size_t i=0;i<sizeof(oracle_rows)/sizeof(*oracle_rows);++i) {
        if(argc>1 && strcmp(argv[1],oracle_rows[i].id)) continue;
        discovery_case=oracle_rows[i].id;printf("REACHED discovery %s\n",discovery_case);fflush(stdout);
        run_row(discovery_case);adversarial_guards(discovery_case);printf("discovery %s: consumer PASS\n",discovery_case);++ran;
    }
    DC(ran>0,"named oracle row reached");
    printf("discovery consumer projections: %zu passed; producer operations and A2 verdict are not qualified here\n",ran);
    return 0;
}
