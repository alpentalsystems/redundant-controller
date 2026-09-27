#!/usr/bin/env python3
"""Tests for central/rc_logd.py against a temporary log file."""
import json
import os
import queue
import socket
import sys
import tempfile
import threading
import time
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
                mask=1, event=0, detail=0, io_boot_id=0)


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


class RingFileTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "rc-log.bin")

    def tearDown(self):
        for name in os.listdir(self.dir):
            os.unlink(os.path.join(self.dir, name))
        os.rmdir(self.dir)

    def seq_in_slot(self, slot):
        with open(self.path, "rb") as f:
            f.seek(slot * 64)
            r = rclog.decode(f.read(64))
        return None if r is None else r["seq"]

    def test_new_file(self):
        ring = rc_logd.RingFile(self.path, 8)
        self.assertEqual(os.path.getsize(self.path), 8 * 64)
        self.assertEqual([ring.append(rec(0)) for _ in range(3)], [0, 1, 2])
        self.assertEqual((self.seq_in_slot(1), self.seq_in_slot(3)), (1, None))
        ring.close()

    def test_wrap_and_resume(self):
        ring = rc_logd.RingFile(self.path, 4)
        for _ in range(6):
            ring.append(rec(0))
        self.assertEqual([self.seq_in_slot(i) for i in range(4)], [4, 5, 2, 3])
        ring.close()
        ring = rc_logd.RingFile(self.path, 4)
        self.assertEqual(ring.append(rec(0)), 6)
        self.assertEqual(self.seq_in_slot(2), 6)
        ring.close()

    def test_corrupt_slot_ignored(self):
        ring = rc_logd.RingFile(self.path, 4)
        for _ in range(6):
            ring.append(rec(0))
        ring.close()
        with open(self.path, "r+b") as f:
            f.seek(64 + 20)
            f.write(b"\xff")
        ring = rc_logd.RingFile(self.path, 4)
        self.assertEqual(ring.next_seq, 5)
        ring.close()

    def test_short_garbage_file_extended(self):
        with open(self.path, "wb") as f:
            f.write(b"\xa5" * 100)
        ring = rc_logd.RingFile(self.path, 4)
        self.assertEqual((os.path.getsize(self.path), ring.next_seq), (4 * 64, 0))
        ring.close()


class ReceiverTest(unittest.TestCase):
    def test_records_from_socket_are_written(self):
        d = tempfile.mkdtemp(dir="/tmp")
        path, sock_path = os.path.join(d, "rc-log.bin"), os.path.join(d, "log.sock")
        ring = rc_logd.RingFile(path, 8)
        sock = rc_logd.open_socket(sock_path)
        stop = threading.Event()
        records = queue.Queue()
        t = threading.Thread(target=rc_logd.receive_forever, args=(sock, records, stop))
        w = threading.Thread(target=rc_logd.write_forever, args=(records, ring, stop))
        t.start()
        w.start()
        out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        out.sendto(b"not a record", sock_path)
        out.sendto(rclog.encode(rec(0)), sock_path)
        out.sendto(rclog.encode(rec(0)), sock_path)
        for _ in range(100):
            if ring.next_seq == 2:
                break
            stop.wait(0.01)
        stop.set()
        t.join(2)
        w.join(2)
        out.close()
        sock.close()
        ring.close()
        with open(path, "rb") as f:
            records, invalid = rclog.decode_file(f.read())
        for name in os.listdir(d):
            os.unlink(os.path.join(d, name))
        os.rmdir(d)
        self.assertFalse(t.is_alive())
        self.assertEqual(([r["seq"] for r in records], invalid), ([0, 1], 0))


class BlockingRing:
    """Stands in for a ring file on a stalled SD card: append waits until released."""

    def __init__(self):
        self.release = threading.Event()
        self.count = 0

    def append(self, rec):
        self.release.wait()
        self.count += 1
        return self.count - 1


class SlowDiskTest(unittest.TestCase):
    def test_slow_writes_do_not_block_receiving(self):
        d = tempfile.mkdtemp(dir="/tmp")
        sock_path = os.path.join(d, "log.sock")
        ring, q, stop = BlockingRing(), queue.Queue(), threading.Event()
        sock = rc_logd.open_socket(sock_path)
        threads = [threading.Thread(target=rc_logd.receive_forever, args=(sock, q, stop)),
                   threading.Thread(target=rc_logd.write_forever, args=(q, ring, stop))]
        for t in threads:
            t.start()
        out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        out.setblocking(False)
        failed = 0
        for _ in range(200):  # like the daemon: MSG_DONTWAIT, no retry
            try:
                out.sendto(rclog.encode(rec(0)), sock_path)
            except OSError:  # EAGAIN on Linux, ENOBUFS on macOS
                failed += 1
            time.sleep(0.001)
        ring.release.set()
        for _ in range(200):
            if ring.count == 200:
                break
            time.sleep(0.01)
        stop.set()
        for t in threads:
            t.join(2)
        out.close()
        sock.close()
        os.unlink(sock_path)
        os.rmdir(d)
        self.assertEqual((failed, ring.count), (0, 200))


if __name__ == "__main__":
    unittest.main()
