#!/usr/bin/env python3
"""Freeze text-derived inputs/answers. Does not import or execute the oracle."""
import copy
import hashlib
import json
from pathlib import Path

ZERO = [0] * 15

def canon(x):
    return json.dumps(x, sort_keys=True, separators=(',', ':'), ensure_ascii=False)

def cid(x):
    return "sym:" + hashlib.sha256(canon(x).encode()).hexdigest()

def record(kind, t=0, **kw):
    r = dict(kind=kind, t=t, **kw)
    if kind in ('LIFE', 'ATTACH_OK', 'END'):
        r.update(pid=kw.get('pid', 1), fork=0)
    if kind == 'LIFE': r.update(start=0, start_known=True, exit=None)
    if kind in ('ATTACH_OK', 'FRONTIER', 'VOID'): r['counters'] = ZERO[:]
    if kind == 'ATTACH_OK': r.update(method='reread', seed=None, state=None)
    return r

def refresh(d):
    """Symbolic full stored-block identities include repaired integrity chaining."""
    prev = d['header']['content_id']
    for b in d['blocks']:
        b['content_id'] = cid([prev, {k:v for k,v in b.items() if k != 'content_id'}])
        prev = b['content_id']
    return d

def base(capture, file_no, H, starts, end, rows, types=None):
    h = dict(valid=True, physical=['b', capture, file_no], H=H, flags=0,
             qualified=True, baseline=ZERO[:], mapping=([[1, 1, 'w']] if capture == 'm' else []),
             creation=0, previous=None)
    h['content_id'] = cid({k:v for k,v in h.items() if k not in ('valid','flags')})
    blocks = []
    for i, row in enumerate(rows):
        typ = (types or [2]*len(rows))[i]
        blocks.append(dict(start=starts[i], end=(starts+[end])[i+1], type=typ, codec=0,
                           checks={}, records=[dict(offset=8 if typ == 2 else 0,
                           content_id=cid(row), value=row, fault=None)]))
    return refresh(dict(header=h, blocks=blocks, tail=dict(kind='eof', length=0, content_id='')))

def prefix(d, D):
    h = d['header']
    return dict(kind='prefix', header=h['content_id'], D=D,
                blocks=[[b['start'], b['end'], b['content_id']] for b in d['blocks'] if b['end'] <= D],
                partial=next(([b['content_id'], D-b['start']] for b in d['blocks'] if b['start'] < D < b['end']), None))

def reheader(d):
    h=d['header']; h['content_id']=cid({k:v for k,v in h.items() if k not in ('valid','flags','content_id')})
    return refresh(d)

A=base('a',0,193,[193,305,537,747],957,[record('LIFE'),record('ATTACH_OK'),record('FRONTIER',10),record('FRONTIER',20)])
AP=copy.deepcopy(A)
v=record('VOID',30,reason='x',invalid_from=20)
AP['blocks'].append(dict(start=957,end=1179,type=2,codec=0,checks={},records=[dict(offset=8,content_id=cid(v),value=v,fault=None)]))
refresh(AP)
N=base('a',1,241,[241,353,465,577],689,[record('LIFE',pid=i) for i in range(2,6)])
N['header']['previous']=dict(file_no=0,D=957,digest=prefix(A,957)); reheader(N)
L=base('a',2,241,[241,353,465,577],689,[record('LIFE',pid=i) for i in range(6,10)])
L['header']['previous']=dict(file_no=1,D=689,digest=prefix(N,689)); reheader(L)
X=base('x',0,193,[193,305,417,529],641,[record('LIFE',pid=i) for i in range(10,14)])
M=base('m',0,201,[201,292,377,489],580,[
    record('TRANS',1,pid=1,old=None,new='w',duration=0,query_id=0,cpu_ns=0),
    record('SAMPLE',2,pid=1,event='w',weight=1,query_id=0),
    record('LIFE'),record('TRANS',3,pid=1,old='w',new=None,duration=2,query_id=0,cpu_ns=0)], [0,1,2,0])
M['blocks'][1]['sample_period']=1; M['blocks'][1]['max_weight']=1
# Include metadata in the identity (these two fields have their fixed defaults in fixtures).
refresh(M)
R=base('a',0,193,[193,305,537,769],979,[record('LIFE'),record('ATTACH_OK'),record('ATTACH_OK',1),record('FRONTIER',20)])
# R has exactly A's immutable header, despite its different data length.
R['header']=copy.deepcopy(A['header']); refresh(R)
D={'A':A,'A+':AP,'N':N,'L':L,'X':X,'M':M,'R':R}

def changed(src,j):
    d=copy.deepcopy(D[src]); r=d['blocks'][j]['records'][0]
    key='t' if d['blocks'][j]['type']==2 else 'query_id'
    r['value'][key]+=1; r['content_id']=cid(r['value']); return refresh(d)

def torn(d,k):
    d=copy.deepcopy(d); B=d['blocks'][k]['start']
    # Type-2 timestamp hints are zero; other blocks begin with first_timestamp_ns.
    first_byte=0 if d['blocks'][k]['type']==2 else d['blocks'][k]['records'][0]['value']['t'] & 255
    d['blocks']=d['blocks'][:k]
    d['tail']=dict(kind='rejected',length=1,content_id=f'{first_byte:02x}',checks=dict(header_complete=False))
    return d

for src,k in [('A',1),('A',3),('N',2),('M',3)]: D[f'{src}t{k}']=torn(D[src],k)
for src,j,k in [('A',2,1),('A',2,3),('M',1,3),('L',2,3)]: D[f'{src}c{j}t{k}']=torn(changed(src,j),k)
for k in (2,3):
    d=copy.deepcopy(A); d['blocks'][k]['checks']['crc_valid']=False
    d['blocks'][k]['content_id']+=':crc-bit-flip'; D[f'Acrc{k}']=d
for name,kind,length,tail_cid,checks in [('Ag','rejected',1,'00',{'header_complete':False}),('Ag′','rejected',1,'01',{'header_complete':False}),('Ao','footer',68,'exact-footer',{}),('Aft','rejected',17,'footer-first17',{'header_complete':False})]:
    d=copy.deepcopy(A); d['tail']=dict(kind=kind,length=length,content_id=tail_cid,checks=checks); D[name]=d
D['Af']=copy.deepcopy(A); D['Af']['header']['flags']=1
D['Ah']=copy.deepcopy(A); D['Ah']['header']['creation']=1; reheader(D['Ah'])
D['Z']=copy.deepcopy(A); D['Z']['header']['valid']=False; D['Z']['header']['raw_content_id']='A:bad-header-crc'
D['Nb']=copy.deepcopy(N); D['Nb']['header']['previous']['digest']={'kind':'wrong','content_id':'first-digest-bit-flipped'}; reheader(D['Nb'])
D['Ni']=copy.deepcopy(N); D['Ni']['header']['previous'].update(D=956,digest=prefix(A,956)); reheader(D['Ni'])
D['Au']=copy.deepcopy(A)
r=D['Au']['blocks'][1]['records'][0]; r['value']['kind']=255; r['content_id']=cid(r['value']); r['fault']={'kind':'unknown_kind','detail':'wire kind 255'}; refresh(D['Au'])
D['Auc3']=changed('Au',3); D['Rc3']=changed('R',3)

# r3b fixtures, derived from REDELIVERY fixture definitions, not evaluator output.
B1=ZERO[:]; B1[4]=1
D['A2']=copy.deepcopy(A); D['A2']['header']['physical'][0]='c'; reheader(D['A2'])
D['A2t0']=torn(D['A2'],0)
D['N2']=copy.deepcopy(N); D['N2']['header']['physical'][0]='c'
D['N2']['header']['previous']['digest']=prefix(D['A2'],957); reheader(D['N2'])
D['Z2']=copy.deepcopy(D['A2']); D['Z2']['header'].update(valid=False,raw_content_id='A2:bad-header-crc')
D['V']=copy.deepcopy(AP); D['V']['blocks'][-1]['records'][0]['value']['counters']=B1[:]
D['V']['blocks'][-1]['records'][0]['content_id']=cid(D['V']['blocks'][-1]['records'][0]['value']); refresh(D['V'])
qrows=[dict(record('LIFE',40,pid=i),fork=40,start=40) for i in range(2,6)]
D['Q']=base('a',1,241,[241,353,465,577],689,qrows)
def successor(name,src,pred,baseline):
    d=copy.deepcopy(D[src]); d['header']['baseline']=copy.deepcopy(baseline)
    pd=D[pred]; end=pd['blocks'][-1]['end'] if pd['blocks'] else pd['header']['H']
    d['header']['previous']=dict(file_no=0,D=end,digest=prefix(pd,end))
    D[name]=reheader(d)
successor('Q','Q','V',B1); successor('Q0','Q','V',ZERO); successor('N1','N','A',B1)
D['Vu']=copy.deepcopy(D['V']); D['Vu']['blocks'][-1]['end']=1059
D['Vu']['blocks'][-1]['records'][0]['value']['counters']=None
D['Vu']['blocks'][-1]['records'][0]['content_id']=cid(D['Vu']['blocks'][-1]['records'][0]['value']); refresh(D['Vu'])
D['Tu']=base('a',1,121,[],121,[]); successor('Tu','Tu','Vu',None)
D['T0']=base('a',1,241,[],241,[]); successor('T0','T0','Vu',ZERO)
D['W']=copy.deepcopy(A); w=record('FRONTIER',30); w['counters']=B1[:]
D['W']['blocks'].append(dict(start=957,end=1167,type=2,codec=0,checks={},records=[dict(offset=8,content_id=cid(w),value=w,fault=None)])); refresh(D['W'])
successor('Qw','Q','W',B1); successor('Qw0','Q','W',ZERO)
for name,src in [('Wi','W'),('Vi','V')]:
    D[name]=copy.deepcopy(D[src]); D[name]['header']['qualified']=False; reheader(D[name])
for name,src,pred,bl in [('Qwi0','Qw0','Wi',ZERO),('Qwi1','Qw0','Wi',B1),('Qvi','Q','Vi',B1)]:
    D[name]=copy.deepcopy(D[src]); D[name]['header']['qualified']=False; successor(name,name,pred,bl)

# Frozen text-derived expectations. r3b additions were transcribed before
# modifying the earlier evaluator; this builder never imports that evaluator.
# Each tuple: inputs, retained record aliases, stops (file alias,position,reason,witnesses), extra diagnostics.
ROWS=[
('At1 At3','a0 a1 a2',[('A',747,'T',['At3'])],[('At1',305,'T')]),
('A At1','a0 a1 a2 a3',[],[('At1',305,'T')]),
('Ag At1','a0 a1 a2 a3',[('A',957,'T',['Ag'])],[('At1',305,'T')]),
('A Acrc2','a0 a1 a2 a3',[],[('Acrc2',537,'CRC')]),
('A Ac2t1','a0 a1 a2 a3',[],[('Ac2t1',305,'T')]),
('A Ac2t3','a0 a1',[('A',537,'BC',[['A','Ac2t3']])],[('Ac2t3',747,'T')]),
('A Ah','',[('A',0,'HC',[['A','Ah']])],[]),
('A Ag Nt2','a0 a1 a2 a3 n0 n1',[('N',465,'T',['Nt2'])],[('Ag',957,'T')]),
('At3 N','a0 a1 a2',[('A',747,'T',['At3']),('N',0,'PU',['N'])],[]),
('At1 At3 A','a0 a1 a2 a3',[],[('At1',305,'T'),('At3',747,'T')]),
('At1 X At3','a0 a1 a2 x0 x1 x2 x3',[('A',747,'T',['At3'])],[('At1',305,'T')]),
('A A Af Ao','a0 a1 a2 a3',[],[]),
('At3 Acrc3','a0 a1 a2',[('A',747,'CRC',['Acrc3'])],[('At3',747,'T')]),
('M Mt3','m0 m2 m3',[],[('Mt3',489,'T')]),
('M Mc1t3','m0',[('M',292,'BC',[['M','Mc1t3']])],[('Mc1t3',489,'T')]),
('A A+ N','a0 a1 a2 a3',[('A',957,'SC',[['A+','N']])],[]),
('A Z','a0 a1 a2 a3',[],[('Z',0,'IH')]),
('A+ N','a0 a1 a2 a3',[('A',957,'SC',[['A+','N']])],[]),
('Ag N','a0 a1 a2 a3',[('A',957,'T',['Ag']),('N',0,'PU',['N'])],[]),
('At1 N L Lc2t3','a0',[('A',305,'T',['At1']),('N',0,'PU',['N']),('L',465,'BC',[['L','Lc2t3']])],[('Lc2t3',577,'T')]),
('Ag Ag′','a0 a1 a2 a3',[('A',957,'T',['Ag','Ag′'])],[]),
('A A+','a0 a1 a2 a3 a4',[],[]),
('A L','a0 a1 a2 a3',[('L',0,'PU',['L'])],[]),
('N','',[('N',0,'PU',['N'])],[]),
('Aft','a0 a1 a2 a3',[('A',957,'T',['Aft'])],[]),
('A Aft','a0 a1 a2 a3',[],[('Aft',957,'T')]),
('A+ Nb','a0 a1 a2 a3 a4',[('N',0,'PU',['Nb'])],[]),
('A+ Ni','a0 a1 a2 a3 a4',[('N',0,'PU',['Ni'])],[]),
('Au Auc3','a0',[('A',[305,8],'RF',['Au','Auc3'])],[(['Au','Auc3'],747,'BC')]),
('R Rc3','r0 r1 r2',[('R',769,'BC',[['R','Rc3']])],[]),
('Z','',[],[('Z',0,'IH')]),
('A A2','',[],[]),
('A N2','',[('N2',0,'PU',['N2'])],[]),
('A A2 X','x0 x1 x2 x3',[],[]),
('A A2 A2','',[],[]),
('V Q','a0 a1 a2 a3 v4 q0 q1 q2 q3',[],[]),
('V Q0','a0 a1 a2 a3 v4 q0 q1 q2 q3',[],[]),
('A N','a0 a1 a2 a3 n0 n1 n2 n3',[],[]),
('A V V Q','a0 a1 a2 a3 v4 q0 q1 q2 q3',[],[]),
('V N1','a0 a1 a2 a3',[('A',957,'SC',[['V','N1']])],[]),
('A N1','a0 a1 a2 a3 n0 n1 n2 n3',[],[]),
('A Z2','a0 a1 a2 a3',[],[('Z2',0,'IH')]),
('Vu Tu','a0 a1 a2 a3 vu4',[],[]),
('Vu T0','a0 a1 a2 a3 vu4',[],[]),
('W Qw','a0 a1 a2 a3 w4 q0 q1 q2 q3',[],[]),
('W Qw0','a0 a1 a2 a3 w4 q0 q1 q2 q3',[],[]),
('A A2t0','',[('A2',193,'T',['A2t0'])],[]),
('Wi Qwi0','ai0 ai1 ai2 ai3 wi4 q0 q1 q2 q3',[],[]),
('Wi Qwi1','ai0 ai1 ai2 ai3 wi4 q0 q1 q2 q3',[],[]),
('Vi Qvi','ai0 ai1 ai2 ai3 vi4 q0 q1 q2 q3',[],[])]

# Explicit independent admission/provenance expectations, indexed by table row.
ADMISSIONS=[{'a0':True},{'a0':True},{'a0':True},{'a0':True},{'a0':True},{'a0':True},{'a0':False},
 {'a0':True,'a1':True},{'a0':True,'a1':False},{'a0':True},{'a0':True,'x0':True},{'a0':True},{'a0':True},
 {'m0':True},{'m0':True},{'a0':True,'a1':False},{'a0':True},{'a0':True,'a1':False},
 {'a0':True,'a1':False},{'a0':True,'a1':False,'a2':False},{'a0':True},{'a0':True},{'a0':True,'a2':False},
 {'a1':False},{'a0':True},{'a0':True},{'a0':True,'a1':False},{'a0':True,'a1':False},{'a0':True},{'a0':True},{}]
PROVENANCE=[['At3'],['A'],['Ag'],['A'],['A'],['A'],[],['A','Ag','Nt2'],['At3','N'],['A'],['At3','X'],
 ['A','Af','Ao'],['At3','Acrc3'],['M'],['M'],['A+','N'],['A'],['A+','N'],['Ag','N'],['At1','N','L'],['Ag','Ag′'],
 ['A+'],['A','L'],['N'],['Aft'],['A','Aft'],['A+','Nb'],['A+','Ni'],['Au','Auc3'],['R','Rc3'],[]]
ADMISSIONS += [{'ba0':False,'ca0':False},{'ba0':False,'ca1':False},
 {'ba0':False,'ca0':False,'bx0':True},{'ba0':False,'ca0':False}] + [
 {'ba0':True,'ba1':True} for _ in range(4)] + [
 {'ba0':True,'ba1':False},{'ba0':True,'ba1':True},{'ba0':True},
 {'ba0':True,'ba1':True},{'ba0':True,'ba1':True},
 {'ba0':True,'ba1':True},{'ba0':True,'ba1':True},{'ba0':False,'ca0':False},
 {'ba0':True,'ba1':True},{'ba0':True,'ba1':True},{'ba0':True,'ba1':True}]
# Include boot in admission keys: r3b permits multiple boots for a capture ID.
for i in range(31): ADMISSIONS[i]={'b'+k:v for k,v in ADMISSIONS[i].items()}
PROVENANCE += [['A','A2'],['A','N2'],['A','A2','X'],['A','A2'],['V','Q'],['V','Q0'],['A','N'],
 ['V','Q'],['V','N1'],['A','N1'],['A'],['Vu','Tu'],['Vu','T0'],['W','Qw'],['W','Qw0'],['A','A2t0'],
 ['Wi','Qwi0'],['Wi','Qwi1'],['Vi','Qvi']]
# Row 6 maximum E is A alone, row 15 M alone: short conflicting copies still diagnose BC.

aliases={}
for src,prefix_name in [('A','a'),('N','n'),('L','l'),('X','x'),('M','m'),('R','r')]:
    for i,b in enumerate(D[src]['blocks']):
        if b['type']==1: continue
        rr=b['records'][0]
        aliases[f'{prefix_name}{i}']=dict(physical=D[src]['header']['physical']+[b['start'],rr['offset']],content_id=rr['content_id'],value=rr['value'])
b=AP['blocks'][-1]; rr=b['records'][0]; aliases['a4']=dict(physical=['b','a',0,957,8],content_id=rr['content_id'],value=rr['value'])

# The unqualified header changes stored block identities, not logical contents.
for src,prefix_name in [('Q','q'),('Wi','ai')]:
    for i,b in enumerate(D[src]['blocks'][:4]):
        rr=b['records'][0]; aliases[f'{prefix_name}{i}']=dict(physical=D[src]['header']['physical']+[b['start'],rr['offset']],content_id=rr['content_id'],value=rr['value'])
for src,alias in [('V','v4'),('Vu','vu4'),('W','w4'),('Wi','wi4'),('Vi','vi4')]:
    b=D[src]['blocks'][-1]; rr=b['records'][0]
    aliases[alias]=dict(physical=D[src]['header']['physical']+[b['start'],rr['offset']],content_id=rr['content_id'],value=rr['value'])

# Freeze replay summaries directly from the normative RA/RV/... definitions.
def replay_state(kind, extra=None):
    frontiers={'RA':[10,20],'RV':[10],'RU':[10],'RW':[10,20],'RI':[],'RVI':[],'LX':[]}[kind]
    baseline=None if kind=='RU' else B1[:] if kind in ('RV','RW','RVI') else ZERO[:]
    life=[dict(pid=i,fork=0,life=[0,None]) for i in range(10,14)] if kind=='LX' else [dict(pid=1,fork=0,life=[0,None])]
    if extra:
        life += [dict(pid=i,fork=40 if extra=='LQ' else 0,life=[40 if extra=='LQ' else 0,None]) for i in range(2,6)]
    spans=[[0,10]] if kind in ('RV','RU') else [[0,20]] if kind in ('RA','RW') else []
    projections=[dict(**entry,coverage=spans if entry['pid']==1 else [],waits=[],open=None) for entry in life]
    return dict(frontier=frontiers[-1] if frontiers else None,projections=projections,baseline=baseline,frontiers=frontiers,revoked=[20] if kind in ('RV','RU') else [],
                coverage=[[0,10]] if kind in ('RV','RU') else [[0,20]] if kind in ('RA','RW') else [],
                high_water=20 if kind in ('RA','RV','RU','RW') else None,
                lifecycle_uncertainty=30 if kind in ('RV','RU','RVI') else None,life_entries=life,stop=None)
NEW_REPLAY={17:{'a':replay_state('RA')},31:{},32:{},33:{},34:{'x':replay_state('LX')},35:{},
 36:{'a':replay_state('RV','LQ')},37:{'a':replay_state('RV')},38:{'a':replay_state('RA','LN')},
 39:{'a':replay_state('RV','LQ')},40:{'a':replay_state('RA')},41:{'a':replay_state('RA')},
 42:{'a':replay_state('RA')},43:{'a':replay_state('RU')},44:{'a':replay_state('RU')},
 45:{'a':replay_state('RW','LQ')},46:{'a':replay_state('RW')},47:{},
 48:{'a':replay_state('RI','LQ')},49:{'a':replay_state('RI')},50:{'a':replay_state('RVI','LQ')}}
MISMATCH={37:('Q0',B1,ZERO),41:('N1',ZERO,B1),44:('T0',None,ZERO),46:('Qw0',B1,ZERO),49:('Qwi1',ZERO,B1)}
for num,(name,expected,declared) in MISMATCH.items():
    NEW_REPLAY[num]['a']['stop']=dict(file=D[name]['header']['physical'],position=[0,None],
                reason='baseline_mismatch',expected=expected,declared=declared)

cases=[]
for num,(ins,rs,st,extra) in enumerate(ROWS,1):
    ambiguous=num in (32,33,34,35,47)
    agg='FAILED' if st or num in (17,31,42) or ambiguous or num in MISMATCH else 'PASS'
    caps={D[n]['header']['physical'][1] for n in ins.split() if D[n]['header']['valid']}
    status={cap:('FAILED' if (ambiguous and cap=='a') or any(D[f]['header']['physical'][1]==cap for f,_,_,_ in st) or (num in MISMATCH and cap=='a') or (num==30 and cap=='a') else 'PASS') for cap in caps}
    u=sorted(set(n for n in ins.split() if D[n]['header']['valid'] and D[n]['header']['physical'][1]=='a')) if ambiguous else []
    cases.append(dict(case=num,input=ins.split(),expected=dict(record_aliases=rs.split(),stops=st,extra_diagnostics=extra,
                    admitted=ADMISSIONS[num-1],source_deliveries=PROVENANCE[num-1],
                    failed_decode=num in (17,31,42),aggregate_status=agg,capture_status=status,ambiguity_witnesses=u,
                    replay_summaries=NEW_REPLAY.get(num),
                    replay=({'frontiers':[10],'revoked':[20],'coverage':[[0,10]],'high_water':20} if num==22 else
                    {'stop':[537,8],'reason':'overlapping_attachment','life':[[0,None]],'coverage':[]} if num==30 else None))))
Path('cases.json').write_text(json.dumps(dict(format='abstract-redelivery-v1',deliveries=D,records=aliases,cases=cases),indent=2,ensure_ascii=False)+'\n')
Path('model.json').write_text(json.dumps({'format':'abstract-redelivery-v1','deliveries':[D['V'],D['Q0'],D['X']]},indent=2)+'\n')
print('Frozen 50 text-derived table answers, admission/provenance, failure levels and revised replay cells.')
