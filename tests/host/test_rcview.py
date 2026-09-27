#!/usr/bin/env python3
"""Tests for tools/rcview.py: segments, merge, filters, CSV, timeline, and the API."""
import csv
import io
import json
import os
import sys
import threading
import unittest
import urllib.request

sys.dont_write_bytecode = True
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rcview  # noqa: E402
import rclog  # noqa: E402

BOOT = 1_790_000_000_000


def rec(seq, io, slot=0, role=2, kind=rclog.SNAPSHOT, event=0, detail=0, boot=BOOT, valid=True):
    return dict(type=kind, seq=seq, io_time_ms=io if valid else 0, mono_ms=seq,
                wall_ms=boot + io, slot=slot, role=role, mode=0,
                flags=(0x20 if valid else 0) | 0x01, active_slot=0, io_fail=0,
                bit_results=0, step=seq, mask=1, event=event, detail=detail)


def ev(seq, io, event, detail=0, slot=0, role=2, boot=BOOT):
    return rec(seq, io, slot=slot, role=role, kind=rclog.EVENT, event=event, detail=detail,
               boot=boot)


class DataTest(unittest.TestCase):
    def test_split_on_io_card_restart(self):
        segs = rcview.split_segments([rec(1, 1000), rec(2, 1100), rec(3, 50), rec(4, 150)])
        self.assertEqual([[r["seq"] for r in s] for s in segs], [[1, 2], [3, 4]])

    def test_records_without_io_time_join_next_segment(self):
        segs = rcview.split_segments([rec(1, 0, valid=False), rec(2, 500), rec(3, 600),
                                      rec(4, 0, valid=False)])
        self.assertEqual([[r["seq"] for r in s] for s in segs], [[1, 2, 3, 4]])

    def test_merge_interleaves_by_io_time(self):
        logs = {"A": [rec(1, 100), rec(2, 300)],
                "B": [rec(10, 200, slot=1), rec(11, 400, slot=1)]}
        segs = rcview.merge(logs)
        self.assertEqual(len(segs), 1)
        self.assertEqual([(n, r["seq"]) for n, r in segs[0]["rows"]],
                         [("A", 1), ("B", 10), ("A", 2), ("B", 11)])
        self.assertEqual(sorted(segs[0]["controllers"]), ["A", "B"])

    def test_merge_tolerates_small_clock_offset(self):
        logs = {"A": [rec(1, 100)], "B": [rec(10, 200, slot=1, boot=BOOT + 3000)]}
        self.assertEqual(len(rcview.merge(logs)), 1)

    def test_merge_matches_logs_that_start_at_different_times(self):
        # B's log starts an hour later but in the same I/O card run.
        logs = {"A": [rec(1, 100), rec(2, 3_600_100)], "B": [rec(10, 3_600_200, slot=1)]}
        self.assertEqual(len(rcview.merge(logs)), 1)

    def test_merge_separates_io_card_runs(self):
        logs = {"A": [rec(1, 100)], "B": [rec(10, 200, slot=1, boot=BOOT + 60_000)]}
        segs = rcview.merge(logs)
        self.assertEqual(len(segs), 2)
        self.assertLess(segs[0]["boot_wall"], segs[1]["boot_wall"])

    def test_controller_name(self):
        self.assertEqual(rcview.controller_name([rec(1, 1, slot=1)], "x.bin"), "B")
        self.assertEqual(rcview.controller_name([], "x.bin"), "x.bin")

    def test_filter_rows(self):
        rows = [("A", rec(1, 100)), ("A", ev(2, 150, 5)), ("B", ev(3, 200, 2, 1, slot=1)),
                ("B", rec(4, 300, slot=1))]
        self.assertEqual(len(rcview.filter_rows(rows)), 2)  # events by default
        self.assertEqual(len(rcview.filter_rows(rows, kind="all")), 4)
        self.assertEqual(len(rcview.filter_rows(rows, kind="snapshots")), 2)
        self.assertEqual(len(rcview.filter_rows(rows, kind="all", ctl={"B"})), 2)
        self.assertEqual(len(rcview.filter_rows(rows, events={"role_changed"})), 1)
        self.assertEqual(len(rcview.filter_rows(rows, kind="all", io_from=150, io_to=200)), 2)

    def test_csv(self):
        text = rcview.to_csv([("B", ev(3, 200, 2, 1, slot=1))])
        lines = list(csv.reader(io.StringIO(text)))
        self.assertEqual(lines[0], rcview.COLUMNS)
        row = dict(zip(lines[0], lines[1]))
        self.assertEqual((row["controller"], row["event"], row["detail"]),
                         ("B", "role_changed", "standby"))
        self.assertEqual(row["io_time_ms"], "200")

    def test_timeline(self):
        rows = [("A", rec(1, 100)), ("B", rec(2, 100, slot=1, role=1)),
                ("A", rec(3, 200)), ("A", ev(4, 250, 2, 1, role=1)), ("A", rec(5, 300, role=1)),
                ("A", rec(6, 5000, role=1))]
        tl = rcview.timeline(rows)
        self.assertEqual(tl["lanes"]["A"], [[100, 200, "active"], [250, 300, "standby"],
                                            [5000, 5000, "standby"]])
        self.assertEqual(tl["lanes"]["B"], [[100, 100, "standby"]])
        self.assertEqual(tl["markers"], [[250, "A", "role_changed"]])


if __name__ == "__main__":
    unittest.main()
