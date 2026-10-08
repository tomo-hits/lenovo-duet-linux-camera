#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
import os,json,subprocess,tempfile,hashlib
from pathlib import Path
r=Path(__file__).resolve().parents[1]
headers=['completion.h','spinlock.h','types.h','kthread.h','workqueue.h','err.h','errno.h','sched/task.h','jiffies.h']
result=[]
with tempfile.TemporaryDirectory(prefix='duet-worker-pipeline-') as temp:
 d=Path(temp)
 for n in headers:
  f=d/'linux'/n;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('#include "pipeline-shim.h"\n')
 for mode,flags in [('regular',['-O2']),('asan-ubsan',['-O1','-g','-fsanitize=address,undefined'])]:
  exe=d/mode
  subprocess.run(['clang','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(d),'-I'+str(r),'-I'+str(r/'tests'),*flags,*[str(r/n) for n in ['duet_p1_workers.c','duet_p1_handoff.c','tests/test_pipeline.c']],'-o',str(exe)],check=True)
  out=subprocess.check_output([str(exe)],text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
  result.append({'mode':mode,**json.loads(out)})
report={'scope':'actual executor/mailbox C with pthread mock publisher; no camera DMA/MMIO; real kernel untested','results':result,'sources':{str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in r.rglob('*') if p.is_file() and p.suffix in ('.c','.h','.py')}}
(r.parents[1]/'logs/2026-10-04-dual-pipeline-host-v1.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(result))
