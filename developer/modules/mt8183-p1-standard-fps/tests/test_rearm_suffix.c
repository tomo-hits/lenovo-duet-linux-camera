/* Modified on 2026-10-05: explicit publication license. */
/* SPDX-License-Identifier: GPL-2.0-only */
static struct device dev,child_dev;
static struct child sensor={&child_dev},seninf={&child_dev};
static void successful_capture(struct integrated_state *s){
 s->capture_ok=s->stop_verified=s->stopping=s->retain_capture=s->inputs_idle=true;
 s->workers=(struct workers){.initialized=true,.drained=true,.state=DPW_STOPPED};
 s->resources.pm_ref=s->resources.clocks_on=false;s->stage=2;s->commands=36;
 s->capture_attempted=s->fw_exposed=true;s->session_irq_calls=60;
 s->hw.initialized=true;
 s->hw.observed=(struct duet_p1_hw_snapshot){.capture_complete=true,.ring_finalized=true,.allocated=true,.archived_frames=30,.copy_allocations=3};
 s->hw.output_cpu=(void*)1;s->hw.copy_cpu=(void*)2;s->hw.archive_cpu=(void*)3;s->hw.compare_cpu=(void*)4;
 memset(s->composer.cpu,0xaa,TEST_SIZE);memset(s->stop_snapshot,0xaa,sizeof(s->stop_snapshot));
 for(int i=0;i<2;i++){s->channel[i].count=30;s->channel[i].len=129;memset(s->channel[i].bytes,0xaa,129);s->channel[i].reply=1;s->channel[i].id=10+i;s->channel[i].state=s;}
}
static void init(struct integrated_state *s){
 memset(s,0,sizeof(*s));s->resources=(struct resource){.dev=&dev,.base=(void*)9,.irq_owned=true,.irq=255};
 s->composer=(struct composer){.cpu=malloc(TEST_SIZE),.mapped=true,.cam_iova=123};assert(s->composer.cpu);
 s->graph=(struct graph){.prepared=true,.sensor=&sensor,.seninf=&seninf,.identity=77};
 s->smi_ref=true;s->scp_api=(void*)8;s->session_epoch=1;session_epochs=1;
 fault=step=mutations=pm_gets=0;offline=release_hold=true;current=s;successful_capture(s);
}
static unsigned cases;
#define GUARD(change,err) do {struct integrated_state s;init(&s);change;assert(session_rearm(&s)==err);assert(!mutations);assert(s.hw.output_cpu==(void*)1);free(s.composer.cpu);cases++;} while(0)
int main(void){
 GUARD(s.rearms=2,-EPERM);GUARD(s.rearm_error=-EIO,-EPERM);GUARD(s.capture_ok=false,-EPERM);
 GUARD(s.workers.drained=false,-EPERM);GUARD(s.workers.state=5,-EPERM);GUARD(s.workers_active=true,-EPERM);
 GUARD(s.workers.capture_task=(void*)1,-EPERM);GUARD(s.workers.publish_task=(void*)1,-EPERM);GUARD(s.workers.coordinator=(void*)1,-EPERM);
 GUARD(s.handoff.active=true,-EPERM);GUARD(s.handoff.pending=true,-EPERM);GUARD(s.sensor_on=true,-EPERM);GUARD(s.seninf_on=true,-EPERM);
 GUARD(s.inputs_idle=false,-EPERM);GUARD(s.scp_owned=true,-EPERM);GUARD(s.ipi_registered[0]=true,-EPERM);GUARD(s.ipi_registered[1]=true,-EPERM);
 GUARD(s.irq_enabled=1,-EPERM);GUARD(s.resources.pm_ref=true,-EPERM);GUARD(s.resources.clocks_on=true,-EPERM);
 GUARD(s.stop_verified=false,-EPERM);GUARD(s.transport_failed=true,-EPERM);GUARD(s.input_error=-EIO,-EPERM);
 GUARD(offline=false,-EPERM);GUARD(release_hold=false,-EPERM);GUARD(fault=1,-EIO);GUARD(fault=2,-EIO);
 GUARD(s.hw.observed.done_pending=true,-EBUSY);GUARD(s.hw.observed.copy_pending=true,-EBUSY);GUARD(s.hw.observed.ring_cpu_refs=1,-EBUSY);
 GUARD(s.hw.observed.ring_finalized=false,-EBUSY);GUARD(session_epochs=INT64_MAX-1;session_epochs++, -EOVERFLOW);
 for(int f=3;f<=6;f++){
  struct integrated_state s;init(&s);fault=f;assert(session_rearm(&s)<0);assert(s.rearm_error&&s.transport_failed&&!s.capture_ok);
  int n=mutations;assert(session_rearm(&s)==-EPERM&&mutations==n);assert(s.graph.identity==77&&s.graph.prepared);
  if(f>=4){assert(s.retired_sessions==1&&s.retired_cpu_frees==3&&!s.stop_verified&&!s.hw.output_cpu);}
  char buf[4];assert(frame_log_read(NULL,NULL,NULL,buf,0,sizeof(buf))==(f==4||f==5?-ENODATA:1));
  free(s.composer.cpu);cases++;
 }
 struct integrated_state s;init(&s);struct graph g=s.graph;
 for(int i=1;i<=2;i++){
  assert(session_rearm(&s)==0);assert(s.session_epoch==(u64)i+1&&s.retired_epoch==(u64)i);
  assert(s.retired_sessions==(unsigned)i&&s.retired_cpu_frees==(unsigned)3*i&&s.retired_cpu_allocations==(unsigned)3*i);
  assert(!memcmp(&g,&s.graph,sizeof(g))&&s.hw.profile.bayer_id==3&&s.sensor_code==MEDIA_BUS_FMT_SRGGB10_1X10);
  assert(!s.capture_ok&&!s.capture_attempted&&!s.fw_exposed&&!s.stop_verified&&!s.stopping&&!s.session_irq_calls&&!s.workers.initialized);
  for(unsigned j=0;j<TEST_SIZE;j++)assert(!((char*)s.composer.cpu)[j]);
  for(int k=0;k<2;k++)assert(!s.channel[k].count&&!s.channel[k].len&&!s.channel[k].reply&&s.channel[k].id==10+k&&s.channel[k].state==&s);
  successful_capture(&s);step=0;cases++;
 }
 assert(session_rearm(&s)==-EPERM);free(s.composer.cpu);
 printf("{\"cases\":%u,\"actual_rearm_body\":true,\"hardware\":false}\n",cases);return 0;
}
