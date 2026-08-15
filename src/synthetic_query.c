/* synthetic_query.c -- PG13 sampled normalized-text grouping. */
#include "synthetic_query.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static bool word_boundary(unsigned char c)
{
    /* PostgreSQL accepts non-ASCII bytes in unquoted identifiers.  We do not
     * need to decode UTF-8 here, but every high-bit byte must remain part of
     * the identifier so a following digit is not mistaken for a literal. */
    return !(isalnum(c) || c >= 0x80 || c == '_' || c == '$');
}

static bool identifier_start(unsigned char c)
{
    return isalpha(c) || c >= 0x80 || c == '_';
}

static bool identifier_cont(unsigned char c)
{
    return isalnum(c) || c >= 0x80 || c == '_' || c == '$';
}

static bool tight_punctuation(unsigned char c)
{
    return strchr(",()[]=<>+-*/%^;:.", c) != NULL;
}

static bool keyword_literal(const char *s, size_t n)
{
    static const char *const words[] = {"null", "true", "false"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strlen(words[i]) == n && strncasecmp(s, words[i], n) == 0)
            return true;
    return false;
}

static int append_char(char *out, size_t out_size, size_t *used, char c)
{
    if (*used + 1 >= out_size)
        return -1;
    out[(*used)++] = c;
    out[*used] = '\0';
    return 0;
}

static int append_space(char *out, size_t out_size, size_t *used)
{
    if (*used == 0 || out[*used - 1] == ' ')
        return 0;
    return append_char(out, out_size, used, ' ');
}

static int append_placeholder(char *out, size_t out_size, size_t *used)
{
    if (*used && out[*used - 1] == '?')
        return 0;
    return append_char(out, out_size, used, '?');
}

/* Skip a regular/E/U&/B/X quoted string.  The caller points at the quote. */
static size_t skip_single_quote(const char *s, size_t i)
{
    i++;
    while (s[i]) {
        if (s[i] == '\\' && s[i + 1]) {
            i += 2;
            continue;
        }
        if (s[i] == '\'') {
            if (s[i + 1] == '\'') {
                i += 2;
                continue;
            }
            return i + 1;
        }
        i++;
    }
    return i;
}

/* Double-quoted identifiers are not string literals. Copy them byte-for-byte
 * (including doubled quote escapes) so comment markers, quotes, dollar signs,
 * and digits inside an identifier never enter the literal/comment scanner. */
static size_t copy_quoted_identifier(const char *s, size_t i,
                                     char *out, size_t out_size,
                                     size_t *used)
{
    if (append_char(out, out_size, used, s[i++]) != 0)
        return 0;
    while (s[i]) {
        if (append_char(out, out_size, used, s[i]) != 0)
            return 0;
        if (s[i] == '"') {
            if (s[i + 1] == '"') {
                i++;
                if (append_char(out, out_size, used, s[i++]) != 0)
                    return 0;
                continue;
            }
            return i + 1;
        }
        i++;
    }
    return 0; /* incomplete activity text */
}

static size_t dollar_tag_len(const char *s)
{
    if (*s != '$')
        return 0;
    size_t i = 1;
    if (s[i] == '$')
        return 2;
    if (!(isalpha((unsigned char)s[i]) || (unsigned char)s[i] >= 0x80 ||
          s[i] == '_'))
        return 0; /* $1 is a bind parameter, not a quoted literal */
    for (i++; isalnum((unsigned char)s[i]) ||
         (unsigned char)s[i] >= 0x80 || s[i] == '_'; i++)
        ;
    return s[i] == '$' ? i + 1 : 0;
}

static size_t skip_number(const char *s, size_t i)
{
    if (s[i] == '.') {
        i++;
        while (isdigit((unsigned char)s[i]) || s[i] == '_') i++;
        goto exponent;
    }
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        while (isxdigit((unsigned char)s[i]) || s[i] == '_') i++;
        return i;
    }
    while (isdigit((unsigned char)s[i]) || s[i] == '_') i++;
    /* PostgreSQL accepts a trailing decimal point (4.) but its scanner keeps
     * the first dot out of a `..` sequence. */
    if (s[i] == '.' && s[i + 1] != '.') {
        i++;
        while (isdigit((unsigned char)s[i]) || s[i] == '_') i++;
    }
exponent:
    if (s[i] == 'e' || s[i] == 'E') {
        size_t e = i++;
        if (s[i] == '+' || s[i] == '-') i++;
        if (!isdigit((unsigned char)s[i]))
            return e;
        while (isdigit((unsigned char)s[i]) || s[i] == '_') i++;
    }
    return i;
}

size_t pgwt_pg13_normalize_query(const char *input, char *output,
                                 size_t output_size)
{
    if (!input || !output || output_size < 2)
        return 0;
    output[0] = '\0';
    size_t used = 0;
    int block_depth = 0;

    for (size_t i = 0; input[i]; ) {
        unsigned char c = (unsigned char)input[i];
        if (block_depth) {
            if (input[i] == '/' && input[i + 1] == '*') {
                block_depth++;
                i += 2;
            } else if (input[i] == '*' && input[i + 1] == '/') {
                block_depth--;
                i += 2;
                if (!block_depth && append_space(output, output_size,
                                                  &used) != 0)
                    return 0;
            } else {
                i++;
            }
            continue;
        }
        if (input[i] == '-' && input[i + 1] == '-') {
            i += 2;
            while (input[i] && input[i] != '\n' && input[i] != '\r') i++;
            if (append_space(output, output_size, &used) != 0) return 0;
            continue;
        }
        if (input[i] == '/' && input[i + 1] == '*') {
            block_depth = 1;
            i += 2;
            continue;
        }
        if (isspace(c)) {
            size_t next = i + 1;
            while (isspace((unsigned char)input[next])) next++;
            if (used && !tight_punctuation((unsigned char)output[used - 1]) &&
                input[next] &&
                !tight_punctuation((unsigned char)input[next]) &&
                append_space(output, output_size, &used) != 0)
                return 0;
            i++;
            continue;
        }
        if (c == '"') {
            i = copy_quoted_identifier(input, i, output, output_size, &used);
            if (!i) return 0;
            continue;
        }

        /* Prefixes that make a following single quote a literal remain part
         * of the literal and are replaced together. */
        if ((c == 'e' || c == 'E' || c == 'b' || c == 'B' ||
             c == 'x' || c == 'X') && input[i + 1] == '\'' &&
            (i == 0 || word_boundary((unsigned char)input[i - 1]))) {
            if (append_placeholder(output, output_size, &used) != 0) return 0;
            i = skip_single_quote(input, i + 1);
            continue;
        }
        if ((c == 'u' || c == 'U') && input[i + 1] == '&' &&
            input[i + 2] == '\'' &&
            (i == 0 || word_boundary((unsigned char)input[i - 1]))) {
            if (append_placeholder(output, output_size, &used) != 0) return 0;
            i = skip_single_quote(input, i + 2);
            continue;
        }
        if (c == '\'') {
            if (append_placeholder(output, output_size, &used) != 0) return 0;
            i = skip_single_quote(input, i);
            continue;
        }
        size_t tag_len = dollar_tag_len(input + i);
        if (tag_len) {
            const char *end = input + i + tag_len;
            while (*end && strncmp(end, input + i, tag_len) != 0)
                end++;
            if (!*end) return 0; /* incomplete activity text */
            if (append_placeholder(output, output_size, &used) != 0) return 0;
            i = (size_t)(end - input) + tag_len;
            continue;
        }
        if (isdigit(c) && (i == 0 || word_boundary((unsigned char)input[i - 1]))) {
            if (append_placeholder(output, output_size, &used) != 0) return 0;
            i = skip_number(input, i);
            continue;
        }
        if (c == '.' && isdigit((unsigned char)input[i + 1]) &&
            (i == 0 || (input[i - 1] != '.' &&
                        word_boundary((unsigned char)input[i - 1])))) {
            if (append_placeholder(output, output_size, &used) != 0) return 0;
            i = skip_number(input, i);
            continue;
        }
        if (identifier_start(c)) {
            size_t start = i++;
            while (identifier_cont((unsigned char)input[i])) i++;
            size_t n = i - start;
            if (keyword_literal(input + start, n)) {
                if (append_placeholder(output, output_size, &used) != 0) return 0;
            } else {
                for (size_t j = start; j < i; j++)
                    if (append_char(output, output_size, &used,
                                    input[j]) != 0) return 0;
            }
            continue;
        }
        if (tight_punctuation(c) && used && output[used - 1] == ' ')
            output[--used] = '\0';
        if (append_char(output, output_size, &used, input[i++]) != 0)
            return 0;
    }
    if (block_depth)
        return 0;
    while (used && output[used - 1] == ' ')
        output[--used] = '\0';
    return used;
}

static uint64_t fnv1a(uint64_t hash, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t pgwt_pg13_synthetic_key(uint32_t databaseid, uint32_t userid,
                                 const char *normalized)
{
    if (!databaseid || !userid || !normalized || !normalized[0])
        return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    static const char domain[] = PGWT_PG13_SYNTH_VERSION;
    hash = fnv1a(hash, domain, sizeof(domain)); /* include terminating NUL */
    /* Fixed network byte order makes the versioned key architecture-stable. */
    const unsigned char context[8] = {
        (unsigned char)(databaseid >> 24),
        (unsigned char)(databaseid >> 16),
        (unsigned char)(databaseid >> 8),
        (unsigned char)databaseid,
        (unsigned char)(userid >> 24),
        (unsigned char)(userid >> 16),
        (unsigned char)(userid >> 8),
        (unsigned char)userid,
    };
    hash = fnv1a(hash, context, sizeof(context));
    hash = fnv1a(hash, normalized, strlen(normalized));
    return hash ? hash : 1; /* zero is reserved for unattributed samples */
}

int pgwt_pg13_synthetic_query(uint32_t databaseid, uint32_t userid,
                              const char *activity, char *normalized,
                              size_t normalized_size, uint64_t *key)
{
    if (key) *key = 0;
    size_t n = pgwt_pg13_normalize_query(activity, normalized,
                                          normalized_size);
    if (!n || !key)
        return -1;
    *key = pgwt_pg13_synthetic_key(databaseid, userid, normalized);
    return *key ? 0 : -1;
}

int pgwt_pg13_synthetic_cached(struct pgwt_pg13_synthetic_cache *cache,
                               uint32_t databaseid, uint32_t userid,
                               const char *activity,
                               const char **normalized, uint64_t *key,
                               bool *cache_hit)
{
    if (normalized) *normalized = NULL;
    if (key) *key = 0;
    if (cache_hit) *cache_hit = false;
    if (!cache || !databaseid || !userid || !activity || !activity[0] ||
        !normalized || !key)
        return -1;
    if (cache->valid && cache->databaseid == databaseid &&
        cache->userid == userid && strcmp(cache->activity, activity) == 0) {
        *normalized = cache->normalized;
        *key = cache->query_id;
        if (cache_hit) *cache_hit = true;
        return 0;
    }
    uint64_t fresh = 0;
    if (pgwt_pg13_synthetic_query(databaseid, userid, activity,
                                  cache->normalized,
                                  sizeof(cache->normalized), &fresh) != 0) {
        cache->valid = false;
        return -1;
    }
    size_t len = strnlen(activity, sizeof(cache->activity));
    if (!len || len >= sizeof(cache->activity)) {
        cache->valid = false;
        return -1;
    }
    memcpy(cache->activity, activity, len + 1);
    cache->databaseid = databaseid;
    cache->userid = userid;
    cache->query_id = fresh;
    cache->valid = true;
    *normalized = cache->normalized;
    *key = fresh;
    return 0;
}
