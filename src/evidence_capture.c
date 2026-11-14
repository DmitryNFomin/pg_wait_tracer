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

struct pgwt_evidence_capture {
    char *directory;
    char boot[256], identity[33];
    uint64_t wall, monotonic;
    unsigned pg_version;
    size_t cap, count, limit;
    pgwt_evidence_record *records; /* last slot reserved for the closing loss */
    bool refused;
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
    struct stat st;
    if (!directory || stat(directory, &st) || !S_ISDIR(st.st_mode)) {
        refuse(NULL, "trace directory unavailable");
        return NULL;
    }
    struct pgwt_evidence_capture *c = calloc(1, sizeof(*c));
    if (!c) {
        refuse(NULL, "allocation failed");
        return NULL;
    }
    c->cap = cap;
    c->limit = (cap - RESERVED_BYTES) / sizeof(*c->records) - 1;
    c->directory = strdup(directory);
    c->records = calloc(c->limit + 1, sizeof(*c->records));
    c->pg_version = pg_version;
    struct timespec wall, monotonic;
    if (!c->directory || !c->records || boot_identity(c->boot) ||
        capture_identity(c->identity) || clock_gettime(CLOCK_REALTIME, &wall) ||
        clock_gettime(CLOCK_MONOTONIC, &monotonic)) {
        refuse(c, "cannot initialise bounded capture or clock identity");
        free(c->directory);
        free(c->records);
        free(c);
        return NULL;
    }
    c->wall = ns(wall);
    c->monotonic = ns(monotonic);
    return c;
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
    pgwt_evidence_record *generation = NULL;
    for (size_t i = 0; i < c->count; ++i) {
        pgwt_evidence_record *r = &c->records[i];
        if (r->pid == ev.pid && r->fork <= ev.timestamp &&
            (!generation || r->fork > generation->fork))
            generation = r;
    }
    if (type == PGWT_LIFECYCLE_EXIT) {
        if (!generation)
            return; /* Pre-existing/unknown generation: prove nothing. */
        if (generation->has_exit && generation->exit != ev.timestamp) {
            refuse(c, "contradictory lifecycle exit");
            return;
        }
        generation->has_exit = true;
        generation->exit = generation->t = ev.timestamp;
        return;
    }
    if (generation && generation->fork == ev.timestamp)
        return; /* A repeated observation supplies no additional fact. */
    if (c->count == c->limit) {
        refuse(c, "size cap reached");
        return;
    }
    c->records[c->count++] = (pgwt_evidence_record){
        .kind = PGWT_EVIDENCE_LIFE, .t = ev.timestamp, .pid = ev.pid,
        .has_fork = true, .fork = ev.timestamp,
        .start = ev.timestamp, .start_known = true,
    };
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
        refuse(c, "trace directory unwritable");
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
    if (c->refused)
        goto done;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        refuse(c, "closing clock read failed");
        goto done;
    }
    pgwt_evidence_record *closing = &c->records[c->count];
    *closing = (pgwt_evidence_record){
        .kind = PGWT_EVIDENCE_VOID, .t = ns(now),
        .void_reason = {"unverified shutdown", sizeof("unverified shutdown") - 1},
    };
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
        .records = c->records, .record_count = c->count + 1,
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
    free(c->records);
    free(c->directory);
    free(c);
    return rc;
}
