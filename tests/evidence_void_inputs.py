"""Logical evidence-v1 loss witnesses; all answers come from the frozen reference.

No expected values or product replay code are used to generate these inputs.
"""
import copy
import random

MAX = (1 << 64) - 1
IDENTITY = (0, 1, 2, 10, 12, 14)


def life(t=0, pid=101, fork=0, exit=None, start=None):
    return dict(kind="LIFE", t=t, pid=pid, fork=fork,
                start=(fork or 0) if start is None else start,
                start_known=fork is not None, exit=exit)


def attach(t=1, pid=101, fork=0, counters=None, state=None, method="reread"):
    return dict(kind="ATTACH_OK", t=t, pid=pid, fork=fork, method=method,
                seed=state, state=state, counters=([0]*15 if counters is None else counters[:]))


def trans(t, old=None, new="A", duration=0, pid=101):
    return dict(kind="TRANS", t=t, pid=pid, old=old, new=new, duration=duration)


def frontier(t, counters=None):
    return dict(kind="FRONTIER", t=t, counters=([0]*15 if counters is None else counters[:]))


def loss(t=12, slot=11, value=1, invalid=None, unknown=False, counters=None):
    v = [0]*15 if counters is None else counters[:]
    v[slot] = value
    return dict(kind="VOID", t=t, counters=None if unknown else v,
                invalid_from=invalid, reason="opaque storage diagnostic")


def scenario(base, name, records, left=1, right=25, fork=0, pid=101):
    c = copy.deepcopy(base)
    c["id"] = name
    c["files"][0]["records"] = records
    c["target"].update(pid=pid, fork=fork)
    c["query"].update(event="A", **{"from":left, "to":right})
    return c


def named(base):
    cases = []
    def add(name, records, **kw):
        c = scenario(base, "void_" + name, records, **kw)
        cases.append(c)
        return c
    prefix = [life(), attach(), trans(3), frontier(8)]
    ring = loss()["counters"]
    add("before_window", copy.deepcopy(prefix)+[loss(), frontier(24,ring)], left=16, right=24)
    add("inside_window", copy.deepcopy(prefix)+[loss()], left=4, right=16)
    add("after_window", copy.deepcopy(prefix)+[loss()], left=1, right=8)
    add("detection_is_not_bound", copy.deepcopy(prefix)+[loss(t=20)], left=1, right=20)
    add("drops_uncertified_close", copy.deepcopy(prefix)+[trans(10,"A",None,7),loss()])
    add("recovery", copy.deepcopy(prefix)+[loss(),attach(16,counters=ring),trans(18),frontier(24,ring)],left=16,right=24)
    add("recovery_early_attach", copy.deepcopy(prefix)+[loss(),attach(11,counters=ring),frontier(24,ring)])
    add("recovery_at_void", copy.deepcopy(prefix)+[loss(),attach(12,counters=ring),trans(18),frontier(24,ring)],left=12,right=24)
    add("multiple_void_latest", copy.deepcopy(prefix)+[loss(),loss(t=16),attach(14,counters=ring),frontier(24,ring)])
    add("before_first_frontier", [life(),attach(),trans(3),loss(),frontier(24,ring)])
    add("pending_attach_dropped", [attach(),loss(),life(t=14),frontier(24,ring)])
    add("pending_END_dropped", copy.deepcopy(prefix)+[dict(kind="END",t=20,pid=101,fork=0,reason="stop"),loss(),frontier(24,ring)])
    # A pre-loss transition whose timestamp is in a later run cannot be
    # resurrected after recovery, even if it looks like its anchor dependency.
    add("old_transition_dropped", copy.deepcopy(prefix)+[trans(16,None,"B"),loss(),attach(16,counters=ring,state="B",method="transition"),frontier(24,ring)])
    add("other_backend", [life(),life(pid=202),attach(),attach(pid=202),trans(3),trans(4,new="B",pid=202),frontier(8),loss(slot=4),frontier(24,loss(slot=4)["counters"])])
    add("key_includes_pid", [life(),life(pid=202,exit=0),attach(),trans(3),frontier(8)])
    add("assignment_includes_pid", [life(),life(pid=202),attach(),trans(3),frontier(8)])
    add("blocked_PID_is_local", copy.deepcopy(prefix[:3])+[trans(5,pid=202),frontier(8)])
    # All components compare individually, and each lifecycle-sensitive slot
    # must taint old identities; other slots permit reattachment to them.
    for slot in range(15):
        v=loss(slot=slot)["counters"]
        add(f"slot_{slot}", copy.deepcopy(prefix)+[loss(slot=slot),attach(16,counters=v),trans(18),frontier(24,v)],left=16,right=24)
        add(f"mismatch_frontier_{slot}", copy.deepcopy(prefix)+[frontier(10,v),frontier(24,v)])
        add(f"mismatch_attach_{slot}", copy.deepcopy(prefix)+[attach(12,counters=v),frontier(24)])
    add("counter_component_not_sum", copy.deepcopy(prefix)+[loss(slot=4), loss(t=14,slot=11),attach(16,counters=ring),trans(18),frontier(24,ring)],left=16,right=24)
    add("nonzero_baseline", [life(),attach(counters=ring),trans(3),frontier(8,ring)])["files"][0]["loss_baseline"]=ring[:]
    add("unknown_baseline", [life(),attach(),frontier(8)])["files"][0]["loss_baseline"]=None
    add("unreadable_taints", copy.deepcopy(prefix)+[loss(unknown=True),loss(t=14),attach(16,counters=ring),trans(18),frontier(24,ring)])
    add("unknown_baseline_taints_new", [loss(unknown=True),loss(t=14),life(t=16,fork=13),attach(17,fork=13,counters=ring),trans(18),frontier(24,ring)],fork=13)
    add("fresh_LIFE_while_unknown", [loss(unknown=True),life(t=13,fork=13),loss(t=14),attach(16,fork=13,counters=ring),trans(18),frontier(24,ring)],fork=13)
    add("LIFE_update_ignored", copy.deepcopy(prefix)+[loss(slot=0),life(t=20,exit=20)])
    add("LIFE_contradiction_ignored", copy.deepcopy(prefix)+[loss(slot=0),life(t=20,fork=None,start=5)],fork=None)
    for fork in (0,None,11,12,13):
        add("fresh_fork_"+str(fork),copy.deepcopy(prefix)+[loss(slot=0),life(t=14,fork=fork),attach(16,fork=fork,counters=loss(slot=0)["counters"]),trans(18),frontier(24,loss(slot=0)["counters"])],fork=fork,left=16,right=24)
    ident=loss(slot=0)["counters"]
    add("fresh_LIFE_before_VOID",[life(),life(t=12,fork=12),loss(slot=0),attach(16,fork=12,counters=ident),trans(18),frontier(24,ident)],fork=12)
    add("fresh_attach_before_LIFE", copy.deepcopy(prefix)+[loss(slot=0),attach(16,fork=14,counters=ident),life(t=14,fork=14),trans(18),frontier(24,ident)],fork=14)
    add("fresh_LIFE_then_attach", copy.deepcopy(prefix)+[loss(slot=0),life(t=14,fork=14),attach(16,fork=14,counters=ident),trans(18),frontier(24,ident)],fork=14,left=16,right=24)
    add("fresh_fork_at_uncertainty", copy.deepcopy(prefix)+[loss(slot=0),life(t=12,fork=12),attach(16,fork=12,counters=ident),trans(18),frontier(24,ident)],fork=12,left=16,right=24)
    add("latest_identity_VOID",copy.deepcopy(prefix)+[loss(slot=0),loss(t=18,slot=0,value=2),life(t=19,fork=14),attach(20,fork=14,counters=loss(slot=0,value=2)["counters"]),trans(22),frontier(28,loss(slot=0,value=2)["counters"])],fork=14)
    # Restoration from an unreadable vector is itself lifecycle uncertainty.
    add("unknown_preceding_baseline",[loss(unknown=True),loss(t=20),life(t=21,fork=14),attach(22,fork=14,counters=ring),trans(23),frontier(28,ring)],fork=14)
    add("fresh_fork_does_not_infer_old_exit",copy.deepcopy(prefix)+[loss(slot=0),life(t=14,fork=14),attach(16,fork=14,counters=ident),trans(18),frontier(24,ident)])
    # Revocation applies across old epochs and ended runs; no r-1 frontier.
    for invalid in (0,1,8,9,10,11,30):
        add(f"revoke_{invalid}",copy.deepcopy(prefix)+[frontier(10),loss(t=20,invalid=invalid,value=0)],right=10)
    add("revocation_taints",copy.deepcopy(prefix)+[loss(invalid=30,value=0),attach(16),trans(18),frontier(24)],left=16,right=24)
    add("revoked_run_never_restored",copy.deepcopy(prefix)+[loss(invalid=8,value=0),frontier(24)])
    add("H_survives_rollback",copy.deepcopy(prefix)+[frontier(10),loss(invalid=1,value=0),frontier(9)])
    add("H_survives_plain_VOID",copy.deepcopy(prefix)+[loss(),frontier(8,ring)])
    add("revoke_ended_by_certificate", [life(),attach(),dict(kind="END",t=5,pid=101,fork=0,reason="stop"),frontier(10),loss(t=20,invalid=8,value=0)])
    add("revoke_previous_epoch",copy.deepcopy(prefix)+[loss(),attach(16,counters=ring),trans(18),frontier(24,ring),loss(t=30,invalid=8,counters=ring)],right=8)
    add("revoke_previous_epoch_retains_earlier",copy.deepcopy(prefix)+[frontier(10),loss(),attach(16,counters=ring),frontier(24,ring),loss(t=30,invalid=9,counters=ring)],right=8)
    add("revoke_END_reconstructs_open",[life(),attach(),trans(3),frontier(8),trans(10,"A",None,7),dict(kind="END",t=11,pid=101,fork=0,reason="stop"),frontier(14),loss(t=20,invalid=10,value=0)],right=8)
    add("exit_fallback_after_rollback",copy.deepcopy(prefix)+[frontier(10),life(t=12,exit=12),frontier(16),loss(t=20,invalid=10,value=0)],right=14)
    add("exit_before_VOID_retained",copy.deepcopy(prefix)+[life(t=12,exit=12),loss(slot=0)])
    add("unqualified_VOID_taints_LIFE",[life(),loss(slot=0),life(t=20,exit=20)])["files"][0]["qualified"]=False
    add("unqualified_fresh_LIFE",[life(),loss(slot=0),life(t=14,fork=14)],fork=14)["files"][0]["qualified"]=False
    add("tainted_generation_no_longer_competes",copy.deepcopy(prefix)+[loss(slot=0),life(t=14,fork=14),attach(16,fork=14,counters=ident),trans(18),frontier(24,ident)],fork=14,left=16,right=24)
    add("malformed_attach_counters",[life(),dict(attach(),counters=None),frontier(8)])
    # VOID detection time can precede H; the contract orders records physically,
    # and requires only recovery a>=VOID.t. Thus surviving frozen and new runs
    # can have starts/endpoints in either order, disproving U4's tie proofs.
    add("tie_start",[life(),attach(10),frontier(20),loss(t=2),attach(3,counters=ring),frontier(30,ring)],left=40,right=50)
    add("tie_end",[life(),attach(3),frontier(20),loss(t=2),attach(3,counters=ring),dict(kind="END",t=8,pid=101,fork=0,reason="stop"),frontier(30,ring)],left=40,right=50)
    add("fallback_before_exit_after_end",[life(t=10,exit=10),attach(),dict(kind="END",t=5,pid=101,fork=0,reason="stop"),frontier(20)],right=5)
    add("unknown_taint_before_restore",copy.deepcopy(prefix)+[loss(unknown=True),life(t=20,exit=20)])
    add("unknown_baseline_taint_before_restore",[life(),loss(value=0),life(t=20,exit=20)])["files"][0]["loss_baseline"]=None
    add("unqualified_certificate_cannot_taint_LIFE",[life(),frontier(8,ident),life(t=20,exit=20)])["files"][0]["qualified"]=False
    add("recovery_END",copy.deepcopy(prefix)+[loss(),attach(16,counters=ring),trans(18),dict(kind="END",t=20,pid=101,fork=0,reason="stop"),frontier(24,ring)],left=16,right=20)
    add("frozen_touching_boundary",[life(),attach(10),frontier(20),loss(t=2),attach(3,counters=ring),trans(10),dict(kind="END",t=10,pid=101,fork=0,reason="stop"),frontier(30,ring)],left=3,right=10)
    add("tainted_pending_END",copy.deepcopy(prefix)+[loss(slot=0),dict(kind="END",t=20,pid=101,fork=0,reason="stop"),frontier(24,ident)])
    add("fresh_LIFE_order_is_first",[loss(slot=0),life(t=14,fork=14),attach(16,fork=14,counters=ident),life(t=20,fork=14),trans(18),frontier(24,ident)],fork=14,left=16,right=24)
    add("pending_attach_tainted_before_LIFE",[attach(16,fork=14),loss(slot=0),life(t=14,fork=14),frontier(24,ident)],fork=14)
    add("pending_END_tainted_before_LIFE",[dict(kind="END",t=16,pid=101,fork=14,reason="stop"),loss(slot=0),life(t=14,fork=14),frontier(24,ident)],fork=14)
    add("END_earlier_than_VOID",copy.deepcopy(prefix)+[loss(),attach(16,counters=ring),dict(kind="END",t=11,pid=101,fork=0,reason="stop"),frontier(24,ring)])
    add("tainted_future_fork_LIFE_update",[life(t=14,fork=14),loss(slot=0),life(t=20,fork=14,exit=20)],fork=14)
    add("fresh_provisional_after_zero_VOID",[loss(t=0,slot=0),life(t=1,fork=None),attach(2,fork=None,counters=ident),trans(3),frontier(8,ident)],fork=None)
    text_loss=loss(slot=4)["counters"]
    add("equal_total_counter_change",[life(),attach(counters=text_loss),trans(3),frontier(8,text_loss),frontier(10,ring),frontier(24,ring)])["files"][0]["loss_baseline"]=text_loss
    return cases


def generate(base, count=2400, seed=0x868005):
    """Random multi-epoch traces, with valid and deliberately ambiguous suffixes.

    Perturb append order, anchors, END, lifecycle recovery, vectors and rollback
    independently. Include high uint64 timestamps and queries outside loss times.
    """
    rng=random.Random(seed)
    cases=[]
    for i in range(count):
        pid=rng.choice([101,202])
        fork=rng.choice([0,0,None])
        target_fork=fork
        baseline=[rng.randrange(3) for _ in range(15)]
        initial=baseline[:]
        recs=[life(pid=pid,fork=fork)]
        t=1
        highest=0
        for epoch in range(rng.randint(1,5)):
            if epoch:
                t+=rng.randint(1,3)
                invalid=rng.choice([None,None,None,0,rng.randrange(t+1)])
                slot=rng.randrange(15)
                unknown=rng.randrange(8)==0
                if baseline is None: baseline=[0]*15
                vec=baseline[:]
                # MAX+1 deliberately probes malformed counter-vector refusal.
                vec[slot]=rng.choice([0,vec[slot]+1,MAX])
                recs.append(loss(t,slot,vec[slot],invalid,unknown,vec))
                baseline=None if unknown else vec
                if rng.randrange(4)==0:
                    # A readable restoration must still taint a fresh old fork.
                    t+=rng.randint(1,3)
                    baseline=vec[:]
                    recs.append(loss(t,slot,vec[slot],counters=vec))
                if rng.random()<0.55:
                    oldfork=fork
                    fork=rng.choice([None,oldfork,max(0,t-1),t,t+1])
                    t=max(t,fork or 0)
                    recs.append(life(t,pid,fork))
                    if rng.random()<0.6: target_fork=fork
            t+=rng.randrange(1,4)
            vec=baseline[:] if baseline is not None else [0]*15
            a=t
            current=rng.choice([None,"A","B"])
            method=rng.choice(["reread","transition"])
            at=attach(a,pid,fork,vec,current,method)
            if epoch and rng.random()<0.15: at["t"]=max(0,t-4)
            recs.append(at)
            entry=None
            if method=="transition":
                recs.append(trans(a,"untrusted" if current is None else None,current,pid=pid))
                entry=a if current else None
            for j in range(rng.randrange(6)):
                t+=rng.randrange(1,4)
                new=rng.choice([x for x in [None,"A","B"] if x!=current])
                recs.append(trans(t,current,new,t-entry if entry is not None else 0,pid))
                current,entry=new,t if new else None
                if rng.random()<0.4:
                    ft=max(highest+1,t)
                    recs.append(frontier(ft,vec)); highest=ft
            if rng.random()<0.5:
                recs.append(dict(kind="END",t=t+1,pid=pid,fork=fork,reason="stop"))
                t+=1
            if rng.random()<0.85:
                ft=max(highest+1,t+rng.randrange(3))
                recs.append(frontier(ft,vec));highest=ft
            t=max(t,highest)
        # Some late LIFE and transitions follow the final frontier. They must
        # not silently extend an old snapshot or compete after lifecycle taint.
        if rng.randrange(6)==0:
            recs.append(life(t+2,pid,fork,exit=t+2))
        if rng.randrange(7)==0:
            recs.append(trans(t+1,current,None if current else "A",0,pid))
        if rng.randrange(9)==0:
            other=303
            recs.insert(0,life(pid=other))
            recs.insert(1,attach(pid=other))
            recs.insert(2,trans(2,new="B",pid=other))
        if rng.randrange(12)==0:
            # Nonadvancing publications remain invalid across VOID.
            recs.append(frontier(max(0,highest-1),vec))
        left,right=sorted(rng.choices(range(t+8),k=2))
        c=scenario(base,f"void_seed_{seed:x}_{i:04d}",recs,left,right,target_fork,pid)
        c["files"][0]["loss_baseline"]=initial
        if rng.randrange(10)==0: c["files"][0]["qualified"]=False
        if rng.randrange(20)==0: c["query"].pop("event")
        offset=rng.choice([0,1,(1<<53)+7,1<<63,MAX-300])
        for rec in recs:
            for field in ("t","fork","start","exit","invalid_from"):
                if rec.get(field) is not None: rec[field]+=offset
        if c["target"]["fork"] is not None: c["target"]["fork"]+=offset
        for field in ("from","to"): c["query"][field]+=offset
        # scenario() copies its inputs; install the shifted records explicitly.
        c["files"][0]["records"]=recs
        cases.append(c)
    return cases
