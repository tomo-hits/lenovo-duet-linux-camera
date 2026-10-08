#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Duet camera project contributors
"""Actual handoff regression with synthetic event times, without hardware."""
from pathlib import Path
import json
import subprocess
import tempfile

kit = Path(__file__).resolve().parents[1]
source = (kit / 'modules/mt8183-p1-public/mt8183_p1_handoff.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
header = (kit / 'modules/mt8183-p1-public/mt8183_p1_handoff.h').read_text()
header = header[header.index('struct dph_handoff {'):header.index('void dph_init')]
shim = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
typedef uint64_t u64;
typedef int spinlock_t;
struct completion { int ready; };
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define spin_lock_init(l) (*(l)=0)
#define init_completion(c) ((c)->ready=0)
#define reinit_completion(c) ((c)->ready=0)
#define complete(c) ((c)->ready=1)
#define complete_all(c) ((c)->ready=1)
#define msecs_to_jiffies(n) (n)
static unsigned now_ms, event_ms;
static bool cancel;
static void arrive(void);
static void wait_for_completion(struct completion *c) {
 if (!c->ready) { now_ms = event_ms; arrive(); }
}
static unsigned wait_for_completion_timeout(struct completion *c,unsigned timeout) {
 if(c->ready)return 1;
 now_ms+=timeout;return 0;
}
'''
body = shim + header + source + r'''
static struct dph_handoff *current;
static void arrive(void) {
 if (cancel) dph_close(current);
 else { current->pending=true;current->logical=0; }
}
static int publish(void *context,u64 logical) {
 assert(logical==0);dph_close(context);return 0;
}
int main(void) {
 const unsigned times[]={134,2000,4350};
 for(unsigned i=0;i<3;i++) {
  struct dph_handoff h={0};dph_init(&h);current=&h;
  now_ms=0;event_ms=times[i];cancel=false;
  assert(dph_run(&h,publish,&h)==0 && h.completed==1 && now_ms==event_ms);
 }
 struct dph_handoff stopped={0};dph_init(&stopped);current=&stopped;
 now_ms=0;event_ms=10;cancel=true;
 assert(dph_run(&stopped,publish,&stopped)==0 && stopped.completed==0 && stopped.closed);
 struct dph_handoff active={0};dph_init(&active);now_ms=0;
 assert(dph_send(&active,1)==-ETIMEDOUT && now_ms==1500 && active.closed);
 assert(active.pending && active.completed==0); /* Timeout grants no ownership release. */
 assert(dph_send(&active,2)==-ECANCELED);
 struct dph_handoff busy={.active=true};dph_init(&busy);
 assert(dph_send(&busy,3)==-EBUSY);
 puts("PASS actual handoff: slow startup, idle STOP, active deadline, closed/busy admission");
}
'''
with tempfile.TemporaryDirectory(prefix='duet-handoff-wait-') as temp:
    folder = Path(temp)
    (folder / 'test.c').write_text(body)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', str(folder / 'test.c'),
                    '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True)
print(json.dumps({'status': 'PASS', 'cases': 7, 'asan_ubsan': True,
                  'actual_production_functions': True, 'hardware': False}))
