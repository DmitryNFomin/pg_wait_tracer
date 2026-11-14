#!/usr/bin/env python3
"""Bounded abstract multiset generator and independent local assertions.

Exhaustive multiset sizes 0..4 within each declared thematic alphabet, plus
all byte cuts of the raw A/Ao artifact. Every UNIQUE permutation is checked.
No random seed, product parser, concrete compression implementation, or network.
"""
import copy
import itertools
import json
import time
from collections import Counter
from pathlib import Path
from ref_redelivery import evaluate, wire_identity, terminal_reason, canonical, Reconciler
from replay_effects import replay_probe

ROOT=Path(__file__).resolve().parent

def altered(d,tag,block=None,**fields):
    d=copy.deepcopy(d)
    if block is None:
        d['header'].update(fields); d['header']['content_id']+=':'+tag
    else:
        d['blocks'][block].update(fields); d['blocks'][block]['content_id']+=':'+tag
    return d

def records_set(result):
    return {canonical(r) for r in result['records']}

def conflict_set(result):
    return {canonical([d['file'],d['position'],d['reason'],d['witnesses']]) for d in result['diagnostics'] if d['reason'] in ('HC','BC','SC')}

def run():
    started=time.monotonic(); db=json.loads((ROOT/'cases.json').read_text()); D=db['deliveries']
    counts=Counter(); failures=[]; seen=set(); coverage=set(); direct=[]
    def check(ok,label,**details):
        counts['assertions']+=1
        if not ok: failures.append(dict(assertion=label,**details))
    def exercise(ds,tag):
        ds=list(ds)
        ids=tuple(sorted(wire_identity(d) for d in ds))
        if ids in seen: return
        seen.add(ids); coverage.add(tag); counts['multisets']+=1; counts[f'size_{len(ds)}']+=1
        expected=evaluate(ds)
        lookup={wire_identity(d):d for d in ds}
        for permutation in sorted(set(itertools.permutations(ids))):
            counts['permutations']+=1
            result=evaluate([lookup[i] for i in permutation])
            check(result==expected,'permutation',multiset=ids,order=permutation)
            accumulated=Reconciler()
            for k in range(len(permutation)+1):
                updated=accumulated.result() if k==0 else accumulated.add(lookup[permutation[k-1]])
                fresh=evaluate([lookup[i] for i in permutation[:k]])
                counts['permutation_prefix_checks']+=1
                check(updated==fresh,'every permutation accumulated prefix',prefix=permutation[:k])
        # Every edge from a submultiset of size n-1 to this size-n multiset.
        # This includes duplicates and successor-before-predecessor accumulation.
        for ident in sorted(set(ids)):
            shorter=list(ids); shorter.remove(ident)
            previous=evaluate([lookup[i] for i in shorter]); counts['incremental_edges']+=1
            # A fresh accumulated evaluation must equal whole-multiset evaluation.
            accumulated=Reconciler()
            for i in shorter: accumulated.add(lookup[i])
            result=accumulated.add(lookup[ident])
            check(result==expected,'incremental equality',multiset=ids)
            lost=records_set(previous)-records_set(result)
            added=conflict_set(result)-conflict_set(previous)
            counts['monotonicity_edges']+=1
            newly_ambiguous={a['capture'] for a in result['ambiguities']}-{a['capture'] for a in previous['ambiguities']}
            # Exact 2a exception: only positions of a capture newly proved to
            # have >1 validated boot may disappear wholesale. Unrelated captures
            # still require an actual conflict at/before each removed position.
            newconflicts=[json.loads(c) for c in added]
            justified=lambda r: r['physical'][1] in newly_ambiguous or any(
                f[:2]==r['physical'][:2] and (f[2],pos[0],-1 if pos[1] is None else pos[1])<=tuple(r['physical'][2:])
                for f,pos,reason,ws in newconflicts)
            check(all(justified(json.loads(r)) for r in lost),'storage monotonicity',multiset=ids,lost=list(lost),added_conflict=list(added))
            if newly_ambiguous:
                counts['ambiguity_retraction_edges']+=1
                check(all(r['physical'][1] not in newly_ambiguous for r in result['records']), '2a entire capture retracted')
                check(all(c['replay'] is None for c in result['capture_results'] if c['capture'] in newly_ambiguous),'2a replay results retracted')
            if ident in shorter:
                counts['duplicate_edges']+=1
                check(result==previous,'duplicate idempotence',multiset=ids)
        return expected
    # Each alphabet is small enough that all multisets AND all permutations can be enumerated.
    pools=[('prefix_and_clean_ties',['A','At1','At3','Ag','Ag′','Acrc3']),
           ('header_flags_footer',['A','Af','Ao','Aft','Ah','Z']),
           ('conflict_both_vs_one_valid',['A','Ac2t1','Ac2t3','Au','Auc3']),
           ('sample_block_conflicts',['M','Mt3','Mc1t3']),
           ('chain_and_seal',['A','A+','N','L','At3','Ag']),
           ('bad_and_nonboundary_links',['A','A+','N','Nb','Ni']),
           ('reversed_chain_interleaved_captures',['A','N','L','X']),
           ('fault_before_later_conflict',['Au','Auc3','R','Rc3','A']),
           ('second_boot_and_nonroot_reuse',['A','A2','N2','Z2','X','A2t0']),
           ('running_void_baseline',['A','V','Q','Q0','N1']),
           ('unknown_baselines',['A','Vu','Tu','T0']),
           ('frontier_moves_baseline',['A','W','Qw','Qw0']),
           ('unqualified_ignored_baseline',['Wi','Qwi0','Qwi1']),
           ('unqualified_explicit_void',['Vi','Qvi'])]
    for tag,names in pools:
        for size in range(5):
            for choice in itertools.combinations_with_replacement(names,size): exercise([D[n] for n in choice],tag)
    # Independently authenticated seal witnesses with flags and multiple successor claims.
    plusflag=copy.deepcopy(D['A+']); plusflag['header']['flags']=1
    nextflag=copy.deepcopy(D['N']); nextflag['header']['flags']=1
    shortclaim=copy.deepcopy(D['N'])
    shortclaim['header']['previous'].update(D=747,digest=dict(kind='prefix',header=D['A']['header']['content_id'],D=747,
        blocks=[[b['start'],b['end'],b['content_id']] for b in D['A']['blocks'][:3]],partial=None))
    shortclaim['header']['content_id']+=':claim-747'
    for b in shortclaim['blocks']: b['content_id']+=':claim-747-chain'
    alphabet=[D['A+'],plusflag,D['N'],nextflag,shortclaim]
    for size in range(5):
        for choice in itertools.combinations_with_replacement(alphabet,size): exercise(choice,'multiple_seal_claims_and_witness_ties')
    out=evaluate([D['A+'],D['N'],shortclaim])
    check(out['stops'][0]['position']==[747,None] and out['stops'][0]['reason']=='SC','earliest authenticated seal claim')
    check({tuple(d['position']) for d in out['diagnostics'] if d['reason']=='SC'}=={(747,None),(957,None)},'all independent seal claims diagnosed')
    # Independent cuts at EVERY byte through header, each block header/payload/CRC, and footer.
    A=D['A']; Ao=D['Ao']; H=A['header']['H']; total=957+68
    for cut in range(total+1):
        d=copy.deepcopy(A)
        if cut<H:
            d['header'].update(valid=False,raw_length=cut,raw_content_id=f'A-header-prefix:{cut}')
            d['blocks']=[]; d['tail']=dict(kind='eof',length=0,content_id='')
            reason='IH'; end=0; expected_records=0; coverage.add('all_header_cuts')
        elif cut<=957:
            complete=[b for b in A['blocks'] if b['end']<=cut]; d['blocks']=copy.deepcopy(complete)
            end=complete[-1]['end'] if complete else H
            if end==cut:
                d['tail']=dict(kind='eof',length=0,content_id=''); reason=None
                coverage.add('clean_boundary_snapshots')
            else:
                n=cut-end
                d['tail']=dict(kind='rejected',length=n,content_id='00' if n==1 else f'A-unit-prefix:{end}:{n}',
                               checks=dict(header_complete=n>=64,bytes_complete=False))
                reason='T'
                coverage.add('all_block_header_payload_crc_cuts')
            expected_records=len(complete)
        else:
            n=cut-957; end=957; expected_records=4
            if n==68: d=copy.deepcopy(Ao); reason=None
            else:
                # In this fixture the first 64 footer bytes contain num_events=0.
                # >=64 bytes therefore fail bounds before the insufficiency check.
                reason='T' if n<64 else 'IB'
                d['tail']=dict(kind='rejected',length=n,content_id='00' if n==1 else 'footer-first17' if n==17 else f'A-footer-prefix:{n}',
                               checks=dict(header_complete=n>=64,header_fields_valid=False))
            coverage.add('all_footer_cuts')
        out=exercise([d],'byte_cut'); counts['byte_cuts']+=1
        if out is None: out=evaluate([d])
        check(len(out['records'])==expected_records,'cut prefix record count',cut=cut)
        observed=out['diagnostics'][0]['reason'] if out['diagnostics'] else None
        check(observed==reason,'cut classification',cut=cut,expected=reason,actual=observed)
        check((not out['stops'] if reason in (None,'IH') else out['stops'][0]['position']==[end,None]),'cut boundary',cut=cut)
        # Addition of a shorter/torn/bad artifact alongside A cannot remove its positions,
        # except the invalid-header diagnostic remains unassigned.
        exercise([A,d],'cut_with_clean_copy')
    # An entire footer of the right size is clean only when it matches exactly.
    badfooter=copy.deepcopy(D['Ao']); badfooter['tail'].update(kind='rejected',content_id='footer-first-timestamp-bit-flipped',
        checks=dict(header_complete=True,header_fields_valid=False))
    for size in range(1,5):
        for choice in itertools.combinations_with_replacement([A,badfooter,D['N']],size): exercise(choice,'complete_malformed_footer')
    out=evaluate([badfooter])
    check(out['stops'][0]['reason']=='IB' and out['stops'][0]['position']==[957,None],'malformed footer follows ordinary terminal classification')
    out=evaluate([badfooter,D['N']])
    check(out['admissions'][1]['links'][0]['status']=='unavailable','full-size bad footer is not a clean predecessor')
    schema=copy.deepcopy(A); schema['header'].update(valid=False,raw_content_id='A:unsupported-descriptor-schema')
    out=exercise([A,schema],'unsupported_header_schema')
    check(out['unassigned_header_failure'] and len(out['records'])==4,'unsupported header cannot claim identity')
    # Compound terminal defects: explicit text-derived precedence assertions.
    defects=[('short_header',dict(header_complete=False,header_fields_valid=False,bytes_complete=False,crc_valid=False,decode_valid=False),'T'),
             ('unsafe_bounds',dict(header_fields_valid=False,bytes_complete=False,crc_valid=False),'IB'),
             ('short_payload',dict(bytes_complete=False,crc_valid=False,decode_valid=False),'T'),
             ('crc_before_decode',dict(crc_valid=False,decode_valid=False),'CRC'),
             ('decode',dict(decode_valid=False),'IB')]
    variants=[]
    for tag,checks,wanted in defects:
        d=altered(A,tag,2,checks=checks); variants.append(d)
        out=exercise([d],tag)
        check(out['stops'][0]['reason']==wanted and out['stops'][0]['position']==[537,None],tag)
        check(len(out['records'])==2,'no resynchronization after bad unit',variant=tag)
        exercise([d,A],tag)
    # Multiple ties: all three reasons at maximum E; CRC wins T and IB.
    for combo in itertools.combinations_with_replacement(range(len(variants)),3): exercise([variants[i] for i in combo],'terminal_reason_ties')
    # All check variants are abstract independent validation outcomes, not purported byte-parser tests.
    validation_tags=['unsupported_payload_schema','unsupported_block_type','unsupported_codec','reserved',
                     'bad_self_offset','bad_preceding_crc','unsafe_count','bad_lz4_distance','bad_lz4_end',
                     'bad_leb128','bad_column_layout','unconsumed_suffix','bad_sample_weights','missing_common_t']
    for tag in validation_tags:
        checks={'header_fields_valid':False} if tag in ('unsupported_block_type','unsupported_codec','reserved','bad_self_offset','bad_preceding_crc','unsafe_count') else {'decode_valid':False}
        d=altered(D['M'],tag,1,checks=checks)
        out=exercise([d],tag)
        check(out['stops'][0]['reason']=='IB','whole-block validation failure',variant=tag)
        exercise([D['M'],d],tag)
    # Differing stored bytes/lengths, even when decoded records agree; compression differs.
    raw=D['M']; compressed=copy.deepcopy(raw)
    compressed['blocks'][0].update(codec=1,content_id='same-rows:valid-lz4',end=raw['blocks'][0]['end']+2)
    for b in compressed['blocks'][1:]: b['start']+=2; b['end']+=2; b['content_id']+=':offset-and-chain-repaired'
    out=exercise([raw,compressed],'raw_lz4_and_different_lengths')
    check(out['stops'][0]['position']==[201,None] and out['stops'][0]['reason']=='BC','stored bytes rather than decoded equality')
    # Empty predecessor and header-only successor, exact footer / dirty alternative.
    empty=copy.deepcopy(A); empty['blocks']=[]
    successor=copy.deepcopy(D['N']); successor['header']['previous'].update(D=H,digest=dict(kind='prefix',header=A['header']['content_id'],D=H,blocks=[],partial=None))
    successor['header']['content_id']+=':empty-predecessor'
    for b in successor['blocks']: b['content_id']+=':repaired-for-header'
    dirty=copy.deepcopy(empty); dirty['tail']=dict(kind='rejected',length=1,content_id='00',checks={'header_complete':False})
    for size in range(1,5):
        for choice in itertools.combinations_with_replacement([empty,dirty,successor,A],size): exercise(choice,'empty_predecessor_multiple_claims')
    out=evaluate([empty,successor]); check(len(out['records'])==4 and not out['stops'],'empty predecessor is an actual clean witness')
    out=evaluate([A,successor]); check(out['stops'][0]['reason']=='SC' and out['stops'][0]['position']==[H,None],'empty-D extension authenticates SC')
    # Static mismatch and invalid ordinals: no header-valid shortcut to a link.
    mismatch=copy.deepcopy(D['N']); mismatch['header']['qualified']=False; mismatch['header']['content_id']+=':unqualified'
    out=exercise([A,mismatch],'static_descriptor_mismatch')
    check(out['stops'][0]['reason']=='PU','static descriptor mismatch prevents witness')
    for field in ('baseline','mapping'):
        other=copy.deepcopy(D['N']); other['header']['content_id']+=':mismatch-'+field
        if field=='baseline': other['header']['baseline'][4]=1
        else:
            other['header']['mapping']=[[1,1,'w']]; other['header']['H']+=8
            for b in other['blocks']: b['start']+=8; b['end']+=8
        for b in other['blocks']: b['content_id']+=':mismatch-'+field
        out=exercise([A,other],'static_'+field+'_mismatch')
        if field=='mapping':
            check(out['stops'][0]['reason']=='PU' and not out['admissions'][1]['links'][0]['checks'][0]['static_match'],'static mapping mismatch')
        else:
            check(not out['stops'] and out['admissions'][1]['links'][0]['checks'][0]['static_match'] and out['replay_stops'][0]['reason']=='baseline_mismatch','baseline is replay state, not static linkage')
    ordinal=copy.deepcopy(D['N']); ordinal['header'].update(valid=False,raw_content_id='N:invalid-ordinal'); ordinal['header']['previous']['file_no']=5
    out=exercise([A,ordinal],'invalid_ordinal_header'); check(out['unassigned_header_failure'] and not out['stops'],'invalid ordinal has no trusted identity')
    # Whole-block validity masks all interior record faults.
    both=altered(D['Au'],'crc-and-rf',1,checks={'crc_valid':False})
    out=exercise([both],'whole_block_before_record_fault')
    check(all(d['reason']!='RF' for d in out['diagnostics']),'invalid block emits no record fault')
    # Several rows: a sample/marker omitted; repeated equal contents at different offsets survive;
    # a record stop in a block preserves all eligible preceding rows and original offsets.
    multi=copy.deepcopy(A); b=multi['blocks'][0]; row=copy.deepcopy(b['records'][0]); b['records']=[row,dict(copy.deepcopy(row),offset=44)]
    b['content_id']='two-identical-LIFE-records'; b['end']+=36
    for other in multi['blocks'][1:]: other['start']+=36; other['end']+=36; other['content_id']+=':multi-row-chain'
    out=exercise([multi],'identical_records_at_distinct_positions')
    check(len(out['records'])==5 and out['records'][1]['physical'][-1]==44,'same content distinct position')
    fault=copy.deepcopy(multi); rr=fault['blocks'][0]['records'][1]; rr['fault']={'kind':'bad_boolean'}; rr['content_id']+=':bad'; fault['blocks'][0]['content_id']+=':fault'
    out=exercise([fault],'structural_fault_in_multi_record_block')
    check(len(out['records'])==1 and out['stops'][0]['position']==[193,44],'record fault prefix within validated block')
    marker=copy.deepcopy(D['M']); marker['header']['mapping'].append([2,2,'admin']); marker['header']['H']+=12; marker['header']['content_id']+=':admin-mapping'
    for b in marker['blocks']: b['start']+=12; b['end']+=12; b['content_id']+=':admin-header-chain'
    rows=marker['blocks'][0]['records']; rows[0]['value']={'kind':'MARKER','t':1,'name':'admin'}; rows[0]['content_id']='marker'; marker['blocks'][0]['content_id']='marker-block'
    out=exercise([marker],'omitted_marker_and_sample'); check([r['physical'][-2:] for r in out['records']]==[[389,8],[501,0]],'omissions preserve offsets')
    # Zero-duration transitions remain distinct physical rows; semantic assignment is separate.
    zero=copy.deepcopy(D['M']); z=zero['blocks'][0]['records'][0]; z['value'].update(t=0,duration=0,old='w',new=None); z['content_id']='zero-wait'; zero['blocks'][0]['content_id']='zero-wait-block'
    exercise([zero],'zero_duration_wait')
    # Structural exceptions, null/readable counters, delayed dependency ordering are codec-neutral.
    for mode in ('unqualified','null_counters','counter_change','delayed_life'):
        d=copy.deepcopy(A)
        if mode=='unqualified':
            d['header']['qualified']=False; d['header']['content_id']+=':unqualified'
            r=d['blocks'][1]['records'][0]; r['fault']={'kind':'malformed_method'}; r['value']['method']=255
        elif mode=='null_counters':
            r=d['blocks'][1]['records'][0]; r['value']['counters']=None; r['fault']={'kind':'null_counters'}
        elif mode=='counter_change':
            r=d['blocks'][1]['records'][0]; r['value']['counters'][4]=1
        else:
            r=d['blocks'][0]['records'][0]; other=d['blocks'][1]['records'][0]
            r['value'],other['value']=other['value'],r['value']
            other['content_id']+=':delayed'
        r['content_id']+=':'+mode
        for b in d['blocks']: b['content_id']+=':'+mode
        out=exercise([d],mode)
        check(not out['stops'],'semantic and exception cases create no codec RF stop',variant=mode)
        if mode=='unqualified': check(any(x['reason']=='RF' for x in out['diagnostics']),'ignored certificate still diagnosed')
    # Replay assertions are independent of storage monotonicity.
    rA=evaluate([A]); rAP=evaluate([A,D['A+']]); before=replay_probe(rA['records']); after=replay_probe(rAP['records'])
    check(before['coverage']==[[0,20]] and after['coverage']==[[0,10]] and after['high_water']==20,'retroactive VOID and publication high-water')
    # Query ending at 15 is before VOID.t=30: clipping does not license skipping VOID.
    query=(0,15)
    cover=lambda rs:sum(max(0,min(b,query[1])-max(a,query[0])) for a,b in rs)
    check(cover(before['coverage'])==15 and cover(after['coverage'])==10,'query-before-detection still revokes')
    rR=evaluate([D['R'],D['Rc3']]); r=replay_probe(rR['records'])
    check(r['stop']['position']==[537,8] and rR['stops'][0]['position']==[769,None] and r['coverage']==[],'reachable replay fault separate from later codec BC')
    counts['replay_assertions']=3
    # Semantic counter loss does not create a reachable replay fault.
    loss=copy.deepcopy(rA['records']); loss[1]['value']['counters']=None
    rp=replay_probe(loss)
    check(rp['stop'] is None and rp['coverage']==[] and rp['life']==[[0,None]],'null counter loss retains LIFE without coverage')
    counts['replay_assertions']+=1
    # Clause 2a observes ALL header-valid variants once, including unlinked file 1.
    flag2=copy.deepcopy(D['A2']); flag2['header']['flags']=1
    footer2=copy.deepcopy(D['A2']); footer2['tail']=copy.deepcopy(D['Ao']['tail'])
    dirty2=copy.deepcopy(D['A2']); dirty2['tail']=copy.deepcopy(D['Ag']['tail'])
    for size in range(5):
        for choice in itertools.combinations_with_replacement([A,D['A2'],flag2,footer2,dirty2,D['N2']],size):
            exercise(choice,'2a_distinct_witnesses')
    out=evaluate([A,D['A2'],flag2,footer2,dirty2,D['A2'],D['X']])
    u=out['ambiguities'][0]
    check(u['boots']==['b','c'] and len(u['witnesses'])==5,'2a complete distinct witnesses with byte duplicates deduplicated')
    check(all(w['position']==[0,None] for w in u['witnesses']),'2a witness header positions')
    check([c['capture'] for c in out['capture_results'] if c['replay'] is not None]==['x'],'2a retains independent capture only')
    out=evaluate([A,D['N2']]); pu=next(d for d in out['diagnostics'] if d['reason']=='PU')
    check(pu['detail']['available']==[] and pu['detail']['declared_predecessor']['file']==['c','a',0], 'predecessor candidates exclusively own boot/capture/immediate ordinal')
    check(out['records']==[] and out['ambiguities'][0]['witnesses'] and out['stops'][0]['reason']=='PU','nonroot reuse preserves local inspection without replay')
    out=evaluate([A,D['Z2']]); check(not out['ambiguities'] and len(out['records'])==4,'invalid header cannot prove reuse')
    out=evaluate([A,D['A2t0']]); check(out['ambiguities'] and out['stops'][0]['reason']=='T','reuse preserves intrinsic normalized stops')
    out=evaluate([A,D['At1']]); check(out['aggregate_status']=='PASS' and out['diagnostics'] and out['capture_results'][0]['status']=='PASS','suppressed delivery failure alone passes capture and aggregate')
    out=evaluate([A,D['Z']]); check(out['aggregate_status']=='FAILED' and out['capture_results'][0]['status']=='PASS' and out['capture_results'][0]['replay']['coverage']==[[0,20]],'unassigned header fails aggregate with usable passing capture')
    out=evaluate([D['At3'],D['X']]); check(out['aggregate_status']=='FAILED' and next(c for c in out['capture_results'] if c['capture']=='x')['status']=='PASS','failed capture preserves passing independent result')
    for pn,sn,baseline,passed in [('V','Q',D['Q']['header']['baseline'],True),('V','Q0',D['Q']['header']['baseline'],False),
          ('Vu','Tu',None,True),('Vu','T0',None,False),('W','Qw',D['Q']['header']['baseline'],True),
          ('W','Qw0',D['Q']['header']['baseline'],False),('Wi','Qwi0',[0]*15,True),
          ('Wi','Qwi1',[0]*15,False),('Vi','Qvi',D['Q']['header']['baseline'],True)]:
        out=evaluate([D[sn],D[pn]]) # explicitly successor first
        rp=out['capture_results'][0]['replay']; counts['r3b_replay_assertions']+=1
        check(not out['stops'] and out['admissions'][1]['links'][0]['status']=='witnessed','baseline excluded from local link checks',successor=sn)
        check(rp['baseline']==baseline and (rp['stop'] is None)==passed,'running boundary baseline',successor=sn)
        check((out['aggregate_status']=='PASS')==passed,'replay failure reaches capture/aggregate level',successor=sn)
    # A prior semantic stop makes every later successor mismatch unreachable.
    later=copy.deepcopy(D['N1']); later['header']['previous'].update(D=979,digest=dict(kind='prefix',header=D['R']['header']['content_id'],D=979,
        blocks=[[b['start'],b['end'],b['content_id']] for b in D['R']['blocks']],partial=None))
    later['header']['content_id']+=':after-overlap'
    out=exercise([D['R'],later],'unreachable_baseline_after_semantic_stop')
    check(len(out['replay_stops'])==1 and out['replay_stops'][0]['reason']=='overlapping_attachment','do not check baseline beyond earlier replay stop')
    out=evaluate([D['V'],D['N1']]); rp=out['capture_results'][0]['replay']
    check(out['stops'][0]['reason']=='SC' and rp['stop'] is None and rp['baseline']==[0]*15,'sealing proof independent of baseline and prevents unreachable check')
    # Equality tests every counter component; unknown and zero never coalesce.
    for slot in range(15):
        successor=copy.deepcopy(D['N']); successor['header']['baseline'][slot]=1
        successor['header']['content_id']+=f':baseline-slot-{slot}'
        out=exercise([successor,A],f'all_baseline_components_{slot}')
        if out is None: out=evaluate([successor,A])
        check(not out['stops'] and out['replay_stops'][0]['expected']==[0]*15 and out['replay_stops'][0]['declared'][slot]==1,'all fifteen boundary components',slot=slot)
        counts['baseline_component_assertions']+=1
    root=copy.deepcopy(A); root['header']['baseline']=None; root['header']['content_id']+=':unknown-root'; root['blocks']=[]
    out=exercise([root],'unknown_root_baseline'); check(out['capture_results'][0]['replay']['baseline'] is None and out['aggregate_status']=='PASS','unknown file-zero baseline does not become zero')
    restored=copy.deepcopy(D['W']); restored['header']['baseline']=None; restored['header']['content_id']+=':unknown-root-restoration'
    out=exercise([restored],'unknown_baseline_readable_restoration')
    check(out['capture_results'][0]['replay']['baseline']==D['Q']['header']['baseline'] and not out['replay_stops'],'readable certificates consume loss and restore unknown baseline without a descriptor reset')
    report=dict(bounds=dict(max_multiset_size=4,thematic_alphabets=[dict(name=t,size=len(n)) for t,n in pools],
                           additional_alphabets=[dict(name='multiple_seal_claims_and_witness_ties',size=5),dict(name='empty_predecessor_multiple_claims',size=4),dict(name='complete_malformed_footer',size=3),dict(name='2a_distinct_witnesses',size=6)],
                           cut_offsets=[0,total],deterministic_rounds=1,random_rounds=0,targeted_direct_max_size=7,scope='exhaustive within alphabets; targeted extensions; not global cartesian product'),
                monotonicity_exception='Only a capture newly shown by independently valid headers to name multiple boots may retract all its positions/results under 2a; all other lost storage positions require a new HC/BC/SC at or before that position in the same boot/capture.',
                counts=dict(counts),coverage=sorted(coverage),failures=failures,elapsed_seconds=round(time.monotonic()-started,3))
    (ROOT/'props-result.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
    print('properties '+json.dumps(dict(counts),sort_keys=True))
    print(f'failures {len(failures)}; elapsed {report["elapsed_seconds"]}s')
    for f in failures: print(json.dumps(f,ensure_ascii=False))
    return bool(failures)

if __name__=='__main__': raise SystemExit(run())
