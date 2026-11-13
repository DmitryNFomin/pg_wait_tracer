#ifndef PGWT_EVIDENCE_CODEC_H
#define PGWT_EVIDENCE_CODEC_H

#include "evidence_replay.h"

/* In-memory v5 codec. No IO, global state, platform headers, or default writer
 * version. Input buffers and their strings must remain immutable until free.
 * Physical-position deduplication is deliberately reserved for U7b. */
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
typedef struct {
    size_t input_file, block_offset, record_offset;
    /* Exclusive byte boundary of the last intact block (0 for bad headers).
     * A record fault has a validated block but a physical record boundary. */
    size_t validated_end;
    pgwt_codec_reason reason;
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
typedef struct {
    pgwt_evidence_file *files;
    pgwt_codec_file_info *info;
    size_t file_count;
    pgwt_codec_stop *stops;
    size_t stop_count;
} pgwt_codec_decoded;

/* Return 0 for complete, 1 for an explicitly bounded prefix, -1 for invalid
 * API arguments/allocation failure. Output must be fresh or freed before reuse.
 * A bad header rejects that buffer; identified failures stop only that capture,
 * allowing independent captures in later buffers. Record bodies are
 * validated only after ALL framing/integrity checks for their segment pass.
 * Contract precedence (frozen documents are deliberately unchanged):
 * CODEC.md:479-481,574-577,585-606 call record faults continued global loss;
 * REVISION-r3.md:7-15 and CONTRACT.md:235-254 require a physical chain stop.
 * Thus unsupported_shape marks a stop, never a synthetic VOID. CODEC.md:334-339
 * (§5) and :577 also call null certificate counters a fault; r3:14 and CONTRACT.md:284-288
 * instead retain their readable loss semantics. Writer refusal still applies.
 * CODEC.md:146-149 repeats the original baseline, whereas CONTRACT.md:22-23
 * requires the running baseline. Replay checks that semantic restatement.
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
const char *pgwt_codec_reason_name(pgwt_codec_reason reason);
#endif
