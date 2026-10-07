#!/usr/bin/env python3
"""Bounded Metal System Trace attached to the actual Vulkan capture child.
Profiler run only: its cadence is not an unprofiled performance comparison.
"""
import argparse,json,os,re,signal,subprocess,sys,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True);p.add_argument('--frame',type=int,default=350);p.add_argument('--start-frame',type=int,default=151);p.add_argument('--duration',type=int,default=20);p.add_argument('--timeout',type=int,default=900);a=p.parse_args()
assert 1<=a.start_frame<a.frame and 1<=a.duration<=60
root=Path(a.out).resolve();root.mkdir(exist_ok=False);repo=Path(__file__).resolve().parents[1]
trace=None;trace_file=None;attached=None;attached_frame=None;last=0;start=time.monotonic()
with (root/'capture-driver.log').open('w') as capture_log,(root/'trace-driver.log').open('w') as trace_log:
 capture=subprocess.Popen([sys.executable,str(repo/'scripts/sponza_reference.py'),'vulkan','--frame',str(a.frame),'--min-runtime','65','--timeout',str(a.timeout),'--output',str(root/'captures')],cwd=repo,stdout=capture_log,stderr=subprocess.STDOUT,start_new_session=True)
 try:
  while capture.poll() is None:
   paths=list((root/'captures').glob('vulkan-*/render.log'))
   if paths:
    assert len(paths)==1;rows=re.findall(r'\[CullDiag\] frame=(\d+)',paths[0].read_text());last=int(rows[-1]) if rows else 0
   if trace is None and last>=a.start_frame:
    output=subprocess.run(['ps','-axo','pid=,ppid=,comm='],capture_output=True,text=True,check=True).stdout
    records=[]
    for line in output.splitlines():
     row=line.strip().split(maxsplit=2)
     if len(row)==3:records.append((int(row[0]),int(row[1]),row[2]))
    descendants={capture.pid}
    for _ in range(4):descendants.update(pid for pid,parent,_ in records if parent in descendants)
    targets=[pid for pid,_,name in records if pid in descendants and name.endswith('/TestVulkanSponzaRenderGraph')]
    assert len(targets)==1,'cannot identify unique native capture child'
    attached=targets[0];attached_frame=last;trace_file=root/'metal-system.trace'
    trace=subprocess.Popen(['xcrun','xctrace','record','--template','Metal System Trace','--attach',str(attached),'--time-limit',str(a.duration)+'s','--output',str(trace_file),'--no-prompt'],stdout=trace_log,stderr=subprocess.STDOUT,start_new_session=True)
    print(json.dumps(dict(event='attach_requested',pid=attached,observed_frame=last,path=str(trace_file))),flush=True)
   if time.monotonic()-start>a.timeout:raise TimeoutError('capture timeout')
   time.sleep(.2)
  capture_code=capture.returncode
  if trace is not None:
   try:trace_code=trace.wait(timeout=30)
   except subprocess.TimeoutExpired:os.killpg(trace.pid,signal.SIGTERM);trace_code=trace.wait(timeout=15)
  else:trace_code=None
 finally:
  for proc in [trace,capture]:
   if proc is not None and proc.poll() is None:
    os.killpg(proc.pid,signal.SIGTERM)
    try:proc.wait(timeout=15)
    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
result=dict(scope='bounded Instruments attachment to native Vulkan child; profiler cadence is not performance budget',capture_exit=capture_code,trace_exit=trace_code,attached_pid=attached,requested_start_frame=a.start_frame,attach_request_observed_frame=attached_frame,duration_seconds=a.duration,elapsed_seconds=time.monotonic()-start,trace_path=str(trace_file) if trace_file else None,trace_exists=trace_file is not None and trace_file.exists(),last_cull_logged_frame=last)
(root/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
raise SystemExit(0 if capture_code==0 and trace_code==0 and result['trace_exists'] else 1)
