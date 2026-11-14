#include "../src/core/pgwt_merge_adapter.h"
/* Reuse the existing exact-integer JSON reader and frozen-output comparator.
 * The old runner remains compiled under another name; adapter replay below
 * supplies its own wire producer, source configuration and byte-checking sink. */
#define main merge_contract_test_main
#include "test_core_merge_contract.c"
#undef main
#include "merge_adapter_goldens.h"

/* Assertion failures are clean exits even under LeakSanitizer. This registry
 * belongs to the test harness, including failed-construction mutants. */
static ma *live_adapters[64];
static pgwt_merge *live_cores[64];
static ma *tracked_adapter_create(const ma_config *config, pgwt_merge_reason *error)
{
    ma *a=ma_create(config,error);
    if (a) {
        for (size_t i=0;i<64;++i) if (!live_adapters[i]) { live_adapters[i]=a; return a; }
        ma_destroy(a); fputs("adapter test registry exhausted\n",stderr); exit(2);
    }
    return NULL;
}
static void tracked_adapter_destroy(ma *a)
{
    for (size_t i=0;i<64;++i) if (live_adapters[i]==a) live_adapters[i]=NULL;
    ma_destroy(a);
}
static pgwt_merge *tracked_core_create(const pgwt_merge_config *config, pgwt_merge_status *status)
{
    pgwt_merge *m=pgwt_merge_create(config,status);
    if (m) {
        for (size_t i=0;i<64;++i) if (!live_cores[i]) { live_cores[i]=m; return m; }
        pgwt_merge_destroy(m); fputs("core test registry exhausted\n",stderr); exit(2);
    }
    return NULL;
}
static void tracked_core_destroy(pgwt_merge *m)
{
    for (size_t i=0;i<64;++i) if (live_cores[i]==m) live_cores[i]=NULL;
    pgwt_merge_destroy(m);
}
#define ma_create tracked_adapter_create
#define ma_destroy tracked_adapter_destroy
#define pgwt_merge_create tracked_core_create
#define pgwt_merge_destroy tracked_core_destroy

static const char *active_guard;
#define MUST(expr) do { if (!(expr)) { \
    fprintf(stderr, "adapter ASSERT %s: %s (line %d)\n", active_guard, #expr, __LINE__); \
    return false; } } while (0)

static ma_snapshot readable(void)
{
    ma_snapshot s = {.valid = true};
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) s.component_valid[i] = true;
    return s;
}

static bool same_input(const pgwt_merge_input *a, const pgwt_merge_input *b)
{
#define EQ(field) if (a->field != b->field) return false
    EQ(kind); EQ(observation.d); EQ(observation.counters.known);
    if (memcmp(a->observation.counters.value, b->observation.counters.value,
               sizeof(a->observation.counters.value))) return false;
    EQ(source); EQ(w); EQ(p); EQ(n); EQ(reason);
    EQ(attachments); EQ(attachment_count); EQ(attachment_proofs_fresh); EQ(identity_repaired);
    EQ(record.source); EQ(record.o); EQ(record.t); EQ(record.pid); EQ(record.g); EQ(record.q);
    EQ(record.rank); EQ(record.identity_known); EQ(record.has_q); EQ(record.fork_seed_proved);
    EQ(record.dependency); EQ(record.dependency_id); EQ(record.payload_size);
    if (a->record.payload_size && (!a->record.payload || !b->record.payload ||
        memcmp(a->record.payload, b->record.payload, a->record.payload_size))) return false;
    if (!a->record.payload_size && a->record.payload != b->record.payload) return false;
#undef EQ
    return true;
}

static bool one(ma_result r, pgwt_merge_input expected, bool hold)
{
    if (r.count != 1 || r.hold != hold || !same_input(&r.input[0], &expected)) {
        fprintf(stderr, "adapter ASSERT %s: exact golden input/count/hold\n", active_guard);
        return false;
    }
    return true;
}

/* Every test owns its table. No mutable table state lives inside the adapter. */
typedef struct {
    ma *adapter;
    ma_source_decl sources[4];
    ma_config config;
    ma_lifetime lifetimes[8];
    ma_binding bindings[8];
    const ma_envelope *held[4];
    ma_table table;
    ma_snapshot snapshot;
} premises;

static bool setup(premises *p, pgwt_merge_mode mode)
{
    memset(p, 0, sizeof(*p));
    for (size_t s = 0; s < 4; ++s)
        p->sources[s] = (ma_source_decl){.mechanism = s < 2 ? MA_WATCHPOINT_PERF : MA_ORDERED_RING,
                                        .qualification_complete = true};
    p->config = (ma_config){.mode = mode, .synthetic_premises = true,
                            .sources = p->sources, .source_count = 4};
    pgwt_merge_reason error;
    p->adapter = ma_create(&p->config, &error);
    if (!p->adapter) return false;
    p->lifetimes[0] = (ma_lifetime){.pid=10,.g=1,.identity_bound=true};
    p->lifetimes[1] = (ma_lifetime){.pid=20,.g=1,.identity_bound=true};
    p->lifetimes[2] = (ma_lifetime){.pid=30,.g=1,.identity_bound=true};
    p->lifetimes[3] = (ma_lifetime){.pid=10,.g=2,.identity_bound=true};
    p->lifetimes[4] = (ma_lifetime){.pid=10,.g=0,.identity_bound=true};
    p->bindings[0] = (ma_binding){.query_id=0xabc,.kind=MA_NOT_REQUIRED};
    p->table = (ma_table){.lifetimes=p->lifetimes,.lifetime_count=5,
        .bindings=p->bindings,.binding_count=1,.held=p->held,.source_count=4};
    p->snapshot = readable();
    return true;
}

static ma_envelope wire(ma_kind kind, const unsigned char *bytes, size_t size)
{
    return (ma_envelope){.kind=kind,.version=1,.origin=MA_EXACT,
        .source=kind == MA_TRACE ? 0 : 3,.size=size,.position=1,.cookie=1,
        .q=1,.cookie_present=true,.has_q=true,.bytes=bytes};
}
static ma_envelope control(ma_kind kind)
{
    return (ma_envelope){.kind=kind,.version=1,.cookie_present=true,.cookie=1,.control_pid=30};
}
static ma_result map(premises *p, const ma_envelope *e)
{
    return ma_map(p->adapter, e, &p->table, 20, &p->snapshot);
}
static void qualified_lifetime(ma_lifetime *l)
{
    l->proof=MA_PROVED;
    l->facts_known=l->facts_true=MA_ALL_FACTS;
}

static bool golden1(void)
{
    premises p; MUST(setup(&p, PGWT_MERGE_Q));
    ma_envelope e=wire(MA_TRACE,golden_T,48);
    pgwt_merge_input expected=golden_trace;
    expected.record.g=0; expected.record.identity_known=false;
    e.cookie_present=false;
    MUST(one(map(&p,&e),expected,false));
    e.cookie_present=true; e.cookie=0;
    MUST(one(map(&p,&e),expected,false));
    e.cookie=1;
    MUST(one(map(&p,&e),golden_trace,false));
    ma_destroy(p.adapter); return true;
}
static bool golden2(void)
{
    for (unsigned mode=0; mode<2; ++mode) {
        premises p; MUST(setup(&p,(pgwt_merge_mode)mode));
        ma_envelope e=wire(MA_TRACE,golden_T,48); e.source=2; e.position=9; e.q=3;
        pgwt_merge_input expected=golden_trace;
        expected.record.source=2; expected.record.o=9;
        expected.record.q=mode == PGWT_MERGE_Q ? 3 : 0;
        expected.record.has_q=mode == PGWT_MERGE_Q;
        MUST(one(map(&p,&e),expected,false));
        ma_destroy(p.adapter);
    }
    return true;
}
static bool golden3(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope e=wire(MA_TRACE,golden_T,48);
    pgwt_merge_input expected=golden_trace;
    p.bindings[0].kind=MA_WAIT; p.bindings[0].dependency_id=7;
    expected.record.dependency=PGWT_MERGE_WAIT; expected.record.dependency_id=7;
    MUST(one(map(&p,&e),expected,false));
    p.table.binding_count=0;
    MUST(one(map(&p,&e),golden_interrupted,false));
    p.table.binding_count=1; p.bindings[0].kind=MA_LOST;
    expected.record.dependency=PGWT_MERGE_LOST;
    MUST(one(map(&p,&e),expected,false));
    ma_destroy(p.adapter); return true;
}
static bool golden4(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    unsigned char bytes[48]; memcpy(bytes,golden_T,48);
    memset(bytes+16,0xff,4); memset(bytes+24,0,8);
    ma_envelope e=wire(MA_TRACE,bytes,48);
    pgwt_merge_input expected=golden_trace; expected.record.payload=bytes;
    MUST(one(map(&p,&e),expected,false));
    const unsigned char tags[]={0xf0,0xf1,0xf2,0xf3,0xf6,0xf7};
    for (size_t i=0;i<sizeof(tags);++i) {
        memset(bytes+12,0xff,8); bytes[12]=bytes[16]=tags[i];
        e.source=2; expected.record.source=2;
        MUST(one(map(&p,&e),expected,false));
    }
    ma_destroy(p.adapter); return true;
}
static bool golden5(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope e=wire(MA_LIFECYCLE,golden_F,24);
    p.lifetimes[2].proof=MA_PENDING;
    pgwt_merge_input check={.kind=PGWT_MERGE_CHECK,.observation={.d=20,.counters={.known=true}}};
    MUST(one(map(&p,&e),check,true));
    qualified_lifetime(&p.lifetimes[2]);
    ma_result r=map(&p,&e);
    MUST(one(r,golden_fork,false));
    MUST(r.has_lifetime_update && r.next_lifetime.fork_forwarded);
    p.lifetimes[2].proof=MA_FAILED;
    pgwt_merge_input expected=golden_fork; expected.record.fork_seed_proved=false;
    r=map(&p,&e);
    MUST(r.count==2 && !r.hold && same_input(&r.input[0],&golden_interrupted) &&
         same_input(&r.input[1],&expected) && r.has_lifetime_update && r.next_lifetime.failure_reported);
    p.lifetimes[2]=r.next_lifetime;
    MUST(one(map(&p,&e),expected,false));
    p.lifetimes[2].proof=MA_NOT_ATTEMPTED; p.lifetimes[2].failure_reported=false;
    MUST(one(map(&p,&e),expected,false));
    ma_destroy(p.adapter); return true;
}
static bool golden6(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    qualified_lifetime(&p.lifetimes[2]);
    ma_envelope e=wire(MA_LIFECYCLE,golden_I,24);
    MUST(one(map(&p,&e),golden_init,false));
    e.bytes=golden_X;
    MUST(one(map(&p,&e),golden_exit,false));
    ma_destroy(p.adapter); return true;
}
static bool golden7(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    p.bindings[0].kind=MA_WAIT; p.bindings[0].dependency_id=7;
    ma_envelope e=wire(MA_TEXT,golden_Q,272); e.position=4;
    MUST(one(map(&p,&e),golden_text,false));
    e=wire(MA_TRACE,golden_T,48);
    pgwt_merge_input expected=golden_trace;
    expected.record.dependency=PGWT_MERGE_WAIT; expected.record.dependency_id=7;
    MUST(one(map(&p,&e),expected,false));
    ma_destroy(p.adapter); return true;
}
static bool golden8(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope e=control(MA_BOUNDARY); e.source=2; e.position=4; e.w=11; e.boundary=MA_RING_BOUNDARY;
    MUST(one(map(&p,&e),golden_ring_boundary,false));
    e.source=1; e.boundary=MA_IDLE_PERF;
    MUST(one(map(&p,&e),golden_idle_boundary,false));
    e.boundary=MA_NO_BOUNDARY;
    pgwt_merge_input check={.kind=PGWT_MERGE_CHECK,.observation={.d=20,.counters={.known=true}}};
    MUST(one(map(&p,&e),check,true));
    for (size_t s=0;s<4;++s) {
        ma_mechanism save=p.sources[s].mechanism; p.sources[s].mechanism=MA_NONE;
        pgwt_merge_reason error=PGWT_MERGE_COUNTER_CHANGE;
        ma *bad=ma_create(&p.config,&error); size_t count=99;
        MUST(!bad && error==PGWT_MERGE_UNKNOWN_EVIDENCE);
        MUST(!ma_sources(bad,&count) && !count);
        pgwt_merge_config base={0},out;
        MUST(!ma_core_config(bad,&base,&out));
        p.sources[s].mechanism=save;
    }
    ma_destroy(p.adapter); return true;
}
static bool golden9(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope e=control(MA_LOSS); e.source=1; e.lost=3;
    p.snapshot.value[11]=1;
    MUST(one(ma_map(p.adapter,&e,&p.table,12,&p.snapshot),golden_loss,false));
    ma_destroy(p.adapter); return true;
}
static bool golden10(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope e=wire(MA_LIFECYCLE,golden_F,24); p.lifetimes[2].proof=MA_PENDING;
    p.snapshot.value[11]=1;
    MUST(one(ma_map(p.adapter,&e,&p.table,9,&p.snapshot),golden_held,true));
    pgwt_merge_input expected=golden_held;
    p.snapshot.component_valid[4]=false; expected.observation.counters.known=false;
    MUST(one(ma_map(p.adapter,&e,&p.table,9,&p.snapshot),expected,true));
    memset(expected.observation.counters.value,0,sizeof(expected.observation.counters.value));
    MUST(one(ma_map(p.adapter,&e,&p.table,9,NULL),expected,true));
    ma_destroy(p.adapter); return true;
}
static bool golden11(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope e=control(MA_WRITER_REJECT); p.snapshot.value[14]=1;
    MUST(one(ma_map(p.adapter,&e,&p.table,9,&p.snapshot),golden_writer,false));
    ma_destroy(p.adapter); return true;
}
static bool golden12(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    qualified_lifetime(&p.lifetimes[2]);
    ma_envelope e=control(MA_ATTACH_EVENT); e.attach_event=MA_INTERRUPTED;
    ma_result r=map(&p,&e);
    MUST(one(r,golden_interrupted,false));
    MUST(r.has_lifetime_update && r.next_lifetime.proof==MA_FAILED && r.next_lifetime.failure_reported);
    p.lifetimes[2]=r.next_lifetime;
    e=wire(MA_LIFECYCLE,golden_I,24); r=map(&p,&e);
    MUST(one(r,golden_init,false) && !r.has_lifetime_update && p.lifetimes[2].proof==MA_FAILED);
    ma_destroy(p.adapter); return true;
}

static void put_le(unsigned char *p, uint64_t v, size_t n)
{
    for (size_t i=0;i<n;++i) { p[i]=(unsigned char)v; v>>=8; }
}

typedef struct {
    size_t source, size;
    uint64_t position;
    const char *label;
    const json_node *node;
    unsigned char bytes[272];
} replay_wire;
typedef struct {
    fixture f;
    replay_wire records[256];
    size_t record_count;
    bool bytes_ok;
} replay_capture;

static bool wire_sink(void *context, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    replay_capture *c=context;
    pgwt_merge_output labelled=*out;
    if (out->kind==PGWT_MERGE_OUT_DATA || out->kind==PGWT_MERGE_OUT_TEXT) {
        const replay_wire *w=NULL;
        for (size_t i=0;i<c->record_count;++i)
            if (c->records[i].source==out->record.source && c->records[i].position==out->record.o) {
                w=&c->records[i]; break;
            }
        if (!w || out->record.payload_size!=w->size ||
            memcmp(out->record.payload,w->bytes,w->size)) {
            c->bytes_ok=false;
            return false;
        }
        /* The contract labels identify byte templates; equality of the full
         * actual bytes is checked above, before using the existing comparator. */
        labelled.record.payload=w->label;
        labelled.record.payload_size=strlen(w->label);
    }
    return sink(&c->f,&labelled,post);
}

static ma_snapshot from_counters(pgwt_merge_counters c)
{
    ma_snapshot s=readable(); s.valid=c.known;
    memcpy(s.value,c.value,sizeof(s.value));
    return s;
}

static pgwt_merge_status deliver(premises *p, pgwt_merge *m, const ma_envelope *e,
                                 uint64_t d, const ma_snapshot *s)
{
    ma_result r=ma_map(p->adapter,e,&p->table,d,s);
    if (!r.count || r.count>2) { fputs("adapter replay: invalid input count\n",stderr); exit(2); }
    pgwt_merge_status status=PGWT_MERGE_OK;
    for (size_t i=0;i<r.count;++i) {
        status=pgwt_merge_feed(m,&r.input[i]);
        if (status==PGWT_MERGE_EXIT_REQUIRED) break;
    }
    if (r.has_lifetime_update) {
        bool found=false;
        for (size_t i=0;i<p->table.lifetime_count;++i)
            if (p->lifetimes[i].pid==r.next_lifetime.pid && p->lifetimes[i].g==r.next_lifetime.g) {
                p->lifetimes[i]=r.next_lifetime; found=true; break;
            }
        if (!found) { fputs("adapter replay: update without lifetime\n",stderr); exit(2); }
    }
    if (r.hold) p->held[e->source]=e;
    else if (e->source<4 && p->held[e->source]==e) p->held[e->source]=NULL;
    return status;
}

/* Builds wire inputs solely from fixture INPUTS, never from expected outputs. */
static replay_wire *make_wire(replay_capture *c, premises *p, const json_node *n)
{
    replay_wire *w=&c->records[c->record_count++];
    bool text=string_is(member(n,"kind"),"TEXT");
    w->node=n; w->source=source(&c->f,member(n,"source"));
    w->position=number(member(n,"pos"));
    w->label=string_value(member(n,text ? "name" : "label"));
    if (text) {
        w->size=272; memcpy(w->bytes,golden_Q,272);
        uint64_t dep=dependency_id(&c->f,member(n,"name"));
        put_le(w->bytes+8,dep,8);
        return w;
    }
    const json_node *id=identity(&c->f,string_value(member(n,"who")));
    uint64_t pid=number(member(id,"pid")), t=number(member(n,"t"));
    pgwt_merge_rank rank=rank_of(member(n,"rec"));
    if (rank==PGWT_MERGE_TRANS) {
        w->size=48; memcpy(w->bytes,golden_T,48);
        put_le(w->bytes,t,8); put_le(w->bytes+8,pid,4);
        uint64_t query=0xabc;
        if (member(n,"requires")) {
            query=dependency_id(&c->f,member(n,"requires"));
            bool found=false;
            for (size_t i=0;i<p->table.binding_count;++i)
                if (p->bindings[i].query_id==query) found=true;
            if (!found) p->bindings[p->table.binding_count++]=(ma_binding){
                .query_id=query,.dependency_id=query,.kind=MA_WAIT};
        }
        put_le(w->bytes+32,query,8);
        if (string_is(member(n,"rec"),"MARKER")) {
            put_le(w->bytes+12,0xfffffff0,4); put_le(w->bytes+16,0xfffffff0,4);
            put_le(w->bytes+24,0,8);
        } else if (!strcmp(w->label,"TRANS10") && number(member(c->f.row,"id"))==9) {
            put_le(w->bytes+16,0xffffffff,4);
        } else if (!strcmp(w->label,"leave-X10")) {
            /* The row's distinct zero-duration physical transition. */
            put_le(w->bytes+24,0,8);
        }
    } else {
        w->size=24;
        memcpy(w->bytes,rank==PGWT_MERGE_FORK ? golden_F :
                         rank==PGWT_MERGE_INIT ? golden_I : golden_X,24);
        put_le(w->bytes+4,pid,4); put_le(w->bytes+16,t,8);
    }
    return w;
}

/* cookie_test 1=absent, 2=present zero; otherwise compare frozen outputs. */
static bool adapter_row(const json_node *root, const json_node *row, const char *mode,
                         unsigned cookie_test)
{
    replay_capture c={.f={.root=root,.row=row},.bytes_ok=true};
    premises p; MUST(setup(&p,!strcmp(mode,"N") ? PGWT_MERGE_N : PGWT_MERGE_Q));
    const json_node *start=member(row,"start"), *inputs=member(row,"inputs");
    const json_node *expected=member(member(row,"expected"),mode);
    MUST(list_length(inputs)<=256);
    for (const json_node *n=inputs->child;n;n=n->next) {
        if (string_is(member(n,"kind"),"WRITER_REJECT")) c.f.reject=n;
        if (string_is(member(n,"kind"),"DATA") || string_is(member(n,"kind"),"TEXT"))
            (void)make_wire(&c,&p,n);
        if (member(n,"fork_seed_proved") && boolean(member(n,"fork_seed_proved"))) {
            const json_node *id=identity(&c.f,string_value(member(n,"who")));
            for (size_t i=0;i<p.table.lifetime_count;++i)
                if (p.lifetimes[i].pid==number(member(id,"pid")) &&
                    p.lifetimes[i].g==number(member(id,"g"))) qualified_lifetime(&p.lifetimes[i]);
        }
    }
    for (size_t s=0;s<4;++s)
        p.sources[s].initial_cutoff=optional_number(member(member(start,"cutoffs"),
            string_value(item(member(root,"sources"),s))),0);
    ma_destroy(p.adapter); p.adapter=ma_create(&p.config,NULL); MUST(p.adapter);
    pgwt_merge_attachment proofs[2];
    const char *const attached[]={"A","B"};
    for (size_t i=0;i<2;++i) {
        const json_node *id=identity(&c.f,attached[i]);
        proofs[i]=(pgwt_merge_attachment){.pid=number(member(id,"pid")),.g=number(member(id,"g")),
            .next_q=optional_number(member(member(start,"next_q"),attached[i]),1),
            .identity_proved=true,.sequence_proved=true};
    }
    pgwt_merge_config base={.staged_capacity=128,.payload_capacity=272,.attachment_capacity=32,
        .attachments=proofs,.attachment_count=2,.sink=wire_sink,.sink_context=&c,
        .observation={.counters=named_counters(&c.f,"C0")},.last_observed=named_counters(&c.f,"C0")};
    if (member(start,"baseline")) base.observation.counters=counters(&c.f,member(start,"baseline"));
    if (member(start,"last_observed")) base.last_observed=counters(&c.f,member(start,"last_observed"));
    if (member(start,"suspended")) base.suspended=boolean(member(start,"suspended"));
    if (member(start,"last_frontier")) {
        base.has_frontier=true; base.frontier=number(member(start,"last_frontier"));
    }
    pgwt_merge_config config; MUST(ma_core_config(p.adapter,&base,&config));
    pgwt_merge_status status;
    pgwt_merge *m=pgwt_merge_create(&config,&status); MUST(m);
    pgwt_merge_status statuses[256]={0}; bool processed[256]={0};
    size_t index=0,record_index=0;
    for (const json_node *n=inputs->child;n;n=n->next,++index) {
        const json_node *kind=member(n,"kind");
        if (string_is(kind,"NOTE") || string_is(kind,"WRITER_REJECT")) continue;
        ma_envelope e=control(MA_CHECK);
        if (string_is(kind,"DATA") || string_is(kind,"TEXT")) {
            bool text=string_is(kind,"TEXT"); replay_wire *w=&c.records[record_index++];
            e=wire(text ? MA_TEXT : w->size==48 ? MA_TRACE : MA_LIFECYCLE,w->bytes,w->size);
            e.source=w->source; e.position=w->position;
            if (!text) {
                const json_node *id=identity(&c.f,string_value(member(n,"who")));
                e.cookie=number(member(id,"g"));
                if (cookie_test==1) e.cookie_present=false;
                if (cookie_test==2) e.cookie=0;
                e.has_q=member(n,"q") && member(n,"q")->kind==J_NUMBER;
                e.q=e.has_q ? number(member(n,"q")) : 0;
            }
        } else if (string_is(kind,"BOUNDARY")) {
            e.kind=MA_BOUNDARY; e.source=source(&c.f,member(n,"source"));
            e.position=number(member(n,"pos")); e.w=number(member(n,"w"));
            e.boundary=e.source<2 ? MA_IDLE_PERF : MA_RING_BOUNDARY;
        } else if (string_is(kind,"LOSS")) {
            e.kind=MA_LOSS; e.source=source(&c.f,member(n,"source")); e.lost=number(member(n,"n"));
        } else if (string_is(kind,"FAULT")) {
            /* Only row41 is adapter-driven: a concrete coverage interruption. */
            MUST(number(member(row,"id"))==41);
            e.kind=MA_ATTACH_EVENT; e.attach_event=MA_INTERRUPTED; e.control_pid=30;
        } else MUST(string_is(kind,"CHECK"));
        ma_snapshot s=from_counters(member(n,"C") ? counters(&c.f,member(n,"C")) : named_counters(&c.f,"C0"));
        status=deliver(&p,m,&e,optional_number(member(n,"d"),0),&s);
        statuses[index]=status; processed[index]=true;
        if (status==PGWT_MERGE_EXIT_REQUIRED) break;
    }
    bool pass=c.bytes_ok;
    if (cookie_test) {
        pass=pass && status==PGWT_MERGE_SUSPENDED && c.f.count==2;
        for (size_t i=0;i<c.f.count;++i)
            pass=pass && c.f.outputs[i].out.kind==PGWT_MERGE_OUT_VOID &&
                 c.f.outputs[i].out.reason==PGWT_MERGE_UNKNOWN_EVIDENCE;
    } else {
        const json_node *outputs=member(expected,"outputs");
        const char *field=NULL;
        if (c.f.count!=list_length(outputs)) field="output count";
        else for (size_t i=0;i<c.f.count;++i)
            if (!equal_output(&c.f,&c.f.outputs[i],item(outputs,i),&field)) break;
        pass=pass && !field && ((status==PGWT_MERGE_EXIT_REQUIRED)==boolean(member(expected,"exit_nonzero")));
        const json_node *invalid=member(expected,"epoch_invalid");
        pass=pass && pgwt_merge_get_report(m).epoch_invalid==(invalid ? boolean(invalid) : false);
        const json_node *checks=member(expected,"statuses");
        for (const json_node *s=checks ? checks->child : NULL;s;s=s->next) {
            size_t at=(size_t)number(member(s,"input_index"));
            pass=pass && at<256 && processed[at] && statuses[at]==status_value(member(s,"status"));
        }
        if (c.f.reject) pass=pass && c.f.rejected;
        if (!pass) fprintf(stderr,"adapter row %" PRIu64 "/%s MISMATCH %s (%zu outputs)\n",
            number(member(row,"id")),mode,field ? field : "status/bytes/invalidation",c.f.count);
    }
    pgwt_merge_destroy(m); ma_destroy(p.adapter);
    MUST(pass);
    return true;
}

static pgwt_merge *test_core(premises *p, safety_capture *capture, bool attached)
{
    pgwt_merge_attachment proof={.pid=10,.g=1,.next_q=1,.identity_proved=true,.sequence_proved=true};
    pgwt_merge_config base={.staged_capacity=32,.payload_capacity=272,.attachment_capacity=8,
        .attachments=attached ? &proof : NULL,.attachment_count=attached ? 1 : 0,
        .sink=safety_sink,.sink_context=capture,
        .observation={.counters={.known=true}},.last_observed={.known=true}}, config;
    if (!ma_core_config(p->adapter,&base,&config)) return NULL;
    pgwt_merge_status status;
    return pgwt_merge_create(&config,&status);
}

static bool proof_negative(uint64_t bit)
{
    for (unsigned unknown=0;unknown<2;++unknown) {
        premises p; MUST(setup(&p,PGWT_MERGE_Q));
        ma_envelope begin=control(MA_ATTACH_EVENT); begin.attach_event=MA_BEGIN;
        ma_result r=map(&p,&begin);
        MUST(r.count==1 && r.input[0].kind==PGWT_MERGE_CHECK && r.has_lifetime_update &&
             r.next_lifetime.proof==MA_PENDING);
        p.lifetimes[2]=r.next_lifetime;
        ma_envelope fork=wire(MA_LIFECYCLE,golden_F,24);
        r=map(&p,&fork); MUST(r.hold && r.count==1 && r.input[0].kind==PGWT_MERGE_CHECK);
        p.held[3]=&fork;
        ma_envelope complete=control(MA_ATTACH_EVENT); complete.attach_event=MA_COMPLETE;
        complete.facts_known=MA_ALL_FACTS; complete.facts_true=MA_ALL_FACTS;
        if (unknown) complete.facts_known &= ~bit;
        else complete.facts_true &= ~bit;
        r=map(&p,&complete);
        MUST(one(r,golden_interrupted,false) && r.has_lifetime_update &&
             r.next_lifetime.proof==MA_FAILED && r.next_lifetime.failure_reported);
        p.lifetimes[2]=r.next_lifetime;
        r=map(&p,&fork);
        pgwt_merge_input expected=golden_fork; expected.record.fork_seed_proved=false;
        MUST(one(r,expected,false) && r.next_lifetime.proof==MA_FAILED);
        p.lifetimes[2]=r.next_lifetime; p.held[3]=NULL;
        ma_envelope init=wire(MA_LIFECYCLE,golden_I,24); r=map(&p,&init);
        MUST(one(r,golden_init,false) && !r.has_lifetime_update && p.lifetimes[2].proof==MA_FAILED);
        complete.facts_known=complete.facts_true=MA_ALL_FACTS;
        r=map(&p,&complete);
        MUST(r.input[0].kind==PGWT_MERGE_CHECK && r.next_lifetime.proof==MA_FAILED);
        /* Already-PROVED but invalidating provenance also faults before FORK. */
        qualified_lifetime(&p.lifetimes[2]); p.lifetimes[2].failure_reported=false;
        if (unknown) p.lifetimes[2].facts_known &= ~bit;
        else p.lifetimes[2].facts_true &= ~bit;
        r=map(&p,&fork);
        MUST(r.count==2 && same_input(&r.input[0],&golden_interrupted) &&
             same_input(&r.input[1],&expected) && r.next_lifetime.proof==MA_FAILED);
        ma_destroy(p.adapter);
    }
    return true;
}
static bool proof_nonfork(void) { return proof_negative(MA_FRESH_FORK); }
static bool proof_late_cookie(void) { return proof_negative(MA_ATTACH_ORDER); }
static bool proof_pid_only(void) { return proof_negative(MA_MATCHING_COOKIE); }
static bool proof_reused(void) { return proof_negative(MA_ALLOCATOR); }
static bool proof_unsafe_cleanup(void) { return proof_negative(MA_SAFE_CLEANUP); }
static bool proof_overwrite_cookie(void) { return proof_negative(MA_PRESERVE_STATE); }
static bool proof_overwrite_q(void) { return proof_negative(MA_SEQUENCE); }
static bool proof_stale_marker(void) { return proof_negative(MA_FRESH_FIRST); }
static bool proof_other_facts(void)
{
    return proof_negative(MA_EMISSION_COOKIE) && proof_negative(MA_FAILURE_ACCOUNTING) &&
           proof_negative(MA_CONTINUOUS) && proof_negative(MA_IDEMPOTENT_INIT);
}

static bool state_and_ownership(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope begin=control(MA_ATTACH_EVENT); begin.attach_event=MA_BEGIN;
    ma_result r=map(&p,&begin); MUST(r.next_lifetime.proof==MA_PENDING);
    p.lifetimes[2]=r.next_lifetime;
    ma_envelope fork=wire(MA_LIFECYCLE,golden_F,24);
    MUST(map(&p,&fork).hold); p.held[3]=&fork;
    ma_envelope later=control(MA_BOUNDARY); later.source=3; later.position=2;
    later.w=100; later.boundary=MA_RING_BOUNDARY;
    r=map(&p,&later);
    MUST(r.count==1 && r.input[0].kind==PGWT_MERGE_FAULT && r.input[0].reason==PGWT_MERGE_QUEUE_OWNERSHIP);
    later=wire(MA_LIFECYCLE,golden_I,24); later.position=2;
    MUST(map(&p,&later).input[0].reason==PGWT_MERGE_QUEUE_OWNERSHIP);
    ma_envelope other=wire(MA_TRACE,golden_T,48);
    MUST(one(map(&p,&other),golden_trace,false));
    ma_envelope complete=control(MA_ATTACH_EVENT); complete.attach_event=MA_COMPLETE;
    complete.facts_known=complete.facts_true=MA_ALL_FACTS;
    r=map(&p,&complete); MUST(r.next_lifetime.proof==MA_PROVED && r.input[0].kind==PGWT_MERGE_CHECK);
    p.lifetimes[2]=r.next_lifetime;
    p.snapshot.value[11]=1;
    r=ma_map(p.adapter,&fork,&p.table,90,&p.snapshot);
    MUST(!r.hold && r.input[0].kind==PGWT_MERGE_DATA && r.input[0].record.fork_seed_proved &&
         r.input[0].observation.d==90 && r.input[0].observation.counters.value[11]==1);
    p.lifetimes[2]=r.next_lifetime; p.held[3]=NULL;
    r=map(&p,&begin);
    MUST(r.input[0].kind==PGWT_MERGE_FAULT && r.next_lifetime.proof==MA_FAILED);
    /* An ordinary forwarded FORK likewise cannot be upgraded later. */
    p.lifetimes[2].proof=MA_NOT_ATTEMPTED;
    r=map(&p,&begin); MUST(r.input[0].kind==PGWT_MERGE_FAULT && r.next_lifetime.proof==MA_FAILED);
    for (unsigned event=MA_INTERRUPTED;event<=MA_NONIDEMPOTENT_INIT;++event) {
        qualified_lifetime(&p.lifetimes[2]);
        begin.attach_event=(ma_attach_event)event;
        r=map(&p,&begin); MUST(r.input[0].kind==PGWT_MERGE_FAULT && r.next_lifetime.proof==MA_FAILED);
        p.lifetimes[2]=r.next_lifetime;
        r=map(&p,&begin); MUST(r.input[0].kind==PGWT_MERGE_FAULT); /* separately reported */
    }
    ma_destroy(p.adapter);
    /* N alone may omit sequence obligation bit7. */
    MUST(setup(&p,PGWT_MERGE_N)); qualified_lifetime(&p.lifetimes[2]);
    p.lifetimes[2].facts_known &= ~(uint64_t)MA_SEQUENCE;
    p.lifetimes[2].facts_true &= ~(uint64_t)MA_SEQUENCE;
    MUST(map(&p,&fork).input[0].record.fork_seed_proved);
    ma_destroy(p.adapter); return true;
}

static bool observations_and_holds(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    safety_capture c={0}; pgwt_merge *m=test_core(&p,&c,true); MUST(m);
    ma_envelope e=wire(MA_LIFECYCLE,golden_F,24); p.lifetimes[2].proof=MA_PENDING;
    MUST(deliver(&p,m,&e,8,&p.snapshot)==PGWT_MERGE_OK && !c.count);
    p.snapshot.value[11]=1;
    MUST(deliver(&p,m,&e,9,&p.snapshot)==PGWT_MERGE_SUSPENDED && c.count==1 &&
         c.outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE && c.outputs[0].d==9);
    MUST(deliver(&p,m,&e,10,&p.snapshot)==PGWT_MERGE_SUSPENDED && c.count==1);
    p.snapshot.value[11]=0;
    MUST(deliver(&p,m,&e,11,&p.snapshot)==PGWT_MERGE_SUSPENDED && c.count==2 &&
         c.outputs[1].reason==PGWT_MERGE_COUNTER_UNKNOWN);
    ma_envelope loss=control(MA_LOSS); loss.source=1; loss.lost=3;
    MUST(deliver(&p,m,&loss,12,&p.snapshot)==PGWT_MERGE_SUSPENDED && c.count==3 &&
         c.outputs[2].reason==PGWT_MERGE_PERF_LOST);
    pgwt_merge_destroy(m); p.held[3]=NULL;
    for (size_t i=0;i<PGWT_MERGE_COUNTERS;++i) {
        p.snapshot=readable(); p.snapshot.value[i]=i+1;
        p.snapshot.component_valid[i]=false;
        ma_result r=map(&p,&e);
        MUST(r.count==1 && r.hold && !r.input[0].observation.counters.known &&
             r.input[0].observation.counters.value[i]==i+1);
    }
    p.snapshot=readable(); p.snapshot.valid=false;
    MUST(!map(&p,&e).input[0].observation.counters.known);
    /* Unqualified boundary hold must also carry the counter observation. */
    p.snapshot=readable(); p.snapshot.value[11]=1;
    ma_envelope boundary=control(MA_BOUNDARY); boundary.source=2;
    ma_result r=ma_map(p.adapter,&boundary,&p.table,9,&p.snapshot);
    MUST(one(r,golden_held,true));
    p.snapshot=readable(); c=(safety_capture){0}; m=test_core(&p,&c,true); MUST(m);
    ma_envelope check=control(MA_CHECK); p.snapshot.value[1]=1;
    MUST(deliver(&p,m,&check,20,&p.snapshot)==PGWT_MERGE_SUSPENDED && c.count==1 &&
         c.outputs[0].reason==PGWT_MERGE_COUNTER_CHANGE && c.outputs[0].counters.value[1]==1);
    pgwt_merge_destroy(m);
    /* Zero LOST is not a new positive report; non-perf LOST is malformed. */
    loss.lost=0; MUST(map(&p,&loss).input[0].n==0);
    loss.source=2; r=map(&p,&loss);
    MUST(r.input[0].kind==PGWT_MERGE_FAULT && r.input[0].reason==PGWT_MERGE_MALFORMED_ENVELOPE);
    ma_destroy(p.adapter); return true;
}

static bool binding_and_decode(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    ma_envelope trace=wire(MA_TRACE,golden_T,48);
    /* Explicit byte decoding also works at unaligned addresses. */
    unsigned char unaligned[49]; memcpy(unaligned+1,golden_T,48); trace.bytes=unaligned+1;
    MUST(one(map(&p,&trace),golden_trace,false));
    trace.size=47;
    ma_result r=map(&p,&trace); MUST(r.input[0].reason==PGWT_MERGE_MALFORMED_ENVELOPE);
    trace.size=49; MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_MALFORMED_ENVELOPE);
    trace.size=48; trace.version=2; MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_MALFORMED_ENVELOPE);
    trace.version=1; trace.origin=MA_SAMPLE;
    MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    trace.origin=MA_ADMIN; MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    trace.origin=MA_EXACT; trace.cookie=77;
    MUST(!map(&p,&trace).input[0].record.identity_known);
    trace.cookie=1; p.lifetimes[5]=p.lifetimes[0]; p.table.lifetime_count=6;
    MUST(!map(&p,&trace).input[0].record.identity_known);
    p.table.lifetime_count=5;
    p.bindings[1]=p.bindings[0]; p.table.binding_count=2;
    MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_DEPENDENCY_LOST);
    p.bindings[0].kind=MA_WAIT; p.bindings[0].dependency_id=7;
    p.bindings[1]=(ma_binding){.query_id=0xdef,.kind=MA_WAIT,.dependency_id=7};
    MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_DEPENDENCY_LOST);
    p.table.binding_count=1; p.bindings[0].dependency_id=0;
    MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_DEPENDENCY_LOST);
    p.bindings[0].kind=MA_NOT_REQUIRED; p.bindings[0].dependency_id=7;
    MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_DEPENDENCY_LOST);
    ma_envelope text=wire(MA_TEXT,golden_Q,272);
    p.table.binding_count=0; MUST(map(&p,&text).input[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    p.table.binding_count=1; p.bindings[0].kind=MA_LOST;
    MUST(map(&p,&text).input[0].reason==PGWT_MERGE_DEPENDENCY_LOST);
    text.size=24; MUST(map(&p,&text).input[0].reason==PGWT_MERGE_MALFORMED_ENVELOPE);
    unsigned char bad[48]; memcpy(bad,golden_T,48); put_le(bad+8,0,4);
    trace=wire(MA_TRACE,bad,48); MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    memcpy(bad,golden_T,48); put_le(bad+20,1,4);
    MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    memcpy(bad,golden_T,48); put_le(bad+12,0xfffffff4,4);
    MUST(map(&p,&trace).input[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    ma_envelope boundary=control(MA_BOUNDARY); boundary.source=0; boundary.boundary=MA_RING_BOUNDARY;
    MUST(map(&p,&boundary).input[0].reason==PGWT_MERGE_MALFORMED_ENVELOPE);
    ma_envelope lifecycle=wire(MA_LIFECYCLE,golden_I,24); lifecycle.source=2;
    MUST(map(&p,&lifecycle).input[0].reason==PGWT_MERGE_MALFORMED_ENVELOPE);
    ma_destroy(p.adapter); return true;
}

static bool construction_and_refusals(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    pgwt_merge_reason error;
    p.config.synthetic_premises=false;
    MUST(!ma_create(&p.config,&error) && error==PGWT_MERGE_UNKNOWN_EVIDENCE);
    p.config.synthetic_premises=true;
    for (size_t s=0;s<4;++s) {
        p.sources[s].qualification_complete=false;
        MUST(!ma_create(&p.config,&error) && error==PGWT_MERGE_UNKNOWN_EVIDENCE);
        p.sources[s].qualification_complete=true;
        ma_mechanism save=p.sources[s].mechanism;
        p.sources[s].mechanism=s<2 ? MA_ORDERED_RING : MA_WATCHPOINT_PERF;
        MUST(!ma_create(&p.config,&error) && error==PGWT_MERGE_UNKNOWN_EVIDENCE);
        p.sources[s].mechanism=save;
    }
    size_t count; const pgwt_merge_source *sources=ma_sources(p.adapter,&count);
    MUST(sources && count==4);
    for (size_t i=0;i<4;++i) MUST(sources[i].qualified && !sources[i].cutoff);
    /* Adapter owns copies; changing the declarations cannot retroactively
     * qualify a stream or change accepted starting cutoffs. */
    p.sources[0].initial_cutoff=999; p.sources[0].qualification_complete=false;
    MUST(!ma_sources(p.adapter,NULL)[0].cutoff && ma_sources(p.adapter,NULL)[0].qualified);
    pgwt_merge_attachment proof={.pid=10,.g=1,.next_q=9,.identity_proved=true,.sequence_proved=true};
    pgwt_merge_config base={.attachments=&proof,.attachment_count=1,.staged_capacity=7,
        .payload_capacity=300,.frontier_capacity=9,.suspended=true,.has_frontier=true,.frontier=5,
        .sink=safety_sink,.mode=PGWT_MERGE_N},out;
    MUST(ma_core_config(p.adapter,&base,&out));
    MUST(out.sources==sources && out.source_count==4 && out.mode==PGWT_MERGE_Q &&
         out.attachments==&proof && out.attachments[0].next_q==9 && out.attachment_count==1 &&
         out.staged_capacity==7 && out.payload_capacity==300 && out.frontier_capacity==9 &&
         out.suspended && out.has_frontier && out.frontier==5 && out.sink==safety_sink);
    base.sources=sources;
    MUST(!ma_core_config(p.adapter,&base,&out));
    ma_destroy(p.adapter); return true;
}

static bool failed_attempt_no_laundering(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    safety_capture c={0}; pgwt_merge *m=test_core(&p,&c,false); MUST(m);
    ma_envelope event=control(MA_ATTACH_EVENT); event.attach_event=MA_BEGIN;
    MUST(deliver(&p,m,&event,18,&p.snapshot)==PGWT_MERGE_OK);
    ma_envelope fork=wire(MA_LIFECYCLE,golden_F,24);
    MUST(deliver(&p,m,&fork,19,&p.snapshot)==PGWT_MERGE_OK && p.held[3]==&fork);
    event.attach_event=MA_COMPLETE;
    event.facts_known=MA_ALL_FACTS; event.facts_true=MA_ALL_FACTS & ~MA_FRESH_FORK;
    MUST(deliver(&p,m,&event,20,&p.snapshot)==PGWT_MERGE_SUSPENDED && c.count==1 &&
         c.outputs[0].kind==PGWT_MERGE_OUT_VOID && c.outputs[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    MUST(deliver(&p,m,&fork,21,&p.snapshot)==PGWT_MERGE_SUSPENDED && !p.held[3]);
    ma_envelope init=wire(MA_LIFECYCLE,golden_I,24); init.position=2;
    MUST(deliver(&p,m,&init,22,&p.snapshot)==PGWT_MERGE_SUSPENDED);
    unsigned char bytes[48]; memcpy(bytes,golden_T,48); put_le(bytes+8,30,4); put_le(bytes,12,8);
    ma_envelope trace=wire(MA_TRACE,bytes,48);
    MUST(deliver(&p,m,&trace,23,&p.snapshot)==PGWT_MERGE_SUSPENDED);
    for (size_t source=0;source<4;++source) {
        ma_envelope boundary=control(MA_BOUNDARY); boundary.source=source; boundary.w=13;
        boundary.position=source==3 ? 3 : 2;
        boundary.boundary=source<2 ? MA_IDLE_PERF : MA_RING_BOUNDARY;
        MUST(deliver(&p,m,&boundary,24,&p.snapshot)==PGWT_MERGE_SUSPENDED);
    }
    MUST(c.count==1 && p.lifetimes[2].proof==MA_FAILED);
    pgwt_merge_destroy(m); ma_destroy(p.adapter); return true;
}

static bool recovered_init_refusal(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    safety_capture c={0}; pgwt_merge *m=test_core(&p,&c,false); MUST(m);
    /* Stored INIT recovery supplies no L envelope. Only genuine FORK and
     * subsequent DATA exist; neither a cookie nor a timer invents INIT. */
    ma_envelope fork=wire(MA_LIFECYCLE,golden_F,24);
    MUST(deliver(&p,m,&fork,20,&p.snapshot)==PGWT_MERGE_OK);
    ma_envelope check=control(MA_CHECK);
    MUST(deliver(&p,m,&check,21,&p.snapshot)==PGWT_MERGE_OK);
    unsigned char bytes[48]; memcpy(bytes,golden_T,48);
    put_le(bytes+8,30,4); put_le(bytes,12,8);
    ma_envelope trace=wire(MA_TRACE,bytes,48);
    MUST(deliver(&p,m,&trace,22,&p.snapshot)==PGWT_MERGE_OK);
    for (size_t s=0;s<4;++s) {
        ma_envelope boundary=control(MA_BOUNDARY); boundary.source=s; boundary.position=2;
        boundary.w=13; boundary.boundary=s<2 ? MA_IDLE_PERF : MA_RING_BOUNDARY;
        (void)deliver(&p,m,&boundary,23,&p.snapshot);
    }
    MUST(pgwt_merge_get_report(m).status==PGWT_MERGE_SUSPENDED && c.count==1 &&
         c.outputs[0].kind==PGWT_MERGE_OUT_VOID && c.outputs[0].reason==PGWT_MERGE_UNKNOWN_EVIDENCE);
    MUST(p.lifetimes[2].proof==MA_NOT_ATTEMPTED);
    pgwt_merge_destroy(m);
    /* A genuinely delivered late INIT keeps its timestamp and violates L's
     * accepted boundary. A stored fact is never restamped into that position. */
    c=(safety_capture){0}; m=test_core(&p,&c,true); MUST(m);
    ma_envelope boundary=control(MA_BOUNDARY); boundary.source=3; boundary.position=1;
    boundary.w=13; boundary.boundary=MA_RING_BOUNDARY;
    MUST(deliver(&p,m,&boundary,20,&p.snapshot)==PGWT_MERGE_OK);
    ma_envelope init=wire(MA_LIFECYCLE,golden_I,24); init.position=2;
    MUST(deliver(&p,m,&init,40,&p.snapshot)==PGWT_MERGE_SUSPENDED && c.count==1 &&
         c.outputs[0].reason==PGWT_MERGE_SEAL_VIOLATION && c.outputs[0].d==40);
    pgwt_merge_destroy(m); ma_destroy(p.adapter); return true;
}

static bool rejecting_writer(void *context, const pgwt_merge_output *out,
                              pgwt_merge_observation *post)
{
    safety_capture *c=context;
    if (out->kind==PGWT_MERGE_OUT_DATA) {
        ++c->transition_attempts;
        *post=golden_writer.observation;
        return false;
    }
    return safety_sink(context,out,post);
}
static bool sink_post_failure(void)
{
    premises p; MUST(setup(&p,PGWT_MERGE_Q));
    safety_capture c={0};
    pgwt_merge_attachment proof={.pid=10,.g=1,.next_q=1,.identity_proved=true,.sequence_proved=true};
    pgwt_merge_config base={.staged_capacity=8,.payload_capacity=272,.attachment_capacity=8,
        .attachments=&proof,.attachment_count=1,.sink=rejecting_writer,.sink_context=&c,
        .observation={.counters={.known=true}},.last_observed={.known=true}},config;
    MUST(ma_core_config(p.adapter,&base,&config)); pgwt_merge_status status;
    pgwt_merge *m=pgwt_merge_create(&config,&status); MUST(m);
    ma_envelope trace=wire(MA_TRACE,golden_T,48);
    MUST(deliver(&p,m,&trace,0,&p.snapshot)==PGWT_MERGE_OK);
    for (size_t s=0;s<4;++s) {
        ma_envelope boundary=control(MA_BOUNDARY); boundary.source=s;
        boundary.position=s==0 ? 2 : 1; boundary.w=11;
        boundary.boundary=s<2 ? MA_IDLE_PERF : MA_RING_BOUNDARY;
        status=deliver(&p,m,&boundary,0,&p.snapshot);
    }
    MUST(status==PGWT_MERGE_SUSPENDED && c.transition_attempts==1 && c.count==1 &&
         c.outputs[0].kind==PGWT_MERGE_OUT_VOID && c.outputs[0].d==9 &&
         c.outputs[0].reason==PGWT_MERGE_WRITER_PUSH_FAIL &&
         equal_counters(c.outputs[0].counters,golden_writer.observation.counters));
    pgwt_merge_destroy(m); ma_destroy(p.adapter); return true;
}

typedef struct { const char *name; bool (*run)(void); } adapter_guard;
static const adapter_guard adapter_guards[]={
    {"G1",golden1},{"G2",golden2},{"G3",golden3},{"G4",golden4},
    {"G5",golden5},{"G6",golden6},{"G7",golden7},{"G8",golden8},
    {"G9",golden9},{"G10",golden10},{"G11",golden11},{"G12",golden12},
    {"proof_nonfork",proof_nonfork},{"proof_late_cookie",proof_late_cookie},
    {"proof_pid_only",proof_pid_only},{"proof_reused",proof_reused},
    {"proof_unsafe_cleanup",proof_unsafe_cleanup},{"proof_overwrite_cookie",proof_overwrite_cookie},
    {"proof_overwrite_q",proof_overwrite_q},{"proof_stale_marker",proof_stale_marker},
    {"proof_other_facts",proof_other_facts},{"state_ownership",state_and_ownership},
    {"observations_holds",observations_and_holds},{"binding_decode",binding_and_decode},
    {"construction_refusals",construction_and_refusals},{"recovered_init",recovered_init_refusal},
    {"writer_sink",sink_post_failure},{"failed_attempt",failed_attempt_no_laundering}
};

static int adapter_main(int argc, char **argv)
{
    if (argc==3 && !strcmp(argv[1],"--guard")) {
        for (size_t i=0;i<sizeof(adapter_guards)/sizeof(adapter_guards[0]);++i)
            if (!strcmp(argv[2],adapter_guards[i].name)) {
                active_guard=adapter_guards[i].name; printf("REACHED %s\n",active_guard); fflush(stdout);
                return adapter_guards[i].run() ? 0 : 1;
            }
        fputs("adapter: unknown guard\n",stderr); return 2;
    }
    if (argc>2) return 2;
    size_t passed=0;
    for (size_t i=0;i<sizeof(adapter_guards)/sizeof(adapter_guards[0]);++i) {
        active_guard=adapter_guards[i].name; printf("REACHED %s\n",active_guard); fflush(stdout);
        if (!adapter_guards[i].run()) return 1;
        ++passed;
    }
    printf("adapter goldens: 12/12; guard sweep: %zu/%zu\n",passed, sizeof(adapter_guards)/sizeof(adapter_guards[0]));
    char error[256];
    json_document *d=read_document(argc>1 ? argv[1] : "contracts/merge-v1/examples.json",error,sizeof(error));
    if (!d && argc==1) d=read_document("../contracts/merge-v1/examples.json",error,sizeof(error));
    if (!d) { fprintf(stderr,"adapter: %s\n",error); return 2; }
    const json_node *rows=member(d->root,"rows");
    bool seen[44]={0}; size_t count=0,cases=0;
    active_guard="end_to_end";
    for (const json_node *row=rows ? rows->child : NULL;row;row=row->next) {
        uint64_t id=number(member(row,"id"));
        if (id<1 || id>43 || seen[id]) { free_document(d); return 2; }
        seen[id]=true;
        if (id==27 || id==28 || id==30 || id==31) continue;
        if (list_length(member(row,"modes"))!=2) { free_document(d); return 2; }
        for (size_t mode=0;mode<2;++mode) {
            const char *name=mode ? "Q" : "N";
            if (!adapter_row(d->root,row,name,0)) { free_document(d); return 1; }
            printf("adapter row %" PRIu64 "/%s PASS\n",id,name); ++cases;
        }
        ++count;
    }
    for (size_t id=1;id<=43;++id) if (!seen[id]) { free_document(d); return 2; }
    printf("adapter end-to-end: %zu rows, %zu/%zu N/Q cases\n",count,cases,2*count);
    if (count!=39 || cases!=78) { free_document(d); return 2; }
    active_guard="negative_cookie";
    const json_node *row1=NULL;
    for (const json_node *row=rows->child;row;row=row->next)
        if (number(member(row,"id"))==1) row1=row;
    for (unsigned mode=0;mode<2;++mode)
        for (unsigned cookie=1;cookie<=2;++cookie)
            if (!adapter_row(d->root,row1,mode ? "Q" : "N",cookie)) { free_document(d); return 1; }
    puts("adapter negative controls: 4/4 absent/zero-cookie N/Q; NONE source refusal: 4/4");
    puts("adapter refusals: production construction, default shared ring, cookie-less DATA, recovered INIT, production proof PASS");
    free_document(d); return 0;
}

int main(int argc, char **argv)
{
    int result=adapter_main(argc,argv);
    for (size_t i=0;i<64;++i) {
        if (live_cores[i]) pgwt_merge_destroy(live_cores[i]);
        if (live_adapters[i]) ma_destroy(live_adapters[i]);
    }
    return result;
}
