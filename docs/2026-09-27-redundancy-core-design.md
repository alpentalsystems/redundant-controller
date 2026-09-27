# Redundant controller: redundancy core design (sub-project 1)

Status: approved 2026-09-27.

## Context

Demo project for Alpental Systems: a redundant (Active/Standby) controller
built from boards on hand, as a generalized version of the "redundant control
plus built-in test" structure common in defense and industrial control units.
No customer hardware, part numbers, or contract details appear in code, docs,
or posts.

The full project has three sub-projects, each with its own spec, plan, and
blog post:

1. **Redundancy core** (this document): heartbeat, Active/Standby roles,
   boot arbitration, failover, and output control through the I/O card.
2. Built-in test (PBIT/CBIT), test/operational mode, TCP command server.
3. Ring-buffer logging and a PC log viewer.

## Goals

- Exactly one Active controller at all times, including at simultaneous boot
  and when the link between the two controllers breaks.
- Failover to the Standby within 200 ms after the Active loses power.
- The same software image on both controllers; identity comes from wiring.
- Measured evidence for four experiments (see Verification).

## Non-goals (for this sub-project)

- Built-in test, TCP command interface, logging to a ring buffer, PC viewer
  (sub-projects 2 and 3).
- State replication beyond the demo application's step counter.
- Redundancy of the I/O card itself (it is a single point of failure here;
  documented as a known limitation).
- Hardware pulse ("I'm alive") lines; may be added later.

## Architecture

```
            home Wi-Fi router (2.4 GHz for the Pi 3B)
           /                |                    \
        Mac          rc-a (slot A, Wi-Fi)   rc-b (slot B, Wi-Fi)    management: SSH,
   (later: upper            |                    |                  later the upper
    controller)             +---- Ethernet ------+                  controller (TCP)
                            |     (cross-link)   |
                      UART (3 wires)       UART (3 wires)
                            v                    v
                 STM32 USART2 (PA2/PA3)   STM32 UART4 (PC10/PC11)
                            +-- STM32F3 Discovery (I/O card, referee) --+
```

| Role | Board | Software |
|---|---|---|
| Controller slot A | Raspberry Pi 3B `rc-a` | Linux daemon in C |
| Controller slot B | Raspberry Pi 3B `rc-b` | same image and daemon |
| I/O card and referee | STM32F3 Discovery rev E | Zephyr 4.4.2 |
| Upper controller (later) | Mac or Pi 5 | client script |

The controllers are the "brains": both run the same program all the time
(hot standby). The I/O card is the hands and eyes (LEDs, buttons, GPIO). It
is not a dedicated monitor: it acts as referee because every output command
already passes through it.

### Key decisions

1. **The I/O card is the referee.** It grants Active to exactly one
   controller. Decisions are never made by the controllers alone.
2. **Outputs exist only on the I/O card.** The I/O card applies output
   commands only from the controller it granted Active. A controller that
   believes it is Active but cannot reach the I/O card cannot drive any
   output, so split-brain cannot reach the outputs.
3. **Slot identity comes from wiring.** The UART a controller is connected
   to defines its slot (USART2 = A, UART4 = B). The I/O card tells each
   controller its slot. Both controllers run the same image.
4. **The cross-link is for monitoring, not for decisions.** Controllers
   exchange heartbeats over the direct Ethernet cable to detect and log
   peer loss. Losing the cross-link never changes roles.
5. **Fail-safe I/O card.** The I/O card feeds a hardware watchdog (IWDG).
   If its firmware stops, it resets, and every output returns to the safe
   state (off) on reset.
6. **The cross-link checks the referee.** Each controller shares its granted
   role with the peer. If both are granted Active at the same time, both
   report a referee fault and stop sending outputs.

## Demo application: LED chaser

The Active controller moves one lit LED around the ring, one step every
200 ms, through SET_OUTPUTS. It sends the current step to the Standby over
the cross-link, so a new Active continues from the same step (bumpless
transfer). The takeover is visible, and a jump in the pattern would show a
missed state update.

## Roles and arbitration (I/O card)

Per slot, the I/O card tracks: link state (heartbeat received within the
timeout) and the role the controller reports.

- **Heartbeat timeout:** a slot is lost when no valid frame arrives for
  100 ms.
- **Election window:** when no slot is Active and a controller appears,
  the I/O card waits 1500 ms from that first appearance, then grants
  Active: a present slot that reports Active first (slot A if both),
  otherwise slot A, otherwise slot B. During the window STATUS grants
  role 0 (unknown), and a controller keeps its current role on
  "unknown", so an I/O card reboot under a running system causes no role
  change.
- **Failover:** when the Active slot is lost and the other slot is present,
  the other slot gets Active immediately.
- **No fallback:** when a lost slot returns, it gets Standby. Active never
  moves back on its own.
- **No controller present:** outputs go to the safe state (all LEDs off)
  and no slot is Active until one appears.

## Controller behavior (Pi daemon)

- Sends a heartbeat frame to the I/O card every 20 ms with its sequence
  number and the role it believes it has.
- Takes the role the I/O card grants in each status frame. It becomes
  Active only when granted, and steps down to Standby as soon as a status
  frame grants Standby.
- If no status frame arrives for 100 ms, it reports "referee lost" but keeps
  its role and keeps sending output commands: the I/O card only applies them
  from the slot it granted Active, so a controller that lost only the I/O
  card's replies (a broken I/O-card-to-controller wire) keeps the outputs
  alive instead of freezing them.
- Sends a heartbeat to the peer over UDP every 20 ms and logs peer loss
  and recovery with timestamps.
- Logs every role change with a monotonic timestamp.
- Shares its granted role and the chaser step with the peer; reports a
  referee fault if the peer is also granted Active.

## Messages

### UART framing (controller <-> I/O card)

115200 baud, 8N1. Binary frames:

| Field | Size | Notes |
|---|---|---|
| sync | 1 | 0xA5 |
| type | 1 | message type |
| len | 1 | payload length |
| payload | len | little-endian fields |
| crc | 2 | CRC-16/CCITT-FALSE over type, len, payload |

Frames with a bad CRC or length are dropped and counted. The CRC is sent
low byte first.

| Type | Direction | Payload |
|---|---|---|
| HEARTBEAT | controller -> I/O | seq (u32), reported role (u8) |
| STATUS | I/O -> controller | echoed seq (u32), slot (u8), granted role (u8), active slot (u8) |
| SET_OUTPUTS | controller -> I/O | LED mask (u8) |

Roles: 0 = unknown, 1 = standby, 2 = active.

### Cross-link heartbeat (controller <-> controller)

UDP to the IPv6 link-local multicast group `ff02::1` on `eth0`, port 47000
(no address configuration needed), every 20 ms: slot, granted role, seq,
referee link state, chaser step. A controller ignores messages carrying its
own slot. A send failure on the cross-link is logged and never stops the
controller. A peer's Active claim counts as a referee fault only if it
arrives after this controller became Active.

## I/O card indications

- The LED ring shows the output mask set by the Active controller.
- All LEDs are off while no slot is Active; every Active change is logged
  on the console with the gap from the lost slot's last frame.

## Timing budget

Active loses power -> I/O card timeout (100 ms, plus up to 20 ms until the
next expected frame) -> grant to the Standby in its next status frame
(up to 20 ms) -> worst case about 140 ms, under the 200 ms goal.

## Verification

Host tests (written first) for the arbitration logic on the I/O card and the
role logic in the daemon, as portable C modules like the compass project.

Experiments on the boards, results recorded in `docs/test-log.md`:

1. **Power loss:** pull the Active controller's power 3 times; measure the
   time until the Standby is granted Active. Goal: under 200 ms every time.
2. **Cross-link loss:** unplug the Ethernet cable between the controllers;
   roles must not change.
3. **Simultaneous boot:** power both controllers together 3 times;
   exactly one Active every time and no referee fault. Slot A wins only when
   both appear within the election window; Pi boot times vary by seconds.
4. **I/O card loss:** hold the STM32 in reset or remove its power while the
   system runs; outputs go off, both controllers report "referee lost", and
   after recovery no role changes.

Timing is taken from I/O card and daemon logs; if a logic analyzer is
available, GPIO markers on grant and role change give an independent
measurement.

## Rejected alternative: controllers decide alone (option B)

Two controllers alone cannot tell a crashed peer from a broken link. If the
Standby takes over whenever heartbeats stop, a broken cross-link gives two
Actives driving the same outputs; if it waits, a real failure leaves no
Active. The I/O card is already a single point of failure for all I/O in
both options, so using it as the referee adds no new failure mode for the
system function; the added risk is a referee logic bug, covered by the
watchdog, the cross-check, and host tests. Option B fits systems where each
controller has its own output path (duplicated I/O with downstream voting),
with several independent links and fencing.

## Real-world counterparts

- Redundant (hot-standby) PLCs: two CPUs with a sync link, switched I/O
  that follows the primary, bumpless transfer on failover.
- Defense and aerospace control units: two CPU cards plus a discrete I/O
  card, operator console over Ethernet, built-in test, recorded logs.
- Railway door and signaling I/O: outputs must fall back to a safe state
  when a controller freezes.
- Satellites and ships: cross-strapped computers with simple arbitration
  logic; class rules require surviving a single controller loss.

## Repository layout

```
redundant-controller/
  docs/          design, plan, test log
  io-card/       Zephyr app for the STM32F3 (referee, outputs)
  central/       Linux daemon for the Pi controllers
  common/        frame encoding and CRC shared by both sides, host-tested
  tests/host/    host tests
```

## Open questions

- Pi 3B UART latency and scheduling jitter under load: measured in
  experiment 1, may require a real-time priority for the daemon.

## Build setup

The repository has its own `west.yml` pinned to Zephyr v4.4.2 (same module
allowlist as the compass project), so it builds on its own. Local builds may
reuse an existing Zephyr 4.4.2 checkout through `ZEPHYR_BASE`.
