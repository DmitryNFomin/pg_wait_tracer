# Redelivery cases for independent answers

The fixture notation below defines the entire case table. Cases describe **codec record arrays**, with replay effects stated explicitly where tested.

- All fixtures use raw blocks, one row/record per block, valid integrity fields unless expressly changed, zero flags/creation metadata, qualified=true and fifteen zero baseline counters. Boot is `"b"`; captures are `"a"` for A/N/L/R, `"m"` for M and `"x"` for X. M alone maps event ID 1 to wait name `"w"`; other mappings are empty. Type-2 timestamp hints are zero. Unmentioned query IDs and CPU values are zero.
- `LIFE(p)` means `t=0,pid=p,fork=0,start=0,start_known=true,exit=absent`. `ATTACH(t)` means pid=1, fork=0, reread, seed/state=null and zero counters. `FRONTIER(t)` has zero counters. All record contents named below include these fields.
- `A` is clean file 0 with four **type-2** blocks: `a0=LIFE(1)`, `a1=ATTACH(0)`, `a2=FRONTIER(10)`, `a3=FRONTIER(20)`. H=193; block starts are `[193,305,537,747]`; D=957.
- `A+` appends a fifth **type-2** block at 957 containing `a4=VOID(t=30,counters=zero,reason="x",invalid_from=20)`. Its end is 1179. This is the only VOID in the fixtures.
- `N` is clean file 1, referencing A’s D=957 and digest, with four type-2 records `n0…n3=LIFE(2)…LIFE(5)`. `L` is clean file 2, referencing N, with `l0…l3=LIFE(6)…LIFE(9)`. Both have H=241, starts `[241,353,465,577]`, D=689.
- `X` is independent clean file 0 with four type-2 records `x0…x3=LIFE(10)…LIFE(13)`, H=193, starts `[193,305,417,529]`, D=641.
- `M` is clean file 0 with four blocks: type 0 `m0=TRANS(t=1,pid=1,old=null,new="w",duration=0)`; type 1 one sample `(t=2,pid=1,event="w",weight=1)`; type 2 `m2=LIFE(1)`; type 0 `m3=TRANS(t=3,pid=1,old="w",new=null,duration=2)`. The sample period and maximum weight are both 1. H=201; starts `[201,292,377,489]`; D=580. There is no attachment.
- For a base F, `FtK` truncates it exactly one byte into zero-based block K; exactly K blocks remain validated. `FcrcK` flips one bit of block K’s stored CRC, preserving all other bytes.
- `FcJ` changes block J validly: increment its type-2 record’s t by 1, or its type-0/1 row’s query_id by 1; repair that block’s CRC and all subsequent CRC chaining. Lengths remain unchanged. `FcJtK` then applies the truncation. Thus `Ac2t1` is byte-identical to `At1`; `Mt3`, `Mc1t3` and `Lc2t3` are defined.
- `Ag` appends byte `00` to A; `Ag′` appends byte `01`. Both have four validated blocks and a truncated tail at 957. `Ao` adds A’s exact 68-byte footer. `Aft` appends only its first 17 bytes, yielding a truncated tail at 957.
- `Af` changes only A’s flags to 1. `Ah` changes A’s immutable start_time_ns from 0 to 1 and repairs header/block integrity. `Z` instead flips one bit of A’s stored header CRC without repair; it has no trusted physical-file identity.
- `Nb` changes N’s predecessor digest by flipping its first bit; `Ni` changes N’s referenced D to 956 and stores the digest through that byte offset. Both repair their own header/block integrity; neither modification changes block sizes.
- `Au` changes A’s block-1 record kind to 255, preserving its bounded body/common t and repairing CRC chaining. `Auc3` additionally applies the valid block-3 change. Both have four validated blocks and an RF at `(305,8)`.
- `R` has A’s header and four type-2 records `r0=LIFE(1)`, `r1=ATTACH(0)`, `r2=ATTACH(1)`, `r3=FRONTIER(20)`. Starts are `[193,305,537,769]`; D=979. `Rc3` changes its last frontier to 21. The overlapping attachment is a replay fault at `(537,8)`.
- A named record denotes its full physical identity **and the contents above**. Single type-2 record offsets are 8; single type-0/1 row offsets are 0. `a0…a3` means four ordered positions, not a timestamp range.
- `S(F,o,r;W)` is a normalized codec stop; scalar o abbreviates `(o,null)`. W is a delivery set, or a set of proving pairs for conflicts. `d(delivery,o,r)` is an independent failure diagnostic. In the diagnostics column, **S** expands to diagnostics underlying every listed stop, including its witnesses and required link evidence; `+` adds suppressed failures. Names denote their content-derived identifiers, so no hash must be guessed.

| # | Input multiset | Codec records | Normalized codec stops | Diagnostics / explicit replay effect |
|---:|---|---|---|---|
| 1 | `At1,At3` | | | |
| 2 | `A,At1` | | | |
| 3 | `Ag,At1` | | | |
| 4 | `A,Acrc2` | | | |
| 5 | `A,Ac2t1` | | | |
| 6 | `A,Ac2t3` | | | |
| 7 | `A,Ah` | | | |
| 8 | `A,Ag,Nt2` | | | |
| 9 | `At3,N` | | | |
| 10 | `At1,At3,A` | | | |
| 11 | `At1,X,At3` | | | |
| 12 | `A,A,Af,Ao` | | | |
| 13 | `At3,Acrc3` | | | |
| 14 | `M,Mt3` | | | |
| 15 | `M,Mc1t3` | | | |
| 16 | `A,A+,N` | | | |
| 17 | `A,Z` | | | |
| 18 | `A+,N` | | | |
| 19 | `Ag,N` | | | |
| 20 | `At1,N,L,Lc2t3` | | | |
| 21 | `Ag,Ag′` | | | |
| 22 | `A,A+` | | | |
| 23 | `A,L` | | | |
| 24 | `N` | | | |
| 25 | `Aft` | | | |
| 26 | `A,Aft` | | | |
| 27 | `A+,Nb` | | | |
| 28 | `A+,Ni` | | | |
| 29 | `Au,Auc3` | | | |
| 30 | `R,Rc3` | | | |
| 31 | `Z` | | | |
