## Row 32
Output N: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12`  
Output Q: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12`  
Exit: no  
Reasoning: The qualified FORK establishes D through 25b and initializes Q’s next_q to 1. At W=13, both records are eligible. D12 establishes the coverage lower bound.

## Row 33
Output N: `VOID20[C0,unknown_evidence]`  
Output Q: `VOID20[C0,unknown_evidence]`  
Exit: no  
Reasoning: An ordinary FORK without INIT supplies neither creation route. Validation fails before publishing any part of the eligible batch.

## Row 34
Output N: `VOID20[C0,unknown_evidence]`  
Output Q: `VOID20[C0,unknown_evidence]`  
Exit: no  
Reasoning: A is a previously known generation of PID 10 without an EXIT preceding D’s FORK. D therefore fails the creation-admission requirement.

## Row 35
Output N: `VOID20[C0,unknown_evidence]`  
Output Q: `VOID20[C0,unknown_evidence]`  
Exit: no  
Reasoning: D has neither an attachment proof nor valid creation evidence. identity_known alone does not establish admission or Q’s sequence baseline.

## Row 36
Output N: `FORK_D10{fork_seed_proved=true} → INIT_D11 → TRANS_D12[U] → F12`  
Output Q: `FORK_D10{fork_seed_proved=true} → INIT_D11 → TRANS_D12[U] → F12`  
Exit: no  
Reasoning: The qualified FORK selects 25b despite the genuine INIT. INIT retains timestamp 11 and does not remove D12’s uncertified incoming interval.

## Row 37
Output N: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12`  
Output Q: `VOID20[C0,sequence_gap]`  
Exit: no  
Reasoning: N omits q and accepts the qualified creation. In Q, creation initializes next_q=1, but the complete eligible batch starts at q2; validation fails before publication.

## Row 38
Output N: `VOID20[C0,unknown_evidence]`  
Output Q: `VOID20[C0,unknown_evidence]`  
Exit: no  
Reasoning: S(10) makes D9 eligible while FORK10 remains ineligible. The creation proof cannot precede D9 in validated output order. Suspension suppresses subsequent output.

## Row 39
Output N: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12`  
Output Q: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12`  
Exit: no  
Reasoning: Both arrivals are staged at W=0. At W=13, validated output order places FORK10 before TRANS12, satisfying creation admission regardless of arrival order.

## Row 40
Output N: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12 → INIT_D14 → TRANS_D15[T] → F15`  
Output Q: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12 → INIT_D14 → TRANS_D15[T] → F15`  
Exit: no  
Reasoning: D12 establishes the coverage lower bound. The later INIT is idempotent and preserves sequence and coverage state. S(16) releases INIT14 and the subsequent certified TRANS15.

## Row 41
Output N: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12 → VOID20[C0,unknown_evidence]`  
Output Q: `FORK_D10{fork_seed_proved=true} → TRANS_D12[U] → F12 → VOID20[C0,unknown_evidence]`  
Exit: no  
Reasoning: The first release succeeds. The explicit FAULT then emits VOID and suspends the epoch; S(16) cannot resume trusted output.

## Row 42
Output N: `FORK_D10{fork_seed_proved=true} → MARKER_D12[U] → TRANS_D14[T] → F14`  
Output Q: `FORK_D10{fork_seed_proved=true} → MARKER_D12[U] → TRANS_D14[T] → F14`  
Exit: no  
Reasoning: The qualified marker establishes fresh state and D’s coverage lower bound at 12. TRANS14 is subsequent and receives [T]; Q validates consecutive q1, q2.

## Row 43
Output N: `VOID20[Ci,counter_change]`  
Output Q: `VOID20[Ci,counter_change]`  
Exit: no  
Reasoning: CHECK observes Ci differing from C0 and immediately suspends the epoch. Later unchanged observations generate no additional counter-change report, and the cookie cannot revive the epoch.

## Ambiguities

None. No row requires a special feed-call return status.