#!/usr/bin/env python3
"""Real-byte REDELIVERY cases and bounded reference differential.

No expected-answer files are written. A persistent C transport uses the product
encoder for headers/record payloads and the decoder for all observed results.
Fault injection, integrity repair and abstract-to-byte identity translation live
only here. All builds and generated artifacts stay in a TemporaryDirectory.
"""
import copy
import hashlib
import itertools
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import Counter

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
REF = ROOT / 'contracts/evidence-v1/codec/redelivery-ref'
sys.path.insert(0, str(REF))
from ref_redelivery import evaluate, wire_identity, canonical
from check_cases import compare as compare_frozen
from evidence_codec_redelivery_models import multisets


def build(output, source=None):
    cmd = shlex.split(os.environ.get('CC', 'cc')) + shlex.split(os.environ.get('CFLAGS', '-g -O2'))
    cmd += ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'src'), '-I'+str(ROOT/'src/core'), '-I'+str(ROOT/'tests'),
            str(ROOT/'tests/evidence_codec_driver.c'), str(source or ROOT/'src/core/evidence_codec.c'),
            str(ROOT/'src/core/evidence_replay.c'), str(ROOT/'src/core/wait_metrics.c'),
            str(ROOT/'src/core/contract_json.c'), '-o', str(output)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)


class Driver:
    def __init__(self, path):
        self.proc = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1)
    def call(self, request):
        self.proc.stdin.write(json.dumps(request, separators=(',', ':'))+'\n')
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError('driver failed: '+self.proc.stderr.read())
        return json.loads(line)
    def close(self):
        self.proc.stdin.close()
        status = self.proc.wait(timeout=30)
        stderr = self.proc.stderr.read()
        self.proc.stdout.close(); self.proc.stderr.close()
        if status or stderr:
            raise RuntimeError(f'driver exit {status}: {stderr}')


def u(n, size):
    return n.to_bytes(size, 'little')


def leb(n):
    out = bytearray()
    while n >= 128:
        out.append((n & 127) | 128); n >>= 7
    out.append(n)
    return bytes(out)


def crc(data):
    return u(zlib.crc32(data), 4)


def digest(data, end):
    return hashlib.sha256(data[:6]+data[8:end]).digest()


def ident(data):
    return f'{len(data)}:{hashlib.sha256(data).hexdigest()}'


def hsize(data):
    return int.from_bytes(data[40:44], 'little')


class Materializer:
    """Catalog recipes are content dictionaries, not product-decoded inputs.

    Equal interned tokens must materialize identically. Every catalog offset and
    complete block span is checked before a case reaches the product decoder.
    CRC failures preserve the original following chain, as the notation requires.
    """
    def __init__(self, driver, catalog):
        self.driver = driver; self.catalog = catalog
        self.cache = {}; self.headers = {}; self.blocks = {}; self.enc_cache = {}
        self.header_recipes = {}
        for d in catalog.values():
            self.header_recipes.setdefault(d['header']['content_id'], d)
        self.block_recipes = {b['content_id']: (d, b) for d in catalog.values() for b in d['blocks']}

    def encode(self, header, records):
        h = dict(boot=header['physical'][0], capture=header['physical'][1], file_no=0,
                 qualified=header['qualified'], baseline=header['baseline'], mapping=header['mapping'],
                 flags=header['flags'], creation=header.get('creation', 0))
        request = dict(header=h, records=records)
        key = canonical(request)
        if key not in self.enc_cache:
            self.enc_cache[key] = bytes.fromhex(self.driver.call(request))
        return self.enc_cache[key]

    def header(self, h):
        # The encoder constructs a valid successor using an empty root witness;
        # test injection replaces the declared reference with the model's bytes.
        request_h = dict(boot=h['physical'][0], capture=h['physical'][1], file_no=h['physical'][2],
                         qualified=h['qualified'], baseline=h['baseline'], mapping=h['mapping'],
                         flags=h['flags'], creation=h.get('creation', 0))
        request = dict(header=request_h, records=[])
        if request_h['file_no']:
            previous = self.encode(h, []).hex()
            for ordinal in range(1, request_h['file_no']):
                ph = dict(request_h, file_no=ordinal)
                previous = self.driver.call(dict(header=ph, records=[], previous=previous))
            request['previous'] = previous
        data = bytearray.fromhex(self.driver.call(request))
        if h['previous'] is not None:
            start = 48+2+len(h['physical'][0].encode())+2+len(h['physical'][1].encode())+8+1
            prev = h['previous']; dg = prev['digest']
            if dg['kind'] == 'prefix':
                wanted = [b[2] for b in dg['blocks']]
                if dg.get('partial'): wanted.append(dg['partial'][0])
                recipes = [d for d in self.catalog.values() if d['header']['content_id']==dg['header'] and
                           [b['content_id'] for b in d['blocks'][:len(wanted)]]==wanted and
                           (d['blocks'][-1]['end'] if d['blocks'] else d['header']['H'])>=dg['D']]
                assert recipes, ('missing digest recipe', dg)
                base = self.materialize(recipes[0])
                sha = digest(base, dg['D'])
            else:
                # The wrong-digest fixture flips the first bit of A's digest.
                base = self.materialize(self.catalog['A']); sha = digest(base, prev['D'])
                sha = bytes([sha[0]^1])+sha[1:]
            data[start:start+48] = u(prev['file_no'],8)+u(prev['D'],8)+sha
        data[-4:] = crc(data[:6]+data[8:-4])
        assert len(data) == h['H'], ('header layout', len(data), h['H'])
        immutable = bytes(data[:6]+data[8:])
        previous = self.headers.setdefault(h['content_id'], immutable)
        assert previous == immutable, ('inconsistent header token', h['content_id'])
        return bytes(data)

    def payload(self, h, b):
        values = [copy.deepcopy(r['value']) for r in b['records']]
        if b['type'] == 1:
            # U7a writes exact evidence only. Convert its TRANS-shaped column
            # template into the contract's sample columns (test-only injection).
            template = [dict(kind='TRANS', t=v['t'], pid=max(v['pid'],1), old=None,
                             new=v.get('event'), duration=0, query_id=v.get('query_id',0), cpu_ns=0) for v in values]
            self.encode(h, template)
            events = {name: mid for mid, role, name in h['mapping'] if role == 1}
            prev=0; times=[]
            for v in values:
                times.append(leb((v['t']-prev) % 2**64)); prev=v['t']
            return (b''.join(times)+b''.join(u(v['pid'],4) for v in values)+
                    b''.join(u(events.get(v.get('event'),0),4) for v in values)+
                    b''.join(u(v.get('query_id',0),8) for v in values))
        templates = copy.deepcopy(values)
        for v in templates:
            if not isinstance(v['kind'], str): v['kind']='ATTACH_OK'
            if v['kind']=='ATTACH_OK' and v.get('method') not in ('reread','transition'): v['method']='reread'
            if v['kind'] in ('ATTACH_OK','FRONTIER') and v['counters'] is None: v['counters']=[0]*15
            if v['kind']=='TRANS' and not v['pid']: v['pid']=1
            if v['kind']=='MARKER':
                t=v['t'];v.clear(); v.update(kind='TRANS',t=t,pid=1,old=None,new='w',duration=0)
        data = self.encode(h, templates); H=hsize(data)
        payload = bytearray(data[H+64:-4])
        if b['type']==0:
            prev=0;width=0
            for v in values: width+=len(leb((v['t']-prev)%2**64));prev=v['t']
            for i,v in enumerate(values):
                if v['kind']=='MARKER':
                    mid=next(mid for mid,role,name in h['mapping'] if role==2 and name==v['name'])
                    payload[width+4*i:width+4*i+4]=u(0,4)
                    payload[width+4*len(values)+4*i:width+4*len(values)+4*i+4]=u(mid,4)
                    payload[width+8*len(values)+4*i:width+8*len(values)+4*i+4]=u(mid,4)
                elif v['pid']==0: payload[width+4*i:width+4*i+4]=u(0,4)
        if b['type']==2:
            offset=8
            for r, v in zip(b['records'],values):
                assert offset == r['offset'], ('record layout', offset, r['offset'])
                length = int.from_bytes(payload[offset+1:offset+5], 'little')
                fault=r.get('fault')
                if not isinstance(v['kind'], str): payload[offset]=v['kind']
                elif v['kind']=='ATTACH_OK' and v.get('method') not in ('reread','transition'):
                    payload[offset+5+8+4+1+(8 if v.get('fork') is not None else 0)] = v['method']
                if v['kind'] in ('ATTACH_OK','FRONTIER') and v['counters'] is None:
                    counter = offset+5+8
                    if v['kind']=='ATTACH_OK': counter += 4+1+(8 if v.get('fork') is not None else 0)+1+4+4
                    payload[counter:counter+121]=b'\0'; length-=120
                    payload[offset+1:offset+5]=u(length,4)
                if fault and fault['kind']=='bad_boolean':
                    payload[offset+5+8+4] = 2
                offset += 5+length
        return bytes(payload)

    def materialize(self, d):
        key = canonical(d)
        if key in self.cache: return self.cache[key]
        h=d['header']; data=bytearray(self.header(h)); preceding=data[-4:]; index=[]
        for b in d['blocks']:
            assert len(data)==b['start'], ('block start',len(data),b['start'])
            payload=self.payload(h,b); stored=payload
            if b['codec']==1:
                # An all-literal raw LZ4 stream, no external compression library.
                n=len(payload); stored=bytes([min(n,15)<<4])
                if n>=15:
                    q=n-15
                    while q>=255: stored+=b'\xff';q-=255
                    stored+=bytes([q])
                stored+=payload
            values=[r['value'] for r in b['records']]
            first=values[0]['t'] if b['type']!=2 else 0
            last=values[-1]['t'] if b['type']!=2 else 0
            head=(u(first,8)+u(last,8)+u(len(values),4)+u(len(stored),4)+u(len(payload),4)+
                  u(b['type'],2)+u(0,2)+u(b.get('sample_period',0),8)+u(b.get('max_weight',0),8)+
                  u(len(data),8)+preceding+u(b['codec'],1)+b'\0'*3)
            unit=head+stored; checksum=crc(unit); unit+=checksum
            checks=b.get('checks',{})
            if not checks.get('crc_valid',True): unit=unit[:-4]+bytes([unit[-4]^1])+unit[-3:]
            # Independent test recipes for whole-block faults. Apply after
            # saving the original checksum so unrepaired later bytes remain
            # exactly the original chain, as stipulated by the abstract model.
            if not checks.get('header_fields_valid', True):
                unit=unit[:48]+u(len(data)+1,8)+unit[56:]
            elif not checks.get('safely_bounded', True):
                unit=unit[:16]+u(4097,4)+unit[20:]
            elif not checks.get('bytes_complete', True):
                total=(d['blocks'][-1]['end'] if d['blocks'] else h['H'])+d['tail']['length']
                unit=unit[:20]+u(total-len(data)-64+1,4)+unit[24:]
            elif not checks.get('decode_valid', True) and checks.get('crc_valid',True):
                damaged=bytearray(unit[:-4]);damaged[64]^=1
                unit=bytes(damaged)+crc(damaged)
            assert len(data)+len(unit)==b['end'], ('block end',len(data)+len(unit),b['end'])
            if b['content_id'] in self.blocks: assert self.blocks[b['content_id']]==unit, ('inconsistent block token',b['content_id'])
            else: self.blocks[b['content_id']]=unit
            index.append((first,len(data)));data+=unit;preceding=checksum
        tail=d['tail']; ending=b''
        footer=b''.join(u(t,8)+u(off,8) for t,off in index)+u(len(index),4)
        if tail['kind']=='footer': ending=footer
        elif tail['length']:
            cid=tail['content_id']
            if len(cid)==2: ending=bytes.fromhex(cid)
            elif cid=='footer-first17': ending=footer[:17]
            elif cid.startswith('A-unit-prefix:'):
                _,start,n=cid.split(':');base=self.materialize(self.catalog['A']);ending=base[int(start):int(start)+int(n)]
            elif cid.startswith('A-footer-prefix:'): ending=footer[:int(cid.rsplit(':',1)[1])]
            elif cid=='footer-first-timestamp-bit-flipped': ending=bytes([footer[0]^1])+footer[1:]
            else: raise AssertionError(('unmaterialized tail',cid))
        assert len(ending)==tail['length'], ('tail length',tail)
        data+=ending
        if not h['valid']:
            raw=h.get('raw_content_id','')
            if 'header-prefix:' in raw: data=data[:h.get('raw_length',h['H'])]
            else: data[h['H']-4]^=1
        self.cache[key]=bytes(data)
        return bytes(data)


def normalized_reference(ds, materializer):
    expected=evaluate(ds)
    ids={wire_identity(d):ident(materializer.materialize(d)) for d in ds}
    def relabel(x):
        if isinstance(x,str): return ids.get(x,x)
        if isinstance(x,list): return [relabel(v) for v in x]
        if isinstance(x,dict): return {k:relabel(v) for k,v in x.items()}
        return x
    expected=relabel(expected)
    expected['records']=[{k:v for k,v in r.items() if k!='content_id'} for r in expected['records']]
    for a in expected['admissions']:
        for l in a['links']:
            h=next(d['header'] for d in ds if ident(materializer.materialize(d))==l['delivery'])
            prev=h['previous']; l['D']=0 if prev is None else prev['D']
            data=materializer.materialize(next(d for d in ds if ident(materializer.materialize(d))==l['delivery']))
            off=48+2+len(h['physical'][0].encode())+2+len(h['physical'][1].encode())+8+1
            l['digest']=('00'*32) if prev is None else data[off+16:off+48].hex()
            l.pop('witnesses');l.pop('seal_pairs')
        a.pop('missing_files');a.pop('record_count')
    captures=[]
    for c in expected['capture_results']:
        witnesses=sorted({ident(materializer.materialize(d)) for d in ds if d['header']['valid'] and d['header']['physical'][1]==c['capture']})
        replay=c['replay']
        if replay is not None:
            replay={k:replay[k] for k in ('frontier','high_water','baseline','lifecycle_uncertainty','frontiers','revoked','stop','projections')}
            replay['projections']=[dict(pid=p['pid'],fork=p['fork'],projection={k:p[k] for k in ('life','coverage','waits','open')}) for p in replay['projections'] if p['life'] is not None]
        captures.append(dict(capture=c['capture'],status=c['status'],ambiguous=c['ambiguity'] is not None,boots=c['boots'],witnesses=witnesses,replay=replay))
    diagnostics={}
    for d in expected['diagnostics']:
        d={k:d[k] for k in ('file','position','reason','witnesses')}; key=canonical([d['file'],d['position'],d['reason']])
        if key not in diagnostics: diagnostics[key]=d
        else: diagnostics[key]['witnesses']+=d['witnesses']
    result={k:expected[k] for k in ('records','stops','admissions','aggregate_status','unassigned_header_failure')}
    result['diagnostics']=list(diagnostics.values());result['captures']=captures
    return canonicalize(result)


def canonicalize(result):
    r=copy.deepcopy(result)
    for field in ('stops','diagnostics'):
        for s in r[field]:
            if s['reason'] in ('HC','BC'): s['witnesses']=[sorted(w) for w in s['witnesses']]
            s['witnesses']=sorted({canonical(w):w for w in s['witnesses']}.values(),key=canonical)
        r[field].sort(key=canonical)
    for a in r['admissions']:
        a['sources'].sort()
        for l in a['links']: l['checks'].sort(key=canonical)
        a['links'].sort(key=canonical)
    r['admissions'].sort(key=canonical)
    for c in r['captures']:
        c['boots'].sort();c['witnesses'].sort()
        if c['replay'] is not None: c['replay']['projections'].sort(key=canonical)
    r['captures'].sort(key=canonical)
    return r


def first_difference(a,b,path=''):
    if type(a)!=type(b): return path,a,b
    if isinstance(a,dict):
        if a.keys()!=b.keys(): return path+'.keys',sorted(a),sorted(b)
        for k in a:
            d=first_difference(a[k],b[k],path+'.'+k)
            if d: return d
    elif isinstance(a,list):
        if len(a)!=len(b): return path+'.length',len(a),len(b)
        for i,(x,y) in enumerate(zip(a,b)):
            d=first_difference(x,y,path+f'[{i}]')
            if d: return d
    elif a!=b: return path,a,b
    return None


def supplemental(catalog):
    """Concrete realizable recipes supplement the exported symbolic pools.

    Repaired mutations change every dependent integrity identity. Literal byte
    cuts retain original identities. Counts/spans are declared before decode.
    """
    D=catalog
    flag=copy.deepcopy(D['A+']);flag['header']['flags']=1
    nflag=copy.deepcopy(D['N']);nflag['header']['flags']=1
    short=copy.deepcopy(D['N']);short['header']['previous'].update(D=747,digest=dict(kind='prefix',header=D['A']['header']['content_id'],D=747,
        blocks=[[b['start'],b['end'],b['content_id']] for b in D['A']['blocks'][:3]],partial=None))
    short['header']['content_id']+=':claim747'
    for b in short['blocks']: b['content_id']+=':claim747'
    empty=copy.deepcopy(D['A']);empty['blocks']=[]
    successor=copy.deepcopy(D['N']);successor['header']['previous'].update(D=193,digest=dict(kind='prefix',header=D['A']['header']['content_id'],D=193,blocks=[],partial=None))
    successor['header']['content_id']+=':empty'
    for b in successor['blocks']: b['content_id']+=':empty'
    dirty=copy.deepcopy(empty);dirty['tail']=dict(kind='rejected',length=1,content_id='00',checks=dict(header_complete=False))
    badfooter=copy.deepcopy(D['Ao']);badfooter['tail'].update(kind='rejected',content_id='footer-first-timestamp-bit-flipped',checks=dict(header_fields_valid=False))
    flag2=copy.deepcopy(D['A2']);flag2['header']['flags']=1
    footer2=copy.deepcopy(D['A2']);footer2['tail']=copy.deepcopy(D['Ao']['tail'])
    dirty2=copy.deepcopy(D['A2']);dirty2['tail']=copy.deepcopy(D['Ag']['tail'])
    alphabets=[('multiple_seal_claims',[D['A+'],flag,D['N'],nflag,short]),
               ('empty_predecessor',[empty,dirty,successor,D['A']]),
               ('malformed_footer',[D['A'],badfooter,D['N']]),
               ('ambiguity_witness_ties',[D['A'],D['A2'],flag2,footer2,dirty2,D['N2']])]
    for tag,alphabet in alphabets:
        for size in range(5):
            for ds in itertools.combinations_with_replacement(alphabet,size): yield tag,list(ds)
    # Six complete blocks make an exact footer longer than a small rejected
    # sample block. The dirty copy sorts first among equal-E source carriers.
    # A clean footer suppresses its terminal stop, so atomic publication must
    # hold independently of the later capture-cutoff guard.
    many=dict(header=copy.deepcopy(D['M']['header']),blocks=[],tail=dict(kind='footer',length=100,content_id='six-block-exact-footer'))
    at=many['header']['H']
    for i in range(6):
        row=copy.deepcopy(D['M']['blocks'][2]['records'][0])
        many['blocks'].append(dict(start=at,end=at+112,type=2,codec=0,checks={},records=[row],content_id=f'six-life-prefix:{i}'))
        at+=112
    dirty_many=copy.deepcopy(many);dirty_many['tail']=dict(kind='eof',length=0,content_id='')
    sample=copy.deepcopy(D['M']['blocks'][1]);sample.update(start=at,end=at+85,content_id='six-life-bad-crc-sample',checks=dict(crc_valid=False))
    dirty_many['blocks'].append(sample)
    yield 'suppressed_bad_block_atomic',[many,dirty_many]
    yield 'same_block_fault_conflict',[D['A'],D['Au']]
    yield 'bc_sc_tie',[D['A+'],D['V'],D['N']]
    yield 'hc_pu_tie',[D['N'],D['N1']]
    yield 'baseline_failure_independent_capture',[D['V'],D['Q0'],D['X']]
    yield 'nonadjacent_conflict_pairs',[D['At1'],D['N'],D['L'],D['Lc2t3'],D['X']]
    for slot in range(15):
        n=copy.deepcopy(D['N']);n['header']['baseline'][slot]=1;n['header']['content_id']+=f':baseline{slot}'
        for b in n['blocks']: b['content_id']+=f':baseline{slot}'
        yield f'baseline_slot_{slot}',[D['A'],n]
    malformed=copy.deepcopy(D['A']);malformed['header']['qualified']=False;malformed['header']['content_id']+=':ignored-malformed'
    for b in malformed['blocks']: b['content_id']+=':ignored-malformed'
    r=malformed['blocks'][1]['records'][0];r['value']['method']=255;r['fault']={'kind':'malformed_method'};r['content_id']+=':bad-method'
    yield 'ignored_malformed_certificate',[malformed]
    # Same stored rows in different compression and lengths must conflict.
    compressed=copy.deepcopy(D['M']);compressed['blocks'][0].update(codec=1,end=294,content_id='M:literal-lz4')
    for b in compressed['blocks'][1:]: b['start']+=2;b['end']+=2;b['content_id']+=':lz4-offsets'
    yield 'raw_lz4_conflict',[D['M'],compressed]
    multi=copy.deepcopy(D['A']);row=copy.deepcopy(multi['blocks'][0]['records'][0]);row['offset']=44
    multi['blocks'][0]['records'].append(row);multi['blocks'][0]['end']+=36
    for b in multi['blocks'][1:]: b['start']+=36;b['end']+=36
    for b in multi['blocks']: b['content_id']+=':two-life-rows'
    yield 'distinct_equal_records',[multi]
    fault=copy.deepcopy(multi);fault['blocks'][0]['records'][1]['fault']={'kind':'bad_boolean'};fault['blocks'][0]['records'][1]['content_id']+=':bad-boolean'
    for b in fault['blocks']: b['content_id']+=':bad-boolean'
    yield 'fault_inside_block',[fault]
    variants=[]
    for label,checks in [('bounds',dict(header_fields_valid=False,bytes_complete=False,crc_valid=False)),
                         ('truncated_payload',dict(bytes_complete=False,crc_valid=False)),
                         ('crc_before_decode',dict(crc_valid=False,decode_valid=False)),
                         ('decode_schema',dict(decode_valid=False))]:
        bad=copy.deepcopy(D['A']);bad['blocks'][2]['checks']=checks;bad['blocks'][2]['content_id']+=':'+label
        variants.append(bad)
        yield label,[bad]
        yield label+'_clean_alternative',[D['A'],bad]
    for ds in itertools.combinations_with_replacement(variants,3): yield 'terminal_reason_ties',list(ds)
    marker=copy.deepcopy(D['M']);marker['header']['mapping'].append([2,2,'admin']);marker['header']['H']+=12;marker['header']['content_id']+=':marker-map'
    for b in marker['blocks']: b['start']+=12;b['end']+=12;b['content_id']+=':marker-map'
    marker['blocks'][0]['records'][0]['value']=dict(kind='MARKER',t=1,name='admin');marker['blocks'][0]['records'][0]['content_id']+=':marker'
    yield 'marker_and_sample_offsets',[marker]
    marker_rows=copy.deepcopy(marker)
    extra=copy.deepcopy(D['M']['blocks'][0]['records'][0]);extra['offset']=1
    marker_rows['blocks'][0]['records'].append(extra);marker_rows['blocks'][0]['end']+=23
    for b in marker_rows['blocks'][1:]: b['start']+=23;b['end']+=23
    for b in marker_rows['blocks']: b['content_id']+=':same-block-marker'
    yield 'same_block_marker_offset',[marker_rows]
    badpid=copy.deepcopy(D['M']);badpid['blocks'][0]['records'][0]['value']['pid']=0;badpid['blocks'][0]['records'][0]['content_id']+=':bad-pid'
    badpid['blocks'][0]['records'][0]['fault']={'kind':'bad_pid'}
    for b in badpid['blocks']: b['content_id']+=':bad-trans-pid'
    yield 'zero_payload_offset_fault',[badpid]
    zero=copy.deepcopy(D['M']);zero['blocks'][0]['records'][0]['value'].update(t=0,duration=0,old='w',new=None);zero['blocks'][0]['records'][0]['content_id']+=':zero-wait'
    for b in zero['blocks']: b['content_id']+=':zero-wait'
    yield 'zero_duration_wait',[zero]
    repeated=copy.deepcopy(zero);row=copy.deepcopy(repeated['blocks'][0]['records'][0]);row['offset']=1
    repeated['blocks'][0]['records'].append(row);repeated['blocks'][0]['end']+=23
    for b in repeated['blocks'][1:]: b['start']+=23;b['end']+=23
    for b in repeated['blocks']: b['content_id']+=':repeated-zero-wait'
    yield 'identical_zero_waits_distinct_positions',[repeated]
    null=copy.deepcopy(D['A']);r=null['blocks'][1]['records'][0];r['value']['counters']=None;r['fault']={'kind':'null_counters'};r['content_id']+=':null-counters'
    null['blocks'][1]['end']-=120
    for b in null['blocks'][2:]: b['start']-=120;b['end']-=120
    for b in null['blocks'][1:]: b['content_id']+=':null-counter'
    yield 'null_counter_loss',[null]
    loss=copy.deepcopy(D['A']);loss['blocks'][1]['records'][0]['value']['counters'][4]=1;loss['blocks'][1]['records'][0]['content_id']+=':counter-loss'
    for b in loss['blocks'][1:]: b['content_id']+=':counter-loss'
    yield 'readable_counter_loss',[loss]
    delayed=copy.deepcopy(D['A']);a=delayed['blocks'][0]['records'][0];b=delayed['blocks'][1]['records'][0]
    a['value'],b['value']=b['value'],a['value'];a['content_id'],b['content_id']=b['content_id'],a['content_id'];delayed['blocks'][0]['end']+=120;delayed['blocks'][1]['start']+=120
    for b in delayed['blocks']: b['content_id']+=':delayed-life'
    yield 'delayed_life',[delayed]
    # Every absolute byte cut of A+exact-footer, both alone and with clean A.
    for cut in range(1026):
        d=copy.deepcopy(D['A']);h=d['header'];reason=None
        if cut<h['H']:
            h.update(valid=False,raw_length=cut,raw_content_id=f'A-header-prefix:{cut}');d['blocks']=[]
        else:
            d['blocks']=[b for b in d['blocks'] if b['end']<=cut]
            end=d['blocks'][-1]['end'] if d['blocks'] else h['H']
            n=cut-end
            # A's first four zero hint bytes exactly encode the empty footer.
            # This real-byte alias is legal under clause 6. The abstract
            # generator's rejected-tail annotation is not a parser oracle.
            if cut==197:
                d['tail']=dict(kind='footer',length=4,content_id='exact-empty-footer')
            elif n and cut<=957:
                d['tail']=dict(kind='rejected',length=n,content_id='00' if n==1 else f'A-unit-prefix:{end}:{n}',checks=dict(header_complete=n>=64,bytes_complete=False))
            elif cut>957:
                n=cut-957
                if n==68: d=copy.deepcopy(D['Ao'])
                else: d['tail']=dict(kind='rejected',length=n,content_id='00' if n==1 else 'footer-first17' if n==17 else f'A-footer-prefix:{n}',checks=dict(header_complete=n>=64,header_fields_valid=False))
        yield 'every_byte_cut',[d]
        yield 'cut_with_clean_alternative',[D['A'],d]


def run(binary, table_only=False):
    db=json.loads((REF/'cases.json').read_text());driver=Driver(binary);m=Materializer(driver,db['deliveries']);counts=Counter()
    expected_cache={}; seen=set()
    def fresh(ds):
        key=tuple(sorted(wire_identity(d) for d in ds))
        if key not in expected_cache: expected_cache[key]=normalized_reference(ds,m)
        return expected_cache[key]
    def decode(ds):
        return canonicalize(driver.call(dict(buffers=[m.materialize(d).hex() for d in ds])))
    def compare(ds, tag):
        expected=fresh(ds);actual=decode(ds)
        difference=first_difference(expected,actual)
        assert difference is None, (tag,difference)
        return actual
    def exercise(ds, tag):
        ids=tuple(sorted(wire_identity(d) for d in ds))
        if ids in seen: return
        seen.add(ids); counts['multisets']+=1
        lookup={wire_identity(d):d for d in ds}; result=compare(ds,tag)
        for permutation in sorted(set(itertools.permutations(ids))):
            ordered=[lookup[i] for i in permutation]
            compare(ordered,(tag,'permutation',permutation));counts['permutations']+=1
            for k in range(len(ordered)+1):
                compare(ordered[:k],(tag,'incremental',permutation[:k]));counts['incremental_prefixes']+=1
        for ident in sorted(set(ids)):
            shorter=list(ids);shorter.remove(ident);previous=compare([lookup[i] for i in shorter],(tag,'edge',ident))
            counts['monotonicity_edges']+=1
            old={canonical(r):r for r in previous['records']};new={canonical(r) for r in result['records']}
            oldconflicts={canonical(d) for d in previous['diagnostics'] if d['reason'] in ('HC','BC','SC')}
            added=[d for d in result['diagnostics'] if d['reason'] in ('HC','BC','SC') and canonical(d) not in oldconflicts]
            ambiguities={c['capture'] for c in result['captures'] if c['ambiguous']}-{c['capture'] for c in previous['captures'] if c['ambiguous']}
            for key in old.keys()-new:
                p=old[key]['physical']
                assert p[1] in ambiguities or any(d['file'][:2]==p[:2] and
                    (d['file'][2],d['position'][0],-1 if d['position'][1] is None else d['position'][1])<=tuple(p[2:]) for d in added), ('monotonicity',tag,p)
            if ambiguities: counts['ambiguity_edges']+=1
            if ident in shorter:
                assert result==previous, ('duplicate idempotence',tag);counts['duplicate_edges']+=1
    try:
        for d in db['deliveries'].values(): m.materialize(d)
        for c in db['cases']:
            ds=[db['deliveries'][n] for n in c['input']]
            assert not compare_frozen(db,c,evaluate(ds)), ('reference differs from frozen case',c['case'])
            for order in sorted(set(itertools.permutations(c['input']))):
                compare([db['deliveries'][n] for n in order],('case',c['case'],order))
                counts['case_permutations']+=1
                accumulated=[]
                for k in range(len(order)+1):
                    if k: accumulated.append(db['deliveries'][order[k-1]])
                    compare(accumulated,('case incremental',c['case'],order[:k]))
                    counts['case_incremental_prefixes']+=1
            counts['cases']+=1
        print(f"codec redelivery cases {counts['cases']}/50; permutations {counts['case_permutations']}; incremental prefixes {counts['case_incremental_prefixes']}; 0 disagreements",flush=True)
        if not table_only:
            for tag,names in multisets(REF): exercise([db['deliveries'][n] for n in names],tag)
            for tag,ds in supplemental(db['deliveries']):
                for d in ds:
                    m.catalog.setdefault(canonical(d),d)
                    m.header_recipes.setdefault(d['header']['content_id'],d)
                exercise(ds,tag)
                if tag=='every_byte_cut': counts['every_cut']+=1
            print('codec redelivery differential '+json.dumps(dict(counts),sort_keys=True)+'; 0 disagreements',flush=True)
    finally: driver.close()
    return counts


def probe(binary, name):
    db=json.loads((REF/'cases.json').read_text());driver=Driver(binary);m=Materializer(driver,db['deliveries'])
    try:
        if name.startswith('case_'):
            c=next(c for c in db['cases'] if c['case']==int(name[5:]))
            ds=[db['deliveries'][n] for n in c['input']]
        else:
            ds=next(ds for tag,ds in supplemental(db['deliveries']) if tag==name)
        for d in ds:
            m.catalog.setdefault(canonical(d),d);m.header_recipes.setdefault(d['header']['content_id'],d)
        expected=normalized_reference(ds,m)
        actual=canonicalize(driver.call(dict(buffers=[m.materialize(d).hex() for d in ds])))
        diff=first_difference(expected,actual)
        print(f"codec redelivery_{name} {'FAIL assertion' if diff else 'PASS'}",flush=True)
        if diff: print('independent field: '+repr(diff),flush=True)
        return 1 if diff else 0
    finally: driver.close()


def main():
    if len(sys.argv)==4 and sys.argv[1]=='--probe':
        return probe(Path(sys.argv[2]),sys.argv[3])
    with tempfile.TemporaryDirectory(prefix='pgwt-codec-redelivery-') as directory:
        binary=Path(directory)/'driver';build(binary);run(binary)


if __name__=='__main__': raise SystemExit(main())
