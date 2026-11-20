/* Live-test reader adapter, deliberately calling the shared verdict. Input is
 * one boot's trace files in file_no order; times are monotonic nanoseconds.
 * Built in a temporary directory by test_exact_perf_coverage_live.py. */
#include "coverage.h"
#include "coverage_record.h"
#include "event_reader.h"
#include "trace_closure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1],"--events-before")) {
        uint64_t w=strtoull(argv[2],NULL,10);
        for(int file=3;file<argc;++file) {
            struct pgwt_event_reader reader;
            if(pgwt_reader_open(&reader,argv[file])!=0)return 3;
            struct pgwt_trace_event *events=calloc(PGWT_BLOCK_EVENTS,sizeof(*events));
            if(!events)return 5;
            for(int block=0;block<reader.num_blocks;++block) {
                struct pgwt_block_info info;
                int n=pgwt_reader_decode_block_info(&reader,block,events,PGWT_BLOCK_EVENTS,&info);
                if(n<0)return 4;
                for(int i=0;i<n;++i) if(events[i].timestamp_ns<w)
                    printf("%llu %u\n",(unsigned long long)events[i].timestamp_ns,events[i].pid);
            }
            free(events);pgwt_reader_close(&reader);
        }
        return 0;
    }
    if (argc < 5) return 2;
    int64_t from=strtoll(argv[1],NULL,10),to=strtoll(argv[2],NULL,10),now=strtoll(argv[3],NULL,10);
    struct pgwt_cov_boot boot={.same_kernel_boot=1,.status=PGWT_COV_STATUS_NONE};
    struct pgwt_cov_rec *facts=NULL;
    size_t n=0,capacity=0;
    unsigned char nonce[16]={0};
    for(int file=4;file<argc;++file) {
        struct pgwt_event_reader reader;
        if(pgwt_reader_open(&reader,argv[file])!=0) return 3;
        boot.closure_clean=(reader.header.flags & PGWT_FLAG_CLOSED_CLEAN)!=0;
        for(int block=0;block<reader.num_blocks;++block) {
            struct pgwt_coverage_record rec;uint64_t obs;
            int rc=pgwt_reader_coverage_record(&reader,block,&rec,&obs);
            if(rc==1)continue;
            if(rc!=0)return 4;
            if(n==capacity) {
                capacity=capacity?capacity*2:128;
                struct pgwt_cov_rec *next=realloc(facts,capacity*sizeof(*next));
                if(!next)return 5;
                facts=next;
            }
            if(rec.kind==PGWT_COVK_BOOT) {
                if(n && memcmp(nonce,rec.boot_nonce,sizeof(nonce)))return 6;
                memcpy(nonce,rec.boot_nonce,sizeof(nonce));
                boot.boot_start=(int64_t)rec.a;
            }
            facts[n++]=(struct pgwt_cov_rec){.kind=rec.kind==PGWT_COVK_BOOT?PGWT_COV_REC_FILE:rec.kind,
                .a=rec.kind==PGWT_COVK_BOOT?(int64_t)obs:(int64_t)rec.a,.b=(int64_t)rec.b,
                .c=rec.kind==PGWT_COVK_BOOT?1:(int64_t)rec.c};
            boot.last_obs=(int64_t)obs;
        }
        pgwt_reader_close(&reader);
    }
    boot.recs=facts;boot.n_recs=n;
    struct pgwt_cov_model model={.boots=&boot,.n_boots=1};
    struct pgwt_cov_span *spans=calloc(n+16,sizeof(*spans));
    struct pgwt_cov_result result={.uncovered=spans,.uncovered_cap=n+16};
    result.scratch_size=pgwt_cov_scratch_bytes(&model);
    result.scratch=malloc(result.scratch_size);
    if(!spans || !result.scratch)return 7;
    int v=pgwt_coverage_verdict(&model,from,to,now,PGWT_COV_FLOOR_DB,&result);
    printf("{\"verdict\":\"%s\",\"reasons\":[",pgwt_cov_verdict_name(v));
    for(size_t i=0;i<result.n_reasons;++i)
        printf("%s\"%s\"",i?",":"",pgwt_cov_reason_name(result.reasons[i]));
    printf("]}\n");
    free(result.scratch);free(spans);free(facts);
    return v==PGWT_COV_ERROR?8:0;
}
