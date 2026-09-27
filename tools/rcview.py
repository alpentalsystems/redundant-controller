#!/usr/bin/env python3
"""Log viewer for the redundant controller.

Usage:
  rcview.py [--hosts H1,H2] [--save DIR]   download both controllers' logs and view them
  rcview.py --open FILE [FILE ...]         view saved logs
The page is served at http://127.0.0.1:8765 and opened in the browser.
"""
import csv
import datetime
import io
import os
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
import rclog  # noqa: E402

SEGMENT_DROP_MS = 1000
MATCH_WINDOW_MS = 10000
GAP_MS = 1000
TIMELINE_EVENTS = {"started", "role_changed", "healthy", "unhealthy", "referee_lost",
                   "referee_back", "mode_changed", "fault_set", "fault_cleared"}
COLUMNS = (["controller", "seq", "kind", "event", "detail", "io_time_ms", "wall", "mono_ms",
            "role", "mode", "healthy", "referee", "peer", "peer_healthy", "fault", "active",
            "io_fail", "step", "mask"] + list(rclog.BIT_ITEMS))


def controller_name(records, fallback):
    slots = [r["slot"] for r in records if r["slot"] in rclog.SLOTS]
    if not slots:
        return fallback
    return rclog.SLOTS[max(set(slots), key=slots.count)]


def split_segments(records):
    """Splits one controller's records (sorted by seq) at I/O card restarts.

    Records without I/O card time join the segment of the next record that has it.
    """
    segments, current, pending, last_io = [], None, [], None
    for r in records:
        if not rclog.io_valid(r):
            pending.append(r)
            continue
        if current is None or r["io_time_ms"] < last_io - SEGMENT_DROP_MS:
            current = []
            segments.append(current)
        current.extend(pending)
        pending = []
        current.append(r)
        last_io = r["io_time_ms"]
    if pending:
        if current is None:
            segments.append(pending)
        else:
            current.extend(pending)
    return segments


def boot_time(segment):
    """Estimated wall-clock time of the I/O card's boot for this segment."""
    offsets = [r["wall_ms"] - r["io_time_ms"] for r in segment if rclog.io_valid(r)]
    if not offsets:
        return segment[0]["wall_ms"]
    return int(statistics.median(offsets))


def merge(logs):
    """logs: {name: records}. Returns merged segments ordered by I/O card boot time."""
    parts = []
    for name, records in logs.items():
        for seg in split_segments(records):
            parts.append((boot_time(seg), name, seg))
    parts.sort(key=lambda p: p[0])
    merged = []
    for boot, name, seg in parts:
        target = None
        for m in merged:
            if abs(boot - m["boot_wall"]) <= MATCH_WINDOW_MS and name not in m["controllers"]:
                target = m
                break
        if target is None:
            target = {"boot_wall": boot, "controllers": [], "rows": []}
            merged.append(target)
        target["controllers"].append(name)
        target["rows"].extend((name, r) for r in seg)
    for m in merged:
        m["rows"].sort(key=lambda nr: (nr[1]["io_time_ms"], nr[0], nr[1]["seq"]))
    return merged


def filter_rows(rows, ctl=None, kind="events", events=None, io_from=None, io_to=None):
    out = []
    for name, r in rows:
        is_event = r["type"] == rclog.EVENT
        if ctl is not None and name not in ctl:
            continue
        if (kind == "events" and not is_event) or (kind == "snapshots" and is_event):
            continue
        if events is not None and rclog.EVENTS.get(r["event"]) not in events:
            continue
        if io_from is not None and r["io_time_ms"] < io_from:
            continue
        if io_to is not None and r["io_time_ms"] > io_to:
            continue
        out.append((name, r))
    return out


def row_values(name, r):
    f = rclog.flags(r)
    bits = rclog.bit_results(r)
    wall = datetime.datetime.fromtimestamp(r["wall_ms"] / 1000).strftime(
        "%Y-%m-%d %H:%M:%S.%f")[:-3]
    return ([name, r["seq"], "event" if r["type"] == rclog.EVENT else "snapshot",
             rclog.EVENTS.get(r["event"], str(r["event"])), rclog.detail_text(r),
             r["io_time_ms"] if f["io_time_valid"] else None, wall, r["mono_ms"],
             rclog.ROLES.get(r["role"], str(r["role"])),
             rclog.MODES.get(r["mode"], str(r["mode"])), f["healthy"], f["referee"],
             f["peer"], f["peer_healthy"], f["fault"],
             rclog.SLOTS.get(r["active_slot"], "none"), r["io_fail"], r["step"], r["mask"]] +
            [bits[item] for item in rclog.BIT_ITEMS])


def to_csv(rows):
    out = io.StringIO()
    w = csv.writer(out)
    w.writerow(COLUMNS)
    for name, r in rows:
        w.writerow(["" if v is None else v for v in row_values(name, r)])
    return out.getvalue()


def timeline(rows):
    """Role intervals per controller in I/O card time, plus markers for key events."""
    lanes, markers = {}, []
    for name, r in rows:
        if not rclog.io_valid(r):
            continue
        t = r["io_time_ms"]
        role = rclog.ROLES.get(r["role"], "unknown")
        lane = lanes.setdefault(name, [])
        if lane and lane[-1][2] == role and t - lane[-1][1] <= GAP_MS:
            lane[-1][1] = t
        else:
            lane.append([t, t, role])
        event = rclog.EVENTS.get(r["event"], "")
        if r["type"] == rclog.EVENT and event in TIMELINE_EVENTS:
            markers.append([t, name, event])
    return {"lanes": lanes, "markers": markers}
