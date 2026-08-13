/* plan_tree.c — Execution plan tree sidecar implementation
 *
 * Walks PlannedStmt->Plan in backend memory at ExecutorStart (once per unique query_id)
 * and records the plan tree structure into plan_trees.jsonl.
 */
#define _GNU_SOURCE
#include "plan_tree.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

/* NodeTag mapping per major PostgreSQL version */
static const char *nodetag_name_pg13(uint32_t tag)
{
    switch (tag) {
    case 246: return "Result";
    case 247: return "ProjectSet";
    case 248: return "ModifyTable";
    case 249: return "Append";
    case 250: return "Merge Append";
    case 251: return "Recursive Union";
    case 252: return "BitmapAnd";
    case 253: return "BitmapOr";
    case 254: return "Seq Scan";
    case 255: return "Sample Scan";
    case 256: return "Index Scan";
    case 257: return "Index Only Scan";
    case 258: return "Bitmap Index Scan";
    case 259: return "Bitmap Heap Scan";
    case 260: return "Tid Scan";
    case 261: return "Subquery Scan";
    case 262: return "Function Scan";
    case 263: return "Values Scan";
    case 265: return "CTE Scan";
    case 267: return "WorkTable Scan";
    case 268: return "Foreign Scan";
    case 269: return "Custom Scan";
    case 270: return "Nested Loop";
    case 271: return "Merge Join";
    case 272: return "Hash Join";
    case 273: return "Materialize";
    case 274: return "Sort";
    case 275: return "Incremental Sort";
    case 276: return "Group";
    case 277: return "Aggregate";
    case 278: return "WindowAgg";
    case 279: return "Unique";
    case 280: return "Gather";
    case 281: return "Gather Merge";
    case 282: return "Hash";
    case 283: return "SetOp";
    case 284: return "LockRows";
    case 285: return "Limit";
    default:  return NULL;
    }
}

static const char *nodetag_name_pg14(uint32_t tag)
{
    switch (tag) {
    case 267: return "Result";
    case 268: return "ProjectSet";
    case 269: return "ModifyTable";
    case 270: return "Append";
    case 271: return "Merge Append";
    case 272: return "Recursive Union";
    case 273: return "BitmapAnd";
    case 274: return "BitmapOr";
    case 275: return "Seq Scan";
    case 276: return "Sample Scan";
    case 277: return "Index Scan";
    case 278: return "Index Only Scan";
    case 279: return "Bitmap Index Scan";
    case 280: return "Bitmap Heap Scan";
    case 281: return "Tid Scan";
    case 282: return "Tid Range Scan";
    case 283: return "Subquery Scan";
    case 284: return "Function Scan";
    case 285: return "Values Scan";
    case 287: return "CTE Scan";
    case 289: return "WorkTable Scan";
    case 290: return "Foreign Scan";
    case 291: return "Custom Scan";
    case 292: return "Nested Loop";
    case 293: return "Merge Join";
    case 294: return "Hash Join";
    case 295: return "Materialize";
    case 296: return "Sort";
    case 297: return "Incremental Sort";
    case 298: return "Group";
    case 299: return "Aggregate";
    case 300: return "WindowAgg";
    case 301: return "Unique";
    case 302: return "Gather";
    case 303: return "Gather Merge";
    case 304: return "Hash";
    case 305: return "SetOp";
    case 306: return "LockRows";
    case 307: return "Limit";
    default:  return NULL;
    }
}

static const char *nodetag_name_pg15(uint32_t tag)
{
    switch (tag) {
    case 286: return "Result";
    case 287: return "ProjectSet";
    case 288: return "ModifyTable";
    case 289: return "Append";
    case 290: return "Merge Append";
    case 291: return "Recursive Union";
    case 292: return "BitmapAnd";
    case 293: return "BitmapOr";
    case 294: return "Seq Scan";
    case 295: return "Sample Scan";
    case 296: return "Index Scan";
    case 297: return "Index Only Scan";
    case 298: return "Bitmap Index Scan";
    case 299: return "Bitmap Heap Scan";
    case 300: return "Tid Scan";
    case 301: return "Tid Range Scan";
    case 302: return "Subquery Scan";
    case 303: return "Function Scan";
    case 304: return "Values Scan";
    case 306: return "CTE Scan";
    case 308: return "WorkTable Scan";
    case 309: return "Foreign Scan";
    case 310: return "Custom Scan";
    case 311: return "Nested Loop";
    case 312: return "Merge Join";
    case 313: return "Hash Join";
    case 314: return "Materialize";
    case 315: return "Sort";
    case 316: return "Incremental Sort";
    case 317: return "Group";
    case 318: return "Aggregate";
    case 319: return "WindowAgg";
    case 320: return "Unique";
    case 321: return "Gather";
    case 322: return "Gather Merge";
    case 323: return "Hash";
    case 324: return "SetOp";
    case 325: return "LockRows";
    case 326: return "Limit";
    default:  return NULL;
    }
}

static const char *nodetag_name_pg16(uint32_t tag)
{
    switch (tag) {
    case 307: return "Result";
    case 308: return "ProjectSet";
    case 309: return "ModifyTable";
    case 310: return "Append";
    case 311: return "Merge Append";
    case 312: return "Recursive Union";
    case 313: return "BitmapAnd";
    case 314: return "BitmapOr";
    case 315: return "Seq Scan";
    case 316: return "Sample Scan";
    case 317: return "Index Scan";
    case 318: return "Index Only Scan";
    case 319: return "Bitmap Index Scan";
    case 320: return "Bitmap Heap Scan";
    case 321: return "Tid Scan";
    case 322: return "Tid Range Scan";
    case 323: return "Subquery Scan";
    case 324: return "Function Scan";
    case 325: return "Values Scan";
    case 327: return "CTE Scan";
    case 329: return "WorkTable Scan";
    case 330: return "Foreign Scan";
    case 331: return "Custom Scan";
    case 332: return "Nested Loop";
    case 333: return "Merge Join";
    case 334: return "Hash Join";
    case 335: return "Materialize";
    case 337: return "Sort";
    case 338: return "Incremental Sort";
    case 339: return "Group";
    case 340: return "Aggregate";
    case 341: return "WindowAgg";
    case 342: return "Unique";
    case 343: return "Gather";
    case 344: return "Gather Merge";
    case 345: return "Hash";
    case 346: return "SetOp";
    case 347: return "LockRows";
    case 348: return "Limit";
    default:  return NULL;
    }
}

static const char *nodetag_name_pg17(uint32_t tag)
{
    switch (tag) {
    case 327: return "Result";
    case 328: return "ProjectSet";
    case 329: return "ModifyTable";
    case 330: return "Append";
    case 331: return "Merge Append";
    case 332: return "Recursive Union";
    case 333: return "BitmapAnd";
    case 334: return "BitmapOr";
    case 335: return "Seq Scan";
    case 336: return "Sample Scan";
    case 337: return "Index Scan";
    case 338: return "Index Only Scan";
    case 339: return "Bitmap Index Scan";
    case 340: return "Bitmap Heap Scan";
    case 341: return "Tid Scan";
    case 342: return "Tid Range Scan";
    case 343: return "Subquery Scan";
    case 344: return "Function Scan";
    case 345: return "Values Scan";
    case 347: return "CTE Scan";
    case 349: return "WorkTable Scan";
    case 350: return "Foreign Scan";
    case 351: return "Custom Scan";
    case 352: return "Nested Loop";
    case 354: return "Merge Join";
    case 355: return "Hash Join";
    case 356: return "Materialize";
    case 358: return "Sort";
    case 359: return "Incremental Sort";
    case 360: return "Group";
    case 361: return "Aggregate";
    case 362: return "WindowAgg";
    case 363: return "Unique";
    case 364: return "Gather";
    case 365: return "Gather Merge";
    case 366: return "Hash";
    case 367: return "SetOp";
    case 368: return "LockRows";
    case 369: return "Limit";
    default:  return NULL;
    }
}

static const char *nodetag_name_pg18(uint32_t tag)
{
    switch (tag) {
    case 332: return "Result";
    case 333: return "ProjectSet";
    case 334: return "ModifyTable";
    case 335: return "Append";
    case 336: return "Merge Append";
    case 337: return "Recursive Union";
    case 338: return "BitmapAnd";
    case 339: return "BitmapOr";
    case 340: return "Seq Scan";
    case 341: return "Sample Scan";
    case 342: return "Index Scan";
    case 343: return "Index Only Scan";
    case 344: return "Bitmap Index Scan";
    case 345: return "Bitmap Heap Scan";
    case 346: return "Tid Scan";
    case 347: return "Tid Range Scan";
    case 348: return "Subquery Scan";
    case 349: return "Function Scan";
    case 350: return "Values Scan";
    case 352: return "CTE Scan";
    case 354: return "WorkTable Scan";
    case 355: return "Foreign Scan";
    case 356: return "Custom Scan";
    case 357: return "Nested Loop";
    case 359: return "Merge Join";
    case 360: return "Hash Join";
    case 361: return "Materialize";
    case 363: return "Sort";
    case 364: return "Incremental Sort";
    case 365: return "Group";
    case 366: return "Aggregate";
    case 367: return "WindowAgg";
    case 368: return "Unique";
    case 369: return "Gather";
    case 370: return "Gather Merge";
    case 371: return "Hash";
    case 372: return "SetOp";
    case 373: return "LockRows";
    case 374: return "Limit";
    default:  return NULL;
    }
}

const char *pgwt_nodetag_name(uint32_t tag, int pg_major_version)
{
    const char *n = NULL;
    if (pg_major_version >= 18)
        n = nodetag_name_pg18(tag);
    else if (pg_major_version == 17)
        n = nodetag_name_pg17(tag);
    else if (pg_major_version == 16)
        n = nodetag_name_pg16(tag);
    else if (pg_major_version == 15)
        n = nodetag_name_pg15(tag);
    else if (pg_major_version == 14)
        n = nodetag_name_pg14(tag);
    else if (pg_major_version <= 13 && pg_major_version > 0)
        n = nodetag_name_pg13(tag);

    if (!n) {
        /* Fallback: try PG17 then PG18 */
        n = nodetag_name_pg17(tag);
        if (!n) n = nodetag_name_pg18(tag);
    }
    return n ? n : "Plan Node";
}

void pgwt_pt_format_label(const struct pgwt_plan_node *node,
                          char *buf, size_t buf_sz)
{
    if (!node || !buf || buf_sz == 0)
        return;

    buf[0] = '\0';
    const char *type = node->type[0] ? node->type : "Plan Node";

    if (node->index_name[0] && node->rel[0]) {
        snprintf(buf, buf_sz, "%s on %s using %s", type, node->rel, node->index_name);
    } else if (node->rel[0] && node->alias[0]) {
        snprintf(buf, buf_sz, "%s on %s (%s)", type, node->rel, node->alias);
    } else if (node->rel[0]) {
        snprintf(buf, buf_sz, "%s on %s", type, node->rel);
    } else {
        snprintf(buf, buf_sz, "%s", type);
    }

    if (node->workers > 0) {
        size_t len = strlen(buf);
        if (len + 16 < buf_sz)
            snprintf(buf + len, buf_sz - len, " (%d workers)", node->workers);
    }
}

static void pt_log_cap_once(struct pgwt_plan_tree_capture *pt)
{
    if (pt->cap_logged)
        return;
    pt->cap_logged = true;
    fprintf(stderr, "WARN: plan-tree id table is FULL (%d unique query_ids) — "
            "plan trees for NEW query_ids will no longer be captured until daemon restart\n",
            PGWT_PT_HT_SIZE);
}

static uint64_t pt_hash64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/* Returns 0 on newly inserted, 1 on already seen, 2 on table full. */
static int pt_seen_check_or_insert(struct pgwt_plan_tree_capture *pt, uint64_t query_id)
{
    uint32_t idx = (uint32_t)(pt_hash64(query_id) & (PGWT_PT_HT_SIZE - 1));
    for (int i = 0; i < PGWT_PT_HT_SIZE; i++) {
        if (pt->seen[idx] == query_id)
            return 1;
        if (pt->seen[idx] == 0) {
            if (pt->num_seen >= PGWT_PT_HT_SIZE)
                return 2;
            pt->seen[idx] = query_id;
            pt->num_seen++;
            return 0;
        }
        idx = (idx + 1) & (PGWT_PT_HT_SIZE - 1);
    }
    return 2;
}

#define PT_COMPACT_THRESHOLD (10 * 1024 * 1024) /* 10 MB default */

/* Parse query_id from JSONL line */
static int pt_parse_line_qid(const char *line, uint64_t *qid)
{
    if (!line) return -1;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    if (len == 0 || line[len - 1] != '}')
        return -1;

    const char *q = strstr(line, "\"q\":\"");
    if (q) {
        *qid = strtoull(q + 5, NULL, 10);
        return (*qid != 0) ? 0 : -1;
    }
    q = strstr(line, "\"q\":");
    if (q) {
        *qid = strtoull(q + 4, NULL, 10);
        return (*qid != 0) ? 0 : -1;
    }
    return -1;
}

/* Load existing query_ids with DUR-4 atomic compaction if over threshold */
static void pt_load_existing(struct pgwt_plan_tree_capture *pt, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return;

    long threshold = PT_COMPACT_THRESHOLD;
    const char *env = getenv("PGWT_PT_COMPACT_BYTES");
    if (env && atol(env) > 0)
        threshold = atol(env);
    int compact = st.st_size > threshold;

    FILE *in = fopen(path, "r");
    if (!in)
        return;

    FILE *out = NULL;
    char tmp_path[600];
    if (compact) {
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        out = fopen(tmp_path, "w");
        if (!out)
            compact = 0;
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t len;
    int loaded = 0, dropped = 0;
    while ((len = getline(&line, &line_cap, in)) > 0) {
        uint64_t qid = 0;
        if (pt_parse_line_qid(line, &qid) != 0 || qid == 0) {
            dropped++;
            continue;
        }
        int rc = pt_seen_check_or_insert(pt, qid);
        if (rc == 0)
            loaded++;
        else if (rc == 2)
            pt_log_cap_once(pt);

        if (compact && (rc == 0 || rc == 2)) {
            if (fwrite(line, 1, (size_t)len, out) != (size_t)len) {
                fclose(out);
                out = NULL;
                unlink(tmp_path);
                compact = 0;
            }
        }
    }
    free(line);
    fclose(in);

    if (compact && out) {
        fflush(out);
        fsync(fileno(out));
        fclose(out);
        if (rename(tmp_path, path) != 0)
            unlink(tmp_path);
        else if (pt->verbose)
            fprintf(stderr, "INFO: compacted plan_trees.jsonl\n");
    }

    if (pt->verbose)
        fprintf(stderr, "INFO: loaded %d existing plan tree ids (%d unreadable lines skipped)%s\n",
                loaded, dropped, pt->cap_logged ? " — id table full" : "");
}

int pgwt_pt_init(struct pgwt_plan_tree_capture *pt, const char *trace_dir,
                 int pg_major_version, int verbose, gid_t trace_gid)
{
    if (!pt || !trace_dir)
        return -1;

    memset(pt, 0, sizeof(*pt));
    snprintf(pt->trace_dir, sizeof(pt->trace_dir), "%s", trace_dir);
    pt->pg_version = pg_major_version;
    pt->verbose = (verbose != 0);
    pt->trace_gid = trace_gid;

    char path[512];
    snprintf(path, sizeof(path), "%s/plan_trees.jsonl", trace_dir);

    /* DUR-4: dedup on load + bounded compaction, then APPEND */
    pt_load_existing(pt, path);

    pt->fp = fopen(path, "a");
    if (!pt->fp) {
        fprintf(stderr, "WARN: cannot open %s for plan-tree capture: %s\n",
                path, strerror(errno));
        return -1;
    }

    /* Set 0640 permissions (DUR-10) */
    if (trace_gid != (gid_t)-1) {
        (void)fchown(fileno(pt->fp), (uid_t)-1, trace_gid);
    }
    fchmod(fileno(pt->fp), 0640);

    pt->enabled = true;
    return 0;
}

void pgwt_pt_close(struct pgwt_plan_tree_capture *pt)
{
    if (!pt)
        return;
    if (pt->fp) {
        fclose(pt->fp);
        pt->fp = NULL;
    }
    pt->enabled = false;
}

void pgwt_pt_set_offsets(struct pgwt_plan_tree_capture *pt,
                         int plan_type, int plan_plannodeid,
                         int plan_lefttree, int plan_righttree,
                         int plan_sizeof, int plannedstmt_plantree,
                         int gather_num_workers)
{
    if (!pt)
        return;
    pt->plan_type_off = plan_type;
    pt->plan_plannodeid_off = plan_plannodeid;
    pt->plan_lefttree_off = plan_lefttree;
    pt->plan_righttree_off = plan_righttree;
    pt->plan_sizeof = plan_sizeof;
    pt->plannedstmt_plantree_off = plannedstmt_plantree;
    pt->gather_num_workers_off = gather_num_workers;
}

/* Escape a string for JSON output */
static void write_json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"') fputs("\\\"", fp);
        else if (*s == '\\') fputs("\\\\", fp);
        else if (*s == '\n') fputs("\\n", fp);
        else if (*s == '\r') fputs("\\r", fp);
        else if (*s == '\t') fputs("\\t", fp);
        else if ((unsigned char)*s < 0x20) fprintf(fp, "\\u%04x", (unsigned char)*s);
        else fputc(*s, fp);
    }
    fputc('"', fp);
}

void pgwt_pt_store_nodes(struct pgwt_plan_tree_capture *pt,
                         pid_t pid, uint64_t query_id, uint64_t wall_ns,
                         const struct pgwt_plan_node *nodes, int num_nodes)
{
    if (!pt || !pt->enabled || !pt->fp || !nodes || num_nodes <= 0 || query_id == 0)
        return;

    int rc = pt_seen_check_or_insert(pt, query_id);
    if (rc != 0) {
        if (rc == 2)
            pt_log_cap_once(pt);
        return;
    }

    fprintf(pt->fp, "{\"q\":\"%llu\",\"pid\":%d,\"ts\":%llu",
            (unsigned long long)query_id, (int)pid,
            (unsigned long long)wall_ns);

    if (pt->pg_version > 0)
        fprintf(pt->fp, ",\"pg_version\":%d", pt->pg_version);

    fprintf(pt->fp, ",\"nodes\":[");
    for (int i = 0; i < num_nodes; i++) {
        const struct pgwt_plan_node *n = &nodes[i];
        if (i > 0) fputc(',', pt->fp);
        fprintf(pt->fp, "{\"id\":%u,\"tag\":%u", n->id, n->tag);
        if (n->type[0]) {
            fprintf(pt->fp, ",\"type\":");
            write_json_str(pt->fp, n->type);
        }
        if (n->rel[0]) {
            fprintf(pt->fp, ",\"rel\":");
            write_json_str(pt->fp, n->rel);
        }
        if (n->alias[0]) {
            fprintf(pt->fp, ",\"alias\":");
            write_json_str(pt->fp, n->alias);
        }
        if (n->filter[0]) {
            fprintf(pt->fp, ",\"filter\":");
            write_json_str(pt->fp, n->filter);
        }
        if (n->workers > 0) {
            fprintf(pt->fp, ",\"workers\":%d", n->workers);
        }
        if (n->has_left) {
            fprintf(pt->fp, ",\"left_id\":%u", n->left_id);
        }
        if (n->has_right) {
            fprintf(pt->fp, ",\"right_id\":%u", n->right_id);
        }

        char label[256];
        pgwt_pt_format_label(n, label, sizeof(label));
        if (label[0]) {
            fprintf(pt->fp, ",\"label\":");
            write_json_str(pt->fp, label);
        }
        fputc('}', pt->fp);
    }
    fprintf(pt->fp, "]}\n");
    fflush(pt->fp);
}

static int read_u32(int fd, uint64_t addr, uint32_t *out)
{
    if (pread(fd, out, sizeof(*out), (off_t)addr) != (ssize_t)sizeof(*out))
        return -1;
    return 0;
}

static int read_ptr(int fd, uint64_t addr, uint64_t *out)
{
    if (pread(fd, out, sizeof(*out), (off_t)addr) != (ssize_t)sizeof(*out))
        return -1;
    return 0;
}

static int walk_plan(struct pgwt_plan_tree_capture *pt, int fd, uint64_t plan_addr,
                     struct pgwt_plan_node *nodes, int *num_nodes)
{
    if (!plan_addr || *num_nodes >= PGWT_PT_MAX_NODES)
        return 0;

    uint32_t type_tag = 0;
    uint32_t node_id = 0;
    uint64_t left = 0, right = 0;

    if (read_u32(fd, plan_addr + (uint64_t)pt->plan_type_off, &type_tag) < 0)
        return -1;
    if (read_u32(fd, plan_addr + (uint64_t)pt->plan_plannodeid_off, &node_id) < 0)
        return -1;
    if (read_ptr(fd, plan_addr + (uint64_t)pt->plan_lefttree_off, &left) < 0)
        return -1;
    if (read_ptr(fd, plan_addr + (uint64_t)pt->plan_righttree_off, &right) < 0)
        return -1;

    /* Validate plausibility: tag should be > 0 and node_id < 100000 */
    if (type_tag > 0 && node_id < 100000 && *num_nodes < PGWT_PT_MAX_NODES) {
        struct pgwt_plan_node *n = &nodes[*num_nodes];
        memset(n, 0, sizeof(*n));
        n->id = node_id;
        n->tag = type_tag;
        snprintf(n->type, sizeof(n->type), "%s",
                 pgwt_nodetag_name(type_tag, pt->pg_version));

        /* Gather / Gather Merge: workers offset */
        const char *tname = n->type;
        if (strcmp(tname, "Gather") == 0 || strcmp(tname, "Gather Merge") == 0) {
            if (pt->gather_num_workers_off > 0) {
                uint32_t workers = 0;
                if (read_u32(fd, plan_addr + (uint64_t)pt->gather_num_workers_off, &workers) == 0
                    && workers > 0 && workers < 1024)
                    n->workers = (int)workers;
            }
        }

        if (left) {
            uint32_t lid = 0;
            if (read_u32(fd, left + (uint64_t)pt->plan_plannodeid_off, &lid) == 0) {
                n->left_id = lid;
                n->has_left = 1;
            }
        }
        if (right) {
            uint32_t rid = 0;
            if (read_u32(fd, right + (uint64_t)pt->plan_plannodeid_off, &rid) == 0) {
                n->right_id = rid;
                n->has_right = 1;
            }
        }

        (*num_nodes)++;
    }

    if (left && walk_plan(pt, fd, left, nodes, num_nodes) < 0)
        return -1;
    if (right && walk_plan(pt, fd, right, nodes, num_nodes) < 0)
        return -1;

    return 0;
}

int pgwt_pt_capture_plannedstmt(struct pgwt_plan_tree_capture *pt,
                               pid_t pid, uint64_t query_id,
                               uint64_t wall_ns, uint64_t plannedstmt)
{
    if (!pt || !pt->enabled || !pt->fp || !plannedstmt)
        return -1;
    if (pt->plan_plannodeid_off <= 0 || pt->plannedstmt_plantree_off <= 0)
        return -1;

    uint64_t seen_key = query_id ? query_id : plannedstmt;
    uint32_t idx = (uint32_t)(pt_hash64(seen_key) & (PGWT_PT_HT_SIZE - 1));
    for (int i = 0; i < PGWT_PT_HT_SIZE; i++) {
        if (pt->seen[idx] == seen_key)
            return 0; /* already seen */
        if (pt->seen[idx] == 0)
            break;
        idx = (idx + 1) & (PGWT_PT_HT_SIZE - 1);
    }
    if (pt->num_seen >= PGWT_PT_HT_SIZE) {
        pt_log_cap_once(pt);
        return 0;
    }

    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", (int)pid);
    int fd = open(mem_path, O_RDONLY);
    if (fd < 0) {
        if (pt->verbose)
            fprintf(stderr, "WARN: plan capture: cannot open %s: %s\n",
                    mem_path, strerror(errno));
        return -1;
    }

    uint64_t plan_tree = 0;
    if (read_ptr(fd, plannedstmt + (uint64_t)pt->plannedstmt_plantree_off, &plan_tree) < 0
        || plan_tree == 0) {
        close(fd);
        return -1;
    }

    struct pgwt_plan_node nodes[PGWT_PT_MAX_NODES];
    int num_nodes = 0;
    int rc = walk_plan(pt, fd, plan_tree, nodes, &num_nodes);
    close(fd);

    if (rc < 0 || num_nodes == 0)
        return -1;

    if (wall_ns == 0) {
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        wall_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
    }

    pgwt_pt_store_nodes(pt, pid, seen_key, wall_ns, nodes, num_nodes);
    return 0;
}
