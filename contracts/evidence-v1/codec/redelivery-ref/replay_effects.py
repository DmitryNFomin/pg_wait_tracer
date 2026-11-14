#!/usr/bin/env python3
"""Bounded replay evaluator for LIFE, nonwaiting reread runs, FRONTIER and VOID.

Unsupported named-wait/transition-run semantics raise ReplayScopeError, never a
fabricated PASS. Descriptor checks and all r3b table effects are implemented.
"""

ZERO=[0]*15
INITIAL=object()
LIFECYCLE=(0,1,2,10,12,14)

class ReplayScopeError(ValueError):
    """An explicitly unsupported semantic domain, not a contract ambiguity."""

def replay_probe(records, qualified=True, baseline=INITIAL, descriptors=None):
    # Clause 19: unknown null is distinct from known zero, including at file 0.
    baseline=ZERO[:] if baseline is INITIAL else baseline
    lives={}; runs=[]; frontiers=[]; revoked=[]; H=None; last_void=None
    uncertainty=None; eligible=set(); invalid_lives=set(); stop=None; retained=[]; semantic=[]
    events=[]
    if descriptors is None:
        events=[('record',item) for item in records]
    else:
        for desc in descriptors:
            events.append(('descriptor',desc))
            events.extend(('record',r) for r in records if r['physical'][:3]==desc['file'])
    def fault(item,reason):
        return dict(file=item['physical'][:3],position=item['physical'][-2:],reason=reason)
    def life_value(r): return [r['start'],r['exit']]
    for index,(etype,item) in enumerate(events):
        if etype=='descriptor':
            if item['file'][2]==0:
                baseline=item['baseline']; qualified=item['qualified']
            # REDELIVERY 19 / CONTRACT §1: check, never reset, running state.
            elif item['baseline']!=baseline:
                stop=dict(file=item['file'],position=[0,None],reason='baseline_mismatch',
                          expected=baseline,declared=item['baseline']); break
            continue
        r=item['value']; kind=r['kind']; t=r['t']; key=(r.get('pid'),r.get('fork'))
        if kind=='LIFE':
            # CONTRACT §§2,4: retain old verified LIFE through loss; new identity
            # after uncertainty requires an appended fresh fork/readable baseline.
            if uncertainty is not None and key not in lives:
                if baseline is None or not r['start_known'] or r['fork'] is None or r['fork']<uncertainty:
                    retained.append(item); continue
            if key in lives:
                old=lives[key]
                if old['start']!=r['start'] or old['start_known']!=r['start_known'] or (old['exit'] is not None and old['exit']!=r['exit']):
                    invalid_lives.add(key)
                elif old['exit'] is None: lives[key]=r
            else: lives[key]=r
            if uncertainty is None or (baseline is not None and r['start_known'] and r['fork'] is not None and r['fork']>=uncertainty): eligible.add(key)
            retained.append(item); continue
        # Clause 19: ignored certificates never change baseline, even when null.
        if not qualified and kind in ('ATTACH_OK','FRONTIER','END'):
            semantic.append(dict(file=item['physical'][:3],position=item['physical'][-2:],kind='ignored_certificate'))
            retained.append(item); continue
        if kind=='FRONTIER' and H is not None and t<=H:
            stop=fault(item,'nonadvancing_frontier'); break
        if kind in ('FRONTIER','ATTACH_OK'):
            counters=r['counters']
            if counters is None or baseline is None or counters!=baseline:
                if baseline is not None and counters is not None and any(a<b for a,b in zip(counters,baseline)):
                    counters=None
                r=dict(kind='VOID',t=t,counters=counters,invalid_from=None)
                kind='VOID'
                semantic.append(dict(file=item['physical'][:3],position=item['physical'][-2:],kind='counter_loss'))
        if kind=='VOID':
            cut=r['invalid_from']; counters=r['counters']
            # CONTRACT §4: compare lifecycle slots before replacing the baseline.
            if cut is not None or baseline is None or counters is None or any(counters[i]!=baseline[i] for i in LIFECYCLE):
                uncertainty=t; eligible.clear()
            if cut is not None:
                revoked.extend(f for f in frontiers if f>=cut)
                frontiers=[f for f in frontiers if f<cut]
            for run in runs:
                valid=[f for f in frontiers if f>=run['anchor'] and (run['end'] is None or f<=run['end'])]
                run['end']=max(valid) if valid else -1; run['frozen']=True
            baseline=counters; last_void=t
        elif kind=='ATTACH_OK':
            if r['method']!='reread' or r['state'] is not None or r.get('seed') is not None:
                raise ReplayScopeError('REDELIVERY clause 19 / CONTRACT §§3–5: named/transition attachment outside bounded replay domain')
            if last_void is not None and t<last_void:
                stop=fault(item,'recovery_before_void'); break
            # §§1,3: a delayed LIFE through the next FRONTIER (or EOF) can resolve
            # an attachment; physical delay itself is never a semantic stop.
            life=lives.get(key)
            if life is None:
                for later_type,later in events[index+1:]:
                    if later_type!='record': continue
                    lr=later['value']
                    if lr['kind']=='LIFE' and (lr.get('pid'),lr.get('fork'))==key:
                        life=lr; break
                    if lr['kind']=='FRONTIER': break
            if uncertainty is not None and key not in eligible:
                retained.append(item); continue
            if key in invalid_lives or life is None:
                # Missing/contradictory identity withholds certification (§5).
                retained.append(item); continue
            if any(run['key']==key and not run['frozen'] and run.get('ended') is None for run in runs):
                stop=fault(item,'overlapping_attachment'); break
            if t<life['start'] or (life['exit'] is not None and t>=life['exit']):
                stop=fault(item,'attachment_outside_life'); break
            runs.append(dict(key=key,anchor=t,end=None,frozen=False,ended=None))
        elif kind=='FRONTIER':
            frontiers.append(t); H=t
        elif kind in ('SAMPLE','MARKER'):
            pass # CONTRACT §§1–2: these observations carry no exact replay effect.
        elif kind in ('TRANS','END'):
            # The existing M fixtures have no addressable exact runs. Do not
            # pretend to implement timestamp/run resolution in the general case.
            if kind=='TRANS' and not any(run['key'][0]==r['pid'] for run in runs):
                retained.append(item); continue
            if kind=='END' and uncertainty is not None and key not in eligible:
                retained.append(item); continue
            raise ReplayScopeError(f'REDELIVERY clause 19 / CONTRACT §§3–5: {kind} run dependencies outside bounded replay domain')
        else:
            raise ReplayScopeError(f'REDELIVERY clause 19 / CONTRACT §5: unsupported replay shape {kind!r}')
        retained.append(item)
    coverage=[]
    for run in runs:
        if run['key'] not in lives or run['key'] in invalid_lives: continue
        life=lives[run['key']]
        if life['exit'] is not None:
            raise ReplayScopeError('REDELIVERY clause 19 / CONTRACT §5: certified exit projection outside bounded replay domain')
        valid=[f for f in frontiers if f>=run['anchor'] and (run['end'] is None or f<=run['end'])]
        if valid: coverage.append([run['anchor'],max(valid)])
    life_entries=[dict(pid=k[0],fork=k[1],life=None if k in invalid_lives else life_value(v)) for k,v in sorted(lives.items(),key=lambda kv:(kv[0][0],-1 if kv[0][1] is None else kv[0][1]))]
    projections=[]
    for entry in life_entries:
        k=(entry['pid'],entry['fork']); spans=[]
        if entry['life'] is not None:
            for run in runs:
                if run['key']!=k: continue
                valid=[f for f in frontiers if f>=run['anchor'] and (run['end'] is None or f<=run['end'])]
                if valid: spans.append([run['anchor'],max(valid)])
        projections.append(dict(**entry,coverage=spans,waits=[],open=None))
    return dict(frontier=frontiers[-1] if frontiers else None,projections=projections,frontiers=frontiers,revoked=sorted(set(revoked)),coverage=coverage,high_water=H,
                life=[x['life'] for x in life_entries],life_entries=life_entries,baseline=baseline,
                lifecycle_uncertainty=uncertainty,stop=stop,records=retained,semantic_diagnostics=semantic)
