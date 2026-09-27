#!/usr/bin/env python3
"""Tests for tools/rcctl.py against fake controllers on localhost."""
import json
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import rcctl  # noqa: E402

ACTIVE = {"ok": True, "role": "active", "slot": "B"}
STANDBY = {"ok": True, "role": "standby", "slot": "A"}
NOT_ACTIVE = {"ok": False, "error": "not active", "active": "B"}
OK = {"ok": True}


class FakeController:
    """Answers each line with replies[first word], recording the lines."""

    def __init__(self, replies):
        self.replies = replies
        self.lines = []
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen()
        self.addr = "127.0.0.1:%d" % self.sock.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn, conn.makefile("rwb") as f:
                for raw in f:
                    line = raw.decode().strip()
                    self.lines.append(line)
                    word = line.split()[0] if line else ""
                    f.write((json.dumps(self.replies[word]) + "\n").encode())
                    f.flush()

    def close(self):
        self.sock.close()


class RcctlTest(unittest.TestCase):
    def test_parse_host(self):
        self.assertEqual(rcctl.parse_host("10.0.0.1"), ("10.0.0.1", 5000))
        self.assertEqual(rcctl.parse_host("127.0.0.1:5123"), ("127.0.0.1", 5123))

    def test_command_goes_to_active(self):
        a = FakeController({"STATUS": STANDBY, "LEDS": NOT_ACTIVE})
        b = FakeController({"STATUS": ACTIVE, "LEDS": OK})
        try:
            host, reply = rcctl.send_to_active([a.addr, b.addr], "LEDS 0x55")
        finally:
            a.close()
            b.close()
        self.assertEqual(host, b.addr)
        self.assertEqual(reply, OK)
        self.assertNotIn("LEDS 0x55", a.lines)

    def test_retries_other_after_not_active(self):
        a = FakeController({"STATUS": dict(ACTIVE, slot="A"), "LEDS": NOT_ACTIVE})
        b = FakeController({"STATUS": STANDBY, "LEDS": OK})
        try:
            host, reply = rcctl.send_to_active([a.addr, b.addr], "LEDS 0x55")
        finally:
            a.close()
            b.close()
        self.assertIn("LEDS 0x55", a.lines)
        self.assertEqual(host, b.addr)
        self.assertEqual(reply, OK)

    def test_prefers_active_confirmed_by_referee(self):
        # A lost its referee link and still believes it is Active; the card chose B.
        stale = {"ok": True, "role": "active", "slot": "A", "referee": False, "active": "A"}
        real = {"ok": True, "role": "active", "slot": "B", "referee": True, "active": "B"}
        a = FakeController({"STATUS": stale, "LEDS": {"ok": False, "error": "referee lost"}})
        b = FakeController({"STATUS": real, "LEDS": OK})
        try:
            host, reply = rcctl.send_to_active([a.addr, b.addr], "LEDS 0x55")
        finally:
            a.close()
            b.close()
        self.assertEqual(host, b.addr)
        self.assertEqual(reply, OK)
        self.assertNotIn("LEDS 0x55", a.lines)

    def test_unreachable_controller_is_reported(self):
        b = FakeController({"STATUS": ACTIVE})
        dead = FakeController({})
        dead.close()
        try:
            out = rcctl.query_all([dead.addr, b.addr], "STATUS")
        finally:
            b.close()
        self.assertFalse(out[dead.addr]["ok"])
        self.assertEqual(out[b.addr], ACTIVE)


if __name__ == "__main__":
    unittest.main()
