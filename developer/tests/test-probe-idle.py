#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the production initial-idle transition with faulted PM/DMA operations."""
from pathlib import Path
import subprocess,tempfile
source=(Path(__file__).resolve().parents[1]/'modules/mt8183-p1-public/mt8183_p1_continuous.c').read_text()
start=source.index('static int video_initial_idle(struct integrated_state *s)\n{');end=source.index('static int video_rearm(',start)
functions=source[start:end]
harness=r'''
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
struct hw { bool initialized,published; int error; struct { unsigned copy_allocations,copy_frees; } observed; };
struct integrated_state {
 bool capture_attempted,fw_exposed,workers_active,scp_owned,sensor_on,seninf_on,ipi_registered[2],transport_failed;
 bool inputs_idle,video_needs_prepare,cycle_verified,stream_module_ref;
 int irq_enabled,capture_error,input_error;
 struct { bool pm_ref,clocks_on; } resources;
 struct hw hw;unsigned retired_cpu_allocations,retired_cpu_frees;
};
#define atomic_read(p) (*(p))
static int gate_error,release_error,gates,frees;
static int cam_put_and_gate(struct integrated_state *s) {
 gates++;if(gate_error)return gate_error;s->resources.pm_ref=false;s->resources.clocks_on=false;return 0;
}
static int mt8183_p1_hw_release(struct hw *h,bool published_gate) {
 assert(!published_gate);frees++;if(release_error)return release_error;
 h->observed.copy_frees=3;return 0;
}
'''
checks=r'''
int main(void) {
 struct integrated_state initial={0},s;
 initial.hw.initialized=true;initial.hw.observed.copy_allocations=3;
 initial.resources.pm_ref=initial.resources.clocks_on=true;
 s=initial;gate_error=-EIO;assert(video_initial_idle(&s)==-EIO);
 assert(gates==1&&frees==0&&s.hw.initialized&&!s.video_needs_prepare);
 gate_error=0;release_error=-EBUSY;assert(video_initial_idle(&s)==-EBUSY);
 assert(frees==1&&s.hw.initialized&&!s.video_needs_prepare);
 release_error=0;s=initial;assert(!video_initial_idle(&s));
 assert(!s.hw.initialized&&s.inputs_idle&&s.video_needs_prepare&&!s.cycle_verified);
 assert(s.retired_cpu_allocations==3&&s.retired_cpu_frees==3);
 assert(video_unpublished_idle(&s));
 bool *busy[]={&s.capture_attempted,&s.hw.published,&s.fw_exposed,&s.workers_active,
 &s.scp_owned,&s.sensor_on,&s.seninf_on,&s.ipi_registered[0],&s.ipi_registered[1],&s.transport_failed};
 for(unsigned i=0;i<sizeof(busy)/sizeof(*busy);i++) {
  *busy[i]=true;int before=frees;assert(video_initial_idle(&s)==-EBUSY);assert(frees==before);
  assert(!video_unpublished_idle(&s));*busy[i]=false;
 }
 s.irq_enabled=1;assert(video_initial_idle(&s)==-EBUSY);assert(!video_unpublished_idle(&s));s.irq_enabled=0;
 s.resources.pm_ref=true;assert(!video_unpublished_idle(&s));s.resources.pm_ref=false;
 s.resources.clocks_on=true;assert(!video_unpublished_idle(&s));s.resources.clocks_on=false;
 s.stream_module_ref=true;assert(!video_unpublished_idle(&s));s.stream_module_ref=false;
 s.capture_error=1;assert(!video_unpublished_idle(&s));s.capture_error=0;
 s.input_error=1;assert(!video_unpublished_idle(&s));s.input_error=0;
 assert(video_unpublished_idle(&s));return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='duet-probe-idle-') as d:
 p=Path(d);(p/'test.c').write_text(harness+functions+checks)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Production probe-idle fault/ownership test PASS')
