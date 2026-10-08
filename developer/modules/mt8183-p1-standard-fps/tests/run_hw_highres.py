#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
import hashlib,json,os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]; repo=root.parents[1]
units=['duet_p1_hw.c','duet_p1_adapter.c','duet_p1_bqueue.c','duet_p1_stream.c','duet_p1_stream_codec.c','tests/test_highres_geometry.c']
headers=['completion.h','spinlock.h','types.h','device.h','dma-map-ops.h','dma-mapping.h','errno.h','io.h','iommu-dma.h','iommu.h','jiffies.h','ktime.h','mm.h','pm_runtime.h','remoteproc/mtk_scp.h','string.h','vmalloc.h','stddef.h']
report={'scope':'actual HW wrapper with synthetic registers/IPI/DMA/timing; not real kernel/PM/IRQ lifetime proof','runs':[],
        'sources':{str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file() and p.suffix in ('.c','.h','.py')}}
with tempfile.TemporaryDirectory(prefix='duet-hw-') as temp:
    d=Path(temp)
    for name in headers:
        p=d/'linux'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "hw-shim.h"\n')
    for name,flags in [('regular',['-O2']),('asan-ubsan',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=d/name
        cmd=['clang','-std=gnu11','-Wall','-Wextra','-Werror','-DDUET_P1_STREAM_HOST_TEST','-I'+str(d),'-I'+str(root/'tests'),'-I'+str(root),*flags,*[str(root/p) for p in units],'-o',str(binary)]
        subprocess.run(cmd,check=True)
        result=json.loads(subprocess.check_output([str(binary)],text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'}))
        report['runs'].append({'mode':name,'result':result})
(repo/'logs/2026-10-04-dual-pm-retire-hw-host-v2.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report['runs']))
