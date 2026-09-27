#!/usr/bin/env python3
"""Upper-controller client for the redundant controller.

Usage: rcctl.py [--hosts H1,H2] status|bit|leds <hex>|lamp|run-bit
Hosts come from --hosts, then RC_HOSTS, then the bench defaults.
"""
import argparse
import json
import os
import socket
import sys
import time

PORT = 5000
TIMEOUT_S = 2.0
DEFAULT_HOSTS = "192.168.45.50,192.168.45.176"


def parse_host(text):
    host, sep, port = text.rpartition(":")
    if not sep:
        return text, PORT
    return host, int(port)


def request(addr, line):
    host, port = parse_host(addr)
    with socket.create_connection((host, port), timeout=TIMEOUT_S) as s:
        s.sendall((line + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                raise ConnectionError("connection closed")
            buf += chunk
    return json.loads(buf)


def safe_request(addr, line):
    try:
        return request(addr, line)
    except (OSError, ValueError) as e:
        return {"ok": False, "error": str(e)}


def query_all(hosts, line):
    return {h: safe_request(h, line) for h in hosts}


def send_to_active(hosts, line):
    """Sends line to the controller reporting Active; tries the other once on 'not active'."""
    status = query_all(hosts, "STATUS")
    order = sorted(hosts, key=lambda h: status[h].get("role") != "active")
    host, reply = order[0], {"ok": False, "error": "no controllers"}
    for host in order[:2]:
        reply = safe_request(host, line)
        if reply.get("error") != "not active":
            break
    return host, reply


def print_status(replies):
    keys = ["slot", "role", "mode", "healthy", "active", "referee", "peer", "peer_healthy",
            "fault", "step", "mask", "io_fail", "error"]
    hosts = list(replies)
    print("%-13s" % "" + "".join("%-22s" % h for h in hosts))
    for k in keys:
        if any(k in r for r in replies.values()):
            print("%-13s" % k + "".join("%-22s" % replies[h].get(k, "") for h in hosts))


def print_bit(replies):
    for host, r in replies.items():
        if not r.get("ok"):
            print("== %s error: %s" % (host, r.get("error")))
            continue
        print("== %s healthy=%s io_fail=0x%02x" % (host, r["healthy"], r["io_fail"]))
        for key in ("pbit", "cbit"):
            rep = r.get(key)
            if rep is None:
                print("  %s: not run yet" % key)
                continue
            print("  %s (age %d ms)" % (key, rep["age_ms"]))
            for it in rep["items"]:
                print("    %-15s %-6s %-9s %s" % (it["name"], it["result"],
                                                 "critical" if it["critical"] else "",
                                                 it["value"]))


def main(argv=None):
    p = argparse.ArgumentParser(description="Redundant controller client")
    p.add_argument("--hosts", default=os.environ.get("RC_HOSTS", DEFAULT_HOSTS),
                   help="comma-separated controller addresses (host or host:port)")
    p.add_argument("command", choices=["status", "bit", "leds", "lamp", "run-bit"])
    p.add_argument("value", nargs="?", help="LED mask in hex for leds, e.g. 0x55")
    args = p.parse_args(argv)
    hosts = args.hosts.split(",")

    if args.command in ("status", "bit"):
        replies = query_all(hosts, args.command.upper())
        (print_status if args.command == "status" else print_bit)(replies)
        return 0 if all(r.get("ok") for r in replies.values()) else 1
    if args.command == "leds":
        if args.value is None:
            p.error("leds needs a value, e.g. 0x55")
        line = "LEDS " + args.value
    elif args.command == "lamp":
        line = "LAMP_TEST"
    else:
        line = "RUN_BIT"
    host, reply = send_to_active(hosts, line)
    print("%s: %s" % (host, json.dumps(reply)))
    if args.command == "run-bit" and reply.get("ok"):
        time.sleep(0.2)
        print_bit({host: safe_request(host, "BIT")})
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
