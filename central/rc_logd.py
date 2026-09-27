#!/usr/bin/env python3
"""Serves the controller's ring log over HTTP and flushes it to disk.

Usage: rc_logd.py [--path FILE] [--port N]
GET /log returns the raw file; GET /info returns size, valid records, and max seq.
"""
import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "common"))
import rclog  # noqa: E402

DEFAULT_PATH = "/var/lib/rc-central/rc-log.bin"
DEFAULT_PORT = 8080
FLUSH_S = 2.0


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
    args = p.parse_args(argv)
    stop = threading.Event()
    threading.Thread(target=flush_forever, args=(args.path, stop), daemon=True).start()
    server = make_server(args.path, args.port)
    sys.stderr.write("rc-logd: serving %s on port %d\n" % (args.path, args.port))
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
