#define main pgwt_server_main
#include "../../../src/server.c"
#undef main
static uint64_t total(struct pgwt_trace_event *a,int n) {
 struct pgwt_filter f={0}; struct pgwt_transitions_result res;
 pgwt_compute_transitions(a,n,&f,10000,&res);
 uint64_t result=res.total_transitions; free(res.rows); return result;
}
static int admitted(struct pgwt_trace_event *e) {
 struct pgwt_filter f={0};
 return !(e->flags & PGWT_EVENT_FLAG_SAMPLE) && pgwt_filter_matches(&f,e) && !pgwt_is_hidden_event(e->old_event) && !pgwt_is_hidden_event(e->new_event) && e->new_event!=PGWT_EVENT_EXIT && !PGWT_IS_MARKER(e->old_event) && !PGWT_IS_MARKER(e->new_event);
}
int main(int argc,char **argv) {
 struct pgwt_server *srv=calloc(1,sizeof(*srv)); assert(server_init(srv,argv[1])==0);
 int count; struct pgwt_load_info info={0};
 struct pgwt_trace_event *loaded=server_load_events_fi(srv,0,0,0,&count,&info);
 printf("loader records=%d total=%llu categories=%d window=%llu..%llu\n",count,(unsigned long long)total(loaded,count),srv->n_pid_cats,(unsigned long long)srv->earliest_wall_ns,(unsigned long long)srv->latest_wall_ns);
 struct pgwt_event_reader r; char path[4096]; snprintf(path,sizeof(path),"%s/current.trace",argv[1]); assert(pgwt_reader_open(&r,path)==0);
 struct pgwt_trace_event *raw=malloc((size_t)(r.num_blocks+1)*PGWT_BLOCK_EVENTS*sizeof(*raw)); int n=0; int disorder=0;
 for(int b=0;b<r.num_blocks;b++) { struct pgwt_block_info bi; int k=pgwt_reader_decode_block_info(&r,b,raw+n,PGWT_BLOCK_EVENTS,&bi); assert(k>=0); if(bi.block_type==PGWT_BLOCK_TRANSITIONS) n+=k; }
 for(int i=1;i<n;i++) if(raw[i-1].timestamp_ns>raw[i].timestamp_ns)disorder++;
 printf("decoded records=%d untagged=%llu inversions=%d\n",n,(unsigned long long)total(raw,n),disorder);
 struct pgwt_trace_event *nullcats=malloc((size_t)n*sizeof(*raw)),*realcats=malloc((size_t)n*sizeof(*raw));
 memcpy(nullcats,raw,(size_t)n*sizeof(*raw)); memcpy(realcats,raw,(size_t)n*sizeof(*raw));
 pgwt_tag_events(nullcats,n,NULL,0); pgwt_tag_events(realcats,n,srv->pid_cats,srv->n_pid_cats);
 printf("decode-order NULL cats=%llu real cats=%llu\n",(unsigned long long)total(nullcats,n),(unsigned long long)total(realcats,n));
 int differences=0;
 for(int i=0;i<n;i++) if(admitted(&nullcats[i])!=admitted(&realcats[i])) {
 printf("category_difference index=%d pid=%u timestamp=%llu duration=%llu old_raw=%08x old_null=%08x old_real=%08x new=%08x flags_real=%08x\n",i,raw[i].pid,(unsigned long long)raw[i].timestamp_ns,(unsigned long long)raw[i].duration_ns,raw[i].old_event,nullcats[i].old_event,realcats[i].old_event,raw[i].new_event,realcats[i].flags); differences++;
 }
 printf("category admission differences=%d\n",differences);
 assert(stable_sort_events(raw,n)==0);
 memcpy(nullcats,raw,(size_t)n*sizeof(*raw)); memcpy(realcats,raw,(size_t)n*sizeof(*raw));
 pgwt_tag_events(nullcats,n,NULL,0); pgwt_tag_events(realcats,n,srv->pid_cats,srv->n_pid_cats);
 printf("sorted NULL cats=%llu real cats=%llu\n",(unsigned long long)total(nullcats,n),(unsigned long long)total(realcats,n));
 return 0;
}
