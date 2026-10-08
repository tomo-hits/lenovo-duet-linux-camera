#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual upstream state functions; construct flushing with production unlock.
The thin API shim models a legal delayed Format callback and a real stream error.
Real application acceptance is separate and remains required.
"""
from pathlib import Path
import subprocess,tempfile,hashlib,json,re
base=Path(__file__).resolve().parent
def extract(s,name):
 m=re.search(r'^static [^\n]+\n'+re.escape(name)+r' \([^;]*?\)\n\{',s,re.M);assert m,name
 p=m.start();a=m.end()-1;depth=1;i=a+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[p:i]
shim=r'''
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#define TRUE true
#define FALSE false
#define GST_PIPEWIRE_DEFAULT_TIMEOUT 30
#define SPA_NSEC_PER_SEC 1000000000ULL
#define GST_DEBUG_OBJECT(...) ((void)0)
enum pw_stream_state { PW_STREAM_STATE_ERROR=-1,PW_STREAM_STATE_PAUSED=2 };
typedef enum {GST_STATE_CHANGE_NULL_TO_READY,GST_STATE_CHANGE_READY_TO_PAUSED,GST_STATE_CHANGE_PAUSED_TO_PLAYING,GST_STATE_CHANGE_PLAYING_TO_PAUSED,GST_STATE_CHANGE_PAUSED_TO_READY,GST_STATE_CHANGE_READY_TO_NULL} GstStateChange;
typedef int GstStateChangeReturn;
#define GST_STATE_CHANGE_FAILURE -1
#define GST_STATE_CHANGE_SUCCESS 0
#define GST_STATE_CHANGE_NO_PREROLL 1
typedef bool gboolean;
typedef struct GstPipeWireSrc GstPipeWireSrc;
struct Loop {int depth, waits;bool timeout;enum pw_stream_state state;GstPipeWireSrc *target;};
struct Core {struct Loop *loop;};
struct Stream {struct Core *core;struct Loop *pwstream;};
struct GstPipeWireSrc {struct Stream *stream;bool flushing,negotiated,autoconnect;};
typedef GstPipeWireSrc GstBaseSrc;
typedef GstPipeWireSrc GstElement;
#define GST_BASE_SRC(x) (x)
#define GST_PIPEWIRE_SRC(x) (x)
#define GST_PIPEWIRE_SRC_CAST(x) (x)
static void pw_thread_loop_lock(struct Loop*l){l->depth++;}
static void pw_thread_loop_unlock(struct Loop*l){assert(l->depth>0);l->depth--;}
static void pw_thread_loop_signal(struct Loop*l,bool b){(void)l;(void)b;}
static void pw_thread_loop_get_time(struct Loop*l,struct timespec*t,uint64_t n){(void)l;(void)n;*t=(struct timespec){0};}
static enum pw_stream_state pw_stream_get_state(struct Loop*l,const char**e){(void)e;return l->state;}
static const char *pw_stream_state_as_string(enum pw_stream_state s){(void)s;return "shim";}
static int pw_thread_loop_timed_wait_full(struct Loop*l,const struct timespec*t){(void)t;l->waits++;if(l->timeout)return -ETIMEDOUT;l->target->negotiated=TRUE;return 0;}
static void pw_thread_loop_wait(struct Loop*l){assert(!pw_thread_loop_timed_wait_full(l,NULL));}
static void pw_stream_set_active(struct Loop*l,bool a){(void)l;(void)a;}
static bool gst_pipewire_stream_open(struct Stream*s,const void*e){(void)s;(void)e;return TRUE;}
static void gst_pipewire_stream_close(struct Stream*s){(void)s;}
static enum pw_stream_state wait_started(GstPipeWireSrc*s){(void)s;return PW_STREAM_STATE_PAUSED;}
static bool gst_base_src_is_live(GstBaseSrc*s){(void)s;return TRUE;}
static GstStateChangeReturn parent_change(GstElement*e,GstStateChange t){(void)e;(void)t;return GST_STATE_CHANGE_SUCCESS;}
static struct {GstStateChangeReturn (*change_state)(GstElement*,GstStateChange);} parent_value={parent_change};
static void *parent_class=&parent_value;
#define GST_ELEMENT_CLASS(x) ((__typeof__(&parent_value))(x))
static int stream_events;
'''
main=r'''
int main(void){
 for(int scenario=0;scenario<5;scenario++){
  struct Loop loop={.state=PW_STREAM_STATE_PAUSED};struct Core core={&loop};struct Stream stream={&core,&loop};GstPipeWireSrc src={.stream=&stream,.autoconnect=TRUE};loop.target=&src;
  if(scenario==0)assert(gst_pipewire_src_unlock(&src));
  if(scenario==1)loop.state=PW_STREAM_STATE_ERROR;
  if(scenario==2)loop.timeout=TRUE;
  if(scenario>=3)src.negotiated=TRUE;
  if(scenario==4){assert(gst_pipewire_src_unlock(&src));assert(gst_pipewire_src_unlock_stop(&src));}
  int ret=gst_pipewire_src_change_state(&src,GST_STATE_CHANGE_PAUSED_TO_PLAYING);
  printf("{\"case\":%d,\"return\":%d,\"lock_depth\":%d,\"waits\":%d}\n",scenario,ret,loop.depth,loop.waits);
#ifdef FIXED
  assert(ret==(scenario==1||scenario==2?GST_STATE_CHANGE_FAILURE:GST_STATE_CHANGE_SUCCESS));assert(loop.depth==0);if(scenario==0)assert(loop.waits==1);
#else
  assert(ret==(scenario<=2?GST_STATE_CHANGE_FAILURE:GST_STATE_CHANGE_SUCCESS));assert(loop.depth==(scenario<=2?1:0));
#endif
 }
 return 0;
}
'''
for label,rel in [('original','inputs/gstpipewiresrc-before.c'),('fixed','inputs/gstpipewiresrc-after.c')]:
 p=base/rel;s=p.read_text();code=shim+'\n'+ '\n'.join(extract(s,n) for n in ['gst_pipewire_src_unlock','gst_pipewire_src_unlock_stop','wait_negotiated','gst_pipewire_src_change_state'])+'\n'+main
 with tempfile.TemporaryDirectory(prefix='duet-gst-state-') as d:
  a=Path(d)/'test.c';b=Path(d)/'test';a.write_text(code)
  subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-Werror','-Wno-unused-function']+(['-DFIXED'] if label=='fixed' else [])+[str(a),'-o',str(b)],check=True)
  output=subprocess.check_output([str(b)],text=True)
  print(json.dumps(dict(source=rel,sha256=hashlib.sha256(p.read_bytes()).hexdigest(),label=label)),flush=True);print(output,end='',flush=True)
print('PASS actual unlock->negotiate->state regression; original bugs reproduced; fixed callbacks/error/timeout release locks')
