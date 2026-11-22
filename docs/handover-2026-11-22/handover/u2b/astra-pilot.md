VERIFIED — `pilot_buffers` loads all `.trace`/`.trace.lz4` files; the query layer skips containers v2/v3/v4/v6 and decodes the remaining inputs as evidence-v1. (`src/server.c:9193`, `src/core/evidence_query.c:9`, `:34`, `:47`)
VERIFIED — Pilot selection uses `target.capture`, PID/fork, and its own query window; metrics come from evidence replay. (`src/core/evidence_query.c:76`, `:103`)
VERIFIED — Both writers can run in one daemon and directory: evidence capture is opt-in, defaults to the trace directory, and publishes `evidence-<identity>.trace` at shutdown. (`src/daemon.c:1538`, `:1906`, `:2269`; `src/evidence_capture.c:104`, `:131`, `:316`)
INFERRED — Thus v5 and v6 can describe overlapping time from the same daemon run; different format versions do **not** imply mutually exclusive captures.
VERIFIED — Their session identities are generated independently: evidence capture ID versus coverage boot nonce. Today’s evidence writer emits LIFE plus VOID, with `qualified=false`. (`src/evidence_capture.c:86`, `:390`, `:410`; `src/event_writer.c:1403`)
INFERRED — Co-location or overlapping timestamps therefore cannot establish that v6 coverage certifies the pilot’s evidence.

INFERRED — **Choose (a) as the correction**, explicitly amending r2 §3; it is not what the current spec literally requires.
VERIFIED — The spec says AND, while existing tests require unchanged v5 metrics alongside v6 COVERAGE files. (`spec-coverage-label-r2.md:85`; `tests/test_pilot_route.py:358`, `:362`, `:372`)
INFERRED — (b) suppresses valid v5-only answers and breaks that contract; “every pilot answer” is too broad because v6 files can coexist, though their certificate is not linked to the pilot capture.
INFERRED — A directory-wide AND or inherited v6 certification would associate unrelated evidence streams; treating v6 records as pilot evidence would invent evidence-v1 guarantees.
INFERRED — A future combined verdict needs an explicit identity/window mapping and reviewed contract changes; it is not today’s implementation.

VERIFIED — Responses already identify `pilot:true`, `contract:"evidence-v1"` and per-metric `basis`, `completeness`, and `reasons`. (`src/server.c:9269`, `:9120`)
INFERRED — Preserve those fields and make the scope explicit to consumers: “Evidence-v1 pilot; v6 trace coverage certification does not apply.” Do not attach a generic coverage-certified badge.
VERIFIED — `compare()` pins the response keys and pilot/contract markers; `startup_refusals()` requires valid v5-only answers; `routing_cases()` checks mixed v5/v6 independence. (`tests/test_pilot_route.py:134`, `:254`, `:327`)
INFERRED — Add a full-handler mixed-directory regression for certification leakage: the existing mixed routing probe bypasses the handler. Any new response field needs an intentional schema/test update. (`tests/test_pilot_route.py:268`, `:287`)