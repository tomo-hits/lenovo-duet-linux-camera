# Recording frame timing (Issue #1)

English | [日本語](RECORDING_DIAGNOSTICS.ja.md)

<!-- SPDX-License-Identifier: CC0-1.0 -->

Snapshot recording stutter is still under investigation. The source changes here
correct delivery accounting and provide measurements; they do not establish a
recording-performance fix or change the signed v1.0.2 packages. New driver
diagnostics require a candidate built for the matched kernel using the
[build guide](BUILD.md) and the normal installation/activation procedure.

## Compare the same scene

Record the installed Snapshot, libcamera, PipeWire/GStreamer, module and kernel
versions, selected camera, lighting, negotiated size/fps/pixel format, actual
encoder and its settings, and playback application. Determine the selected
SoftISP implementation and EGL renderer: the `SWIspWorker` thread name alone does
not distinguish CPU debayer, hardware EGL or software rendering.

For each camera, compare a short preview-only session, an encode-free
PipeWire/GStreamer receiver, and a recording under the same conditions. Collect
per-thread CPU, temperature/throttling, queue/QoS events, buffer return times and
write load. Compare source and encoder/muxer presentation timestamps, not only
nominal fps or the number of sensor interrupts. Keep camera media on the device.

The packaged WirePlumber profile pins a size and frame rate (normally
1536x864/30 fps). The SPA patch advertises that size only and rejects another
size. A downstream caps filter alone cannot select a different camera mode.
Use a temporary, validated node profile and normal stop/restart for a 15 fps or
lower-resolution comparison, then restore the original profile. A smaller
output can reduce encoding cost while retaining the same RAW capture geometry.

## Driver delivery counters

When debugfs is already available, the candidate provides a read-only, root-only
`/sys/kernel/debug/mt8183_p1/<platform-device>/video_result` snapshot. It contains
numeric counters only and does not start a stream. Read it before and after the
recording interval, together with `capture_result` and `session_result`.

| Field | Meaning |
| --- | --- |
| `stream_id` | Frontend stream attempt; compare deltas only within one stream ID |
| `running` | Frontend admission state; not proof that sensor frames are arriving |
| `delivered` | Buffers completed successfully to VB2 during this stream |
| `no_buffer` | Frames discarded because the active stream had no queued VB2 buffer |
| `stopping` | Frames discarded after frontend admission closed |
| `errors` | Invalid frame input or errors while preparing/converting/completing a frame buffer |
| `total_delivered`, `total_dropped` | Lifetime totals; dropped includes no-buffer and stopping discards |

Per-stream counters reset before the start callback can produce frames; lifetime
totals persist across reopening. A failed start can also consume a stream ID.
The STOP log reports the final counters after workers have joined. A successful
VB2 completion still does not prove delivery to Snapshot or successful encoding.

The backend separately records completion of the delivery attempt and actual
VB2 delivery. A benign discard completes the protocol transaction without
claiming delivery or causing capture failure. `archived` is a protocol reclamation
count, not the number of frames received by an application. DMA copy/compare,
ownership checks, SCP isolation and the normal STOP sequence remain required.

## Analyze a local recording

With Python 3 and ffprobe installed, run from the `developer` directory:

```sh
python3 scripts/analyze-video-timing.py --video /path/to/recording.webm
python3 scripts/analyze-video-timing.py --video /path/to/recording.webm --expected-fps 30/1
```

Use `--expected-fps` only with the measured/negotiated target, not an assumed
default. Alternatively, supply ffprobe JSON via `--json /path/to/frames.json`
or standard input with `--json -`. Generate it locally with:

```sh
ffprobe -v error -select_streams v:0 -show_frames -show_entries frame=best_effort_timestamp_time,pts_time -of json /path/to/recording.webm > /tmp/duet-recording-frames.json
python3 scripts/analyze-video-timing.py --json /tmp/duet-recording-frames.json
```

The analyzer reports observed presentation spacing and anomalous/missing
timestamps. Gap estimates against an expected cadence are diagnostic: variable
frame rate, timestamp rounding, encoder decisions and playback load must be
considered. Regular timestamps do not detect repeated image content, and a gap
alone does not locate a dropped frame in the capture/encoding pipeline.

## Acceptance before a performance claim

Repeat front/rear recordings at the declared attainable rate, check saved-video
spacing and visible motion, and verify that queue latency does not accumulate.
Test early stop, normal close, reopening in a new process, and camera switching.
Confirm the existing final STOP/SCP ownership state. Treat a stable 15 fps
profile as a separately declared mode, not evidence that 30 fps recording works.

Host regression commands are in the [test guide](../tests/README.md). They use
synthetic buffers/provider responses and do not replace matched ARM64/KCFI
builds or actual recording tests.
