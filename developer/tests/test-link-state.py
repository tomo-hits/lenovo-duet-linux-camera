#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual PipeWire port_state_changed + pw_impl_link_prepare, coupled state sequence.
Thin work-queue API shims count admissions/cancellations; not a full graph test.
"""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parent
body=r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#define SPA_ID_INVALID UINT32_MAX
#define SPA_CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define pw_log_debug(...) ((void)0)
enum pw_impl_port_state {PW_IMPL_PORT_STATE_ERROR=-1,PW_IMPL_PORT_STATE_INIT,PW_IMPL_PORT_STATE_CONFIGURE,PW_IMPL_PORT_STATE_READY,PW_IMPL_PORT_STATE_PAUSED};
enum {PW_LINK_STATE_ERROR=-1,PW_LINK_STATE_INIT,PW_LINK_STATE_NEGOTIATING};
struct pw_impl_port {int identity;};
struct pw_impl_node {bool active;};
struct port_info {struct pw_impl_port *port;struct pw_impl_node *node;};
struct pw_impl_link {bool prepared,preparing,destroyed;unsigned passive;int state;};
struct impl {struct pw_impl_link this;struct port_info input,output;void *work;};
typedef void (*pw_work_func_t)(void *,void *,int,uint32_t);
static int admitted,cancelled,busy_cleared;static struct port_info *last_info;
static void check_states(void *a,void *b,int c,uint32_t d){}
static int pw_work_queue_add(void *a,void *b,int c,pw_work_func_t f,void *d){++admitted;return 1;}
static int pw_work_queue_cancel(void *a,void *b,uint32_t c){++cancelled;return 0;}
static int port_set_busy_id(struct pw_impl_link *a,struct port_info *b,uint32_t c,int d){++busy_cleared;last_info=b;return 0;}
static void link_update_state(struct pw_impl_link *p,int state,int result,char *message){p->state=state;free(message);}
PREPARE
CHANGED
static void init(struct impl *p,struct pw_impl_port *i,struct pw_impl_port *o,struct pw_impl_node *n){memset(p,0,sizeof(*p));p->input.port=i;p->output.port=o;p->input.node=p->output.node=n;n->active=true;admitted=cancelled=busy_cleared=0;last_info=NULL;}
int main(){struct impl p;struct pw_impl_port i={1},o={2};struct pw_impl_node n;
 for(int side=0;side<2;++side){
  init(&p,&i,&o,&n);assert(pw_impl_link_prepare(&p.this)==0);assert(p.this.preparing&&admitted==1);
  struct pw_impl_port *port=side?&i:&o;
  port_state_changed(&p.this,port,side?&o:&i,PW_IMPL_PORT_STATE_INIT,PW_IMPL_PORT_STATE_CONFIGURE,NULL);
  assert(cancelled==1&&busy_cleared==1&&last_info==(side?&p.input:&p.output));
  int oldadmitted=admitted;assert(pw_impl_link_prepare(&p.this)==0);
  printf("suspend while preparing side=%d admitted_again=%d\n",side,admitted-oldadmitted);
#ifdef FIXED
  assert(admitted==oldadmitted+1);
#else
  assert(admitted==oldadmitted);
#endif
 }
 {init(&p,&i,&o,&n);pw_impl_link_prepare(&p.this);p.this.prepared=true;port_state_changed(&p.this,&o,&i,PW_IMPL_PORT_STATE_PAUSED,PW_IMPL_PORT_STATE_READY,NULL);assert(!p.this.prepared&&p.this.state==PW_LINK_STATE_NEGOTIATING);int before=admitted;pw_impl_link_prepare(&p.this);
#ifdef FIXED
 assert(admitted==before+1);
#else
 assert(admitted==before);
#endif
 }
 {init(&p,&i,&o,&n);pw_impl_link_prepare(&p.this);p.this.prepared=true;port_state_changed(&p.this,&o,&i,PW_IMPL_PORT_STATE_PAUSED,PW_IMPL_PORT_STATE_CONFIGURE,NULL);assert(!p.this.prepared&&p.this.state==PW_LINK_STATE_INIT);int before=admitted;pw_impl_link_prepare(&p.this);
#ifdef FIXED
 assert(admitted==before+1);
#else
 assert(admitted==before);
#endif
 }
 {init(&p,&i,&o,&n);p.this.prepared=true;p.this.preparing=true;port_state_changed(&p.this,&o,&i,PW_IMPL_PORT_STATE_READY,PW_IMPL_PORT_STATE_PAUSED,NULL);assert(p.this.prepared&&p.this.preparing&&admitted==0);}
 {init(&p,&i,&o,&n);n.active=false;pw_impl_link_prepare(&p.this);assert(!p.this.preparing&&admitted==0);}
 {init(&p,&i,&o,&n);p.this.destroyed=true;pw_impl_link_prepare(&p.this);assert(admitted==0);}
 {init(&p,&i,&o,&n);port_state_changed(&p.this,&o,&i,PW_IMPL_PORT_STATE_READY,PW_IMPL_PORT_STATE_ERROR,"original error");assert(p.this.state==PW_LINK_STATE_ERROR&&admitted==0);}
 puts("PASS 8 coupled production link-state cases; work queue API shims");
}
'''
def function(s,start):
 a=s.index(start);b=s.index('{',a);d=1;k=b+1
 while d:d+=(s[k]=='{')-(s[k]=='}');k+=1
 return s[a:k]
with tempfile.TemporaryDirectory(prefix='duet-pw-link-') as d:
 for path,fixed in [(root/'inputs/impl-link-before.c',False),(root/'inputs/impl-link-after.c',True)]:
  s=path.read_text();code=body.replace('PREPARE',function(s,'int pw_impl_link_prepare(')).replace('CHANGED',function(s,'static void port_state_changed('));p=Path(d)/'test.c';p.write_text(code)
  cmd=['clang','-std=c11','-D_DARWIN_C_SOURCE','-O1','-g','-fsanitize=address,undefined',str(p),'-o',str(Path(d)/'test')]
  if fixed:cmd.insert(1,'-DFIXED')
  subprocess.run(cmd,check=True);print(path.name,flush=True);subprocess.run([str(Path(d)/'test')],check=True)
