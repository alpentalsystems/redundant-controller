# Redundant controller: built-in test and TCP commands (sub-project 2)

Status: draft for owner review, 2026-09-27.

## Context

Sub-project 1 (tag `part1`, design in
`docs/2026-09-27-redundancy-core-design.md`) built two Active/Standby
controllers and an I/O card that acts as referee. This sub-project adds
built-in test, a test/operational mode, and a TCP command interface for an
upper controller. Sub-project 3 (ring-buffer log and PC viewer) stays out
of scope.

No customer hardware, part numbers, or contract details appear in code,
docs, or posts.

## Goals

- Startup test (PBIT) and continuous test (CBIT, every 1 s) on both
  controllers and on the I/O card, with results over TCP.
- A controller that fails a critical test hands Active to a healthy
  Standby. The I/O card still makes every role decision.
- A physical test/operational mode switch. In test mode an operator drives
  the outputs over TCP; in operational mode TCP is read-only.
- A TCP server on both controllers and a client script that routes output
  commands to the Active.
- Measured evidence for four experiments (see Verification).

## Non-goals

- Ring-buffer logging, log download, PC viewer (sub-project 3).
- Authentication or encryption on the TCP interface (known limitation,
  stated in the post).
- Redundancy of the I/O card itself (unchanged from sub-project 1).

## Decisions

1. **BIT drives failover through the referee.** Each controller reports a
   `healthy` flag in its heartbeat. The I/O card moves Active from an
   unhealthy Active to a healthy Standby. Controllers never step down on
   their own.
2. **The mode lives on the I/O card** and is changed only with the USER
   button. The I/O card reports the mode in every STATUS. A remote client
   can never switch a running system into test mode.
3. **Both controllers run the TCP server.** Either one answers queries;
   only the Active accepts output commands. The client talks to both.
4. **Text commands, JSON replies**, one line each. The daemon only writes
   JSON; it never parses it.
5. **I/O card BIT is report-only.** It is not redundant, so its failures
   never change roles.

## Message changes

Both sides are updated together; no compatibility with sub-project 1
frames is kept. Payloads stay little-endian and within 16 bytes.

| Message | Direction | Payload (new fields in bold) |
|---|---|---|
| HEARTBEAT (1) | controller -> I/O | seq (u32), role (u8), **healthy (u8)** |
| STATUS (2) | I/O -> controller | seq (u32), slot (u8), granted role (u8), active slot (u8), **mode (u8)**, **io_fail (u8)** |
| SET_OUTPUTS (3) | controller -> I/O | mask (u8), unchanged |
| PEER (4) | controller <-> controller | seq (u32), slot (u8), role (u8), referee_ok (u8), step (u16), **healthy (u8)**, **test_mask (u8)** |
| **RUN_BIT (5)** | controller -> I/O | empty |

- `mode`: 0 = operational, 1 = test.
- `io_fail`: one bit per I/O card test item that is failing (see below).
- The I/O card acts on RUN_BIT only from the Active; the result appears in
  the next STATUS.

## Controller BIT

| Bit | Item | Pass when | Critical |
|---|---|---|---|
| 0 | I/O card link | a valid STATUS within the last 100 ms, and fewer than 5 CRC or length errors in the last 1 s | yes |
| 1 | Control loop timing | no heartbeat interval over 50 ms since the last check | yes |
| 2 | Cross-link | a PEER message within the last 100 ms | no |
| 3 | Supply voltage | `in0_lcrit_alarm` of the `rpi_volt` hwmon device is 0 (same flag as `vcgencmd get_throttled` bit 0, read without blocking) | no |
| 4 | CPU temperature | below 80 C (`/sys/class/thermal/thermal_zone0/temp`) | no |

- Each item reports pass, fail, or error, plus its measured value. Error
  (for example `vcgencmd` missing) counts as fail for that item and is
  never fatal to the daemon.
- **PBIT:** the daemon sends heartbeats from start with `healthy = 0`.
  After 1 s it runs the first check; that result is logged and kept as the
  PBIT result.
- **CBIT:** the same check every 1 s after that. `RUN_BIT` over TCP runs
  it immediately.
- **Health:** a passing PBIT sets `healthy = 1` at once, so a booting
  controller is healthy before the 1.5 s election window ends. After
  that, the controller becomes unhealthy on the first check with a failed
  critical item, and healthy again only after 3 consecutive checks with
  every critical item passing. Non-critical items never change health.

## I/O card BIT

| Bit | Item | Pass when |
|---|---|---|
| 0 | GPIO loopback | PD12 (output) driven high then low is read back on PD13 (input) through a jumper wire |
| 1 | LED readback | the LED output pins match the last applied mask |
| 2 | Sensors | the LSM303AGR accelerometer and magnetometer devices are ready |
| 3 | Slot A UART | fewer than 5 CRC or length errors in the last 1 s |
| 4 | Slot B UART | same for slot B |
| 5 | Reset cause | the last reset was not caused by the watchdog (startup only; stays set until the next reset) |

The I/O card runs these at boot, every 1 s, and on RUN_BIT from the
Active, and logs every change of `io_fail` on its console.

## Arbitration changes (I/O card)

| Situation | Decision |
|---|---|
| Election at the end of the window | reported Active and healthy -> healthy -> reported Active -> present (slot A first at each step) |
| Active lost (heartbeat timeout) | the other slot if present, healthy or not (unchanged) |
| Active present but unhealthy, other present and healthy, for 1.2 s without a break | move Active to the other slot |
| Active unhealthy, other absent or unhealthy | keep the current Active |
| Unhealthy slot recovers | no fallback: it stays Standby |

The 1.2 s hold covers common-mode faults: after an I/O card restart or
stall both controllers turn unhealthy and recover up to one BIT period
apart, and neither may take Active from the other in that time.

Timing target for an unhealthy handover: up to 1.1 s for the controller's
BIT to see the fault, the 1.2 s hold, and one heartbeat. Goal: under 2.5 s
from the fault.

## Test mode

- Entering test mode: the Active pauses the chaser at its current step and
  drives the operator mask, which starts at 0 (all off).
- The operator mask is shared in PEER, so a new Active after a failover in
  test mode keeps it.
- Lamp test: all LEDs on for 1 s, off for 1 s, then back to the operator
  mask.
- Returning to operational mode: the chaser resumes from the paused step;
  the operator mask resets to 0.
- The board has no spare user LED (LD3-LD10 are the output ring; LD1 and
  LD2 are not software controlled). The mode shows on the I/O card
  console, in `STATUS`, and by the chaser stopping.
- The USER button is debounced (50 ms); one press toggles the mode once.

## TCP interface

Port 5000 on every interface, up to 4 clients, lines up to 128 bytes, LF or
CRLF endings, leading and trailing spaces ignored, commands case-sensitive.

| Command | Operational mode | Test mode | On the Standby |
|---|---|---|---|
| `STATUS` | reply | reply | reply |
| `BIT` | reply | reply | reply |
| `LEDS <hex>` (0x00-0xFF) | `operational mode` error | sets the operator mask | `not active` error |
| `LAMP_TEST` | `operational mode` error | runs the lamp test | `not active` error |
| `RUN_BIT` | `operational mode` error | runs controller BIT now, sends RUN_BIT, replies ok; the client then reads BIT | `not active` error |
| other | `unknown command` error | same | same |

Replies (one line each):

```
{"ok":true,"slot":"A","role":"active","mode":"test","healthy":true,"active":"A","referee":true,"peer":true,"step":123,"mask":85}
{"ok":true,"healthy":true,"pbit":{"items":[...]},"cbit":{"age_ms":420,"items":[{"name":"io_link","result":"pass","critical":true,"value":"status_age_ms=12 errors=0"}]},"io_fail":0}
{"ok":true}
{"ok":false,"error":"not active","active":"B"}
{"ok":false,"error":"operational mode"}
{"ok":false,"error":"unknown command"}
{"ok":false,"error":"line too long"}
```

In the `BIT` reply, `pbit` and `cbit` both hold the five items in the order
of the controller BIT table (`io_link`, `loop_timing`, `cross_link`,
`supply_voltage`, `cpu_temp`); `result` is `pass`, `fail`, or `error`;
`age_ms` is the time since the last check.

`LEDS` with a value outside 0x00-0xFF replies `{"ok":false,"error":"bad value"}`.
A line longer than 128 bytes gets `line too long` and the client is
disconnected. A client that disconnects or errors is logged and dropped;
the daemon and other clients continue.

`tools/rcctl.py <command>` connects to both controllers (addresses from
arguments or environment), prints `status` and `bit` side by side, and
sends `leds`, `lamp`, and `run-bit` to the one reporting Active, retrying
once on the other after a `not active` reply.

## Code structure

| Unit | Responsibility | Tested on host |
|---|---|---|
| `common/proto.c` | new fields and RUN_BIT | yes |
| `io-card/src/arbiter.c` | health-aware election and handover | yes |
| `io-card/src/io_bit.c` | I/O card item evaluation from raw readings | yes |
| `io-card/src/mode.c` | button debounce and mode toggle | yes |
| `io-card/src/main.c` | hardware reads, RUN_BIT, console | no (board) |
| `central/src/bit.c` | controller item limits and health hysteresis | yes |
| `central/src/cmd.c` | one command line plus state in, JSON reply plus action out | yes |
| `central/src/role.c` | test-mode output, lamp test, pause and resume | yes |
| `central/src/main.c` | TCP sockets in the poll loop, reading voltage and temperature | no (board) |
| `tools/rcctl.py` | client | manual |

## Verification

Host tests are written first for every unit marked above: arbiter health
rules, BIT limits and hysteresis, every command in every state including
malformed input, test-mode output sequences, new message fields, and
button debounce.

Bench experiments, 3 runs each (experiment 4: one run), results in `docs/test-log.md`:

1. **PBIT:** boot both controllers; both report PBIT results; supply
   voltage shows fail (non-critical) on the current bench.
2. **Unhealthy handover:** disconnect the Active controller's RX wire (I/O
   card to controller). Pass: the Standby becomes Active within 2.5 s;
   after reconnecting, the controller is healthy again after about 3 s and
   stays Standby.
3. **Test mode:** press USER; `rcctl leds 0x55` lights alternate LEDs;
   stop the Active's daemon; the new Active keeps 0x55; press USER; the
   chaser resumes and `rcctl leds 0x0f` is refused.
4. **I/O card BIT:** remove the loopback jumper. Pass: `io_fail` bit 0 is
   set within 1 s and no role changes.
5. **I/O card restart (regression):** hold the STM32 in reset for 3 s
   with OpenOCD while the system runs. Pass: no role change; both
   controllers report unhealthy, then healthy again.

Both controllers still report under-voltage; timing results carry that
note.

## Build and bench setup

Unchanged from sub-project 1, plus one jumper wire from PD12 to PD13 on the
STM32F3 Discovery.
