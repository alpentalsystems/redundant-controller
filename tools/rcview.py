#!/usr/bin/env python3
"""Log viewer for the redundant controller.

Usage:
  rcview.py [--hosts H1,H2] [--save DIR]   download both controllers' logs and view them
  rcview.py --open FILE [FILE ...]         view saved logs
The page is served at http://127.0.0.1:8765 and opened in the browser.
"""
import argparse
import csv
import datetime
import io
import json
import os
import statistics
import sys
import time
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))
import rclog  # noqa: E402

SEGMENT_DROP_MS = 1000
MATCH_WINDOW_MS = 10000
GAP_MS = 1000
TIMELINE_EVENTS = {"started", "role_changed", "healthy", "unhealthy", "referee_lost",
                   "referee_back", "mode_changed", "fault_set", "fault_cleared"}
COLUMNS = (["controller", "seq", "kind", "event", "detail", "io_time_ms", "io_estimated",
            "wall", "mono_ms",
            "role", "mode", "healthy", "referee", "peer", "peer_healthy", "fault", "active",
            "io_fail", "step", "mask"] + list(rclog.BIT_ITEMS))
LOG_PORT = 8080
VIEW_PORT = 8765
DEFAULT_HOSTS = "192.168.45.50,192.168.45.176"
PAGE_SIZE = 500


def controller_name(records, fallback):
    slots = [r["slot"] for r in records if r["slot"] in rclog.SLOTS]
    if not slots:
        return fallback
    return rclog.SLOTS[max(set(slots), key=slots.count)]


def split_segments(records):
    """Splits one controller's records (sorted by seq) at I/O card restarts.

    Records without I/O card time join the segment of the next record that has it and
    get an estimated I/O card time from the controller's own clock (io_estimated).
    """
    segments, current, pending, last = [], None, [], None
    for r in records:
        if not rclog.io_valid(r):
            pending.append(r)
            continue
        if current is None or r["io_time_ms"] < last["io_time_ms"] - SEGMENT_DROP_MS:
            current = []
            segments.append(current)
        for p in pending:
            estimate(p, r)
        current.extend(pending)
        pending = []
        current.append(r)
        last = r
    if pending:
        if current is None:
            segments.append(pending)
        else:
            for p in pending:
                estimate(p, last)
            current.extend(pending)
    return segments


def estimate(r, ref):
    """I/O card time of r from a nearby record with a real one, using controller uptime."""
    r["io_time_ms"] = max(0, ref["io_time_ms"] + (r["mono_ms"] - ref["mono_ms"]))
    r["io_estimated"] = True


def has_io_time(r):
    return rclog.io_valid(r) or r.get("io_estimated", False)


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
             r["io_time_ms"] if has_io_time(r) else None, r.get("io_estimated", False),
             wall, r["mono_ms"],
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
        if not has_io_time(r):
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


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>rcview</title>
<style>
body{font:13px -apple-system,"Segoe UI",sans-serif;margin:16px;color:#1f2937}
.bar{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin:8px 0}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
th,td{border-bottom:1px solid #e5e7eb;padding:2px 6px;text-align:left;white-space:nowrap}
th{position:sticky;top:0;background:#f9fafb}
tr.event td{background:#fff7ed}
#tl{width:100%;height:64px;border:1px solid #e5e7eb;cursor:crosshair}
.active{fill:#2e8b57}.standby{fill:#3b6fd8}.unknown{fill:#9ca3af}
#err{color:#b91c1c}
</style></head><body>
<p id="err"></p>
<div class="bar">
 <label>Segment <select id="seg"></select></label>
 <label><input type="checkbox" id="ctlA" checked> A</label>
 <label><input type="checkbox" id="ctlB" checked> B</label>
 <label>Show <select id="kind"><option value="events">events</option>
  <option value="all">events + snapshots</option><option value="snapshots">snapshots</option></select></label>
 <label>Event <select id="event"><option value="">any</option></select></label>
 <label>From (s) <input id="from" size="10"></label>
 <label>To (s) <input id="to" size="10"></label>
 <button id="apply">Apply</button>
 <button id="csv">Export CSV</button>
</div>
<svg id="tl"></svg>
<div class="bar"><button id="prev">&lt;</button><span id="pageinfo"></span><button id="next">&gt;</button></div>
<table><thead id="head"></thead><tbody id="body"></tbody></table>
<script>
const EVENTS = __EVENTS__;
const $ = id => document.getElementById(id);
let page = 0, pages = 1, tl = null;
function params() {
  const p = new URLSearchParams();
  p.set("seg", $("seg").value);
  p.set("ctl", ["A", "B"].filter(c => $("ctl" + c).checked).join(","));
  p.set("kind", $("kind").value);
  if ($("event").value) p.set("events", $("event").value);
  const f = parseFloat($("from").value), t = parseFloat($("to").value);
  if (!isNaN(f)) p.set("from", Math.round(f * 1000));
  if (!isNaN(t)) p.set("to", Math.round(t * 1000));
  return p;
}
async function getJSON(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(url + ": " + r.status);
  return r.json();
}
function cell(v) { return v === null ? "" : String(v); }
async function loadRows() {
  const p = params();
  p.set("page", page);
  const d = await getJSON("/api/rows?" + p);
  pages = Math.max(d.pages, 1);
  $("pageinfo").textContent = ` page ${d.page + 1} / ${pages} (${d.total} rows) `;
  $("head").innerHTML = "<tr>" + d.columns.map(c => `<th>${c}</th>`).join("") + "</tr>";
  const kind = d.columns.indexOf("kind");
  $("body").innerHTML = d.rows.map(r =>
    `<tr class="${r[kind]}">` + r.map(v => `<td>${cell(v)}</td>`).join("") + "</tr>").join("");
}
function drawTimeline() {
  const svg = $("tl"), w = svg.clientWidth, names = Object.keys(tl.lanes).sort();
  let lo = Infinity, hi = -Infinity;
  svg.innerHTML = "";
  for (const n of names) for (const s of tl.lanes[n]) { lo = Math.min(lo, s[0]); hi = Math.max(hi, s[1]); }
  if (!isFinite(lo)) return;
  svg.dataset.lo = lo; svg.dataset.hi = hi;
  const x = t => 30 + (t - lo) / Math.max(hi - lo, 1) * (w - 40);
  names.forEach((n, i) => {
    const y = 6 + i * 28;
    svg.insertAdjacentHTML("beforeend", `<text x="4" y="${y + 15}">${n}</text>`);
    for (const s of tl.lanes[n]) {
      svg.insertAdjacentHTML("beforeend", `<rect class="${s[2]}" x="${x(s[0])}" y="${y}" ` +
        `width="${Math.max(x(s[1]) - x(s[0]), 1)}" height="22"><title>${n} ${s[2]} ` +
        `${(s[0] / 1000).toFixed(3)}-${(s[1] / 1000).toFixed(3)} s</title></rect>`);
    }
  });
  for (const m of tl.markers) {
    const i = names.indexOf(m[1]), xm = x(m[0]);
    svg.insertAdjacentHTML("beforeend", `<line x1="${xm}" x2="${xm}" y1="${2 + i * 28}" ` +
      `y2="${32 + i * 28}" stroke="#e0453a" stroke-width="2"><title>${m[1]} ${m[2]} ` +
      `${(m[0] / 1000).toFixed(3)} s</title></line>`);
  }
}
async function loadSegment() {
  tl = await getJSON("/api/timeline?seg=" + $("seg").value);
  drawTimeline();
  page = 0;
  await loadRows();
}
function run(f) { f().catch(e => { $("err").textContent = String(e); }); }
$("tl").addEventListener("click", e => {
  const svg = $("tl"), lo = +svg.dataset.lo, hi = +svg.dataset.hi, w = svg.clientWidth;
  const t = lo + (e.offsetX - 30) / (w - 40) * (hi - lo);
  $("from").value = ((t - 2000) / 1000).toFixed(3);
  $("to").value = ((t + 2000) / 1000).toFixed(3);
  $("kind").value = "all";
  page = 0;
  run(loadRows);
});
$("apply").onclick = () => { page = 0; run(loadRows); };
$("prev").onclick = () => { if (page > 0) { page--; run(loadRows); } };
$("next").onclick = () => { if (page + 1 < pages) { page++; run(loadRows); } };
$("csv").onclick = () => { window.location = "/api/csv?" + params(); };
$("seg").onchange = () => run(loadSegment);
window.onresize = () => { if (tl) drawTimeline(); };
run(async () => {
  for (const e of EVENTS) $("event").insertAdjacentHTML("beforeend", `<option>${e}</option>`);
  const segs = await getJSON("/api/segments");
  for (const s of segs) {
    $("seg").insertAdjacentHTML("beforeend", `<option value="${s.id}">${s.label}</option>`);
  }
  await loadSegment();
});
</script></body></html>
"""


def parse_filter(qs):
    """Query string (parse_qs dict) to filter_rows() arguments; raises ValueError."""
    def get(k):
        return qs.get(k, [""])[0]
    ctl = {c for c in get("ctl").split(",") if c} if "ctl" in qs else None
    events = {e for e in get("events").split(",") if e} or None
    kind = get("kind") or "events"
    if kind not in ("events", "snapshots", "all"):
        raise ValueError("kind")
    return {"ctl": ctl, "kind": kind, "events": events,
            "io_from": int(get("from")) if get("from") else None,
            "io_to": int(get("to")) if get("to") else None}


def segment_info(i, seg):
    times = [r["io_time_ms"] for _, r in seg["rows"] if rclog.io_valid(r)]
    boot = datetime.datetime.fromtimestamp(seg["boot_wall"] / 1000).strftime("%Y-%m-%d %H:%M:%S")
    return {"id": i, "label": "I/O card boot %s (%s, %d rows)" % (
                boot, ", ".join(sorted(seg["controllers"])), len(seg["rows"])),
            "controllers": sorted(seg["controllers"]), "rows": len(seg["rows"]),
            "io_from": min(times) if times else None, "io_to": max(times) if times else None}


def make_server(segments, port, host="127.0.0.1"):
    page = PAGE.replace("__EVENTS__", json.dumps([e for e in rclog.EVENTS.values() if e]))

    class Handler(BaseHTTPRequestHandler):
        def send(self, code, ctype, body, extra=None):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urlparse(self.path)
            qs = parse_qs(url.query)
            try:
                if url.path == "/":
                    self.send(200, "text/html; charset=utf-8", page)
                    return
                if url.path == "/api/segments":
                    body = [segment_info(i, s) for i, s in enumerate(segments)]
                    self.send(200, "application/json", json.dumps(body))
                    return
                seg = segments[int(qs.get("seg", ["0"])[0])]
                if url.path == "/api/timeline":
                    self.send(200, "application/json", json.dumps(timeline(seg["rows"])))
                    return
                rows = filter_rows(seg["rows"], **parse_filter(qs))
                if url.path == "/api/rows":
                    page_no = int(qs.get("page", ["0"])[0])
                    part = rows[page_no * PAGE_SIZE:(page_no + 1) * PAGE_SIZE]
                    body = {"total": len(rows), "page": page_no,
                            "pages": (len(rows) + PAGE_SIZE - 1) // PAGE_SIZE,
                            "columns": COLUMNS, "rows": [row_values(n, r) for n, r in part]}
                    self.send(200, "application/json", json.dumps(body))
                    return
                if url.path == "/api/csv":
                    self.send(200, "text/csv; charset=utf-8", to_csv(rows),
                              {"Content-Disposition": 'attachment; filename="rc-log.csv"'})
                    return
                self.send(404, "text/plain", "not found")
            except (ValueError, IndexError) as e:
                self.send(400, "text/plain", "bad request: %s" % e)

        def log_message(self, fmt, *args):
            pass

    return ThreadingHTTPServer((host, port), Handler)


def download(hosts, save_dir):
    """Downloads /log from each host and saves it; returns {saved path: bytes}."""
    os.makedirs(save_dir, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d_%H%M%S")
    out = {}
    for host in hosts:
        url = "http://%s:%d/log" % (host, LOG_PORT)
        try:
            with urllib.request.urlopen(url, timeout=60) as resp:
                data = resp.read()
        except OSError as e:
            print("%s: download failed: %s" % (host, e), file=sys.stderr)
            continue
        path = os.path.join(save_dir, "%s_%s.bin" % (stamp, host))
        with open(path, "wb") as f:
            f.write(data)
        out[path] = data
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description="Redundant controller log viewer")
    p.add_argument("--hosts", default=os.environ.get("RC_HOSTS", DEFAULT_HOSTS),
                   help="comma-separated controller addresses")
    p.add_argument("--save", default="logs", help="directory for downloaded logs")
    p.add_argument("--open", nargs="+", metavar="FILE", help="view saved logs")
    p.add_argument("--port", type=int, default=VIEW_PORT)
    p.add_argument("--no-browser", action="store_true")
    args = p.parse_args(argv)
    if args.open:
        sources = {}
        for path in args.open:
            with open(path, "rb") as f:
                sources[path] = f.read()
    else:
        sources = download(args.hosts.split(","), args.save)
    if not sources:
        print("no logs to show", file=sys.stderr)
        return 1
    logs = {}
    for label, data in sources.items():
        records, invalid = rclog.decode_file(data)
        name = controller_name(records, os.path.basename(label))
        while name in logs:
            name += "'"
        logs[name] = records
        seqs = "seq %d-%d" % (records[0]["seq"], records[-1]["seq"]) if records else "empty"
        print("%s: controller %s, %d records (%s), %d invalid slots" % (
            label, name, len(records), seqs, invalid))
    server = make_server(merge(logs), args.port)
    url = "http://127.0.0.1:%d/" % server.server_address[1]
    print("viewer at %s (Ctrl-C to stop)" % url)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
