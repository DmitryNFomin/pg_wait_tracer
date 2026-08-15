/* test_backend_status_layout.c -- PgBackendStatus discovery/runtime tests. */
#define _GNU_SOURCE
#include "backend_status_layout.h"

#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run = 0, ok = 0;
#define CHECK(c, ...) do { \
        run++; \
        if (c) ok++; \
        else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

/* Emit the subset of `readelf --debug-dump=info --wide` used by the parser.
 * Type references deliberately point forward, exercising typedef/type
 * following rather than relying on declaration order. */
static void emit_layout(FILE *fp, unsigned base, int major, unsigned query_off,
                        int bad_location, int omit_activity, int overlap,
                        int anonymous_struct, unsigned size_override)
{
    int running = major >= 18 ? 3 : 2;
    int fastpath = major >= 18 ? 5 : 4;
    int disabled = major >= 18 ? 7 : 6;
    unsigned struct_size = major >= 18 ? 440 : (major >= 14 ? 432 : 424);

    fprintf(fp, " <1><%x>: Abbrev Number: 1 (DW_TAG_structure_type)\n",
            base);
    if (!anonymous_struct)
        fprintf(fp, "    <%x> DW_AT_name : PgBackendStatus\n", base + 1);
    fprintf(fp, "    <%x> DW_AT_byte_size : %u\n", base + 2,
            size_override ? size_override : struct_size);
#define MEMBER(id_, name_, type_, off_) do { \
        fprintf(fp, \
                " <2><%x>: Abbrev Number: 2 (DW_TAG_member)\n" \
                "    <%x> DW_AT_name : %s\n" \
                "    <%x> DW_AT_type : <0x%x>\n", \
                base + (id_), base + (id_) + 1, (name_), \
                base + (id_) + 2, base + (type_)); \
        if (bad_location && strcmp((name_), "st_state") == 0) \
            fprintf(fp, "    <%x> DW_AT_data_member_location: " \
                    "1 byte block: 23 (DW_OP_plus_uconst: %u)\n", \
                    base + (id_) + 3, (unsigned)(off_)); \
        else \
            fprintf(fp, "    <%x> DW_AT_data_member_location: %u\n", \
                    base + (id_) + 3, (unsigned)(off_)); \
    } while (0)
    MEMBER(0x10, "st_changecount", 0x300, 0);
    MEMBER(0x20, "st_procpid", 0x300, 4);
    MEMBER(0x30, "st_databaseid", 0x300, 48);
    MEMBER(0x40, "st_userid", 0x300, 52);
    MEMBER(0x50, "st_state", 0x200, overlap ? 52 : 232);
    if (!omit_activity)
        MEMBER(0x60, "st_activity_raw", 0x310, 248);
    if (major >= 14)
        MEMBER(0x70, "st_query_id", 0x320, query_off);
#undef MEMBER

    fprintf(fp,
            " <1><%x>: Abbrev Number: 3 (DW_TAG_enumeration_type)\n"
            "    <%x> DW_AT_byte_size : 4\n"
            " <2><%x>: Abbrev Number: 4 (DW_TAG_enumerator)\n"
            "    <%x> DW_AT_name : STATE_RUNNING\n"
            "    <%x> DW_AT_const_value : %d\n"
            " <2><%x>: Abbrev Number: 4 (DW_TAG_enumerator)\n"
            "    <%x> DW_AT_name : STATE_FASTPATH\n"
            "    <%x> DW_AT_const_value : %d\n"
            " <2><%x>: Abbrev Number: 4 (DW_TAG_enumerator)\n"
            "    <%x> DW_AT_name : STATE_DISABLED\n"
            "    <%x> DW_AT_const_value : %d\n",
            base + 0x200, base + 0x201,
            base + 0x210, base + 0x211, base + 0x212, running,
            base + 0x220, base + 0x221, base + 0x222, fastpath,
            base + 0x230, base + 0x231, base + 0x232, disabled);
    fprintf(fp,
            " <1><%x>: Abbrev Number: 5 (DW_TAG_base_type)\n"
            "    <%x> DW_AT_byte_size : 4\n"
            "    <%x> DW_AT_encoding : 5 (signed)\n"
            " <1><%x>: Abbrev Number: 6 (DW_TAG_pointer_type)\n"
            "    <%x> DW_AT_byte_size : 8\n"
            " <1><%x>: Abbrev Number: 5 (DW_TAG_base_type)\n"
            "    <%x> DW_AT_byte_size : 8\n"
            "    <%x> DW_AT_encoding : %d (%s)\n"
            " <1><%x>: Abbrev Number: 7 (DW_TAG_typedef)\n"
            "    <%x> DW_AT_name : PgBackendStatus\n"
            "    <%x> DW_AT_type : <0x%x>\n",
            base + 0x300, base + 0x301, base + 0x302,
            base + 0x310, base + 0x311,
            base + 0x320, base + 0x321, base + 0x322,
            major >= 18 ? 5 : 7, major >= 18 ? "signed" : "unsigned",
            base + 0x400, base + 0x401, base + 0x402, base);
}

static int parse_one(int major, int bad_location, int omit_activity,
                     int overlap, struct PgBackendStatusLayout *layout)
{
    FILE *fp = tmpfile();
    if (!fp)
        return -1;
    emit_layout(fp, 0x1000, major, 424, bad_location, omit_activity, overlap,
                0, 0);
    rewind(fp);
    char why[192];
    int rc = pgwt_pgbs_parse_dwarf(fp, major, 8, layout, why, sizeof(why));
    fclose(fp);
    return rc;
}

static void test_dwarf(void)
{
    printf("--- strict DWARF member resolution ---\n");
    struct PgBackendStatusLayout layout;
    CHECK(parse_one(17, 0, 0, 0, &layout) == 0,
          "constant member offsets accepted");
    CHECK(layout.source == PGWT_PGBS_SOURCE_DWARF,
          "accepted layout source is DWARF");
    CHECK(layout.st_state.offset == 232 && layout.st_state.width == 4,
          "st_state offset/width resolved");
    CHECK(layout.st_activity_raw.offset == 248 &&
          layout.st_activity_raw.width == 8,
          "st_activity_raw offset/width resolved");
    CHECK(layout.st_query_id.offset == 424 &&
          layout.st_query_id.width == 8 && !layout.st_query_id_signed,
          "PG17 unsigned st_query_id resolved");
    CHECK(layout.state_running == 2 && layout.state_fastpath == 4 &&
          layout.state_max == 6, "BackendState constants resolved");

    FILE *typedef_fp = tmpfile();
    CHECK(typedef_fp != NULL, "anonymous typedef fixture created");
    if (typedef_fp) {
        emit_layout(typedef_fp, 0x1000, 17, 424, 0, 0, 0, 1, 0);
        rewind(typedef_fp);
        char why[192];
        CHECK(pgwt_pgbs_parse_dwarf(typedef_fp, 17, 8, &layout,
                                    why, sizeof(why)) == 0 &&
              layout.st_query_id.offset == 424,
              "PgBackendStatus typedef followed to anonymous struct");
        fclose(typedef_fp);
    }

    CHECK(parse_one(18, 0, 0, 0, &layout) == 0 &&
          layout.st_query_id_signed && layout.state_running == 3 &&
          layout.state_fastpath == 5,
          "PG18 signed query id and shifted enum resolved");
    CHECK(parse_one(13, 0, 0, 0, &layout) == 0 &&
          !layout.st_query_id.present &&
          layout.st_query_id.validation == PGWT_PGBS_FIELD_ABSENT,
          "PG13 query id is explicitly ABSENT");

    CHECK(parse_one(17, 1, 0, 0, &layout) != 0,
          "location expression rejected");
    CHECK(parse_one(17, 0, 1, 0, &layout) != 0,
          "missing mandatory member rejected");
    CHECK(parse_one(17, 0, 0, 1, &layout) != 0,
          "overlapping members rejected");

    FILE *small_fp = tmpfile();
    CHECK(small_fp != NULL, "implausible-size fixture created");
    if (small_fp) {
        emit_layout(small_fp, 0x1000, 17, 424, 0, 0, 0, 0, 16);
        rewind(small_fp);
        char why[192];
        CHECK(pgwt_pgbs_parse_dwarf(small_fp, 17, 8, &layout,
                                    why, sizeof(why)) != 0,
              "implausible struct size rejected");
        fclose(small_fp);
    }

    FILE *fp = tmpfile();
    CHECK(fp != NULL, "ambiguous fixture created");
    if (fp) {
        emit_layout(fp, 0x1000, 17, 424, 0, 0, 0, 0, 0);
        emit_layout(fp, 0x2000, 17, 416, 0, 0, 0, 0, 0);
        rewind(fp);
        char why[192];
        CHECK(pgwt_pgbs_parse_dwarf(fp, 17, 8, &layout,
                                    why, sizeof(why)) != 0 &&
              strstr(why, "ambiguous") != NULL,
              "conflicting duplicate structs rejected as ambiguous");
        fclose(fp);
    }
}

static void test_hard_table(void)
{
    printf("--- per-major/architecture hard table ---\n");
    const enum pgwt_pgbs_arch arches[] = {
        PGWT_PGBS_ARCH_X86_64, PGWT_PGBS_ARCH_ARM64
    };
    const enum pgwt_pgbs_abi abis[] = {
        PGWT_PGBS_ABI_SYSV_LP64, PGWT_PGBS_ABI_AAPCS64_LP64
    };
    for (size_t a = 0; a < 2; a++) {
        for (int major = 13; major <= 18; major++) {
            struct PgBackendStatusLayout layout;
            CHECK(pgwt_pgbs_hard_table_lookup(major, arches[a], 8, abis[a],
                                               &layout) == 0,
                  "PG%d %s row exists", major, pgwt_pgbs_arch_name(arches[a]));
            CHECK(layout.st_changecount.offset == 0 &&
                  layout.st_changecount.width == 4 &&
                  layout.st_procpid.offset == 4 &&
                  layout.st_procpid.width == 4 &&
                  layout.st_databaseid.offset == 48 &&
                  layout.st_databaseid.width == 4 &&
                  layout.st_userid.offset == 52 &&
                  layout.st_userid.width == 4 &&
                  layout.st_state.offset == 232 &&
                  layout.st_state.width == 4 &&
                  layout.st_activity_raw.offset == 248 &&
                  layout.st_activity_raw.width == 8,
                  "PG%d %s common fields match derived layout",
                  major, pgwt_pgbs_arch_name(arches[a]));
            CHECK((major == 13 && !layout.st_query_id.present) ||
                  (major >= 14 && layout.st_query_id.present &&
                   layout.st_query_id.offset == 424 &&
                   layout.st_query_id.width == 8),
                  "PG%d %s query-id presence/shape", major,
                  pgwt_pgbs_arch_name(arches[a]));
            CHECK(layout.st_query_id_signed == (major >= 18),
                  "PG%d %s query-id signedness", major,
                  pgwt_pgbs_arch_name(arches[a]));
            CHECK(layout.state_running == (major >= 18 ? 3 : 2) &&
                  layout.state_fastpath == (major >= 18 ? 5 : 4),
                  "PG%d %s state enum", major,
                  pgwt_pgbs_arch_name(arches[a]));
        }
    }
    struct PgBackendStatusLayout layout;
    CHECK(pgwt_pgbs_hard_table_lookup(19, PGWT_PGBS_ARCH_X86_64, 8,
                                      PGWT_PGBS_ABI_SYSV_LP64, &layout) != 0,
          "unknown major has no row");
    CHECK(pgwt_pgbs_hard_table_lookup(17, PGWT_PGBS_ARCH_X86_64, 4,
                                      PGWT_PGBS_ABI_SYSV_LP64, &layout) != 0,
          "wrong pointer width has no row");
    CHECK(pgwt_pgbs_hard_table_lookup(17, PGWT_PGBS_ARCH_ARM64, 8,
                                      PGWT_PGBS_ABI_SYSV_LP64, &layout) != 0,
          "wrong ABI has no row");
}

static struct pgwt_pgbs_snapshot good_snapshot(void)
{
    return (struct pgwt_pgbs_snapshot) {
        .changecount_before = 8,
        .changecount_after = 8,
        .procpid = 4242,
        .databaseid = 16384,
        .userid = 10,
        .state = 2,
        .activity_raw = 0x100000,
        .query_id = 0xf123456789abcdefULL,
        .read_mask = PGWT_PGBS_READ_CHANGECOUNT | PGWT_PGBS_READ_PROCPID |
                     PGWT_PGBS_READ_DATABASEID | PGWT_PGBS_READ_USERID |
                     PGWT_PGBS_READ_STATE | PGWT_PGBS_READ_ACTIVITY |
                     PGWT_PGBS_READ_QUERY_ID,
        .activity_readable = true,
        .activity_marker_matched = true,
    };
}

static void test_validation(void)
{
    printf("--- coherency, shadow comparison and degrade paths ---\n");
    struct PgBackendStatusLayout base;
    CHECK(pgwt_pgbs_hard_table_lookup(17, PGWT_PGBS_ARCH_X86_64, 8,
                                      PGWT_PGBS_ABI_SYSV_LP64, &base) == 0,
          "validation fixture row");
    struct pgwt_pgbs_expected expected = {
        .pid = 4242, .databaseid = 16384, .userid = 10,
        .require_running = true, .state_shadow_available = true,
        .state_active = true, .activity_marker_required = true,
        .query_id_available = true,
        .query_id = 0xf123456789abcdefULL,
    };
    struct pgwt_pgbs_snapshot snapshot = good_snapshot();
    struct PgBackendStatusLayout layout = base;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) == 0,
          "stable even coherent snapshot validates");
    CHECK(layout.validation == PGWT_PGBS_VALIDATION_VALIDATED &&
          layout.fallback_mask == 0 && layout.validated_pid == 4242,
          "fully validated descriptor has no fallback");

    layout = base;
    snapshot = good_snapshot();
    snapshot.changecount_after = 9;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.fallback_mask ==
              (PGWT_PGBS_FALLBACK_STATE |
               PGWT_PGBS_FALLBACK_ACTIVITY_RAW |
               PGWT_PGBS_FALLBACK_QUERY_ID),
          "changed/odd changecount rejects dependent fields");

    layout = base;
    snapshot = good_snapshot();
    snapshot.state = 99;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.fallback_mask == PGWT_PGBS_FALLBACK_STATE &&
          layout.st_activity_raw.validation == PGWT_PGBS_FIELD_VALIDATED &&
          layout.st_query_id.validation == PGWT_PGBS_FIELD_VALIDATED,
          "invalid state degrades only the activity-state consumer");

    layout = base;
    snapshot = good_snapshot();
    snapshot.query_id++;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.fallback_mask == PGWT_PGBS_FALLBACK_QUERY_ID,
          "query shadow mismatch keeps query-id uprobe only");

    layout = base;
    snapshot = good_snapshot();
    snapshot.query_id = 0;
    expected.query_id = 0;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.st_query_id.validation == PGWT_PGBS_FIELD_INVALID &&
          layout.fallback_mask == PGWT_PGBS_FALLBACK_QUERY_ID,
          "coincidental query-id 0==0 never validates a field offset");
    expected.query_id = 0xf123456789abcdefULL;

    layout = base;
    snapshot = good_snapshot();
    snapshot.state = 0;
    expected.require_running = false;
    expected.state_active = false;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.st_state.validation == PGWT_PGBS_FIELD_INVALID &&
          layout.fallback_mask == PGWT_PGBS_FALLBACK_STATE,
          "idle-only state agreement never validates a field offset");
    expected.require_running = true;
    expected.state_active = true;

    layout = base;
    snapshot = good_snapshot();
    snapshot.activity_readable = false;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.fallback_mask == PGWT_PGBS_FALLBACK_ACTIVITY_RAW,
          "unreadable activity pointer degrades independently");

    layout = base;
    snapshot = good_snapshot();
    snapshot.activity_marker_matched = false;
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.fallback_mask == PGWT_PGBS_FALLBACK_ACTIVITY_RAW,
          "readable pointer without the controlled marker is rejected");

    layout = base;
    layout.st_query_id.present = false;
    layout.st_query_id.offset = 0;
    snapshot = good_snapshot();
    CHECK(pgwt_pgbs_validate_snapshot(&layout, &snapshot, &expected) != 0 &&
          layout.fallback_mask == PGWT_PGBS_FALLBACK_QUERY_ID &&
          layout.st_query_id.offset == 0,
          "missing field records fallback and never invents an offset");
}

static void validate_sampled_fields(struct PgBackendStatusLayout *layout)
{
    layout->validation = PGWT_PGBS_VALIDATION_VALIDATED;
    layout->st_changecount.validation = PGWT_PGBS_FIELD_VALIDATED;
    layout->st_procpid.validation = PGWT_PGBS_FIELD_VALIDATED;
    layout->st_databaseid.validation = PGWT_PGBS_FIELD_VALIDATED;
    layout->st_userid.validation = PGWT_PGBS_FIELD_VALIDATED;
    layout->st_state.validation = PGWT_PGBS_FIELD_VALIDATED;
    layout->st_query_id.validation = PGWT_PGBS_FIELD_VALIDATED;
    layout->st_activity_raw.validation = PGWT_PGBS_FIELD_VALIDATED;
}

static void test_sampled_attr(void)
{
    printf("--- Stage 2 coherent sampled attribution ---\n");
    struct PgBackendStatusLayout layout;
    CHECK(pgwt_pgbs_hard_table_lookup(18, PGWT_PGBS_ARCH_X86_64, 8,
                                      PGWT_PGBS_ABI_SYSV_LP64,
                                      &layout) == 0,
          "PG18 sampled-attribution fixture row");
    validate_sampled_fields(&layout);
    CHECK(pgwt_pgbs_sampled_attr_enabled(&layout),
          "validated PG18 layout enables at-tick source");

    struct pgwt_pgbs_snapshot snapshot = {
        .changecount_before = 12,
        .changecount_after = 12,
        .procpid = (uint32_t)getpid(),
        .databaseid = 16384,
        .userid = 10,
        .state = 3,                 /* PG18 STATE_RUNNING */
        .query_id = 0xf123456789abcdefULL,
        .read_mask = PGWT_PGBS_READ_CHANGECOUNT |
                     PGWT_PGBS_READ_PROCPID |
                     PGWT_PGBS_READ_DATABASEID |
                     PGWT_PGBS_READ_USERID |
                     PGWT_PGBS_READ_STATE |
                     PGWT_PGBS_READ_QUERY_ID,
    };
    struct pgwt_pgbs_sampled_attr attr = {0};
    CHECK(pgwt_pgbs_derive_sampled_attr(&layout, &snapshot, getpid(),
                                        &attr) == 0 &&
          attr.cmd_open && attr.state == 3 && attr.databaseid == 16384 &&
          attr.userid == 10 &&
          attr.query_id == snapshot.query_id,
          "PG18 RUNNING=3 admits CPU and preserves signed query-id bits");

    snapshot.state = 5;             /* PG18 STATE_FASTPATH */
    CHECK(pgwt_pgbs_derive_sampled_attr(&layout, &snapshot, getpid(),
                                        &attr) == 0 &&
          attr.cmd_open && attr.state == 5 &&
          attr.query_id == snapshot.query_id,
          "PG18 FASTPATH=5 is command-open");

    snapshot.state = 1;             /* any validated idle state */
    CHECK(pgwt_pgbs_derive_sampled_attr(&layout, &snapshot, getpid(),
                                        &attr) == 0 &&
          !attr.cmd_open && attr.query_id == 0,
          "idle state closes admission and normalizes query-id to zero");

    /* A failed derivation starts by clearing the destination.  Seed it with
     * stale command state to prove an incoherent tick cannot reuse it. */
    attr.cmd_open = true;
    attr.query_id = UINT64_MAX;
    snapshot.state = 3;
    snapshot.changecount_after = 14;
    CHECK(pgwt_pgbs_derive_sampled_attr(&layout, &snapshot, getpid(),
                                        &attr) != 0 &&
          !attr.cmd_open && attr.query_id == 0,
          "incoherent read drops state instead of reusing the prior tick");
    snapshot.changecount_after = snapshot.changecount_before;

    attr.cmd_open = true;
    attr.query_id = UINT64_MAX;
    snapshot.procpid++;
    CHECK(pgwt_pgbs_derive_sampled_attr(&layout, &snapshot, getpid(),
                                        &attr) != 0 &&
          !attr.cmd_open && attr.query_id == 0,
          "PID identity mismatch drops state");
    snapshot.procpid = (uint32_t)getpid();

    /* Exercise the live /proc/<pid>/mem wrapper against a synthetic
     * PgBackendStatus row in this process. */
    uint8_t row[440];
    memset(row, 0, sizeof(row));
    uint32_t changecount = 20;
    uint32_t pid = (uint32_t)getpid();
    uint32_t databaseid = 16384;
    uint32_t userid = 10;
    uint32_t state = 5;
    uint64_t query_id = 0x7123456789abcdefULL;
    memcpy(row + layout.st_changecount.offset, &changecount,
           sizeof(changecount));
    memcpy(row + layout.st_procpid.offset, &pid, sizeof(pid));
    memcpy(row + layout.st_databaseid.offset, &databaseid,
           sizeof(databaseid));
    memcpy(row + layout.st_userid.offset, &userid, sizeof(userid));
    memcpy(row + layout.st_state.offset, &state, sizeof(state));
    memcpy(row + layout.st_query_id.offset, &query_id, sizeof(query_id));
    uint64_t my_be_entry = (uint64_t)(uintptr_t)row;
    CHECK(pgwt_pgbs_read_sampled_attr(getpid(),
                                      (uint64_t)(uintptr_t)&my_be_entry,
                                      &layout, &attr) == 0 &&
          attr.cmd_open && attr.state == 5 && attr.query_id == query_id,
          "live reader dereferences MyBEEntry and reuses coherent helper");

    layout.validation = PGWT_PGBS_VALIDATION_DEGRADED;
    CHECK(!pgwt_pgbs_sampled_attr_enabled(&layout),
          "degraded layout keeps the uprobe-map fallback");
    layout.validation = PGWT_PGBS_VALIDATION_VALIDATED;
    layout.st_state.validation = PGWT_PGBS_FIELD_INVALID;
    CHECK(!pgwt_pgbs_sampled_attr_enabled(&layout),
          "invalid st_state field keeps the uprobe-map fallback");
    layout.st_state.validation = PGWT_PGBS_FIELD_VALIDATED;
    layout.st_query_id.validation = PGWT_PGBS_FIELD_INVALID;
    CHECK(!pgwt_pgbs_sampled_attr_enabled(&layout),
          "invalid st_query_id field keeps the uprobe-map fallback");

    CHECK(pgwt_pgbs_hard_table_lookup(13, PGWT_PGBS_ARCH_X86_64, 8,
                                      PGWT_PGBS_ABI_SYSV_LP64,
                                      &layout) == 0,
          "PG13 fallback fixture row");
    validate_sampled_fields(&layout);
    layout.st_query_id.validation = PGWT_PGBS_FIELD_ABSENT;
    CHECK(!pgwt_pgbs_sampled_activity_enabled(&layout),
          "PG13 without a validated activity capacity keeps fail-safe probes");
    layout.activity_buffer_size = 64;
    layout.status_anchor = (uint64_t)(uintptr_t)row;
    CHECK(pgwt_pgbs_sampled_attr_enabled(&layout) &&
          pgwt_pgbs_sampled_activity_enabled(&layout),
          "validated PG13 enables the activity-text at-tick route");

    snapshot = good_snapshot();
    snapshot.procpid = (uint32_t)getpid();
    snapshot.state = 2;
    snapshot.query_id = 0;
    CHECK(pgwt_pgbs_derive_sampled_activity(
              &layout, &snapshot, getpid(), "SELECT 42", false, &attr) == 0 &&
          attr.cmd_open && attr.query_id == 0 &&
          strcmp(attr.activity, "SELECT 42") == 0 &&
          attr.databaseid == 16384 && attr.userid == 10,
          "PG13 coherent activity/context is returned while RUNNING");
    CHECK(pgwt_pgbs_derive_sampled_activity(
              &layout, &snapshot, getpid(), "SELECT 42", true, &attr) == 0 &&
          attr.cmd_open && attr.activity[0] == '\0' &&
          attr.activity_truncated,
          "PG13 truncated activity keeps the command but leaves it unattributed");
    CHECK(pgwt_pgbs_derive_sampled_activity(
              &layout, &snapshot, getpid(), "", false, &attr) == 0 &&
          attr.cmd_open && attr.activity[0] == '\0',
          "PG13 empty activity is coherently unattributed");
    snapshot.activity_readable = false;
    CHECK(pgwt_pgbs_derive_sampled_activity(
              &layout, &snapshot, getpid(), "SELECT 42", false, &attr) == 0 &&
          attr.cmd_open && attr.activity[0] == '\0',
          "PG13 unreadable activity is coherently unattributed");
    snapshot.activity_readable = true;
    snapshot.state = (uint32_t)layout.state_max;
    CHECK(pgwt_pgbs_derive_sampled_activity(
              &layout, &snapshot, getpid(), "SELECT 42", false, &attr) == 0 &&
          !attr.cmd_open && attr.activity[0] == '\0',
          "PG13 disabled state is command-closed and unattributed");
    snapshot.state = 2;

    memset(row, 0, sizeof(row));
    changecount = 24;
    state = 2;
    char activity[64] = "SELECT 99";
    layout.activity_buffer_size = sizeof(activity);
    uint64_t activity_ptr = (uint64_t)(uintptr_t)activity;
    memcpy(row + layout.st_changecount.offset, &changecount,
           sizeof(changecount));
    memcpy(row + layout.st_procpid.offset, &pid, sizeof(pid));
    memcpy(row + layout.st_databaseid.offset, &databaseid,
           sizeof(databaseid));
    memcpy(row + layout.st_userid.offset, &userid, sizeof(userid));
    memcpy(row + layout.st_state.offset, &state, sizeof(state));
    memcpy(row + layout.st_activity_raw.offset, &activity_ptr,
           sizeof(activity_ptr));
    my_be_entry = (uint64_t)(uintptr_t)row;
    CHECK(pgwt_pgbs_read_sampled_attr(getpid(),
                                      (uint64_t)(uintptr_t)&my_be_entry,
                                      &layout, &attr) == 0 &&
          attr.cmd_open && strcmp(attr.activity, "SELECT 99") == 0,
          "PG13 live reader coherently dereferences st_activity_raw");
    memset(activity, '7', sizeof(activity));
    CHECK(pgwt_pgbs_read_sampled_attr_at(
              getpid(), (uint64_t)(uintptr_t)row, &layout, &attr) == 0 &&
          attr.cmd_open && attr.activity_truncated && !attr.activity[0],
          "PG13 direct-row reader rejects capacity-truncated activity");
}

static void test_warmup_aggregation(void)
{
    printf("--- bounded warmup coincidence resistance ---\n");
    struct PgBackendStatusLayout base;
    CHECK(pgwt_pgbs_hard_table_lookup(17, PGWT_PGBS_ARCH_X86_64, 8,
                                      PGWT_PGBS_ABI_SYSV_LP64, &base) == 0,
          "warmup fixture row");
    struct PgBackendStatusLayout candidate = base;
    candidate.validated_pid = 4242;
    candidate.st_state.validation = PGWT_PGBS_FIELD_VALIDATED;
    candidate.st_activity_raw.validation = PGWT_PGBS_FIELD_VALIDATED;
    candidate.st_query_id.validation = PGWT_PGBS_FIELD_VALIDATED;

    struct pgwt_pgbs_warmup_evidence evidence = {0};
    for (int i = 0; i < 3; i++)
        pgwt_pgbs_warmup_note(&evidence, &candidate, false, true, 0,
                              true, 0);
    CHECK(evidence.observations == 3 && evidence.state_matches == 0 &&
          evidence.query_matches == 0 &&
          !pgwt_pgbs_warmup_complete(&evidence, 17),
          "three idle/zero coincidences do not satisfy warmup");

    memset(&evidence, 0, sizeof(evidence));
    for (int i = 0; i < 3; i++)
        pgwt_pgbs_warmup_note(&evidence, &candidate, true, true, 2,
                              true, 111);
    struct PgBackendStatusLayout result = base;
    pgwt_pgbs_warmup_apply(&result, &evidence);
    CHECK(evidence.state_matches == 3 && evidence.query_matches == 3 &&
          !evidence.state_varied && !evidence.query_id_varied &&
          result.st_state.validation == PGWT_PGBS_FIELD_INVALID &&
          result.st_query_id.validation == PGWT_PGBS_FIELD_INVALID &&
          result.fallback_mask == (PGWT_PGBS_FALLBACK_STATE |
                                   PGWT_PGBS_FALLBACK_QUERY_ID),
          "constant running state and identical query ID remain untrusted");

    memset(&evidence, 0, sizeof(evidence));
    const uint64_t qids[] = {111, 222, 111};
    pgwt_pgbs_warmup_note(&evidence, &candidate, false, true, 0,
                          false, 0);
    for (size_t i = 0; i < sizeof(qids) / sizeof(qids[0]); i++)
        pgwt_pgbs_warmup_note(&evidence, &candidate, true, true, 2,
                              true, qids[i]);
    result = base;
    pgwt_pgbs_warmup_apply(&result, &evidence);
    CHECK(evidence.state_varied &&
          pgwt_pgbs_warmup_complete(&evidence, 17) &&
          result.validation == PGWT_PGBS_VALIDATION_VALIDATED &&
          result.fallback_mask == 0,
          "state transition plus three marker/query matches validate warmup");

    memset(&evidence, 0, sizeof(evidence));
    struct PgBackendStatusLayout markerless = candidate;
    markerless.st_activity_raw.validation = PGWT_PGBS_FIELD_INVALID;
    pgwt_pgbs_warmup_note(&evidence, &markerless, false, true, 0,
                          false, 0);
    for (size_t i = 0; i < sizeof(qids) / sizeof(qids[0]); i++)
        pgwt_pgbs_warmup_note(&evidence, &markerless, true, true, 2,
                              true, qids[i]);
    result = base;
    pgwt_pgbs_warmup_apply(&result, &evidence);
    CHECK(evidence.state_varied && evidence.activity_matches == 0 &&
          result.st_activity_raw.validation == PGWT_PGBS_FIELD_INVALID &&
          result.fallback_mask == PGWT_PGBS_FALLBACK_ACTIVITY_RAW,
          "warmup never blesses activity without content-marker matches");
}

static void test_fail_safe_and_pid_exclusion(void)
{
    printf("--- fail-safe policy, PID exclusion and retirement identity ---\n");
    struct PgBackendStatusLayout layout;
    CHECK(pgwt_pgbs_hard_table_lookup(17, PGWT_PGBS_ARCH_X86_64, 8,
                                      PGWT_PGBS_ABI_SYSV_LP64, &layout) == 0,
          "fail-safe fixture row");
    struct pgwt_pgbs_exclusion_set exclusions = {0};
    pid_t unknown_identity = (pid_t)2147483647;
    CHECK(pgwt_pgbs_exclusion_add(&exclusions, unknown_identity) == 0 &&
          pgwt_pgbs_exclusion_contains(&exclusions, unknown_identity),
          "missing /proc identity remains conservatively excluded");
    struct pgwt_pgbs_exclusion_set full = {
        .count = PGWT_PGBS_MAX_EXCLUDED_PIDS,
    };
    CHECK(pgwt_pgbs_exclusion_add(&full, unknown_identity) != 0,
          "full exclusion set refuses an unrecorded validation PID");
    bool capture_proceeded = false;
    pgwt_pgbs_validate_runtime(&layout, 0, 0, NULL, &exclusions);
    capture_proceeded = true;
    CHECK(capture_proceeded &&
          layout.validation == PGWT_PGBS_VALIDATION_DEGRADED &&
          layout.fallback_mask == (PGWT_PGBS_FALLBACK_STATE |
                                   PGWT_PGBS_FALLBACK_ACTIVITY_RAW |
                                   PGWT_PGBS_FALLBACK_QUERY_ID),
          "validation failure degrades layout and has no capture-abort result");

    pid_t child = fork();
    CHECK(child >= 0, "retirement fixture fork");
    if (child == 0) {
        for (;;)
            pause();
    }
    if (child > 0) {
        uint64_t start_time = 0;
        CHECK(pgwt_pgbs_pid_start_time(child, &start_time) == 0 &&
              start_time != 0,
              "reads /proc start time for backend identity");
        CHECK(pgwt_pgbs_exclusion_add(&exclusions, child) == 0 &&
              pgwt_pgbs_exclusion_contains(&exclusions, child),
              "live validation PID is excluded from capture enrollment");
        CHECK(pgwt_pgbs_pid_is_original(child, start_time) &&
              !pgwt_pgbs_pid_is_original(child, start_time + 1),
              "retirement predicate distinguishes a recycled PID identity");
        kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        CHECK(!pgwt_pgbs_pid_is_original(child, start_time) &&
              !pgwt_pgbs_exclusion_contains(&exclusions, child),
              "retired validation identity no longer excludes a future PID");
    }
}

int main(void)
{
    printf("=== test_backend_status_layout ===\n");
    test_dwarf();
    test_hard_table();
    test_validation();
    test_sampled_attr();
    test_warmup_aggregation();
    test_fail_safe_and_pid_exclusion();
    printf("\n%d/%d tests passed\n", ok, run);
    return ok == run ? 0 : 1;
}
