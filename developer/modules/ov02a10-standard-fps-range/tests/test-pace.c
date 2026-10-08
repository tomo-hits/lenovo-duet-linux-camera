/* SPDX-License-Identifier: GPL-2.0 */
/* Additional tests of extracted production functions, not a second driver. */
static int pace_max_format;
static unsigned pace_fault_positions;

static void check_phase(const struct ov02a10_pace_phase *p, unsigned kind,
                        unsigned attempts, unsigned successes, int result)
{
 CHECK(p->entered && p->complete && !p->overflow && p->kind==kind);
 CHECK(p->attempts==attempts && p->successes==successes && p->result==result);
 CHECK(p->first_wait_us==(kind==OV02A10_PACE_START?0:250) && p->between_wait_us==250);
 CHECK(p->begin_ns<p->end_ns);
 CHECK(p->bus.count==attempts);
 CHECK(p->wait.count==(attempts?attempts-1:0));
 CHECK(p->end_gap.count==p->wait.count && p->begin_gap.count==p->wait.count);
 if(attempts) {
  CHECK(p->first_bus_ns>=p->begin_ns && p->last_bus_end_ns<p->end_ns);
  CHECK(p->first_wait_ns==(kind==OV02A10_PACE_START?7:300007));
  CHECK(p->bus.min==7 && p->bus.max==7 && p->bus.sum==attempts*7);
 }
 if(attempts>1) {
  CHECK(p->wait.min==300007 && p->wait.max==300007 && p->wait.sum==p->wait.count*300007);
  CHECK(p->end_gap.min>=300007 && p->begin_gap.min>=p->end_gap.min);
 }
 CHECK(!p->reserved[0] && !p->reserved[1]);
}

static void test_pace_success(void)
{
 for(unsigned rotation=0;rotation<2;rotation++) for(unsigned mipi=3;mipi<=4;mipi++) {
  init_sensor(rotation,mipi); controls[2].current=1580;
  struct ov02a10_pace_record blank=sensor.pace_record;
  CHECK(blank.schema==1 && blank.size==504 && !blank.attempt_valid);
  CHECK(!ov02a10_s_stream(&sensor.subdev,0)); CHECK(!memcmp(&blank,&sensor.pace_record,sizeof(blank)));
  CHECK(!ov02a10_s_stream(&sensor.subdev,1));
  struct ov02a10_pace_record saved=sensor.pace_record;
  unsigned expected=121+2*rotation+(mipi==3);
  CHECK((unsigned)op_count==expected);
  check_phase(&saved.start,OV02A10_PACE_START,expected,expected,0);
  CHECK(saved.attempt_valid && saved.epoch==1 && saved.probe_instance_ns==sensor.probe_instance_ns);
  CHECK(saved.pm_attempted && saved.pm_complete && !saved.pm_error && saved.pm_return_ns<saved.start.begin_ns);
  CHECK(!saved.stop.entered && !sensor.pace_active);
  for(unsigned i=1;i<=expected;i++) {
   CHECK(bus_sleeps[i]==i-1 && bus_kinds[i]==OV02A10_PACE_START);
  }
  CHECK(trace[expected].page==1 && trace[expected].address==0xac && trace[expected].value==1);
  CHECK(!ov02a10_s_stream(&sensor.subdev,1)); CHECK(!memcmp(&saved,&sensor.pace_record,sizeof(saved)));
  atomic_fetch_add(&ticks,1000000000); /* A long stream is not an I2C gap. */
  CHECK(!ov02a10_s_stream(&sensor.subdev,0));
  check_phase(&sensor.pace_record.stop,OV02A10_PACE_STOP,2,2,0);
  CHECK(!memcmp(&saved.start,&sensor.pace_record.start,sizeof(saved.start)));
  CHECK(sensor.pace_record.stop.end_gap.max<1000000 && !sensor.pace_active);
  CHECK(bus_sleeps[expected+1]==expected && bus_sleeps[expected+2]==expected+1);
  CHECK(bus_kinds[expected+1]==OV02A10_PACE_STOP && bus_kinds[expected+2]==OV02A10_PACE_STOP);
  saved=sensor.pace_record;
  CHECK(!ov02a10_s_stream(&sensor.subdev,0)); CHECK(!memcmp(&saved,&sensor.pace_record,sizeof(saved)));
  CHECK(resumes==1 && pm_get_count==4 && pm_put_count==5 && usage==0);
  read_count=op_count=0; sleep_count=0;
  CHECK(!ov02a10_s_stream(&sensor.subdev,1)); CHECK(sensor.pace_record.epoch==2 && !sensor.pace_record.stop.entered);
  CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 }
}

static void test_pace_faults(void)
{
 init_sensor(true,3); CHECK(!ov02a10_s_stream(&sensor.subdev,1));
 unsigned total=op_count; CHECK(total==124);
 CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 for(unsigned fail=1;fail<=total;fail++) {
  init_sensor(true,3); controls[2].current=1580; faults[fail]=-EREMOTEIO;
  CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EREMOTEIO);
  const struct ov02a10_pace_record *r=&sensor.pace_record;
  unsigned attempts=r->start.attempts;
  check_phase(&r->start,OV02A10_PACE_START,attempts,attempts-1,-EREMOTEIO);
  check_phase(&r->stop,OV02A10_PACE_CLEANUP,2,2,0);
  CHECK(r->start.first_bus_error==-EREMOTEIO && !r->stop.first_bus_error);
  CHECK(!sensor.pace_active && usage==0 && pm_put_count==pm_get_count+1);
  CHECK(sleep_count==attempts+1 && (unsigned)op_count==attempts+2);
  for(unsigned i=1;i<=attempts;i++) CHECK(bus_sleeps[i]==i-1 && bus_kinds[i]==1);
  CHECK(bus_sleeps[attempts+1]==attempts && bus_kinds[attempts+1]==3);
  CHECK(!starts && !sensor.streaming); pace_fault_positions++;
 }
 for(unsigned fail=1;fail<=2;fail++) {
  init_sensor(true,3); faults[1]=-EREMOTEIO; faults[1+fail]=-EACCES;
  CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EREMOTEIO);
  check_phase(&sensor.pace_record.start,1,1,0,-EREMOTEIO);
  check_phase(&sensor.pace_record.stop,3,fail,fail-1,-EACCES);
  CHECK(sensor.start_record.first_error==-EREMOTEIO && sensor.start_record.cleanup_error==-EACCES);
  CHECK(!sensor.pace_active && usage==0 && pm_put_count==1);
  init_sensor(true,3); CHECK(!ov02a10_s_stream(&sensor.subdev,1));
  faults[op_count+fail]=-EIO;
  CHECK(ov02a10_s_stream(&sensor.subdev,0)==-EIO);
  check_phase(&sensor.pace_record.stop,2,fail,fail-1,-EIO);
  CHECK(!sensor.pace_active && usage==0 && pm_put_count==5);
 }
 init_sensor(true,3); overrides[1]=134;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EUCLEAN);
 CHECK(sensor.start_record.exposure_raw==390 && !starts && !sensor.start_record.stream_on_attempted);
 CHECK(sensor.pace_record.start.result==-EUCLEAN && !sensor.pace_record.start.first_bus_error);
 CHECK(sensor.pace_record.stop.kind==3 && sensor.pace_record.stop.complete && usage==0);
 init_sensor(true,3); overrides[0]=256;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EPROTO);
 CHECK(sensor.pace_record.start.first_bus_error==-EPROTO && !sensor.pace_active && usage==0);
 init_sensor(true,3); resume_result=-EHOSTDOWN;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EHOSTDOWN);
 CHECK(sensor.pace_record.attempt_valid && sensor.pace_record.pm_attempted && sensor.pace_record.pm_complete);
 CHECK(sensor.pace_record.pm_error==-EHOSTDOWN && sensor.pace_record.pm_return_ns);
 CHECK(!sensor.pace_record.start.entered && !sensor.pace_record.stop.entered && !sleep_count && !op_count && !pm_put_count);
 init_sensor(true,3); sensor.stream_attempt_epoch=U64_MAX;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EOVERFLOW);
 CHECK(sensor.pace_record.epoch_overflow && sensor.pace_record.epoch==U64_MAX && !sensor.pace_record.pm_attempted);
 CHECK(!resumes && !op_count && !sensor.pace_active);
}

static void test_idle_control_isolation(void)
{
 for(unsigned which=0;which<4;which++) {
  init_sensor(true,3); struct ov02a10_pace_record saved=sensor.pace_record;
  active=true; controls[which].val=controls[which].current;
  mutex_lock(&sensor.mutex); CHECK(!ov02a10_set_ctrl(&controls[which])); mutex_unlock(&sensor.mutex);
  CHECK(!sleep_count && !memcmp(&saved,&sensor.pace_record,sizeof(saved)) && usage==0);
  CHECK(pm_get_count==1 && pm_put_count==1);
 }
}

static void test_pace_overflow(void)
{
 /* Stats failure cannot skip cleanup or leak a stream PM reference. */
 unsigned positions[]={1,124};
 for(unsigned i=0;i<2;i++) {
  init_sensor(true,3); inject_sum_overflow_at=positions[i];
  CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EOVERFLOW);
  CHECK(sensor.pace_record.start.overflow && sensor.pace_record.start.complete);
  CHECK(sensor.pace_record.start.result==-EOVERFLOW && sensor.pace_record.stop.complete);
  CHECK(sensor.pace_record.stop.attempts==2 && !sensor.pace_active && usage==0);
  CHECK(starts==(i==1)); /* Overflow detected after enable still runs cleanup. */
 }
 init_sensor(true,3); inject_clock_reverse_at=2;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EOVERFLOW);
 CHECK(sensor.pace_record.start.overflow && !starts && sensor.pace_record.stop.complete && usage==0);
 init_sensor(true,3); CHECK(!ov02a10_s_stream(&sensor.subdev,1));
 inject_sum_overflow_at=op_count+1;
 CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 CHECK(sensor.pace_record.stop.overflow && sensor.pace_record.stop.attempts==2 && usage==0 && !sensor.pace_active);
 /* Invalid stop evidence is reported; external rearm gate must reject it. */
 struct ov02a10_pace_phase p={0};
 p.wait.count=U64_MAX; ov02a10_pace_stat_add(&p,&p.wait,1);
 CHECK(p.overflow && p.wait.count==U64_MAX);
 memset(&p,0,sizeof(p)); p.wait.sum=U64_MAX;
 ov02a10_pace_stat_add(&p,&p.wait,1); CHECK(p.overflow && !p.wait.count && p.wait.sum==U64_MAX);
 init_sensor(true,3); mutex_lock(&sensor.mutex); ov02a10_pace_begin(&sensor,1);
 sensor.pace_record.start.attempts=~0U; sensor.pace_record.start.successes=~0U;
 CHECK(!ov02a10_pace_bus(&sensor,false,0xfd,1)); ov02a10_pace_end(&sensor,0); mutex_unlock(&sensor.mutex);
 CHECK(sensor.pace_record.start.overflow && sensor.pace_record.start.attempts==~0U && !sensor.pace_active);
}

static void test_pace_export(void)
{
 _Static_assert(sizeof(struct ov02a10_start_record)==768,"old ABI");
 _Static_assert(sizeof(struct ov02a10_pace_stat)==32,"stat ABI");
 _Static_assert(sizeof(struct ov02a10_pace_phase)==224,"phase ABI");
 _Static_assert(sizeof(struct ov02a10_pace_record)==504,"record ABI");
 _Static_assert(offsetof(struct ov02a10_pace_record,start)==56,"start offset");
 _Static_assert(offsetof(struct ov02a10_pace_record,stop)==280,"stop offset");
 _Static_assert(offsetof(struct ov02a10_pace_phase,begin_ns)==48,"time offset");
 _Static_assert(offsetof(struct ov02a10_pace_phase,wait)==96,"stats offset");
 init_sensor(true,3);
 struct ov02a10_pace_record out, sentinel; memset(&sentinel,0x55,sizeof(sentinel)); out=sentinel;
 CHECK(ov02a10_get_pace_record(&dev,1,&out,sizeof(out))==-ENODEV && !memcmp(&out,&sentinel,sizeof(out)));
 CHECK(!ov02a10_record_publish(&sensor));
 CHECK(ov02a10_get_pace_record(&dev,2,&out,sizeof(out))==-EPROTONOSUPPORT);
 CHECK(ov02a10_get_pace_record(&dev,1,&out,sizeof(out)-1)==-EMSGSIZE);
 CHECK(ov02a10_get_pace_record(NULL,1,&out,sizeof(out))==-EINVAL);
 CHECK(ov02a10_get_pace_record(&dev,1,NULL,sizeof(out))==-EINVAL);
 struct device wrong={0}; CHECK(ov02a10_get_pace_record(&wrong,1,&out,sizeof(out))==-ENODEV);
 CHECK(!memcmp(&out,&sentinel,sizeof(out)));
 CHECK(!ov02a10_s_stream(&sensor.subdev,1)); CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 CHECK(!ov02a10_get_pace_record(&dev,1,&out,sizeof(out)) && !memcmp(&out,&sensor.pace_record,sizeof(out)));
 unsigned ops=op_count, sleeps=sleep_count, pm=pm_put_count+pm_get_count+resumes;
 struct { char a; char buf[PAGE_SIZE]; char b; } b={.a=1,.b=2};
 ssize_t n=pace_show(&dev,NULL,b.buf);
 CHECK(n>0 && n<PAGE_SIZE-1 && b.buf[n-1]=='\n' && b.a==1 && b.b==2);
 CHECK(strstr(b.buf,"start_attempts=124") && strstr(b.buf,"stop_attempts=2") && strstr(b.buf,"stats=count,min,max,sum"));
 memset(&out,0xff,sizeof(out)); out.pm_error=INT32_MIN;
 out.start.result=out.start.first_bus_error=out.stop.result=out.stop.first_bus_error=INT32_MIN;
 n=ov02a10_pace_format(b.buf,&out);
 CHECK(n<PAGE_SIZE-1 && n==(ssize_t)strlen(b.buf) && b.buf[n-1]=='\n' && b.a==1 && b.b==2);
 pace_max_format=(int)n;
 CHECK((unsigned)op_count==ops && sleep_count==sleeps && (unsigned)(pm_put_count+pm_get_count+resumes)==pm);
 ov02a10_record_unpublish(&sensor); CHECK(pace_show(&dev,NULL,b.buf)==-ENODEV);
 CHECK(ov02a10_get_pace_record(&dev,1,&out,sizeof(out))==-ENODEV);
}

static void *pace_getter_run(void *unused)
{
 (void)unused; getter_thread=true; struct ov02a10_pace_record r;
 CHECK(!ov02a10_get_pace_record(&dev,1,&r,sizeof(r)));
 CHECK(r.schema==1 && r.probe_instance_ns==sensor.probe_instance_ns && !r.attempt_valid);
 return NULL;
}
static void test_pace_remove_race(void)
{
 for(unsigned i=0;i<128;i++) {
  init_sensor(false,4); CHECK(!ov02a10_record_publish(&sensor));
  atomic_store(&getter_has_registry,false); atomic_store(&unpublish_started,false); atomic_store(&unpublish_done,false);
  mutex_lock(&sensor.mutex); pthread_t getter,remover;
  CHECK(!pthread_create(&getter,NULL,pace_getter_run,NULL));
  while(!atomic_load(&getter_has_registry)) sched_yield();
  CHECK(!pthread_create(&remover,NULL,unpublish_run,NULL));
  while(!atomic_load(&unpublish_started)) sched_yield();
  CHECK(!atomic_load(&unpublish_done)); mutex_unlock(&sensor.mutex);
  CHECK(!pthread_join(getter,NULL)); CHECK(!pthread_join(remover,NULL));
  CHECK(atomic_load(&unpublish_done) && sysfs_removes==1);
 }
}

static void test_write_arguments(void)
{
 init_sensor(true,3); controls[2].current=1580; controls[0].current=240;
 CHECK(!ov02a10_s_stream(&sensor.subdev,1));
 CHECK(sensor.write_trace_count<=16);
 CHECK(sensor.startup_settle_ns>=140000000);
 unsigned last_low=0;
 for(unsigned i=0;i<sensor.write_trace_count;i++) {
  CHECK(!sensor.write_trace[i].result);
  if(sensor.write_trace[i].reg==4 && sensor.write_trace[i].page==1)
   last_low=sensor.write_trace[i].value;
 }
 CHECK(last_low==240);
 CHECK(!ov02a10_s_stream(&sensor.subdev,0));
}
int main(void)
{
 CHECK(!baseline_main()); sensor_initialized=false;
 unsigned long baseline=atomic_load(&checks);
 test_write_arguments(); test_pace_success(); test_pace_faults(); test_idle_control_isolation(); test_pace_overflow(); test_pace_export(); test_pace_remove_race();
 if(sensor_initialized) mutex_destroy(&sensor.mutex);
 CHECK(!held_locks);
 printf("{\"checks\":%lu,\"additional_checks\":%lu,\"start_fault_positions\":%u,\"pace_record_size\":%zu,\"pace_max_format_bytes\":%d,\"pace_getter_remove_interleavings\":128}\n",atomic_load(&checks),atomic_load(&checks)-baseline,pace_fault_positions,sizeof(struct ov02a10_pace_record),pace_max_format);
 return 0;
}
