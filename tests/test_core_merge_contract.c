#include "../src/core/pgwt_merge.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A dedicated JSON parser retains raw number tokens. cJSON's double values
 * cannot represent the contract's UINT64_MAX inputs or expected outputs. */
typedef enum { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } json_kind;
typedef struct json_node {
    json_kind kind;
    char *key, *string;
    size_t key_length, length;
    const char *number;
    bool boolean;
    struct json_node *child, *next;
} json_node;

typedef struct {
    char *text;
    json_node *root;
} json_document;

typedef struct {
    const char *at;
    const char *error;
} json_parser;

static void message(char *out, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (size) vsnprintf(out, size, format, args);
    va_end(args);
}

static void free_node(json_node *n)
{
    while (n) {
        json_node *next = n->next;
        free_node(n->child);
        free(n->key);
        free(n->string);
        free(n);
        n = next;
    }
}

static void free_document(json_document *d)
{
    if (d) {
        free_node(d->root);
        free(d->text);
        free(d);
    }
}

static void whitespace(json_parser *p)
{
    while (*p->at == ' ' || *p->at == '\t' || *p->at == '\r' || *p->at == '\n')
        ++p->at;
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool hex4(json_parser *p, unsigned *value)
{
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)*p->at;
        unsigned x;
        if (c >= '0' && c <= '9') x = c - '0';
        else if (c >= 'a' && c <= 'f') x = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') x = c - 'A' + 10;
        else return false;
        ++p->at;
        *value = *value * 16 + x;
    }
    return true;
}

static void utf8(char *out, size_t *length, unsigned c)
{
    if (c <= 0x7f) out[(*length)++] = (char)c;
    else if (c <= 0x7ff) {
        out[(*length)++] = (char)(0xc0 | (c >> 6));
        out[(*length)++] = (char)(0x80 | (c & 0x3f));
    } else if (c <= 0xffff) {
        out[(*length)++] = (char)(0xe0 | (c >> 12));
        out[(*length)++] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[(*length)++] = (char)(0x80 | (c & 0x3f));
    } else {
        out[(*length)++] = (char)(0xf0 | (c >> 18));
        out[(*length)++] = (char)(0x80 | ((c >> 12) & 0x3f));
        out[(*length)++] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[(*length)++] = (char)(0x80 | (c & 0x3f));
    }
}

static char *parse_string(json_parser *p, size_t *length)
{
    if (*p->at != '"') return NULL;
    ++p->at;
    char *out = malloc(strlen(p->at) + 1);
    if (!out) { p->error = "allocation failed"; return NULL; }
    *length = 0;
    while (*p->at && *p->at != '"') {
        unsigned char c = (unsigned char)*p->at++;
        if (c < 0x20) goto invalid;
        if (c != '\\') { out[(*length)++] = (char)c; continue; }
        char escape = *p->at;
        if (!escape) break;
        ++p->at;
        switch (escape) {
        case '"': case '\\': case '/': out[(*length)++] = escape; break;
        case 'b': out[(*length)++] = '\b'; break;
        case 'f': out[(*length)++] = '\f'; break;
        case 'n': out[(*length)++] = '\n'; break;
        case 'r': out[(*length)++] = '\r'; break;
        case 't': out[(*length)++] = '\t'; break;
        case 'u': {
            unsigned code;
            if (!hex4(p, &code)) goto invalid;
            if (code >= 0xd800 && code <= 0xdbff) {
                unsigned low;
                if (p->at[0] != '\\' || p->at[1] != 'u') goto invalid;
                p->at += 2;
                if (!hex4(p, &low) || low < 0xdc00 || low > 0xdfff) goto invalid;
                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
            } else if (code >= 0xdc00 && code <= 0xdfff) goto invalid;
            utf8(out, length, code);
            break;
        }
        default: goto invalid;
        }
    }
    if (*p->at == '"') {
        ++p->at;
        out[*length] = 0;
        return out;
    }
invalid:
    p->error = "invalid JSON string";
    free(out);
    return NULL;
}

static bool parse_number(json_parser *p, json_node *n)
{
    n->number = p->at;
    if (*p->at == '-') ++p->at;
    if (*p->at == '0') ++p->at;
    else {
        if (*p->at < '1' || *p->at > '9') return false;
        while (digit(*p->at)) ++p->at;
    }
    if (*p->at == '.') {
        ++p->at;
        if (!digit(*p->at)) return false;
        while (digit(*p->at)) ++p->at;
    }
    if (*p->at == 'e' || *p->at == 'E') {
        ++p->at;
        if (*p->at == '+' || *p->at == '-') ++p->at;
        if (!digit(*p->at)) return false;
        while (digit(*p->at)) ++p->at;
    }
    n->length = (size_t)(p->at - n->number);
    return true;
}

static json_node *parse_value(json_parser *p, unsigned depth)
{
    whitespace(p);
    if (depth > 128) { p->error = "JSON nesting limit"; return NULL; }
    json_node *n = calloc(1, sizeof(*n));
    if (!n) { p->error = "allocation failed"; return NULL; }
    char c = *p->at;
    if (c == '"') {
        n->kind = J_STRING;
        n->string = parse_string(p, &n->length);
        if (!n->string) goto invalid;
    } else if (c == '[' || c == '{') {
        n->kind = c == '[' ? J_ARRAY : J_OBJECT;
        char close = c == '[' ? ']' : '}';
        ++p->at;
        whitespace(p);
        json_node **tail = &n->child;
        if (*p->at != close) {
            for (;;) {
                char *key = NULL;
                size_t key_length = 0;
                if (n->kind == J_OBJECT) {
                    key = parse_string(p, &key_length);
                    if (!key) goto invalid;
                    whitespace(p);
                    if (*p->at != ':') { free(key); goto invalid; }
                    ++p->at;
                }
                json_node *child = parse_value(p, depth + 1);
                if (!child) { free(key); goto invalid; }
                child->key = key;
                child->key_length = key_length;
                *tail = child;
                tail = &child->next;
                whitespace(p);
                if (*p->at != ',') break;
                ++p->at;
                whitespace(p);
            }
        }
        if (*p->at != close) goto invalid;
        ++p->at;
    } else if (c == '-' || digit(c)) {
        n->kind = J_NUMBER;
        if (!parse_number(p, n)) goto invalid;
    } else if (strncmp(p->at, "null", 4) == 0) {
        n->kind = J_NULL;
        p->at += 4;
    } else if (strncmp(p->at, "true", 4) == 0) {
        n->kind = J_BOOL;
        n->boolean = true;
        p->at += 4;
    } else if (strncmp(p->at, "false", 5) == 0) {
        n->kind = J_BOOL;
        p->at += 5;
    } else goto invalid;
    return n;
invalid:
    if (!p->error) p->error = "invalid JSON syntax";
    free_node(n);
    return NULL;
}

static json_document *parse_document(char *owned, char *error, size_t size)
{
    json_document *d = calloc(1, sizeof(*d));
    if (!d) { free(owned); message(error, size, "allocation failed"); return NULL; }
    d->text = owned;
    json_parser p = {owned, NULL};
    d->root = parse_value(&p, 0);
    whitespace(&p);
    if (!d->root || *p.at) {
        message(error, size, "%s at byte %zu", p.error ? p.error : "trailing JSON",
                (size_t)(p.at - owned));
        free_document(d);
        return NULL;
    }
    return d;
}

static json_document *read_document(const char *path, char *error, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { message(error, size, "cannot open %s", path); return NULL; }
    size_t used = 0, capacity = 4096;
    char *text = malloc(capacity);
    if (!text) { fclose(f); message(error, size, "allocation failed"); return NULL; }
    for (;;) {
        size_t got = fread(text + used, 1, capacity - used - 1, f);
        used += got;
        if (ferror(f)) { free(text); fclose(f); message(error, size, "read failed"); return NULL; }
        if (feof(f)) break;
        if (capacity > SIZE_MAX / 2) { free(text); fclose(f); message(error, size, "file too large"); return NULL; }
        capacity *= 2;
        char *grown = realloc(text, capacity);
        if (!grown) { free(text); fclose(f); message(error, size, "allocation failed"); return NULL; }
        text = grown;
    }
    fclose(f);
    text[used] = 0;
    if (memchr(text, 0, used)) {
        free(text); message(error, size, "NUL byte in JSON source"); return NULL;
    }
    return parse_document(text, error, size);
}

static bool bytes_equal(const char *a, size_t an, const char *b, size_t bn)
{
    return an == bn && memcmp(a, b, an) == 0;
}

static const json_node *member(const json_node *object, const char *key)
{
    if (!object || object->kind != J_OBJECT) return NULL;
    for (const json_node *n = object->child; n; n = n->next)
        if (bytes_equal(n->key, n->key_length, key, strlen(key))) return n;
    return NULL;
}

static size_t list_length(const json_node *n)
{
    size_t count = 0;
    if (n) for (n = n->child; n; n = n->next) ++count;
    return count;
}

static const json_node *item(const json_node *n, size_t index)
{
    if (!n || n->kind != J_ARRAY) return NULL;
    n = n->child;
    while (n && index--) n = n->next;
    return n;
}

/* contract 2.1, 3, 7.5: tokens, not coerced values. Checked multiply/add
 * accepts UINT64_MAX exactly and rejects overflow, fractions and exponents.
 * -0 is an integer token whose mathematical value is in the unsigned range. */
static bool integer(const json_node *n, uint64_t *out)
{
    if (!n || n->kind != J_NUMBER) return false;
    if (n->length == 2 && memcmp(n->number, "-0", 2) == 0) { *out = 0; return true; }
    uint64_t value = 0;
    for (size_t i = 0; i < n->length; ++i) {
        char c = n->number[i];
        if (!digit(c)) return false;
        unsigned d = (unsigned)(c - '0');
        if (value > (UINT64_MAX - d) / 10) return false;
        value = value * 10 + d;
    }
    *out = value;
    return true;
}




static bool string_is(const json_node *n, const char *s)
{
    return n && n->kind == J_STRING && strcmp(n->string, s) == 0;
}

static const char *string_value(const json_node *n)
{
    if (!n || n->kind != J_STRING) { fputs("fixture: required string\n", stderr); exit(2); }
    return n->string;
}

static uint64_t number(const json_node *n)
{
    uint64_t value;
    if (!integer(n, &value)) { fputs("fixture: required uint64 token\n", stderr); exit(2); }
    return value;
}

static uint64_t optional_number(const json_node *n, uint64_t fallback)
{
    return n ? number(n) : fallback;
}

static bool boolean(const json_node *n)
{
    if (!n || n->kind != J_BOOL) { fputs("fixture: required boolean\n", stderr); exit(2); }
    return n->boolean;
}

typedef struct {
    pgwt_merge_output out;
    char payload[256];
} captured;
typedef struct {
    const json_node *root, *row, *reject;
    captured outputs[256];
    size_t count;
    bool rejected;
} fixture;

static pgwt_merge_counters counters(const fixture *f, const json_node *n)
{
    pgwt_merge_counters c = {0};
    if (!n || string_is(n, "unknown") || n->kind == J_NULL) return c;
    if (n->kind == J_STRING) n = member(member(f->root, "counters"), n->string);
    if (!n || n->kind != J_ARRAY || list_length(n) != PGWT_MERGE_COUNTERS) {
        fputs("fixture: bad counter vector\n", stderr); exit(2);
    }
    c.known = true;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) c.value[i] = number(item(n, i));
    return c;
}

static pgwt_merge_counters named_counters(const fixture *f, const char *name)
{
    return counters(f, member(member(f->root, "counters"), name));
}

static size_t source(const fixture *f, const json_node *n)
{
    const json_node *sources = member(f->root, "sources");
    for (size_t i = 0; i < list_length(sources); ++i)
        if (string_is(n, string_value(item(sources, i)))) return i;
    fputs("fixture: unknown source\n", stderr); exit(2);
}

static const json_node *identity(const fixture *f, const char *who)
{
    const json_node *id = member(member(f->root, "identities"), who);
    if (!id) { fputs("fixture: unknown identity\n", stderr); exit(2); }
    return id;
}

static bool sink(void *context, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    fixture *f = context;
    if (out->kind == PGWT_MERGE_OUT_DATA && f->reject && !f->rejected) {
        const json_node *id = identity(f, string_value(member(f->reject, "who")));
        if (out->record.pid == number(member(id, "pid")) &&
            out->record.g == number(member(id, "g")) &&
            out->record.t == number(member(f->reject, "t"))) {
            f->rejected = true;
            post->d = number(member(f->reject, "d"));
            post->counters = counters(f, member(f->reject, "C"));
            return false;
        }
    }
    if (f->count == sizeof(f->outputs) / sizeof(f->outputs[0])) {
        fputs("fixture: output capture exhausted\n", stderr); exit(2);
    }
    captured *c = &f->outputs[f->count++];
    c->out = *out;
    if (out->kind == PGWT_MERGE_OUT_TEXT || out->kind == PGWT_MERGE_OUT_DATA) {
        if (out->record.payload_size >= sizeof(c->payload)) {
            fputs("fixture: payload too large\n", stderr); exit(2);
        }
        memcpy(c->payload, out->record.payload, out->record.payload_size);
        c->payload[out->record.payload_size] = 0;
        c->out.record.payload = c->payload;
    }
    return true;
}

static pgwt_merge_rank rank_of(const json_node *n)
{
    if (string_is(n, "FORK")) return PGWT_MERGE_FORK;
    if (string_is(n, "INIT")) return PGWT_MERGE_INIT;
    if (string_is(n, "TRANS") || string_is(n, "MARKER")) return PGWT_MERGE_TRANS;
    if (string_is(n, "EXIT")) return PGWT_MERGE_EXIT;
    fputs("fixture: unknown record rank\n", stderr); exit(2);
}

static bool equal_counters(pgwt_merge_counters a, pgwt_merge_counters b)
{
    return a.known == b.known && (!a.known || !memcmp(a.value, b.value, sizeof(a.value)));
}

/* Compare the fixture fields directly: the coordinator does not generate any
 * expected values. A distinct exit code (1) identifies row mismatches for the
 * mutation runner; fixture/setup errors use 2 and sanitizer crashes are errors. */
static bool equal_output(const fixture *f, const captured *a, const json_node *e,
                          const char **field)
{
#define REQUIRE(test, name) do { if (!(test)) { *field = name; return false; } } while (0)
    const json_node *kind = member(e, "kind");
    if (string_is(kind, "DATA")) {
        REQUIRE(a->out.kind == PGWT_MERGE_OUT_DATA, "kind");
        if (strcmp(a->payload, string_value(member(e, "label")))) {
            fprintf(stderr, "label mismatch: input payload '%s', expected '%s'\n",
                    a->payload, string_value(member(e, "label")));
            *field = "label"; return false;
        }
        const json_node *id = identity(f, string_value(member(e, "who")));
        REQUIRE(a->out.record.pid == number(member(id, "pid")) &&
                a->out.record.g == number(member(id, "g")), "who");
        REQUIRE(a->out.record.t == number(member(e, "t")), "t");
    } else if (string_is(kind, "TEXT")) {
        REQUIRE(a->out.kind == PGWT_MERGE_OUT_TEXT, "kind");
        REQUIRE(!strcmp(a->payload, string_value(member(e, "name"))), "name");
    } else if (string_is(kind, "FRONTIER")) {
        REQUIRE(a->out.kind == PGWT_MERGE_OUT_FRONTIER, "kind");
        REQUIRE(a->out.F == number(member(e, "F")), "F");
        REQUIRE(equal_counters(a->out.counters, counters(f, member(e, "C"))), "C");
    } else if (string_is(kind, "VOID")) {
        REQUIRE(a->out.kind == PGWT_MERGE_OUT_VOID, "kind");
        REQUIRE(a->out.d == number(member(e, "d")), "d");
        REQUIRE(equal_counters(a->out.counters, counters(f, member(e, "C"))), "C");
        REQUIRE(pgwt_merge_reason_name(a->out.reason) &&
                !strcmp(pgwt_merge_reason_name(a->out.reason), string_value(member(e, "reason"))), "reason");
    } else { fputs("fixture: unknown output kind\n", stderr); exit(2); }
#undef REQUIRE
    return true;
}

static bool run_case(const json_node *root, const json_node *row, const char *mode)
{
    fixture f = {.root = root, .row = row};
    const json_node *inputs = member(row, "inputs"), *start = member(row, "start");
    const json_node *expected = member(member(row, "expected"), mode);
    if (!inputs || inputs->kind != J_ARRAY || !expected) {
        fputs("fixture: missing inputs or mode expectation\n", stderr); exit(2);
    }
    /* WRITER_REJECT is a sink instruction, not a later coordinator input.
     * Install it before replay so the specified call can actually reject. */
    for (const json_node *n = inputs->child; n; n = n->next)
        if (string_is(member(n, "kind"), "WRITER_REJECT")) f.reject = n;
    size_t source_count = list_length(member(root, "sources"));
    if (source_count > 32) { fputs("fixture: too many sources\n", stderr); exit(2); }
    pgwt_merge_source sources[32] = {0};
    for (size_t i = 0; i < source_count; ++i) {
        sources[i].qualified = true;
        sources[i].cutoff = optional_number(member(member(start, "cutoffs"),
                                    string_value(item(member(root, "sources"), i))), 0);
    }
    pgwt_merge_attachment attachments[32];
    size_t na = 0;
    for (const json_node *id = member(root, "identities")->child; id; id = id->next) {
        if (na == 32) { fputs("fixture: too many identities\n", stderr); exit(2); }
        attachments[na++] = (pgwt_merge_attachment){.pid = number(member(id, "pid")),
            .g = number(member(id, "g")), .identity_proved = true, .sequence_proved = true,
            .next_q = optional_number(member(member(start, "next_q"), id->key), 1)};
    }
    pgwt_merge_config config = {.mode = !strcmp(mode, "N") ? PGWT_MERGE_N : PGWT_MERGE_Q,
        .sources = sources, .source_count = source_count, .staged_capacity = 128,
        .payload_capacity = 256, .attachment_capacity = 32, .attachments = attachments,
        .attachment_count = na, .sink = sink, .sink_context = &f,
        .observation = {.counters = named_counters(&f, "C0")},
        .last_observed = named_counters(&f, "C0")};
    if (member(start, "baseline")) config.observation.counters = counters(&f, member(start, "baseline"));
    if (member(start, "last_observed")) config.last_observed = counters(&f, member(start, "last_observed"));
    if (member(start, "suspended")) config.suspended = boolean(member(start, "suspended"));
    if (member(start, "last_frontier")) {
        config.has_frontier = true;
        config.frontier = number(member(start, "last_frontier"));
    }
    pgwt_merge_status status;
    pgwt_merge *m = pgwt_merge_create(&config, &status);
    if (!m) { fputs("fixture: create failed\n", stderr); exit(2); }
    for (const json_node *n = inputs->child; n; n = n->next) {
        const json_node *kind = member(n, "kind");
        if (string_is(kind, "NOTE") || string_is(kind, "WRITER_REJECT")) continue;
        pgwt_merge_input in = {.observation = {.d = optional_number(member(n, "d"), 0),
                                                .counters = named_counters(&f, "C0")}};
        if (member(n, "C")) in.observation.counters = counters(&f, member(n, "C"));
        if (string_is(kind, "DATA") || string_is(kind, "TEXT")) {
            bool text = string_is(kind, "TEXT");
            in.kind = text ? PGWT_MERGE_TEXT : PGWT_MERGE_DATA;
            in.record.source = source(&f, member(n, "source"));
            in.record.o = number(member(n, "pos"));
            const char *payload = string_value(member(n, text ? "name" : "label"));
            in.record.payload = payload;
            in.record.payload_size = strlen(payload);
            if (!text) {
                const json_node *id = identity(&f, string_value(member(n, "who")));
                in.record.pid = number(member(id, "pid"));
                in.record.g = number(member(id, "g"));
                in.record.identity_known = true;
                in.record.t = number(member(n, "t"));
                in.record.rank = rank_of(member(n, "rec"));
                if (config.mode == PGWT_MERGE_Q && in.record.rank == PGWT_MERGE_TRANS && member(n, "q")) {
                    in.record.has_q = true;
                    in.record.q = number(member(n, "q"));
                }
            }
        } else if (string_is(kind, "BOUNDARY")) {
            in.kind = PGWT_MERGE_BOUNDARY;
            in.source = source(&f, member(n, "source"));
            in.p = number(member(n, "pos"));
            in.w = number(member(n, "w"));
        } else if (string_is(kind, "CHECK")) in.kind = PGWT_MERGE_CHECK;
        else if (string_is(kind, "LOSS")) {
            in.kind = PGWT_MERGE_LOSS;
            in.source = source(&f, member(n, "source"));
            in.n = number(member(n, "n"));
        } else if (string_is(kind, "FAULT")) {
            in.kind = PGWT_MERGE_FAULT;
            bool found = false;
            for (unsigned r = 0; r < PGWT_MERGE_REASON_COUNT; ++r)
                if (string_is(member(n, "reason"), pgwt_merge_reason_name((pgwt_merge_reason)r))) {
                    in.reason = (pgwt_merge_reason)r; found = true;
                }
            if (!found) { fputs("fixture: unknown fault\n", stderr); exit(2); }
        } else { fputs("fixture: unsupported input kind\n", stderr); exit(2); }
        status = pgwt_merge_feed(m, &in);
        if (status == PGWT_MERGE_EXIT_REQUIRED) break;
    }
    const json_node *outputs = member(expected, "outputs");
    if (!outputs || outputs->kind != J_ARRAY) { fputs("fixture: missing outputs\n", stderr); exit(2); }
    const char *field = NULL;
    size_t mismatch = 0;
    if (f.count != list_length(outputs)) field = "output count";
    else for (size_t i = 0; i < f.count; ++i)
        if (!equal_output(&f, &f.outputs[i], item(outputs, i), &field)) { mismatch = i; break; }
    if (!field && (status == PGWT_MERGE_EXIT_REQUIRED) != boolean(member(expected, "exit_nonzero")))
        field = "exit_nonzero";
    /* Contract §4 invalidation is an out-of-band report, not an output kind. */
    uint64_t row_id = number(member(row, "id"));
    if (!field && row_id == 17 && !pgwt_merge_get_report(m).epoch_invalid) field = "epoch invalidation";
    if (!field && f.reject && !f.rejected) field = "writer rejection not reached";
    if (field) printf("row %" PRIu64 "/%s MISMATCH %s at output %zu (got %zu, expected %zu)\n",
                      row_id, mode, field, mismatch, f.count, list_length(outputs));
    else printf("row %" PRIu64 "/%s PASS\n", row_id, mode);
    pgwt_merge_destroy(m);
    return field == NULL;
}

/* Direct API checks cover normative paths absent from the frozen 20 rows.
 * They are deliberately excluded from mutation coverage of those rows. */
typedef struct {
    pgwt_merge_output outputs[32];
    size_t count;
    bool reject_void;
} safety_capture;

static bool safety_sink(void *context, const pgwt_merge_output *out,
                         pgwt_merge_observation *post)
{
    safety_capture *c = context;
    (void)post;
    if (out->kind == PGWT_MERGE_OUT_VOID && c->reject_void) return false;
    if (c->count >= 32) return false;
    c->outputs[c->count++] = *out;
    return true;
}

static bool safety_checks(void)
{
#define VERIFY(test) do { if (!(test)) { \
    fprintf(stderr, "core merge safety: line %d failed: %s\n", __LINE__, #test); \
    pgwt_merge_destroy(m); return false; } } while (0)
    pgwt_merge *m = NULL;
    safety_capture c = {0};
    pgwt_merge_source sources[4] = {{.qualified = true}, {.qualified = true},
                                   {.qualified = true}, {.qualified = true}};
    pgwt_merge_attachment a = {.pid = 10, .g = 1, .next_q = 1,
                               .identity_proved = true, .sequence_proved = true};
    pgwt_merge_observation obs = {.d = 42, .counters = {.known = true}};
    pgwt_merge_config config = {.mode = PGWT_MERGE_Q, .sources = sources, .source_count = 4,
        .staged_capacity = 2, .payload_capacity = 8, .attachment_capacity = 1,
        .attachments = &a, .attachment_count = 1, .observation = obs,
        .last_observed = obs.counters, .sink = safety_sink, .sink_context = &c};
    pgwt_merge_status status;
    /* Overflow is a breach with a VOID attempt, never an allocation refusal. */
    config.staged_capacity = SIZE_MAX;
    VERIFY(pgwt_merge_create(&config, &status) == NULL);
    VERIFY(status == PGWT_MERGE_EXIT_REQUIRED && c.count == 1 &&
           c.outputs[0].reason == PGWT_MERGE_MALFORMED_ENVELOPE && c.outputs[0].d == 42);
    config.staged_capacity = 2;
    c.count = 0;
    config.attachment_count = SIZE_MAX;
    VERIFY(pgwt_merge_create(&config, &status) == NULL);
    VERIFY(status == PGWT_MERGE_EXIT_REQUIRED &&
           c.outputs[0].reason == PGWT_MERGE_MALFORMED_ENVELOPE);
    config.attachment_count = 2;
    c.count = 0;
    VERIFY(pgwt_merge_create(&config, &status) == NULL);
    VERIFY(status == PGWT_MERGE_SUSPENDED && c.outputs[0].reason == PGWT_MERGE_ALLOCATION_FAIL);
    config.attachment_count = 1;
    c.count = 0;
    a.sequence_proved = false;
    VERIFY(pgwt_merge_create(&config, &status) == NULL);
    VERIFY(status == PGWT_MERGE_SUSPENDED && c.outputs[0].reason == PGWT_MERGE_UNKNOWN_EVIDENCE);
    a.sequence_proved = true;
    a.next_q = 0;
    c.count = 0;
    VERIFY(pgwt_merge_create(&config, &status) == NULL);
    VERIFY(c.outputs[0].reason == PGWT_MERGE_UNKNOWN_EVIDENCE);
    a.next_q = 1;
    c.count = 0;
    m = pgwt_merge_create(&config, &status);
    VERIFY(m && status == PGWT_MERGE_OK);
    /* Zero is a valid exact unsigned source position; capacity exhaustion
     * pauses capture and preserves detection time, without heap growth. */
    pgwt_merge_input in = {.kind = PGWT_MERGE_DATA, .observation = obs,
        .record = {.source = 0, .o = 0, .t = 10, .pid = 10, .g = 1, .q = 1,
                   .rank = PGWT_MERGE_TRANS, .identity_known = true, .has_q = true}};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK && c.count == 0);
    in.record.o = 1; in.record.t = 11; in.record.q = 2;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK);
    in.record.o = 2; in.record.t = 12; in.record.q = 3;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED);
    VERIFY(c.count == 1 && c.outputs[0].reason == PGWT_MERGE_ALLOCATION_FAIL);
    /* FAULT is immediate and separately reported even during suspension. */
    in = (pgwt_merge_input){.kind = PGWT_MERGE_FAULT, .observation = obs,
                             .reason = PGWT_MERGE_DEPENDENCY_LOST};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED && c.count == 2);
    in.reason = PGWT_MERGE_PERF_LOST;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED && c.count == 3);
    VERIFY(c.outputs[1].reason == PGWT_MERGE_DEPENDENCY_LOST &&
           c.outputs[2].reason == PGWT_MERGE_PERF_LOST);
    /* No revival by boundaries; REARM requires repaired identity and a
     * positive proved Q baseline, then fresh boundaries in the new epoch. */
    in = (pgwt_merge_input){.kind = PGWT_MERGE_BOUNDARY, .observation = obs,
                             .source = 1, .p = 0, .w = 100};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED && c.count == 3);
    in = (pgwt_merge_input){.kind = PGWT_MERGE_REARM, .observation = obs,
        .attachments = &a, .attachment_count = 1, .attachment_proofs_fresh = true};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED);
    in.identity_repaired = true;
    a.next_q = 0;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED);
    a.next_q = 1;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK);
    VERIFY(pgwt_merge_get_report(m).epoch == 2 && !pgwt_merge_get_report(m).has_frontier);
    c.count = 0;
    /* Untimed TEXT accepts only its own envelope fields, retains lifecycle
     * position order, and stays staged until W>0. */
    in = (pgwt_merge_input){.kind = PGWT_MERGE_TEXT, .observation = obs,
        .record = {.source = 3, .o = 0, .rank = (pgwt_merge_rank)99,
                   .dependency = (pgwt_merge_dependency)99, .payload = "x", .payload_size = 1}};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK && c.count == 0);
    in.record.o = 1; in.record.payload = "y";
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK && c.count == 0);
    for (size_t s = 0; s < 4; ++s) {
        in = (pgwt_merge_input){.kind = PGWT_MERGE_BOUNDARY, .observation = obs,
                                 .source = s, .p = 3, .w = 11};
        VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK);
        if (s < 3) VERIFY(c.count == 0);
    }
    VERIFY(c.count == 3 && c.outputs[0].kind == PGWT_MERGE_OUT_TEXT &&
           c.outputs[0].record.o == 0 && c.outputs[1].record.o == 1 &&
           c.outputs[2].kind == PGWT_MERGE_OUT_FRONTIER && c.outputs[2].F == 10);
    /* A reset/decrease is unknown accounting; an identical reread emits
     * no second report, and an explicit new FAULT still emits VOID. */
    in = (pgwt_merge_input){.kind = PGWT_MERGE_CHECK, .observation = obs};
    in.observation.counters.value[0] = 1;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED);
    VERIFY(c.outputs[3].reason == PGWT_MERGE_COUNTER_CHANGE);
    in.observation = obs;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED);
    VERIFY(c.outputs[4].reason == PGWT_MERGE_COUNTER_UNKNOWN);
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED && c.count == 5);
    c.reject_void = true;
    in.kind = PGWT_MERGE_FAULT; in.reason = PGWT_MERGE_DUPLICATE_IDENTITY;
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_EXIT_REQUIRED);
    VERIFY(pgwt_merge_get_report(m).void_failed);
    pgwt_merge_destroy(m);
    m = NULL;
    c = (safety_capture){0};
    config.staged_capacity = 3;
    m = pgwt_merge_create(&config, &status);
    VERIFY(m);
    /* Later-arriving TEXT precedes DATA in the same complete eligible batch. */
    in = (pgwt_merge_input){.kind = PGWT_MERGE_DATA, .observation = obs,
        .record = {.source = 0, .o = 0, .t = 10, .pid = 10, .g = 1, .q = 1,
                   .rank = PGWT_MERGE_TRANS, .identity_known = true, .has_q = true}};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK);
    in = (pgwt_merge_input){.kind = PGWT_MERGE_TEXT, .observation = obs,
        .record = {.source = 3, .o = 0, .payload = "x", .payload_size = 1}};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK);
    in.record.o = 1; in.record.payload = "y";
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK && c.count == 0);
    for (size_t s = 0; s < 4; ++s) {
        in = (pgwt_merge_input){.kind = PGWT_MERGE_BOUNDARY, .observation = obs,
                                 .source = s, .p = 2, .w = 11};
        VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_OK);
    }
    VERIFY(c.count == 4 && c.outputs[0].kind == PGWT_MERGE_OUT_TEXT &&
           c.outputs[0].record.o == 0 && c.outputs[1].kind == PGWT_MERGE_OUT_TEXT &&
           c.outputs[1].record.o == 1 && c.outputs[2].kind == PGWT_MERGE_OUT_DATA &&
           c.outputs[2].record.t == 10 && c.outputs[3].kind == PGWT_MERGE_OUT_FRONTIER);
    pgwt_merge_destroy(m);
    m = NULL;
    c = (safety_capture){0};
    config.staged_capacity = 2;
    m = pgwt_merge_create(&config, &status);
    VERIFY(m);
    in = (pgwt_merge_input){.kind = PGWT_MERGE_TEXT, .observation = obs,
        .record = {.source = 3, .o = 0, .payload = "123456789", .payload_size = 9}};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_SUSPENDED);
    VERIFY(c.outputs[0].reason == PGWT_MERGE_ALLOCATION_FAIL);
    /* Impossible REARM size must be rejected before reading attachments. */
    in = (pgwt_merge_input){.kind = PGWT_MERGE_REARM, .observation = obs,
        .attachment_proofs_fresh = true, .identity_repaired = true, .attachment_count = SIZE_MAX};
    VERIFY(pgwt_merge_feed(m, &in) == PGWT_MERGE_EXIT_REQUIRED);
    VERIFY(c.outputs[1].reason == PGWT_MERGE_MALFORMED_ENVELOPE);
    pgwt_merge_destroy(m);
#undef VERIFY
    puts("core merge safety: passed");
    return true;
}

int main(int argc, char **argv)
{
    bool fixture_only = argc > 1 && !strcmp(argv[1], "--fixture-only");
    bool safety_only = argc > 1 && !strcmp(argv[1], "--safety-only");
    if (!fixture_only && !safety_checks()) return 2;
    if (safety_only) return 0;
    int path_arg = fixture_only ? 2 : 1;
    const char *path = argc > path_arg ? argv[path_arg] : "contracts/merge-v1/examples.json";
    char error[256];
    json_document *d = read_document(path, error, sizeof(error));
    if (!d && argc <= path_arg) d = read_document("../contracts/merge-v1/examples.json", error, sizeof(error));
    if (!d) { fprintf(stderr, "fixture: %s\n", error); return 2; }
    const json_node *rows = member(d->root, "rows");
    if (!rows || rows->kind != J_ARRAY || list_length(rows) < 20) {
        fputs("fixture: fewer than 20 rows\n", stderr); free_document(d); return 2;
    }
    size_t total = 0, passed = 0;
    for (const json_node *r = rows->child; r; r = r->next) {
        const json_node *modes = member(r, "modes");
        if (!modes || modes->kind != J_ARRAY || !list_length(modes)) {
            fputs("fixture: no listed modes\n", stderr); free_document(d); return 2;
        }
        for (const json_node *m = modes->child; m; m = m->next) {
            if (!string_is(m, "N") && !string_is(m, "Q")) {
                fputs("fixture: unknown mode\n", stderr); free_document(d); return 2;
            }
            ++total;
            if (run_case(d->root, r, m->string)) ++passed;
        }
    }
    printf("core merge contract: %zu/%zu passed\n", passed, total);
    free_document(d);
    return total == passed ? 0 : 1;
}
