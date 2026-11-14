# Redelivery r3b cases for independent answers

The fixture notation below defines the entire case table. Derive codec record arrays and replay effects from these inputs and the normative rules.

- Unless explicitly overridden, fixtures use raw blocks, one row/record per block, valid integrity fields, zero flags/creation metadata, qualified=true and fifteen zero baseline counters. Boot is `"b"`; captures are `"a"` for A/N/L/R and their variants, `"m"` for M and `"x"` for X. M alone maps event ID 1 to wait name `"w"`; other mappings are empty. Type-2 timestamp hints are zero. Unmentioned query IDs and CPU values are zero.
- `LIFE(p)` means `t=0,pid=p,fork=0,start=0,start_known=true,exit=absent`. `ATTACH(t)` means pid=1, fork=0, reread, seed/state=null and zero counters. `FRONTIER(t)` has zero counters. All record contents named below include these fields.
- `A` is clean file 0 with four **type-2** blocks: `a0=LIFE(1)`, `a1=ATTACH(0)`, `a2=FRONTIER(10)`, `a3=FRONTIER(20)`. H=193; block starts are `[193,305,537,747]`; D=957.
- `A+` appends a fifth type-2 block to A at 957 containing `a4=VOID(t=30,counters=B0,reason="x",invalid_from=20)`, ending at 1179. This is the only VOID in cases 1–31.
- `N` is clean file 1, referencing A’s D=957 and digest, with four type-2 records `n0…n3=LIFE(2)…LIFE(5)`. `L` is clean file 2, referencing N, with `l0…l3=LIFE(6)…LIFE(9)`. Both have H=241, starts `[241,353,465,577]`, D=689.
- `X` is independent clean file 0 with four type-2 records `x0…x3=LIFE(10)…LIFE(13)`, H=193, starts `[193,305,417,529]`, D=641.
- `M` is clean file 0 with four blocks: type 0 `m0=TRANS(t=1,pid=1,old=null,new="w",duration=0)`; type 1 one sample `(t=2,pid=1,event="w",weight=1)`; type 2 `m2=LIFE(1)`; type 0 `m3=TRANS(t=3,pid=1,old="w",new=null,duration=2)`. The sample period and maximum weight are both 1. H=201; starts `[201,292,377,489]`; D=580. There is no attachment.
- For a base F, `FtK` truncates it exactly one byte into zero-based block K. `FcrcK` flips one bit of block K’s stored CRC, preserving all other bytes.
- `FcJ` changes block J validly: increment its type-2 record’s t by 1, or its type-0/1 row’s query_id by 1; repair that block’s CRC and all subsequent CRC chaining. Lengths remain unchanged. `FcJtK` then applies the truncation. Thus `Ac2t1` is byte-identical to `At1`; `Mt3`, `Mc1t3` and `Lc2t3` are defined.
- `Ag` appends byte `00` to A; `Ag′` appends byte `01`. `Ao` adds A’s exact 68-byte footer. `Aft` appends only its first 17 bytes.
- `Af` changes only A’s flags to 1. `Ah` changes A’s immutable start_time_ns from 0 to 1 and repairs header/block integrity. `Z` instead flips one bit of A’s stored header CRC without repair.
- `Nb` changes N’s predecessor digest by flipping its first bit; `Ni` changes N’s referenced D to 956 and stores the digest through that byte offset. Both repair their own header/block integrity; neither modification changes block sizes.
- `Au` changes A’s block-1 record kind to 255, preserving its bounded body/common t and repairing CRC chaining. `Auc3` additionally applies the valid block-3 change.
- `R` has A’s header and four type-2 records `r0=LIFE(1)`, `r1=ATTACH(0)`, `r2=ATTACH(1)`, `r3=FRONTIER(20)`. Starts are `[193,305,537,769]`; D=979. `Rc3` changes its last frontier to 21.
- A named record denotes its full physical identity **and the contents above**. Single type-2 record offsets are 8; single type-0/1 row offsets are 0. `a0…a3` means four ordered positions, not a timestamp range.
- `S(F,o,r;W)` is a normalized codec stop; scalar o abbreviates `(o,null)`. W is a delivery set, or a set of proving pairs for conflicts. `d(delivery,o,r)` is an independent failure diagnostic. In the diagnostics column, **S** expands to diagnostics underlying every listed stop, including its witnesses and required link evidence; `+` adds suppressed failures. Names denote their content-derived identifiers, so no hash must be guessed.

- `B0` is fifteen zeros; `B1` differs only at zero-based slot 4, `query_text_drop=1`.
- `A2` is A with boot `"c"` and repaired header/block integrity; all lengths and offsets remain unchanged. `A2t0` retains its complete valid header plus one byte of block 0: length 194.
- `N2` is N with boot `"c"`, referencing A2’s D=957 and digest, with repaired integrity. A2 is not implicitly supplied.
- `Z2` flips one bit of A2’s stored header CRC without repair.
- `V` is A+ with its final VOID counters changed to B1 and repaired integrity. Records are `a0…a3,v4`; `v4=VOID(30,B1,"x",20)` at `(957,8)`, ending at 1179. Header and first four blocks equal A’s.
- `Q` is clean file 1 referencing V’s D=1179 and digest, declaring B1. Its four type-2 records `q0…q3` are LIFE for pid=2…5 respectively, each with t=40, fork=40, start=40, start_known=true, exit=null. H=241; starts `[241,353,465,577]`; D=689.
- `Q0` differs from Q only by declaring B0, with integrity repaired; records remain `q0…q3`.
- `N1` differs from N only by declaring B1, with repaired integrity; it still references A at D=957 and contains `n0…n3`.
- `Vu` is V with final VOID counters=null: records `a0…a3,vu4`, where `vu4=VOID(30,null,"x",20)` at `(957,8)`. That block is 102 bytes; D=1059. Header and preceding blocks equal A’s.
- `Tu` is clean, header-only file 1 referencing Vu at D=1059 with its digest, declaring null; H=D=121. `T0` instead declares B0; H=D=241. Both contain no records.
- `W` is A followed by one type-2 block `w4=FRONTIER(t=30,counters=B1)` at `(957,8)`. Its final block is 210 bytes; D=1167.
- `Qw` is Q with predecessor reference changed to W at D=1167 and its digest. `Qw0` also changes baseline to B0. Their records and offsets remain `q0…q3`.
- `Wi` is W with qualified=false and repaired header/block integrity. `Qwi0` is Qw0 with qualified=false and predecessor digest/reference recomputed for Wi. `Qwi1` differs from Qwi0 only by declaring B1. All retain their corresponding record contents and offsets.
- `Vi` is V with qualified=false and repaired integrity. `Qvi` is Q with qualified=false and predecessor digest/reference recomputed for Vi.
- All modified fixtures repair dependent integrity fields unless expressly stated otherwise. No fixture implicitly adds a predecessor or footer.

`U(c;W)` denotes CAPTURE_BOOT_AMBIGUITY for capture c and complete delivery-witness set W; report the observed boot set as well. Each witness supplies its delivery identity, boot, file ordinal and header position `(0,null)`.
`P(F;expected,declared)` denotes the separate baseline-mismatch replay stop at F’s descriptor `(0,null)`. `S` and `d` retain their meanings above.

For replay descriptions, `HW` means publication high-water, distinct from header length H.

| # | Input multiset | Codec records | Normalized codec stops | Diagnostics / explicit replay effect | Aggregate |
|---:|---|---|---|---|---|
| 17 | `A,Z` |  |  |  |  |
| 31 | `Z` |  |  |  |  |
| 32 | `A,A2` |  |  |  |  |
| 33 | `A,N2` |  |  |  |  |
| 34 | `A,A2,X` |  |  |  |  |
| 35 | `A,A2,A2` |  |  |  |  |
| 36 | `V,Q` |  |  |  |  |
| 37 | `V,Q0` |  |  |  |  |
| 38 | `A,N` |  |  |  |  |
| 39 | `A,V,V,Q` |  |  |  |  |
| 40 | `V,N1` |  |  |  |  |
| 41 | `A,N1` |  |  |  |  |
| 42 | `A,Z2` |  |  |  |  |
| 43 | `Vu,Tu` |  |  |  |  |
| 44 | `Vu,T0` |  |  |  |  |
| 45 | `W,Qw` |  |  |  |  |
| 46 | `W,Qw0` |  |  |  |  |
| 47 | `A,A2t0` |  |  |  |  |
| 48 | `Wi,Qwi0` |  |  |  |  |
| 49 | `Wi,Qwi1` |  |  |  |  |
| 50 | `Vi,Qvi` |  |  |  |  |
