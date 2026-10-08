#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import tempfile,subprocess,json,os,hashlib
r=Path(__file__).resolve().parents[1]
s=(r/'duet_p1_continuous.c').read_text();a=s.index('static int session_rearm(');b=s.index('\nstatic ssize_t control_store(',a);body=s[a:b]
a=s.index("static ssize_t frame_log_read(");b=s.index("\nstatic const struct bin_attribute",a);body+=s[a:b]
results=[]
with tempfile.TemporaryDirectory(prefix='duet-rearm-') as tmp:
 p=Path(tmp);c=p/'test.c';c.write_text((r/'tests/test_rearm_prefix.c').read_text()+body+(r/'tests/test_rearm_suffix.c').read_text())
 for mode,flags in [('regular',['-O2']),('asan-ubsan',['-O1','-g','-fsanitize=address,undefined'])]:
  exe=p/mode;subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',*flags,str(c),'-o',str(exe)],check=True)
  out=subprocess.check_output([str(exe)],text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
  results.append(dict(mode=mode,**json.loads(out)))
report=dict(scope='exact parent rearm function with mocked endpoint/HW/power operations; no camera/kernel execution',body_sha256=hashlib.sha256(body.encode()).hexdigest(),results=results)
(r.parents[1]/'logs/2026-09-27-p1-restart30-rearm-host.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
