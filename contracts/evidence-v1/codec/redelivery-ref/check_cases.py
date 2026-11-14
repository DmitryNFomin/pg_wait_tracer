#!/usr/bin/env python3
"""Compare each frozen table cell, including all unique permutations/submultisets."""
import argparse
import itertools
import json
from pathlib import Path
from ref_redelivery import evaluate, wire_identity, canonical, RANK, Reconciler
from replay_effects import replay_probe

ROOT=Path(__file__).resolve().parent

def freeze_view(db, result, names):
    """Translate symbolic byte identities to fixture names only for reporting."""
    ids={}
    for name in sorted(names): ids.setdefault(wire_identity(db['deliveries'][name]),name)
    # Ac2t1 and At1 are byte-identical; the spelling in the current row is used.
    def name(x): return ids[x]
    def witnesses(ws,reason):
        if reason in ('HC','BC'): return sorted([sorted([name(a),name(b)]) for a,b in ws])
        if reason=='SC': return sorted([[name(a),name(b)] for a,b in ws])
        return sorted(name(w) for w in ws)
    stops=[dict(file=s['file'],position=s['position'],reason=s['reason'],witnesses=witnesses(s['witnesses'],s['reason'])) for s in result['stops']]
    diag={}
    for d in result['diagnostics']:
        key=canonical([d['file'],d['position'],d['reason']])
        if key not in diag: diag[key]=dict(file=d['file'],position=d['position'],reason=d['reason'],witnesses=[])
        diag[key]['witnesses']+=witnesses(d['witnesses'],d['reason'])
    for d in diag.values(): d['witnesses']=json.loads(canonical(sorted({canonical(w):w for w in d['witnesses']}.values(),key=canonical)))
    return dict(records=result['records'],stops=sorted(stops,key=canonical),diagnostics=sorted(diag.values(),key=canonical),
                sources=sorted(name(s) for a in result['admissions'] for s in a['sources']),
                admitted={a['file'][0]+a['file'][1]+str(a['file'][2]):a['admitted'] for a in result['admissions']},
                unassigned_header_failure=result['unassigned_header_failure'],aggregate_status=result['aggregate_status'],
                decode_failed=result['decode_failed'],
                capture_status={c['capture']:c['status'] for c in result['capture_results']},
                ambiguities=[dict(capture=a['capture'],reason=a['reason'],boots=a['boots'],
                    witnesses=sorted([dict(delivery=name(w['delivery']),boot=w['boot'],file_no=w['file_no'],position=w['position']) for w in a['witnesses']],key=canonical)) for a in result['ambiguities']])

def expected_view(db,c):
    e=c['expected']; ds=db['deliveries']; stops=[]; diags={}
    def pos(x): return x if isinstance(x,list) else [x,None]
    def add_diag(file,position,reason,witnesses):
        key=canonical([file,position,reason])
        if key not in diags: diags[key]=dict(file=file,position=position,reason=reason,witnesses=[])
        diags[key]['witnesses']+=witnesses
    for alias,offset,reason,ws in e['stops']:
        file=ds[alias]['header']['physical']; ws=sorted([sorted(w) if reason in ('HC','BC') else w for w in ws],key=canonical)
        s=dict(file=file,position=pos(offset),reason=reason,witnesses=ws); stops.append(s)
        add_diag(file,pos(offset),reason,ws)
    for delivery,offset,reason in e['extra_diagnostics']:
        if isinstance(delivery,list):
            file=ds[delivery[0]]['header']['physical']; ws=[sorted(delivery)]
        else:
            file=ds[delivery]['header']['physical'] if ds[delivery]['header']['valid'] else None; ws=[delivery]
        add_diag(file,pos(offset),reason,ws)
    for d in diags.values(): d['witnesses']=sorted({canonical(w):w for w in d['witnesses']}.values(),key=canonical)
    return dict(records=[db['records'][a] for a in e['record_aliases']],stops=sorted(stops,key=canonical),
                diagnostics=sorted(diags.values(),key=canonical),sources=sorted(e['source_deliveries']),
                admitted=e['admitted'],unassigned_header_failure=e['failed_decode'],aggregate_status=e['aggregate_status'],
                decode_failed=e['aggregate_status']=='FAILED',capture_status=e['capture_status'],
                ambiguities=[] if not e['ambiguity_witnesses'] else [dict(capture='a',reason='CAPTURE_BOOT_AMBIGUITY',boots=['b','c'],
                   witnesses=sorted([dict(delivery=n,boot=ds[n]['header']['physical'][0],file_no=ds[n]['header']['physical'][2],position=[0,None]) for n in e['ambiguity_witnesses']],key=canonical))])

# Independent, manually declared local link facts for ALL successor rows.
# (predecessor alias, E, clean, boundary, digest, exact, extension, witness, seal)
LOCAL={
8:{'Nt2':[('A',957,True,True,True,True,False,True,False),('Ag',957,False,True,True,True,False,False,False)]},
9:{'N':[('At3',747,False,False,None,False,False,False,False)]},
16:{'N':[('A',957,True,True,True,True,False,True,False),('A+',1179,True,True,True,False,True,False,True)]},
18:{'N':[('A+',1179,True,True,True,False,True,False,True)]},
19:{'N':[('Ag',957,False,True,True,True,False,False,False)]},
20:{'N':[('At1',305,False,False,None,False,False,False,False)],
'L':[('N',689,True,True,True,True,False,True,False)],'Lc2t3':[('N',689,True,True,True,True,False,True,False)]},
23:{'L':[]},24:{'N':[]},
27:{'Nb':[('A+',1179,True,True,False,False,True,False,False)]},
28:{'Ni':[('A+',1179,True,False,None,False,False,False,False)]}}

# r3b independent local facts. Baselines intentionally absent from static checks.
LOCAL.update({33:{'N2':[]},
36:{'Q':[('V',1179,True,True,True,True,False,True,False)]},
37:{'Q0':[('V',1179,True,True,True,True,False,True,False)]},
38:{'N':[('A',957,True,True,True,True,False,True,False)]},
39:{'Q':[('A',957,True,False,None,False,False,False,False),('V',1179,True,True,True,True,False,True,False)]},
40:{'N1':[('V',1179,True,True,True,False,True,False,True)]},
41:{'N1':[('A',957,True,True,True,True,False,True,False)]},
43:{'Tu':[('Vu',1059,True,True,True,True,False,True,False)]},
44:{'T0':[('Vu',1059,True,True,True,True,False,True,False)]},
45:{'Qw':[('W',1167,True,True,True,True,False,True,False)]},
46:{'Qw0':[('W',1167,True,True,True,True,False,True,False)]},
48:{'Qwi0':[('Wi',1167,True,True,True,True,False,True,False)]},
49:{'Qwi1':[('Wi',1167,True,True,True,True,False,True,False)]},
50:{'Qvi':[('Vi',1179,True,True,True,True,False,True,False)]}})

def expected_links(db,c):
    result={}
    for name in set(c['input']):
        d=db['deliveries'][name]; h=d['header']
        if not h['valid']: continue
        ident=wire_identity(d)
        if h['physical'][2]==0:
            result[ident]=dict(status='root',witnesses=[],seal_pairs=[],checks=[]); continue
        checks=[]
        for pn,E,clean,boundary,digest,exact,extension,witness,seal in LOCAL[c['case']][name]:
            checks.append(dict(predecessor=wire_identity(db['deliveries'][pn]),validated_end=E,clean=clean,static_match=True,
                               boundary=boundary,digest_match=digest,exact_end=exact,extension=extension,witness=witness,seal_proof=seal))
        checks.sort(key=lambda x:x['predecessor'])
        ws=sorted(x['predecessor'] for x in checks if x['witness'])
        pairs=sorted([[x['predecessor'],ident] for x in checks if x['seal_proof']])
        result[ident]=dict(status='seal_conflict' if pairs else 'witnessed' if ws else 'unavailable',witnesses=ws,seal_pairs=pairs,checks=checks)
    return result

def compare(db,c,result):
    actual=freeze_view(db,result,c['input']); expected=expected_view(db,c); errors=[]
    for column in expected:
        if actual[column]!=expected[column]: errors.append(dict(column=column,expected=expected[column],actual=actual[column]))
    for cr in result['capture_results']:
        cap=cr['capture']
        boots=sorted({db['deliveries'][n]['header']['physical'][0] for n in c['input'] if db['deliveries'][n]['header']['valid'] and db['deliveries'][n]['header']['physical'][1]==cap})
        wanted_stops=[st for st in result['stops'] if st['file'][1]==cap]
        wanted_ambiguity=next((a for a in result['ambiguities'] if a['capture']==cap),None)
        for field,wanted in [('boots',boots),('codec_stops',wanted_stops),('ambiguity',wanted_ambiguity)]:
            if cr.get(field)!=wanted: errors.append(dict(column='capture.'+field,expected=wanted,actual=cr.get(field)))
    links={x['delivery']:{k:v for k,v in x.items() if k!='delivery'} for a in result['admissions'] for x in a['links']}
    elinks=expected_links(db,c)
    if links!=elinks: errors.append(dict(column='local_link_witnesses_and_admission',expected=elinks,actual=links))
    # Clause 20: every PU diagnostic must carry the declared predecessor and all checks.
    for diag in result['diagnostics']:
        if diag['reason']!='PU': continue
        ident=diag['witnesses'][0]
        source=next(db['deliveries'][n] for n in c['input'] if wire_identity(db['deliveries'][n])==ident)
        h=source['header']; prev=h['previous']
        wanted=dict(declared_predecessor=dict(file=h['physical'][:2]+[prev['file_no']],**prev),available=elinks[ident]['checks'])
        if diag.get('detail')!=wanted: errors.append(dict(column='PU_evidence',expected=wanted,actual=diag.get('detail')))
    for diag in result['diagnostics']:
        if diag.get('failure_level')!='delivery': errors.append(dict(column='delivery_failure_levels',actual=diag))
    # New explicit replay cells are compared field-by-field, with complete
    # physical records and contents before the reachable semantic cutoff.
    summaries=c['expected']['replay_summaries']
    if summaries is not None:
        got={cr['capture']:cr['replay'] for cr in result['capture_results'] if cr['replay'] is not None}
        if set(got)!=set(summaries): errors.append(dict(column='replay_capture_set',expected=sorted(summaries),actual=sorted(got)))
        for cap,wanted in summaries.items():
            if cap not in got: continue
            for field,value in wanted.items():
                if got[cap].get(field)!=value: errors.append(dict(column='replay.'+field,capture=cap,expected=value,actual=got[cap].get(field)))
            rs=[db['records'][alias] for alias in c['expected']['record_aliases'] if db['records'][alias]['physical'][1]==cap]
            if wanted['stop'] is not None: rs=[r for r in rs if r['physical'][2]<wanted['stop']['file'][2]]
            if got[cap]['records']!=rs: errors.append(dict(column='replay.records',expected=rs,actual=got[cap]['records']))
        expected_stops=[v['stop'] for v in summaries.values() if v['stop'] is not None]
        if result['replay_stops']!=expected_stops: errors.append(dict(column='replay_stops',expected=expected_stops,actual=result['replay_stops']))
    replay=c['expected']['replay']
    if replay is not None:
        got=replay_probe(result['records'])
        if c['case']==22: got={k:got[k] for k in replay}
        else: got=dict(stop=got['stop']['position'],reason=got['stop']['reason'],life=got['life'],coverage=got['coverage'])
        if got!=replay: errors.append(dict(column='explicit_replay_effect',expected=replay,actual=got))
    return errors

def run():
    db=json.loads((ROOT/'cases.json').read_text()); failures=[]; passed=0; permutations=0; incremental=0; subsets=set()
    for c in db['cases']:
        ds=[db['deliveries'][n] for n in c['input']]; baseline=evaluate(ds)
        errors=compare(db,c,baseline)
        for order in sorted(set(itertools.permutations(c['input']))):
            permutations+=1; actual=evaluate([db['deliveries'][n] for n in order])
            # Compare EVERY permutation directly against the frozen expected cells.
            ordererrors=compare(db,c,actual)
            if ordererrors: errors.extend([dict(order=order,**e) for e in ordererrors])
            accumulated=Reconciler()
            for k in range(len(order)+1):
                prefix=order[:k]
                if k: updated=accumulated.add(db['deliveries'][order[k-1]])
                else: updated=accumulated.result()
                oracle=evaluate([db['deliveries'][n] for n in prefix])
                incremental+=1
                if updated!=oracle: errors.append(dict(column='incremental',prefix=prefix))
                subsets.add(tuple(sorted(prefix)))
        if errors: failures.append(dict(case=c['case'],disagreements=errors))
        else: passed+=1
    report=dict(cases_passed=passed,cases_total=50,permutations=permutations,
                incremental_prefix_checks=incremental,unique_accumulated_submultisets=len(subsets),failures=failures,
                expected_basis='REDELIVERY r3b table transcribed in build_cases.py plus manually declared independent link/replay facts; no product output')
    (ROOT/'cases-result.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
    print(f'cases {passed}/50; permutations {permutations}; incremental prefixes {incremental}; disagreements {sum(len(f["disagreements"]) for f in failures)}')
    for f in failures:
        print(json.dumps(f,ensure_ascii=False))
    return 0 if passed==50 else 1

if __name__=='__main__': raise SystemExit(run())
