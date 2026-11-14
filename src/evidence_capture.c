#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "evidence_capture.h"
#include "core/evidence_codec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#define DEFAULT_CAP (8U * 1024U * 1024U)
#define MAX_CAP (64U * 1024U * 1024U)
#define RESERVED_BYTES 1024U
#define LEAF UINT32_C(0x80000000)

/* Only these two kinds enter the codec. Never accept a codec kind as input. */
enum capture_kind { CAPTURE_LIFE, CAPTURE_VOID };
struct lifetime { uint64_t fork, exit; uint32_t pid; bool has_exit; };
struct pid_entry { uint32_t pid, root; };
struct fork_branch { uint64_t bit; uint32_t child[2], maximum; };

static pgwt_evidence_record record(enum capture_kind kind)
{
    pgwt_evidence_record r = {0};
    switch (kind) {
    case CAPTURE_LIFE: r.kind = PGWT_EVIDENCE_LIFE; break;
    case CAPTURE_VOID: r.kind = PGWT_EVIDENCE_VOID; break;
    default: abort();
    }
    return r;
}

struct pgwt_evidence_capture {
    char *directory;
    char boot[256], identity[33];
    uint64_t wall, monotonic;
    unsigned pg_version;
    size_t cap, count, limit;
    struct lifetime *lives;
    struct pid_entry *pids;
    struct fork_branch *branches;
    size_t pid_slots, branch_count;
    bool refused, lifecycle_conflict;
};

static void refuse(struct pgwt_evidence_capture *c, const char *why)
{
    if (!c || !c->refused)
        fprintf(stderr, "EVIDENCE CAPTURE REFUSED: %s; no v5 file published\n", why);
    if (c)
        c->refused = true;
}

static uint64_t ns(struct timespec t)
{
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static int boot_identity(char boot[256])
{
#ifdef __APPLE__
    size_t size = 256;
    if (sysctlbyname("kern.bootsessionuuid", boot, &size, NULL, 0) != 0)
        return -1;
    boot[255] = 0;
#else
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!f)
        return -1;
    bool ok = fgets(boot, 256, f) != NULL;
    int rc = fclose(f);
    if (!ok || rc)
        return -1;
#endif
    boot[strcspn(boot, "\r\n")] = 0;
    return boot[0] ? 0 : -1;
}

static int capture_identity(char identity[33])
{
    unsigned char bytes[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    int rc = fclose(f);
    if (n != sizeof(bytes) || rc)
        return -1;
    for (size_t i = 0; i < sizeof(bytes); ++i)
        snprintf(identity + 2*i, 3, "%02x", bytes[i]);
    return 0;
}

struct pgwt_evidence_capture *pgwt_evidence_capture_init(const char *directory,
                                                        unsigned pg_version)
{
    const char *enabled = getenv("PGWT_EVIDENCE_CAPTURE");
    if (!enabled || !enabled[0] || !strcmp(enabled, "0"))
        return NULL;
    if (strcmp(enabled, "1")) {
        refuse(NULL, "PGWT_EVIDENCE_CAPTURE must be 1");
        return NULL;
    }
    if (!directory || !directory[0]) {
        fprintf(stderr, "EVIDENCE CAPTURE OFF: no trace directory configured\n");
        return NULL;
    }
    size_t cap = DEFAULT_CAP;
    const char *bound = getenv("PGWT_EVIDENCE_CAPTURE_MAX_BYTES");
    if (bound) {
        cap = 0;
        for (const char *p = bound; *p; ++p) {
            if (*p < '0' || *p > '9' || cap > (MAX_CAP - (size_t)(*p - '0')) / 10) {
                refuse(NULL, "invalid size cap (maximum 64 MiB)");
                return NULL;
            }
            cap = cap * 10 + (size_t)(*p - '0');
        }
    }
    if (cap < RESERVED_BYTES + 2*sizeof(pgwt_evidence_record)) {
        refuse(NULL, "size cap reached");
        return NULL;
    }
    const char *output = getenv("PGWT_EVIDENCE_CAPTURE_DIR");
    if (output)
        directory = output;
    struct stat st;
    if (stat(directory, &st) || !S_ISDIR(st.st_mode)) {
        refuse(NULL, "evidence output directory unavailable");
        return NULL;
    }
    struct pgwt_evidence_capture *c = calloc(1, sizeof(*c));
    if (!c) {
        refuse(NULL, "allocation failed");
        return NULL;
    }
    c->cap = cap;
    c->limit = (cap - RESERVED_BYTES) / sizeof(pgwt_evidence_record) - 1;
    c->pid_slots = 1;
    while (c->pid_slots < 2*c->limit)
        c->pid_slots *= 2;
    c->directory = strdup(directory);
    /* No allocation or resizing inside the lifecycle callback. Even at the
     * smallest cap, lives + branches + hash slots fit within the record cap.
     * The logical codec array and encoded bytes are separately capped. */
    c->lives = calloc(c->limit, sizeof(*c->lives));
    c->pids = calloc(c->pid_slots, sizeof(*c->pids));
    c->branches = calloc(c->limit, sizeof(*c->branches));
    c->pg_version = pg_version;
    struct timespec wall, monotonic;
    if (!c->directory || !c->lives || !c->pids || !c->branches || boot_identity(c->boot) ||
        capture_identity(c->identity) || clock_gettime(CLOCK_REALTIME, &wall) ||
        clock_gettime(CLOCK_MONOTONIC, &monotonic)) {
        refuse(c, "cannot initialise bounded capture or clock identity");
        free(c->directory);
        free(c->lives);
        free(c->pids);
        free(c->branches);
        free(c);
        return NULL;
    }
    c->wall = ns(wall);
    c->monotonic = ns(monotonic);
    return c;
}

/* Open-addressed pid index at <= 1/2 load, with a mixed uint32 hash. A pid's
 * compressed binary radix tree keeps its fork history, so delayed EXITs and
 * repeated old FORKs retain the scan implementation's timestamp semantics.
 * Each radix path has at most 64 branches, independent of generation count.
 * Hash lookup is expected O(1); FORK/EXIT do bounded uint64 work after it. */
static struct pid_entry *pid_entry(struct pgwt_evidence_capture *c, uint32_t pid)
{
    uint32_t h = pid;
    h ^= h >> 16; h *= UINT32_C(0x7feb352d);
    h ^= h >> 15; h *= UINT32_C(0x846ca68b);
    h ^= h >> 16;
    size_t slot = h & (c->pid_slots - 1);
    while (c->pids[slot].pid && c->pids[slot].pid != pid)
        slot = (slot + 1) & (c->pid_slots - 1);
    return &c->pids[slot];
}

static uint32_t maximum(struct pgwt_evidence_capture *c, uint32_t root)
{
    return root & LEAF ? root : c->branches[root - 1].maximum;
}

static uint64_t fork_time(struct pgwt_evidence_capture *c, uint32_t leaf)
{
    return c->lives[(leaf & ~LEAF) - 1].fork;
}

static uint32_t predecessor(struct pgwt_evidence_capture *c, uint32_t root, uint64_t t)
{
    uint32_t candidate = 0;
    while (root) {
        uint32_t last = maximum(c, root);
        if (fork_time(c, last) <= t)
            return last;
        if (root & LEAF)
            break;
        struct fork_branch *b = &c->branches[root - 1];
        last = maximum(c, b->child[0]);
        if (fork_time(c, last) <= t) {
            candidate = last;
            root = b->child[1];
        } else {
            root = b->child[0];
        }
    }
    return candidate;
}

static void insert(struct pgwt_evidence_capture *c, struct pid_entry *p, uint32_t leaf)
{
    if (!p->root) {
        p->root = leaf;
        return;
    }
    uint64_t t = fork_time(c, leaf);
    uint32_t near = p->root;
    while (!(near & LEAF)) {
        struct fork_branch *b = &c->branches[near - 1];
        near = b->child[(t & b->bit) != 0];
    }
    uint64_t difference = t ^ fork_time(c, near);
    uint64_t bit = UINT64_C(1) << 63;
    while (!(difference & bit))
        bit >>= 1; /* Duplicate keys were removed by predecessor(). */
    uint32_t *link = &p->root;
    while (!(*link & LEAF) && c->branches[*link - 1].bit > bit) {
        struct fork_branch *b = &c->branches[*link - 1];
        if (t > fork_time(c, b->maximum))
            b->maximum = leaf;
        link = &b->child[(t & b->bit) != 0];
    }
    struct fork_branch *b = &c->branches[c->branch_count++];
    unsigned side = (t & bit) != 0;
    b->bit = bit;
    b->child[side] = leaf;
    b->child[!side] = *link;
    uint32_t last = maximum(c, *link);
    b->maximum = t > fork_time(c, last) ? leaf : last;
    *link = (uint32_t)c->branch_count;
}

void pgwt_evidence_capture_observe(struct pgwt_evidence_capture *c,
                                  const void *payload, size_t size)
{
    if (!c || c->refused)
        return;
    uint32_t type;
    if (!payload || size < sizeof(type)) {
        refuse(c, "short lifecycle payload");
        return;
    }
    memcpy(&type, payload, sizeof(type));
    if (type != PGWT_LIFECYCLE_FORK && type != PGWT_LIFECYCLE_EXIT)
        return;
    struct pgwt_lifecycle_event ev;
    if (size < sizeof(ev)) {
        refuse(c, "short lifecycle payload");
        return;
    }
    memcpy(&ev, payload, sizeof(ev));
    if (!ev.pid) {
        refuse(c, "invalid lifecycle pid");
        return;
    }
    struct pid_entry *p = pid_entry(c, ev.pid);
    uint32_t previous = predecessor(c, p->root, ev.timestamp);
    struct lifetime *generation = previous ? &c->lives[(previous & ~LEAF) - 1] : NULL;
    if (type == PGWT_LIFECYCLE_EXIT) {
        if (!generation)
            return; /* Pre-existing/unknown generation: prove nothing. */
        if (generation->has_exit && generation->exit != ev.timestamp) {
            c->lifecycle_conflict = true;
            return; /* At close, loss may require discarding every LIFE. */
        }
        generation->has_exit = true;
        generation->exit = ev.timestamp;
        return;
    }
    if (generation && generation->fork == ev.timestamp)
        return; /* A repeated observation supplies no additional fact. */
    if (c->count == c->limit) {
        refuse(c, "size cap reached");
        return;
    }
    c->lives[c->count++] = (struct lifetime){.pid = ev.pid, .fork = ev.timestamp};
    p->pid = ev.pid;
    insert(c, p, LEAF | (uint32_t)c->count);
}

/* Publish only complete encoder output, without replacing an older capture.
 * Every failed write/flush/close/link removes the unpublished temporary file. */
static int publish(struct pgwt_evidence_capture *c, pgwt_codec_bytes bytes)
{
    size_t length = strlen(c->directory) + 96;
    char *temporary = malloc(length), *destination = malloc(length);
    if (!temporary || !destination) {
        free(temporary);
        free(destination);
        refuse(c, "publication allocation failed");
        return -1;
    }
    snprintf(temporary, length, "%s/.evidence-%s-XXXXXX", c->directory, c->identity);
    snprintf(destination, length, "%s/evidence-%s.trace", c->directory, c->identity);
    int fd = mkstemp(temporary);
    int rc = -1;
    if (fd < 0) {
        refuse(c, "evidence output directory unwritable");
        goto done;
    }
    struct stat st;
    bool ok = !stat(c->directory, &st) && !fchown(fd, (uid_t)-1, st.st_gid) &&
              !fchmod(fd, 0640);
    size_t offset = 0;
    while (ok && offset < bytes.size) {
        ssize_t n = write(fd, bytes.data + offset, bytes.size - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            ok = false;
        else
            offset += (size_t)n;
    }
    if (ok && fsync(fd))
        ok = false;
    if (close(fd))
        ok = false;
    if (ok && link(temporary, destination) == 0)
        rc = 0;
    if (unlink(temporary) && !rc) {
        unlink(destination);
        rc = -1;
    }
    if (rc)
        refuse(c, "file publication failed");
done:
    free(temporary);
    free(destination);
    return rc;
}

int pgwt_evidence_capture_close(struct pgwt_evidence_capture **capture,
                                struct pgwt_counter_vector final)
{
    if (!capture || !*capture)
        return 0;
    struct pgwt_evidence_capture *c = *capture;
    *capture = NULL;
    int rc = -1;
    pgwt_codec_bytes bytes = {0};
    pgwt_evidence_record *records = NULL;
    if (c->refused)
        goto done;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        refuse(c, "closing clock read failed");
        goto done;
    }
    /* A lost EXIT followed by a lost FORK can relabel a later EXIT. No
     * per-pid loss attribution exists, so discard every potentially corrupt
     * LIFE rather than presenting that pairing as an observed lifetime. */
    if (!final.valid || !final.components[PGWT_C_LIFECYCLE_FORK_DROP].valid ||
        !final.components[PGWT_C_LIFECYCLE_EXIT_DROP].valid ||
        final.components[PGWT_C_LIFECYCLE_FORK_DROP].value ||
        final.components[PGWT_C_LIFECYCLE_EXIT_DROP].value) {
        fprintf(stderr, "EVIDENCE CAPTURE: LIFE omitted: lifecycle fork/exit loss "
                        "counters nonzero or unreadable\n");
        c->count = 0;
    } else if (c->lifecycle_conflict) {
        refuse(c, "contradictory lifecycle exit");
        goto done;
    }
    records = calloc(c->count + 1, sizeof(*records));
    if (!records) {
        refuse(c, "closing allocation failed");
        goto done;
    }
    for (size_t i = 0; i < c->count; ++i) {
        struct lifetime life = c->lives[i];
        records[i] = record(CAPTURE_LIFE);
        records[i].pid = life.pid;
        records[i].has_fork = records[i].start_known = true;
        records[i].fork = records[i].start = life.fork;
        records[i].has_exit = life.has_exit;
        records[i].exit = life.exit;
        records[i].t = life.has_exit ? life.exit : life.fork;
    }
    pgwt_evidence_record *closing = &records[c->count];
    *closing = record(CAPTURE_VOID);
    closing->t = ns(now);
    closing->void_reason = (pgwt_core_event){"unverified shutdown", sizeof("unverified shutdown") - 1};
    closing->counters.known = final.valid;
    for (size_t i = 0; i < PGWT_COUNTER_COMPONENTS; ++i) {
        closing->counters.known &= final.components[i].valid;
        closing->counters.value[i] = final.components[i].value;
    }
    pgwt_evidence_file file = {
        .version = 5, .present = true, .qualified = false,
        .boot = {c->boot, strlen(c->boot)},
        .capture = {c->identity, strlen(c->identity)},
        /* No initial counter read was made: baseline is explicitly unknown. */
        .records = records, .record_count = c->count + 1,
    };
    pgwt_codec_descriptor descriptor = {
        .pg_version = c->pg_version, .start_time_ns = c->wall,
        .clock_offset_ns = c->monotonic, .footer = true,
    };
    const char *reason = NULL;
    if (pgwt_codec_encode(&file, &descriptor, NULL, &bytes, &reason)) {
        refuse(c, reason ? reason : "encoding failed");
        goto done;
    }
    if (bytes.size > c->cap) {
        refuse(c, "size cap reached");
        goto done;
    }
    rc = publish(c, bytes);
done:
    pgwt_codec_bytes_free(&bytes);
    free(records);
    free(c->lives);
    free(c->pids);
    free(c->branches);
    free(c->directory);
    free(c);
    return rc;
}
