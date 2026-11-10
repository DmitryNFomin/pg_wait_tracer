/* Phase-marker evidence. No attach outcomes or current-host fallback. */
#include "phase_capability.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *phase_names[] = {"execute", "plan"};

static struct pgwt_phase_value value(enum pgwt_phase_state state, const char *reason)
{
    struct pgwt_phase_value v = {.state = state};
    snprintf(v.reason, sizeof(v.reason), "%s", reason);
    return v;
}

const char *pgwt_phase_state_name(enum pgwt_phase_state state)
{
    switch (state) {
    case PGWT_PHASE_AVAILABLE: return "available";
    case PGWT_PHASE_UNAVAILABLE: return "unavailable";
    case PGWT_PHASE_MIXED: return "mixed";
    default: return "unknown";
    }
}

struct pgwt_phase_value pgwt_phase_classify(enum pgwt_phase_fact kernel,
                                           enum pgwt_phase_fact binary)
{
    /* A failed measurement never becomes a negative capability claim, even
     * when the other fact independently proves a limitation. */
    if (kernel != PGWT_PHASE_PRESENT && kernel != PGWT_PHASE_ABSENT)
        return value(PGWT_PHASE_UNKNOWN, "kernel_probe_failed");
    if (binary != PGWT_PHASE_PRESENT && binary != PGWT_PHASE_ABSENT)
        return value(PGWT_PHASE_UNKNOWN, "binary_probe_failed");
    if (kernel == PGWT_PHASE_ABSENT)
        return value(PGWT_PHASE_UNAVAILABLE, "kernel_no_ref_ctr");
    if (binary == PGWT_PHASE_ABSENT)
        return value(PGWT_PHASE_UNAVAILABLE, "binary_no_sdt");
    return value(PGWT_PHASE_AVAILABLE, "none");
}

static int valid_value(const struct pgwt_phase_value *v)
{
    size_t n = strnlen(v->reason, sizeof(v->reason));
    if (!n || n == sizeof(v->reason) || v->state < PGWT_PHASE_UNKNOWN ||
        v->state > PGWT_PHASE_UNAVAILABLE)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((v->reason[i] < 'a' || v->reason[i] > 'z') &&
            (v->reason[i] < '0' || v->reason[i] > '9') && v->reason[i] != '_')
            return 0;
    return v->state == PGWT_PHASE_AVAILABLE ? !strcmp(v->reason, "none") :
        strcmp(v->reason, "none") != 0;
}

static int make_path(char *path, size_t n, const char *dir)
{
    if (!dir) { errno = EINVAL; return -1; }
    int rc = snprintf(path, n, "%s/%s", dir, PGWT_PHASE_CAPABILITY_FILE);
    if (rc < 0 || (size_t)rc >= n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int pgwt_phase_append(const char *dir, gid_t gid,
                      const struct pgwt_phase_record *record, uint64_t *errors)
{
    char path[4096], stamp[32];
    int fd = -1, rc = -1, saved = EINVAL;
    cJSON *obj = NULL;
    char *json = NULL, *line = NULL;
    struct stat st;
    if (make_path(path, sizeof(path), dir) != 0) { saved = errno; goto done; }
    if (!record || !memchr(record->kernel_release, 0, sizeof(record->kernel_release)) ||
        !record->kernel_release[0])
        goto done;
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++)
        if (!valid_value(&record->phase[p])) goto done;
    saved = ENOMEM;
    obj = cJSON_CreateObject();
    snprintf(stamp, sizeof(stamp), "%" PRIu64, record->start_wall_ns);
    if (!obj || !cJSON_AddNumberToObject(obj, "version", PGWT_PHASE_CAPABILITY_VERSION) ||
        !cJSON_AddStringToObject(obj, "start_wall_ns", stamp) ||
        !cJSON_AddStringToObject(obj, "kernel_release", record->kernel_release))
        goto done;
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++) {
        cJSON *phase = cJSON_AddObjectToObject(obj, phase_names[p]);
        if (!phase || !cJSON_AddStringToObject(phase, "state", pgwt_phase_state_name(record->phase[p].state)) ||
            !cJSON_AddStringToObject(phase, "reason", record->phase[p].reason))
            goto done;
    }
    json = cJSON_PrintUnformatted(obj);
    if (!json) goto done;
    size_t len = strlen(json);
    line = malloc(len + 3);
    if (!line) goto done;
    fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640);
    if (fd < 0) { saved = errno; goto done; }
    if (fstat(fd, &st) != 0) { saved = errno; goto done; }
    if (!S_ISREG(st.st_mode)) { saved = EINVAL; goto done; }
    if ((gid != (gid_t)-1 && fchown(fd, (uid_t)-1, gid) != 0) || fchmod(fd, 0640) != 0) {
        saved = errno; goto done;
    }
    /* Isolate a previous crash's torn append without rewriting history.
     * A now-complete malformed line remains untrusted by the reader. */
    size_t prefix = 0;
    if (st.st_size > 0) {
        char last;
        if (pread(fd, &last, 1, st.st_size - 1) != 1) { saved = EIO; goto done; }
        if (last != '\n') line[prefix++] = '\n';
    }
    memcpy(line + prefix, json, len); line[prefix + len] = '\n';
    size_t bytes = prefix + len + 1;
    /* One append syscall: a partial write is a counted failure, never a
     * retry that might interleave fragments. fsync also detects delayed IO. */
    ssize_t wrote = write(fd, line, bytes);
    if (wrote != (ssize_t)bytes) { saved = wrote < 0 ? errno : EIO; goto done; }
    if (fsync(fd) != 0) { saved = errno; goto done; }
    rc = 0;
 done:
    if (fd >= 0 && close(fd) != 0 && rc == 0) { saved = errno; rc = -1; }
    cJSON_Delete(obj); cJSON_free(json); free(line);
    if (rc != 0) {
        if (errors) ++*errors;
        fprintf(stderr, "WARN: phase_capabilities.jsonl append failed: %s (write_failures=%" PRIu64 ")\n",
                strerror(saved), errors ? *errors : 0);
        errno = saved;
    }
    return rc;
}

void pgwt_phase_window_free(struct pgwt_phase_window *out)
{
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++) free(out->phase[p].ranges);
    memset(out, 0, sizeof(*out));
}

static int unknown_window(struct pgwt_phase_window *out, const char *why)
{
    pgwt_phase_window_free(out);
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++)
        out->phase[p].value = value(PGWT_PHASE_UNKNOWN, why);
    return -1;
}

static int same(const struct pgwt_phase_value *a, const struct pgwt_phase_value *b)
{
    return a->state == b->state && !strcmp(a->reason, b->reason);
}

static int add_range(struct pgwt_phase_result *out, uint64_t lo, uint64_t hi,
                     struct pgwt_phase_value v)
{
    if (lo >= hi) return 0;
    if (out->n_ranges && same(&out->ranges[out->n_ranges - 1].value, &v)) {
        out->ranges[out->n_ranges - 1].to_ns = hi;
        return 0;
    }
    if (out->n_ranges >= SIZE_MAX / sizeof(*out->ranges) - 1) return -1;
    void *r = realloc(out->ranges, (out->n_ranges + 1) * sizeof(*out->ranges));
    if (!r) return -1;
    out->ranges = r;
    out->ranges[out->n_ranges++] = (struct pgwt_phase_range){lo, hi, v};
    return 0;
}

static int interval(struct pgwt_phase_window *out, const struct pgwt_phase_record *r,
                    uint64_t end, uint64_t from, uint64_t to)
{
    uint64_t lo = r->start_wall_ns > from ? r->start_wall_ns : from;
    uint64_t hi = end < to ? end : to;
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++)
        if (add_range(&out->phase[p], lo, hi, r->phase[p]) != 0) return -1;
    return 0;
}

/* 0 valid, -1 malformed, -2 unsupported version. Decimal strings preserve
 * nanoseconds exactly, including values beyond JSON double's integer range. */
static int parse_record(const char *line, struct pgwt_phase_record *r)
{
    cJSON *obj = cJSON_ParseWithOpts(line, NULL, 1);
    if (!obj) return -1;
    int rc = -1;
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(obj, "version");
    if (!cJSON_IsNumber(ver)) goto done;
    if (ver->valuedouble != PGWT_PHASE_CAPABILITY_VERSION) { rc = -2; goto done; }
    cJSON *stamp = cJSON_GetObjectItemCaseSensitive(obj, "start_wall_ns");
    cJSON *kernel = cJSON_GetObjectItemCaseSensitive(obj, "kernel_release");
    if (!cJSON_IsString(stamp) || !stamp->valuestring[0] || !cJSON_IsString(kernel) ||
        !kernel->valuestring[0] || strlen(kernel->valuestring) >= sizeof(r->kernel_release))
        goto done;
    for (const char *s = stamp->valuestring; *s; s++) if (*s < '0' || *s > '9') goto done;
    errno = 0;
    r->start_wall_ns = strtoull(stamp->valuestring, NULL, 10);
    if (errno == ERANGE) goto done;
    strcpy(r->kernel_release, kernel->valuestring);
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++) {
        cJSON *ph = cJSON_GetObjectItemCaseSensitive(obj, phase_names[p]);
        cJSON *state = cJSON_GetObjectItemCaseSensitive(ph, "state");
        cJSON *reason = cJSON_GetObjectItemCaseSensitive(ph, "reason");
        if (!cJSON_IsString(state) || !cJSON_IsString(reason) ||
            strlen(reason->valuestring) >= sizeof(r->phase[p].reason)) goto done;
        int s;
        for (s = PGWT_PHASE_UNKNOWN; s <= PGWT_PHASE_UNAVAILABLE; s++)
            if (!strcmp(state->valuestring, pgwt_phase_state_name(s))) break;
        r->phase[p] = value(s, reason->valuestring);
        if (!valid_value(&r->phase[p])) goto done;
    }
    rc = 0;
 done:
    cJSON_Delete(obj);
    return rc;
}

/* Bounded storage, with explicit lengths so an embedded NUL cannot hide a
 * completed malformed record. Only a final line without LF is uncommitted.
 * Return 1 for a complete line, 0 for EOF/torn tail, -1 for invalid/read error. */
static int read_line(FILE *f, char *line, size_t cap)
{
    size_t n = 0;
    int invalid = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) return ferror(f) ? -1 : 0;
        if (c == '\n') {
            if (invalid) return -1;
            line[n] = 0;
            return 1;
        }
        if (c == 0) invalid = 1;
        if (n + 1 < cap) line[n++] = (char)c;
        else invalid = 1;
    }
}

int pgwt_phase_read_window(const char *dir, uint64_t from, uint64_t to,
                           struct pgwt_phase_window *out)
{
    char path[4096], line[4096];
    struct pgwt_phase_record prev = {0};
    int have_prev = 0;
    memset(out, 0, sizeof(*out));
    if (from >= to) return unknown_window(out, "invalid_window");
    if (make_path(path, sizeof(path), dir) != 0) return unknown_window(out, "unreadable_file");
    FILE *f = fopen(path, "r");
    if (!f) return unknown_window(out, errno == ENOENT ? "no_file" : "unreadable_file");
    const char *failure = NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) failure = "unreadable_file";
    while (!failure) {
        int got_line = read_line(f, line, sizeof(line));
        if (got_line == 0) break;
        if (got_line < 0) { failure = "invalid_record"; break; }
        if (line[0] == 0) continue;
        struct pgwt_phase_record next = {0};
        int parsed = parse_record(line, &next);
        if (parsed != 0) { failure = parsed == -2 ? "unsupported_version" : "invalid_record"; break; }
        if (have_prev && next.start_wall_ns < prev.start_wall_ns) { failure = "unordered_records"; break; }
        if (have_prev) {
            if (interval(out, &prev, next.start_wall_ns, from, to) != 0) failure = "allocation_failed";
        } else if (from < next.start_wall_ns) {
            struct pgwt_phase_record unknown = {.start_wall_ns = from};
            for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++)
                unknown.phase[p] = value(PGWT_PHASE_UNKNOWN, "before_first_record");
            if (interval(out, &unknown, next.start_wall_ns, from, to) != 0) failure = "allocation_failed";
        }
        prev = next; have_prev = 1;
    }
    if (ferror(f)) failure = "unreadable_file";
    if (fclose(f) != 0) failure = "unreadable_file";
    if (!failure && !have_prev) failure = "no_records";
    if (!failure && interval(out, &prev, to, from, to) != 0) failure = "allocation_failed";
    if (failure) return unknown_window(out, failure);
    for (int p = 0; p < PGWT_CAP_PHASE_COUNT; p++) {
        struct pgwt_phase_result *r = &out->phase[p];
        r->value = r->n_ranges ? r->ranges[0].value : value(PGWT_PHASE_UNKNOWN, "no_records");
        for (size_t i = 0; i < r->n_ranges; i++) {
            if (r->ranges[i].value.state == PGWT_PHASE_UNKNOWN) {
                r->value = r->ranges[i].value; break;
            }
            if (!same(&r->value, &r->ranges[i].value)) r->value = value(PGWT_PHASE_MIXED, "mixed_runs");
        }
    }
    return 0;
}
