/* plan_tree.h — Execution plan tree sidecar (plan_trees.jsonl)
 *
 * Captures query plan trees once per unique query_id at ExecutorStart.
 * Stores raw NodeTag numbers, resolved operator types across PostgreSQL 13-18,
 * child edges (left_id/right_id), relation metadata, and parallel worker counts.
 *
 * Adheres to DUR-4 (append-only, dedup on capture) and DUR-10 (file permissions
 * 0640 and log-once saturation cap).
 */
#ifndef PGWT_PLAN_TREE_H
#define PGWT_PLAN_TREE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#define PGWT_PT_MAX_NODES   128
#define PGWT_PT_MAX_REL      64
#define PGWT_PT_MAX_ALIAS    32
#define PGWT_PT_MAX_FILTER  128
#define PGWT_PT_MAX_DETAIL  128
#define PGWT_PT_MAX_TYPE     32
#define PGWT_PT_MAX_INDEX    64
#define PGWT_PT_HT_SIZE    4096

struct pgwt_plan_node {
    uint32_t id;                        /* Plan.plan_node_id (0-indexed) */
    uint32_t tag;                       /* Raw NodeTag value */
    char     type[PGWT_PT_MAX_TYPE];     /* Operator type e.g. "Seq Scan", "Hash Join" */
    char     rel[PGWT_PT_MAX_REL];       /* Relation name if known */
    char     alias[PGWT_PT_MAX_ALIAS];   /* Table alias if known */
    char     filter[PGWT_PT_MAX_FILTER]; /* Filter expression */
    char     detail[PGWT_PT_MAX_DETAIL];
    char     index_name[PGWT_PT_MAX_INDEX];
    int      has_filter;
    int      workers;                   /* Gather / Gather Merge worker count (0 = N/A) */
    uint32_t left_id;                   /* Left child plan_node_id */
    uint32_t right_id;                  /* Right child plan_node_id */
    int      has_left;
    int      has_right;
};

struct pgwt_plan_tree_capture {
    char    trace_dir[256];
    FILE   *fp;
    bool    enabled;
    bool    verbose;
    int     pg_version;                 /* Major version (13, 14, 15, 16, 17, 18) */
    gid_t   trace_gid;

    /* Offsets for live capture via /proc/<pid>/mem (from discovery) */
    int     plan_type_off;              /* offsetof(Node, type) = 0 */
    int     plan_plannodeid_off;        /* offsetof(Plan, plan_node_id) = 40 */
    int     plan_lefttree_off;          /* offsetof(Plan, lefttree) = 64 */
    int     plan_righttree_off;         /* offsetof(Plan, righttree) = 72 */
    int     plan_sizeof;                /* sizeof(Plan) = 104 */
    int     plannedstmt_plantree_off;   /* PG13-17: 32, PG18: 40 */
    int     gather_num_workers_off;     /* offsetof(Gather, num_workers) = 104 */

    /* Dedup: open-addressing hash table of seen query_ids (DUR-4 / DUR-10) */
    uint64_t seen[PGWT_PT_HT_SIZE];
    int      num_seen;
    bool     cap_logged;
};

/* Resolve a raw PostgreSQL NodeTag to a human-readable operator name. */
const char *pgwt_nodetag_name(uint32_t tag, int pg_major_version);

/* Initialize plan tree capture to trace_dir/plan_trees.jsonl. */
int  pgwt_pt_init(struct pgwt_plan_tree_capture *pt, const char *trace_dir,
                  int pg_major_version, int verbose, gid_t trace_gid);
void pgwt_pt_close(struct pgwt_plan_tree_capture *pt);

/* Set runtime struct offsets resolved during discovery. */
void pgwt_pt_set_offsets(struct pgwt_plan_tree_capture *pt,
                         int plan_type, int plan_plannodeid,
                         int plan_lefttree, int plan_righttree,
                         int plan_sizeof, int plannedstmt_plantree,
                         int gather_num_workers);

/* Write a pre-built plan tree to plan_trees.jsonl. */
void pgwt_pt_store_nodes(struct pgwt_plan_tree_capture *pt,
                         pid_t pid, uint64_t query_id, uint64_t wall_ns,
                         const struct pgwt_plan_node *nodes, int num_nodes);

/* Walk PlannedStmt in backend memory (/proc/<pid>/mem) and write plan_trees.jsonl. */
int  pgwt_pt_capture_plannedstmt(struct pgwt_plan_tree_capture *pt,
                                 pid_t pid, uint64_t query_id,
                                 uint64_t wall_ns, uint64_t plannedstmt);

/* Format standard display label for a plan node (e.g. "Seq Scan on users (u)"). */
void pgwt_pt_format_label(const struct pgwt_plan_node *node,
                          char *buf, size_t buf_sz);

#endif /* PGWT_PLAN_TREE_H */
