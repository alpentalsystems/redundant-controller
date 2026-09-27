# Redundancy Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Tasks 5, 7, and 8 need the owner at the bench (power, cables), so run them inline.

**Goal:** Two Raspberry Pi 3B controllers run as Active/Standby under an STM32F3 I/O card that acts as referee, with deterministic boot arbitration, failover under 200 ms, and a visible LED chaser that continues across a takeover.

**Architecture:** Three portable C modules carry all decisions and are tested on the host first: the frame protocol (`common/`), the referee's arbitration (`io-card/src/arbiter.c`), and the controller's role logic (`central/src/role.c`). Thin platform layers wrap them: a Zephyr app on the STM32 (UART interrupts, LEDs, watchdog) and a Linux daemon on the Pis (serial, UDP cross-link, systemd).

**Tech Stack:** C11, Zephyr v4.4.2 + SDK 1.0.1 (STM32F3 Discovery rev E), Raspberry Pi OS Lite 64-bit (Debian 13), CMake/CTest, OpenOCD, GitHub Actions.

**Spec:** `docs/2026-09-27-redundancy-core-design.md`

## Global Constraints

- UART: 115200 baud, 8N1. Frame = sync 0xA5, type, len, payload (little-endian fields), CRC-16/CCITT-FALSE over type, len, payload (sent low byte first). Max payload 16 bytes.
- Message types: HEARTBEAT=1 (seq u32, role u8), STATUS=2 (seq u32, slot u8, granted role u8, active slot u8), SET_OUTPUTS=3 (mask u8), PEER=4 (seq u32, slot u8, role u8, referee_ok u8, step u16).
- Roles: 0 unknown, 1 standby, 2 active. Slots: A=0 (USART2, PA2/PA3), B=1 (UART4, PC10/PC11), none=0xFF.
- Timing: heartbeat 20 ms; I/O card slot timeout 100 ms; controller referee/peer timeout 100 ms; election window 1500 ms; chaser step 200 ms; failover goal under 200 ms.
- Arbitration: prefer a present slot that reports ACTIVE (A if both), else A, else B; failover to the other present slot immediately; never move back on its own; outputs only from the Active slot; no Active means all outputs off.
- Controllers run the same image; slot identity comes from the I/O card.
- `common/`, `io-card/src/arbiter.*`, `central/src/role.*` include only C standard headers plus `rc/proto.h`.
- Zephyr C style: tabs, braces on every `if`, `/* */` comments, explicit comparisons. `-Wall -Wextra -Werror` on host and Pi builds.
- No customer names, part numbers, or contract details anywhere.
- Commits: conventional commits, ending with `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. Do not push until the owner says so.

## Design refinements made while planning (owner to confirm)

1. **Election window starts when the first controller appears**, not when the I/O card boots. The STM32 boots in milliseconds while the Pis take tens of seconds, so a window tied to the STM32's boot would expire before either Pi appears and the first Pi up would win. Starting the window at first appearance keeps simultaneous boot deterministic (slot A) and still covers an I/O card reboot under a running system.
2. **STATUS during the election grants role 0 (unknown).** A controller that receives "unknown" keeps its current role, so an I/O card reboot does not make the Active step down and back up.
3. **Cross-link uses IPv6 link-local multicast** (`ff02::1` on `eth0`, UDP port 47000) instead of 10.0.0.x addresses. It needs no address configuration at all, so both images stay identical. Each controller ignores PEER messages carrying its own slot.
4. **I/O card indication:** all LEDs off while no slot is Active; every Active change is logged on the console with timestamps (used for the failover measurement).

## Review Focus

1. Noise bytes on a UART (floating lines, a Pi booting) must never produce a frame or block later valid frames, including a stray 0xA5 inside noise. Test in Task 2.
2. A controller must ignore its own PEER message (multicast can loop back). Test in Task 6.
3. A HEARTBEAT on a slot index other than A or B must be ignored by the arbiter. Test in Task 4.
4. Both controllers reporting ACTIVE when the I/O card elects (for example after a double fault) must still give exactly one Active, slot A. Test in Task 4.
5. The chaser step counter wraps from 65535 to 0 without a visible jump. Test in Task 6.

## Commands

- Host tests (Mac, from repo root): `cmake -S tests/host -B build/host && cmake --build build/host && ctest --test-dir build/host --output-on-failure`
- I/O card build (Mac): `cd ~/ws/stm32f3-ws && source ~/ws/zp/zephyr-venv/bin/activate && west build -p always -b stm32f3_disco@E -d build/rc-io-card ~/ws/redundant-controller/io-card`
- I/O card flash (via the Air): `tools/flash-remote.sh ~/ws/stm32f3-ws/build/rc-io-card/zephyr/zephyr.hex`
- I/O card console (via the Air): `tools/console-remote.sh 10`
- Controllers: `ssh -i ~/.ssh/id_ed25519_alpental_pi alpental@192.168.45.50` (rc-a), `...@192.168.45.176` (rc-b)

## File map

| File | Responsibility |
|---|---|
| `tests/host/CMakeLists.txt`, `tests/host/check.h` | Host test build and check macros |
| `common/include/rc/proto.h`, `common/proto.c` | CRC, frame encode, streaming parser, message pack/unpack |
| `io-card/src/arbiter.h`, `io-card/src/arbiter.c` | Referee decisions (pure) |
| `io-card/src/leds.h`, `io-card/src/leds.c` | 8-LED ring as a bit mask |
| `io-card/src/main.c` | UART interrupts, frames to arbiter, STATUS replies, outputs, watchdog, logs |
| `io-card/CMakeLists.txt`, `io-card/prj.conf` | Zephyr app build |
| `west.yml` | Zephyr v4.4.2 manifest for standalone builds |
| `central/src/role.h`, `central/src/role.c` | Controller role, fault check, chaser (pure) |
| `central/src/main.c` | Serial, UDP cross-link, main loop, event log |
| `central/CMakeLists.txt`, `central/rc-central.service` | Pi build and systemd unit |
| `tools/flash-remote.sh`, `tools/console-remote.sh`, `tools/deploy-central.sh` | Bench helpers |
| `docs/test-log.md` | Experiment results |
| `README.md`, `.github/workflows/ci.yml` | Overview, CI |

---

### Task 1: Host test harness and CRC-16

**Files:**
- Create: `tests/host/CMakeLists.txt`, `tests/host/check.h`, `tests/host/test_proto.c`, `common/include/rc/proto.h`, `common/proto.c`

**Interfaces:**
- Produces: `uint16_t rc_crc16(const uint8_t *data, size_t len)`; constants `RC_SYNC`, `RC_MAX_PAYLOAD`, `RC_FRAME_OVERHEAD`, `RC_FRAME_MAX`, `RC_MSG_*`, `RC_ROLE_*`, `RC_SLOT_*`; `check.h` macros `CHECK(cond)` and `CHECK_DONE()`.

- [ ] **Step 1: Create `tests/host/check.h`**

```c
#ifndef CHECK_H_
#define CHECK_H_

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

#define CHECK_DONE()                                                           \
	((failures == 0) ? (printf("all checks passed\n"), 0)                 \
			 : (printf("%d check(s) failed\n", failures), 1))

#endif /* CHECK_H_ */
```

- [ ] **Step 2: Create `tests/host/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(rc_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(test_proto test_proto.c ${REPO}/common/proto.c)
target_include_directories(test_proto PRIVATE ${REPO}/common/include)
add_test(NAME proto COMMAND test_proto)
```

- [ ] **Step 3: Write the failing test `tests/host/test_proto.c`**

```c
#include <string.h>

#include "check.h"
#include "rc/proto.h"

static void test_crc16_check_value(void)
{
	const char *s = "123456789";

	CHECK(rc_crc16((const uint8_t *)s, strlen(s)) == 0x29B1U);
}

static void test_crc16_empty(void)
{
	CHECK(rc_crc16(NULL, 0U) == 0xFFFFU);
}

int main(void)
{
	test_crc16_check_value();
	test_crc16_empty();
	return CHECK_DONE();
}
```

- [ ] **Step 4: Run to see it fail**

Run the host test command. Expected: build fails, `rc/proto.h` not found.

- [ ] **Step 5: Create `common/include/rc/proto.h`**

```c
#ifndef RC_PROTO_H_
#define RC_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RC_SYNC 0xA5U
#define RC_MAX_PAYLOAD 16U
/* sync, type, len, crc (2) */
#define RC_FRAME_OVERHEAD 5U
#define RC_FRAME_MAX (RC_FRAME_OVERHEAD + RC_MAX_PAYLOAD)

#define RC_MSG_HEARTBEAT 1U
#define RC_MSG_STATUS 2U
#define RC_MSG_SET_OUTPUTS 3U
#define RC_MSG_PEER 4U

#define RC_ROLE_UNKNOWN 0U
#define RC_ROLE_STANDBY 1U
#define RC_ROLE_ACTIVE 2U

#define RC_SLOT_A 0U
#define RC_SLOT_B 1U
#define RC_SLOT_NONE 0xFFU

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection. */
uint16_t rc_crc16(const uint8_t *data, size_t len);

#endif /* RC_PROTO_H_ */
```

- [ ] **Step 6: Create `common/proto.c`**

```c
#include "rc/proto.h"

static uint16_t crc_update(uint16_t crc, uint8_t byte)
{
	crc ^= (uint16_t)((uint16_t)byte << 8);
	for (int bit = 0; bit < 8; bit++) {
		if ((crc & 0x8000U) != 0U) {
			crc = (uint16_t)((crc << 1) ^ 0x1021U);
		} else {
			crc = (uint16_t)(crc << 1);
		}
	}
	return crc;
}

uint16_t rc_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc = crc_update(crc, data[i]);
	}
	return crc;
}
```

- [ ] **Step 7: Run to see it pass**

Expected: `all checks passed`, `100% tests passed`, no warnings.

- [ ] **Step 8: Commit**

```bash
git add tests/host common
git commit -m "feat: add CRC-16 for the controller protocol" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Frame encoder and streaming parser

**Files:**
- Modify: `common/include/rc/proto.h`, `common/proto.c`, `tests/host/test_proto.c`

**Interfaces:**
- Consumes: `rc_crc16`, constants from Task 1.
- Produces: `struct rc_frame { uint8_t type; uint8_t len; uint8_t payload[RC_MAX_PAYLOAD]; }`; `size_t rc_frame_encode(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out, size_t out_size)` (returns frame length, 0 on error); `struct rc_parser` with counters `crc_errors`, `len_errors`; `void rc_parser_init(struct rc_parser *p)`; `bool rc_parser_feed(struct rc_parser *p, uint8_t byte, struct rc_frame *out)` (true when `*out` holds a valid frame).

- [ ] **Step 1: Add failing tests** to `tests/host/test_proto.c` (above `main`):

```c
static size_t feed_all(struct rc_parser *p, const uint8_t *buf, size_t n,
		       struct rc_frame *frames, size_t max_frames)
{
	size_t count = 0U;

	for (size_t i = 0; i < n; i++) {
		struct rc_frame f;

		if (rc_parser_feed(p, buf[i], &f) && count < max_frames) {
			frames[count++] = f;
		}
	}
	return count;
}

static void test_encode_layout(void)
{
	const uint8_t payload[2] = {0x11U, 0x22U};
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 2U, out, sizeof(out));
	uint16_t crc = rc_crc16(&out[1], 4U);

	CHECK(n == 7U);
	CHECK(out[0] == RC_SYNC);
	CHECK(out[1] == RC_MSG_SET_OUTPUTS);
	CHECK(out[2] == 2U);
	CHECK(out[3] == 0x11U && out[4] == 0x22U);
	CHECK(out[5] == (uint8_t)(crc & 0xFFU));
	CHECK(out[6] == (uint8_t)(crc >> 8));
}

static void test_encode_rejects_bad_input(void)
{
	uint8_t payload[RC_MAX_PAYLOAD + 1U] = {0};
	uint8_t out[RC_FRAME_MAX + 1U];

	CHECK(rc_frame_encode(1U, payload, RC_MAX_PAYLOAD + 1U, out, sizeof(out)) == 0U);
	CHECK(rc_frame_encode(1U, payload, 4U, out, 8U) == 0U);
	CHECK(rc_frame_encode(1U, NULL, 1U, out, sizeof(out)) == 0U);
}

static void test_parser_roundtrip(void)
{
	const uint8_t payload[3] = {1U, 2U, 3U};
	uint8_t buf[RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_HEARTBEAT, payload, 3U, buf, sizeof(buf));
	struct rc_parser p;
	struct rc_frame frames[2];

	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 2U) == 1U);
	CHECK(frames[0].type == RC_MSG_HEARTBEAT);
	CHECK(frames[0].len == 3U);
	CHECK(memcmp(frames[0].payload, payload, 3U) == 0);
}

static void test_parser_empty_payload(void)
{
	uint8_t buf[RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_STATUS, NULL, 0U, buf, sizeof(buf));
	struct rc_parser p;
	struct rc_frame frames[1];

	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 1U) == 1U);
	CHECK(frames[0].len == 0U);
}

static void test_parser_noise_then_frame(void)
{
	/* Noise with a stray sync byte whose "length" (0x40) is too big, then a valid frame. */
	uint8_t buf[64] = {0x00U, 0xFFU, 0x13U, RC_SYNC, 0x07U, 0x40U, 0x99U, 0x3CU};
	const uint8_t payload[1] = {0x5AU};
	size_t n = 8U + rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 1U, &buf[8], sizeof(buf) - 8U);
	struct rc_parser p;
	struct rc_frame frames[2];

	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 2U) == 1U);
	CHECK(p.len_errors == 1U);
	CHECK(frames[0].type == RC_MSG_SET_OUTPUTS);
	CHECK(frames[0].payload[0] == 0x5AU);
}

static void test_parser_bad_crc_counted_and_recovers(void)
{
	const uint8_t payload[1] = {0x01U};
	uint8_t buf[2 * RC_FRAME_MAX];
	size_t n1 = rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 1U, buf, sizeof(buf));
	size_t n2;
	struct rc_parser p;
	struct rc_frame frames[2];

	buf[3] ^= 0xFFU; /* corrupt the payload of the first frame */
	n2 = rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 1U, &buf[n1], sizeof(buf) - n1);
	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n1 + n2, frames, 2U) == 1U);
	CHECK(p.crc_errors == 1U);
	CHECK(frames[0].payload[0] == 0x01U);
}

static void test_parser_rejects_oversize_len(void)
{
	const uint8_t bad[3] = {RC_SYNC, RC_MSG_HEARTBEAT, RC_MAX_PAYLOAD + 1U};
	struct rc_parser p;
	struct rc_frame frames[1];

	rc_parser_init(&p);
	CHECK(feed_all(&p, bad, 3U, frames, 1U) == 0U);
	CHECK(p.len_errors == 1U);
}

static void test_parser_back_to_back(void)
{
	const uint8_t a[1] = {0xAAU};
	const uint8_t b[1] = {0xBBU};
	uint8_t buf[2 * RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_SET_OUTPUTS, a, 1U, buf, sizeof(buf));
	struct rc_parser p;
	struct rc_frame frames[2];

	n += rc_frame_encode(RC_MSG_SET_OUTPUTS, b, 1U, &buf[n], sizeof(buf) - n);
	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 2U) == 2U);
	CHECK(frames[0].payload[0] == 0xAAU && frames[1].payload[0] == 0xBBU);
}
```

Add to `main()` before `return`:

```c
	test_encode_layout();
	test_encode_rejects_bad_input();
	test_parser_roundtrip();
	test_parser_empty_payload();
	test_parser_noise_then_frame();
	test_parser_bad_crc_counted_and_recovers();
	test_parser_rejects_oversize_len();
	test_parser_back_to_back();
```

- [ ] **Step 2: Run to see it fail** — build fails on undeclared `rc_frame_encode` / `struct rc_parser`.

- [ ] **Step 3: Add to `proto.h`** before `#endif`:

```c
struct rc_frame {
	uint8_t type;
	uint8_t len;
	uint8_t payload[RC_MAX_PAYLOAD];
};

/* Returns the frame length, or 0 if the payload or buffer is invalid. */
size_t rc_frame_encode(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out,
		       size_t out_size);

struct rc_parser {
	uint8_t state;
	uint8_t type;
	uint8_t len;
	uint8_t idx;
	uint8_t crc_lo;
	uint16_t crc;
	uint8_t payload[RC_MAX_PAYLOAD];
	uint32_t crc_errors;
	uint32_t len_errors;
};

void rc_parser_init(struct rc_parser *p);

/* Feeds one byte; returns true when *out holds a complete, valid frame. */
bool rc_parser_feed(struct rc_parser *p, uint8_t byte, struct rc_frame *out);
```

- [ ] **Step 4: Add to `proto.c`**: add `#include <string.h>` after the first include, then append:

```c
size_t rc_frame_encode(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out,
		       size_t out_size)
{
	size_t total = RC_FRAME_OVERHEAD + (size_t)len;
	uint16_t crc;

	if ((len > RC_MAX_PAYLOAD) || (out_size < total) || ((len > 0U) && (payload == NULL))) {
		return 0U;
	}
	out[0] = RC_SYNC;
	out[1] = type;
	out[2] = len;
	if (len > 0U) {
		memcpy(&out[3], payload, len);
	}
	crc = rc_crc16(&out[1], (size_t)len + 2U);
	out[3U + len] = (uint8_t)(crc & 0xFFU);
	out[4U + len] = (uint8_t)(crc >> 8);
	return total;
}

enum {
	ST_SYNC,
	ST_TYPE,
	ST_LEN,
	ST_PAYLOAD,
	ST_CRC_LO,
	ST_CRC_HI,
};

void rc_parser_init(struct rc_parser *p)
{
	memset(p, 0, sizeof(*p));
	p->state = ST_SYNC;
}

bool rc_parser_feed(struct rc_parser *p, uint8_t byte, struct rc_frame *out)
{
	switch (p->state) {
	case ST_SYNC:
		if (byte == RC_SYNC) {
			p->crc = 0xFFFFU;
			p->state = ST_TYPE;
		}
		return false;
	case ST_TYPE:
		p->type = byte;
		p->crc = crc_update(p->crc, byte);
		p->state = ST_LEN;
		return false;
	case ST_LEN:
		if (byte > RC_MAX_PAYLOAD) {
			p->len_errors++;
			p->state = ST_SYNC;
			return false;
		}
		p->len = byte;
		p->idx = 0U;
		p->crc = crc_update(p->crc, byte);
		p->state = (byte == 0U) ? ST_CRC_LO : ST_PAYLOAD;
		return false;
	case ST_PAYLOAD:
		p->payload[p->idx++] = byte;
		p->crc = crc_update(p->crc, byte);
		if (p->idx == p->len) {
			p->state = ST_CRC_LO;
		}
		return false;
	case ST_CRC_LO:
		p->crc_lo = byte;
		p->state = ST_CRC_HI;
		return false;
	case ST_CRC_HI:
		p->state = ST_SYNC;
		if ((uint16_t)(p->crc_lo | ((uint16_t)byte << 8)) != p->crc) {
			p->crc_errors++;
			return false;
		}
		out->type = p->type;
		out->len = p->len;
		memcpy(out->payload, p->payload, p->len);
		return true;
	default:
		p->state = ST_SYNC;
		return false;
	}
}
```

- [ ] **Step 5: Run to see it pass.** Expected: `all checks passed`, no warnings.

- [ ] **Step 6: Commit**

```bash
git add common tests/host
git commit -m "feat: add frame encoder and streaming parser" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Message pack and unpack

**Files:**
- Modify: `common/include/rc/proto.h`, `common/proto.c`, `tests/host/test_proto.c`

**Interfaces:**
- Consumes: `rc_frame_encode`, `struct rc_frame`.
- Produces: structs `rc_heartbeat {uint32_t seq; uint8_t role;}`, `rc_status {uint32_t seq; uint8_t slot; uint8_t granted_role; uint8_t active_slot;}`, `rc_set_outputs {uint8_t mask;}`, `rc_peer {uint32_t seq; uint8_t slot; uint8_t role; uint8_t referee_ok; uint16_t step;}`; `size_t rc_encode_<msg>(const struct rc_<msg> *m, uint8_t *out, size_t out_size)`; `int rc_decode_<msg>(const struct rc_frame *f, struct rc_<msg> *m)` (0 ok, -1 wrong type or length) for msg in heartbeat, status, set_outputs, peer.

- [ ] **Step 1: Add failing tests** to `test_proto.c`:

```c
static bool decode_one(const uint8_t *buf, size_t n, struct rc_frame *f)
{
	struct rc_parser p;

	rc_parser_init(&p);
	return feed_all(&p, buf, n, f, 1U) == 1U;
}

static void test_heartbeat_roundtrip(void)
{
	struct rc_heartbeat in = {.seq = 0x01020304U, .role = RC_ROLE_ACTIVE};
	struct rc_heartbeat out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_heartbeat(&in, buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD + 5U);
	CHECK(buf[3] == 0x04U); /* little-endian seq */
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_heartbeat(&f, &out) == 0);
	CHECK(out.seq == in.seq && out.role == in.role);
}

static void test_status_roundtrip(void)
{
	struct rc_status in = {.seq = 7U, .slot = RC_SLOT_B, .granted_role = RC_ROLE_STANDBY,
			       .active_slot = RC_SLOT_A};
	struct rc_status out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_status(&in, buf, sizeof(buf));

	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_status(&f, &out) == 0);
	CHECK(out.seq == 7U && out.slot == RC_SLOT_B);
	CHECK(out.granted_role == RC_ROLE_STANDBY && out.active_slot == RC_SLOT_A);
}

static void test_set_outputs_roundtrip(void)
{
	struct rc_set_outputs in = {.mask = 0x81U};
	struct rc_set_outputs out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_set_outputs(&in, buf, sizeof(buf));

	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_set_outputs(&f, &out) == 0);
	CHECK(out.mask == 0x81U);
}

static void test_peer_roundtrip(void)
{
	struct rc_peer in = {.seq = 0xFFFFFFFFU, .slot = RC_SLOT_A, .role = RC_ROLE_ACTIVE,
			     .referee_ok = 1U, .step = 0xBEEFU};
	struct rc_peer out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_peer(&in, buf, sizeof(buf));

	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_peer(&f, &out) == 0);
	CHECK(out.seq == in.seq && out.slot == in.slot && out.role == in.role);
	CHECK(out.referee_ok == 1U && out.step == 0xBEEFU);
}

static void test_decode_rejects_wrong_type_or_len(void)
{
	struct rc_frame f = {.type = RC_MSG_STATUS, .len = 5U};
	struct rc_heartbeat hb;
	struct rc_status st;

	CHECK(rc_decode_heartbeat(&f, &hb) == -1);
	CHECK(rc_decode_status(&f, &st) == -1);
}
```

Add to `main()`:

```c
	test_heartbeat_roundtrip();
	test_status_roundtrip();
	test_set_outputs_roundtrip();
	test_peer_roundtrip();
	test_decode_rejects_wrong_type_or_len();
```

- [ ] **Step 2: Run to see it fail** — undeclared `rc_encode_heartbeat` and structs.

- [ ] **Step 3: Add to `proto.h`** before `#endif`:

```c
struct rc_heartbeat {
	uint32_t seq;
	uint8_t role;
};

struct rc_status {
	uint32_t seq;
	uint8_t slot;
	uint8_t granted_role;
	uint8_t active_slot;
};

struct rc_set_outputs {
	uint8_t mask;
};

struct rc_peer {
	uint32_t seq;
	uint8_t slot;
	uint8_t role;
	uint8_t referee_ok;
	uint16_t step;
};

size_t rc_encode_heartbeat(const struct rc_heartbeat *m, uint8_t *out, size_t out_size);
size_t rc_encode_status(const struct rc_status *m, uint8_t *out, size_t out_size);
size_t rc_encode_set_outputs(const struct rc_set_outputs *m, uint8_t *out, size_t out_size);
size_t rc_encode_peer(const struct rc_peer *m, uint8_t *out, size_t out_size);

/* Return 0 on success, -1 if the frame type or length does not match. */
int rc_decode_heartbeat(const struct rc_frame *f, struct rc_heartbeat *m);
int rc_decode_status(const struct rc_frame *f, struct rc_status *m);
int rc_decode_set_outputs(const struct rc_frame *f, struct rc_set_outputs *m);
int rc_decode_peer(const struct rc_frame *f, struct rc_peer *m);
```

- [ ] **Step 4: Append to `proto.c`**:

```c
static void put_u16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xFFU);
	b[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v & 0xFFU);
	b[1] = (uint8_t)((v >> 8) & 0xFFU);
	b[2] = (uint8_t)((v >> 16) & 0xFFU);
	b[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *b)
{
	return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t get_u32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

size_t rc_encode_heartbeat(const struct rc_heartbeat *m, uint8_t *out, size_t out_size)
{
	uint8_t p[5];

	put_u32(p, m->seq);
	p[4] = m->role;
	return rc_frame_encode(RC_MSG_HEARTBEAT, p, sizeof(p), out, out_size);
}

size_t rc_encode_status(const struct rc_status *m, uint8_t *out, size_t out_size)
{
	uint8_t p[7];

	put_u32(p, m->seq);
	p[4] = m->slot;
	p[5] = m->granted_role;
	p[6] = m->active_slot;
	return rc_frame_encode(RC_MSG_STATUS, p, sizeof(p), out, out_size);
}

size_t rc_encode_set_outputs(const struct rc_set_outputs *m, uint8_t *out, size_t out_size)
{
	return rc_frame_encode(RC_MSG_SET_OUTPUTS, &m->mask, 1U, out, out_size);
}

size_t rc_encode_peer(const struct rc_peer *m, uint8_t *out, size_t out_size)
{
	uint8_t p[9];

	put_u32(p, m->seq);
	p[4] = m->slot;
	p[5] = m->role;
	p[6] = m->referee_ok;
	put_u16(&p[7], m->step);
	return rc_frame_encode(RC_MSG_PEER, p, sizeof(p), out, out_size);
}

int rc_decode_heartbeat(const struct rc_frame *f, struct rc_heartbeat *m)
{
	if ((f->type != RC_MSG_HEARTBEAT) || (f->len != 5U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->role = f->payload[4];
	return 0;
}

int rc_decode_status(const struct rc_frame *f, struct rc_status *m)
{
	if ((f->type != RC_MSG_STATUS) || (f->len != 7U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->slot = f->payload[4];
	m->granted_role = f->payload[5];
	m->active_slot = f->payload[6];
	return 0;
}

int rc_decode_set_outputs(const struct rc_frame *f, struct rc_set_outputs *m)
{
	if ((f->type != RC_MSG_SET_OUTPUTS) || (f->len != 1U)) {
		return -1;
	}
	m->mask = f->payload[0];
	return 0;
}

int rc_decode_peer(const struct rc_frame *f, struct rc_peer *m)
{
	if ((f->type != RC_MSG_PEER) || (f->len != 9U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->slot = f->payload[4];
	m->role = f->payload[5];
	m->referee_ok = f->payload[6];
	m->step = get_u16(&f->payload[7]);
	return 0;
}
```

- [ ] **Step 5: Run to see it pass.**

- [ ] **Step 6: Commit**

```bash
git add common tests/host
git commit -m "feat: add protocol messages" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Referee arbitration logic

**Files:**
- Create: `io-card/src/arbiter.h`, `io-card/src/arbiter.c`, `tests/host/test_arbiter.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `RC_SLOT_*`, `RC_ROLE_*` from `rc/proto.h`.
- Produces: `struct arbiter`; `void arb_init(struct arbiter *a)`; `void arb_on_heartbeat(struct arbiter *a, uint8_t slot, uint8_t reported_role, int64_t now_ms)`; `bool arb_tick(struct arbiter *a, int64_t now_ms)` (true when the Active slot changed); `bool arb_present(const struct arbiter *a, uint8_t slot, int64_t now_ms)`; `uint8_t arb_active(const struct arbiter *a)`; `uint8_t arb_granted_role(const struct arbiter *a, uint8_t slot)`; `bool arb_accepts_outputs(const struct arbiter *a, uint8_t slot)`; `int64_t arb_last_rx_ms(const struct arbiter *a, uint8_t slot)`; constants `ARB_HB_TIMEOUT_MS` (100), `ARB_ELECTION_WINDOW_MS` (1500).

- [ ] **Step 1: Add the test target** to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_arbiter test_arbiter.c ${REPO}/io-card/src/arbiter.c)
target_include_directories(test_arbiter PRIVATE ${REPO}/common/include ${REPO}/io-card/src)
add_test(NAME arbiter COMMAND test_arbiter)
```

- [ ] **Step 2: Write the failing test `tests/host/test_arbiter.c`**

```c
#include "arbiter.h"
#include "check.h"

/* Advance time in 20 ms heartbeat periods from `from` to `to` (exclusive). */
static void run(struct arbiter *a, int64_t from, int64_t to, bool send_a, uint8_t role_a,
		bool send_b, uint8_t role_b)
{
	for (int64_t t = from; t < to; t += 20) {
		if (send_a) {
			arb_on_heartbeat(a, RC_SLOT_A, role_a, t);
		}
		if (send_b) {
			arb_on_heartbeat(a, RC_SLOT_B, role_b, t);
		}
		(void)arb_tick(a, t);
	}
}

static void test_nobody_present(void)
{
	struct arbiter a;

	arb_init(&a);
	(void)arb_tick(&a, 5000);
	CHECK(arb_active(&a) == RC_SLOT_NONE);
	CHECK(arb_granted_role(&a, RC_SLOT_A) == RC_ROLE_UNKNOWN);
	CHECK(!arb_accepts_outputs(&a, RC_SLOT_A));
}

static void test_simultaneous_boot_elects_a(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 10000, 11480, true, RC_ROLE_UNKNOWN, true, RC_ROLE_UNKNOWN);
	CHECK(arb_active(&a) == RC_SLOT_NONE); /* still inside the window */
	CHECK(arb_granted_role(&a, RC_SLOT_A) == RC_ROLE_UNKNOWN);
	run(&a, 11480, 11600, true, RC_ROLE_UNKNOWN, true, RC_ROLE_UNKNOWN);
	CHECK(arb_active(&a) == RC_SLOT_A);
	CHECK(arb_granted_role(&a, RC_SLOT_A) == RC_ROLE_ACTIVE);
	CHECK(arb_granted_role(&a, RC_SLOT_B) == RC_ROLE_STANDBY);
	CHECK(arb_accepts_outputs(&a, RC_SLOT_A));
	CHECK(!arb_accepts_outputs(&a, RC_SLOT_B));
}

static void test_b_first_a_within_window_elects_a(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 1000, false, 0U, true, RC_ROLE_UNKNOWN);
	run(&a, 1000, 2000, true, RC_ROLE_UNKNOWN, true, RC_ROLE_UNKNOWN);
	CHECK(arb_active(&a) == RC_SLOT_A);
}

static void test_only_b_present(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 2000, false, 0U, true, RC_ROLE_UNKNOWN);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_referee_reboot_keeps_reported_active(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 2000, true, RC_ROLE_STANDBY, true, RC_ROLE_ACTIVE);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_both_claim_active_elects_a(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 2000, true, RC_ROLE_ACTIVE, true, RC_ROLE_ACTIVE);
	CHECK(arb_active(&a) == RC_SLOT_A);
}

static void test_failover_timing(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 2000, true, RC_ROLE_UNKNOWN, true, RC_ROLE_UNKNOWN);
	CHECK(arb_active(&a) == RC_SLOT_A);
	/* Last A heartbeat at 1980 ms; B keeps sending. */
	arb_on_heartbeat(&a, RC_SLOT_B, RC_ROLE_STANDBY, 2080);
	CHECK(!arb_tick(&a, 2080)); /* 100 ms after A's last frame: still present */
	CHECK(arb_active(&a) == RC_SLOT_A);
	CHECK(arb_tick(&a, 2081)); /* 101 ms: lost, B takes over immediately */
	CHECK(arb_active(&a) == RC_SLOT_B);
	CHECK(arb_last_rx_ms(&a, RC_SLOT_A) == 1980);
}

static void test_no_fallback_when_a_returns(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 2000, true, RC_ROLE_UNKNOWN, true, RC_ROLE_UNKNOWN);
	run(&a, 2000, 2300, false, 0U, true, RC_ROLE_STANDBY);
	CHECK(arb_active(&a) == RC_SLOT_B);
	run(&a, 2300, 5000, true, RC_ROLE_ACTIVE, true, RC_ROLE_ACTIVE);
	CHECK(arb_active(&a) == RC_SLOT_B);
	CHECK(arb_granted_role(&a, RC_SLOT_A) == RC_ROLE_STANDBY);
}

static void test_both_lost_then_reelect(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 2000, true, RC_ROLE_UNKNOWN, false, 0U);
	CHECK(arb_active(&a) == RC_SLOT_A);
	(void)arb_tick(&a, 2500);
	CHECK(arb_active(&a) == RC_SLOT_NONE);
	CHECK(!arb_accepts_outputs(&a, RC_SLOT_A));
	run(&a, 3000, 4600, false, 0U, true, RC_ROLE_UNKNOWN);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_invalid_slot_ignored(void)
{
	struct arbiter a;

	arb_init(&a);
	for (int64_t t = 0; t < 2000; t += 20) {
		arb_on_heartbeat(&a, 2U, RC_ROLE_ACTIVE, t);
		arb_on_heartbeat(&a, RC_SLOT_NONE, RC_ROLE_ACTIVE, t);
		(void)arb_tick(&a, t);
	}
	CHECK(arb_active(&a) == RC_SLOT_NONE);
	CHECK(arb_granted_role(&a, 2U) == RC_ROLE_UNKNOWN);
	CHECK(!arb_accepts_outputs(&a, 2U));
}

int main(void)
{
	test_nobody_present();
	test_simultaneous_boot_elects_a();
	test_b_first_a_within_window_elects_a();
	test_only_b_present();
	test_referee_reboot_keeps_reported_active();
	test_both_claim_active_elects_a();
	test_failover_timing();
	test_no_fallback_when_a_returns();
	test_both_lost_then_reelect();
	test_invalid_slot_ignored();
	return CHECK_DONE();
}
```

- [ ] **Step 3: Run to see it fail** — `arbiter.h` not found.

- [ ] **Step 4: Create `io-card/src/arbiter.h`**

```c
#ifndef ARBITER_H_
#define ARBITER_H_

#include <stdbool.h>
#include <stdint.h>

#include "rc/proto.h"

#define ARB_HB_TIMEOUT_MS 100
#define ARB_ELECTION_WINDOW_MS 1500

struct arb_slot {
	bool heard;
	int64_t last_rx_ms;
	uint8_t reported_role;
};

struct arbiter {
	struct arb_slot slot[2];
	uint8_t active;
	bool electing;
	int64_t election_start_ms;
};

void arb_init(struct arbiter *a);
void arb_on_heartbeat(struct arbiter *a, uint8_t slot, uint8_t reported_role, int64_t now_ms);

/* Updates presence and the Active slot; returns true when the Active slot changed. */
bool arb_tick(struct arbiter *a, int64_t now_ms);

bool arb_present(const struct arbiter *a, uint8_t slot, int64_t now_ms);
uint8_t arb_active(const struct arbiter *a);

/* RC_ROLE_UNKNOWN while no slot is Active (election pending or nobody present). */
uint8_t arb_granted_role(const struct arbiter *a, uint8_t slot);

bool arb_accepts_outputs(const struct arbiter *a, uint8_t slot);
int64_t arb_last_rx_ms(const struct arbiter *a, uint8_t slot);

#endif /* ARBITER_H_ */
```

- [ ] **Step 5: Create `io-card/src/arbiter.c`**

```c
#include <string.h>

#include "arbiter.h"

static bool valid_slot(uint8_t slot)
{
	return (slot == RC_SLOT_A) || (slot == RC_SLOT_B);
}

static uint8_t other(uint8_t slot)
{
	return (slot == RC_SLOT_A) ? RC_SLOT_B : RC_SLOT_A;
}

void arb_init(struct arbiter *a)
{
	memset(a, 0, sizeof(*a));
	a->active = RC_SLOT_NONE;
}

void arb_on_heartbeat(struct arbiter *a, uint8_t slot, uint8_t reported_role, int64_t now_ms)
{
	if (!valid_slot(slot)) {
		return;
	}
	a->slot[slot].heard = true;
	a->slot[slot].last_rx_ms = now_ms;
	a->slot[slot].reported_role = reported_role;
}

bool arb_present(const struct arbiter *a, uint8_t slot, int64_t now_ms)
{
	if (!valid_slot(slot)) {
		return false;
	}
	return a->slot[slot].heard && ((now_ms - a->slot[slot].last_rx_ms) <= ARB_HB_TIMEOUT_MS);
}

static uint8_t elect(const struct arbiter *a, int64_t now_ms)
{
	bool pa = arb_present(a, RC_SLOT_A, now_ms);
	bool pb = arb_present(a, RC_SLOT_B, now_ms);

	if (pa && (a->slot[RC_SLOT_A].reported_role == RC_ROLE_ACTIVE)) {
		return RC_SLOT_A;
	}
	if (pb && (a->slot[RC_SLOT_B].reported_role == RC_ROLE_ACTIVE)) {
		return RC_SLOT_B;
	}
	if (pa) {
		return RC_SLOT_A;
	}
	if (pb) {
		return RC_SLOT_B;
	}
	return RC_SLOT_NONE;
}

bool arb_tick(struct arbiter *a, int64_t now_ms)
{
	uint8_t before = a->active;

	if (a->active != RC_SLOT_NONE) {
		if (!arb_present(a, a->active, now_ms)) {
			uint8_t o = other(a->active);

			a->active = arb_present(a, o, now_ms) ? o : RC_SLOT_NONE;
		}
	} else if (!a->electing) {
		if (arb_present(a, RC_SLOT_A, now_ms) || arb_present(a, RC_SLOT_B, now_ms)) {
			a->electing = true;
			a->election_start_ms = now_ms;
		}
	} else if ((now_ms - a->election_start_ms) >= ARB_ELECTION_WINDOW_MS) {
		a->electing = false;
		a->active = elect(a, now_ms);
	}
	return a->active != before;
}

uint8_t arb_active(const struct arbiter *a)
{
	return a->active;
}

uint8_t arb_granted_role(const struct arbiter *a, uint8_t slot)
{
	if (!valid_slot(slot) || (a->active == RC_SLOT_NONE)) {
		return RC_ROLE_UNKNOWN;
	}
	return (slot == a->active) ? RC_ROLE_ACTIVE : RC_ROLE_STANDBY;
}

bool arb_accepts_outputs(const struct arbiter *a, uint8_t slot)
{
	return valid_slot(slot) && (slot == a->active);
}

int64_t arb_last_rx_ms(const struct arbiter *a, uint8_t slot)
{
	return valid_slot(slot) ? a->slot[slot].last_rx_ms : -1;
}
```

- [ ] **Step 6: Run to see it pass.** Expected: both test executables report `all checks passed`.

- [ ] **Step 7: Commit**

```bash
git add io-card/src/arbiter.h io-card/src/arbiter.c tests/host
git commit -m "feat: add referee arbitration logic" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: I/O card firmware

**Files:**
- Create: `west.yml`, `io-card/CMakeLists.txt`, `io-card/prj.conf`, `io-card/src/leds.h`, `io-card/src/leds.c`, `io-card/src/main.c`, `tools/flash-remote.sh`, `tools/console-remote.sh`

**Interfaces:**
- Consumes: `rc/proto.h` (parser, messages), `arbiter.h`.
- Produces: firmware that answers every HEARTBEAT with a STATUS on the same UART, applies SET_OUTPUTS from the Active slot only, turns all LEDs off when no slot is Active, feeds the IWDG, and logs `active: X -> Y` lines with `gap_ms` (time from the lost slot's last frame to the change).

- [ ] **Step 1: Create `west.yml`**

```yaml
manifest:
  self:
    path: redundant-controller

  remotes:
    - name: zephyrproject-rtos
      url-base: https://github.com/zephyrproject-rtos

  projects:
    - name: zephyr
      remote: zephyrproject-rtos
      revision: v4.4.2
      import:
        name-allowlist:
          - cmsis
          - cmsis_6
          - hal_stm32
```

- [ ] **Step 2: Create `io-card/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20.0)

# Flash with OpenOCD; the board default is STM32CubeProgrammer.
set(BOARD_FLASH_RUNNER openocd)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rc_io_card)

target_include_directories(app PRIVATE ../common/include src)
target_sources(app PRIVATE
  src/main.c
  src/arbiter.c
  src/leds.c
  ../common/proto.c
)
```

- [ ] **Step 3: Create `io-card/prj.conf`**

```
CONFIG_GPIO=y
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_RING_BUFFER=y
CONFIG_WATCHDOG=y
CONFIG_LOG=y
```

- [ ] **Step 4: Create `io-card/src/leds.h`**

```c
#ifndef LEDS_H_
#define LEDS_H_

#include <stdint.h>

int leds_init(void);

/* Bit i lights ring position i (0 = N, clockwise). */
int leds_set_mask(uint8_t mask);

#endif /* LEDS_H_ */
```

- [ ] **Step 5: Create `io-card/src/leds.c`**

```c
#include <errno.h>
#include <zephyr/drivers/gpio.h>

#include "leds.h"

static const struct gpio_dt_spec ring[8] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(red_led_3), gpios),    /* N  LD3  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(orange_led_5), gpios), /* NE LD5  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(green_led_7), gpios),  /* E  LD7  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led_9), gpios),   /* SE LD9  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(red_led_10), gpios),   /* S  LD10 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(orange_led_8), gpios), /* SW LD8  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(green_led_6), gpios),  /* W  LD6  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led_4), gpios),   /* NW LD4  */
};

int leds_init(void)
{
	for (int i = 0; i < 8; i++) {
		int err;

		if (!gpio_is_ready_dt(&ring[i])) {
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(&ring[i], GPIO_OUTPUT_INACTIVE);
		if (err != 0) {
			return err;
		}
	}
	return 0;
}

int leds_set_mask(uint8_t mask)
{
	for (int i = 0; i < 8; i++) {
		int err = gpio_pin_set_dt(&ring[i], (int)((mask >> i) & 1U));

		if (err != 0) {
			return err;
		}
	}
	return 0;
}
```

- [ ] **Step 6: Create `io-card/src/main.c`**

```c
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "arbiter.h"
#include "leds.h"
#include "rc/proto.h"

LOG_MODULE_REGISTER(io_card, LOG_LEVEL_INF);

#define WDT_TIMEOUT_MS 250U
#define RX_BUF_SIZE 256U

struct link {
	const struct device *dev;
	uint8_t slot;
	struct ring_buf rb;
	uint8_t rb_mem[RX_BUF_SIZE];
	struct rc_parser parser;
	uint32_t rx_dropped;
	uint32_t outputs_rejected;
};

static struct link links[2] = {
	{.dev = DEVICE_DT_GET(DT_NODELABEL(usart2)), .slot = RC_SLOT_A},
	{.dev = DEVICE_DT_GET(DT_NODELABEL(uart4)), .slot = RC_SLOT_B},
};

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static struct arbiter arb;
static uint8_t applied_mask;

static char slot_name(uint8_t slot)
{
	return (slot == RC_SLOT_A) ? 'A' : ((slot == RC_SLOT_B) ? 'B' : '-');
}

static void uart_isr(const struct device *dev, void *user_data)
{
	struct link *l = user_data;

	if (uart_irq_update(dev) == 0) {
		return;
	}
	while (uart_irq_rx_ready(dev) != 0) {
		uint8_t buf[16];
		int n = uart_fifo_read(dev, buf, sizeof(buf));
		uint32_t put;

		if (n <= 0) {
			break;
		}
		put = ring_buf_put(&l->rb, buf, (uint32_t)n);
		l->rx_dropped += (uint32_t)n - put;
	}
}

static void send_frame(const struct device *dev, const uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uart_poll_out(dev, buf[i]);
	}
}

static void set_outputs(uint8_t mask)
{
	int err;

	if (mask == applied_mask) {
		return;
	}
	err = leds_set_mask(mask);
	if (err != 0) {
		LOG_ERR("LED update failed: %d", err);
		return;
	}
	applied_mask = mask;
}

static void handle_frame(struct link *l, const struct rc_frame *f, int64_t now)
{
	struct rc_heartbeat hb;
	struct rc_set_outputs so;

	if (rc_decode_heartbeat(f, &hb) == 0) {
		struct rc_status st;
		uint8_t out[RC_FRAME_MAX];
		size_t n;

		arb_on_heartbeat(&arb, l->slot, hb.role, now);
		st.seq = hb.seq;
		st.slot = l->slot;
		st.granted_role = arb_granted_role(&arb, l->slot);
		st.active_slot = arb_active(&arb);
		n = rc_encode_status(&st, out, sizeof(out));
		send_frame(l->dev, out, n);
	} else if (rc_decode_set_outputs(f, &so) == 0) {
		if (arb_accepts_outputs(&arb, l->slot)) {
			set_outputs(so.mask);
		} else {
			l->outputs_rejected++;
		}
	} else {
		LOG_WRN("slot %c: unexpected frame type %u len %u", slot_name(l->slot), f->type,
			f->len);
	}
}

static int links_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		struct link *l = &links[i];
		int err;

		if (!device_is_ready(l->dev)) {
			return -ENODEV;
		}
		ring_buf_init(&l->rb, sizeof(l->rb_mem), l->rb_mem);
		rc_parser_init(&l->parser);
		err = uart_irq_callback_user_data_set(l->dev, uart_isr, l);
		if (err != 0) {
			return err;
		}
		uart_irq_rx_enable(l->dev);
	}
	return 0;
}

static int wdt_start(void)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = WDT_TIMEOUT_MS},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ch;
	int err;

	if (!device_is_ready(wdt)) {
		return -ENODEV;
	}
	ch = wdt_install_timeout(wdt, &cfg);
	if (ch < 0) {
		return ch;
	}
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err != 0) {
		return err;
	}
	return ch;
}

int main(void)
{
	int wdt_ch;
	int err;

	err = leds_init();
	if (err != 0) {
		LOG_ERR("LEDs not ready: %d", err);
		return 0;
	}
	err = links_init();
	if (err != 0) {
		LOG_ERR("UART links not ready: %d", err);
		return 0;
	}
	wdt_ch = wdt_start();
	if (wdt_ch < 0) {
		LOG_ERR("watchdog start failed: %d", wdt_ch);
		return 0;
	}
	arb_init(&arb);
	LOG_INF("io-card ready: A=USART2 PA2/PA3, B=UART4 PC10/PC11");

	for (;;) {
		int64_t now = k_uptime_get();
		uint8_t before = arb_active(&arb);

		for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
			struct link *l = &links[i];
			uint8_t byte;

			while (ring_buf_get(&l->rb, &byte, 1U) == 1U) {
				struct rc_frame f;

				if (rc_parser_feed(&l->parser, byte, &f)) {
					handle_frame(l, &f, now);
				}
			}
		}
		if (arb_tick(&arb, now)) {
			uint8_t after = arb_active(&arb);
			int64_t gap = (before != RC_SLOT_NONE) ? now - arb_last_rx_ms(&arb, before)
							       : -1;

			LOG_INF("t=%lld active: %c -> %c gap_ms=%lld", now, slot_name(before),
				slot_name(after), gap);
		}
		if (arb_active(&arb) == RC_SLOT_NONE) {
			set_outputs(0U);
		}
		err = wdt_feed(wdt, wdt_ch);
		if (err != 0) {
			LOG_ERR("watchdog feed failed: %d", err);
		}
		k_msleep(1);
	}
	return 0;
}
```

- [ ] **Step 7: Create the bench helpers** `tools/flash-remote.sh` and `tools/console-remote.sh` (make both executable):

```bash
#!/bin/bash
# Flash the STM32 attached to the bench host: flash-remote.sh <zephyr.hex>
set -euo pipefail
HOST=${RC_BENCH_HOST:-seokyoungjeong@seoks-macbook-air}
KEY=${RC_BENCH_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
scp -q -i "$KEY" "$1" "$HOST:/tmp/rc-io-card.hex"
ssh -i "$KEY" -o BatchMode=yes "$HOST" \
  '/opt/homebrew/bin/openocd -f board/stm32f3discovery.cfg -c "program /tmp/rc-io-card.hex verify reset exit" 2>&1 | grep -E "Verified|Error|error"'
```

```bash
#!/bin/bash
# Print the STM32 console from the bench host for N seconds: console-remote.sh [seconds]
set -euo pipefail
HOST=${RC_BENCH_HOST:-seokyoungjeong@seoks-macbook-air}
KEY=${RC_BENCH_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
ssh -i "$KEY" -o BatchMode=yes "$HOST" \
  "P=\$(ls /dev/cu.usbmodem* | head -1); { stty 115200 raw -echo; cat; } < \$P & c=\$!; sleep ${1:-5}; kill \$c" |
  LC_ALL=C sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
```

- [ ] **Step 8: Build.** Run the I/O card build command. Expected: no warnings.

- [ ] **Step 9: Flash and check with no controller running (owner at bench).** Run `tools/flash-remote.sh ...` then `tools/console-remote.sh 5`. Expected: `Verified OK`; console shows `io-card ready`; all LEDs off; no `active:` lines (no controller daemon is running yet).

- [ ] **Step 10: Run host tests, then commit**

```bash
git add west.yml io-card tools
git commit -m "feat: add I/O card firmware with referee and watchdog" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Controller role logic and chaser

**Files:**
- Create: `central/src/role.h`, `central/src/role.c`, `tests/host/test_role.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `struct rc_status`, `struct rc_peer`, `RC_ROLE_*`, `RC_SLOT_*`.
- Produces: `struct role_state` (fields `slot`, `role`, `referee_ok`, `peer_ok`, `peer_role`, `peer_step`, `fault`, `step`); event bits `ROLE_EV_*`; `void role_init(struct role_state *s, int64_t now_ms)`; `unsigned role_on_status(struct role_state *s, const struct rc_status *m, int64_t now_ms)`; `unsigned role_on_peer(struct role_state *s, const struct rc_peer *m, int64_t now_ms)`; `unsigned role_tick(struct role_state *s, int64_t now_ms)`; `bool role_may_drive(const struct role_state *s)`; `bool role_chaser_due(struct role_state *s, int64_t now_ms, uint8_t *mask)`.

- [ ] **Step 1: Add the test target** to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_role test_role.c ${REPO}/central/src/role.c)
target_include_directories(test_role PRIVATE ${REPO}/common/include ${REPO}/central/src)
add_test(NAME role COMMAND test_role)
```

- [ ] **Step 2: Write the failing test `tests/host/test_role.c`**

```c
#include "check.h"
#include "role.h"

static struct rc_status status(uint8_t slot, uint8_t granted)
{
	struct rc_status m = {.seq = 1U, .slot = slot, .granted_role = granted,
			      .active_slot = RC_SLOT_NONE};

	return m;
}

static struct rc_peer peer(uint8_t slot, uint8_t role, uint16_t step)
{
	struct rc_peer m = {.seq = 1U, .slot = slot, .role = role, .referee_ok = 1U,
			    .step = step};

	return m;
}

static void test_initial_state(void)
{
	struct role_state s;

	role_init(&s, 0);
	CHECK(s.slot == RC_SLOT_NONE);
	CHECK(s.role == RC_ROLE_UNKNOWN);
	CHECK(!role_may_drive(&s));
}

static void test_status_grants_active_and_learns_slot(void)
{
	struct role_state s;
	struct rc_status m = status(RC_SLOT_B, RC_ROLE_ACTIVE);
	unsigned ev;

	role_init(&s, 0);
	ev = role_on_status(&s, &m, 10);
	CHECK((ev & ROLE_EV_ROLE_CHANGED) != 0U);
	CHECK((ev & ROLE_EV_SLOT_LEARNED) != 0U);
	CHECK((ev & ROLE_EV_REFEREE_BACK) != 0U);
	CHECK(s.slot == RC_SLOT_B && s.role == RC_ROLE_ACTIVE);
	CHECK(role_may_drive(&s));
}

static void test_unknown_grant_keeps_role(void)
{
	struct role_state s;
	struct rc_status a = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	struct rc_status u = status(RC_SLOT_A, RC_ROLE_UNKNOWN);

	role_init(&s, 0);
	(void)role_on_status(&s, &a, 10);
	CHECK((role_on_status(&s, &u, 30) & ROLE_EV_ROLE_CHANGED) == 0U);
	CHECK(s.role == RC_ROLE_ACTIVE);
}

static void test_referee_lost_and_back(void)
{
	struct role_state s;
	struct rc_status m = status(RC_SLOT_A, RC_ROLE_ACTIVE);

	role_init(&s, 0);
	(void)role_on_status(&s, &m, 1000);
	CHECK((role_tick(&s, 1100) & ROLE_EV_REFEREE_LOST) == 0U);
	CHECK((role_tick(&s, 1101) & ROLE_EV_REFEREE_LOST) != 0U);
	CHECK(!role_may_drive(&s));
	CHECK((role_on_status(&s, &m, 1500) & ROLE_EV_REFEREE_BACK) != 0U);
	CHECK(role_may_drive(&s));
}

static void test_chaser_steps_only_when_driving(void)
{
	struct role_state s;
	struct rc_status sb = status(RC_SLOT_A, RC_ROLE_STANDBY);
	struct rc_status ac = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	uint8_t mask = 0U;

	role_init(&s, 0);
	(void)role_on_status(&s, &sb, 0);
	CHECK(!role_chaser_due(&s, 500, &mask));
	(void)role_on_status(&s, &ac, 500);
	CHECK(role_chaser_due(&s, 500, &mask));
	CHECK(mask == 0x02U); /* step 0 -> 1 */
	CHECK(!role_chaser_due(&s, 699, &mask));
	CHECK(role_chaser_due(&s, 700, &mask));
	CHECK(mask == 0x04U);
}

static void test_bumpless_takeover(void)
{
	struct role_state s;
	struct rc_status sb = status(RC_SLOT_B, RC_ROLE_STANDBY);
	struct rc_status ac = status(RC_SLOT_B, RC_ROLE_ACTIVE);
	struct rc_peer p = peer(RC_SLOT_A, RC_ROLE_ACTIVE, 5U);
	uint8_t mask = 0U;

	role_init(&s, 0);
	(void)role_on_status(&s, &sb, 0);
	(void)role_on_peer(&s, &p, 10);
	(void)role_on_status(&s, &ac, 300);
	CHECK(role_chaser_due(&s, 300, &mask));
	CHECK(mask == (uint8_t)(1U << 6)); /* continues at step 6 */
}

static void test_fault_when_both_active(void)
{
	struct role_state s;
	struct rc_status ac = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	struct rc_peer p_act = peer(RC_SLOT_B, RC_ROLE_ACTIVE, 0U);
	struct rc_peer p_sb = peer(RC_SLOT_B, RC_ROLE_STANDBY, 0U);

	role_init(&s, 0);
	(void)role_on_status(&s, &ac, 0);
	CHECK((role_on_peer(&s, &p_act, 10) & ROLE_EV_FAULT_SET) != 0U);
	CHECK(!role_may_drive(&s));
	CHECK((role_on_peer(&s, &p_sb, 30) & ROLE_EV_FAULT_CLEARED) != 0U);
	CHECK(role_may_drive(&s));
}

static void test_own_peer_message_ignored(void)
{
	struct role_state s;
	struct rc_status ac = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	struct rc_peer own = peer(RC_SLOT_A, RC_ROLE_ACTIVE, 9U);

	role_init(&s, 0);
	(void)role_on_status(&s, &ac, 0);
	CHECK(role_on_peer(&s, &own, 10) == 0U);
	CHECK(!s.peer_ok);
	CHECK(!s.fault);
}

static void test_peer_lost(void)
{
	struct role_state s;
	struct rc_status sb = status(RC_SLOT_B, RC_ROLE_STANDBY);
	struct rc_peer p = peer(RC_SLOT_A, RC_ROLE_ACTIVE, 1U);

	role_init(&s, 0);
	(void)role_on_status(&s, &sb, 0);
	CHECK((role_on_peer(&s, &p, 0) & ROLE_EV_PEER_BACK) != 0U);
	CHECK((role_tick(&s, 101) & ROLE_EV_PEER_LOST) != 0U);
	CHECK(!s.peer_ok);
}

static void test_step_wraps_without_jump(void)
{
	struct role_state s;
	struct rc_status ac = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	uint8_t mask = 0U;

	role_init(&s, 0);
	(void)role_on_status(&s, &ac, 0);
	s.step = 65534U;
	CHECK(role_chaser_due(&s, 0, &mask));
	CHECK(mask == 0x80U); /* 65535 % 8 = 7 */
	CHECK(role_chaser_due(&s, 200, &mask));
	CHECK(mask == 0x01U); /* 0 % 8 = 0: the next LED, no jump */
}

int main(void)
{
	test_initial_state();
	test_status_grants_active_and_learns_slot();
	test_unknown_grant_keeps_role();
	test_referee_lost_and_back();
	test_chaser_steps_only_when_driving();
	test_bumpless_takeover();
	test_fault_when_both_active();
	test_own_peer_message_ignored();
	test_peer_lost();
	test_step_wraps_without_jump();
	return CHECK_DONE();
}
```

- [ ] **Step 3: Run to see it fail** — `role.h` not found.

- [ ] **Step 4: Create `central/src/role.h`**

```c
#ifndef ROLE_H_
#define ROLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "rc/proto.h"

#define ROLE_REFEREE_TIMEOUT_MS 100
#define ROLE_PEER_TIMEOUT_MS 100
#define ROLE_CHASER_PERIOD_MS 200

#define ROLE_EV_ROLE_CHANGED (1U << 0)
#define ROLE_EV_REFEREE_LOST (1U << 1)
#define ROLE_EV_REFEREE_BACK (1U << 2)
#define ROLE_EV_PEER_LOST (1U << 3)
#define ROLE_EV_PEER_BACK (1U << 4)
#define ROLE_EV_FAULT_SET (1U << 5)
#define ROLE_EV_FAULT_CLEARED (1U << 6)
#define ROLE_EV_SLOT_LEARNED (1U << 7)

struct role_state {
	uint8_t slot;
	uint8_t role;
	bool referee_ok;
	int64_t last_status_ms;
	bool peer_ok;
	bool peer_seen;
	int64_t last_peer_ms;
	uint8_t peer_role;
	uint16_t peer_step;
	bool fault;
	uint16_t step;
	int64_t last_step_ms;
};

void role_init(struct role_state *s, int64_t now_ms);
unsigned role_on_status(struct role_state *s, const struct rc_status *m, int64_t now_ms);
unsigned role_on_peer(struct role_state *s, const struct rc_peer *m, int64_t now_ms);
unsigned role_tick(struct role_state *s, int64_t now_ms);
bool role_may_drive(const struct role_state *s);

/* When driving and a step is due, advances the chaser and returns true with *mask set. */
bool role_chaser_due(struct role_state *s, int64_t now_ms, uint8_t *mask);

#endif /* ROLE_H_ */
```

- [ ] **Step 5: Create `central/src/role.c`**

```c
#include <string.h>

#include "role.h"

void role_init(struct role_state *s, int64_t now_ms)
{
	memset(s, 0, sizeof(*s));
	s->slot = RC_SLOT_NONE;
	s->role = RC_ROLE_UNKNOWN;
	s->peer_role = RC_ROLE_UNKNOWN;
	s->last_status_ms = now_ms;
	s->last_peer_ms = now_ms;
}

static unsigned update_fault(struct role_state *s)
{
	bool f = (s->role == RC_ROLE_ACTIVE) && s->peer_ok && (s->peer_role == RC_ROLE_ACTIVE);

	if (f == s->fault) {
		return 0U;
	}
	s->fault = f;
	return f ? ROLE_EV_FAULT_SET : ROLE_EV_FAULT_CLEARED;
}

unsigned role_on_status(struct role_state *s, const struct rc_status *m, int64_t now_ms)
{
	unsigned ev = 0U;

	s->last_status_ms = now_ms;
	if (!s->referee_ok) {
		s->referee_ok = true;
		ev |= ROLE_EV_REFEREE_BACK;
	}
	if ((s->slot == RC_SLOT_NONE) && ((m->slot == RC_SLOT_A) || (m->slot == RC_SLOT_B))) {
		s->slot = m->slot;
		ev |= ROLE_EV_SLOT_LEARNED;
	}
	if ((m->granted_role != RC_ROLE_UNKNOWN) && (m->granted_role != s->role)) {
		if ((m->granted_role == RC_ROLE_ACTIVE) && s->peer_seen) {
			s->step = s->peer_step;
		}
		if (m->granted_role == RC_ROLE_ACTIVE) {
			s->last_step_ms = now_ms - ROLE_CHASER_PERIOD_MS;
		}
		s->role = m->granted_role;
		ev |= ROLE_EV_ROLE_CHANGED;
	}
	return ev | update_fault(s);
}

unsigned role_on_peer(struct role_state *s, const struct rc_peer *m, int64_t now_ms)
{
	unsigned ev = 0U;

	if ((s->slot != RC_SLOT_NONE) && (m->slot == s->slot)) {
		return 0U;
	}
	s->last_peer_ms = now_ms;
	s->peer_role = m->role;
	s->peer_step = m->step;
	s->peer_seen = true;
	if (!s->peer_ok) {
		s->peer_ok = true;
		ev |= ROLE_EV_PEER_BACK;
	}
	return ev | update_fault(s);
}

unsigned role_tick(struct role_state *s, int64_t now_ms)
{
	unsigned ev = 0U;

	if (s->referee_ok && ((now_ms - s->last_status_ms) > ROLE_REFEREE_TIMEOUT_MS)) {
		s->referee_ok = false;
		ev |= ROLE_EV_REFEREE_LOST;
	}
	if (s->peer_ok && ((now_ms - s->last_peer_ms) > ROLE_PEER_TIMEOUT_MS)) {
		s->peer_ok = false;
		ev |= ROLE_EV_PEER_LOST;
	}
	return ev | update_fault(s);
}

bool role_may_drive(const struct role_state *s)
{
	return (s->role == RC_ROLE_ACTIVE) && s->referee_ok && !s->fault;
}

bool role_chaser_due(struct role_state *s, int64_t now_ms, uint8_t *mask)
{
	if (!role_may_drive(s) || ((now_ms - s->last_step_ms) < ROLE_CHASER_PERIOD_MS)) {
		return false;
	}
	s->step++;
	s->last_step_ms = now_ms;
	*mask = (uint8_t)(1U << (s->step % 8U));
	return true;
}
```

- [ ] **Step 6: Run to see it pass.** Expected: three test executables, all `all checks passed`.

- [ ] **Step 7: Commit**

```bash
git add central/src/role.h central/src/role.c tests/host
git commit -m "feat: add controller role logic and LED chaser" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Controller daemon on the Pis

**Files:**
- Create: `central/src/main.c`, `central/CMakeLists.txt`, `central/rc-central.service`, `tools/deploy-central.sh`
- Modify: `docs/2026-09-27-redundancy-core-design.md` (record the four planning refinements)

**Interfaces:**
- Consumes: `rc/proto.h`, `role.h`.
- Produces: `rc-central` binary; log lines on stdout, one per event: `t=<ms> slot=<A|B|-> role=<standby|active|unknown> event=<name>`.

- [ ] **Step 1: Create `central/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(rc_central C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -O2)

add_executable(rc-central src/main.c src/role.c ../common/proto.c)
target_include_directories(rc-central PRIVATE ../common/include src)
install(TARGETS rc-central DESTINATION bin)
```

- [ ] **Step 2: Create `central/src/main.c`**

```c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "rc/proto.h"
#include "role.h"

#define SERIAL_DEV "/dev/serial0"
#define PEER_IFACE "eth0"
#define PEER_GROUP "ff02::1"
#define PEER_PORT 47000
#define HEARTBEAT_PERIOD_MS 20

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void die(const char *what)
{
	fprintf(stderr, "fatal: %s: %s\n", what, strerror(errno));
	exit(1);
}

static int open_serial(void)
{
	struct termios tio;
	int fd = open(SERIAL_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (fd < 0) {
		die("open " SERIAL_DEV);
	}
	if (tcgetattr(fd, &tio) != 0) {
		die("tcgetattr");
	}
	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tio.c_cflag |= CLOCAL | CREAD;
	if (tcsetattr(fd, TCSANOW, &tio) != 0) {
		die("tcsetattr");
	}
	return fd;
}

static int open_peer(struct sockaddr_in6 *group)
{
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	unsigned int ifindex = if_nametoindex(PEER_IFACE);
	struct sockaddr_in6 local = {.sin6_family = AF_INET6, .sin6_port = htons(PEER_PORT),
				     .sin6_addr = IN6ADDR_ANY_INIT};
	int one = 1;
	int zero = 0;

	if (fd < 0) {
		die("socket");
	}
	if (ifindex == 0U) {
		die("if_nametoindex " PEER_IFACE);
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
		die("SO_REUSEADDR");
	}
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, PEER_IFACE, sizeof(PEER_IFACE)) != 0) {
		die("SO_BINDTODEVICE");
	}
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		die("bind");
	}
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex)) != 0) {
		die("IPV6_MULTICAST_IF");
	}
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &zero, sizeof(zero)) != 0) {
		die("IPV6_MULTICAST_LOOP");
	}
	memset(group, 0, sizeof(*group));
	group->sin6_family = AF_INET6;
	group->sin6_port = htons(PEER_PORT);
	group->sin6_scope_id = ifindex;
	if (inet_pton(AF_INET6, PEER_GROUP, &group->sin6_addr) != 1) {
		die("inet_pton");
	}
	return fd;
}

static const char *role_name(uint8_t role)
{
	switch (role) {
	case RC_ROLE_ACTIVE:
		return "active";
	case RC_ROLE_STANDBY:
		return "standby";
	default:
		return "unknown";
	}
}

static char slot_name(uint8_t slot)
{
	return (slot == RC_SLOT_A) ? 'A' : ((slot == RC_SLOT_B) ? 'B' : '-');
}

static void log_events(unsigned ev, const struct role_state *s, int64_t t)
{
	static const struct {
		unsigned bit;
		const char *name;
	} names[] = {
		{ROLE_EV_SLOT_LEARNED, "slot_learned"},   {ROLE_EV_ROLE_CHANGED, "role_changed"},
		{ROLE_EV_REFEREE_LOST, "referee_lost"},   {ROLE_EV_REFEREE_BACK, "referee_back"},
		{ROLE_EV_PEER_LOST, "peer_lost"},         {ROLE_EV_PEER_BACK, "peer_back"},
		{ROLE_EV_FAULT_SET, "referee_fault"},     {ROLE_EV_FAULT_CLEARED, "fault_cleared"},
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((ev & names[i].bit) != 0U) {
			printf("t=%lld slot=%c role=%s step=%u event=%s\n", (long long)t,
			       slot_name(s->slot), role_name(s->role), (unsigned)s->step,
			       names[i].name);
		}
	}
	if (ev != 0U) {
		fflush(stdout);
	}
}

static void write_all(int fd, const uint8_t *buf, size_t n)
{
	while (n > 0U) {
		ssize_t w = write(fd, buf, n);

		if (w < 0) {
			if (errno == EAGAIN) {
				continue;
			}
			die("serial write");
		}
		buf += w;
		n -= (size_t)w;
	}
}

int main(void)
{
	struct sockaddr_in6 group;
	int serial = open_serial();
	int peer = open_peer(&group);
	struct rc_parser parser;
	struct role_state s;
	uint32_t hb_seq = 0U;
	uint32_t peer_seq = 0U;
	int64_t next_hb = now_ms();

	setvbuf(stdout, NULL, _IOLBF, 0);
	rc_parser_init(&parser);
	role_init(&s, now_ms());
	printf("t=%lld rc-central started\n", (long long)now_ms());

	for (;;) {
		struct pollfd fds[2] = {{.fd = serial, .events = POLLIN}, {.fd = peer, .events = POLLIN}};
		int64_t wait = next_hb - now_ms();
		int64_t t;
		unsigned ev = 0U;
		uint8_t mask;

		if (poll(fds, 2, (wait > 0) ? (int)wait : 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			die("poll");
		}
		t = now_ms();
		if ((fds[0].revents & POLLIN) != 0) {
			uint8_t buf[64];
			ssize_t n = read(serial, buf, sizeof(buf));

			if ((n < 0) && (errno != EAGAIN)) {
				die("serial read");
			}
			for (ssize_t i = 0; i < n; i++) {
				struct rc_frame f;
				struct rc_status st;

				if (rc_parser_feed(&parser, buf[i], &f) && (rc_decode_status(&f, &st) == 0)) {
					ev |= role_on_status(&s, &st, t);
				}
			}
		}
		if ((fds[1].revents & POLLIN) != 0) {
			uint8_t buf[RC_FRAME_MAX];
			ssize_t n = recv(peer, buf, sizeof(buf), 0);
			struct rc_parser pp;

			if ((n < 0) && (errno != EAGAIN)) {
				die("peer recv");
			}
			rc_parser_init(&pp);
			for (ssize_t i = 0; i < n; i++) {
				struct rc_frame f;
				struct rc_peer pm;

				if (rc_parser_feed(&pp, buf[i], &f) && (rc_decode_peer(&f, &pm) == 0)) {
					ev |= role_on_peer(&s, &pm, t);
				}
			}
		}
		ev |= role_tick(&s, t);
		log_events(ev, &s, t);

		if (t >= next_hb) {
			struct rc_heartbeat hb = {.seq = hb_seq++, .role = s.role};
			struct rc_peer pm = {.seq = peer_seq++, .slot = s.slot, .role = s.role,
					     .referee_ok = s.referee_ok ? 1U : 0U, .step = s.step};
			uint8_t out[RC_FRAME_MAX];
			size_t n = rc_encode_heartbeat(&hb, out, sizeof(out));

			write_all(serial, out, n);
			n = rc_encode_peer(&pm, out, sizeof(out));
			if ((sendto(peer, out, n, 0, (struct sockaddr *)&group, sizeof(group)) < 0) &&
			    (errno != EAGAIN)) {
				die("peer send");
			}
			next_hb += HEARTBEAT_PERIOD_MS;
			if (next_hb <= t) {
				next_hb = t + HEARTBEAT_PERIOD_MS;
			}
		}
		if (role_chaser_due(&s, t, &mask)) {
			struct rc_set_outputs so = {.mask = mask};
			uint8_t out[RC_FRAME_MAX];
			size_t n = rc_encode_set_outputs(&so, out, sizeof(out));

			write_all(serial, out, n);
		}
	}
	return 0;
}
```

- [ ] **Step 3: Create `central/rc-central.service`**

```ini
[Unit]
Description=Redundant controller daemon
After=network.target

[Service]
ExecStart=/usr/local/bin/rc-central
Restart=always
RestartSec=1
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=50

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 4: Create `tools/deploy-central.sh`** (executable)

```bash
#!/bin/bash
# Build and install rc-central on the controllers: deploy-central.sh [host...]
set -euo pipefail
KEY=${RC_PI_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
HOSTS=("$@")
if [ ${#HOSTS[@]} -eq 0 ]; then
  HOSTS=(alpental@192.168.45.50 alpental@192.168.45.176)
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for h in "${HOSTS[@]}"; do
  rsync -a --delete -e "ssh -i $KEY" "$ROOT/common" "$ROOT/central" "$h:rc-src/"
  ssh -i "$KEY" -o BatchMode=yes "$h" '
    set -e
    cmake -S rc-src/central -B rc-src/build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build rc-src/build
    sudo cmake --install rc-src/build --prefix /usr/local >/dev/null
    sudo install -m 644 rc-src/central/rc-central.service /etc/systemd/system/rc-central.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now rc-central.service
    sudo systemctl restart rc-central.service
    echo "$(hostname): rc-central $(systemctl is-active rc-central)"'
done
```

- [ ] **Step 5: Install build tools on both Pis once**

```bash
for h in 192.168.45.50 192.168.45.176; do
  ssh -i ~/.ssh/id_ed25519_alpental_pi alpental@$h 'sudo apt-get update -qq && sudo apt-get install -y -qq build-essential cmake rsync'
done
```

Expected: both finish without errors.

- [ ] **Step 6: Deploy and check (owner at bench, I/O card from Task 5 running).** Run `tools/deploy-central.sh`. Expected: both print `rc-central active`. Then:

```bash
for h in 192.168.45.50 192.168.45.176; do ssh -i ~/.ssh/id_ed25519_alpental_pi alpental@$h 'journalctl -u rc-central -n 6 --no-pager -o cat'; done
tools/console-remote.sh 3
```

Expected: rc-a logs `slot_learned`, `peer_back`, then `role_changed` with `role=active`; rc-b logs `role=standby`; the I/O card logs one `active: - -> A`; the LED chaser runs on the board (one LED stepping clockwise every 200 ms).

- [ ] **Step 7: Record the planning refinements in the design doc.** In `docs/2026-09-27-redundancy-core-design.md`:
  - Under "Roles and arbitration", replace the "Boot window" bullet with: "**Election window:** when no slot is Active and a controller appears, the I/O card waits 1500 ms from that first appearance, then grants Active: a present slot that reports Active first (slot A if both), otherwise slot A, otherwise slot B. During the window STATUS grants role 0 (unknown), and a controller keeps its current role on 'unknown'."
  - Under "Cross-link heartbeat", replace the first line with: "UDP to the IPv6 link-local multicast group `ff02::1` on `eth0`, port 47000 (no address configuration needed), every 20 ms: slot, granted role, seq, referee link state, chaser step. A controller ignores messages carrying its own slot."
  - Under "I/O card indications", replace the second bullet with: "All LEDs are off while no slot is Active; every Active change is logged on the console with the gap from the lost slot's last frame."
  - Under the UART framing table, add: "The CRC is sent low byte first."

- [ ] **Step 8: Run host tests, then commit**

```bash
git add central tools docs/2026-09-27-redundancy-core-design.md
git commit -m "feat: add controller daemon with cross-link and systemd unit" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Experiments and test log

**Files:**
- Create: `docs/test-log.md`, `tools/collect.sh`

**Interfaces:**
- Consumes: the running system from Tasks 5 and 7.

**Precondition:** both Pis on proper 5 V / 2.5 A supplies; `vcgencmd get_throttled` shows `throttled=0x0` on both after boot. Do not run Experiment 1 or 3 while either Pi reports under-voltage.

- [ ] **Step 1: Create `tools/collect.sh`** (executable). It prints the last N minutes of both daemons and N seconds of I/O card console:

```bash
#!/bin/bash
# Collect logs for an experiment: collect.sh <minutes> <console-seconds>
set -euo pipefail
KEY=${RC_PI_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
MIN=${1:-5}
for h in alpental@192.168.45.50 alpental@192.168.45.176; do
  echo "== $h"
  ssh -i "$KEY" -o BatchMode=yes "$h" "journalctl -u rc-central --since '-${MIN} min' --no-pager -o cat | grep -E 'event=(role_changed|referee_|peer_|fault)'"
done
echo "== io-card"
"$(dirname "$0")/console-remote.sh" "${2:-3}" | grep -E 'active:|ready'
```

- [ ] **Step 2: Experiment 1, power loss (owner pulls power).** With A Active and the chaser running, start `tools/console-remote.sh 20` in one terminal and pull rc-a's power. Record the `active: A -> B gap_ms=` value and whether the chaser continued from the next LED. Restore power; rc-a must rejoin as standby (no `active:` line). Repeat 10 times, alternating which Pi is Active. Pass: every `gap_ms` under 200 and no chaser jump.

- [ ] **Step 3: Experiment 2, cross-link loss.** Unplug the Pi-to-Pi cable for 10 s, then plug it back. Pass: both log `peer_lost` then `peer_back`; no `role_changed`; no `active:` line.

- [ ] **Step 4: Experiment 3, simultaneous boot.** Put both Pis on one switched power strip; switch it off and on 10 times, waiting for the chaser each time. Pass: every run shows exactly one `active: - -> A` line.

- [ ] **Step 5: Experiment 4, I/O card loss.** Press the STM32 black RESET button and hold it 3 s, then release. Pass: LEDs off while held; both daemons log `referee_lost`, then `referee_back`; no `role_changed` on either; the I/O card logs `active: - -> <previous Active>` after the 1.5 s window and the chaser resumes.

- [ ] **Step 6: Create `docs/test-log.md`** from the recorded data, one table per experiment with columns: run, conditions, observed values, pass/fail, plus a short paragraph per experiment on anything unexpected and what was changed because of it.

- [ ] **Step 7: Commit**

```bash
git add docs/test-log.md tools/collect.sh
git commit -m "docs: record redundancy experiments" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: README and CI

**Files:**
- Create: `README.md`, `.github/workflows/ci.yml`

- [ ] **Step 1: Create `README.md`**

````markdown
# Redundant controller with an I/O card referee

Two Raspberry Pi 3B controllers run as Active/Standby. An STM32F3 Discovery
board is the I/O card: it drives the outputs (an 8-LED ring) and acts as
referee, granting Active to exactly one controller. A failed Active is
replaced within 200 ms, simultaneous boot always elects slot A, and a broken
link between the controllers never creates two Actives.

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
````

- [ ] **Step 2: Create `.github/workflows/ci.yml`**

```yaml
name: CI

on:
  push:
  pull_request:

jobs:
  host-tests:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: |
          cmake -S tests/host -B build/host
          cmake --build build/host
          ctest --test-dir build/host --output-on-failure

  central:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: |
          cmake -S central -B build/central
          cmake --build build/central

  io-card:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
        with:
          path: redundant-controller
      - uses: zephyrproject-rtos/action-zephyr-setup@v1
        with:
          app-path: redundant-controller
          toolchains: arm-zephyr-eabi
      - run: west build -b stm32f3_disco@E -d build/io-card redundant-controller/io-card
```

- [ ] **Step 3: Run host tests and both firmware builds locally.** Expected: all pass, no warnings.

- [ ] **Step 4: Commit**

```bash
git add README.md .github
git commit -m "docs: add README and CI workflow" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 5: Push when the owner says so, then check CI.**
