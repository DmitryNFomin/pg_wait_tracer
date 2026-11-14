/* Frozen before adapter implementation; copied from Astra adapter r2 G1-G12.
 * These byte strings and expected core inputs are independent of ma_map.
 * Exact LE layouts; the omitted tail of Q is 248 zero bytes. */
static const unsigned char golden_T[48] = {
    0x0a,0,0,0,0,0,0,0, 0x0a,0,0,0, 1,0,0,3, 0,0,0,0, 2,0,0,0,
    7,0,0,0,0,0,0,0, 0xbc,0x0a,0,0,0,0,0,0, 3,0,0,0,0,0,0,0
};
static const unsigned char golden_F[24] = {
    1,0,0,0, 0x1e,0,0,0, 0,0,0,0,0,0,0,0, 0x0a,0,0,0,0,0,0,0
};
static const unsigned char golden_I[24] = {
    2,0,0,0, 0x1e,0,0,0, 0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
    0x0b,0,0,0,0,0,0,0
};
static const unsigned char golden_X[24] = {
    3,0,0,0, 0x1e,0,0,0, 0,0,0,0,0,0,0,0, 0x0e,0,0,0,0,0,0,0
};
static const unsigned char golden_Q[272] = {
    4,0,0,0, 0x14,0,0,0, 0xbc,0x0a,0,0,0,0,0,0,
    0x73,0x65,0x6c,0x65,0x63,0x74,0x20,0x31
};
#define GOLD_OBS {.d=20, .counters={.known=true}}
#define GOLD_TRACE_RECORD .source=0,.o=1,.t=10,.pid=10,.g=1, \
    .q=1,.rank=PGWT_MERGE_TRANS,.identity_known=true,.has_q=true, \
    .dependency=PGWT_MERGE_READY,.payload=golden_T,.payload_size=48
/* All unmentioned fields are zero, including TEXT's DATA-only fields. */
static const pgwt_merge_input golden_trace = {
    .kind=PGWT_MERGE_DATA,.observation=GOLD_OBS,.record={GOLD_TRACE_RECORD}
};
static const pgwt_merge_input golden_fork = {
    .kind=PGWT_MERGE_DATA,.observation=GOLD_OBS,
    .record={.source=3,.o=1,.t=10,.pid=30,.g=1,.rank=PGWT_MERGE_FORK,
             .identity_known=true,.fork_seed_proved=true,.dependency=PGWT_MERGE_READY,
             .payload=golden_F,.payload_size=24}
};
static const pgwt_merge_input golden_init = {
    .kind=PGWT_MERGE_DATA,.observation=GOLD_OBS,
    .record={.source=3,.o=1,.t=11,.pid=30,.g=1,.rank=PGWT_MERGE_INIT,
             .identity_known=true,.dependency=PGWT_MERGE_READY,
             .payload=golden_I,.payload_size=24}
};
static const pgwt_merge_input golden_exit = {
    .kind=PGWT_MERGE_DATA,.observation=GOLD_OBS,
    .record={.source=3,.o=1,.t=14,.pid=30,.g=1,.rank=PGWT_MERGE_EXIT,
             .identity_known=true,.dependency=PGWT_MERGE_READY,
             .payload=golden_X,.payload_size=24}
};
static const pgwt_merge_input golden_text = {
    .kind=PGWT_MERGE_TEXT,.observation=GOLD_OBS,
    .record={.source=3,.o=4,.dependency_id=7,.payload=golden_Q,.payload_size=272}
};
static const pgwt_merge_input golden_ring_boundary = {
    .kind=PGWT_MERGE_BOUNDARY,.observation=GOLD_OBS,.source=2,.p=4,.w=11
};
static const pgwt_merge_input golden_idle_boundary = {
    .kind=PGWT_MERGE_BOUNDARY,.observation=GOLD_OBS,.source=1,.p=4,.w=11
};
static const pgwt_merge_input golden_loss = {
    .kind=PGWT_MERGE_LOSS,.source=1,.n=3,
    .observation={.d=12,.counters={.known=true,.value={[11]=1}}}
};
static const pgwt_merge_input golden_held = {
    .kind=PGWT_MERGE_CHECK,
    .observation={.d=9,.counters={.known=true,.value={[11]=1}}}
};
static const pgwt_merge_input golden_writer = {
    .kind=PGWT_MERGE_WRITER_REJECTED,
    .observation={.d=9,.counters={.known=true,.value={[14]=1}}}
};
static const pgwt_merge_input golden_interrupted = {
    .kind=PGWT_MERGE_FAULT,.reason=PGWT_MERGE_UNKNOWN_EVIDENCE,.observation=GOLD_OBS
};
/* G1: absent and present-zero cookies => g0/unknown; historical g1 remains known.
 * G2: E/o9/q3; N => q0/has_q=false.
 * G3: WAIT7, absent binding => FAULT unknown_evidence, LOST7.
 * G4: new_event=FFFFFFFF/duration0; old=new=FFFFFFF0/F1/F2/F3/F6/F7 on E.
 * G5: PENDING=>CHECK/hold; PROVED=>golden_fork; FAILED=>FAULT,FORK(false);
 *     NOT_ATTEMPTED=>FORK(false).
 * G6: golden_init and golden_exit (no proof/q; unchanged address bytes).
 * G7: golden_text, same query binding as pid10 despite TEXT pid20.
 * G8: golden_ring_boundary, golden_idle_boundary; NONE=>CHECK/hold;
 *     a NONE declaration refuses create and produces no core output.
 * G9: golden_loss; Ce remains exactly component11=1, no LOST addition.
 * G10: golden_held; invalid component4/absent snapshot=>known=false.
 * G11: golden_writer; sink returns false with precisely its observation, no feed.
 * G12: golden_interrupted; next proof FAILED, INIT cannot restore it.
 */
#undef GOLD_OBS
#undef GOLD_TRACE_RECORD
