#ifndef PGWT_EVIDENCE_CODEC_H
#define PGWT_EVIDENCE_CODEC_H

#include "evidence_replay.h"

/* In-memory v5 codec. No IO, global state, platform headers, or default writer
 * version. Input buffers and their strings must remain immutable until free.
 * Decode reconciles a whole multiset; reevaluate the accumulated buffers after
 * each update, including updates that retract a conflicting suffix/capture. */
typedef struct { const unsigned char *data; size_t size; } pgwt_codec_buffer;
typedef struct { unsigned char *data; size_t size; } pgwt_codec_bytes;
typedef struct { uint32_t id; unsigned role; pgwt_core_event name; } pgwt_codec_mapping;
typedef struct { uint64_t query_id, cpu_ns; } pgwt_codec_transition;
typedef struct { size_t block_offset, record_offset; } pgwt_codec_position;

typedef struct {
    uint16_t flags;
    uint32_t pg_version;
    uint64_t start_time_ns, clock_offset_ns, nominal_sample_period_ns;
    const pgwt_codec_mapping *mapping;
    size_t mapping_count;
    /* Optional metadata, indexed exactly like logical.records. */
    const pgwt_codec_transition *transitions;
    bool footer;
    /* 0 selects 4096. Smaller batches are useful for bounded callers. */
    size_t block_records;
} pgwt_codec_descriptor;

typedef enum {
    PGWT_CODEC_NONE, PGWT_CODEC_HEADER, PGWT_CODEC_SCHEMA, PGWT_CODEC_BOUNDS,
    PGWT_CODEC_FRAMING, PGWT_CODEC_CRC, PGWT_CODEC_LINK,
    PGWT_CODEC_ENCODING, PGWT_CODEC_RECORD
} pgwt_codec_reason;
/* REDELIVERY clause 3: these numeric values ARE the closed reason ranking.
 * The older pgwt_codec_reason is retained as byte-validation detail only. */
typedef enum {
    PGWT_CODEC_HC, PGWT_CODEC_BC, PGWT_CODEC_SC, PGWT_CODEC_PU,
    PGWT_CODEC_BAD_CRC, PGWT_CODEC_TRUNCATED, PGWT_CODEC_INVALID_HEADER,
    PGWT_CODEC_INVALID_BLOCK, PGWT_CODEC_RF
} pgwt_codec_code;
typedef struct { size_t first, second; } pgwt_codec_witness;
typedef struct {
    /* input_file now indexes canonical deliveries, NEVER the caller's array.
     * group==SIZE_MAX means unassigned, and is diagnostic-only. */
    size_t input_file, block_offset, record_offset;
    /* Non-normative scan boundary of the canonical first offending delivery;
     * conflicts use their block boundary; headers/incoming links use zero. */
    size_t validated_end;
    pgwt_codec_reason reason; /* non-normative validation detail */
    pgwt_codec_code code;
    bool has_record_offset; /* null sorts before even payload offset zero */
    size_t group;
    /* second==SIZE_MAX for offending deliveries; HC/BC unordered; SC roles
     * are first=predecessor, second=successor. Indices name deliveries[]. */
    pgwt_codec_witness *witnesses;
    size_t witness_count;
} pgwt_codec_stop;

typedef struct {
    pgwt_codec_descriptor descriptor;
    pgwt_codec_buffer bytes;
    size_t header_size, data_end, block_count;
    uint64_t previous_data_end;
    unsigned char previous_sha256[32];
    pgwt_codec_position *positions;
    /* Validated sample-stream state, including across intervening exact blocks
     * and file rotation; samples themselves do not enter the logical array. */
    bool has_samples;
    uint64_t last_sample_time, last_sample_weight;
} pgwt_codec_file_info;
typedef struct { pgwt_core_event boot, capture; uint64_t file_no; } pgwt_codec_identity;
typedef struct {
    size_t predecessor, validated_end;
    bool clean, static_match, boundary, exact_end, extension;
    /* digest_match meaningful only at a validated boundary */
    bool digest_match, witness, seal_proof;
} pgwt_codec_link_check;
typedef enum { PGWT_CODEC_ROOT, PGWT_CODEC_WITNESSED,
               PGWT_CODEC_UNAVAILABLE, PGWT_CODEC_SEAL_CONFLICT } pgwt_codec_link;
typedef struct {
    pgwt_codec_buffer bytes;
    unsigned char sha256[32]; /* id=(bytes.size,sha256(all bytes)) */
    size_t group; /* SIZE_MAX for an independently invalid header */
    size_t validated_end;
    bool clean;
    uint64_t previous_data_end;
    unsigned char previous_sha256[32];
    pgwt_codec_link link;
    pgwt_codec_link_check *checks; /* ALL own-group predecessor alternatives */
    size_t check_count;
} pgwt_codec_delivery;
typedef struct {
    pgwt_codec_identity identity;
    size_t *sources, source_count; /* all maximum-E copies, empty on HC */
    bool admitted;
    size_t file_index; /* files[] index, SIZE_MAX when not admitted */
} pgwt_codec_group;
typedef struct {
    pgwt_core_event capture;
    pgwt_core_event *boots; size_t boot_count;
    size_t *witnesses; size_t witness_count; /* all header-valid deliveries */
    /* ambiguous is the separate CAPTURE_BOOT_AMBIGUITY outcome (clause 2a),
     * never a codec reason. Witness boot/ordinal come from deliveries[].group;
     * every ambiguity witness has header position (0,null). */
    bool ambiguous, failed, has_replay;
    pgwt_evidence_capture_report replay; /* final state, owned arrays */
    size_t replay_group; /* SIZE_MAX unless replay stopped */
    pgwt_codec_position replay_position; /* descriptor=(0,0), see replay.stop */
} pgwt_codec_capture;
typedef struct {
    /* Only admitted descriptors and eligible prefix records, canonical chain
     * order. No malformed record tokens/tombstones escape. */
    pgwt_evidence_file *files;
    /* info retains the whole validated source prefix, including blocks after
     * a cutoff; positions/transitions are indexed by the retained records.
     * bytes/flags/footer describe the first canonical source carrier, not a
     * completeness claim. groups[].sources is the complete provenance set. */
    pgwt_codec_file_info *info;
    size_t file_count;
    pgwt_codec_stop *stops; size_t stop_count; /* normalized, verified groups */
    pgwt_codec_stop *diagnostics; size_t diagnostic_count; /* delivery level */
    pgwt_codec_delivery *deliveries; size_t delivery_count;
    pgwt_codec_group *groups; size_t group_count;
    pgwt_codec_capture *captures; size_t capture_count;
    bool failed, unassigned_header_failure; /* aggregate failure, clause 0 */
} pgwt_codec_decoded;

/* Return 0 for aggregate PASS, 1 for FAILED with separately usable results,
 * -1 for invalid API arguments/allocation failure (output zeroed).
 * Caller-visible U7b changes: all buffers are independently inspected before
 * reconciliation; duplicate deliveries collapse; files/diagnostics/witnesses
 * use canonical identity order. Header failures are unassigned diagnostics,
 * never normalized stops. Records exclude the offending unit and later suffix.
 * Replay stops/state remain separate from codec stops and codec record arrays.
 * Baselines are checked only by pgwt_evidence_replay_report, including empty
 * successor descriptors. Ambiguity withholds the entire affected capture.
 * All arrays belong to out; strings and bytes borrow the immutable buffers.
 * Canonical delivery order is length, SHA256, then bytes (no collision branch).
 * Canonical groups sort by boot, capture, ordinal; captures and boots by bytes.
 * PGWT_CODEC_RF diagnostics for ignored unqualified certificates do not stop.
 * The legacy reason field is detail; code supplies the normative vocabulary.
 */
int pgwt_codec_decode(const pgwt_codec_buffer *buffers, size_t count,
                      pgwt_codec_decoded *out);
void pgwt_codec_decoded_free(pgwt_codec_decoded *out);

/* Encode one logical file. previous is mandatory for file_no>0 and must be
 * the complete predecessor bytes, with optional valid footer. Its digest is
 * computed here, omitting flags/footer. No output is published on failure.
 * Writer validation refuses malformed/unsupported records, including unknown
 * certificate counters. Semantic multi-record checks remain replay's job. */
int pgwt_codec_encode(const pgwt_evidence_file *file,
                      const pgwt_codec_descriptor *descriptor,
                      const pgwt_codec_buffer *previous,
                      pgwt_codec_bytes *out, const char **reason);
void pgwt_codec_bytes_free(pgwt_codec_bytes *bytes);

uint32_t pgwt_codec_crc32(const void *bytes, size_t size);
void pgwt_codec_sha256(const void *bytes, size_t size, unsigned char digest[32]);
const char *pgwt_codec_code_name(pgwt_codec_code code);
const char *pgwt_codec_reason_name(pgwt_codec_reason reason);
#endif
