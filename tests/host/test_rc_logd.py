#!/usr/bin/env python3
"""Tests for central/rc_logd.py against a temporary log file."""
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

sys.dont_write_bytecode = True
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "common"))
sys.path.insert(0, os.path.join(ROOT, "central"))
import rc_logd  # noqa: E402
import rclog  # noqa: E402


def rec(seq):
    return dict(type=rclog.SNAPSHOT, seq=seq, io_time_ms=seq, mono_ms=seq, wall_ms=seq, slot=0,
                role=2, mode=0, flags=0x20, active_slot=0, io_fail=0, bit_results=0, step=0,
                mask=1, event=0, detail=0)


class LogdTest(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp()
        os.close(fd)
        with open(self.path, "wb") as f:
            f.write(rclog.encode(rec(4)) + rclog.encode(rec(5)) + bytes(64) +
                    rclog.encode(rec(6)))
        self.server = rc_logd.make_server(self.path, 0, "127.0.0.1")
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        if os.path.exists(self.path):
            os.unlink(self.path)

    def test_info(self):
        with urllib.request.urlopen(self.base + "/info") as r:
            self.assertEqual(json.load(r), {"size": 256, "records": 3, "max_seq": 6})

    def test_log_is_the_file(self):
        with urllib.request.urlopen(self.base + "/log") as r, open(self.path, "rb") as f:
            self.assertEqual(r.read(), f.read())

    def test_unknown_path(self):
        with self.assertRaises(urllib.error.HTTPError) as c:
            urllib.request.urlopen(self.base + "/other")
        self.assertEqual(c.exception.code, 404)

    def test_missing_file(self):
        os.unlink(self.path)
        with self.assertRaises(urllib.error.HTTPError) as c:
            urllib.request.urlopen(self.base + "/log")
        self.assertEqual(c.exception.code, 503)

    def test_flush_runs_and_stops(self):
        stop = threading.Event()
        t = threading.Thread(target=rc_logd.flush_forever, args=(self.path, stop, 0.01))
        t.start()
        stop.wait(0.05)
        stop.set()
        t.join(1)
        self.assertFalse(t.is_alive())


if __name__ == "__main__":
    unittest.main()
