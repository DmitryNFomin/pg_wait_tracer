/* The registered mutant runner appends the verbatim Linux reader functions
 * below these stubs. Bytes, sequence accounting, adapter and core are real;
 * only libbpf's mmap iteration and the monotonic clock are injected. */
struct perf_event_header { uint32_t type; uint16_t misc, size; };
enum { PERF_RECORD_SAMPLE = 9, PERF_RECORD_LOST = 2 };
enum bpf_perf_event_ret { LIBBPF_PERF_EVENT_CONT, LIBBPF_PERF_EVENT_DONE };
struct perf_buffer { struct tb_driver *driver; unsigned cpu; size_t left; bool hint; };
struct ring_context { struct pgwt_daemon *d; uint32_t dest; size_t consumed, limit; };
struct ring_buffer { struct tb_driver *driver; struct ring_context *ctx; size_t left; };
struct linux_transport {
    struct pgwt_daemon *d;
    size_t ncpu, next_reader;
    struct perf_buffer **perf, **hints;
    struct ring_buffer *rings[2];
    struct ring_context ring_ctx[2];
    int callback_error, epoll_fd;
};
static struct linux_transport *linux_state(struct pgwt_daemon *d)
{ return d->percpu_transport->platform; }
static int receive_ring(void *, void *, size_t);
static enum bpf_perf_event_ret receive_perf(void *, int, struct perf_event_header *);
static enum bpf_perf_event_ret receive_hint(void *, int, struct perf_event_header *);
static int consume_all(struct linux_transport *, bool *);
static int linux_consume_io(void *, uint32_t, bool);
static int linux_drain(void *);
static int linux_wait(void *);
struct epoll_event { int unused; };
static struct tb_driver *raw_wait_driver;
static int raw_wait_timeout;
/* Extracted reader bodies reference the optional timeline clock. This raw
 * budget suite leaves it off; reaching the clock would violate that premise. */
uint64_t pgwt_debug_monotonic_ns(void)
{
    fputs("FAIL unexpected attach-timeline clock on the off path\n", stderr);
    exit(2);
}
static int epoll_wait(int fd, struct epoll_event *events, int count, int timeout)
{
    (void)fd; (void)events; (void)count;
    raw_wait_timeout=timeout; raw_wait_driver->now+=(uint64_t)timeout*1000000;
    return 0;
}
static size_t raw_boundary(struct tb_driver *d, unsigned cpu, unsigned dest, unsigned char *wire)
{
    /* Stale round 0: accounted, never accepted as completion evidence. */
    unsigned char body[16] = {0};
    tb_le(body, 14, 8); tb_le(body + 12, dest, 4);
    size_t key = cpu * 3 + dest;
    d->attempts[key] = ++d->seq[key];
    return tb_packet(wire, PGWT_WIRE_BOUNDARY, cpu, d->seq[key], 0, body, sizeof(body));
}
static int perf_buffer__consume(struct perf_buffer *p)
{
    struct linux_transport *l = p->driver->transport->platform;
    while (p->left) {
        --p->left;
        struct { struct perf_event_header event; unsigned char raw[56]; } sample = {
            .event = {.type = PERF_RECORD_SAMPLE, .size = sizeof(sample)}};
        enum bpf_perf_event_ret rc;
        if (p->hint) {
            p->driver->now += p->driver->received_cost_ns;
            rc = receive_hint(l, (int)p->cpu, &sample.event);
        } else {
            tb_le(sample.raw, 52, 4);
            raw_boundary(p->driver, p->cpu, PGWT_WIRE_DEST_CPU, sample.raw + 4);
            rc = receive_perf(l, (int)p->cpu, &sample.event);
        }
        /* libbpf consumes the callback's record before obeying DONE. */
        if (rc == LIBBPF_PERF_EVENT_DONE) break;
    }
    return 0;
}
static int ring_buffer__consume(struct ring_buffer *r)
{
    size_t count = 0;
    while (r->left) {
        unsigned char wire[48];
        size_t n = raw_boundary(r->driver, 0, r->ctx->dest, wire);
        --r->left; ++count;
        int rc = receive_ring(r->ctx, wire, n);
        if (rc < 0) return rc;
    }
    return (int)count;
}
static int raw_drain(void *ctx)
{ return linux_drain(((struct tb_driver *)ctx)->transport->platform); }

static struct pgwt_daemon *raw_daemon;
enum raw_site { RAW_DRAIN_ENTRY, RAW_DRAIN_SLICE, RAW_READER, RAW_PERF,
                RAW_HINT, RAW_RING, RAW_CONSUME, RAW_RING_CAP, RAW_WAIT };
static bool raw_edge(enum raw_site site)
{
    struct tb_driver d; TB(tb_setup(&d)); TB(tb_begin(&d));
    struct pgwt_daemon *daemon = calloc(1, sizeof(*daemon)); raw_daemon=daemon; TB(daemon);
    daemon->exact_perf = true; daemon->percpu_transport = d.transport;
    daemon->merge_capture = d.capture;
    struct perf_buffer p0 = {.driver=&d}, p1 = {.driver=&d,.cpu=1};
    struct perf_buffer h0 = {.driver=&d,.hint=true}, h1 = {.driver=&d,.cpu=1,.hint=true};
    struct perf_buffer *perf[] = {&p0, &p1}, *hints[] = {&h0, &h1};
    struct linux_transport l = {.d=daemon,.ncpu=2,.perf=perf,.hints=hints};
    l.ring_ctx[0] = (struct ring_context){.d=daemon,.dest=PGWT_WIRE_DEST_E};
    l.ring_ctx[1] = (struct ring_context){.d=daemon,.dest=PGWT_WIRE_DEST_L};
    struct ring_buffer e = {.driver=&d,.ctx=&l.ring_ctx[0]};
    struct ring_buffer life = {.driver=&d,.ctx=&l.ring_ctx[1]};
    l.rings[0] = &e; l.rings[1] = &life;
    d.transport->platform = &l; d.transport->io.drain = raw_drain;
    /* Counter change exists before the raw yield, with the final CPU ack
     * still pending. The yield cannot certify anything or cache this away. */
    d.snapshot.value[11] = 1;
    bool empty = true;
    unsigned drains = d.drains;
    pgwt_transport_begin_pass(d.transport, 0);
    uint64_t started = d.now;
    if (site == RAW_WAIT) {
        raw_wait_driver=&d;
        TB(linux_wait(&l)==0 && raw_wait_timeout==1 && d.now-started==1000000);
        d.now=started+PGWT_TRANSPORT_PASS_NS-500000;
        TB(linux_wait(&l)==0 && raw_wait_timeout==0 && d.now-started==PGWT_TRANSPORT_PASS_NS-500000);
        TB(d.transport->stats.wait_budget_yields==1);
    } else if (site == RAW_DRAIN_ENTRY || site == RAW_READER || site == RAW_CONSUME) {
        e.left = 2; d.now += PGWT_TRANSPORT_PASS_NS;
        if (site == RAW_DRAIN_ENTRY) {
            TB(bounded_drain(d.transport)==0 && d.drains==drains);
            TB(d.transport->stats.drain_budget_yields==1);
        } else if (site == RAW_READER) {
            TB(consume_all(&l,&empty)==0 && !empty && l.next_reader==0 && e.left==2);
            TB(d.transport->stats.reader_budget_yields==1);
        } else {
            TB(linux_consume_io(&l,PGWT_WIRE_DEST_E,true)==-EAGAIN && e.left==2);
            TB(linux_consume_io(&l,PGWT_WIRE_DEST_L,true)==0);
            TB(d.transport->stats.consume_budget_yields==2);
        }
    } else if (site == RAW_RING_CAP) {
        e.left = 4097;
        TB(linux_consume_io(&l,PGWT_WIRE_DEST_E,true)==-EAGAIN && e.left==1);
        TB(d.transport->stats.ring_record_cap_yields==1);
    } else if (site == RAW_RING) {
        e.left = 2; l.next_reader = 4;
        d.received_cost_ns = PGWT_TRANSPORT_PASS_NS;
        TB(consume_all(&l,&empty)==0 && !empty && e.left==1);
        TB(d.transport->stats.ring_budget_yields==1);
    } else if (site == RAW_HINT) {
        h0.left = 3; d.received_cost_ns = PGWT_TRANSPORT_PASS_NS;
        TB(consume_all(&l,&empty)==0 && !empty && h0.left==2);
        TB(d.transport->stats.hint_budget_yields==1);
    } else if (site == RAW_PERF) {
        p0.left = 3; d.received_cost_ns = PGWT_TRANSPORT_PASS_NS;
        TB(consume_all(&l,&empty)==0 && !empty && p0.left==2);
        TB(d.transport->stats.perf_budget_yields==1);
    } else {
        p0.left = 3; d.received_cost_ns = PGWT_TRANSPORT_DRAIN_NS / 2;
        TB(bounded_drain(d.transport)==0 && p0.left==1);
        TB(d.now-started==PGWT_TRANSPORT_DRAIN_NS && !pgwt_transport_pass_expired(d.transport));
        TB(d.transport->stats.drain_slice_yields==1 && d.transport->stats.perf_budget_yields==1 && d.transport->stats.reader_budget_yields==1);
    }
    TB(d.now-started<=PGWT_TRANSPORT_PASS_NS && !d.count);
    /* Resume the unread suffix before delivering every CPU's ack. */
    pgwt_transport_begin_pass(d.transport,0); d.received_cost_ns=0;
    TB(consume_all(&l,&empty)==0 && !p0.left && !h0.left && !e.left);
    TB(tb_acks(&d,2,14));
    TB(pgwt_transport_poll(d.transport,false)==0);
    TB(tb_counter_void(&d));
    d.transport->platform=NULL; free(daemon); raw_daemon=NULL; tb_cleanup(&d); return true;
}
static const struct { const char *name; enum raw_site site; } raw_guards[] = {
    {"drain_entry_edge",RAW_DRAIN_ENTRY}, {"drain_slice_edge",RAW_DRAIN_SLICE},
    {"reader_edge",RAW_READER}, {"perf_edge",RAW_PERF}, {"hint_edge",RAW_HINT},
    {"ring_edge",RAW_RING}, {"consume_edge",RAW_CONSUME}, {"ring_cap_edge",RAW_RING_CAP},
    {"wait_edge",RAW_WAIT}
};
static int raw_main(int argc, char **argv)
{
    for(size_t i=0;i<sizeof(raw_guards)/sizeof(raw_guards[0]);++i) {
        if(argc==3 && (!strcmp(argv[1],"--guard")) && strcmp(argv[2],raw_guards[i].name)) continue;
        tb_guard=raw_guards[i].name;
        printf("REACHED %s\n",tb_guard); fflush(stdout);
        if(!raw_edge(raw_guards[i].site)) return 1;
        printf("transport %s PASS (expiry + pending counter => VOID counter_change)\n",tb_guard);
        if(argc==3) return 0;
    }
    if(argc==3) return 2;
    puts("raw budget guards: 9/9 PASS"); return 0;
}

int main(int argc, char **argv)
{
    int rc=raw_main(argc,argv);
    free(raw_daemon);
    /* Mutants fail inside a guard: release its fixture before sanitizer exit. */
    for(size_t i=0;i<64;++i) {
        pgwt_transport_destroy(tb_live[i].transport); pgwt_capture_destroy(tb_live[i].capture);
        if(live_cores[i])pgwt_merge_destroy(live_cores[i]);
        if(live_adapters[i])ma_destroy(live_adapters[i]);
    }
    return rc;
}
