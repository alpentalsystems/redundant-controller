# Test log: redundancy core

Date: 2026-09-27. Firmware and daemon from commit 686cd13.

## Conditions

- Controllers: two Raspberry Pi 3B (rc-a = slot A, rc-b = slot B),
  Raspberry Pi OS Lite 64-bit (Debian 13), daemon at SCHED_FIFO 50.
- I/O card: STM32F3 Discovery rev E, Zephyr 4.4.2.
- Both Pis reported under-voltage during all runs
  (`vcgencmd get_throttled` = `0x50005`). Timing may be different on
  proper 5 V / 2.5 A supplies.
- Times come from the I/O card console (`active: X -> Y gap_ms=N`, where
  N is the time from the lost slot's last valid frame to the new grant)
  and the daemon journals.

## Experiment 1: power loss

Pull the power cable of the Active controller. Pass: `gap_ms` under 200
and the new Active continues the chaser from the old Active's step.

| Run | Handover | gap_ms | New Active's first step | Result |
|---|---|---|---|---|
| 1 | A -> B | 101 | 1563 (from peer) | pass |
| 2 | B -> A | 102 | 1995 (from peer) | pass |
| 3 | B -> A | 101 | 383 (from peer) | pass |

One more pull is not counted: rc-a was pulled before rc-b had booted, so
no controller was present for 4.3 s (`A -> -`, then `- -> B`), and rc-b
started from step 0 because it had no peer to learn the step from.

Before these runs, a kernel hard reboot (sysrq `b`) of the Active gave
`gap_ms` 101 and 102.

In run 3, rc-a logged `peer_lost` on the cross-link 362 ms before it was
granted Active, while the I/O card's gap stayed 101 ms. The pulled Pi's
Ethernet stopped before its CPU and UART did. Roles follow the I/O card
only, so this did not delay the handover.

## Experiment 2: cross-link loss

Run with `ip link set eth0 down` on rc-b for 10 s instead of pulling the
cable.

| Run | Observed | Result |
|---|---|---|
| 1 | `peer_lost` then `peer_back` on both; no `role_changed`; no `active:` line; chaser kept stepping | pass |

The first attempt was void: NetworkManager ran DHCP on eth0 (there is no
DHCP server on the direct cable), gave up after 45 s, and removed the
link-local address, so the cross-link dropped every 45 s. Fixed on both
Pis with `nmcli con modify netplan-eth0 ipv4.method disabled
ipv6.method link-local` (see README); after the fix, 120 s showed no
eth0 state change and no peer loss.

## Experiment 3: simultaneous boot

Both Pis on one switched power strip, off for about 5 s, then on. Pass:
exactly one Active and no `event=fault` on either Pi.

| Run | Active after boot | Faults | Result |
|---|---|---|---|
| 1 | B | none | pass |
| 2 | A | none | pass |
| 3 | B | none | pass |

The design first also required slot A to win. The controller daemons
started up to about 8 s apart after the same power-on, which is longer
than the 1500 ms election window, so the first Pi up wins. Slot A wins
only when both appear within the window (run 2). The pass rule was
changed to "exactly one Active".

At each power-off the I/O card logged a grant to the other slot for
8-14 ms (`A -> B`, then `B -> -`): both heartbeats stopped together, but
the two 100 ms timeouts expired a few ms apart. No outputs were affected;
both controllers were off.

## Experiment 4: I/O card loss

Run with OpenOCD (`reset halt`, 3 s, `resume`) instead of the RESET
button. The chip is held at the reset vector, so the LED GPIOs are in
their reset state (off); this was not checked by eye.

| Run | Observed | Result |
|---|---|---|
| 1 | `referee_lost` then `referee_back` on both (about 3.1 s); no `role_changed`; I/O card `- -> A` at t=1501 ms, the previous Active | pass |

A reflash of the I/O card while both controllers ran gave the same
result (`- -> B` at t=1516 ms, B was Active before).

# Sub-project 2: built-in test and TCP commands

Date: 2026-09-27. Firmware from commit 189056e, daemon from commit
b191e07. Both Pis still reported under-voltage (`rpi_volt`
`in0_lcrit_alarm` = 1 most of the time). Output masks below come from the
controllers' `STATUS` replies.

## Experiment 1: PBIT

Restart both daemons together.

| Run | rc-a | rc-b | Result |
|---|---|---|---|
| 1 | `pbit result=pass`, healthy | `pbit result=pass`, healthy | pass |
| 2 | same | same | pass |
| 3 | same | same | pass |

Every run: `io_link`, `loop_timing` (max interval 20-21 ms), `cross_link`
and `cpu_temp` (47-49 C) pass; `supply_voltage` fails with
`undervoltage=1`, as expected on this bench. It is not critical, so both
controllers stay healthy. The flag comes and goes with load, so it
sometimes passes a few seconds later.

## Experiment 2: unhealthy handover

Pull the Active controller's RX wire (Pi pin 10, I/O card to controller)
for about 10 s, then reconnect.

| Run | Handover | I/O card `gap_ms` | Fault to `unhealthy` | Fault to handover |
|---|---|---|---|---|
| 1 | B -> A | 2 | 444 ms | about 1.65 s |
| 2 | A -> B | 3 | 536 ms | about 1.74 s |
| 3 | B -> A | 3 | 524 ms | about 1.72 s |

All pass (target under 2.5 s). The small `gap_ms` shows that the old
Active was still sending heartbeats: the handover came from its health
flag, not from a timeout. Sub-project 1 could not detect this fault. The
fault-to-handover time is the controller's measured detection time plus
the arbiter's 1.2 s hold. Each time, the new Active continued the chaser
from the old Active's step. The old Active was healthy again about 3.5 s
after the wire was reconnected, and it stayed Standby.

## Experiment 3: test mode

| Run | USER -> test | `leds 0x55` | Stop the Active's daemon | `lamp` | USER -> operational | Result |
|---|---|---|---|---|---|---|
| 1 | both `mode=test`, mask 0 | ok on A | B takes over, mask 85 (0x55) | back to 85 | chaser runs, `leds` refused | pass |
| 2 | same | ok on B | A takes over, mask 85 | back to 85 | same | pass |
| 3 | same | ok on A | B takes over, mask 85 | back to 85 | same | pass |

In run 1, four clients also sent 35,648 `RUN_BIT` commands in 3 s to the
Active. There was no `loop_timing` failure, and no health or role change.
The rate limit and the rule that on-demand checks never change health
held. Each accepted command still writes a log line.

## Experiment 4: I/O card BIT

Remove the PD12-PD13 loopback jumper for a few seconds, then put it back
(one run, by owner decision; two pulls were recorded).

| Pull | `io_bit: fail=0x01` | `io_bit: fail=0x00` | Role change | Result |
|---|---|---|---|---|
| 1 | t=490.4 s | t=494.4 s | none | pass |
| 2 | t=497.4 s | t=499.4 s | none | pass |

## Experiment 5: I/O card restart (regression)

Hold the STM32 in reset for 3 s with OpenOCD while the system runs.

| Run | Before | After | Controllers | Result |
|---|---|---|---|---|
| 1 | B Active | `- -> B` at 1525 ms | both `referee_lost`, `unhealthy`, `referee_back`, `healthy`; no `role_changed` | pass |
| 2 | B Active | `- -> B` at 1510 ms | same | pass |
| 3 | B Active | `- -> B` at 1516 ms | same | pass |

The final review found that the first version checked `io_link` only at
the moment of each check. A short I/O card stall could then be seen by
only one controller and move Active. After the fix (the longest STATUS
gap over the whole period), five 150 ms debugger stalls were each seen
by both controllers (`io_link` fail, unhealthy, healthy again after 3 s),
with no role change.
