// SPDX-License-Identifier: GPL-2.0-only
struct fixture {struct integrated_state state;u8 *raw;pthread_t publisher;bool publishing;int start_error,stop_error;unsigned int starts,stops;};
static void *publish(void *arg)
{
 struct fixture *f=arg;
 assert(!dcv_deliver(&f->state.video,f->raw,DCV_RAW_INPUT_BYTES,37,123456));
 return NULL;
}
static int on(void *arg){struct fixture *f=arg;f->starts++;return f->start_error;}
static int off(void *arg){struct fixture *f=arg;f->stops++;if(f->publishing){assert(!pthread_join(f->publisher,NULL));f->publishing=false;}return f->stop_error;}
static void *stop_thread(void *arg){struct fixture *f=arg;stop(&f->state.video.queue);return NULL;}
static void fixture_init(struct fixture *f)
{
 memset(f,0,sizeof(*f));struct dcv_video *v=&f->state.video;
 mutex_init(&v->buffers_lock);INIT_LIST_HEAD(&v->buffers);v->queue.priv=v;v->context=f;v->start=on;v->stop=off;
 f->raw=malloc(DCV_RAW_INPUT_BYTES);assert(f->raw);memset(f->raw,0xff,DCV_RAW_INPUT_BYTES);
}
static void buffer_init(struct dcv_buffer *b,struct dcv_video *v)
{
 memset(b,0,sizeof(*b));b->vb.vb2_buf.vb2_queue=&v->queue;
 b->vb.vb2_buf.capacity=DCV_BYTES;b->vb.vb2_buf.out=malloc(DCV_BYTES);assert(b->vb.vb2_buf.out);
 INIT_LIST_HEAD(&b->link);assert(!prepare(&b->vb.vb2_buf));queue_buffer(&b->vb.vb2_buf);
}
static void format_graph_controls(void)
{
 struct fixture f;fixture_init(&f);struct dcv_video *v=&f.state.video;
 unsigned int n=1,planes=0,sizes[1]={0};assert(!setup(&v->queue,&n,&planes,sizes,NULL));assert(n==4&&planes==1&&sizes[0]==3995136);
 sizes[0]--;assert(setup(&v->queue,&n,&planes,sizes,NULL)==-EINVAL);planes=2;assert(setup(&v->queue,&n,&planes,sizes,NULL)==-EINVAL);
 struct vb2_buffer bad={.capacity=DCV_BYTES-1};assert(prepare(&bad)==-EINVAL);
 struct file file={.priv=v};assert(open_video(&file)==-ENODEV&&open_calls==0);smp_store_release(&v->registered,true);assert(!open_video(&file)&&open_calls==1);
 struct v4l2_format fmt={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE};assert(!format(&file,NULL,&fmt));
 assert(fmt.fmt.pix.width==1632&&fmt.fmt.pix.height==1224&&fmt.fmt.pix.bytesperline==3264&&fmt.fmt.pix.sizeimage==3995136&&fmt.fmt.pix.pixelformat==V4L2_PIX_FMT_SRGGB10&&fmt.fmt.pix.colorspace==V4L2_COLORSPACE_RAW&&fmt.fmt.pix.xfer_func==V4L2_XFER_FUNC_NONE);
 v->queue.busy=true;assert(setfmt(&file,NULL,&fmt)==-EBUSY);v->queue.busy=false;assert(!setfmt(&file,NULL,&fmt));fmt.type=99;assert(format(&file,NULL,&fmt)==-EINVAL);
 struct v4l2_subdev_state active={0},trial={0};struct v4l2_subdev *sd=&f.state.graph.subdev;
 assert(!graph_init_state(sd,&active)&&!graph_init_state(sd,&trial));
 struct v4l2_subdev_format sf={.pad=0,.which=V4L2_SUBDEV_FORMAT_TRY,.format={.width=800,.height=600}};
 assert(!graph_set_fmt(sd,&trial,&sf));assert(trial.fmt[1].width==1632&&active.fmt[0].width==1632);
 sf.pad=1;assert(!graph_get_fmt(sd,&trial,&sf)&&sf.format.code==MEDIA_BUS_FMT_SRGGB10_1X10);
 sf.pad=2;assert(graph_get_fmt(sd,&active,&sf)==-EINVAL&&graph_set_fmt(sd,&active,&sf)==-EINVAL);
 sf.pad=0;sf.stream=1;assert(graph_set_fmt(sd,&active,&sf)==-EINVAL);sf.stream=0;
 WRITE_ONCE(v->running,true);sf.which=V4L2_SUBDEV_FORMAT_ACTIVE;assert(graph_set_fmt(sd,&active,&sf)==-EBUSY);sf.which=V4L2_SUBDEV_FORMAT_TRY;assert(!graph_set_fmt(sd,&trial,&sf));WRITE_ONCE(v->running,false);
 struct v4l2_subdev_mbus_code_enum code={0};assert(!graph_enum_code(sd,&trial,&code));code.index=4;assert(graph_enum_code(sd,&trial,&code)==-EINVAL);
 struct v4l2_subdev_frame_size_enum size={.code=MEDIA_BUS_FMT_SRGGB10_1X10};assert(!graph_enum_size(sd,&trial,&size)&&size.min_width==1632&&size.max_height==1224);size.pad=2;assert(graph_enum_size(sd,&trial,&size)==-EINVAL);
 struct media_link link={.graph_obj.mdev=&f.state.graph.media};assert(!graph_link_notify(&link,MEDIA_LNK_FL_ENABLED,MEDIA_DEV_NOTIFY_PRE_LINK_CH));WRITE_ONCE(v->running,true);assert(graph_link_notify(&link,MEDIA_LNK_FL_ENABLED,MEDIA_DEV_NOTIFY_PRE_LINK_CH)==-EBUSY);assert(graph_link_notify(&link,0,MEDIA_DEV_NOTIFY_PRE_LINK_CH)==-EBUSY);WRITE_ONCE(v->running,false);
 struct v4l2_subdev upstream={0};graph_format(&upstream.format);struct media_entity entity={.subdev=true,.sd=&upstream};struct media_pad source={.entity=&entity,.index=1};link.source=&source;struct media_pad sink={.entity=&v->vdev.entity};link.sink=&sink;
 assert(!validate_link(&link));upstream.format.width=800;assert(validate_link(&link)==-EPIPE);graph_format(&upstream.format);upstream.format.code++;assert(validate_link(&link)==-EPIPE);graph_format(&upstream.format);upstream.format.field=1;assert(validate_link(&link)==-EPIPE);upstream.get_error=-EIO;assert(validate_link(&link)==-EIO);entity.subdev=false;assert(validate_link(&link)==-EINVAL);
 for(unsigned i=0;i<4;i++) {
  fmt.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;fmt.fmt.pix.pixelformat=dcv_bayer_pixels[i];
  assert(!setfmt(&file,NULL,&fmt));assert(v->pixel_format==dcv_bayer_pixels[i]);
  fmt.fmt.pix.pixelformat=0;assert(!getfmt(&file,NULL,&fmt)&&fmt.fmt.pix.pixelformat==dcv_bayer_pixels[i]);
  upstream.get_error=0;entity.subdev=true;graph_format(&upstream.format);upstream.format.code=dcv_bayer_codes[i];
  assert(!validate_link(&link));upstream.format.code=dcv_bayer_codes[(i+1)%4];assert(validate_link(&link)==-EPIPE);
  sf.which=V4L2_SUBDEV_FORMAT_TRY;sf.pad=0;sf.format.code=dcv_bayer_codes[i];assert(!graph_set_fmt(sd,&trial,&sf));
  assert(trial.fmt[1].code==dcv_bayer_codes[i]);
 }
 struct v4l2_ctrl_handler controls={.ctrl={{1,5000,190,7151},{2,240,4,7150},{3,80,16,248},{4,0,0,1}}};struct v4l2_subdev sensor={.ctrl_handler=&controls};f.state.graph.sensor=&sensor;
 assert(!capture_conditions(&f.state)&&f.state.requested_exposure==240&&f.state.requested_gain==80);
 controls.ctrl[1].value=430;controls.ctrl[2].value=64;assert(!capture_conditions(&f.state)&&f.state.requested_exposure==430&&f.state.requested_gain==64);
 int blanks[]={190,191,470,887,1258,1580,1583,7151};
 for(unsigned int i=0;i<sizeof(blanks)/sizeof(blanks[0]);i++){
  controls.ctrl[0].value=blanks[i];assert(!capture_conditions(&f.state));
  assert(controls.ctrl[0].value==blanks[i]&&controls.ctrl[1].value==430&&controls.ctrl[2].value==64);
 }
 controls.ctrl[0].value=189;assert(capture_conditions(&f.state)==-EINVAL&&controls.ctrl[0].value==189);
 controls.ctrl[0].value=7152;assert(capture_conditions(&f.state)==-EINVAL&&controls.ctrl[0].value==7152);
 /* Different sensor/mode ranges must remain owned by the real control. */
 controls.ctrl[0].minimum=24;controls.ctrl[0].maximum=6000;controls.ctrl[0].value=24;assert(!capture_conditions(&f.state));
 controls.ctrl[0].value=23;assert(capture_conditions(&f.state)==-EINVAL);
 controls.ctrl[0].minimum=4000;controls.ctrl[0].maximum=5000;controls.ctrl[0].value=3999;assert(capture_conditions(&f.state)==-EINVAL);
 controls.ctrl[0].value=4000;assert(!capture_conditions(&f.state));controls.ctrl[0].value=5000;controls.ctrl[3].value=1;assert(capture_conditions(&f.state)==-EINVAL&&controls.ctrl[3].value==1);controls.ctrl[3].id=0;assert(capture_conditions(&f.state)==-ENOENT);f.state.sensor_on=true;assert(capture_conditions(&f.state)==-EPERM);
 free(f.raw);pthread_mutex_destroy(&v->buffers_lock.lock);
}
static void lifecycle(void)
{
 struct fixture f;fixture_init(&f);struct dcv_video *v=&f.state.video;struct dcv_buffer b;
 buffer_init(&b,v);pipeline_error=-ENOLINK;assert(start(&v->queue,1)==-ENOLINK&&!v->blocked&&!v->running&&!f.starts&&b.vb.vb2_buf.state==VB2_BUF_STATE_QUEUED);
 pipeline_error=0;queue_buffer(&b.vb.vb2_buf);assert(!start(&v->queue,1));publish(&f);
 assert(b.vb.sequence==37&&b.vb.vb2_buf.timestamp==123456&&b.vb.vb2_buf.payload==DCV_BYTES&&b.vb.vb2_buf.done==2&&b.vb.vb2_buf.state==VB2_BUF_STATE_DONE&&v->delivered==1);
 assert(b.vb.vb2_buf.out[0]==255&&b.vb.vb2_buf.out[1]==3&&b.vb.vb2_buf.out[DCV_BYTES-1]==3);
 assert(!dcv_deliver(v,f.raw,DCV_RAW_INPUT_BYTES,38,123457)&&v->dropped==1);
 stop(&v->queue);assert(!v->running&&!v->pipeline_started&&!v->blocked&&f.stops==1);
 queue_buffer(&b.vb.vb2_buf);assert(!start(&v->queue,1));u8 *allocated=b.vb.vb2_buf.out;b.vb.vb2_buf.out=NULL;
 struct dma_buf imported={0};b.vb.vb2_buf.planes[0].dbuf=&imported;
 assert(dcv_deliver(v,f.raw,DCV_RAW_INPUT_BYTES,1,1)==-EFAULT&&b.vb.vb2_buf.state==VB2_BUF_STATE_ERROR&&!b.vb.vb2_buf.payload);stop(&v->queue);
 assert(imported.begins==1&&imported.ends==1);
 b.vb.vb2_buf.out=allocated;queue_buffer(&b.vb.vb2_buf);assert(!start(&v->queue,1));publish(&f);stop(&v->queue);assert(imported.begins==2&&imported.ends==2);
 imported.begin_error=-EACCES;queue_buffer(&b.vb.vb2_buf);assert(!start(&v->queue,1));assert(dcv_deliver(v,f.raw,DCV_RAW_INPUT_BYTES,1,1)==-EACCES);stop(&v->queue);assert(imported.begins==3&&imported.ends==2&&!b.vb.vb2_buf.payload);
 imported.begin_error=0;imported.end_error=-EIO;queue_buffer(&b.vb.vb2_buf);assert(!start(&v->queue,1));assert(dcv_deliver(v,f.raw,DCV_RAW_INPUT_BYTES,1,1)==-EIO);stop(&v->queue);assert(imported.begins==4&&imported.ends==3&&!b.vb.vb2_buf.payload);
 assert(dcv_deliver(v,f.raw,DCV_RAW_INPUT_BYTES-1,1,1)==-EINVAL);
 free(allocated);free(f.raw);pthread_mutex_destroy(&v->buffers_lock.lock);
 struct fixture failed;fixture_init(&failed);v=&failed.state.video;buffer_init(&b,v);failed.start_error=-117;assert(start(&v->queue,1)==-117&&v->blocked&&!v->running&&!v->pipeline_started&&b.vb.vb2_buf.state==VB2_BUF_STATE_QUEUED);unsigned int attempts=failed.starts;queue_buffer(&b.vb.vb2_buf);assert(start(&v->queue,1)==-117&&failed.starts==attempts);free(b.vb.vb2_buf.out);free(failed.raw);pthread_mutex_destroy(&v->buffers_lock.lock);
 struct fixture fault;fixture_init(&fault);v=&fault.state.video;buffer_init(&b,v);assert(!start(&v->queue,1));fault.stop_error=-EIO;stop(&v->queue);assert(v->blocked&&v->last_error==-EIO&&b.vb.vb2_buf.state==VB2_BUF_STATE_ERROR);free(b.vb.vb2_buf.out);free(fault.raw);pthread_mutex_destroy(&v->buffers_lock.lock);
}
static void held_publisher_close(void)
{
 for(unsigned int i=0;i<8;i++){
  struct fixture f;fixture_init(&f);struct dcv_video *v=&f.state.video;struct dcv_buffer b;buffer_init(&b,v);assert(!start(&v->queue,1));
  pthread_mutex_lock(&lease_lock);lease_hold=true;lease_entered=lease_release=false;pthread_mutex_unlock(&lease_lock);
  f.publishing=true;assert(!pthread_create(&f.publisher,NULL,publish,&f));pthread_mutex_lock(&lease_lock);while(!lease_entered)pthread_cond_wait(&lease_cond,&lease_lock);pthread_mutex_unlock(&lease_lock);
  pthread_t closer;assert(!pthread_create(&closer,NULL,stop_thread,&f));
  while(READ_ONCE(v->running)){} // Admission closes while publisher still owns its private lease.
  assert(!b.vb.vb2_buf.done);
  pthread_mutex_lock(&lease_lock);lease_release=true;pthread_cond_broadcast(&lease_cond);pthread_mutex_unlock(&lease_lock);assert(!pthread_join(closer,NULL));
  assert(b.vb.vb2_buf.done==1&&b.vb.vb2_buf.state==VB2_BUF_STATE_DONE&&!v->pipeline_started&&!v->running&&!v->blocked);
  lease_hold=false;queue_buffer(&b.vb.vb2_buf);assert(!start(&v->queue,1));publish(&f);stop(&v->queue);assert(b.vb.vb2_buf.done==2);
  free(b.vb.vb2_buf.out);free(f.raw);pthread_mutex_destroy(&v->buffers_lock.lock);
 }
}
static void filtered_enumeration(void){struct v4l2_fmtdesc d={0};
 for(unsigned i=0;i<4;i++){
  d=(struct v4l2_fmtdesc){.index=i};assert(!enumfmt(NULL,NULL,&d)&&d.pixelformat==dcv_bayer_pixels[i]);
  d=(struct v4l2_fmtdesc){.mbus_code=dcv_bayer_codes[i]};assert(!enumfmt(NULL,NULL,&d)&&d.pixelformat==dcv_bayer_pixels[i]);d.index=1;assert(enumfmt(NULL,NULL,&d)==-EINVAL);
 }
 d=(struct v4l2_fmtdesc){.mbus_code=1};assert(enumfmt(NULL,NULL,&d)==-EINVAL);d=(struct v4l2_fmtdesc){.index=4};assert(enumfmt(NULL,NULL,&d)==-EINVAL);
}

int main(void)
{filtered_enumeration();
 format_graph_controls();lifecycle();held_publisher_close();
 assert(pipeline_starts==pipeline_stops+1); // One topology rejection never started a pipeline.
 puts("PASS: actual RAW frontend/pad/control functions; sensor cache preserved; topology/buffer faults; 8 held-publisher close/reopen races");return 0;
}
