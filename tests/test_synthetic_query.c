#include "synthetic_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run, passed;
#define CHECK(c, msg) do { run++; if (c) passed++; else printf("  FAIL: %s\n", msg); } while (0)

int main(void)
{
    char a[PGWT_PG13_SYNTH_TEXT_MAX], b[PGWT_PG13_SYNTH_TEXT_MAX];
    uint64_t ka = 0, kb = 0;
    CHECK(pgwt_pg13_synthetic_query(5, 10,
          "/* lead */ SELECT 42, 'secret', TRUE -- tail\n FROM t WHERE x=1.25e3",
          a, sizeof(a), &ka) == 0, "normalization succeeds");
    CHECK(strcmp(a, "SELECT ?,?,? FROM t WHERE x=?") == 0,
          "comments, whitespace and literals normalize");
    CHECK(pgwt_pg13_synthetic_query(5, 10,
          "SELECT 7,E'another',false FROM t WHERE x=99", b, sizeof(b), &kb) == 0 &&
          ka == kb, "literal variants share a stable key");
    CHECK(pgwt_pg13_synthetic_key(5, 11, a) != ka &&
          pgwt_pg13_synthetic_key(6, 10, a) != ka,
          "database/user context participates in the key");
    CHECK(strstr(PGWT_PG13_SYNTH_VERSION, "v1") != NULL,
          "synthetic algorithm is explicitly versioned");
    CHECK(ka == UINT64_C(0x4ebbec33d5a1c89c),
          "versioned key matches the architecture-independent known vector");
    CHECK(pgwt_pg13_normalize_query("SELECT $$unterminated", b, sizeof(b)) == 0,
          "incomplete activity is rejected");
    CHECK(pgwt_pg13_normalize_query("/* unterminated", b, sizeof(b)) == 0,
          "truncated comment is rejected");
    CHECK(pgwt_pg13_normalize_query(
              "SELECT \"a/*not-comment*/'42\", \"x\"\"--y\" FROM t WHERE n=7",
              b, sizeof(b)) > 0 &&
          strcmp(b,
                 "SELECT \"a/*not-comment*/'42\",\"x\"\"--y\" FROM t WHERE n=?") == 0,
          "double-quoted identifiers are copied without literal/comment parsing");
    CHECK(pgwt_pg13_normalize_query("SELECT \"unterminated", b,
                                    sizeof(b)) == 0,
          "truncated quoted identifier is rejected");
    CHECK(pgwt_pg13_normalize_query("SELECT \xc3\xa9" "1 FROM t", a,
                                    sizeof(a)) > 0 &&
          strcmp(a, "SELECT \xc3\xa9" "1 FROM t") == 0,
          "UTF-8 identifier continuations retain following digits");
    CHECK(pgwt_pg13_normalize_query("SELECT \xc3\xa9" "2 FROM t", b,
                                    sizeof(b)) > 0 &&
          pgwt_pg13_synthetic_key(5, 10, a) !=
          pgwt_pg13_synthetic_key(5, 10, b),
          "distinct UTF-8 identifiers do not collapse to one key");
    CHECK(pgwt_pg13_normalize_query(
              "SELECT $\xc3\xa9$secret$\xc3\xa9$", b, sizeof(b)) > 0 &&
          strcmp(b, "SELECT ?") == 0,
          "UTF-8 dollar-quote tags are replaced as literals");
    CHECK(pgwt_pg13_normalize_query("SELECT 4., .001, 1.e2, 7..x", b,
                                    sizeof(b)) > 0 &&
          strcmp(b, "SELECT ?,?,?,?..x") == 0,
          "leading/trailing-dot numerics normalize without consuming dot-dot");
    CHECK(pgwt_pg13_synthetic_query(0, 10, "SELECT 1", b, sizeof(b), &kb) != 0,
          "missing context stays unattributed");
    struct pgwt_pg13_synthetic_cache cache = {0};
    const char *cached_text = NULL;
    bool hit = false;
    CHECK(pgwt_pg13_synthetic_cached(&cache, 5, 10, "SELECT 42",
                                     &cached_text, &kb, &hit) == 0 && !hit &&
          strcmp(cached_text, "SELECT ?") == 0,
          "first activity sighting populates the exact sampler cache");
    CHECK(pgwt_pg13_synthetic_cached(&cache, 5, 10, "SELECT 42",
                                     &cached_text, &ka, &hit) == 0 && hit &&
          ka == kb,
          "unchanged activity reuses normalized text and key");
    CHECK(pgwt_pg13_synthetic_cached(&cache, 5, 10, "SELECT 43",
                                     &cached_text, &ka, &hit) == 0 && !hit &&
          ka == kb,
          "changed literal is normalized again but retains stable grouping");
    CHECK(pgwt_pg13_synthetic_cached(&cache, 6, 10, "SELECT 43",
                                     &cached_text, &ka, &hit) == 0 && !hit &&
          ka != kb,
          "context change invalidates the per-backend cache");
    CHECK(pgwt_pg13_synthetic_cached(&cache, 6, 10,
                                     "SELECT $$unterminated",
                                     &cached_text, &ka, &hit) != 0 &&
          !cache.valid,
          "failed re-normalization invalidates the old cache entry");
    CHECK(pgwt_pg13_synthetic_cached(&cache, 6, 10, "SELECT 43",
                                     &cached_text, &ka, &hit) == 0 && !hit &&
          strcmp(cached_text, "SELECT ?") == 0,
          "post-failure lookup re-normalizes instead of returning stale text");

    size_t long_len = PGWT_PG13_SYNTH_TEXT_MAX + 16;
    char *long_activity = malloc(long_len + 1);
    CHECK(long_activity != NULL, "long-activity fixture allocates");
    if (long_activity) {
        long_activity[0] = '/';
        long_activity[1] = '*';
        memset(long_activity + 2, 'x', long_len - 12);
        memcpy(long_activity + long_len - 10, "*/SELECT 1", 10);
        long_activity[long_len] = '\0';
        CHECK(pgwt_pg13_synthetic_cached(&cache, 6, 10, long_activity,
                                         &cached_text, &ka, &hit) != 0 &&
              !cache.valid,
              "raw-activity length guard also invalidates the old cache entry");
        free(long_activity);
    }
    printf("%d/%d tests passed\n", passed, run);
    return passed == run ? 0 : 1;
}
