#!/usr/bin/env python3
"""Observe Vulkan wall-frame cadence and RSS through the reference capture host.

This is a local macOS diagnostic, not a GPU timestamp or allocation profiler.
The caller supplies SC/exposure environment settings; each output path is new.
"""
import argparse,json,os,re,signal,subprocess,sys,time,statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--frame',type=int,default=350);p.add_argument('--timeout',type=int,default=900);a=p.parse_args()
root=Path(a.out).resolve();root.mkdir(exist_ok=False)
log=None;at=0;pending="";events=[];rss=[];start=time.monotonic();last_ps=0
with (root/'driver.log').open('w') as driver:
 proc=subprocess.Popen([sys.executable,str(Path(__file__).resolve().with_name('sponza_reference.py')),'vulkan','--frame',str(a.frame),'--min-runtime','65','--timeout',str(a.timeout),'--output',str(root/'captures')],cwd=Path(__file__).resolve().parents[1],stdout=driver,stderr=subprocess.STDOUT,start_new_session=True)
 try:
  while True:
   now=time.monotonic()
   if log is None:
    candidates=list((root/'captures').glob('vulkan-*/render.log'))
    if candidates:assert len(candidates)==1;log=candidates[0]
   if log is not None:
    with log.open() as f:f.seek(at);chunk=f.read();at=f.tell()
    lines=(pending+chunk).split("\n");pending=lines.pop()
    for line in lines:
     m=re.search(r'\[CullDiag\] frame=(\d+) inst=',line)
     if m:events.append({'frame':int(m[1]),'seconds':now-start})
   if now-last_ps>=2:
    last_ps=now
    output=subprocess.run(['ps','-axo','pid=,ppid=,rss=,comm='],capture_output=True,text=True,check=True).stdout
    descendants={proc.pid};records=[]
    for line in output.splitlines():
     pieces=line.strip().split(maxsplit=3)
     if len(pieces)==4:records.append((int(pieces[0]),int(pieces[1]),int(pieces[2]),pieces[3]))
    for _ in range(3):descendants.update(pid for pid,ppid,_,_ in records if ppid in descendants)
    for pid,_,memory,command in records:
     if pid in descendants and command.endswith('TestVulkanSponzaRenderGraph'):rss.append({'seconds':now-start,'rss_kib':memory,'last_frame':events[-1]['frame'] if events else None})
   code=proc.poll()
   if code is not None:break
   time.sleep(.05)
 finally:
  try:os.killpg(proc.pid,signal.SIGTERM)
  except ProcessLookupError:pass
  if proc.poll() is None:
   try:proc.wait(timeout=15)
   except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
intervals=[{'from':x['frame'],'to':y['frame'],'ms_per_frame':(y['seconds']-x['seconds'])*1000/(y['frame']-x['frame'])} for x,y in zip(events,events[1:]) if y['frame']>x['frame']]
steady=[r['ms_per_frame'] for r in intervals if r['from']>=151 and r['to']<=331]
result={'scope':'observed wall frame cadence (50ms log polling), not GPU timestamp; RSS sampled every 2s, not allocation accounting','exit_code':code,'elapsed_seconds':time.monotonic()-start,'poll_ms':50,'steady_range':[151,331],'steady_ms_per_frame_median':statistics.median(steady) if steady else None,'steady_ms_per_frame_samples':steady,'events':events,'intervals':intervals,'rss':rss}
(root/'observation.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k not in ('events','intervals','rss')},indent=2))
raise SystemExit(code)
