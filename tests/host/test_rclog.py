#!/usr/bin/env python3
"""Tests for common/rclog.py, including a log written by the C codec."""
import os
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "common"))
import rclog  # noqa: E402

SAMPLE = os.environ.get("RC_LOG_SAMPLE")


def rec(**kw):
    r = dict(type=rclog.SNAPSHOT, seq=1, io_time_ms=2, mono_ms=3, wall_ms=4, slot=0, role=2,
             mode=0, flags=0b100011, active_slot=0, io_fail=0, bit_results=0, step=5, mask=1,
             event=0, detail=0)
    r.update(kw)
    return r


class RclogTest(unittest.TestCase):
    def test_roundtrip(self):
        r = rec(seq=0x01020304, wall_ms=1790000000123)
        b = rclog.encode(r)
        self.assertEqual(len(b), rclog.RECORD_SIZE)
        self.assertEqual(b[:4], bytes([0x52, 0x4C, 1, rclog.SNAPSHOT]))
        self.assertEqual(rclog.decode(b), r)

    def test_crc_matches_c(self):
        self.assertEqual(rclog.crc16(b"123456789"), 0x29B1)

    def test_flipped_byte_rejected(self):
        b = bytearray(rclog.encode(rec()))
        b[20] ^= 1
        self.assertIsNone(rclog.decode(bytes(b)))

    def test_decode_file_skips_empty_and_counts_bad(self):
        data = (rclog.encode(rec(seq=9)) + bytes(64) + b"\x01" * 64 +
                rclog.encode(rec(seq=3)))
        records, invalid = rclog.decode_file(data)
        self.assertEqual([r["seq"] for r in records], [3, 9])
        self.assertEqual(invalid, 1)

    def test_helpers(self):
        r = rec(bit_results=0x0350, event=12, detail=(3 << 8) | 1, flags=0b100011)
        self.assertEqual(rclog.bit_results(r)["cross_link"], "fail")
        self.assertEqual(rclog.bit_results(r)["cpu_temp"], "not_run")
        self.assertEqual(rclog.detail_text(r), "supply_voltage=fail")
        f = rclog.flags(r)
        self.assertTrue(f["healthy"] and f["referee"] and f["io_time_valid"])
        self.assertFalse(f["peer"])
        self.assertTrue(rclog.io_valid(r))
        self.assertEqual(rclog.detail_text(rec(event=2, detail=1)), "standby")
        self.assertEqual(rclog.detail_text(rec(event=13, detail=(1 << 8) | 0x55)), "leds 0x55")

    def test_log_dropped_event(self):
        r = rec(type=rclog.EVENT, event=15, detail=12)
        self.assertEqual(rclog.EVENTS[15], "log_dropped")
        self.assertEqual(rclog.detail_text(r), "12 records")

    @unittest.skipUnless(SAMPLE, "RC_LOG_SAMPLE not set")
    def test_c_written_sample(self):
        with open(SAMPLE, "rb") as f:
            data = f.read()
        records, invalid = rclog.decode_file(data)
        self.assertEqual(invalid, 0)
        self.assertEqual([r["seq"] for r in records], [7, 8, 9])
        r = records[0]
        self.assertEqual(r["io_time_ms"], 123456)
        self.assertEqual(r["mono_ms"], 9876543210)
        self.assertEqual(r["wall_ms"], 1790000000123)
        self.assertEqual((r["slot"], r["role"], r["mode"]), (0, 2, 1))
        self.assertEqual((r["io_fail"], r["step"], r["mask"]), (1, 513, 0x55))
        self.assertEqual(rclog.bit_results(r)["supply_voltage"], "fail")
        self.assertEqual(rclog.EVENTS[records[1]["event"]], "role_changed")
        self.assertEqual(rclog.detail_text(records[1]), "standby")
        self.assertEqual(rclog.detail_text(records[2]), "supply_voltage=fail")
        self.assertEqual(rclog.encode(r), data[:64])


if __name__ == "__main__":
    unittest.main()
