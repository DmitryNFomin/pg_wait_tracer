import subprocess,json,time,os,sys,hashlib
from pathlib import Path
root=Path('/root/cx-matrix-regression')
trace=sys.argv[1] if len(sys.argv)>1 else '/root/paint1_trace_r2_copy'
results=[]
for rep in range(2):
 for arm in (['aggregate','final','oldsort','untuned','pre'] if rep==0 else ['pre','untuned','oldsort','final','aggregate']):
  env=os.environ.copy();env['PGWT_TRANSITIONS_PROVENANCE']='1';env['PGWT_ALLOC_TUNE_DEBUG']='1'
  if arm=='untuned':env['PGWT_ALLOC_TUNE']='0'
  log=open(root/f'{Path(trace).name}-{arm}-{rep}.stderr','w')
  p=subprocess.Popen([str(root/('final' if arm=='untuned' else arm)/'pgwt-server'),trace],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True,env=env)
  seq=0
  def ask(cmd,**kw):
   global seq
   seq+=1; t=time.perf_counter();p.stdin.write(json.dumps(dict(id=seq,cmd=cmd,**kw))+'\n');p.stdin.flush();d=json.loads(p.stdout.readline());return d,(time.perf_counter()-t)*1000
  info,_=ask('info');end=info['to_ns'];start=max(info['from_ns'],end-900_000_000_000)
  vals=[];can=[];meta=[]
  for i in range(7):
   d,ms=ask('transitions',**{'from':start,'to':end,'buckets':200,'filters':{}})
   vals.append(round(ms,3));meta.append({k:d.get(k) for k in ['fidelity','total','merged_blocks','decoded_blocks','error']})
   if i in [0,6]:
    c,cm=ask('time_model',**{'from':end-5_000_000_000,'to':end});can.append(round(cm,3))
  p.stdin.close();p.wait(timeout=30);log.close()
  row=dict(arm=arm,round=rep,trace=trace,info=info,start=start,end=end,cold_ms=vals[0],warm_ms=vals[1:],canary_5s_ms=can,provenance=meta,hash=hashlib.sha256(json.dumps({k:v for k,v in d.items() if k not in ['id','merged_blocks','decoded_blocks']},sort_keys=True).encode()).hexdigest())
  results.append(row);print(json.dumps(row),flush=True)
(root/f'{Path(trace).name}-results.json').write_text(json.dumps(results,indent=2))
