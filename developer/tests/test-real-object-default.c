/* SPDX-License-Identifier: MIT */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
static void check(GstElement *e, int expected, const char *stage) {
 gboolean copy=FALSE, pool=FALSE;
 g_object_get(e, "always-copy", &copy, "use-bufferpool", &pool, NULL);
 printf("%s always-copy=%d use-bufferpool=%d\n",stage,copy,pool);
 g_assert_cmpint(copy, ==, expected);
 g_assert_cmpint(pool, ==, !expected);
}
int main(int argc, char **argv) {
 g_assert_cmpint(argc, ==, 3);
 int expected=atoi(argv[2]);
 gst_init(NULL,NULL);
 GError *error=NULL;
 GstPlugin *p=gst_plugin_load_file(argv[1],&error);
 if(!p){g_printerr("plugin: %s\n",error?error->message:"unknown");return 2;}
 const char *version=gst_plugin_get_version(p);
 printf("loaded=%s version=%s\n",gst_plugin_get_filename(p),version);
 for(int i=0;i<3;i++) {
  GstElement *e=gst_element_factory_make("pipewiresrc",NULL);
  g_assert_nonnull(e);
  GParamSpec *spec=g_object_class_find_property(G_OBJECT_GET_CLASS(e),"always-copy");
  g_assert_true(G_IS_PARAM_SPEC_BOOLEAN(spec));
  printf("declared always-copy=%d\n",G_PARAM_SPEC_BOOLEAN(spec)->default_value);
  check(e,expected,"factory-default");
  g_object_set(e,"always-copy",FALSE,NULL);check(e,0,"explicit-copy-false");
  g_object_set(e,"always-copy",TRUE,NULL);check(e,1,"explicit-copy-true");
  g_object_set(e,"use-bufferpool",TRUE,NULL);check(e,0,"explicit-pool-true");
  g_object_set(e,"use-bufferpool",FALSE,NULL);check(e,1,"explicit-pool-false");
  gst_object_unref(e);
 }
 gst_object_unref(p);gst_deinit();puts("PASS real GStreamer object defaults, explicit overrides and destruction");return 0;
}
