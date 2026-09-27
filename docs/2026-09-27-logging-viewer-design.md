# Redundant controller: ring-buffer log and Mac log viewer (sub-project 3)

Status: draft for owner review, 2026-09-27.

## Context

Sub-projects 1 and 2 (tags `part1`, `part2`) built two Active/Standby
controllers, an I/O card that acts as referee, built-in test, a test mode,
and a TCP command interface. This sub-project records what the controllers
do and gives an operator a way to download and read it. The original spec
asked for a PC program; the viewer runs on a Mac and stays portable.

No customer hardware, part numbers, or contract details appear in code,
docs, or posts.

## Goals

- Each controller keeps a fixed-size ring log on its SD card: a status
  snapshot every 100 ms plus an event record at the moment of each event.
- The logs of both controllers merge on one timeline accurate to about
  1 ms, even without network time.
- Downloading the log never disturbs the real-time control loop.
- A Mac viewer shows the merged log as a table with filters and a role
  timeline, and exports the filtered rows as CSV.
- Measured evidence for four experiments (see Verification).

## Non-goals

- Log download over the TCP command port (a separate server is used).
- Log records from the I/O card itself (it has no storage); its Active
  slot and BIT result reach the log through STATUS.
- Authentication or encryption on the log server (known limitation, as for
  the command port).
- Native macOS app, `.xlsx` export (CSV opens in Excel and Numbers).
- Graphs beyond the role timeline strip.

## Decisions

1. **The I/O card's clock is the shared timebase.** STATUS carries the I/O
   card's uptime in milliseconds. Each controller logs it next to its own
   uptime and the wall-clock time. Records from both controllers line up
   within about 1 ms, with or without network time.
2. **Snapshots and events.** A snapshot every 100 ms gives the continuous
   picture; an event record written at once gives the exact time.
3. **The real-time daemon never touches the file.** It sends each record
   to `rc-logd` over a non-blocking local datagram socket. `rc-logd` writes,
   flushes, and serves the file. A write to a busy SD card can block any
   process for seconds (measured: 3.4 s and 3.7 s during a 300 MB copy,
   which caused a handover when the daemon wrote the file itself), so file
   I/O stays out of the real-time process.
4. **A local web viewer on the Mac.** Python standard library and one HTML
   page with inline script; no external packages, no internet access.

## Message change

| Message | Direction | Payload (new field in bold) |
|---|---|---|
| STATUS (2) | I/O -> controller | seq (u32), slot (u8), granted role (u8), active slot (u8), mode (u8), io_fail (u8), **io_time_ms (u32)**, **io_boot_id (u16)** |

The payload grows from 9 to 15 bytes. `io_time_ms` is the I/O card's
uptime (`k_uptime_get()`, lower 32 bits) when the STATUS is built.
`io_boot_id` identifies one I/O card run: the I/O card sets it when the
first heartbeat arrives, from the CPU cycle counter (this STM32 has no
random number generator; the arrival moment differs at every start), and
never uses 0.

## Log record

64 bytes, little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `0x4C52` ("RL") |
| 2 | 1 | version, 1 |
| 3 | 1 | type: 1 = snapshot, 2 = event |
| 4 | 4 | seq: increases by one per record, continues across daemon restarts |
| 8 | 4 | io_time_ms: I/O card time at the record |
| 12 | 8 | mono_ms: controller uptime (CLOCK_MONOTONIC) |
| 20 | 8 | wall_ms: wall-clock time, Unix ms (CLOCK_REALTIME) |
| 28 | 1 | slot |
| 29 | 1 | role |
| 30 | 1 | mode |
| 31 | 1 | flags: bit 0 healthy, 1 referee ok, 2 peer ok, 3 peer healthy, 4 fault, 5 io_time valid |
| 32 | 1 | active slot (from the last STATUS) |
| 33 | 1 | io_fail (from the last STATUS) |
| 34 | 2 | BIT results: 2 bits per controller item in item order (0 pass, 1 fail, 2 error, 3 not run) |
| 36 | 2 | chaser step |
| 38 | 1 | output mask (`role_current_mask`) |
| 39 | 1 | event code (0 for snapshots) |
| 40 | 4 | event detail |
| 44 | 2 | io_boot_id from the last STATUS (0 before the first STATUS) |
| 46 | 16 | reserved, zero |
| 62 | 2 | CRC-16/CCITT-FALSE over bytes 0-61, low byte first |

**I/O card time.** `io_time_ms` = the value in the last STATUS plus the
controller time since that STATUS arrived. Before the first STATUS, the
"io_time valid" flag is clear and the field is 0.

**Events and their detail:**

| Code | Event | Detail |
|---|---|---|
| 1 | daemon started | 0 |
| 2 | role changed | new role |
| 3 | healthy | 0 |
| 4 | unhealthy | 0 |
| 5 | referee lost | 0 |
| 6 | referee back | 0 |
| 7 | peer lost | 0 |
| 8 | peer back | 0 |
| 9 | mode changed | new mode |
| 10 | fault set | 0 |
| 11 | fault cleared | 0 |
| 12 | BIT item changed | item << 8 \| result |
| 13 | TCP output command | action << 8 \| LED value |
| 14 | slot learned | slot |
| 15 | log dropped | records the daemon could not hand to `rc-logd` |

## Ring file

- Path `/var/lib/rc-central/rc-log.bin`, 163,840 slots of 64 bytes
  (10 MiB); about 4.5 hours at 10 snapshots per second plus events.
- `rc-logd`'s systemd unit gets `StateDirectory=rc-central`, which creates
  `/var/lib/rc-central`, and `RuntimeDirectory=rc-logd` for the socket
  `/run/rc-logd/log.sock`.
- The daemon encodes each record with `seq` 0 and sends it with
  `MSG_DONTWAIT`. `rc-logd` stamps the next `seq` and writes the record to
  slot `seq mod 163840` with one `pwrite` of 64 bytes.
- If a send fails (socket buffer full, `rc-logd` not running), the daemon
  counts the record as dropped and reports the failure once. On the next
  successful send it writes a "log dropped" event with the count. Control
  never waits for logging.
- At start `rc-logd` reads the whole file once, takes the highest valid
  `seq`, and continues with the next one. Records with a wrong magic,
  version, or CRC are ignored. A missing or short file is extended to full
  size; missing slots read as invalid. A write error is reported once.

## Log server (`rc-logd`)

- Python 3 standard library, systemd unit with `Nice=10` and the lowest
  best-effort I/O priority, port 8080 on every interface.
- Receives record datagrams on `/run/rc-logd/log.sock`; invalid ones are
  reported once and ignored.
- `GET /log`: the whole file as `application/octet-stream`.
- `GET /info`: JSON `{"size": ..., "records": ..., "max_seq": ...}`
  (valid records and the highest seq).
- Every 2 s it calls `fsync` on the log file. On power loss at most about
  the last 2 s are lost.
- If it stops, control continues and the daemon counts dropped records.

## Viewer (`tools/rcview.py`)

- `rcview.py [--hosts H1,H2] [--save DIR]`: downloads `/log` from both
  controllers, saves the raw files as `DIR/<date>_<time>_<host>.bin`
  (default `logs/`), decodes them, and serves the page at
  `http://127.0.0.1:8765`, opening it in the browser.
- `rcview.py --open FILE [FILE ...]`: the same from saved files.
- Decoding checks magic, version, and CRC; invalid slots are skipped and
  counted.
- **Segments:** within one controller's log, ordered by `seq`, a new
  `io_boot_id` or a drop in `io_time_ms` of more than 1 s starts a new
  segment (I/O card restart).
  Records without a valid `io_time_ms` go into the segment of the next
  valid record. Each segment's I/O card boot time is estimated as the
  median of `wall_ms - io_time_ms`. Segments from the two logs with the
  same `io_boot_id` are the same I/O card run and merge, whatever the
  controllers' wall clocks say; segments without an ID fall back to boot
  times within 10 s.
- **Merge:** within a segment, rows are ordered by `io_time_ms`, then by
  controller, then by `seq`.
- **Page:** segment selector, time range, filters (controller, snapshots
  and/or events, event type), a role timeline strip per controller with
  event markers (click to jump the table), and a table paged at 500 rows.
  Default view: events only, both controllers.
- **CSV:** exactly the filtered rows, one column per decoded field, times
  as numbers.

## Code structure

| Unit | Responsibility | Tested on host |
|---|---|---|
| `common/proto.c` | `io_time_ms` in STATUS | yes |
| `common/logrec.c` | encode, decode, and CRC of one record | yes |
| `central/src/role.c` | I/O card time at a given moment | yes |
| `central/src/main.c` | snapshots every 100 ms, event records, send to `rc-logd` | no (board) |
| `io-card/src/main.c` | fill `io_time_ms` | no (board) |
| `central/rc_logd.py` and unit | receive, write, flush, and serve the log | yes (temporary files, local socket and server) |
| `tools/rcview.py` | download, decode, merge, page, CSV | yes |

## Verification

Host tests are written first: STATUS round trip with the new field; record
round trip, CRC, bad magic and version, BIT packing; ring append, wrap,
resume, corrupt slot, short file (in `rc-logd`); records received on the
socket; I/O card time estimate; decoding a log
written by the C code (so C and Python formats cannot drift); merging,
segments, and CSV; the log server's `/info` and `/log`.

Bench experiments, one run each, results in `docs/test-log.md`:

1. **Logging load:** 10 minutes of normal operation. Pass: about 6,000
   snapshots per controller with no seq gaps; `loop_timing` maximum stays
   near 20 ms.
2. **Failover in the viewer:** pull the Active controller's RX wire. Pass:
   the merged view shows both controllers' events in order, and the
   handover time in the log matches the I/O card console within 5 ms.
3. **Power loss:** pull the Active controller's power. Pass: after it
   reboots, its log ends no more than about 2 s before the handover.
4. **Download under load:** download both full logs five times in a row
   while the system runs. Pass: no `loop_timing` failure, no role change.

5. **SD card stress:** copy 300 MB to the Active controller's SD card
   while the system runs. Pass: no `loop_timing` failure, no role change,
   no dropped records.

A viewer screenshot for the post is taken during experiment 2. Both
controllers still report under-voltage; results carry that note.

## Build and bench setup

Unchanged from sub-project 2, plus `rc-logd` installed and enabled by
`tools/deploy-central.sh`.
