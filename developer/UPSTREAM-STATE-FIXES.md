# Shared stack fixes (MIT)

GStreamer state callbacks: exact changes from official PipeWire1.6.9:
https://github.com/PipeWire/pipewire/tree/1.6.9/src/gst

Core rollback: two preparing-flag reset lines from:
https://github.com/PipeWire/pipewire/commit/b41d117609ffa2dcb9c80adecdf0a84934e5b076

The old declared always-copy=true was ineffective: real init selected AUTO
and the factory-created object's getter returned false. real-copy-default-v3
ties DEFAULT_USE_BUFFERPOOL to DEFAULT_ALWAYS_COPY; explicit setters remain.
This is a generic RAM CPU-copy profile using the existing upstream copy path,
not a new camera-specific buffer algorithm. Apps can explicitly override it;
the CPU-copy profile has a bandwidth/copy cost compared with buffer sharing.
Native real-object defaults/overrides/destruction verified old0/new1. The
source-function regressions are distinct from real-object and camera tests.

Accepted RAM composition: original SPApreview3 + upstream core linkreset2 +
Gst realcopy3/state fixes + standard node.pause-on-idle=true. Two sessions with
16 rapid switch clicks/8 photos/normal closes, front >=30min/HW15.015fps/Gtk
15.0146fps, ordinary no-preload reopen, qcam3200x2400→same-WP front/rear reuse,
deep/s2idle resume and ordinary recapture all passed on 2026-10-05. Gtk signals
are numeric, not compositor presentation or optical latency measurements.

Original post-endurance empty-buffer and separate CONFIG-timeout failures
remain preserved. Standalone pause and SPA4 experiments were not adopted.
Successful composition tests do not prove universal race freedom, long-term
reliability, fully calibrated AF/colour, cold boot or upstream acceptance.
