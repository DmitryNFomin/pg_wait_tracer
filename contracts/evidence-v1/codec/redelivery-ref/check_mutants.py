#!/usr/bin/env python3
"""Targeted source mutants: compile, run designated probe, fail intended assertion.

These are focused gates, not a claim of exhaustive mutation coverage of every
compound obligation in REDELIVERY.md. No mutated file replaces the oracle.
"""
import copy
import json
import types
import sys
import replay_effects
from pathlib import Path
from ref_redelivery import evaluate
from check_cases import compare

ROOT=Path(__file__).resolve().parent

def run():
    db=json.loads((ROOT/'cases.json').read_text()); codec=(ROOT/'ref_redelivery.py').read_text(); replay=(ROOT/'replay_effects.py').read_text()
    # module, clause, name, designated case, intended expected column, old, new
    mutants=[
('codec',1,'arrival_order',21,'permutation',"for k in sorted(unique)]", "for k in unique]"),
('codec',2,'duplicate_deliveries',12,'sources',"unique[ident]=d", "unique[ident + ':' + str(len(unique))]=d"),
('codec',3,'reason_rank',13,'stops',"RANK = {r:i for i,r in enumerate(REASONS)}", "RANK = {r:i for i,r in enumerate(REASONS)}\nRANK['CRC'],RANK['T']=RANK['T'],RANK['CRC']"),
('codec',4,'null_position_order',6,'records',"return p[0], -1 if p[1] is None else p[1]", "return p[0], 999999 if p[1] is None else p[1]"),
('codec',5,'skip_sample_validation',15,'stops',"for b in d['blocks']:\n        reason=", "for b in d['blocks']:\n        if b['type']==1: continue\n        reason="),
('codec',6,'bounds_before_truncation',None,'terminal_precedence',"if not checks.get('header_fields_valid', True): return 'IB'", "if not checks.get('header_fields_valid', True): return 'T'"),
('codec',6,'crc_before_decode',None,'crc_precedence',"if not checks.get('crc_valid', True): return 'CRC'", "if not checks.get('crc_valid', True): return 'IB'"),
('codec',7,'RF_is_diagnostic_only',29,'stops',"'stop':stop", "'stop':False"),
('codec',7,'null_counter_becomes_RF',None,'null_counter_exception',"if fault['kind']=='null_counters': continue", "if False: continue"),
('codec',8,'ignore_header_conflict',7,'stops',"if x['raw']['header']['content_id']!=y['raw']['header']['content_id']:", "if False:"),
('codec',9,'ignore_block_conflict',6,'stops',"if a['content_id']!=b['content_id']:", "if False:"),
('codec',10,'shortest_source',1,'records',"E=max(s['E'] for s in g)", "E=min(s['E'] for s in g)"),
('codec',11,'emit_samples',14,'records',"if b['type']==1 or r['value'].get('kind')=='MARKER': continue", "if False: continue"),
('codec',12,'short_terminal_witness',1,'stops',"witnesses=[s['delivery'] for s in maxcopies if s['terminal']['reason']==reason]", "witnesses=[s['delivery'] for s in g if s['terminal'] and s['terminal']['reason']==reason]"),
('codec',13,'dirty_predecessor_witness',19,'stops',"digest and clean and exact", "digest and exact"),
('codec',13,'digest_not_checked',27,'stops',"digest=(prev['digest']==prefix_digest(p,D)) if boundary else None", "digest=True if boundary else None"),
('codec',14,'require_exact_end_for_SC',18,'stops',"seal_proof=bool(static and boundary and digest and extension)", "seal_proof=bool(static and boundary and digest and extension and exact)"),
('codec',14,'SC_in_successor',18,'stops',"diag(predfile,[prev['D'],None],'SC',pairs,eligible=True)", "diag(file,[prev['D'],None],'SC',pairs,eligible=True)"),
('codec',15,'SC_requires_clean_witness',18,'stops',"if pairs:\n            # Clauses 14", "if pairs and witnesses:\n            # Clauses 14"),
('codec',16,'ignore_later_files',20,'stops',"groups=defaultdict(list)", "states=[s for s in states if s['file'] is None or s['file'][2]<2]\n    groups=defaultdict(list)"),
('codec',16,'drop_terminal_tie_witness',21,'stops',"out['witnesses']=_witness_union(w for c in tied for w in c['witnesses'])", "out['witnesses']=_witness_union(w for c in tied for w in c['witnesses'])[:1]"),
('codec',17,'last_stop_wins',29,'stops',"key=min((pkey(c['position']),RANK[c['reason']]) for c in cs)", "key=max((pkey(c['position']),RANK[c['reason']]) for c in cs)"),
('codec',18,'no_capture_cutoff',9,'records',"cutoff=cutoffs.get(cap)", "cutoff=None"),
('replay',19,'skip_VOID_revocation',22,'explicit_replay_effect',"cut=r['invalid_from']; counters=r['counters']", "cut=None; counters=r['counters']"),
('codec',20,'drop_suppressed_diagnostics',2,'diagnostics',"diagnostics=_dedup_diagnostics(diagnostics)", "diagnostics=_dedup_diagnostics([d for d in diagnostics if any(d['file']==s['file'] and d['position']==s['position'] and d['reason']==s['reason'] for s in stops)])"),
('codec',20,'invalid_header_empty_success',31,'unassigned_header_failure',"unassigned=any(s['file'] is None for s in states)", "unassigned=False"),
('codec',20,'PU_without_evidence',9,'PU_evidence',"dict(declared_predecessor=dict(file=list(predfile),**prev),available=checks),True)", "dict(declared_predecessor=dict(file=list(predfile),**prev),available=[]),True)")]
    mutants += [
('codec',0,'suppressed_delivery_fails_aggregate',2,'aggregate_status',"aggregate_status='FAILED' if unassigned or any(c['status']=='FAILED' for c in capture_results) else 'PASS'", "aggregate_status='FAILED' if diagnostics or any(c['status']=='FAILED' for c in capture_results) else 'PASS'"),
('codec',0,'unassigned_does_not_fail_aggregate',17,'aggregate_status',"aggregate_status='FAILED' if unassigned or any(c['status']=='FAILED' for c in capture_results) else 'PASS'", "aggregate_status='FAILED' if any(c['status']=='FAILED' for c in capture_results) else 'PASS'"),
('codec',0,'replay_failure_not_capture_failure',41,'capture_status',"or (replay is not None and replay['stop'] is not None)", "or False"),
('codec',0,'ambiguity_not_capture_failure',32,'capture_status',"or ambiguity is not None or", "or False or"),
('codec',0,'delivery_level_misclassified',2,'delivery_failure_levels',"d['failure_level']='delivery'", "d['failure_level']='capture'"),
('codec',0,'codec_failure_not_capture_failure',1,'capture_status',"failed=bool(cstops)", "failed=False"),
('codec','2a','root_only_boot_observation',33,'ambiguities',"for boot,cap,no in groups: capboots[cap].add(boot)", "for boot,cap,no in groups:\n        if no==0: capboots[cap].add(boot)"),
('codec','2a','invalid_header_proves_reuse',42,'ambiguities',"    ambiguities=[]", "    for st in states:\n        if st['file'] is None: capboots[st['raw']['header']['physical'][1]].add(st['raw']['header']['physical'][0])\n    ambiguities=[]"),
('codec','2a','emit_ambiguous_capture_prefix',32,'records',"and file[1] not in ambiguous", ""),
('codec','2a','withhold_independent_capture',34,'records',"and file[1] not in ambiguous", "and not ambiguous"),
('codec','2a','drop_nonroot_ambiguity_witness',33,'ambiguities',"for s in states if s['file'] is not None and s['file'][1]==cap]", "for s in states if s['file'] is not None and s['file'][1]==cap and s['file'][2]==0]"),
('codec','2a','drop_codec_stop_under_ambiguity',47,'stops',"    cutoffs={}", "    stops=[st for st in stops if st['file'][1] not in ambiguous]\n    cutoffs={}"),
('codec',13,'baseline_in_static_link',36,'stops',"and sh['qualified']==ph['qualified'] #", "and sh['qualified']==ph['qualified'] and sh['baseline']==ph['baseline'] #"),
('codec',13,'cross_boot_predecessor_candidates',33,'local_link_witnesses_and_admission',"checks=[link_check(s,p) for p in groups.get(predfile,[])]", "checks=[link_check(s,p) for f,g in groups.items() if f[1:]==predfile[1:] for p in g]"),
('codec',14,'baseline_required_for_seal_proof',40,'stops',"seal_proof=bool(static and boundary and digest and extension)", "seal_proof=bool(static and boundary and digest and extension and sh['baseline']==ph['baseline'])"),
('replay',19,'skip_successor_baseline_check',41,'replay.stop',"elif item['baseline']!=baseline:", "elif False:"),
('replay',19,'unknown_equals_zero_baseline',44,'replay.stop',"elif item['baseline']!=baseline:", "elif (item['baseline'] or ZERO)!=(baseline or ZERO):"),
('replay',19,'unqualified_skips_descriptor_check',49,'replay.stop',"elif item['baseline']!=baseline:", "elif qualified and item['baseline']!=baseline:"),
('replay',19,'ignored_certificate_moves_baseline',48,'replay.baseline',"retained.append(item); continue\n        if kind=='FRONTIER'", "baseline=r.get('counters',baseline)\n            retained.append(item); continue\n        if kind=='FRONTIER'"),
('replay',19,'explicit_VOID_ignored_unqualified',50,'replay.baseline',"if not qualified and kind in ('ATTACH_OK','FRONTIER','END'):", "if not qualified and kind in ('ATTACH_OK','FRONTIER','END','VOID'):"),
('replay',19,'frontier_loss_does_not_move_baseline',45,'replay.baseline',"baseline=counters; last_void=t", "baseline=baseline if item['value']['kind']=='FRONTIER' else counters; last_void=t"),
('replay',19,'skip_VOID_baseline_update',36,'replay.baseline',"baseline=counters; last_void=t", "last_void=t"),
('replay',19,'baseline_check_before_VOID',37,'replay.stop',"elif item['baseline']!=baseline:", "elif item['baseline']!=ZERO:"),
('replay',19,'stop_forgets_prior_revocations',37,'replay.revoked',"expected=baseline,declared=item['baseline']); break", "expected=baseline,declared=item['baseline']); revoked=[]; break")]
    results=[]
    for domain,clause,name,case,column,old,new in mutants:
        source=codec if domain=='codec' else replay
        entry=dict(clause=clause,mutant=name,case=case,intended_assertion=column,build=False,reached=False,killed=False)
        try:
            if source.count(old)!=1: raise ValueError(f'mutation anchor occurs {source.count(old)} times')
            module=types.ModuleType('mutant_'+name)
            exec(compile(source.replace(old,new),str(ROOT/(name+'.py')),'exec'),module.__dict__)
            entry['build']=True
            if case is None:
                if column=='terminal_precedence': actual=module.terminal_reason(dict(header_fields_valid=False,bytes_complete=False)); expected='IB'
                elif column=='crc_precedence': actual=module.terminal_reason(dict(crc_valid=False,decode_valid=False)); expected='CRC'
                else:
                    d=copy.deepcopy(db['deliveries']['A']); r=d['blocks'][1]['records'][0]
                    r['fault']={'kind':'null_counters'}; r['value']['counters']=None; r['content_id']+=':null'; d['blocks'][1]['content_id']+=':null'
                    actual=module.evaluate([d])['stops']; expected=[]
                entry['reached']=True; entry['killed']=actual!=expected
            else:
                c=db['cases'][case-1]; ds=[db['deliveries'][n] for n in c['input']]
                if domain=='replay' and column=='explicit_replay_effect':
                    actual=module.replay_probe(evaluate(ds)['records'])
                    expected=c['expected']['replay']; entry['reached']=True
                    entry['killed']=any(actual[k]!=v for k,v in expected.items())
                elif domain=='replay':
                    original=sys.modules['replay_effects']
                    try:
                        sys.modules['replay_effects']=module
                        actual=evaluate(ds)
                    finally: sys.modules['replay_effects']=original
                    errors=compare(db,c,actual); entry['reached']=True
                    entry['killed']=any(e['column']==column for e in errors)
                elif column=='permutation':
                    actual=module.evaluate(ds); reverse=module.evaluate(list(reversed(ds))); entry['reached']=True
                    entry['killed']=actual!=reverse
                else:
                    errors=compare(db,c,module.evaluate(ds)); entry['reached']=True
                    entry['killed']=any(e['column']==column for e in errors)
        except Exception as e:
            entry['error']=f'{type(e).__name__}: {e}'
        results.append(entry)
    killed=sum(r['build'] and r['reached'] and r['killed'] for r in results)
    report=dict(mutants=len(results),built=sum(r['build'] for r in results),reached=sum(r['reached'] for r in results),killed=killed,
                numbered_clauses_covered=sorted(set(r['clause'] for r in results if r['killed']),key=str),results=results)
    (ROOT/'mutants-result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'mutants: built {report["built"]}/{len(results)}, reached {report["reached"]}, killed {killed}; clauses {len(report["numbered_clauses_covered"])}/22 (0, 2a, 1–20)')
    for r in results:
        if not r['killed']: print(json.dumps(r))
    return killed!=len(results)

if __name__=='__main__': raise SystemExit(run())
