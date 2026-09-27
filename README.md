# Redundant controller with an I/O card referee

Two Raspberry Pi 3B controllers run as Active/Standby. An STM32F3 Discovery
board is the I/O card: it drives the outputs (an 8-LED ring) and acts as
referee, granting Active to exactly one controller. A failed Active is
replaced within 200 ms, simultaneous boot always ends with exactly one
Active, and a broken link between the controllers never creates two
Actives.

Write-up (Korean, with an English summary):
[라즈베리 파이 두 대와 STM32로 이중화(Active/Standby) 제어기 만들기](https://alpentalsystems.com/posts/2026-09-redundant-controller/) (part 1),
[이중화 제어기 2편: 자체 점검(BIT)으로 '살아 있지만 고장 난' 제어기 잡아내기](https://alpentalsystems.com/posts/2026-09-redundant-controller-bit/) (part 2)

Design: [docs/2026-09-27-redundancy-core-design.md](docs/2026-09-27-redundancy-core-design.md)
Measurements: [docs/test-log.md](docs/test-log.md)

## Layout

- `common/`: frame protocol (CRC-16, streaming parser, messages)
- `io-card/`: Zephyr app for the STM32F3 Discovery rev E (referee, outputs, watchdog)
- `central/`: Linux daemon for the controllers (role logic, cross-link, LED chaser)
- `tests/host/`: host tests for the protocol, arbiter, and role logic
- `tools/`: bench helpers

## Wiring

| Controller | Pi pin 8 (TX) | Pi pin 10 (RX) | Pi pin 6 |
|---|---|---|---|
| slot A | PA3 | PA2 | GND |
| slot B | PC11 | PC10 | GND |

Plus one Ethernet cable directly between the two Pis.

Plus a jumper wire from PD12 to PD13 on the STM32F3 Discovery (I/O card
BIT loopback).

## Built-in test and commands

Each controller runs its BIT 1 s after start (PBIT) and every second after
that (CBIT): I/O card link and control loop timing (critical), cross-link,
supply voltage, and CPU temperature. A controller that fails a critical
item reports itself unhealthy, and the I/O card hands Active to a healthy
Standby. The I/O card checks a GPIO loopback, its LED outputs, the sensors,
the UART error rates, and the reset cause.

The blue USER button toggles test mode. In test mode an operator drives the
LEDs over TCP; in operational mode TCP is read-only.

```sh
tools/rcctl.py status          # both controllers side by side
tools/rcctl.py bit             # PBIT and CBIT results
tools/rcctl.py leds 0x55       # test mode only, sent to the Active
tools/rcctl.py lamp            # lamp test
tools/rcctl.py run-bit         # run BIT now
```

Raw protocol: `nc <controller> 5000`, then `STATUS`, `BIT`, `LEDS <hex>`,
`LAMP_TEST`, or `RUN_BIT`, one per line; each reply is one JSON line.

## Log and viewer

Each controller records a status snapshot every 100 ms and an event
record at each event. The daemon sends each 64-byte record to `rc-logd`
over a local socket and never touches the file, so a slow SD card cannot
stall control. `rc-logd` keeps a 10 MiB ring file
(`/var/lib/rc-central/rc-log.bin`, about 4.5 hours) and serves it on port
8080. Records carry the I/O card's clock and boot ID, so both controllers'
logs merge on one timeline.

```sh
tools/rcview.py                       # download both logs, open the viewer
tools/rcview.py --open logs/*.bin     # view saved logs
```

The viewer runs at http://127.0.0.1:8765 (`--port` to change): role
timeline, filters, and CSV export of the filtered rows.

## Build

```sh
# host tests
cmake -S tests/host -B build/host && cmake --build build/host && ctest --test-dir build/host
# I/O card (west workspace from west.yml)
west init -m https://github.com/alpentalsystems/redundant-controller ws && cd ws && west update
west build -b stm32f3_disco@E redundant-controller/io-card && west flash
# controllers (on each Pi)
cmake -S central -B build && cmake --build build && sudo cmake --install build --prefix /usr/local
```

Each Pi needs `enable_uart=1` and `dtoverlay=disable-bt` in
`/boot/firmware/config.txt`, and no `console=serial0` in `cmdline.txt`.
The cross-link port needs no DHCP (otherwise NetworkManager resets it every
45 s): `sudo nmcli con modify netplan-eth0 ipv4.method disabled ipv6.method link-local`.
