#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize local ffprobe frame presentation timestamps without exporting media.

Use --video FILE, or --json FILE ("-" reads stdin). JSON must contain ffprobe's
decoded `frames`, for one video stream, with best_effort_timestamp_time and/or
pts_time. Packet DTS is deliberately not used: decoding reorders B frames before
ffprobe emits decoded frames. Input order is preserved, never sorted to hide
timestamp reversals. One timestamp field is used throughout; missing values are
not filled from another clock or interpolated.

--expected-fps 30/1 enables nominal-period gap estimates. These are diagnostics,
not proof of dropped capture frames; variable-frame-rate content can have gaps
by design. avg_frame_rate and r_frame_rate are never used as measured frame rate.
"""
import argparse
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import time

MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_FRAMES = 200_000
PROBE_TIMEOUT = 60


class TimingError(ValueError):
    """A bounded, user-readable failure without media or raw process output."""


def read_json(stream):
    payload = stream.read(MAX_JSON_BYTES + 1)
    if len(payload) > MAX_JSON_BYTES:
        raise TimingError("JSON exceeds the 16 MiB limit; analyze a shorter clip")
    try:
        return json.loads(payload)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise TimingError("invalid ffprobe JSON") from exc


def probe_video(path):
    """Read a local file with network protocols disabled; bound time and output."""
    video = Path(path).resolve()
    if not video.is_file():
        raise TimingError("--video requires an existing local regular file")
    executable = shutil.which("ffprobe")
    if not executable:
        raise TimingError("ffprobe is unavailable; install it or use --json")
    command = [executable, "-v", "error", "-protocol_whitelist", "file",
               "-select_streams", "v:0", "-show_frames",
               "-show_entries",
               "frame=media_type,stream_index,best_effort_timestamp_time,pts_time",
               "-of", "json", "-i", str(video)]
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    except OSError as exc:
        raise TimingError("could not start ffprobe") from exc
    payload = bytearray()
    deadline = time.monotonic() + PROBE_TIMEOUT
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimingError("ffprobe exceeded 60 seconds; analyze a shorter clip")
                if not selector.select(min(1, remaining)):
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                payload.extend(chunk)
                if len(payload) > MAX_JSON_BYTES:
                    raise TimingError("ffprobe JSON exceeds 16 MiB; analyze a shorter clip")
        try:
            status = process.wait(timeout=max(0.001, deadline - time.monotonic()))
        except subprocess.TimeoutExpired as exc:
            raise TimingError("ffprobe exceeded 60 seconds; analyze a shorter clip") from exc
        if status:
            raise TimingError("ffprobe could not read this video; no timing result produced")
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()
    try:
        return json.loads(payload)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise TimingError("ffprobe returned invalid JSON") from exc


def timestamp(value):
    """Return seconds or None; disallow nonfinite and pathological magnitudes."""
    if isinstance(value, bool) or not isinstance(value, (str, int, float)):
        return None
    value = str(value)
    if len(value) > 80:
        return None
    try:
        result = Decimal(value)
    except InvalidOperation:
        return None
    if (not result.is_finite() or result.copy_abs() > Decimal("1e12")
            or result.as_tuple().exponent < -12):
        return None
    return result


def expected_rate(value):
    try:
        if not re.fullmatch(r"[0-9]{1,9}(?:/[0-9]{1,9}|\.[0-9]{1,9})?", value):
            raise ValueError
        rate = Fraction(value)
        if not 0 < rate <= 1000:
            raise ValueError
        return rate
    except (ValueError, ZeroDivisionError, OverflowError) as exc:
        raise argparse.ArgumentTypeError("expected fps must be > 0 and <= 1000, e.g. 30/1") from exc


def summarize(document, expected_fps=None):
    if not isinstance(document, dict) or not isinstance(document.get("frames"), list):
        raise TimingError("JSON must contain a decoded frames array, not packets")
    raw_frames = document["frames"]
    if len(raw_frames) > MAX_FRAMES:
        raise TimingError("more than 200000 frames; analyze a shorter clip")
    if any(not isinstance(frame, dict) for frame in raw_frames):
        raise TimingError("each frames entry must be an object")
    frames = [frame for frame in raw_frames if frame.get("media_type", "video") == "video"]
    indices = [frame["stream_index"] for frame in frames if "stream_index" in frame]
    if any(isinstance(index, bool) or not isinstance(index, int) or index < 0 for index in indices):
        raise TimingError("invalid video stream index")
    if len(set(indices)) > 1:
        raise TimingError("multiple video streams; create JSON with -select_streams v:0")
    field = "best_effort_timestamp_time"
    if not any(timestamp(frame.get(field)) is not None for frame in frames):
        field = "pts_time"
    values = [timestamp(frame.get(field)) for frame in frames]
    missing = sum(value is None for value in values)
    intervals = [right - left for left, right in zip(values, values[1:])
                 if left is not None and right is not None]
    duplicates = sum(delta == 0 for delta in intervals)
    reversals = sum(delta < 0 for delta in intervals)
    positive = sorted(delta for delta in intervals if delta > 0)
    complete = len(values) >= 2 and not missing and not duplicates and not reversals
    span = values[-1] - values[0] if complete else None
    stats = None
    if positive:
        def percentile(p):
            return float(positive[max(0, math.ceil(len(positive) * p) - 1)] * 1000)
        stats = {"p50": percentile(.50), "p95": percentile(.95),
                 "p99": percentile(.99), "max": float(positive[-1] * 1000)}
    result = {
        "schema_version": 1,
        "video_frames": len(frames),
        "timestamp_field": field,
        "valid_timestamps": len(values) - missing,
        "missing_or_invalid_timestamps": missing,
        "adjacent_timestamp_pairs": len(intervals),
        "duplicate_adjacent_timestamps": duplicates,
        "backward_adjacent_timestamps": reversals,
        "positive_intervals": len(positive),
        "positive_interval_ms": stats,
        "percentile_method": "nearest rank over positive adjacent intervals",
        "presentation_span_seconds": float(span) if span is not None else None,
        "observed_frame_rate_fps": float(Decimal(len(values) - 1) / span) if span else None,
        "gap_estimate": None,
        "limitations": [
            "Decoded frame order is preserved; timestamps are not sorted. Packet DTS is not used.",
            "One timestamp field is used throughout. Missing samples break adjacent intervals.",
            "Observed fps is (frames - 1) / timestamp span only for a complete, strictly increasing timeline.",
            "Timing describes this saved video, not capture, preview rendering, or playback performance.",
            "Timing gaps do not prove lost capture frames; repeated image content cannot be detected here.",
        ],
    }
    if expected_fps is not None:
        if expected_fps <= 0 or expected_fps > 1000:
            raise TimingError("expected fps must be > 0 and <= 1000")
        period = Decimal(expected_fps.denominator) / Decimal(expected_fps.numerator)
        gaps = [delta for delta in positive if delta >= period * Decimal("1.5")]
        result["gap_estimate"] = {
            "expected_fps": str(expected_fps),
            "threshold_periods": 1.5,
            "long_adjacent_intervals": len(gaps),
            "estimated_unfilled_nominal_slots": sum(int(delta / period + Decimal("0.5")) - 1
                                                     for delta in gaps),
            "assumption": "User-supplied constant frame rate; VFR can produce these gaps by design. Not a dropped-frame count.",
        }
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--json", metavar="FILE", help="ffprobe JSON, or '-' for stdin")
    source.add_argument("--video", metavar="FILE", help="local video; invokes ffprobe with network disabled")
    parser.add_argument("--expected-fps", type=expected_rate, metavar="N/D",
                        help="optional constant-rate assumption for gap estimates")
    args = parser.parse_args(argv)
    try:
        if args.video:
            document = probe_video(args.video)
        elif args.json == "-":
            document = read_json(sys.stdin.buffer)
        else:
            with open(args.json, "rb") as stream:
                document = read_json(stream)
        print(json.dumps(summarize(document, args.expected_fps), indent=2, allow_nan=False))
    except (TimingError, OSError) as exc:
        message = str(exc) if isinstance(exc, TimingError) else "could not read local input"
        print("error: " + message, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
