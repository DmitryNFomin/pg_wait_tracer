#include "contract_json.h"

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

static bool array_size(const json_node *n, size_t size)
{
    return n && n->kind == J_ARRAY && list_length(n) == size;
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

static bool event_string(const json_node *n, pgwt_core_event *out)
{
    if (!n || n->kind != J_STRING) return false;
    *out = (pgwt_core_event){n->string, n->length};
    return true;
}

/* contract 7.5: validate each required field, exact list lengths, and
 * integer-token types. Do not infer validity of one field from another. */
int pgwt_contract_case_load(const char *path, pgwt_contract_case *out,
                            char *error, size_t error_size)
{
    memset(out, 0, sizeof(*out));
    json_document *d = read_document(path, error, error_size);
    if (!d) return -1;
    out->document = d;
    pgwt_core_evidence *e = &out->evidence;
    pgwt_core_query *q = &out->query;
    const json_node *evidence = member(d->root, "evidence");
    const json_node *life = member(evidence, "life");
    const json_node *end = item(life, 1);
    bool life_start_ok = integer(item(life, 0), &e->life_start);
    e->has_life_end = end && end->kind != J_NULL;
    bool life_end_ok = end && (end->kind == J_NULL || integer(end, &e->life_end));
    if (!array_size(life, 2) || !life_start_ok || !life_end_ok)
        e->invalid_fields |= PGWT_CORE_LIFE_INVALID;

    const json_node *coverage = member(evidence, "coverage");
    if (!coverage || coverage->kind != J_ARRAY)
        e->invalid_fields |= PGWT_CORE_COVERAGE_INVALID;
    else {
        e->coverage_count = list_length(coverage);
        pgwt_core_span *spans = e->coverage_count ? calloc(e->coverage_count, sizeof(*spans)) : NULL;
        if (e->coverage_count && !spans) goto allocation_failed;
        e->coverage = spans;
        size_t i = 0;
        for (const json_node *n = coverage->child; n; n = n->next, ++i) {
            bool a_ok = integer(item(n, 0), &spans[i].start);
            bool b_ok = integer(item(n, 1), &spans[i].end);
            if (!array_size(n, 2) || !a_ok || !b_ok)
                e->invalid_fields |= PGWT_CORE_COVERAGE_INVALID;
        }
    }

    const json_node *waits = member(evidence, "waits");
    if (!waits || waits->kind != J_ARRAY)
        e->invalid_fields |= PGWT_CORE_WAITS_INVALID;
    else {
        e->wait_count = list_length(waits);
        pgwt_core_wait *records = e->wait_count ? calloc(e->wait_count, sizeof(*records)) : NULL;
        if (e->wait_count && !records) goto allocation_failed;
        e->waits = records;
        size_t i = 0;
        for (const json_node *n = waits->child; n; n = n->next, ++i) {
            bool event_ok = event_string(item(n, 0), &records[i].event);
            bool a_ok = integer(item(n, 1), &records[i].start);
            bool b_ok = integer(item(n, 2), &records[i].end);
            if (!array_size(n, 3) || !event_ok || !a_ok || !b_ok)
                e->invalid_fields |= PGWT_CORE_WAITS_INVALID;
        }
    }

    const json_node *open = member(evidence, "open");
    if (!open) e->invalid_fields |= PGWT_CORE_OPEN_INVALID;
    e->has_open = open && open->kind != J_NULL;
    if (e->has_open) {
        bool event_ok = event_string(item(open, 0), &e->open.event);
        bool start_ok = integer(item(open, 1), &e->open.start);
        bool as_of_ok = integer(item(open, 2), &e->open.as_of);
        if (!as_of_ok) e->invalid_fields |= PGWT_CORE_OPEN_AS_OF_INVALID;
        if (!array_size(open, 3) || !event_ok || !start_ok || !as_of_ok)
            e->invalid_fields |= PGWT_CORE_OPEN_INVALID;
    }

    const json_node *query = member(d->root, "query");
    if (!query || query->kind != J_OBJECT)
        q->invalid_fields |= PGWT_CORE_QUERY_OBJECT_INVALID;
    if (!event_string(member(query, "event"), &q->event))
        q->invalid_fields |= PGWT_CORE_QUERY_EVENT_INVALID;
    if (!integer(member(query, "from"), &q->from))
        q->invalid_fields |= PGWT_CORE_QUERY_FROM_INVALID;
    if (!integer(member(query, "to"), &q->to))
        q->invalid_fields |= PGWT_CORE_QUERY_TO_INVALID;
    return 0;
allocation_failed:
    message(error, error_size, "allocation failed");
    pgwt_contract_case_free(out);
    return -1;
}

void pgwt_contract_case_free(pgwt_contract_case *input)
{
    free((void *)input->evidence.coverage);
    free((void *)input->evidence.waits);
    free_document(input->document);
    memset(input, 0, sizeof(*input));
}

typedef struct { char *buffer; size_t used, size; bool failed; } json_writer;

static void append(json_writer *w, const char *format, ...)
{
    if (w->failed) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(w->buffer + w->used, w->size - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->size - w->used) w->failed = true;
    else w->used += (size_t)n;
}

static void optional(json_writer *w, pgwt_core_optional_u64 n)
{
    if (n.present) append(w, "%" PRIu64, n.value);
    else append(w, "null");
}

/* contract 5: serialize the result's presence flags, including absent detail,
 * instead of recalculating availability in the adapter. */
int pgwt_contract_result_json(const pgwt_core_result *r, char *buffer, size_t size)
{
    if (!size) return -1;
    json_writer w = {buffer, 0, size, false};
    append(&w, "{\"metric\":\"%s\",\"version\":%u,\"value\":", r->metric, r->version);
    optional(&w, r->value);
    append(&w, ",\"unit\":\"%s\",\"basis\":\"%s\",\"completeness\":\"%s\",\"reasons\":[",
           r->unit, r->basis, r->completeness);
    for (size_t i = 0; i < r->reason_count; ++i)
        append(&w, "%s\"%s\"", i ? "," : "", r->reasons[i]);
    append(&w, "]");
    if (r->detail_present)
        append(&w, ",\"detail\":{\"closed_ns\":%" PRIu64 ",\"open_ns\":%" PRIu64 "}",
               r->detail.closed_ns, r->detail.open_ns);
    append(&w, ",\"diagnostic\":{\"%s\":",
           strcmp(r->metric, "wait_time") == 0 ? "recorded_ns" : "recorded_count");
    optional(&w, r->diagnostic.recorded);
    append(&w, ",\"covered_ns\":");
    optional(&w, r->diagnostic.covered_ns);
    append(&w, ",\"required_ns\":");
    optional(&w, r->diagnostic.required_ns);
    append(&w, "}}");
    return w.failed ? -1 : 0;
}

/* Exact comparison of the contract's integer result numbers. Other numeric
 * tokens are retained verbatim; result objects in this contract use integers. */
static bool equal_values(const json_node *a, const json_node *b)
{
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case J_NULL: return true;
    case J_BOOL: return a->boolean == b->boolean;
    case J_NUMBER: {
        uint64_t x, y;
        if (integer(a, &x) && integer(b, &y)) return x == y;
        return bytes_equal(a->number, a->length, b->number, b->length);
    }
    case J_STRING: return bytes_equal(a->string, a->length, b->string, b->length);
    case J_ARRAY: {
        const json_node *x = a->child, *y = b->child;
        for (; x && y; x = x->next, y = y->next)
            if (!equal_values(x, y)) return false;
        return !x && !y;
    }
    case J_OBJECT:
        if (list_length(a) != list_length(b)) return false;
        for (const json_node *x = a->child; x; x = x->next) {
            const json_node *y;
            for (y = b->child; y; y = y->next)
                if (bytes_equal(x->key, x->key_length, y->key, y->key_length)) break;
            if (!equal_values(x, y)) return false;
        }
        return true;
    }
    return false;
}

static json_document *string_document(const char *text, char *error, size_t size)
{
    char *copy = malloc(strlen(text) + 1);
    if (!copy) { message(error, size, "allocation failed"); return NULL; }
    strcpy(copy, text);
    return parse_document(copy, error, size);
}

int pgwt_contract_compare_results(const char *time_json, const char *count_json,
                                  const char *path, char *error, size_t size)
{
    json_document *expected = read_document(path, error, size);
    if (!expected) return -1;
    json_document *time = string_document(time_json, error, size);
    json_document *count = string_document(count_json, error, size);
    int status = -1;
    if (!time || !count) goto done;
    status = 1;
    bool seen_time = false, seen_count = false;
    if (expected->root->kind != J_OBJECT) {
        message(error, size, "expected file is not an object");
        goto done;
    }
    for (const json_node *n = expected->root->child; n; n = n->next) {
        if (bytes_equal(n->key, n->key_length, "derivation", 10)) continue;
        const json_node *metric = member(n, "metric");
        if (!metric) continue; /* Metadata, not a result object. */
        if (metric->kind != J_STRING) {
            message(error, size, "result metric is not a string"); goto done;
        }
        const json_node *actual;
        if (bytes_equal(metric->string, metric->length, "wait_time", 9) && !seen_time) {
            seen_time = true;
            actual = time->root;
        } else if (bytes_equal(metric->string, metric->length, "completed_waits", 15) && !seen_count) {
            seen_count = true;
            actual = count->root;
        } else { message(error, size, "unknown or duplicate result metric"); goto done; }
        if (!equal_values(actual, n)) {
            message(error, size, "%s result differs", metric->string);
            goto done;
        }
    }
    if (!seen_time || !seen_count) { message(error, size, "missing result object"); goto done; }
    status = 0;
done:
    free_document(expected);
    free_document(time);
    free_document(count);
    return status;
}
