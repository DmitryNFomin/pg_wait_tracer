import json,time,subprocess,os
from pathlib import Path
root=Path('/root/cx-matrix-regression');trace='/tmp/pgwt_cx_rehearsal_tqWPfm'
windows=[('transitions_first',1791800513880240400,1791801413880240400),('transitions_warm',1791800513880240400,1791801413880240400),('matrix_later',1791800683210846700,1791801583210846700)]
for rep in range(2):
 for arm in ['pre','aggregate','final']:
  env=dict(os.environ,PGWT_TRANSITIONS_PROVENANCE='1',PGWT_ALLOC_TUNE_DEBUG='1')
  with open(root/f'sequence-{arm}-{rep}.stderr','w') as log:
   p=subprocess.Popen([str(root/arm/'pgwt-server'),trace],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True,env=env)
   # Earlier raw tabs prime the decoded-record cache, but never the aggregate cache.
   f,t=windows[0][1:]
   st=time.perf_counter();p.stdin.write(json.dumps(dict(id=99,cmd='variants',**{'from':f,'to':t,'buckets':200,'filters':{}}))+'\n');p.stdin.flush();json.loads(p.stdout.readline())
   print(json.dumps(dict(rep=rep,arm=arm,name='raw_cache_warmup_variants',ms=round((time.perf_counter()-st)*1000,3))),flush=True)
   for i,(name,f,t) in enumerate(windows):
    st=time.perf_counter();p.stdin.write(json.dumps(dict(id=i+1,cmd='transitions',**{'from':f,'to':t,'buckets':200,'filters':{}}))+'\n');p.stdin.flush();d=json.loads(p.stdout.readline());ms=(time.perf_counter()-st)*1000
    print(json.dumps(dict(rep=rep,arm=arm,name=name,ms=round(ms,3),**{k:d.get(k) for k in ['merged_blocks','decoded_blocks','total','fidelity']})),flush=True)
   p.stdin.close();p.wait()
