/* Coverage label: the one verdict function (coverage spec r2 §2 plus
 * Amendment A1). Pure: no I/O, no allocation, no globals; the only output
 * storage is the caller's. Not wired into any consumer yet (U2b, U3).
 * Refs #947, #871. */
#ifndef PGWT_COVERAGE_H
#define PGWT_COVERAGE_H

#include <stddef.h>
#include <stdint.h>

/* Times are nanoseconds. Record times are the boot's monotonic clock;
 * windows, `now`, legacy ranges and every output are wall time, where
 * wall = mono + the boot's generation offset. */
#define PGWT_COV_INF  INT64_MAX
#define PGWT_COV_NONE INT64_MIN
/* L: a live boot's last COVERAGE record is at most this old (§2 Live). */
#define PGWT_COV_LIVE_L_NS INT64_C(3000000000)

/* One decoded COVERAGE fact, in file order. A boot's records start with the
 * FILE fact of its earliest retained file; every later file opens with its
 * own FILE fact. */
enum pgwt_cov_rec_kind {
    PGWT_COV_REC_FILE = 1,  /* a BOOT observation, b file_no, c 1 readable / 0 unreadable */
    PGWT_COV_REC_WATERMARK, /* a W, b floor_db, c floor_idle */
    PGWT_COV_REC_GAP_OPEN,  /* a start, c gap_id */
    PGWT_COV_REC_GAP_CLOSE, /* a end, c gap_id */
    PGWT_COV_REC_SEAL,      /* a W_final */
    PGWT_COV_REC_BAD,       /* bad CRC or unknown kind: the rest of the file is lost */
};

struct pgwt_cov_rec {
    int kind;
    int64_t a, b, c;
};

/* Control-socket evidence for liveness (§2 Live). */
enum pgwt_cov_status {
    PGWT_COV_STATUS_NONE,     /* no control socket */
    PGWT_COV_STATUS_MATCH,    /* status reports this boot's nonce */
    PGWT_COV_STATUS_MISMATCH, /* status reports another nonce */
};

struct pgwt_cov_boot {
    int64_t boot_start;        /* mono */
    int64_t wall_offset;       /* generation offset */
    uint64_t postmaster_start; /* wall; 0 = unknown */
    int closure_clean;         /* the boot's trace closure is CLEAN */
    int same_kernel_boot;      /* kernel boot_id equals the server's */
    int status;                /* enum pgwt_cov_status */
    int64_t last_obs;          /* mono observation of the last COVERAGE record */
    const struct pgwt_cov_rec *recs;
    size_t n_recs;
};

struct pgwt_cov_range {
    int64_t from, to;
};

struct pgwt_cov_model {
    const struct pgwt_cov_boot *boots;   /* ordered by (generation, boot_start) */
    size_t n_boots;
    const struct pgwt_cov_range *legacy; /* wall ranges holding v2-v4 data; not boots (A1 item 3) */
    size_t n_legacy;
};

enum pgwt_cov_floor { PGWT_COV_FLOOR_DB, PGWT_COV_FLOOR_IDLE };

enum pgwt_cov_verdict {
    PGWT_COV_COMPLETE,
    PGWT_COV_LIVE_EDGE,
    PGWT_COV_INCOMPLETE,
    PGWT_COV_ERROR,
};

/* Rule precedence order of §2 States; reasons[] is emitted in this order
 * (A1 item 4), never in time order. */
enum pgwt_cov_reason {
    PGWT_COV_R_CAPTURE_LOSS,
    PGWT_COV_R_MISSING_FILE,
    PGWT_COV_R_UNCLEAN_END,
    PGWT_COV_R_RETENTION,
    PGWT_COV_R_RESTART,
    PGWT_COV_R_CLOCK_STEP,
    PGWT_COV_R_NOT_YET_COVERED,
    PGWT_COV_R_NOT_RECORDED,
    PGWT_COV_R_NOT_CAPTURED,
    PGWT_COV_N_REASONS
};

enum pgwt_cov_error {
    PGWT_COV_E_NONE,
    PGWT_COV_E_INVALID_WINDOW, /* from >= to */
    PGWT_COV_E_BAD_MODEL,      /* NULL arrays, or a boot whose records do not start with FILE */
    PGWT_COV_E_BUFFER_FULL,    /* more uncovered ranges than uncovered_cap */
};

/* A1 item 2. */
enum pgwt_cov_restart {
    PGWT_COV_RESTART_NONE,
    PGWT_COV_RESTART_DAEMON,
    PGWT_COV_RESTART_POSTGRES,
    PGWT_COV_RESTART_UNKNOWN,
};

struct pgwt_cov_span {
    int64_t from, to;
    int reason;
};

struct pgwt_cov_result {
    int verdict;                   /* enum pgwt_cov_verdict */
    int error;                     /* enum pgwt_cov_error; set iff verdict == ERROR */
    int reasons[PGWT_COV_N_REASONS];
    size_t n_reasons;
    int64_t covered_to;            /* live_edge only, else PGWT_COV_NONE */
    struct pgwt_cov_span *uncovered; /* caller's buffer, filled for incomplete */
    size_t uncovered_cap;
    size_t n_uncovered;
    int restart_cause;             /* enum pgwt_cov_restart */
    int excludes_in_flight;
};

/* Fills *out (the caller sets uncovered/uncovered_cap first) and returns
 * out->verdict. floor_kind IDLE gives the separate Idle-row verdict: the
 * same rules with floor_idle in E_b, taint and holes. */
int pgwt_coverage_verdict(const struct pgwt_cov_model *model, int64_t from,
                          int64_t to, int64_t now, int floor_kind,
                          struct pgwt_cov_result *out);

const char *pgwt_cov_verdict_name(int verdict);
const char *pgwt_cov_reason_name(int reason);
const char *pgwt_cov_error_name(int error);
const char *pgwt_cov_restart_name(int cause);

#endif
