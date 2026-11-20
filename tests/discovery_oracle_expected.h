/* Structured transcription of oracle-discovery/EXPECTED.md. N = no bound,
 * F = fresh-record bound, Q = quiet bound. Times are boot-relative ns.
 * The verbatim table remains in discovery_oracle_rows.h for drift detection.
 * Physical producer facts that this consumer cannot test are listed separately
 * in discovery_oracle_assertions.txt; they are never reported as qualified. */
static const struct discovery_expected {
    const char *id, *output;
    int64_t close, lower;
    size_t pending;
    char route;
    bool census;
} discovery_expected[] = {
    {"D01", "D12[U] D15[T] F15[C0]", 12, 12, 0, 'F', true},
    {"D02a", "F11[C0] F12[C0] D15[T] F15[C0]", 12, 12, 0, 'Q', true},
    {"D02b", "F15[C0]", -1, -1, 1, 'N', true},
    {"D02c", "D9[U] D10[T] F12[C0]", 9, 9, 0, 'F', true},
    {"D02d", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D02e", "F15[C0]", -1, -1, 1, 'N', true},
    {"D03", "D12[U] D15[T] F15[C0]", 12, 12, 0, 'F', true},
    {"D04a-install", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D04a-state", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D04a-binding", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D04a-address", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D04a-cookie", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D04b", "F15[C0]", -1, -1, 1, 'N', true},
    {"D04c", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D04d", "", -1, -1, 1, 'N', true},
    {"D04e", "VOID20[Cf,counter_change]", -1, -1, 1, 'N', true},
    {"D04f", "VOID20[C0,allocation_fail]", -1, -1, 1, 'N', true},
    {"D04g", "VOID20[Cs,counter_change]", -1, -1, 1, 'N', true},
    {"D07a", "F20[C0]", 20, 12, 0, 'Q', true},
    {"D07b", "F20[C0] F25[C0]", 25, 12, 0, 'Q', true},
    {"D08a", "F17[C0] EXIT_H18 F18[C0]", 18, 12, 0, 'Q', true},
    {"D08b", "F17[C0] F18[C0]", -1, 12, 1, 'Q', true},
    {"D09", "D12[U] D15[T] terminal_D18[T] EXIT_D18 FORK_R20 R22[U] F22[C0]", 12, 12, 0, 'F', true},
    {"D10", "D12[U] D15[T] F15[C0] F30[C0]", -1, 12, 1, 'F', false},
    {"D13a", "VOID20[Ce,counter_change]", -1, -1, 1, 'N', true},
    {"D13b", "", -1, -1, 1, 'N', true},
    {"D13c", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"D13d", "VOID20[Cw,writer_push_fail]", -1, -1, 1, 'N', true},
    {"D13e", "VOID20[C0,unknown_evidence]", -1, -1, 1, 'N', true},
    {"Q01", "D15[T] F15[C0]", 12, 12, 0, 'Q', true},
    {"Q02", "D15[U] F15[C0]", 15, 15, 0, 'F', true},
    {"Q03", "D15[U] F15[C0]", 15, 15, 0, 'F', true},
    {"Q04", "D15[U] F15[C0]", 15, 15, 0, 'F', true},
    {"Q04b", "D15[U] F15[C0]", 15, 15, 0, 'F', true},
    {"Q05", "D15[U] F15[C0]", 15, 15, 0, 'F', true},
    {"Q06", "D15[U] F15[C0]", 15, 15, 0, 'F', true},
    {"Q07", "D15[T] F15[C0]", 12, 12, 0, 'Q', true},
    {"Q08", "F15[C0]", -1, -1, 1, 'N', true},
    {"Q09", "D9[U] F11[C0] D15[T] F15[C0]", 11, 11, 0, 'Q', true},
};
