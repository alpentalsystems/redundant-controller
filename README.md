# Redundant controller with an I/O card referee

Two Raspberry Pi 3B controllers run as Active/Standby. An STM32F3 Discovery
board is the I/O card: it drives the outputs (an 8-LED ring) and acts as
referee, granting Active to exactly one controller. A failed Active is
replaced within 200 ms, simultaneous boot always ends with exactly one
Active, and a broken link between the controllers never creates two
Actives.

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
