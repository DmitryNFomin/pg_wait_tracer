/* backend_status_layout.c -- strict PgBackendStatus layout discovery.
 *
 * This is Stage 1 shadow machinery.  Nothing in the BPF program, sampler or
 * query attribution path reads this descriptor.  A candidate layout is useful
 * only after both structural checks and a live coherent read accept it. */
#define _GNU_SOURCE
#include "backend_status_layout.h"
#include "spawn.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* ── Names and native key ─────────────────────────────────── */

enum pgwt_pgbs_arch pgwt_pgbs_arch_from_name(const char *name)
{
    if (!name)
        return PGWT_PGBS_ARCH_UNKNOWN;
    if (strcmp(name, "x86_64") == 0 || strcmp(name, "amd64") == 0)
        return PGWT_PGBS_ARCH_X86_64;
    if (strcmp(name, "aarch64") == 0 || strcmp(name, "arm64") == 0)
        return PGWT_PGBS_ARCH_ARM64;
    return PGWT_PGBS_ARCH_UNKNOWN;
}

enum pgwt_pgbs_abi pgwt_pgbs_abi_for_arch(enum pgwt_pgbs_arch arch,
                                           unsigned pointer_width)
{
    if (pointer_width != 8 || sizeof(long) != 8)
        return PGWT_PGBS_ABI_UNKNOWN;
    if (arch == PGWT_PGBS_ARCH_X86_64)
        return PGWT_PGBS_ABI_SYSV_LP64;
    if (arch == PGWT_PGBS_ARCH_ARM64)
        return PGWT_PGBS_ABI_AAPCS64_LP64;
    return PGWT_PGBS_ABI_UNKNOWN;
}

const char *pgwt_pgbs_arch_name(enum pgwt_pgbs_arch arch)
{
    switch (arch) {
    case PGWT_PGBS_ARCH_X86_64: return "x86_64";
    case PGWT_PGBS_ARCH_ARM64:  return "arm64";
    default:                    return "unknown";
    }
}

const char *pgwt_pgbs_abi_name(enum pgwt_pgbs_abi abi)
{
    switch (abi) {
    case PGWT_PGBS_ABI_SYSV_LP64:    return "sysv-lp64";
    case PGWT_PGBS_ABI_AAPCS64_LP64: return "aapcs64-lp64";
    default:                         return "unknown";
    }
}

const char *pgwt_pgbs_source_name(enum pgwt_pgbs_source source)
{
    switch (source) {
    case PGWT_PGBS_SOURCE_DWARF:      return "DWARF";
    case PGWT_PGBS_SOURCE_HARD_TABLE: return "hard-table";
    default:                          return "none";
    }
}

const char *pgwt_pgbs_validation_name(enum pgwt_pgbs_validation validation)
{
    switch (validation) {
    case PGWT_PGBS_VALIDATION_VALIDATED: return "validated";
    case PGWT_PGBS_VALIDATION_DEGRADED:  return "degraded";
    case PGWT_PGBS_VALIDATION_REJECTED:  return "rejected";
    default:                             return "not-run";
    }
}

static void set_detail(struct PgBackendStatusLayout *layout,
                       const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(layout->detail, sizeof(layout->detail), fmt, ap);
    va_end(ap);
}

static void set_field(struct pgwt_pgbs_field *field, uint32_t offset,
                      uint8_t width, bool present)
{
    field->offset = offset;
    field->width = width;
    field->present = present;
    field->validation = present ? PGWT_PGBS_FIELD_NOT_RUN
                                : PGWT_PGBS_FIELD_ABSENT;
}

/* ── Header-derived hard table ────────────────────────────── */

struct pgwt_pgbs_hard_row {
    int major;
    enum pgwt_pgbs_arch arch;
    enum pgwt_pgbs_abi abi;
    uint8_t pointer_width;
    uint16_t struct_size;
    uint16_t changecount;
    uint8_t changecount_width;
    uint16_t procpid;
    uint8_t procpid_width;
    uint16_t databaseid;
    uint8_t databaseid_width;
    uint16_t userid;
    uint8_t userid_width;
    uint16_t state;
    uint8_t state_width;
    uint16_t activity_raw;
    uint8_t activity_raw_width;
    uint16_t query_id;
    uint8_t query_id_width;
    bool query_present;
    bool query_signed;
    int8_t state_running;
    int8_t state_fastpath;
    int8_t state_max;
};

/* Generated with offsetof/sizeof probes against PGDG server headers:
 *   x86_64: 13.23, 14.24, 15.19, 16.15, 17.10, 18.4
 *   ARM64:  PGDG ARM64 headers, compiled by Clang --target=aarch64-linux-gnu
 *           against an ARM64 Rocky 8 sysroot.  PG13's removed package used
 *           its 13.23 server headers with the native ARM64 PG configuration.
 * Every field width and enum value is part of the key's value.  Do not merge
 * rows across architectures merely because these LP64 results coincide. */
#define PGBS_ROW(pg, architecture, abi_, size_, qpresent_, qsigned_, run_, fast_, max_) \
    { (pg), (architecture), (abi_), 8, (size_), \
      0, 4, 4, 4, 48, 4, 52, 4, 232, 4, 248, 8, \
      (qpresent_) ? 424 : 0, (qpresent_) ? 8 : 0, \
      (qpresent_), (qsigned_), (run_), (fast_), (max_) }

static const struct pgwt_pgbs_hard_row hard_rows[] = {
    PGBS_ROW(13, PGWT_PGBS_ARCH_X86_64, PGWT_PGBS_ABI_SYSV_LP64,
             424, false, false, 2, 4, 6),
    PGBS_ROW(14, PGWT_PGBS_ARCH_X86_64, PGWT_PGBS_ABI_SYSV_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(15, PGWT_PGBS_ARCH_X86_64, PGWT_PGBS_ABI_SYSV_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(16, PGWT_PGBS_ARCH_X86_64, PGWT_PGBS_ABI_SYSV_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(17, PGWT_PGBS_ARCH_X86_64, PGWT_PGBS_ABI_SYSV_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(18, PGWT_PGBS_ARCH_X86_64, PGWT_PGBS_ABI_SYSV_LP64,
             440, true,  true,  3, 5, 7),

    PGBS_ROW(13, PGWT_PGBS_ARCH_ARM64, PGWT_PGBS_ABI_AAPCS64_LP64,
             424, false, false, 2, 4, 6),
    PGBS_ROW(14, PGWT_PGBS_ARCH_ARM64, PGWT_PGBS_ABI_AAPCS64_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(15, PGWT_PGBS_ARCH_ARM64, PGWT_PGBS_ABI_AAPCS64_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(16, PGWT_PGBS_ARCH_ARM64, PGWT_PGBS_ABI_AAPCS64_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(17, PGWT_PGBS_ARCH_ARM64, PGWT_PGBS_ABI_AAPCS64_LP64,
             432, true,  false, 2, 4, 6),
    PGBS_ROW(18, PGWT_PGBS_ARCH_ARM64, PGWT_PGBS_ABI_AAPCS64_LP64,
             440, true,  true,  3, 5, 7),
};

#undef PGBS_ROW

static void layout_from_row(const struct pgwt_pgbs_hard_row *row,
                            struct PgBackendStatusLayout *out)
{
    memset(out, 0, sizeof(*out));
    out->major = row->major;
    out->arch = row->arch;
    out->abi = row->abi;
    out->pointer_width = row->pointer_width;
    out->struct_size = row->struct_size;
    set_field(&out->st_changecount, row->changecount,
              row->changecount_width, true);
    set_field(&out->st_procpid, row->procpid, row->procpid_width, true);
    set_field(&out->st_databaseid, row->databaseid,
              row->databaseid_width, true);
    set_field(&out->st_userid, row->userid, row->userid_width, true);
    set_field(&out->st_state, row->state, row->state_width, true);
    set_field(&out->st_activity_raw, row->activity_raw,
              row->activity_raw_width, true);
    set_field(&out->st_query_id, row->query_id, row->query_id_width,
              row->query_present);
    out->st_query_id_signed = row->query_signed;
    out->state_running = row->state_running;
    out->state_fastpath = row->state_fastpath;
    out->state_max = row->state_max;
    out->source = PGWT_PGBS_SOURCE_HARD_TABLE;
    out->validation = PGWT_PGBS_VALIDATION_NOT_RUN;
    set_detail(out, "header-derived candidate; runtime validation pending");
}

int pgwt_pgbs_hard_table_lookup(int pg_major, enum pgwt_pgbs_arch arch,
                                unsigned pointer_width,
                                enum pgwt_pgbs_abi abi,
                                struct PgBackendStatusLayout *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < sizeof(hard_rows) / sizeof(hard_rows[0]); i++) {
        const struct pgwt_pgbs_hard_row *row = &hard_rows[i];
        if (row->major == pg_major && row->arch == arch &&
            row->pointer_width == pointer_width && row->abi == abi) {
            layout_from_row(row, out);
            return 0;
        }
    }
    return -1;
}

/* ── Strict readelf/DWARF parser ──────────────────────────── */

enum dwarf_tag {
    DT_OTHER = 0,
    DT_STRUCTURE,
    DT_TYPEDEF,
    DT_MEMBER,
    DT_BASE,
    DT_POINTER,
    DT_ENUM,
    DT_ENUMERATOR,
    DT_CONST,
    DT_VOLATILE,
    DT_RESTRICT,
    DT_ATOMIC,
};

enum dwarf_name {
    DN_NONE = 0,
    DN_PGBS,
    DN_CHANGECOUNT,
    DN_PROCPID,
    DN_DATABASEID,
    DN_USERID,
    DN_STATE,
    DN_ACTIVITY_RAW,
    DN_QUERY_ID,
    DN_STATE_RUNNING,
    DN_STATE_FASTPATH,
    DN_STATE_DISABLED,
};

struct dwarf_die {
    uint64_t id;
    uint64_t type_id;
    int parent;
    int depth;
    uint32_t byte_size;
    int64_t const_value;
    int encoding;
    enum dwarf_tag tag;
    enum dwarf_name name;
    bool has_type;
    bool has_byte_size;
    bool has_const_value;
    bool declaration;
    bool location_seen;
    bool location_constant;
    uint64_t location;
};

struct dwarf_vec {
    struct dwarf_die *dies;
    size_t count;
    size_t capacity;
};

static enum dwarf_tag dwarf_tag_code(const char *tag)
{
    if (strcmp(tag, "DW_TAG_structure_type") == 0) return DT_STRUCTURE;
    if (strcmp(tag, "DW_TAG_typedef") == 0) return DT_TYPEDEF;
    if (strcmp(tag, "DW_TAG_member") == 0) return DT_MEMBER;
    if (strcmp(tag, "DW_TAG_base_type") == 0) return DT_BASE;
    if (strcmp(tag, "DW_TAG_pointer_type") == 0) return DT_POINTER;
    if (strcmp(tag, "DW_TAG_enumeration_type") == 0) return DT_ENUM;
    if (strcmp(tag, "DW_TAG_enumerator") == 0) return DT_ENUMERATOR;
    if (strcmp(tag, "DW_TAG_const_type") == 0) return DT_CONST;
    if (strcmp(tag, "DW_TAG_volatile_type") == 0) return DT_VOLATILE;
    if (strcmp(tag, "DW_TAG_restrict_type") == 0) return DT_RESTRICT;
    if (strcmp(tag, "DW_TAG_atomic_type") == 0) return DT_ATOMIC;
    return DT_OTHER;
}

static enum dwarf_name dwarf_name_code(const char *name)
{
    if (strcmp(name, "PgBackendStatus") == 0) return DN_PGBS;
    if (strcmp(name, "st_changecount") == 0) return DN_CHANGECOUNT;
    if (strcmp(name, "st_procpid") == 0) return DN_PROCPID;
    if (strcmp(name, "st_databaseid") == 0) return DN_DATABASEID;
    if (strcmp(name, "st_userid") == 0) return DN_USERID;
    if (strcmp(name, "st_state") == 0) return DN_STATE;
    if (strcmp(name, "st_activity_raw") == 0) return DN_ACTIVITY_RAW;
    if (strcmp(name, "st_query_id") == 0) return DN_QUERY_ID;
    if (strcmp(name, "STATE_RUNNING") == 0) return DN_STATE_RUNNING;
    if (strcmp(name, "STATE_FASTPATH") == 0) return DN_STATE_FASTPATH;
    if (strcmp(name, "STATE_DISABLED") == 0) return DN_STATE_DISABLED;
    return DN_NONE;
}

static int dwarf_push(struct dwarf_vec *vec, const struct dwarf_die *die)
{
    if (vec->count == vec->capacity) {
        size_t cap = vec->capacity ? vec->capacity * 2 : 4096;
        void *p = realloc(vec->dies, cap * sizeof(*vec->dies));
        if (!p)
            return -1;
        vec->dies = p;
        vec->capacity = cap;
    }
    vec->dies[vec->count++] = *die;
    return 0;
}

static const char *attr_value(const char *line, const char *attr)
{
    const char *p = strstr(line, attr);
    if (!p)
        return NULL;
    p = strchr(p + strlen(attr), ':');
    if (!p)
        return NULL;
    p++;
    while (isspace((unsigned char)*p))
        p++;
    return p;
}

static bool parse_uint_constant(const char *value, uint64_t *out)
{
    if (!value || !*value || *value == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(value, &end, 0);
    if (errno || end == value)
        return false;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_int_constant(const char *value, int64_t *out)
{
    if (!value || !*value)
        return false;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(value, &end, 0);
    if (errno || end == value)
        return false;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return false;
    *out = (int64_t)v;
    return true;
}

static void parse_dwarf_name(struct dwarf_die *die, const char *value)
{
    /* readelf may prefix an indirect-string description.  The actual name is
     * after its last colon; target names themselves contain no colon. */
    const char *name = strrchr(value, ':');
    name = name ? name + 1 : value;
    while (isspace((unsigned char)*name))
        name++;
    char buf[96];
    size_t n = strcspn(name, "\r\n");
    while (n > 0 && isspace((unsigned char)name[n - 1]))
        n--;
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, name, n);
    buf[n] = '\0';
    die->name = dwarf_name_code(buf);
}

static int parse_dwarf_stream(FILE *fp, struct dwarf_vec *vec,
                              char *why, size_t why_sz)
{
    int stack[64];
    for (size_t i = 0; i < sizeof(stack) / sizeof(stack[0]); i++)
        stack[i] = -1;
    int current = -1;
    char line[2048];

    while (fgets(line, sizeof(line), fp)) {
        int depth = 0, abbrev = 0;
        unsigned long long id = 0;
        char tag[80];
        if (sscanf(line, " <%d><%llx>: Abbrev Number: %d (%79[^)])",
                   &depth, &id, &abbrev, tag) == 4) {
            if (depth < 0 || depth >= (int)(sizeof(stack) / sizeof(stack[0]))) {
                snprintf(why, why_sz, "DWARF nesting depth out of range");
                return -1;
            }
            struct dwarf_die die;
            memset(&die, 0, sizeof(die));
            die.id = id;
            die.depth = depth;
            die.parent = depth > 0 ? stack[depth - 1] : -1;
            die.tag = dwarf_tag_code(tag);
            if (dwarf_push(vec, &die) != 0) {
                snprintf(why, why_sz, "out of memory parsing DWARF");
                return -1;
            }
            current = (int)vec->count - 1;
            stack[depth] = current;
            for (int i = depth + 1; i < (int)(sizeof(stack) / sizeof(stack[0])); i++)
                stack[i] = -1;
            continue;
        }
        if (current < 0)
            continue;

        struct dwarf_die *die = &vec->dies[current];
        const char *value;
        if ((value = attr_value(line, "DW_AT_name")) != NULL) {
            parse_dwarf_name(die, value);
        } else if ((value = attr_value(line, "DW_AT_type")) != NULL) {
            const char *p = strstr(value, "<0x");
            unsigned long long type_id;
            if (p && sscanf(p, "<0x%llx>", &type_id) == 1) {
                die->type_id = type_id;
                die->has_type = true;
            }
        } else if ((value = attr_value(line, "DW_AT_byte_size")) != NULL) {
            uint64_t v;
            if (parse_uint_constant(value, &v) && v <= UINT32_MAX) {
                die->byte_size = (uint32_t)v;
                die->has_byte_size = true;
            }
        } else if ((value = attr_value(line,
                                        "DW_AT_data_member_location")) != NULL) {
            uint64_t v;
            die->location_seen = true;
            die->location_constant = parse_uint_constant(value, &v);
            if (die->location_constant)
                die->location = v;
        } else if ((value = attr_value(line, "DW_AT_const_value")) != NULL) {
            int64_t v;
            if (parse_int_constant(value, &v)) {
                die->const_value = v;
                die->has_const_value = true;
            }
        } else if ((value = attr_value(line, "DW_AT_encoding")) != NULL) {
            die->encoding = atoi(value);
        } else if ((value = attr_value(line, "DW_AT_declaration")) != NULL) {
            die->declaration = atoi(value) != 0;
        }
    }
    return 0;
}

static int find_die(const struct dwarf_vec *vec, uint64_t id)
{
    /* DIE offsets emitted by readelf are monotonically increasing. */
    size_t lo = 0, hi = vec->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (vec->dies[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < vec->count && vec->dies[lo].id == id)
        return (int)lo;
    return -1;
}

static int resolve_type(const struct dwarf_vec *vec, int index)
{
    for (int depth = 0; index >= 0 && depth < 32; depth++) {
        const struct dwarf_die *die = &vec->dies[index];
        if (die->tag != DT_TYPEDEF && die->tag != DT_CONST &&
            die->tag != DT_VOLATILE && die->tag != DT_RESTRICT &&
            die->tag != DT_ATOMIC)
            return index;
        if (!die->has_type)
            return -1;
        index = find_die(vec, die->type_id);
    }
    return -1;
}

static int member_type(const struct dwarf_vec *vec,
                       const struct dwarf_die *member)
{
    if (!member->has_type)
        return -1;
    int index = find_die(vec, member->type_id);
    return resolve_type(vec, index);
}

static bool fields_overlap(const struct pgwt_pgbs_field *a,
                           const struct pgwt_pgbs_field *b)
{
    if (!a->present || !b->present)
        return false;
    return a->offset < b->offset + b->width &&
           b->offset < a->offset + a->width;
}

static int structurally_sane(const struct PgBackendStatusLayout *layout,
                             char *why, size_t why_sz)
{
    const struct pgwt_pgbs_field *fields[] = {
        &layout->st_changecount, &layout->st_procpid,
        &layout->st_databaseid, &layout->st_userid, &layout->st_state,
        &layout->st_activity_raw, &layout->st_query_id,
    };
    if (layout->struct_size < 32 || layout->struct_size > 65536) {
        snprintf(why, why_sz, "implausible PgBackendStatus byte size %u",
                 layout->struct_size);
        return -1;
    }
    if (!layout->st_changecount.present || layout->st_changecount.width != 4 ||
        !layout->st_procpid.present || layout->st_procpid.width != 4 ||
        !layout->st_databaseid.present || layout->st_databaseid.width != 4 ||
        !layout->st_userid.present || layout->st_userid.width != 4 ||
        !layout->st_state.present || layout->st_state.width != 4 ||
        !layout->st_activity_raw.present ||
        layout->st_activity_raw.width != layout->pointer_width) {
        snprintf(why, why_sz, "missing mandatory member or implausible width");
        return -1;
    }
    if (layout->major >= 14 &&
        (!layout->st_query_id.present || layout->st_query_id.width != 8)) {
        snprintf(why, why_sz, "PG%d requires an 8-byte st_query_id",
                 layout->major);
        return -1;
    }
    if (layout->major == 13 && layout->st_query_id.present) {
        snprintf(why, why_sz, "PG13 unexpectedly declares st_query_id");
        return -1;
    }
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fields[i]->present &&
            (uint64_t)fields[i]->offset + fields[i]->width > layout->struct_size) {
            snprintf(why, why_sz, "member extends beyond struct byte size");
            return -1;
        }
        for (size_t j = i + 1; j < sizeof(fields) / sizeof(fields[0]); j++) {
            if (fields_overlap(fields[i], fields[j])) {
                snprintf(why, why_sz, "selected members overlap");
                return -1;
            }
        }
    }
    if (layout->state_running < 0 || layout->state_fastpath < 0 ||
        layout->state_max < layout->state_running ||
        layout->state_max < layout->state_fastpath ||
        layout->state_running == layout->state_fastpath ||
        layout->state_max > 64) {
        snprintf(why, why_sz, "implausible BackendState enumerators");
        return -1;
    }
    return 0;
}

static struct pgwt_pgbs_field *layout_field(struct PgBackendStatusLayout *layout,
                                             enum dwarf_name name)
{
    switch (name) {
    case DN_CHANGECOUNT: return &layout->st_changecount;
    case DN_PROCPID:     return &layout->st_procpid;
    case DN_DATABASEID:  return &layout->st_databaseid;
    case DN_USERID:      return &layout->st_userid;
    case DN_STATE:       return &layout->st_state;
    case DN_ACTIVITY_RAW:return &layout->st_activity_raw;
    case DN_QUERY_ID:    return &layout->st_query_id;
    default:             return NULL;
    }
}

static int layout_from_struct(const struct dwarf_vec *vec, int struct_index,
                              int pg_major, unsigned pointer_width,
                              struct PgBackendStatusLayout *layout,
                              char *why, size_t why_sz)
{
    const struct dwarf_die *structure = &vec->dies[struct_index];
    memset(layout, 0, sizeof(*layout));
    layout->major = pg_major;
    layout->pointer_width = pointer_width;
    layout->source = PGWT_PGBS_SOURCE_DWARF;
    if (!structure->has_byte_size) {
        snprintf(why, why_sz, "PgBackendStatus has no byte size");
        return -1;
    }
    layout->struct_size = structure->byte_size;
    set_field(&layout->st_query_id, 0, 0, false);

    int state_type = -1;
    unsigned found_mask = 0;
    for (size_t i = 0; i < vec->count; i++) {
        const struct dwarf_die *member = &vec->dies[i];
        if (member->parent != struct_index || member->tag != DT_MEMBER)
            continue;
        struct pgwt_pgbs_field *field = layout_field(layout, member->name);
        if (!field)
            continue;
        unsigned bit = 1u << (unsigned)(member->name - DN_CHANGECOUNT);
        if (found_mask & bit) {
            snprintf(why, why_sz, "duplicate member in PgBackendStatus");
            return -1;
        }
        found_mask |= bit;
        if (!member->location_seen || !member->location_constant ||
            member->location > UINT32_MAX) {
            snprintf(why, why_sz,
                     "member location is missing or is a DWARF expression");
            return -1;
        }
        int type_index = member_type(vec, member);
        if (type_index < 0 || !vec->dies[type_index].has_byte_size ||
            vec->dies[type_index].byte_size == 0 ||
            vec->dies[type_index].byte_size > UINT8_MAX) {
            snprintf(why, why_sz, "member type has no plausible byte size");
            return -1;
        }
        set_field(field, (uint32_t)member->location,
                  (uint8_t)vec->dies[type_index].byte_size, true);
        if (member->name == DN_STATE)
            state_type = type_index;
        if (member->name == DN_ACTIVITY_RAW &&
            vec->dies[type_index].tag != DT_POINTER) {
            snprintf(why, why_sz, "st_activity_raw is not a pointer");
            return -1;
        }
        if (member->name == DN_QUERY_ID) {
            if (vec->dies[type_index].tag != DT_BASE) {
                snprintf(why, why_sz, "st_query_id is not an integer base type");
                return -1;
            }
            if (vec->dies[type_index].encoding == 5 ||
                vec->dies[type_index].encoding == 6)
                layout->st_query_id_signed = true;
            else if (vec->dies[type_index].encoding == 7 ||
                     vec->dies[type_index].encoding == 8)
                layout->st_query_id_signed = false;
            else {
                snprintf(why, why_sz, "st_query_id has unsupported signedness");
                return -1;
            }
        }
    }

    if (state_type < 0 || vec->dies[state_type].tag != DT_ENUM) {
        snprintf(why, why_sz, "st_state does not resolve to an enum");
        return -1;
    }
    bool have_running = false, have_fastpath = false, have_disabled = false;
    for (size_t i = 0; i < vec->count; i++) {
        const struct dwarf_die *e = &vec->dies[i];
        if (e->parent != state_type || e->tag != DT_ENUMERATOR)
            continue;
        if (e->name == DN_STATE_RUNNING || e->name == DN_STATE_FASTPATH ||
            e->name == DN_STATE_DISABLED) {
            if (!e->has_const_value || e->const_value < 0 ||
                e->const_value > INT32_MAX) {
                snprintf(why, why_sz, "BackendState enumerator has no constant value");
                return -1;
            }
            if (e->name == DN_STATE_RUNNING) {
                if (have_running) goto duplicate_enum;
                have_running = true;
                layout->state_running = (int)e->const_value;
            } else if (e->name == DN_STATE_FASTPATH) {
                if (have_fastpath) goto duplicate_enum;
                have_fastpath = true;
                layout->state_fastpath = (int)e->const_value;
            } else {
                if (have_disabled) goto duplicate_enum;
                have_disabled = true;
                layout->state_max = (int)e->const_value;
            }
        }
    }
    if (!have_running || !have_fastpath || !have_disabled) {
        snprintf(why, why_sz, "mandatory BackendState enumerator missing");
        return -1;
    }
    if (structurally_sane(layout, why, why_sz) != 0)
        return -1;
    set_detail(layout, "complete constant-offset DWARF candidate; runtime validation pending");
    return 0;

duplicate_enum:
    snprintf(why, why_sz, "duplicate BackendState enumerator");
    return -1;
}

static bool layouts_equal(const struct PgBackendStatusLayout *a,
                          const struct PgBackendStatusLayout *b)
{
#define SAME_FIELD(f) \
    (a->f.present == b->f.present && a->f.offset == b->f.offset && \
     a->f.width == b->f.width)
    return a->struct_size == b->struct_size &&
           SAME_FIELD(st_changecount) && SAME_FIELD(st_procpid) &&
           SAME_FIELD(st_databaseid) && SAME_FIELD(st_userid) &&
           SAME_FIELD(st_state) && SAME_FIELD(st_activity_raw) &&
           SAME_FIELD(st_query_id) &&
           a->st_query_id_signed == b->st_query_id_signed &&
           a->state_running == b->state_running &&
           a->state_fastpath == b->state_fastpath &&
           a->state_max == b->state_max;
#undef SAME_FIELD
}

int pgwt_pgbs_parse_dwarf(FILE *fp, int pg_major, unsigned pointer_width,
                          struct PgBackendStatusLayout *out,
                          char *why, size_t why_sz)
{
    if (!fp || !out || !why || why_sz == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    why[0] = '\0';
    struct dwarf_vec vec = {0};
    if (parse_dwarf_stream(fp, &vec, why, why_sz) != 0) {
        free(vec.dies);
        return -1;
    }

    struct PgBackendStatusLayout accepted;
    bool have_accepted = false;
    int seen_structs[256];
    size_t nseen = 0;
    for (size_t i = 0; i < vec.count; i++) {
        const struct dwarf_die *root = &vec.dies[i];
        if (root->name != DN_PGBS ||
            (root->tag != DT_STRUCTURE && root->tag != DT_TYPEDEF))
            continue;
        int target = (int)i;
        if (root->tag == DT_TYPEDEF) {
            if (!root->has_type)
                continue;
            target = resolve_type(&vec, find_die(&vec, root->type_id));
        }
        if (target < 0 || vec.dies[target].tag != DT_STRUCTURE ||
            vec.dies[target].declaration)
            continue;
        bool duplicate = false;
        for (size_t j = 0; j < nseen; j++)
            if (seen_structs[j] == target)
                duplicate = true;
        if (duplicate)
            continue;
        if (nseen < sizeof(seen_structs) / sizeof(seen_structs[0]))
            seen_structs[nseen++] = target;

        struct PgBackendStatusLayout candidate;
        char candidate_why[160];
        if (layout_from_struct(&vec, target, pg_major, pointer_width,
                               &candidate, candidate_why,
                               sizeof(candidate_why)) != 0)
            continue;
        if (!have_accepted) {
            accepted = candidate;
            have_accepted = true;
        } else if (!layouts_equal(&accepted, &candidate)) {
            snprintf(why, why_sz,
                     "ambiguous PgBackendStatus definitions disagree");
            free(vec.dies);
            return -1;
        }
    }
    free(vec.dies);

    if (!have_accepted) {
        snprintf(why, why_sz,
                 "no complete unambiguous PgBackendStatus definition");
        return -1;
    }
    *out = accepted;
    return 0;
}

/* ── Discovery order ─────────────────────────────────────── */

static void native_key(enum pgwt_pgbs_arch *arch, enum pgwt_pgbs_abi *abi)
{
    struct utsname un;
    *arch = PGWT_PGBS_ARCH_UNKNOWN;
    *abi = PGWT_PGBS_ABI_UNKNOWN;
    if (uname(&un) == 0)
        *arch = pgwt_pgbs_arch_from_name(un.machine);
    *abi = pgwt_pgbs_abi_for_arch(*arch, sizeof(void *));
}

static uint32_t required_fallback_mask(int pg_major)
{
    uint32_t mask = PGWT_PGBS_FALLBACK_STATE |
                    PGWT_PGBS_FALLBACK_ACTIVITY_RAW;
    if (pg_major >= 14)
        mask |= PGWT_PGBS_FALLBACK_QUERY_ID;
    return mask;
}

int pgwt_pgbs_discover(const char *pg_binary, int pg_major,
                       struct PgBackendStatusLayout *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    enum pgwt_pgbs_arch arch;
    enum pgwt_pgbs_abi abi;
    native_key(&arch, &abi);

    char dwarf_why[160] = "readelf unavailable";
    if (pg_binary && *pg_binary) {
        char *argv[] = { "readelf", "--debug-dump=info", "--wide",
                         (char *)pg_binary, NULL };
        struct pgwt_proc proc;
        if (pgwt_proc_open(&proc, argv) == 0) {
            struct PgBackendStatusLayout dwarf;
            int rc = pgwt_pgbs_parse_dwarf(proc.out, pg_major, sizeof(void *),
                                           &dwarf, dwarf_why,
                                           sizeof(dwarf_why));
            int status = pgwt_proc_close(&proc);
            if (rc == 0 && status == 0) {
                dwarf.arch = arch;
                dwarf.abi = abi;
                *out = dwarf;
                return 0;
            }
        }
    }

    if (pgwt_pgbs_hard_table_lookup(pg_major, arch, sizeof(void *), abi,
                                    out) == 0) {
        set_detail(out, "DWARF rejected (%s); header-derived candidate; "
                   "runtime validation pending", dwarf_why);
        return 0;
    }

    out->major = pg_major;
    out->arch = arch;
    out->abi = abi;
    out->pointer_width = sizeof(void *);
    out->source = PGWT_PGBS_SOURCE_NONE;
    out->validation = PGWT_PGBS_VALIDATION_REJECTED;
    out->fallback_mask = required_fallback_mask(pg_major);
    set_detail(out, "DWARF rejected (%s); no exact hard-table key", dwarf_why);
    return -1;
}

/* ── Pure runtime validation ──────────────────────────────── */

static bool has_read(const struct pgwt_pgbs_snapshot *snapshot, uint32_t bit)
{
    return (snapshot->read_mask & bit) != 0;
}

int pgwt_pgbs_validate_snapshot(struct PgBackendStatusLayout *layout,
                                const struct pgwt_pgbs_snapshot *snapshot,
                                const struct pgwt_pgbs_expected *expected)
{
    if (!layout || !snapshot || !expected)
        return -1;

    bool coherent = has_read(snapshot, PGWT_PGBS_READ_CHANGECOUNT) &&
                    snapshot->changecount_before ==
                        snapshot->changecount_after &&
                    (snapshot->changecount_before & 1u) == 0;
    bool pid_ok = has_read(snapshot, PGWT_PGBS_READ_PROCPID) &&
                  snapshot->procpid == (uint32_t)expected->pid &&
                  snapshot->procpid > 0;
    bool db_ok = has_read(snapshot, PGWT_PGBS_READ_DATABASEID) &&
                 snapshot->databaseid > 0 &&
                 (!expected->databaseid ||
                  snapshot->databaseid == expected->databaseid);
    bool user_ok = has_read(snapshot, PGWT_PGBS_READ_USERID) &&
                   snapshot->userid > 0 &&
                   (!expected->userid || snapshot->userid == expected->userid);
    bool identity_ok = coherent && pid_ok && db_ok && user_ok;

    layout->st_changecount.validation = coherent
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;
    layout->st_procpid.validation = pid_ok
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;
    layout->st_databaseid.validation = db_ok
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;
    layout->st_userid.validation = user_ok
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;

    bool state_active = snapshot->state == (uint32_t)layout->state_running ||
                        snapshot->state == (uint32_t)layout->state_fastpath;
    /* Idle agreement is not evidence for a field offset: most nearby padding
     * and unrelated enum fields also look like a valid non-running state.
     * Only an independently observed active backend can validate st_state. */
    bool state_ok = identity_ok && has_read(snapshot, PGWT_PGBS_READ_STATE) &&
                    snapshot->state <= (uint32_t)layout->state_max &&
                    expected->state_shadow_available &&
                    expected->state_active && state_active;
    layout->st_state.validation = state_ok
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;

    bool activity_ok = identity_ok &&
                       has_read(snapshot, PGWT_PGBS_READ_ACTIVITY) &&
                       snapshot->activity_raw != 0 &&
                       snapshot->activity_readable &&
                       (!expected->activity_marker_required ||
                        snapshot->activity_marker_matched);
    layout->st_activity_raw.validation = activity_ok
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;

    bool query_ok = true;
    if (layout->major >= 14) {
        query_ok = identity_ok && layout->st_query_id.present &&
                   has_read(snapshot, PGWT_PGBS_READ_QUERY_ID) &&
                   expected->query_id_available &&
                   expected->query_id != 0 && snapshot->query_id != 0 &&
                   snapshot->query_id == expected->query_id;
        layout->st_query_id.validation = query_ok
            ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;
    } else {
        layout->st_query_id.validation = PGWT_PGBS_FIELD_ABSENT;
    }

    layout->fallback_mask = 0;
    if (!state_ok)
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_STATE;
    if (!activity_ok)
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_ACTIVITY_RAW;
    if (layout->major >= 14 && !query_ok)
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_QUERY_ID;
    layout->validation = layout->fallback_mask
        ? PGWT_PGBS_VALIDATION_DEGRADED
        : PGWT_PGBS_VALIDATION_VALIDATED;
    layout->validated_pid = identity_ok ? expected->pid : 0;

    if (layout->validation == PGWT_PGBS_VALIDATION_VALIDATED)
        set_detail(layout, "coherent controlled-backend read and shadow comparison passed");
    else
        set_detail(layout,
                   "runtime validation failed: coherent=%s pid=%s db=%s user=%s "
                   "state=%s activity=%s query=%s",
                   coherent ? "yes" : "no", pid_ok ? "yes" : "no",
                   db_ok ? "yes" : "no", user_ok ? "yes" : "no",
                   state_ok ? "yes" : "no", activity_ok ? "yes" : "no",
                   layout->major < 14 ? "n/a" : (query_ok ? "yes" : "no"));
    return layout->validation == PGWT_PGBS_VALIDATION_VALIDATED ? 0 : -1;
}

/* ── Live controlled-backend validator ───────────────────── */

static ssize_t pread_exact(int fd, void *buf, size_t size, uint64_t addr)
{
    ssize_t n;
    do {
        n = pread(fd, buf, size, (off_t)addr);
    } while (n < 0 && errno == EINTR);
    return n == (ssize_t)size ? n : -1;
}

static int read_field_value(int fd, uint64_t base,
                            const struct pgwt_pgbs_field *field,
                            uint64_t *value)
{
    uint8_t buf[8] = {0};
    if (!field->present || field->width == 0 || field->width > sizeof(buf) ||
        pread_exact(fd, buf, field->width, base + field->offset) < 0)
        return -1;
    memcpy(value, buf, field->width);
    return 0;
}

static int read_snapshot_fd(int fd, uint64_t base,
                            const struct PgBackendStatusLayout *layout,
                            struct pgwt_pgbs_snapshot *snapshot,
                            char *activity, size_t activity_sz)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (activity && activity_sz)
        activity[0] = '\0';
    for (int attempt = 0; attempt < 32; attempt++) {
        uint64_t value;
        snapshot->read_mask = 0;
        if (read_field_value(fd, base, &layout->st_changecount, &value) != 0)
            return -1;
        snapshot->changecount_before = (uint32_t)value;
        snapshot->read_mask |= PGWT_PGBS_READ_CHANGECOUNT;
        if (snapshot->changecount_before & 1u)
            continue;

#define READ_VALUE(field_, member_, bit_) do { \
            if (read_field_value(fd, base, &layout->field_, &value) == 0) { \
                snapshot->member_ = (__typeof__(snapshot->member_))value; \
                snapshot->read_mask |= (bit_); \
            } \
        } while (0)
        READ_VALUE(st_procpid, procpid, PGWT_PGBS_READ_PROCPID);
        READ_VALUE(st_databaseid, databaseid, PGWT_PGBS_READ_DATABASEID);
        READ_VALUE(st_userid, userid, PGWT_PGBS_READ_USERID);
        READ_VALUE(st_state, state, PGWT_PGBS_READ_STATE);
        READ_VALUE(st_activity_raw, activity_raw, PGWT_PGBS_READ_ACTIVITY);
        if (layout->st_query_id.present)
            READ_VALUE(st_query_id, query_id, PGWT_PGBS_READ_QUERY_ID);
#undef READ_VALUE

        if (read_field_value(fd, base, &layout->st_changecount, &value) != 0)
            return -1;
        snapshot->changecount_after = (uint32_t)value;
        if (snapshot->changecount_before != snapshot->changecount_after ||
            (snapshot->changecount_after & 1u))
            continue;

        if (snapshot->activity_raw && activity && activity_sz) {
            ssize_t n = pread(fd, activity, activity_sz - 1,
                              (off_t)snapshot->activity_raw);
            if (n > 0) {
                activity[n] = '\0';
                snapshot->activity_readable = true;
            }
        }
        return 0;
    }
    return -1;
}

static int open_mem(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    return open(path, O_RDONLY | O_CLOEXEC);
}

static int candidate_ok(int fd, uint64_t base,
                        const struct PgBackendStatusLayout *layout,
                        const struct pgwt_pgbs_expected *expected,
                        const char *activity_marker,
                        struct pgwt_pgbs_snapshot *snapshot)
{
    char activity[512];
    if (read_snapshot_fd(fd, base, layout, snapshot,
                         activity, sizeof(activity)) != 0)
        return 0;
    snapshot->activity_marker_matched =
        activity_marker && strstr(activity, activity_marker) != NULL;
    if (snapshot->procpid != (uint32_t)expected->pid ||
        snapshot->databaseid == 0 || snapshot->userid == 0 ||
        (expected->databaseid &&
         snapshot->databaseid != expected->databaseid) ||
        (expected->userid && snapshot->userid != expected->userid) ||
        snapshot->state > (uint32_t)layout->state_max ||
        !snapshot->activity_readable ||
        (activity_marker && !snapshot->activity_marker_matched))
        return 0;
    if (expected->state_shadow_available) {
        bool active = snapshot->state == (uint32_t)layout->state_running ||
                      snapshot->state == (uint32_t)layout->state_fastpath;
        if (active != expected->state_active)
            return 0;
    }
    return 1;
}

static uint64_t scan_shared_for_entry(pid_t pid,
                                      const struct PgBackendStatusLayout *layout,
                                      const struct pgwt_pgbs_expected *expected,
                                      const char *activity_marker,
                                      struct pgwt_pgbs_snapshot *snapshot)
{
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *maps = fopen(maps_path, "r");
    if (!maps)
        return 0;
    int mem_fd = open_mem(pid);
    if (mem_fd < 0) {
        fclose(maps);
        return 0;
    }

    const size_t chunk_size = 1024 * 1024;
    const uint64_t scan_cap = 512ULL * 1024 * 1024;
    uint8_t *buf = malloc(chunk_size);
    uint64_t scanned = 0, found = 0;
    unsigned matches = 0;
    char line[1024];
    while (buf && scanned < scan_cap && fgets(line, sizeof(line), maps)) {
        unsigned long long start, end;
        char perms[5] = "";
        if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3 ||
            perms[0] != 'r' || perms[1] != 'w' || perms[3] != 's' ||
            end <= start)
            continue;
        for (uint64_t pos = start; pos < end && scanned < scan_cap;) {
            size_t want = (size_t)((end - pos) > chunk_size
                                   ? chunk_size : (end - pos));
            if (scanned + want > scan_cap)
                want = (size_t)(scan_cap - scanned);
            ssize_t n = pread(mem_fd, buf, want, (off_t)pos);
            scanned += want;
            if (n > 0) {
                for (size_t off = 0; off + sizeof(uint32_t) <= (size_t)n;
                     off += sizeof(uint32_t)) {
                    uint32_t value;
                    memcpy(&value, buf + off, sizeof(value));
                    if (value != (uint32_t)expected->pid ||
                        pos + off < layout->st_procpid.offset)
                        continue;
                    uint64_t base = pos + off - layout->st_procpid.offset;
                    if (base % layout->pointer_width != 0 ||
                        base < start || base + layout->struct_size > end)
                        continue;
                    struct pgwt_pgbs_snapshot candidate;
                    if (candidate_ok(mem_fd, base, layout, expected,
                                     activity_marker, &candidate)) {
                        found = base;
                        *snapshot = candidate;
                        matches++;
                    }
                }
            }
            pos += want;
        }
    }
    free(buf);
    close(mem_fd);
    fclose(maps);
    return matches == 1 ? found : 0;
}

static int postmaster_connection(pid_t postmaster_pid, int *port,
                                 char *socket_dir, size_t socket_sz,
                                 char *user, size_t user_sz)
{
    char cwd_link[64], pgdata[512];
    snprintf(cwd_link, sizeof(cwd_link), "/proc/%d/cwd", postmaster_pid);
    ssize_t n = readlink(cwd_link, pgdata, sizeof(pgdata) - 1);
    if (n < 0)
        return -1;
    pgdata[n] = '\0';
    char pidfile[640];
    snprintf(pidfile, sizeof(pidfile), "%s/postmaster.pid", pgdata);
    FILE *fp = fopen(pidfile, "r");
    if (!fp)
        return -1;
    *port = 5432;
    socket_dir[0] = '\0';
    char line[512];
    for (int lineno = 1; fgets(line, sizeof(line), fp); lineno++) {
        line[strcspn(line, "\r\n")] = '\0';
        if (lineno == 4)
            *port = atoi(line);
        else if (lineno == 5)
            snprintf(socket_dir, socket_sz, "%s", line);
    }
    fclose(fp);
    char *comma = strchr(socket_dir, ',');
    if (comma)
        *comma = '\0';
    while (socket_dir[0] && isspace((unsigned char)socket_dir[0]))
        memmove(socket_dir, socket_dir + 1, strlen(socket_dir));

    char status_path[64];
    snprintf(status_path, sizeof(status_path), "/proc/%d/status", postmaster_pid);
    fp = fopen(status_path, "r");
    if (!fp)
        return -1;
    uid_t uid = (uid_t)-1;
    while (fgets(line, sizeof(line), fp)) {
        unsigned value;
        if (sscanf(line, "Uid:\t%u", &value) == 1) {
            uid = (uid_t)value;
            break;
        }
    }
    fclose(fp);
    struct passwd *pw = uid == (uid_t)-1 ? NULL : getpwuid(uid);
    if (!pw)
        return -1;
    snprintf(user, user_sz, "%s", pw->pw_name);
    return *port > 0 ? 0 : -1;
}

static int spawn_psql(const char *psql, const char *user,
                      const char *socket_dir, int port, const char *sql,
                      struct pgwt_proc *proc)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    char *argv_with_socket[] = {
        "runuser", "-u", (char *)user, "--", (char *)psql,
        "-X", "-Atq", "-v", "ON_ERROR_STOP=1",
        "-h", (char *)socket_dir, "-p", port_str,
        "-d", "postgres", "-c", (char *)sql, NULL,
    };
    char *argv_default_socket[] = {
        "runuser", "-u", (char *)user, "--", (char *)psql,
        "-X", "-Atq", "-v", "ON_ERROR_STOP=1",
        "-p", port_str, "-d", "postgres", "-c", (char *)sql, NULL,
    };
    return pgwt_proc_open(proc, socket_dir[0] ? argv_with_socket
                                              : argv_default_socket);
}

static int spawn_psql_session(const char *psql, const char *user,
                              const char *socket_dir, int port,
                              const char *appname, struct pgwt_proc *proc)
{
    char port_str[16], dbspec[192];
    snprintf(port_str, sizeof(port_str), "%d", port);
    snprintf(dbspec, sizeof(dbspec),
             "dbname=postgres application_name=%s", appname);
    char *argv_with_socket[] = {
        "runuser", "-u", (char *)user, "--", (char *)psql,
        "-X", "-Atq", "-v", "ON_ERROR_STOP=1",
        "-h", (char *)socket_dir, "-p", port_str,
        "-d", dbspec, NULL,
    };
    char *argv_default_socket[] = {
        "runuser", "-u", (char *)user, "--", (char *)psql,
        "-X", "-Atq", "-v", "ON_ERROR_STOP=1",
        "-p", port_str, "-d", dbspec, NULL,
    };
    return pgwt_proc_open_rw(proc, socket_dir[0] ? argv_with_socket
                                                 : argv_default_socket);
}

static int send_psql(struct pgwt_proc *proc, const char *sql)
{
    if (!proc || !proc->in || fputs(sql, proc->in) == EOF ||
        fputc('\n', proc->in) == EOF || fflush(proc->in) != 0)
        return -1;
    return 0;
}

static int wait_for_pid_exit(pid_t pid)
{
    if (pid <= 0)
        return -1;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (kill(pid, 0) != 0 && errno == ESRCH)
            return 0;
        usleep(50000);
    }
    return -1;
}

static int observe_controlled_backend(const char *psql, const char *user,
                                      const char *socket_dir, int port,
                                      const char *appname, int pg_major,
                                      bool want_active,
                                      struct pgwt_pgbs_expected *expected)
{
    char sql[1024];
    if (pg_major >= 14)
        snprintf(sql, sizeof(sql),
                 "WITH target AS (SELECT pid,datid,usesysid,query_id,state "
                 "FROM pg_stat_activity WHERE application_name='%s' "
                 "AND state='%s' ORDER BY backend_start DESC LIMIT 1) "
                 "SELECT 'target',pid::text,datid::text,usesysid::text,"
                 "COALESCE(query_id,0)::text,"
                 "CASE WHEN state='active' THEN '1' ELSE '0' END FROM target "
                 "UNION ALL SELECT 'observer',pg_backend_pid()::text,"
                 "'0','0','0','0'",
                 appname, want_active ? "active" : "idle");
    else
        snprintf(sql, sizeof(sql),
                 "WITH target AS (SELECT pid,datid,usesysid,state "
                 "FROM pg_stat_activity WHERE application_name='%s' "
                 "AND state='%s' ORDER BY backend_start DESC LIMIT 1) "
                 "SELECT 'target',pid::text,datid::text,usesysid::text,'0',"
                 "CASE WHEN state='active' THEN '1' ELSE '0' END FROM target "
                 "UNION ALL SELECT 'observer',pg_backend_pid()::text,"
                 "'0','0','0','0'",
                 appname, want_active ? "active" : "idle");

    for (int attempt = 0; attempt < 20; attempt++) {
        struct pgwt_proc observer;
        if (spawn_psql(psql, user, socket_dir, port, sql, &observer) != 0)
            return -1;
        pid_t target_pid = 0, observer_pid = 0;
        uint32_t databaseid = 0, userid = 0;
        long long query_id = 0;
        int active = -1;
        char line[256];
        while (fgets(line, sizeof(line), observer.out)) {
            if (sscanf(line, "target|%d|%u|%u|%lld|%d",
                       &target_pid, &databaseid, &userid,
                       &query_id, &active) == 5)
                continue;
            (void)sscanf(line, "observer|%d", &observer_pid);
        }
        int status = pgwt_proc_close(&observer);
        if (observer_pid > 0 && wait_for_pid_exit(observer_pid) != 0)
            return -2;
        bool qid_ok = pg_major < 14 || !want_active || query_id != 0;
        if (status == 0 && target_pid > 0 && databaseid > 0 && userid > 0 &&
            active == (want_active ? 1 : 0) && qid_ok) {
            memset(expected, 0, sizeof(*expected));
            expected->pid = target_pid;
            expected->databaseid = databaseid;
            expected->userid = userid;
            expected->require_running = want_active;
            expected->state_shadow_available = true;
            expected->state_active = want_active;
            expected->query_id = (uint64_t)query_id;
            expected->query_id_available = pg_major >= 14 && query_id != 0;
            return 0;
        }
        usleep(50000);
    }
    return -1;
}

static uint64_t resolve_controlled_snapshot(
    pid_t pid, uint64_t preferred_base, uint64_t my_be_entry_addr,
    const struct PgBackendStatusLayout *layout,
    const struct pgwt_pgbs_expected *expected, const char *activity_marker,
    struct pgwt_pgbs_snapshot *snapshot)
{
    uint64_t base = preferred_base;
    int mem_fd = open_mem(pid);
    if (mem_fd >= 0) {
        if (!base && my_be_entry_addr &&
            pread_exact(mem_fd, &base, sizeof(base), my_be_entry_addr) < 0)
            base = 0;
        /* A readable nonzero MyBEEntry pointer is still only a candidate.
         * Zero it on any incoherent/content mismatch so the unique shared
         * mapping scan remains reachable. */
        if (base && !candidate_ok(mem_fd, base, layout, expected,
                                  activity_marker, snapshot))
            base = 0;
        close(mem_fd);
    }
    if (!base)
        base = scan_shared_for_entry(pid, layout, expected, activity_marker,
                                     snapshot);
    return base;
}

static void degrade_unvalidated(struct PgBackendStatusLayout *layout,
                                const char *reason)
{
    struct pgwt_pgbs_field *fields[] = {
        &layout->st_changecount, &layout->st_procpid,
        &layout->st_databaseid, &layout->st_userid, &layout->st_state,
        &layout->st_activity_raw, &layout->st_query_id,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        fields[i]->validation = fields[i]->present
            ? PGWT_PGBS_FIELD_INVALID : PGWT_PGBS_FIELD_ABSENT;
    layout->validation = layout->source == PGWT_PGBS_SOURCE_NONE
        ? PGWT_PGBS_VALIDATION_REJECTED : PGWT_PGBS_VALIDATION_DEGRADED;
    layout->fallback_mask = required_fallback_mask(layout->major);
    layout->validated_pid = 0;
    set_detail(layout, "%s", reason);
}

int pgwt_pgbs_validate_runtime(struct PgBackendStatusLayout *layout,
                               pid_t postmaster_pid,
                               uint64_t my_be_entry_addr,
                               const char *pg_binary)
{
    if (!layout || layout->source == PGWT_PGBS_SOURCE_NONE) {
        if (layout)
            degrade_unvalidated(layout, "no discovered layout to validate");
        return -1;
    }
    int port;
    char socket_dir[512], user[128];
    if (!pg_binary ||
        postmaster_connection(postmaster_pid, &port, socket_dir,
                              sizeof(socket_dir), user, sizeof(user)) != 0) {
        degrade_unvalidated(layout,
                            "controlled-backend connection metadata unavailable");
        return -1;
    }
    char psql[512];
    snprintf(psql, sizeof(psql), "%s", pg_binary);
    char *slash = strrchr(psql, '/');
    if (!slash) {
        degrade_unvalidated(layout, "postgres binary has no bindir");
        return -1;
    }
    snprintf(slash + 1, (size_t)(psql + sizeof(psql) - slash - 1), "psql");

    char appname[96], idle_marker[96], first_marker[96], second_marker[96];
    snprintf(appname, sizeof(appname), "pgwt-layout-validation-%d-%d",
             postmaster_pid, getpid());
    snprintf(idle_marker, sizeof(idle_marker), "pgwt-layout-idle-%d",
             getpid());
    snprintf(first_marker, sizeof(first_marker), "pgwt-layout-active-a-%d",
             getpid());
    snprintf(second_marker, sizeof(second_marker), "pgwt-layout-active-b-%d",
             getpid());
    struct pgwt_proc controlled;
    if (spawn_psql_session(psql, user, socket_dir, port, appname,
                           &controlled) != 0) {
        degrade_unvalidated(layout, "could not start controlled backend");
        return -1;
    }

    char command[256];
    int observed = 0;
    if (layout->major >= 14)
        observed = send_psql(&controlled, "SET compute_query_id=on;");
    snprintf(command, sizeof(command), "SELECT 1 /* %s */;", idle_marker);
    if (observed == 0)
        observed = send_psql(&controlled, command);

    struct pgwt_pgbs_expected idle_expected, first_expected, second_expected;
    memset(&idle_expected, 0, sizeof(idle_expected));
    memset(&first_expected, 0, sizeof(first_expected));
    memset(&second_expected, 0, sizeof(second_expected));
    struct pgwt_pgbs_snapshot idle_snapshot, first_snapshot, second_snapshot;
    memset(&idle_snapshot, 0, sizeof(idle_snapshot));
    memset(&first_snapshot, 0, sizeof(first_snapshot));
    memset(&second_snapshot, 0, sizeof(second_snapshot));
    uint64_t idle_base = 0, first_base = 0, second_base = 0;

    if (observed == 0)
        observed = observe_controlled_backend(
            psql, user, socket_dir, port, appname, layout->major, false,
            &idle_expected);
    if (observed == 0) {
        idle_expected.activity_marker_required = true;
        idle_base = resolve_controlled_snapshot(
            idle_expected.pid, 0, my_be_entry_addr, layout, &idle_expected,
            idle_marker, &idle_snapshot);
        if (!idle_base)
            observed = -1;
    }

    snprintf(command, sizeof(command),
             "SELECT pg_sleep(0.25) /* %s */;", first_marker);
    if (observed == 0)
        observed = send_psql(&controlled, command);
    if (observed == 0)
        observed = observe_controlled_backend(
            psql, user, socket_dir, port, appname, layout->major, true,
            &first_expected);
    if (observed == 0) {
        first_expected.activity_marker_required = true;
        first_base = resolve_controlled_snapshot(
            first_expected.pid, idle_base, my_be_entry_addr, layout,
            &first_expected, first_marker, &first_snapshot);
        if (!first_base)
            observed = -1;
    }

    /* Wait for the same backend to become idle before issuing the second,
     * structurally distinct statement.  This prevents psql from queueing both
     * commands and gives st_state an observed active->idle boundary. */
    struct pgwt_pgbs_expected between_expected;
    memset(&between_expected, 0, sizeof(between_expected));
    if (observed == 0)
        observed = observe_controlled_backend(
            psql, user, socket_dir, port, appname, layout->major, false,
            &between_expected);

    snprintf(command, sizeof(command),
             "SELECT 1 FROM pg_sleep(0.25) /* %s */;", second_marker);
    if (observed == 0)
        observed = send_psql(&controlled, command);
    if (observed == 0)
        observed = observe_controlled_backend(
            psql, user, socket_dir, port, appname, layout->major, true,
            &second_expected);
    if (observed == 0) {
        second_expected.activity_marker_required = true;
        second_base = resolve_controlled_snapshot(
            second_expected.pid, idle_base, my_be_entry_addr, layout,
            &second_expected, second_marker, &second_snapshot);
        if (!second_base)
            observed = -1;
    }

    pid_t controlled_pid = idle_expected.pid;
    /* Finish the final statement, then deliver EOF and drain psql's small
     * result stream before reaping it.  Closing stdout first would give psql
     * SIGPIPE after the validation had already succeeded. */
    if (controlled.in) {
        fclose(controlled.in);
        controlled.in = NULL;
    }
    char discard[256];
    while (fgets(discard, sizeof(discard), controlled.out))
        ;
    int controlled_status = pgwt_proc_close(&controlled);
    bool retired = controlled_pid > 0 &&
                   wait_for_pid_exit(controlled_pid) == 0;
    if (!retired) {
        char reason[192];
        snprintf(reason, sizeof(reason),
                 "controlled validation backend did not retire before capture "
                 "(pid=%d observation=%d client_status=%d)",
                 controlled_pid, observed, controlled_status);
        degrade_unvalidated(layout, reason);
        return -2;
    }
    if (observed == -2) {
        degrade_unvalidated(layout,
                            "validation observer did not retire before capture");
        return -2;
    }
    if (observed != 0 || controlled_status != 0) {
        degrade_unvalidated(layout,
                            "controlled backend/auth unavailable; no offset accepted");
        return -1;
    }

    bool same_backend = idle_expected.pid == first_expected.pid &&
                        idle_expected.pid == between_expected.pid &&
                        idle_expected.pid == second_expected.pid &&
                        idle_expected.databaseid == first_expected.databaseid &&
                        idle_expected.databaseid == second_expected.databaseid &&
                        idle_expected.userid == first_expected.userid &&
                        idle_expected.userid == second_expected.userid;
    struct PgBackendStatusLayout first_result = *layout;
    (void)pgwt_pgbs_validate_snapshot(&first_result, &first_snapshot,
                                      &first_expected);
    int rc = pgwt_pgbs_validate_snapshot(layout, &second_snapshot,
                                         &second_expected);

    bool idle_state = has_read(&idle_snapshot, PGWT_PGBS_READ_STATE) &&
        idle_snapshot.state <= (uint32_t)layout->state_max &&
        idle_snapshot.state != (uint32_t)layout->state_running &&
        idle_snapshot.state != (uint32_t)layout->state_fastpath;
    bool active_transition = same_backend && idle_state &&
        first_result.st_state.validation == PGWT_PGBS_FIELD_VALIDATED &&
        idle_snapshot.state != first_snapshot.state;
    if (!active_transition) {
        layout->st_state.validation = PGWT_PGBS_FIELD_INVALID;
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_STATE;
        rc = -1;
    }

    bool query_varied = layout->major < 14 ||
        (same_backend && first_expected.query_id_available &&
         second_expected.query_id_available &&
         first_expected.query_id != 0 && second_expected.query_id != 0 &&
         first_expected.query_id != second_expected.query_id &&
         first_result.st_query_id.validation == PGWT_PGBS_FIELD_VALIDATED &&
         layout->st_query_id.validation == PGWT_PGBS_FIELD_VALIDATED);
    if (!query_varied && layout->major >= 14) {
        layout->st_query_id.validation = PGWT_PGBS_FIELD_INVALID;
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_QUERY_ID;
        rc = -1;
    }
    layout->validation = layout->fallback_mask
        ? PGWT_PGBS_VALIDATION_DEGRADED
        : PGWT_PGBS_VALIDATION_VALIDATED;
    if (layout->validation == PGWT_PGBS_VALIDATION_VALIDATED) {
        if (layout->major < 14)
            set_detail(layout,
                       "controlled backend retired before capture; idle/active state and activity markers validated; query ID ABSENT");
        else
            set_detail(layout,
                       "controlled backend retired before capture; idle/active state, activity markers and varying query IDs validated");
    } else {
        set_detail(layout,
                   "controlled sequence failed: same_backend=%s state_transition=%s activity_marker=%s query_varied=%s",
                   same_backend ? "yes" : "no",
                   active_transition ? "yes" : "no",
                   layout->st_activity_raw.validation ==
                       PGWT_PGBS_FIELD_VALIDATED ? "yes" : "no",
                   layout->major < 14 ? "n/a" :
                       (query_varied ? "yes" : "no"));
    }
    return rc;
}

int pgwt_pgbs_validate_uprobe_shadow(struct PgBackendStatusLayout *layout,
                                     pid_t backend_pid,
                                     uint64_t my_be_entry_addr,
                                     bool cmd_open,
                                     bool query_id_available,
                                     uint64_t query_id)
{
    if (!layout || layout->source == PGWT_PGBS_SOURCE_NONE || backend_pid <= 0)
        return -1;
    struct pgwt_pgbs_expected expected = {
        .pid = backend_pid,
        .state_shadow_available = true,
        .state_active = cmd_open,
        .query_id_available = layout->major >= 14 && query_id_available &&
                              query_id != 0,
        .query_id = query_id,
    };
    struct pgwt_pgbs_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    uint64_t base = 0;
    int mem_fd = open_mem(backend_pid);
    if (mem_fd >= 0) {
        if (my_be_entry_addr)
            if (pread_exact(mem_fd, &base, sizeof(base),
                            my_be_entry_addr) < 0)
                base = 0;
        if (base && !candidate_ok(mem_fd, base, layout, &expected, NULL,
                                  &snapshot))
            base = 0;
        close(mem_fd);
    }
    if (!base)
        base = scan_shared_for_entry(backend_pid, layout, &expected, NULL,
                                     &snapshot);
    if (!base)
        return -1;
    return pgwt_pgbs_validate_snapshot(layout, &snapshot, &expected);
}

void pgwt_pgbs_warmup_note(struct pgwt_pgbs_warmup_evidence *evidence,
                           const struct PgBackendStatusLayout *candidate,
                           bool state_active,
                           bool query_id_available,
                           uint64_t query_id)
{
    if (!evidence || !candidate || !candidate->validated_pid)
        return;
    evidence->observations++;
    if (state_active && candidate->st_state.validation ==
                        PGWT_PGBS_FIELD_VALIDATED)
        evidence->state_matches++;
    if (candidate->st_activity_raw.validation == PGWT_PGBS_FIELD_VALIDATED)
        evidence->activity_matches++;
    if (query_id_available && query_id != 0 &&
        candidate->st_query_id.validation == PGWT_PGBS_FIELD_VALIDATED) {
        evidence->query_matches++;
        if (!evidence->query_id_seen) {
            evidence->query_id_seen = true;
            evidence->first_query_id = query_id;
        } else if (query_id != evidence->first_query_id) {
            evidence->query_id_varied = true;
        }
    }
}

bool pgwt_pgbs_warmup_complete(
    const struct pgwt_pgbs_warmup_evidence *evidence, int pg_major)
{
    if (!evidence || evidence->state_matches < 3 ||
        evidence->activity_matches < 3)
        return false;
    return pg_major < 14 ||
           (evidence->query_matches >= 3 && evidence->query_id_varied);
}

void pgwt_pgbs_warmup_apply(struct PgBackendStatusLayout *layout,
                            const struct pgwt_pgbs_warmup_evidence *evidence)
{
    if (!layout || !evidence)
        return;
    bool state_ok = evidence->state_matches >= 3;
    bool activity_ok = evidence->activity_matches >= 3;
    bool query_ok = layout->major < 14 ||
        (evidence->query_matches >= 3 && evidence->query_id_varied);

    layout->st_state.validation = state_ok
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;
    layout->st_activity_raw.validation = activity_ok
        ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;
    if (layout->major >= 14)
        layout->st_query_id.validation = query_ok
            ? PGWT_PGBS_FIELD_VALIDATED : PGWT_PGBS_FIELD_INVALID;
    else
        layout->st_query_id.validation = PGWT_PGBS_FIELD_ABSENT;

    layout->fallback_mask = 0;
    if (!state_ok)
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_STATE;
    if (!activity_ok)
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_ACTIVITY_RAW;
    if (!query_ok)
        layout->fallback_mask |= PGWT_PGBS_FALLBACK_QUERY_ID;
    layout->validation = layout->fallback_mask
        ? PGWT_PGBS_VALIDATION_DEGRADED
        : PGWT_PGBS_VALIDATION_VALIDATED;
}

/* ── Reporting ────────────────────────────────────────────── */

unsigned pgwt_pgbs_fallback_count(const struct PgBackendStatusLayout *layout)
{
    if (!layout)
        return 0;
    uint32_t mask = layout->fallback_mask;
    unsigned count = 0;
    while (mask) {
        count += mask & 1u;
        mask >>= 1;
    }
    return count;
}

static const char *field_validation_name(enum pgwt_pgbs_field_validation value)
{
    switch (value) {
    case PGWT_PGBS_FIELD_VALIDATED: return "validated";
    case PGWT_PGBS_FIELD_ABSENT:    return "ABSENT";
    case PGWT_PGBS_FIELD_INVALID:   return "invalid";
    default:                        return "not-run";
    }
}

static void log_field(const char *name, const struct pgwt_pgbs_field *field)
{
    if (field->present)
        fprintf(stderr, " %s=%u/%u(%s)", name, field->offset, field->width,
                field_validation_name(field->validation));
    else
        fprintf(stderr, " %s=ABSENT", name);
}

void pgwt_pgbs_log(const struct PgBackendStatusLayout *layout)
{
    if (!layout)
        return;
    fprintf(stderr,
            "INFO: PgBackendStatus layout PG%d source=%s arch=%s ptr=%u "
            "abi=%s size=%u validation=%s pid=%d enums=RUNNING:%d,"
            "FASTPATH:%d,max:%d\n",
            layout->major, pgwt_pgbs_source_name(layout->source),
            pgwt_pgbs_arch_name(layout->arch), layout->pointer_width,
            pgwt_pgbs_abi_name(layout->abi), layout->struct_size,
            pgwt_pgbs_validation_name(layout->validation),
            layout->validated_pid, layout->state_running,
            layout->state_fastpath, layout->state_max);
    fprintf(stderr, "INFO: PgBackendStatus fields:");
    log_field("st_changecount", &layout->st_changecount);
    log_field("st_procpid", &layout->st_procpid);
    log_field("st_databaseid", &layout->st_databaseid);
    log_field("st_userid", &layout->st_userid);
    log_field("st_state", &layout->st_state);
    log_field("st_activity_raw", &layout->st_activity_raw);
    log_field("st_query_id", &layout->st_query_id);
    if (layout->st_query_id.present)
        fprintf(stderr, "[%s]",
                layout->st_query_id_signed ? "signed" : "unsigned");
    fprintf(stderr, "\nINFO: PgBackendStatus validation detail: %s\n",
            layout->detail);

    if (layout->fallback_mask) {
        fprintf(stderr, "WARN: PgBackendStatus layout fallback:");
        if (layout->fallback_mask & PGWT_PGBS_FALLBACK_STATE)
            fprintf(stderr, " st_state->activity-uprobe");
        if (layout->fallback_mask & PGWT_PGBS_FALLBACK_ACTIVITY_RAW) {
            if (layout->major == 13)
                fprintf(stderr,
                        " st_activity_raw->PG13-ExecutorStart-uprobe");
            else
                fprintf(stderr,
                        " st_activity_raw->activity/query-text-uprobe");
        }
        if (layout->fallback_mask & PGWT_PGBS_FALLBACK_QUERY_ID)
            fprintf(stderr, " st_query_id->query-id-uprobe");
        fprintf(stderr, " (fallback_fields=%u; no offset guessed)\n",
                pgwt_pgbs_fallback_count(layout));
    }
    fprintf(stderr,
            "INFO: PgBackendStatus layout is shadow-only (Stage 1); "
            "sampler and uprobe paths are unchanged\n");
}
