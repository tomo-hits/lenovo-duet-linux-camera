/* Modified on 2026-10-05: explicit publication license. */
/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual s_ctrl and clustered write paths; synthetic bus/PM only. */
int main(void)
{
 unsigned cold_positions=0,live_positions=0;
 init_sensor(true,3);
 CHECK(!ov02a10_s_stream(&sensor.subdev,1));
 unsigned total=op_count;
 CHECK(total==123 && sensor.streaming && usage==1);
 CHECK(sensor.startup_settle_ns>=140000000 && sensor.start_record.required_readback_complete);
 CHECK(sensor.start_record.exposure_raw==430 && sensor.start_record.reads[3].raw==64);
 CHECK(!ov02a10_s_stream(&sensor.subdev,0) && usage==0);
 for(unsigned fail=1;fail<=total;fail++) {
  init_sensor(true,3); faults[fail]=-EREMOTEIO;
  CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EREMOTEIO);
  CHECK(!sensor.streaming && usage==0 && !sensor.pace_active);
  CHECK(sensor.start_record.first_error==-EREMOTEIO && sensor.pace_record.stop.complete);
  cold_positions++;
 }
 init_sensor(true,3); overrides[1]=134;
 CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EUCLEAN);
 CHECK(sensor.start_record.exposure_raw==390 && !sensor.start_record.stream_on_attempted);
 CHECK(!sensor.streaming && usage==0);
 for(unsigned fail=0;fail<=5;fail++) {
  init_sensor(true,3); CHECK(!ov02a10_s_stream(&sensor.subdev,1));
  struct ov02a10_start_record original=sensor.start_record;
  struct ov02a10_pace_record pace=sensor.pace_record;
  unsigned before=op_count;
  if(fail) faults[before+fail]=-EREMOTEIO;
  controls[0].val=240;controls[1].val=80;
  mutex_lock(&sensor.mutex);
  int ret=ov02a10_set_ctrl(&controls[0]);
  mutex_unlock(&sensor.mutex);
  CHECK(ret==(fail?-EREMOTEIO:0));
  CHECK(!memcmp(&original,&sensor.start_record,sizeof(original)));
  CHECK(!memcmp(&pace,&sensor.pace_record,sizeof(pace)));
  CHECK(sensor.live_updates==1 && sensor.live_pace.complete && !sensor.pace_active);
  CHECK(usage==1 && sensor.streaming);
  CHECK(sensor.live_pace.end_ns-sensor.live_pace.begin_ns<10000000);
  if(!fail) {
   CHECK((registers[1][3]<<8|registers[1][4])==240 && registers[1][0x24]==80);
   CHECK((unsigned)op_count==before+5);
   CHECK(trace[before+5].address==REG_GLOBAL_EFFECTIVE);
  } else {
   unsigned previous=op_count;
   mutex_lock(&sensor.mutex); CHECK(ov02a10_set_ctrl(&controls[0])==-EREMOTEIO); mutex_unlock(&sensor.mutex);
   CHECK((unsigned)op_count==previous);live_positions++;
  }
  mutex_lock(&sensor.mutex);CHECK(ov02a10_set_ctrl(&controls[2])==(fail?-EREMOTEIO:0));mutex_unlock(&sensor.mutex);
  CHECK(!ov02a10_s_stream(&sensor.subdev,0) && usage==0);
 }
 /* New live VBLANK path: every write failure preserves the original error. */
 const unsigned blanks[]={190,191,470,887,1580,1583};
 for(unsigned timing=0;timing<sizeof(blanks)/sizeof(blanks[0]);timing++)
 for(unsigned fail=0;fail<=4;fail++) {
  init_sensor(true,3);CHECK(!ov02a10_s_stream(&sensor.subdev,1));unsigned before=op_count;
  if(fail)faults[before+fail]=-EREMOTEIO;
  controls[2].val=blanks[timing];mutex_lock(&sensor.mutex);
  CHECK(ov02a10_set_ctrl(&controls[2])==(fail?-EREMOTEIO:0));mutex_unlock(&sensor.mutex);
  CHECK(usage==1&&sensor.streaming&&!sensor.pace_active);
  if(!fail)CHECK((registers[1][OV02A10_REG_VTS_H]<<8|registers[1][OV02A10_REG_VTS_L])==(int)(blanks[timing]+1200-OV02A10_BASE_LINES));
  else {unsigned n=op_count;mutex_lock(&sensor.mutex);CHECK(ov02a10_set_ctrl(&controls[2])==-EREMOTEIO);mutex_unlock(&sensor.mutex);CHECK((unsigned)op_count==n);}
  CHECK(!ov02a10_s_stream(&sensor.subdev,0)&&!usage);
 }
 /* All four standard flip layouts, latched readback, forbidden live SET. */
 for(unsigned bits=0;bits<4;bits++) {
  init_sensor(false,3); controls[4].current=controls[4].val=bits&1;
  controls[5].current=controls[5].val=bits>>1;
  ctrl_pm_result=0;mutex_lock(&sensor.mutex);
  CHECK(!ov02a10_set_ctrl(&controls[4]));
  CHECK(sensor.fmt.code==ov02a10_bayer_code(&sensor)&&!op_count);
  mutex_unlock(&sensor.mutex);ctrl_pm_result=1;
  CHECK(!ov02a10_s_stream(&sensor.subdev,1));
  CHECK(registers[1][0x3f]==(int)bits);
  CHECK(sensor.start_record.orientation_readback_match);
  CHECK(sensor.start_record.orientation_write_value==(int)bits);
  unsigned before=op_count;
  mutex_lock(&sensor.mutex);CHECK(ov02a10_set_ctrl(&controls[4])==-EBUSY);mutex_unlock(&sensor.mutex);
  CHECK((unsigned)op_count==before);
  CHECK(!ov02a10_s_stream(&sensor.subdev,0)&&!usage);
 }
 init_sensor(true,3);overrides[8]=0;CHECK(ov02a10_s_stream(&sensor.subdev,1)==-EUCLEAN);CHECK(!sensor.streaming&&!usage&&!starts);
 /* PM-inactive standard SET changes only framework cache, without I2C. */
 init_sensor(true,3);ctrl_pm_result=0;
 mutex_lock(&sensor.mutex);CHECK(!ov02a10_set_ctrl(&controls[0]));mutex_unlock(&sensor.mutex);
 CHECK(!op_count && !usage);
 printf("{\"cold_fault_positions\":%u,\"live_write_fault_positions\":%u,\"strict_mismatch_rejected\":true,\"immutable_start_stop_records\":true,\"cluster_checks\":\"PASS\"}\n",cold_positions,live_positions);
 return 0;
}
