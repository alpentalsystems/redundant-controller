#!/usr/bin/env python3
"""Owns the controller's ring log: writes, flushes, and serves it.

Usage: rc_logd.py [--path FILE] [--port N] [--socket PATH]
The real-time daemon sends each 64-byte record to the datagram socket and
never touches the file, so a slow SD card cannot stall control.
GET /log returns the raw file; GET /info returns size, valid records, and max seq.
"""
import argparse
import json
import os
import queue
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "common"))
import rclog  # noqa: E402

DEFAULT_PATH = "/var/lib/rc-central/rc-log.bin"
DEFAULT_PORT = 8080
DEFAULT_SOCKET = "/run/rc-logd/log.sock"
SLOTS = 163840
FLUSH_S = 2.0


class RingFile:
    """Ring of 64-byte records: seq n is stored in slot n mod slots."""

    def __init__(self, path, slots):
        self.slots = slots
        self.fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644)
        size = slots * rclog.RECORD_SIZE
        if os.fstat(self.fd).st_size < size:
            os.ftruncate(self.fd, size)
        records, _ = rclog.decode_file(os.pread(self.fd, size, 0))
        self.next_seq = records[-1]["seq"] + 1 if records else 0

    def append(self, rec):
        """Stamps rec with the next seq and writes it; a failed write still uses up the seq."""
        seq = self.next_seq
        self.next_seq += 1
        data = rclog.encode(dict(rec, seq=seq))
        os.pwrite(self.fd, data, (seq % self.slots) * rclog.RECORD_SIZE)
        return seq

    def close(self):
        os.close(self.fd)


def open_socket(path):
    if os.path.exists(path):
        os.unlink(path)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind(path)
    sock.settimeout(0.5)
    return sock


def receive_forever(sock, records, stop):
    """Moves each valid record datagram into the queue; never touches the disk.

    Linux queues only about 10 datagrams on the socket, so a receiver that
    waited for a slow SD card would make the daemon drop records.
    """
    bad = False
    while not stop.is_set():
        try:
            data = sock.recv(256)
        except socket.timeout:
            continue
        rec = rclog.decode(data)
        if rec is None:
            if not bad:
                sys.stderr.write("rc-logd: invalid record datagram (%d bytes)\n" % len(data))
                bad = True
            continue
        records.put(rec)


def write_forever(records, ring, stop):
    """Writes queued records to the ring; reports write errors once."""
    write_failed = False
    while not stop.is_set():
        try:
            rec = records.get(timeout=0.5)
        except queue.Empty:
            continue
        try:
            ring.append(rec)
            if write_failed:
                sys.stderr.write("rc-logd: write ok\n")
                write_failed = False
        except OSError as e:
            if not write_failed:
                sys.stderr.write("rc-logd: write failed: %s\n" % e)
                write_failed = True


def info(data):
    records, _ = rclog.decode_file(data)
    return {"size": len(data), "records": len(records),
            "max_seq": records[-1]["seq"] if records else None}


def make_handler(path):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path not in ("/log", "/info"):
                self.send_error(404)
                return
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError as e:
                self.send_error(503, "log unavailable: %s" % e.strerror)
                return
            if self.path == "/log":
                body, ctype = data, "application/octet-stream"
            else:
                body, ctype = json.dumps(info(data)).encode(), "application/json"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            sys.stderr.write("rc-logd: %s %s\n" % (self.address_string(), fmt % args))

    return Handler


def make_server(path, port, host=""):
    return ThreadingHTTPServer((host, port), make_handler(path))


def flush_forever(path, stop, period=FLUSH_S):
    """fsync on any handle writes out the daemon's cached records too."""
    failed = False
    while not stop.wait(period):
        try:
            fd = os.open(path, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            if failed:
                sys.stderr.write("rc-logd: flush ok\n")
                failed = False
        except OSError as e:
            if not failed:
                sys.stderr.write("rc-logd: flush failed: %s\n" % e)
                failed = True


def main(argv=None):
    p = argparse.ArgumentParser(description="Controller log server")
    p.add_argument("--path", default=DEFAULT_PATH)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--socket", default=DEFAULT_SOCKET)
    args = p.parse_args(argv)
    stop = threading.Event()
    ring = RingFile(args.path, SLOTS)
    sys.stderr.write("rc-logd: next seq %d\n" % ring.next_seq)
    sock = open_socket(args.socket)
    records = queue.Queue()
    threading.Thread(target=receive_forever, args=(sock, records, stop), daemon=True).start()
    threading.Thread(target=write_forever, args=(records, ring, stop), daemon=True).start()
    threading.Thread(target=flush_forever, args=(args.path, stop), daemon=True).start()
    server = make_server(args.path, args.port)
    sys.stderr.write("rc-logd: serving %s on port %d\n" % (args.path, args.port))
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
