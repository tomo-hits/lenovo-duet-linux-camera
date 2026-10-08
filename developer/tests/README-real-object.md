# Real GStreamer default regression (MIT)

Build test-real-object-default.c with the target GStreamer development headers:

```
cc -O2 -Wall -Wextra test-real-object-default.c $(pkg-config --cflags --libs gstreamer-1.0) -o test-real-object-default
GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0= GST_REGISTRY_FORK=no ./test-real-object-default /absolute/old/libgstpipewire.so 0
GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0= GST_REGISTRY_FORK=no ./test-real-object-default /absolute/new/libgstpipewire.so 1
```

Use separate processes and the canonical libgstpipewire.so filename in each
staging directory; GStreamer derives the entry-point symbol from that name.
The old plugin includes the ineffective declared-copy default; the new includes
the real-copy-default-v3 correction. Three actual objects per plugin check getter
values and explicit copy/pool overrides, then destruction. This does not model
real camera streaming and cannot substitute for app reuse/endurance acceptance.
Runtime shared-library dependencies must be installed or in LD_LIBRARY_PATH.
