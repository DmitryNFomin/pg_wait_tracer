#!/usr/bin/env python3
"""Independent, abstract evidence-v1 redelivery evaluator (standard library only).

No fixture aliases or product codec/replay code enter this module.
"""
import argparse
import hashlib
import itertools
import json
import sys
from collections import defaultdict

REASONS = ('HC', 'BC', 'SC', 'PU', 'CRC', 'T', 'IH', 'IB', 'RF')
RANK = {r:i for i,r in enumerate(REASONS)}
CERTIFICATES = ('ATTACH_OK', 'FRONTIER', 'END')

class AmbiguityError(ValueError):
    """A normative choice is missing, rather than a byte validation failure."""
    def __init__(self, clause, message):
        super().__init__(f'REDELIVERY clause {clause}: {message}')

class ModelError(ValueError):
    pass

def canonical(x):
    return json.dumps(x, sort_keys=True, separators=(',', ':'), ensure_ascii=False)

def pkey(p):
    # Clause 4: null precedes all decoded-payload offsets.
    return p[0], -1 if p[1] is None else p[1]

def terminal_reason(checks):
    """Clause 6 precedence, also used for rejected footer bytes."""
    if not checks.get('header_complete', True): return 'T'
    if not checks.get('header_fields_valid', True): return 'IB'
    if not checks.get('safely_bounded', True): return 'IB'
    if not checks.get('bytes_complete', True): return 'T'
    if not checks.get('crc_valid', True): return 'CRC'
    if not checks.get('decode_valid', True): return 'IB'
    return None

def wire_identity(d):
    """Clause 2, symbolic name of all bytes; this is NOT their real SHA-256.

    The fixture notation explicitly allows symbolic content-derived identifiers.
    An encoder replaces this symbolic digest with (length,SHA256(actual bytes)).
    Validation annotations and logical field names are not bytes.
    """
    h = d['header']
    raw_header = h.get('raw_content_id', h['content_id'])
    flag_octets=max(0,min(2,h.get('raw_length',h['H'])-6))
    present_flags=h['flags'] & ((1 << (8*flag_octets))-1)
    vector = [raw_header, present_flags,
              [[b['start'],b['end'],b['content_id']] for b in d['blocks']]]
    end = d['blocks'][-1]['end'] if d['blocks'] else h.get('raw_length', h['H'])
    length = end + d['tail']['length']
    if d['tail']['length']:
        vector[2].append([end,length,d['tail']['content_id']])
    return f'{length}:abstract-sha256:{hashlib.sha256(canonical(vector).encode()).hexdigest()}'

def _schema(d):
    try:
        h=d['header']; blocks=d['blocks']; tail=d['tail']
        if not isinstance(h['valid'],bool): raise ModelError('header.valid must be boolean')
        if not 0 <= h['flags'] <= 65535: raise ModelError('flags outside u16')
        if tail['kind'] not in ('eof','footer','rejected'): raise ModelError('unknown tail kind')
        if tail['kind']=='eof' and tail['length']!=0: raise ModelError('nonempty EOF')
        if h['valid']:
            boot,cap,no=h['physical']
            # Clauses 2,19 / CODEC §1: an asserted valid descriptor has exactly
            # fifteen unsigned counters or the explicitly unknown baseline.
            v=h['baseline']
            if v is not None and (not isinstance(v,list) or len(v)!=15 or any(type(x) is not int or not 0<=x<2**64 for x in v)):
                raise ModelError('REDELIVERY clause 19: header baseline must be null or fifteen u64s')
            if not isinstance(no,int) or no<0 or no>=2**64: raise ModelError('invalid verified ordinal')
            prev=h['previous']
            # CODEC §2: invalid ordinal headers never become trusted link identities.
            if (no==0 and prev is not None) or (no>0 and (prev is None or prev['file_no']!=no-1)):
                raise ModelError('CODEC §2: mark this invalid-ordinal header valid=false')
        pos=h['H']
        offsets=set()
        for b in blocks:
            if b['start']!=pos or b['end']<=pos: raise ModelError('raw units must be contiguous positive spans')
            pos=b['end']
            for r in b['records']:
                if r['offset']<0 or r['offset'] in offsets: # reset below, offsets are per block
                    raise ModelError('record offsets must be distinct nonnegative decoded offsets')
                offsets.add(r['offset'])
                if r.get('fault') is not None and 'kind' not in r['fault']: raise ModelError('fault kind required')
            if [r['offset'] for r in b['records']] != sorted(r['offset'] for r in b['records']):
                raise ModelError('records must be in original payload order')
            offsets.clear()
    except KeyError as e:
        raise ModelError(f'missing required abstract field: {e}') from e

def validate(d):
    """Clauses 2,5,6,7: scan one delivery without borrowing/resynchronizing."""
    h=d['header']
    state={'delivery':wire_identity(d),'raw':d,'file':None,'prefix':[],'E':None,
           'terminal':None,'faults':[]}
    if not h['valid']:
        state['terminal']={'position':[0,None],'reason':'IH','detail':h.get('failure','invalid header')}
        return state
    state['file']=tuple(h['physical']); state['E']=h['H']
    for b in d['blocks']:
        reason=terminal_reason(b.get('checks',{}))
        if reason:
            state['terminal']={'position':[state['E'],None],'reason':reason,'detail':b.get('checks',{})}
            return state
        if b['type'] not in (0,1,2) or b['codec'] not in (0,1) or (b['type']==2 and b['codec']!=0):
            state['terminal']={'position':[state['E'],None],'reason':'IB','detail':'unsupported type/codec'}
            return state
        state['prefix'].append(b); state['E']=b['end']
        for r in b['records']:
            fault=r.get('fault')
            if fault is None: continue
            # Clause 7 / CODEC §9: null counters are readable loss, not malformed.
            if fault['kind']=='null_counters': continue
            stop=not (not h['qualified'] and r['value'].get('kind') in CERTIFICATES)
            state['faults'].append({'position':[b['start'],r['offset']],'reason':'RF',
                                    'detail':fault,'stop':stop})
    tail=d['tail']
    if tail['kind']=='rejected':
        reason=terminal_reason(tail.get('checks',{}))
        if reason is None: raise ModelError('rejected tail needs at least one failing check')
        state['terminal']={'position':[state['E'],None],'reason':reason,'detail':tail.get('checks',{})}
    # Clause 6: kind=footer asserts exact reconstructed footer, not merely a footer-shaped suffix.
    return state

def prefix_digest(state,D):
    """Clauses 13,14 / CODEC §8; flags and footer are omitted."""
    blocks=state['prefix']
    return dict(kind='prefix',header=state['raw']['header']['content_id'],D=D,
                blocks=[[b['start'],b['end'],b['content_id']] for b in blocks if b['end']<=D],
                partial=next(([b['content_id'],D-b['start']] for b in blocks if b['start']<D<b['end']),None))

def link_check(s,p):
    """Clauses 13–15: local facts; group/capture cutoffs do not enter here."""
    sh=s['raw']['header']; ph=p['raw']['header']; prev=sh['previous']; D=prev['D']
    static = (sh['physical'][:2]==ph['physical'][:2] and prev['file_no']==ph['physical'][2]
              and sh['qualified']==ph['qualified'] # Clauses 13–14: baseline is replay state.
              and sh['mapping']==ph['mapping'])
    boundaries=[ph['H']]+[b['end'] for b in p['prefix']]
    boundary=D in boundaries
    # Digest comparisons at nonboundaries cannot establish either a witness or SC.
    digest=(prev['digest']==prefix_digest(p,D)) if boundary else None
    clean=p['terminal'] is None
    exact=p['E']==D
    extension=boundary and any(b['start']>=D for b in p['prefix'])
    return dict(predecessor=p['delivery'],validated_end=p['E'],clean=clean,
                static_match=static,boundary=boundary,digest_match=digest,exact_end=exact,
                extension=extension,witness=bool(static and boundary and digest and clean and exact),
                seal_proof=bool(static and boundary and digest and extension))

def evaluate(multiset):
    deliveries=multiset['deliveries'] if isinstance(multiset,dict) else multiset
    if not isinstance(deliveries,list): raise ModelError('deliveries must be an array')
    unique={}
    for d in deliveries:
        _schema(d); ident=wire_identity(d)
        if ident in unique:
            # An identity cannot carry contradictory independent validation facts.
            if canonical(unique[ident])!=canonical(d):
                raise ModelError('same byte identity has inconsistent model annotations')
        unique[ident]=d
    states=[validate(unique[k]) for k in sorted(unique)] # Clauses 1,2,20.
    groups=defaultdict(list)
    diagnostics=[]; candidates=[]
    def diag(file,pos,reason,witnesses,detail=None,eligible=False):
        entry=dict(file=list(file) if file is not None else None,position=pos,reason=reason,witnesses=witnesses)
        if detail is not None: entry['detail']=detail
        diagnostics.append(entry)
        if eligible: candidates.append({k:v for k,v in entry.items() if k!='detail'})
    for s in states:
        if s['file'] is not None: groups[s['file']].append(s)
        if s['terminal']:
            t=s['terminal']; diag(s['file'],t['position'],t['reason'],[s['delivery']],t['detail'])
        for f in s['faults']:
            diag(s['file'],f['position'],'RF',[s['delivery']],f['detail'],f['stop'])
    # Clause 2a: every independently validated header, even an unlinked non-root,
    # proves its boot. Ambiguity is capture-local, outside the closed codec reasons.
    capboots=defaultdict(set)
    for boot,cap,no in groups: capboots[cap].add(boot)
    ambiguities=[]
    for cap,boots in sorted(capboots.items()):
        if len(boots)<=1: continue
        witnesses=[dict(delivery=s['delivery'],boot=s['file'][0],file_no=s['file'][2],position=[0,None])
                   for s in states if s['file'] is not None and s['file'][1]==cap]
        ambiguities.append(dict(capture=cap,reason='CAPTURE_BOOT_AMBIGUITY',
                                boots=sorted(boots),witnesses=sorted(witnesses,key=lambda w:w['delivery'])))
    ambiguous={a['capture'] for a in ambiguities}
    conflict_groups=set(); sources={}
    for file,g in sorted(groups.items()):
        for x,y in itertools.combinations(g,2):
            if x['raw']['header']['content_id']!=y['raw']['header']['content_id']:
                # Clauses 8,16: no block comparison for this incompatible pair.
                conflict_groups.add(file)
                diag(file,[0,None],'HC',[[x['delivery'],y['delivery']]],eligible=True)
                continue
            # Clauses 9,16: complete stored bytes, every type; no rejected units.
            for a,b in zip(x['prefix'],y['prefix']):
                if a['content_id']!=b['content_id']:
                    if a['start']!=b['start']: raise ModelError('equal block identities imply equal spans')
                    diag(file,[a['start'],None],'BC',[[x['delivery'],y['delivery']]],eligible=True)
                    break
        E=max(s['E'] for s in g)
        maxcopies=[s for s in g if s['E']==E]
        # Clause 10: group with HC has no prefix-union source.
        sources[file]=[] if file in conflict_groups else [s['delivery'] for s in maxcopies]
        # Clause 12: only maximum validated prefixes, unless any has clean EOF/footer.
        if all(s['terminal'] is not None for s in maxcopies):
            reason=min((s['terminal']['reason'] for s in maxcopies),key=RANK.__getitem__)
            candidates.append(dict(file=list(file),position=[E,None],reason=reason,
                witnesses=[s['delivery'] for s in maxcopies if s['terminal']['reason']==reason]))
    links={}
    for s in states:
        if s['file'] is None: continue
        file=s['file']; h=s['raw']['header']
        if file[2]==0:
            links[s['delivery']]=dict(status='root',witnesses=[],seal_pairs=[],checks=[])
            continue
        prev=h['previous']; predfile=(file[0],file[1],file[2]-1) # Clause 13: own immediate group only.
        checks=[link_check(s,p) for p in groups.get(predfile,[])]
        witnesses=[c['predecessor'] for c in checks if c['witness']]
        pairs=[[c['predecessor'],s['delivery']] for c in checks if c['seal_proof']]
        if pairs:
            # Clauses 14–16: roles preserved, SC belongs to predecessor, suppress PU.
            status='seal_conflict'
            diag(predfile,[prev['D'],None],'SC',pairs,eligible=True)
        elif not witnesses:
            status='unavailable'
            # Clauses 15,20: preserve declared reference plus ALL alternative checks.
            diag(file,[0,None],'PU',[s['delivery']],
                 dict(declared_predecessor=dict(file=list(predfile),**prev),available=checks),True)
        else: status='witnessed'
        links[s['delivery']]=dict(status=status,witnesses=witnesses,seal_pairs=pairs,checks=checks)
    # Clause 17: normalize file candidates only; independent diagnostics survive.
    byfile=defaultdict(list)
    for c in candidates: byfile[tuple(c['file'])].append(c)
    stops=[]
    for file,cs in sorted(byfile.items()):
        key=min((pkey(c['position']),RANK[c['reason']]) for c in cs)
        tied=[c for c in cs if (pkey(c['position']),RANK[c['reason']])==key]
        out=dict(tied[0]); out['witnesses']=_witness_union(w for c in tied for w in c['witnesses'])
        stops.append(out)
    cutoffs={}
    for stop in stops:
        f=stop['file']; k=(f[2],*pkey(stop['position'])); cap=tuple(f[:2])
        if cap not in cutoffs or k<cutoffs[cap]: cutoffs[cap]=k
    records=[]; admissions=[]
    for file,g in sorted(groups.items()):
        cap=file[:2]; cutoff=cutoffs.get(cap)
        # Clauses 15,18: walk immediate predecessors; local witness != admitted file.
        present=sorted(f[2] for f in groups if f[:2]==cap and f[2]<file[2])
        missing=[]; cursor=0
        for ordinal in present+[file[2]]:
            if cursor<ordinal: missing.append([cursor,ordinal])
            cursor=ordinal+1
        reachable=not missing and (cutoff is None or (file[2],0,-1)<cutoff)
        admitted=reachable and file not in conflict_groups and file[1] not in ambiguous
        srcids=sources[file]
        src=next((s for s in g if s['delivery'] in srcids),None)
        count=0
        if admitted and src:
            for b in src['prefix']:
                for r in b['records']:
                    key=(file[2],b['start'],r['offset'])
                    if cutoff is not None and key>=cutoff: continue
                    # Clause 11: omit valid samples/markers; preserve physical offsets.
                    fault=r.get('fault')
                    if fault and fault['kind']!='null_counters': continue
                    if b['type']==1 or r['value'].get('kind')=='MARKER': continue
                    records.append(dict(physical=list(file)+[b['start'],r['offset']],
                                        content_id=r['content_id'],value=r['value']))
                    count+=1
        decisions=[dict(delivery=s['delivery'],**links[s['delivery']]) for s in g]
        admissions.append(dict(file=list(file),admitted=admitted,sources=srcids,
                               missing_files=missing,record_count=count,links=decisions))
    diagnostics=_dedup_diagnostics(diagnostics)
    # Clause 20: an invalid-header-only multiset is a failed decode, not success.
    unassigned=any(s['file'] is None for s in states)
    # Clauses 0,19: separate delivery diagnostics, capture outcomes and aggregate.
    # Codec arrays/admission stay intact when a reachable replay check fails.
    from replay_effects import replay_probe, ReplayScopeError
    capture_results=[]; replay_stops=[]
    for cap,boots in sorted(capboots.items()):
        cstops=[s for s in stops if s['file'][1]==cap]
        ambiguity=next((a for a in ambiguities if a['capture']==cap),None)
        replay=None
        if ambiguity is None:
            descriptors=[]
            for a in admissions:
                if a['file'][1]!=cap or not a['admitted']: continue
                source=next(st for st in groups[tuple(a['file'])] if st['delivery'] in a['sources'])
                h=source['raw']['header']
                descriptors.append(dict(file=a['file'],baseline=h['baseline'],qualified=h['qualified']))
            if descriptors:
                try:
                    replay=replay_probe([r for r in records if r['physical'][1]==cap],descriptors=descriptors)
                except ReplayScopeError as exc:
                    raise ModelError(str(exc)) from exc
                if replay['stop'] is not None: replay_stops.append(replay['stop'])
        failed=bool(cstops) or ambiguity is not None or (replay is not None and replay['stop'] is not None)
        capture_results.append(dict(capture=cap,boots=sorted(boots),status='FAILED' if failed else 'PASS',
                                    ambiguity=ambiguity,codec_stops=cstops,replay=replay))
    for d in diagnostics: d['failure_level']='delivery'
    aggregate_status='FAILED' if unassigned or any(c['status']=='FAILED' for c in capture_results) else 'PASS'
    return dict(records=records,stops=stops,diagnostics=diagnostics,admissions=admissions,
                ambiguities=ambiguities,capture_results=capture_results,replay_stops=replay_stops,
                aggregate_status=aggregate_status,
                unassigned_header_failure=unassigned,decode_failed=aggregate_status=='FAILED')

class Reconciler:
    """Clause 1: accumulated multiset, deliberately recomputed without stale caches."""
    def __init__(self):
        self.deliveries=[]

    def add(self, delivery):
        # Own the snapshot: caller mutation must not change already delivered bytes.
        self.deliveries.append(json.loads(canonical(delivery)))
        return self.result()

    def result(self):
        return evaluate(self.deliveries)


def _witness_union(witnesses):
    return [json.loads(x) for x in sorted(set(canonical(w) for w in witnesses))]

def _dedup_diagnostics(ds):
    # Clauses 16,20: pair sets are grouped by file/position/reason; detailed single-
    # delivery failures remain independent (including every suppressed terminal).
    groups={}
    for d in ds:
        key=canonical({k:v for k,v in d.items() if k!='witnesses'})
        if key not in groups: groups[key]=dict(d,witnesses=[])
        groups[key]['witnesses']+=d['witnesses']
    for d in groups.values(): d['witnesses']=_witness_union(d['witnesses'])
    return sorted(groups.values(),key=lambda d:(tuple(d['file'] or ['', '', -1]),pkey(d['position']),RANK[d['reason']],canonical(d)))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',nargs='?',help='JSON delivery multiset; default stdin')
    args=parser.parse_args()
    try:
        if args.input:
            with open(args.input) as f: model=json.load(f)
        else: model=json.load(sys.stdin)
        print(json.dumps(evaluate(model),indent=2,ensure_ascii=False))
    except (ModelError,AmbiguityError) as e:
        print(json.dumps({'error':type(e).__name__,'message':str(e)}),file=sys.stderr)
        return 2
    return 0

if __name__=='__main__': sys.exit(main())
