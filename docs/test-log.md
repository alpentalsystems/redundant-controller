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
