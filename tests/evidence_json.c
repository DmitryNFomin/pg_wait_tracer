#include "evidence_json.h"

/* JSON lexer/tree helpers reused from src/core/contract_json.c. Kept test-only;
 * the replay library has no JSON dependency. */

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


static bool is_null(const json_node *n) { return n && n->kind == J_NULL; }
static bool boolean(const json_node *n, bool *out)
{
    if (!n || n->kind != J_BOOL) return false;
    *out = n->boolean;
    return true;
}
static bool optional_uint(const json_node *n, bool *has, uint64_t *out)
{
    *has = !is_null(n);
    return is_null(n) || integer(n, out);
}
static bool event_or_null(const json_node *n, pgwt_core_event *out)
{
    return is_null(n) || event_string(n, out);
}
static bool pid_field(const json_node *n, uint32_t *out)
{
    uint64_t v;
    if (!integer(n, &v) || !v || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}
static bool literal(const json_node *n, const char *text)
{
    return n && n->kind == J_STRING && bytes_equal(n->string, n->length, text, strlen(text));
}
static bool keys(const json_node *n, const char *const *allowed, size_t count)
{
    if (!n || n->kind != J_OBJECT) return false;
    for (const json_node *m = n->child; m; m = m->next) {
        bool found = false;
        for (size_t i = 0; i < count; ++i)
            if (bytes_equal(m->key, m->key_length, allowed[i], strlen(allowed[i]))) found = true;
        if (!found) return false;
    }
    return true;
}
#define KEYS(n, ...) keys(n, (const char *const[]){__VA_ARGS__}, \
                           sizeof((const char *const[]){__VA_ARGS__}) / sizeof(char *))
static void counters(const json_node *n, pgwt_evidence_counters *out)
{
    out->known = array_size(n, 15);
    for (size_t i = 0; i < 15 && out->known; ++i)
        out->known = integer(item(n, i), &out->value[i]);
}

static void record(const json_node *n, pgwt_evidence_record *out)
{
    bool ok = integer(member(n, "t"), &out->t);
    const json_node *kind = member(n, "kind");
    if (literal(kind, "LIFE")) out->kind = PGWT_EVIDENCE_LIFE;
    else if (literal(kind, "ATTACH_OK")) out->kind = PGWT_EVIDENCE_ATTACH_OK;
    else if (literal(kind, "TRANS")) out->kind = PGWT_EVIDENCE_TRANS;
    else if (literal(kind, "FRONTIER")) out->kind = PGWT_EVIDENCE_FRONTIER;
    else if (literal(kind, "END")) out->kind = PGWT_EVIDENCE_END;
    else out->kind = PGWT_EVIDENCE_OTHER;
    if (out->kind != PGWT_EVIDENCE_FRONTIER && out->kind != PGWT_EVIDENCE_OTHER)
        ok = pid_field(member(n, "pid"), &out->pid) && ok;
    if (out->kind == PGWT_EVIDENCE_LIFE || out->kind == PGWT_EVIDENCE_ATTACH_OK ||
        out->kind == PGWT_EVIDENCE_END)
        ok = optional_uint(member(n, "fork"), &out->has_fork, &out->fork) && ok;
    switch (out->kind) {
    case PGWT_EVIDENCE_LIFE:
        ok = integer(member(n, "start"), &out->start) && ok;
        ok = boolean(member(n, "start_known"), &out->start_known) && ok;
        ok = optional_uint(member(n, "exit"), &out->has_exit, &out->exit) && ok;
        ok = KEYS(n, "kind", "t", "pid", "fork", "start", "start_known", "exit") && ok;
        break;
    case PGWT_EVIDENCE_ATTACH_OK:
        out->reread = literal(member(n, "method"), "reread");
        ok = (out->reread || literal(member(n, "method"), "transition")) && ok;
        ok = event_or_null(member(n, "seed"), &out->seed) && ok;
        ok = event_or_null(member(n, "state"), &out->state) && ok;
        counters(member(n, "counters"), &out->counters);
        ok = KEYS(n, "kind", "t", "pid", "fork", "method", "seed", "state", "counters") && ok;
        break;
    case PGWT_EVIDENCE_TRANS:
        ok = event_or_null(member(n, "old"), &out->old_event) && ok;
        ok = event_or_null(member(n, "new"), &out->new_event) && ok;
        ok = integer(member(n, "duration"), &out->duration) && ok;
        ok = KEYS(n, "kind", "t", "pid", "old", "new", "duration") && ok;
        break;
    case PGWT_EVIDENCE_FRONTIER:
        counters(member(n, "counters"), &out->counters);
        ok = KEYS(n, "kind", "t", "counters") && ok;
        break;
    case PGWT_EVIDENCE_END:
        ok = event_string(member(n, "reason"), &out->end_reason) && ok;
        ok = KEYS(n, "kind", "t", "pid", "fork", "reason") && ok;
        break;
    case PGWT_EVIDENCE_OTHER: break; /* Public API must refuse this kind. */
    }
    out->unsupported_shape = !ok;
}

int evidence_case_load(const char *path, evidence_case *out, char *error, size_t size)
{
    memset(out, 0, sizeof(*out));
    json_document *d = read_document(path, error, size);
    if (!d) return -1;
    out->document = d;
    const json_node *id = member(d->root, "id");
    out->id = id && id->kind == J_STRING ? id->string : "anonymous";
    pgwt_evidence_input *in = &out->input;
    const json_node *files = member(d->root, "files");
    if (!files || files->kind != J_ARRAY) in->unsupported_shape = true;
    in->file_count = files && files->kind == J_ARRAY ? list_length(files) : 0;
    if (in->file_count) {
        out->files = calloc(in->file_count, sizeof(*out->files));
        if (!out->files) goto allocation_error;
    }
    in->files = out->files;
    for (size_t i = 0; i < in->file_count; ++i) {
        pgwt_evidence_file *f = &out->files[i];
        const json_node *n = item(files, i);
        uint64_t version = 0;
        bool ok = integer(member(n, "version"), &version) && version <= UINT32_MAX;
        f->version = (unsigned)version;
        ok = event_string(member(n, "boot"), &f->boot) && ok;
        ok = event_string(member(n, "capture"), &f->capture) && ok;
        ok = integer(member(n, "file_no"), &f->file_no) && ok;
        uint64_t previous;
        ok = optional_uint(member(n, "previous"), &f->has_previous, &previous) && ok;
        /* Qualification absence is explicitly unqualified (contract sections 1,5).
         * present is a scenario convenience whose absent value means intact. */
        const json_node *qualified = member(n, "qualified");
        const json_node *present = member(n, "present");
        ok = (!qualified || boolean(qualified, &f->qualified)) && ok;
        f->present = true;
        ok = (!present || boolean(present, &f->present)) && ok;
        counters(member(n, "loss_baseline"), &f->baseline);
        ok = KEYS(n, "version", "boot", "capture", "file_no", "previous", "qualified",
                  "loss_baseline", "present", "records") && ok;
        const json_node *records = member(n, "records");
        ok = records && records->kind == J_ARRAY && ok;
        f->record_count = records && records->kind == J_ARRAY ? list_length(records) : 0;
        pgwt_evidence_record *decoded = NULL;
        if (f->record_count) {
            decoded = calloc(f->record_count, sizeof(*decoded));
            if (!decoded) goto allocation_error;
        }
        f->records = decoded;
        const json_node *r = records ? records->child : NULL;
        for (size_t j = 0; j < f->record_count; ++j, r = r->next) record(r, &decoded[j]);
        f->unsupported_shape = !ok;
    }
    const json_node *target = member(d->root, "target");
    bool target_ok = event_string(member(target, "capture"), &in->capture);
    target_ok = pid_field(member(target, "pid"), &in->pid) && target_ok;
    target_ok = optional_uint(member(target, "fork"), &in->has_fork, &in->fork) && target_ok;
    target_ok = KEYS(target, "capture", "pid", "fork") && target_ok;
    in->unsupported_shape |= !target_ok;
    const json_node *q = member(d->root, "query");
    if (!q || q->kind != J_OBJECT) out->query.invalid_fields |= PGWT_CORE_QUERY_OBJECT_INVALID;
    if (!event_string(member(q, "event"), &out->query.event)) out->query.invalid_fields |= PGWT_CORE_QUERY_EVENT_INVALID;
    if (!integer(member(q, "from"), &out->query.from)) out->query.invalid_fields |= PGWT_CORE_QUERY_FROM_INVALID;
    if (!integer(member(q, "to"), &out->query.to)) out->query.invalid_fields |= PGWT_CORE_QUERY_TO_INVALID;
    return 0;
allocation_error:
    evidence_case_free(out);
    message(error, size, "allocation failed");
    return -1;
}

void evidence_case_free(evidence_case *input)
{
    if (input->files)
        for (size_t i = 0; i < input->input.file_count; ++i) free((void *)input->files[i].records);
    free(input->files);
    free_document(input->document);
    memset(input, 0, sizeof(*input));
}

static void json_event(FILE *f, pgwt_core_event event)
{
    fputc('"', f);
    for (size_t i = 0; i < event.length; ++i) {
        unsigned char c = (unsigned char)event.data[i];
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void projection_json(FILE *f, const pgwt_core_evidence *e)
{
    fputs("{\"life\":", f);
    if (e->invalid_fields & PGWT_CORE_LIFE_INVALID) fputs("null", f);
    else {
        fprintf(f, "[%" PRIu64 ",", e->life_start);
        if (e->has_life_end) fprintf(f, "%" PRIu64, e->life_end);
        else fputs("null", f);
        fputc(']', f);
    }
    fputs(",\"coverage\":[", f);
    for (size_t i = 0; i < e->coverage_count; ++i)
        fprintf(f, "%s[%" PRIu64 ",%" PRIu64 "]", i ? "," : "", e->coverage[i].start, e->coverage[i].end);
    fputs("],\"waits\":[", f);
    for (size_t i = 0; i < e->wait_count; ++i) {
        fputs(i ? ",[" : "[", f);
        json_event(f, e->waits[i].event);
        fprintf(f, ",%" PRIu64 ",%" PRIu64 "]", e->waits[i].start, e->waits[i].end);
    }
    fputs("],\"open\":", f);
    if (!e->has_open) fputs("null", f);
    else {
        fputc('[', f);
        json_event(f, e->open.event);
        fprintf(f, ",%" PRIu64 ",%" PRIu64 "]", e->open.start, e->open.as_of);
    }
    fputc('}', f);
}

int evidence_output_json(FILE *f, const pgwt_evidence_output *out)
{
    char t[2048], n[2048];
    if (pgwt_contract_result_json(&out->wait_time, t, sizeof(t)) ||
        pgwt_contract_result_json(&out->completed_waits, n, sizeof(n))) return -1;
    fputs("{\"projection\":", f);
    projection_json(f, &out->projection);
    fprintf(f, ",\"wait_time\":%s,\"completed_waits\":%s}", t, n);
    return ferror(f) ? -1 : 0;
}

/* Produce the first differing leaf/shape, rather than only a metric name. */
static bool difference(const json_node *a, const json_node *b, char *path, size_t size)
{
    if (equal_values(a, b)) return false;
    size_t used = strlen(path);
    if (used + 1 >= size) return true;
    if (a && b && a->kind == J_OBJECT && b->kind == J_OBJECT) {
        for (const json_node *n = b->child; n; n = n->next) {
            snprintf(path + used, size > used ? size - used : 0, ".%s", n->key);
            if (difference(member(a, n->key), n, path, size)) return true;
            path[used] = 0;
        }
    } else if (a && b && a->kind == J_ARRAY && b->kind == J_ARRAY) {
        const json_node *x = a->child, *y = b->child;
        for (size_t i = 0; x && y; ++i, x = x->next, y = y->next) {
            snprintf(path + used, size > used ? size - used : 0, "[%zu]", i);
            if (difference(x, y, path, size)) return true;
            path[used] = 0;
        }
    }
    return true;
}

int evidence_compare(const evidence_case *input, const pgwt_evidence_output *out,
                     const char *expected_path, const char *projection_path,
                     char *field, size_t size)
{
    json_document *expected = read_document(expected_path, field, size);
    json_document *projections = read_document(projection_path, field, size);
    json_document *actual = NULL;
    int status = -1;
    if (!expected || !projections) goto done;
    /* tmpfile keeps projection serialization off the working tree and permits
     * arbitrarily many waits without a fixed serialization buffer. */
    FILE *f = tmpfile();
    if (!f) goto done;
    if (evidence_output_json(f, out) || fflush(f)) { fclose(f); goto done; }
    long length = ftell(f);
    if (length < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); goto done; }
    char *text = malloc((size_t)length + 1);
    if (!text) { fclose(f); goto done; }
    size_t got = fread(text, 1, (size_t)length, f);
    fclose(f);
    if (got != (size_t)length) { free(text); goto done; }
    text[got] = 0;
    actual = parse_document(text, field, size);
    if (!actual) goto done;
    const json_node *answer = NULL;
    for (const json_node *n = expected->root->child; n; n = n->next)
        if (literal(member(n, "id"), input->id)) { answer = n; break; }
    const json_node *projection = member(projections->root, input->id);
    if (!answer || !projection) { message(field, size, "missing frozen answer"); goto done; }
    status = 1;
    const char *metrics[] = {"wait_time", "completed_waits"};
    for (size_t i = 0; i < 2; ++i) {
        message(field, size, "%s", metrics[i]);
        if (difference(member(actual->root, metrics[i]), member(answer, metrics[i]), field, size)) goto done;
    }
    message(field, size, "projection");
    if (difference(member(actual->root, "projection"), projection, field, size)) goto done;
    status = 0;
done:
    free_document(expected);
    free_document(projections);
    free_document(actual);
    return status;
}
