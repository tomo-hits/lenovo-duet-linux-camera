// SPDX-License-Identifier: GPL-2.0-only
struct fixture {struct duet_p1_graph graph;struct dcv_video video;struct completion entered,done;};
static void *cleanup(void *arg)
{
 struct fixture *f=arg;dcv_unregister(&f->video);complete(&f->entered);
 graph_cleanup(&f->graph);dcv_cleanup(&f->video);complete(&f->done);return NULL;
}
static void init(struct fixture *f,unsigned int refs)
{
 memset(f,0,sizeof(*f));f->graph.v4l2.refs=refs;f->graph.v4l2.release=graph_nodes_release;
 assert(!pthread_mutex_init(&f->graph.v4l2.lock,NULL));init_completion(&f->graph.nodes_released);init_completion(&f->entered);init_completion(&f->done);
 f->graph.media_initialized=f->graph.media_registered=f->graph.v4l2_registered=f->graph.entity_initialized=f->graph.subdev_finalized=f->graph.subdev_registered=f->graph.notifier_initialized=f->graph.notifier_registered=true;
 f->video.registered=f->video.node_registered=f->video.queue_initialized=f->video.entity_initialized=true;
}
static bool is_done(struct completion *c){pthread_mutex_lock(&c->lock);bool done=c->done;pthread_mutex_unlock(&c->lock);return done;}
static void destroy(struct fixture *f)
{
 pthread_mutex_destroy(&f->graph.v4l2.lock);
 struct completion *cs[]={&f->graph.nodes_released,&f->entered,&f->done,&base_put};
 for(unsigned int i=0;i<4;i++){pthread_mutex_destroy(&cs[i]->lock);pthread_cond_destroy(&cs[i]->cond);}
}
int main(void)
{
 for(unsigned int i=0;i<32;i++){
  struct fixture *f=malloc(sizeof(*f));assert(f);init_completion(&base_put);init(f,3);pthread_t worker;assert(!pthread_create(&worker,NULL,cleanup,f));
  wait_for_completion(&f->entered);wait_for_completion(&base_put);
  // Parent base ref has been dropped but two V4L2 core node refs remain.
  assert(!is_done(&f->done)&&f->graph.subdev_finalized&&f->graph.entity_initialized&&!f->graph.subdev.cleaned&&!f->video.queue.released&&!f->video.vdev.entity.cleaned);
  v4l2_device_put(&f->graph.v4l2);assert(!is_done(&f->done)&&!f->video.queue.released);
  // Simulate the final subdev core touching sd before it drops parent ref.
  assert(f->graph.subdev.unregistered&&!f->graph.subdev.cleaned);
  v4l2_device_put(&f->graph.v4l2);assert(!pthread_join(worker,NULL));
  assert(is_done(&f->done)&&!f->video.registered&&f->video.vdev.unregistered&&f->video.queue.released&&f->video.vdev.entity.cleaned&&f->graph.subdev.cleaned&&f->graph.media.cleaned);
  destroy(f);free(f);
 }
 struct fixture f;init_completion(&base_put);init(&f,1);cleanup(&f);assert(is_done(&f.done));destroy(&f);
 init_completion(&base_put);init(&f,1);f.graph.v4l2_registered=false;f.graph.subdev_registered=false;cleanup(&f);assert(is_done(&f.done));destroy(&f);
 puts("PASS: actual cleanup waits for final video/subdev core refs before freeing pads/state/queue; 32 delayed-release races and early/no-open unwind");return 0;
}
