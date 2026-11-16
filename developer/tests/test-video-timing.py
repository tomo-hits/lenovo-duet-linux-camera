#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic saved-video timing regressions; no camera or media is accessed."""
import argparse
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/analyze-video-timing.py"
SPEC = importlib.util.spec_from_file_location("video_timing", SCRIPT)
timing = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(timing)


def frames(*times, field="best_effort_timestamp_time"):
    return {"frames": [{field: value} for value in times]}


class VideoTimingTests(unittest.TestCase):
    def test_regular_rate_is_measured_from_timestamps(self):
        data = frames("100.000", "100.040", "100.080", "100.120")
        data["streams"] = [{"avg_frame_rate": "30000/1001", "r_frame_rate": "60/1"}]
        result = timing.summarize(data)
        self.assertEqual(result["observed_frame_rate_fps"], 25)
        self.assertEqual(result["presentation_span_seconds"], .12)
        self.assertEqual(result["positive_interval_ms"], dict(p50=40, p95=40, p99=40, max=40))
        self.assertIsNone(result["gap_estimate"])

    def test_explicit_gap_assumption_and_vfr(self):
        data = frames("0", ".04", ".16", ".20")
        without_assumption = timing.summarize(data)
        self.assertIsNone(without_assumption["gap_estimate"])
        result = timing.summarize(data, timing.expected_rate("25/1"))
        self.assertEqual(result["observed_frame_rate_fps"], 15)
        self.assertEqual(result["positive_interval_ms"]["p95"], 120)
        self.assertEqual(result["gap_estimate"]["long_adjacent_intervals"], 1)
        self.assertEqual(result["gap_estimate"]["estimated_unfilled_nominal_slots"], 2)
        self.assertIn("VFR", result["gap_estimate"]["assumption"])

    def test_missing_values_are_not_bridged_or_filled_from_pts(self):
        data = frames("0", None, ".08", ".12")
        data["frames"][1]["pts_time"] = ".04"
        result = timing.summarize(data)
        self.assertEqual(result["timestamp_field"], "best_effort_timestamp_time")
        self.assertEqual(result["missing_or_invalid_timestamps"], 1)
        self.assertEqual(result["adjacent_timestamp_pairs"], 1)
        self.assertEqual(result["positive_interval_ms"]["max"], 40)
        self.assertIsNone(result["observed_frame_rate_fps"])

    def test_falls_back_to_pts_for_entire_timeline(self):
        result = timing.summarize(frames("0", ".05", ".1", field="pts_time"))
        self.assertEqual(result["timestamp_field"], "pts_time")
        self.assertEqual(result["observed_frame_rate_fps"], 20)

    def test_duplicates_and_reversals_are_exposed_not_sorted(self):
        result = timing.summarize(frames("0", ".04", ".04", ".02", ".08"))
        self.assertEqual(result["duplicate_adjacent_timestamps"], 1)
        self.assertEqual(result["backward_adjacent_timestamps"], 1)
        self.assertEqual(result["positive_intervals"], 2)
        self.assertEqual(result["positive_interval_ms"]["max"], 60)
        self.assertIsNone(result["observed_frame_rate_fps"])

    def test_packet_dts_does_not_override_presentation_time(self):
        data = frames("0", ".04", ".08")
        for frame, dts in zip(data["frames"], ("-.08", ".04", "0")):
            frame["pkt_dts_time"] = dts
        self.assertEqual(timing.summarize(data)["observed_frame_rate_fps"], 25)
        with self.assertRaises(timing.TimingError):
            timing.summarize({"packets": data["frames"]})

    def test_empty_single_and_equal_timelines_do_not_divide_by_zero(self):
        for data in (frames(), frames("0"), frames("0", "0"), frames(None, None)):
            with self.subTest(data=data):
                result = timing.summarize(data)
                self.assertIsNone(result["observed_frame_rate_fps"])
                self.assertIsNone(result["positive_interval_ms"])

    def test_nonfinite_malformed_and_extreme_timestamps(self):
        values = ("NaN", "Infinity", "-Infinity", "N/A", True, [],
                  "1e9999999", "1e-9999999", "x" * 81)
        for value in values:
            with self.subTest(value=value):
                result = timing.summarize(frames("0", value, ".08"))
                self.assertEqual(result["missing_or_invalid_timestamps"], 1)
                self.assertEqual(result["adjacent_timestamp_pairs"], 0)
                json.dumps(result, allow_nan=False)

    def test_large_origin_and_negative_timestamps(self):
        result = timing.summarize(frames("999999999999.000000", "999999999999.040000"))
        self.assertEqual(result["observed_frame_rate_fps"], 25)
        self.assertEqual(timing.summarize(frames("-.04", "0"))["observed_frame_rate_fps"], 25)

    def test_rejects_mixed_video_streams_and_invalid_shapes(self):
        for data in ({}, [], {"frames": [None]},
                     {"frames": [{"stream_index": 0}, {"stream_index": 1}]},
                     {"frames": [{"stream_index": True}]}):
            with self.subTest(data=data), self.assertRaises(timing.TimingError):
                timing.summarize(data)

    def test_input_limits_and_invalid_json(self):
        with mock.patch.object(timing, "MAX_JSON_BYTES", 8), self.assertRaises(timing.TimingError):
            timing.read_json(io.BytesIO(b" " * 9))
        with mock.patch.object(timing, "MAX_FRAMES", 1), self.assertRaises(timing.TimingError):
            timing.summarize(frames("0", ".04"))
        with self.assertRaises(timing.TimingError):
            timing.read_json(io.BytesIO(b"not json"))

    def test_expected_rate_validation(self):
        self.assertEqual(str(timing.expected_rate("30000/1001")), "30000/1001")
        for value in ("0", "-1", "1/0", "NaN", "1001", "x", "1e-9999999"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                timing.expected_rate(value)

    def test_missing_ffprobe_has_actionable_failure(self):
        with tempfile.NamedTemporaryFile() as video:
            with mock.patch.object(timing.shutil, "which", return_value=None):
                with self.assertRaisesRegex(timing.TimingError, "ffprobe is unavailable"):
                    timing.probe_video(video.name)

    def probe_with_fake_process(self, code):
        popen = subprocess.Popen

        def launch(command, **kwargs):
            self.assertEqual(command[command.index("-protocol_whitelist") + 1], "file")
            self.assertEqual(command[command.index("-select_streams") + 1], "v:0")
            self.assertTrue(Path(command[-1]).is_absolute())
            self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
            return popen([sys.executable, "-c", code], **kwargs)

        with tempfile.NamedTemporaryFile() as video:
            with mock.patch.object(timing.shutil, "which", return_value="/synthetic/ffprobe"):
                with mock.patch.object(timing.subprocess, "Popen", side_effect=launch):
                    return timing.probe_video(video.name)

    def test_probe_reads_bounded_numeric_json(self):
        data = frames("0", ".04")
        result = self.probe_with_fake_process("print(" + repr(json.dumps(data)) + ")")
        self.assertEqual(timing.summarize(result)["observed_frame_rate_fps"], 25)

    def test_probe_failure_hides_raw_error_output(self):
        with self.assertRaisesRegex(timing.TimingError, "could not read this video") as caught:
            self.probe_with_fake_process("import sys; sys.stderr.write('PRIVATE'); sys.exit(1)")
        self.assertNotIn("PRIVATE", str(caught.exception))

    def test_probe_stops_on_output_limit(self):
        with mock.patch.object(timing, "MAX_JSON_BYTES", 128):
            with self.assertRaisesRegex(timing.TimingError, "exceeds"):
                self.probe_with_fake_process("print('x' * 1024)")

    def test_probe_timeout_kills_and_reaps_process(self):
        with mock.patch.object(timing, "PROBE_TIMEOUT", .05):
            with self.assertRaisesRegex(timing.TimingError, "exceeded"):
                self.probe_with_fake_process("import time; time.sleep(10)")

    def test_stdin_cli_emits_only_summary_and_fails_cleanly(self):
        completed = subprocess.run([sys.executable, str(SCRIPT), "--json", "-"],
                                   input=json.dumps(frames("0", ".04")), text=True,
                                   capture_output=True, check=True)
        self.assertEqual(json.loads(completed.stdout)["observed_frame_rate_fps"], 25)
        self.assertEqual(completed.stderr, "")
        failure = subprocess.run([sys.executable, str(SCRIPT), "--json", "-"],
                                 input="PRIVATE INVALID INPUT", text=True, capture_output=True)
        self.assertEqual(failure.returncode, 2)
        self.assertEqual(failure.stdout, "")
        self.assertNotIn("PRIVATE", failure.stderr)


if __name__ == "__main__":
    unittest.main()
