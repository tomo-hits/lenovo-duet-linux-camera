/* Modified on 2026-10-05: explicit publication license. */
/* SPDX-License-Identifier: GPL-2.0-only */
/* Synthetic-only author harness, appended after extracted production code. */
static struct ov02a10 sensor;
static struct device dev;
static struct v4l2_ctrl controls[6];
static bool sensor_initialized;
static int registers[2][256], page, op_count, read_count, write_count;
static int faults[512], overrides[OV02A10_RECORD_READS];
static struct { int read, page, address, value, result; } trace[512];
static int resume_result, ctrl_pm_result, put_result, usage, resumes, pm_get_count, pm_put_count;
static bool active;
static int setup_override, range_changes, sysfs_result, sysfs_creates, sysfs_removes;
static int starts, max_format_bytes;
static unsigned bus_sleeps[512], bus_kinds[512];
static int inject_sum_overflow_at, inject_clock_reverse_at;
static void observe_bus(int idx)
{
 bus_sleeps[idx]=sleep_count;
 bus_kinds[idx]=sensor.pace_active ? sensor.pace_active->kind : 0;
 if (idx==inject_sum_overflow_at && sensor.pace_active)
  sensor.pace_active->bus.sum=U64_MAX;
 if (idx==inject_clock_reverse_at) atomic_store(&ticks,0);
}
static atomic_bool unpublish_started, unpublish_done;

static int i2c_smbus_write_byte_data(struct i2c_client *c,u8 reg,u8 value)
{
 CHECK(c==&client && op_count+1<512);
 int idx=++op_count, ret=faults[idx]; write_count++; observe_bus(idx);
 trace[idx]=(typeof(trace[0])){0,page,reg,value,ret};
 if (ret) return ret;
 if (reg==0xfd) { CHECK(value<2); page=value; }
 else registers[page][reg]=value;
 if (page==1 && reg==0xac && value==1) {
  CHECK(sensor.start_record.required_readback_complete);
  starts++;
 }
 return 0;
}
static int i2c_smbus_read_byte_data(struct i2c_client *c,u8 reg)
{
 CHECK(c==&client && page==1 && op_count+1<512);
 CHECK(read_count<(int)OV02A10_RECORD_READS);
 int idx=++op_count, ret=faults[idx]; observe_bus(idx);
 if (!ret) ret=overrides[read_count]!=INT_MIN ? overrides[read_count] : registers[page][reg];
 read_count++;
 trace[idx]=(typeof(trace[0])){1,page,reg,0,ret};
 return ret;
}
static int pm_runtime_resume_and_get(struct device *d)
{
 CHECK(d==&dev); resumes++;
 if (resume_result<0) return resume_result;
 usage++; active=true; return resume_result;
}
static int pm_runtime_get_if_in_use(struct device *d)
{
 CHECK(d==&dev); pm_get_count++;
 if (ctrl_pm_result>0) usage++;
 return ctrl_pm_result;
}
static int pm_runtime_put(struct device *d)
{
 CHECK(d==&dev && usage>0); usage--; pm_put_count++; return put_result;
}
static bool pm_runtime_active(struct device *d) { CHECK(d==&dev); return active; }
static int __v4l2_ctrl_modify_range(struct v4l2_ctrl *c,s64 min,s64 max,s64 step,s64 def)
{
 CHECK(c==sensor.exposure); range_changes++;
 c->minimum=min; c->maximum=max; c->step=step; c->default_value=def;
 return 0;
}
static int __v4l2_ctrl_handler_setup(struct v4l2_ctrl_handler *h)
{
 CHECK(h==&sensor.ctrl_handler);
 if (setup_override) return setup_override;
 unsigned order[]={2,0,3,4};
 for(unsigned i=0;i<6;i++) controls[i].val=controls[i].current;
 for(unsigned i=0;i<ARRAY_SIZE(order);i++) {
  struct v4l2_ctrl *c=&controls[order[i]]; c->val=c->current;
  int ret=ov02a10_set_ctrl(c); if(ret) return ret;
 }
 return 0;
}
static int sysfs_create_group(struct kobject *k,const struct attribute_group *g)
{
 CHECK(!held_locks && g==&ov02a10_telemetry_group); (void)k;
 sysfs_creates++; return sysfs_result;
}
static void sysfs_remove_group(struct kobject *k,const struct attribute_group *g)
{
 CHECK(!held_locks && g==&ov02a10_telemetry_group); (void)k;
 sysfs_removes++;
 /* Active show may enter the getter while kernfs drains: no registry or
  * sensor mutex may be held, and the instance has already been unlinked.
  */
 struct ov02a10_start_record r, original;
 memset(&r,0x55,sizeof(r)); original=r;
 CHECK(ov02a10_get_start_record(&dev,OV02A10_RECORD_SCHEMA,&r,sizeof(r))==-ENODEV);
 CHECK(!memcmp(&r,&original,sizeof(r)));
}

static void init_sensor(bool rotate, unsigned mipi)
{
 CHECK(ov02a10_record_registry.next==&ov02a10_record_registry);
 if(sensor_initialized) mutex_destroy(&sensor.mutex);
 memset(&sensor,0,sizeof(sensor)); memset(controls,0,sizeof(controls));
 memset(registers,0,sizeof(registers)); memset(trace,0,sizeof(trace)); memset(faults,0,sizeof(faults));
 for(unsigned i=0;i<OV02A10_RECORD_READS;i++) overrides[i]=INT_MIN;
 page=op_count=read_count=write_count=0;
 sleep_count=0; inject_sum_overflow_at=inject_clock_reverse_at=0;
 memset(bus_sleeps,0,sizeof(bus_sleeps)); memset(bus_kinds,0,sizeof(bus_kinds));
 resume_result=0; ctrl_pm_result=1; put_result=usage=resumes=pm_get_count=pm_put_count=0; active=false;
 setup_override=range_changes=sysfs_result=sysfs_creates=sysfs_removes=starts=0;
 sensor.dev=&dev; sensor.subdev.client=&client; sensor.subdev.ctrl_handler=&sensor.ctrl_handler;
 sensor.cur_mode=&supported_modes[0]; sensor.fmt.code=rotate?0x300f:0x3007;
 sensor.upside_down=rotate; sensor.mipi_clock_voltage=mipi;
 mutex_init(&sensor.mutex); sensor_initialized=true;
 INIT_LIST_HEAD(&sensor.telemetry_entry); sensor.probe_instance_ns=ktime_get_ns();
 int defaults[]={430,64,1580,0}; int mins[]={4,16,1580,0}; int maxs[]={2776,248,1580,1};
 int ids[]={V4L2_CID_EXPOSURE,V4L2_CID_ANALOGUE_GAIN,V4L2_CID_VBLANK,V4L2_CID_TEST_PATTERN};
 for(unsigned i=0;i<4;i++) {
  controls[i].handler=&sensor.ctrl_handler; controls[i].id=ids[i];
  controls[i].current=controls[i].val=defaults[i]; controls[i].p_cur.p_s32=&controls[i].current;
  controls[i].minimum=mins[i]; controls[i].maximum=maxs[i]; controls[i].step=1; controls[i].default_value=defaults[i];
 }
 sensor.exposure=&controls[0]; sensor.gain=&controls[1]; sensor.vblank=&controls[2]; sensor.test_pattern=&controls[3];
 sensor.hflip=&controls[4];sensor.vflip=&controls[5];
 for(unsigned i=4;i<6;i++){controls[i].handler=&sensor.ctrl_handler;controls[i].id=i==4?V4L2_CID_HFLIP:V4L2_CID_VFLIP;controls[i].current=controls[i].val=rotate;controls[i].p_cur.p_s32=&controls[i].current;}
 ov02a10_record_clear(&sensor);
}

static void check_record_success(void)
{
 const struct ov02a10_start_record *r=&sensor.start_record;
 CHECK(r->schema==2 && r->size==sizeof(*r));
 CHECK(r->attempt_valid && r->epoch==sensor.stream_attempt_epoch && r->probe_instance_ns==sensor.probe_instance_ns);
 CHECK(r->phase==OV02A10_RECORD_STREAM_STARTED && !r->first_error && r->stream_started);
 CHECK(r->readback_complete && r->read_valid_mask==2047 && r->read_attempted_mask==2047);
 CHECK(r->pair_coherent_mask==3 && r->expected_match_mask==2047);
 CHECK(r->attempt_ns<=r->cache_ns && r->cache_ns<r->read_start_ns);
 CHECK(r->read_start_ns<r->page_select_start_ns && r->page_select_start_ns<r->page_select_end_ns);
 for(unsigned i=0;i<11;i++) {
  const struct ov02a10_record_read *v=&r->reads[i];
  CHECK(v->valid && v->attempted && v->raw>=0 && v->raw<=255 && !v->error);
  CHECK(v->start_ns<v->end_ns);
  CHECK(!i || r->reads[i-1].end_ns<v->start_ns);
 }
 CHECK(r->reads[10].end_ns<r->page_restore_start_ns && r->page_restore_start_ns<r->page_restore_end_ns);
 CHECK(r->page_restore_end_ns<r->read_end_ns && r->read_end_ns<r->stream_on_begin_ns && r->stream_on_begin_ns<r->stream_on_end_ns);
 CHECK(r->exposure_raw==controls[0].current);
 CHECK(r->vts_delta_raw==controls[2].current-24);
 CHECK(r->driver_total_lines==controls[2].current+1200);
 CHECK(usage==1 && resumes==1 && pm_get_count==4 && pm_put_count==4 && read_count==11 && starts==1 && page==1);
}

static void test_success_and_idempotence(void)
{
 for(unsigned rotation=0;rotation<2;rotation++) for(unsigned mipi=3;mipi<=4;mipi++) for(int vb=190;vb<=7151;vb+=(vb==190?1390:5571)) {
  init_sensor(rotation,mipi); controls[2].current=vb;
  CHECK(!ov02a10_s_stream(&sensor.subdev,1)); check_record_success();
  struct ov02a10_start_record saved=sensor.start_record; int oldop=op_count;
  CHECK(!ov02a10_s_stream(&sensor.subdev,1)); CHECK(!memcmp(&saved,&sensor.start_record,sizeof(saved))); CHECK(op_count==oldop);
  for(unsigned i=0;i<4;i++) { controls[i].val=1; CHECK(ov02a10_set_ctrl(&controls[i])==-EBUSY); }
  CHECK(op_count==oldop && range_changes==1 && usage==1);
  CHECK(!ov02a10_s_stream(&sensor.subdev,0)); CHECK(usage==0 && !sensor.streaming && sensor.start_record.stop_seen);
  CHECK(sensor.start_record.stream_started && !sensor.start_record.stop_error && sensor.start_record.stop_ns>saved.stream_on_end_ns);
  oldop=op_count; CHECK(!ov02a10_s_stream(&sensor.subdev,0)); CHECK(op_count==oldop && usage==0);
 }
}

static void test_each_transfer_failure(void)
{
 init_sensor(true,3); CHECK(!ov02a10_s_stream(&sensor.subdev,1)); int total=op_count;
 int read_first=0,select=0,restore=total-1;
 for(int i=1;i<=total;i++) if(trace[i].read) { read_first=i; break; }
 CHECK(read_first>1); select=read_first-1;
 CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 for(int fail=1;fail<=total;fail++) {
  init_sensor(true,3); faults[fail]=-EREMOTEIO;
  CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EREMOTEIO);
  struct ov02a10_start_record *r=&sensor.start_record;
  CHECK(r->attempt_valid && r->epoch==1 && r->first_error==-EREMOTEIO && r->phase==OV02A10_RECORD_START_FAILED);
  CHECK(!r->stream_started && !sensor.streaming && !starts && usage==0 && r->cleanup_attempted);
  CHECK(!r->cleanup_error && !r->pm_put_error && page==1);
  if(fail<select) CHECK(!r->read_start_ns && !read_count && r->setup_error==-EREMOTEIO);
  if(fail==select) CHECK(!read_count && r->page_select_error==-EREMOTEIO && r->page_restore_start_ns);
  if(fail>=read_first && fail<restore) {
   unsigned idx=(unsigned)(fail-read_first);
   CHECK(read_count==(int)idx+1);
   CHECK(r->read_attempted_mask==BIT(idx+1)-1 && r->read_valid_mask==BIT(idx)-1);
   CHECK(r->reads[idx].error==-EREMOTEIO && r->reads[idx].raw==-1 && !r->reads[idx].valid);
   for(unsigned j=idx+1;j<11;j++) CHECK(!r->reads[j].attempted && !r->reads[j].valid && r->reads[j].raw==-1 && !r->reads[j].error);
   CHECK(r->page_restore_start_ns && !r->page_restore_error);
  }
  if(fail==restore) CHECK(r->read_valid_mask==2047 && r->page_restore_error==-EREMOTEIO && !r->readback_complete);
  if(fail==total) CHECK(r->stream_on_attempted && r->stream_on_error==-EREMOTEIO && r->readback_complete);
  else CHECK(!r->stream_on_attempted);
 }
 /* Primary read failure, restore failure, cleanup failure and PM-put
  * failure must remain independent. Cleanup is not a hidden read retry.
  */
 for(int primary=select;primary<=restore;primary++) {
  init_sensor(true,3); faults[primary]=-EREMOTEIO;
  int next_restore=primary==restore?0:primary+1;
  if(next_restore) faults[next_restore]=-EIO;
  int cleanup=next_restore?next_restore+1:primary+1;
  faults[cleanup]=-EACCES; put_result=-EBUSY;
  CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EREMOTEIO);
  CHECK(sensor.start_record.first_error==-EREMOTEIO && sensor.start_record.cleanup_error==-EACCES && sensor.start_record.pm_put_error==-EBUSY);
  CHECK(sensor.start_record.page_restore_error==(next_restore?-EIO:-EREMOTEIO));
  CHECK(!sensor.streaming && usage==0);
 }
}

static void test_read_values_and_epochs(void)
{
 /* Mismatch is preserved, not silently treated as all-register success. */
 for (unsigned rotate=0; rotate<2; rotate++) for (int raw=0; raw<=3; raw+=3) {
  init_sensor(rotate,3); overrides[8]=raw;
  CHECK(!ov02a10_s_stream(&sensor.subdev,1));
  struct ov02a10_start_record *r=&sensor.start_record;
  CHECK(r->required_match_mask==1791 && r->required_readback_complete);
  CHECK(!r->rotation_verified && r->orientation_write_value==(rotate?3:0));
  CHECK(r->orientation_readback_match==(raw==(rotate?3:0)));
  CHECK(r->readback_complete==r->orientation_readback_match);
  CHECK(r->expected_match_mask==(r->orientation_readback_match?2047:1791));
  CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 }

 for(unsigned slot=0;slot<11;slot++) for(int bit=0;bit<8;bit++) {
  init_sensor(false,4);
  int expected[]={1,174,1,64,0,166,0,0,0,4,0}; overrides[slot]=expected[slot]^(1<<bit);
  int ret=ov02a10_s_stream(&sensor.subdev,1);
  CHECK(ret==((slot==0 || slot==2 || slot==4 || slot==6)?-EAGAIN:-EUCLEAN));
  CHECK(read_count==11 && !starts && usage==0 && sensor.start_record.read_valid_mask==2047);
  CHECK(!(sensor.start_record.expected_match_mask & BIT(slot)));
 }
 init_sensor(false,4); controls[0].current=255;
 CHECK(!ov02a10_s_stream(&sensor.subdev,1)); CHECK(sensor.start_record.reads[1].raw==255 && sensor.start_record.reads[1].valid); CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 init_sensor(false,4); overrides[0]=256; CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EPROTO); CHECK(!sensor.start_record.reads[0].valid && sensor.start_record.reads[0].raw==-1);
 init_sensor(false,4); resume_result=-EHOSTDOWN;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EHOSTDOWN); CHECK(!op_count && !pm_put_count && !usage && sensor.start_record.pm_get_error==-EHOSTDOWN);
 resume_result=0; CHECK(!ov02a10_s_stream(&sensor.subdev,1)); CHECK(sensor.start_record.epoch==2); CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 read_count=0; op_count=0; setup_override=-EINVAL;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EINVAL); CHECK(sensor.start_record.epoch==3 && !sensor.start_record.stream_started && !sensor.start_record.read_valid_mask && sensor.start_record.setup_error==-EINVAL);
 sensor.stream_attempt_epoch=U64_MAX; int oldop=op_count, oldresume=resumes;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EOVERFLOW); CHECK(sensor.start_record.epoch_overflow && sensor.start_record.epoch==U64_MAX && sensor.start_record.first_error==-EOVERFLOW && !sensor.start_record.stream_started);
 CHECK(op_count==oldop && resumes==oldresume);
}

static void test_pm_and_stop(void)
{
 for(int which=0;which<3;which++) {
  init_sensor(false,4); CHECK(!ov02a10_s_stream(&sensor.subdev,1));
  int old=op_count; if(which<2) faults[old+1+which]=-EIO; else put_result=-EACCES;
  CHECK(ov02a10_s_stream(&sensor.subdev,0)==(which<2?-EIO:-EACCES));
  CHECK(!sensor.streaming && usage==0 && sensor.start_record.stop_seen && sensor.start_record.stream_started);
  CHECK(sensor.start_record.stop_error==(which<2?-EIO:0));
  old=op_count; CHECK(!ov02a10_s_stream(&sensor.subdev,0)); CHECK(op_count==old);
 }
 for(int result=-1;result<=1;result++) {
  init_sensor(false,4); ctrl_pm_result=result; active=true; usage=0;
  CHECK(ov02a10_set_ctrl(&controls[2])==result*(result<0));
  CHECK(op_count==(result>0?4:0) && pm_put_count==(result>0?1:0) && !usage && range_changes==1);
 }
 init_sensor(false,4); mutex_lock(&sensor.mutex); CHECK(!ov02a10_record_begin(&sensor));
 CHECK(ov02a10_record_readback(&sensor)==-EHOSTDOWN); CHECK(!op_count); mutex_unlock(&sensor.mutex);
}

static void test_export_and_format(void)
{
 init_sensor(false,4);
 struct ov02a10_start_record out, sentinel;
 memset(&sentinel,0xa5,sizeof(sentinel)); out=sentinel;
 CHECK(ov02a10_get_start_record(&dev,2,&out,sizeof(out))==-ENODEV); CHECK(!memcmp(&out,&sentinel,sizeof(out)));
 sysfs_result=-ENOMEM; CHECK(ov02a10_record_publish(&sensor)==-ENOMEM); CHECK(ov02a10_record_registry.next==&ov02a10_record_registry);
 sysfs_result=0; CHECK(!ov02a10_record_publish(&sensor));
 CHECK(!ov02a10_get_start_record(&dev,2,&out,sizeof(out))); CHECK(!out.epoch && !out.attempt_valid && out.phase==OV02A10_RECORD_NEVER_STARTED && out.reads[0].raw==-1);
 out=sentinel; CHECK(ov02a10_get_start_record(&dev,1,&out,sizeof(out))==-EPROTONOSUPPORT); CHECK(!memcmp(&out,&sentinel,sizeof(out)));
 CHECK(ov02a10_get_start_record(&dev,2,&out,sizeof(out)-1)==-EMSGSIZE); CHECK(!memcmp(&out,&sentinel,sizeof(out)));
 CHECK(ov02a10_get_start_record(NULL,2,&out,sizeof(out))==-EINVAL); CHECK(!memcmp(&out,&sentinel,sizeof(out)));
 CHECK(ov02a10_get_start_record(&dev,2,NULL,sizeof(out))==-EINVAL);
 struct device wrong={0}; CHECK(ov02a10_get_start_record(&wrong,2,&out,sizeof(out))==-ENODEV); CHECK(!memcmp(&out,&sentinel,sizeof(out)));
 CHECK(!ov02a10_s_stream(&sensor.subdev,1)); CHECK(!ov02a10_s_stream(&sensor.subdev,0));
 int ops=op_count, pm=pm_get_count+pm_put_count+resumes;
 struct { unsigned char before; char text[PAGE_SIZE]; unsigned char after; } buffer;
 buffer.before=0x5a; buffer.after=0xa5;
 CHECK(!ov02a10_get_start_record(&dev,2,&out,sizeof(out)));
 ssize_t n=telemetry_show(&dev,NULL,buffer.text); CHECK(n>0 && n<PAGE_SIZE-1 && n==(ssize_t)strlen(buffer.text));
 CHECK(buffer.text[n-1]=='\n' && buffer.before==0x5a && buffer.after==0xa5);
 CHECK(strstr(buffer.text,"applied_frame=unknown") && strstr(buffer.text,"read_valid_mask=2047"));
 CHECK(op_count==ops && pm==pm_get_count+pm_put_count+resumes && !memcmp(&out,&sensor.start_record,sizeof(out)));
 /* Maximum-width field values, including impossible states, challenge the
  * sysfs formatter capacity independently of the normal-state validator.
  */
 memset(&out,0xff,sizeof(out));
 out.first_error=out.pm_get_error=out.setup_error=out.page_select_error=out.page_restore_error=out.stream_on_error=INT32_MIN;
 out.cleanup_error=out.stop_error=out.pm_put_error=out.exposure_raw=out.vts_delta_raw=out.driver_total_lines=INT32_MIN;
 for(unsigned i=0;i<4;i++) { out.controls[i].minimum=INT64_MIN; out.controls[i].maximum=INT64_MIN; out.controls[i].step=INT64_MIN; out.controls[i].default_value=INT64_MIN; out.controls[i].value=INT32_MIN; }
 for(unsigned i=0;i<11;i++) { out.reads[i].raw=INT32_MIN; out.reads[i].error=INT32_MIN; }
 n=ov02a10_record_format(buffer.text,&out); CHECK(n<PAGE_SIZE-1 && n==(ssize_t)strlen(buffer.text) && buffer.text[n-1]=='\n');
 CHECK(buffer.before==0x5a && buffer.after==0xa5); if(n>max_format_bytes) max_format_bytes=(int)n;
 ov02a10_record_unpublish(&sensor); CHECK(sysfs_removes==1);
 CHECK(telemetry_show(&dev,NULL,buffer.text)==-ENODEV);
}

static void test_multiple_instances(void)
{
 init_sensor(false,4);
 struct ov02a10 second={0}; struct device other={0}; struct ov02a10_start_record r;
 second.dev=&other; second.probe_instance_ns=sensor.probe_instance_ns+1;
 mutex_init(&second.mutex); INIT_LIST_HEAD(&second.telemetry_entry); ov02a10_record_clear(&second);
 ov02a10_record_register(&sensor); ov02a10_record_register(&second);
 CHECK(!ov02a10_get_start_record(&other,2,&r,sizeof(r)) && r.probe_instance_ns==second.probe_instance_ns);
 CHECK(!ov02a10_get_start_record(&dev,2,&r,sizeof(r)) && r.probe_instance_ns==sensor.probe_instance_ns);
 ov02a10_record_unregister(&second);
 CHECK(ov02a10_get_start_record(&other,2,&r,sizeof(r))==-ENODEV);
 CHECK(!ov02a10_get_start_record(&dev,2,&r,sizeof(r)));
 ov02a10_record_unregister(&sensor); mutex_destroy(&second.mutex);
}

static void *getter_run(void *unused)
{
 (void)unused; getter_thread=true;
 struct ov02a10_start_record r;
 CHECK(!ov02a10_get_start_record(&dev,2,&r,sizeof(r)));
 CHECK(r.schema==2 && !r.attempt_valid); return NULL;
}
static void *unpublish_run(void *unused)
{
 (void)unused; atomic_store(&unpublish_started,true);
 ov02a10_record_unpublish(&sensor); atomic_store(&unpublish_done,true); return NULL;
}
static void test_concurrent_getter_remove(void)
{
 for(unsigned i=0;i<128;i++) {
  init_sensor(false,4); CHECK(!ov02a10_record_publish(&sensor));
  atomic_store(&getter_has_registry,false); atomic_store(&unpublish_started,false); atomic_store(&unpublish_done,false);
  mutex_lock(&sensor.mutex);
  pthread_t getter,remover; CHECK(!pthread_create(&getter,NULL,getter_run,NULL));
  while(!atomic_load(&getter_has_registry)) sched_yield();
  CHECK(!pthread_create(&remover,NULL,unpublish_run,NULL));
  while(!atomic_load(&unpublish_started)) sched_yield();
  CHECK(!atomic_load(&unpublish_done));
  mutex_unlock(&sensor.mutex);
  CHECK(!pthread_join(getter,NULL)); CHECK(!pthread_join(remover,NULL));
  CHECK(atomic_load(&unpublish_done) && sysfs_removes==1);
  struct ov02a10_start_record r; CHECK(ov02a10_get_start_record(&dev,2,&r,sizeof(r))==-ENODEV);
 }
}

int main(void)
{
 test_success_and_idempotence(); test_each_transfer_failure(); test_read_values_and_epochs();
 test_pm_and_stop(); test_export_and_format(); test_multiple_instances(); test_concurrent_getter_remove();
 if(sensor_initialized) mutex_destroy(&sensor.mutex);
 CHECK(!held_locks);
 printf("{\"checks\":%lu,\"record_size\":%zu,\"max_format_bytes\":%d,\"getter_remove_interleavings\":128}\n",atomic_load(&checks),sizeof(struct ov02a10_start_record),max_format_bytes);
 return 0;
}
