# Ring-Buffer Log and Mac Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each controller keeps a 10 MiB ring log of 100 ms snapshots and events stamped with the I/O card's clock; a low-priority server on each Pi serves it; a Mac viewer merges both logs into one filtered timeline with CSV export.

**Architecture:** The record codec (C and Python), the ring file, and the I/O card time estimate are pure, host-tested units. The real-time daemon only appends 64-byte records; `rc_logd.py` serves and flushes the file; `tools/rcview.py` downloads, decodes, merges, and serves one local HTML page with a small JSON API.

**Tech Stack:** C11 (daemon, codec), Zephyr 4.4.2 (I/O card), Python 3 standard library (codec, log server, viewer), CMake/CTest host tests.

**Spec:** `docs/2026-09-27-logging-viewer-design.md`

## Global Constraints

- Record: 64 bytes, little-endian, magic `0x4C52`, version 1, CRC-16/CCITT-FALSE over bytes 0-61, low byte first.
- Ring: 163,840 slots (10 MiB) at `/var/lib/rc-central/rc-log.bin`; record `seq` goes to slot `seq mod slots`; one `pwrite` per record, no `fsync` in the daemon.
- Snapshots every 100 ms; events at once.
- Logging never stops or delays control: errors are logged once and the daemon continues.
- `rc-logd`: port 8080, `GET /log`, `GET /info`, `fsync` every 2 s, `Nice=10`, `IOSchedulingClass=idle`.
- Viewer: `http://127.0.0.1:8765`, Python standard library only, no internet access, CSV only.
- C11 with `-Wall -Wextra -Werror`; kernel-style tabs; comments short, English, plain ASCII.
- No customer names, part numbers, or contract details.

## Design refinements (found while planning; approve with this plan)

1. **Segment matching by I/O card boot time.** The spec matches segments from the two controllers by their start time. The two logs cover different time spans (a controller restarted or wrapped at a different moment), so the same I/O card run can start hours apart in the two files. Instead, each segment gets an estimated I/O card boot time, the median of `wall_ms - io_time_ms` over its records; segments whose boot times differ by at most 10 s are the same I/O card run.
2. **`.gitignore`** with `logs/` (downloaded logs) and `__pycache__/` (from the Python tests and imports), so running the viewer or the tests leaves the working tree clean.

Task 0 writes refinement 1 into the spec.

## Review Focus

1. **SD card write stalls:** a `pwrite` into the page cache can block when writeback throttles dirty pages on a slow SD card; that would show as `loop_timing` failures and a handover (experiments 1 and 4 measure the loop maximum).
2. **Torn records during download:** `rc-logd` reads the file while the daemon writes; a half-written slot must fail its CRC and be skipped and counted, never shown as data.
3. **I/O card clock discontinuities:** I/O card restarts (clock back to 0) and records before the first STATUS (no I/O card time) must land in the right segment.
4. **Bad log file at start:** unwritable directory, short or corrupt file: the daemon must still start controlling, with logging disabled or resumed from the highest valid seq.
5. **Full logs in the viewer:** about 330,000 rows must stay responsive (server-side filtering and 500-row pages).

---

### Task 0: Spec refinement

**Files:**
- Modify: `docs/2026-09-27-logging-viewer-design.md`

- [ ] **Step 1: Replace the segment rule**

Replace the Viewer bullet that starts with "**Segments:**" with:

```markdown
- **Segments:** within one controller's log, ordered by `seq`, a drop in
  `io_time_ms` of more than 1 s starts a new segment (I/O card restart).
  Records without a valid `io_time_ms` go into the segment of the next
  valid record. Each segment's I/O card boot time is estimated as the
  median of `wall_ms - io_time_ms`; segments from the two logs whose boot
  times differ by at most 10 s are the same I/O card run and merge.
```

- [ ] **Step 2: Commit**

```bash
git add docs/2026-09-27-logging-viewer-design.md
git commit -m "docs: match log segments by I/O card boot time" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 1: I/O card time in STATUS

**Files:**
- Modify: `common/include/rc/proto.h`, `common/proto.c`, `io-card/src/main.c`
- Test: `tests/host/test_proto.c`

**Interfaces:**
- Produces: `struct rc_status` gains `uint32_t io_time_ms` (last member); STATUS payload 13 bytes.

- [ ] **Step 1: Write the failing tests**

In `tests/host/test_proto.c` `test_status_roundtrip`, add `.io_time_ms = 0xA1B2C3D4U` to the initializer of `in`, change `CHECK(n == RC_FRAME_OVERHEAD + 9U);` to `CHECK(n == RC_FRAME_OVERHEAD + 13U);`, and add after the last CHECK:

```c
	CHECK(out.io_time_ms == 0xA1B2C3D4U);
```

In `test_decode_rejects_part1_lengths`, add before the first CHECK:

```c
	struct rc_frame st2 = {.type = RC_MSG_STATUS, .len = 9U};
```

and after the last CHECK:

```c
	CHECK(rc_decode_status(&st2, &s) == -1); /* part 2 length */
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build/host 2>&1 | grep -m2 error`
Expected: `field designator 'io_time_ms' does not refer to any field` (or the GCC equivalent).

- [ ] **Step 3: Implement**

In `common/include/rc/proto.h`, add `uint32_t io_time_ms;` after `uint8_t io_fail;` in `struct rc_status`.

In `common/proto.c` `rc_encode_status`, change `uint8_t p[9];` to `uint8_t p[13];` and add after `p[8] = m->io_fail;`:

```c
	put_u32(&p[9], m->io_time_ms);
```

In `rc_decode_status`, change `(f->len != 9U)` to `(f->len != 13U)` and add after `m->io_fail = f->payload[8];`:

```c
	m->io_time_ms = get_u32(&f->payload[9]);
```

In `io-card/src/main.c` `handle_frame`, add after `st.io_fail = io_fail;`:

```c
		st.io_time_ms = (uint32_t)now;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 8`.

Run: `cd ~/ws/stm32f3-ws && . ~/ws/zp/zephyr-venv/bin/activate && west build -b stm32f3_disco@E -d build/rc-io-card ~/ws/redundant-controller/io-card 2>&1 | grep -E "FLASH:|error"`
Expected: a FLASH line, no `error`.

- [ ] **Step 5: Commit**

```bash
git add common tests/host/test_proto.c io-card/src/main.c
git commit -m "feat: add the I/O card time to STATUS" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Log record codec (C)

**Files:**
- Create: `common/include/rc/logrec.h`, `common/logrec.c`, `tests/host/test_logrec.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `rc_crc16` from `common/proto.c`.
- Produces: `RC_LOG_RECORD_SIZE` (64), `RC_LOG_MAGIC`, `RC_LOG_VERSION`, `RC_LOG_SNAPSHOT` (1), `RC_LOG_EVENT` (2), `RC_LOG_F_*` flag bits, `RC_LOG_EV_*` event codes (0-14), `RC_LOG_BIT_NOT_RUN` (3); `struct rc_log_record`; `void rc_log_encode(const struct rc_log_record *r, uint8_t *out)`; `int rc_log_decode(const uint8_t *in, struct rc_log_record *r)` (0 or -1); `uint16_t rc_log_pack_bits(const uint8_t *results, int n)`; `uint8_t rc_log_bit_result(uint16_t packed, int item)`.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_logrec.c`:

```c
#include <string.h>

#include "check.h"
#include "rc/logrec.h"
#include "rc/proto.h"

static struct rc_log_record sample(void)
{
	struct rc_log_record r;

	memset(&r, 0, sizeof(r));
	r.type = RC_LOG_EVENT;
	r.seq = 0x01020304U;
	r.io_time_ms = 123456U;
	r.mono_ms = 9876543210ULL;
	r.wall_ms = 1790000000123ULL;
	r.slot = RC_SLOT_B;
	r.role = RC_ROLE_ACTIVE;
	r.mode = RC_MODE_TEST;
	r.flags = RC_LOG_F_HEALTHY | RC_LOG_F_IO_TIME_VALID;
	r.active_slot = RC_SLOT_B;
	r.io_fail = 0x21U;
	r.bit_results = 0x0350U;
	r.step = 513U;
	r.mask = 0x55U;
	r.event = RC_LOG_EV_ROLE_CHANGED;
	r.detail = RC_ROLE_ACTIVE;
	return r;
}

/* Rewrites the CRC after a test changes a byte on purpose. */
static void fix_crc(uint8_t *b)
{
	uint16_t crc = rc_crc16(b, 62U);

	b[62] = (uint8_t)(crc & 0xFFU);
	b[63] = (uint8_t)(crc >> 8);
}

static void test_roundtrip(void)
{
	struct rc_log_record in = sample();
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	memset(&out, 0, sizeof(out));
	rc_log_encode(&in, b);
	CHECK(b[0] == 0x52U && b[1] == 0x4CU && b[2] == RC_LOG_VERSION);
	CHECK(b[4] == 0x04U); /* little-endian seq */
	for (size_t i = 44U; i < 62U; i++) {
		CHECK(b[i] == 0U);
	}
	CHECK(rc_log_decode(b, &out) == 0);
	CHECK(out.type == in.type && out.seq == in.seq && out.io_time_ms == in.io_time_ms);
	CHECK(out.mono_ms == in.mono_ms && out.wall_ms == in.wall_ms);
	CHECK(out.slot == in.slot && out.role == in.role && out.mode == in.mode);
	CHECK(out.flags == in.flags && out.active_slot == in.active_slot);
	CHECK(out.io_fail == in.io_fail && out.bit_results == in.bit_results);
	CHECK(out.step == in.step && out.mask == in.mask);
	CHECK(out.event == in.event && out.detail == in.detail);
}

static void test_flipped_byte_rejected(void)
{
	struct rc_log_record in = sample();
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	rc_log_encode(&in, b);
	b[20] ^= 0x01U;
	CHECK(rc_log_decode(b, &out) == -1);
}

static void test_bad_magic_version_type(void)
{
	struct rc_log_record in = sample();
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	rc_log_encode(&in, b);
	b[0] = 0x00U;
	fix_crc(b);
	CHECK(rc_log_decode(b, &out) == -1);
	rc_log_encode(&in, b);
	b[2] = 2U;
	fix_crc(b);
	CHECK(rc_log_decode(b, &out) == -1);
	rc_log_encode(&in, b);
	b[3] = 3U;
	fix_crc(b);
	CHECK(rc_log_decode(b, &out) == -1);
}

static void test_empty_slot_rejected(void)
{
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	memset(b, 0, sizeof(b));
	CHECK(rc_log_decode(b, &out) == -1);
}

static void test_bit_packing(void)
{
	const uint8_t results[5] = {0U, 0U, 1U, 1U, RC_LOG_BIT_NOT_RUN};

	CHECK(rc_log_pack_bits(results, 5) == 0x0350U);
	CHECK(rc_log_bit_result(0x0350U, 2) == 1U);
	CHECK(rc_log_bit_result(0x0350U, 4) == RC_LOG_BIT_NOT_RUN);
	CHECK(rc_log_bit_result(0x0350U, 0) == 0U);
}

int main(void)
{
	test_roundtrip();
	test_flipped_byte_rejected();
	test_bad_magic_version_type();
	test_empty_slot_rejected();
	test_bit_packing();
	return CHECK_DONE();
}
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_logrec test_logrec.c ${REPO}/common/logrec.c ${REPO}/common/proto.c)
target_include_directories(test_logrec PRIVATE ${REPO}/common/include)
add_test(NAME logrec COMMAND test_logrec)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S tests/host -B build/host 2>&1 | grep -m2 -i "cannot find\|logrec.c"`
Expected: CMake reports that `common/logrec.c` cannot be found.

- [ ] **Step 3: Implement**

Create `common/include/rc/logrec.h`:

```c
#ifndef RC_LOGREC_H_
#define RC_LOGREC_H_

#include <stdint.h>

#define RC_LOG_RECORD_SIZE 64U
#define RC_LOG_MAGIC 0x4C52U
#define RC_LOG_VERSION 1U

#define RC_LOG_SNAPSHOT 1U
#define RC_LOG_EVENT 2U

#define RC_LOG_F_HEALTHY (1U << 0)
#define RC_LOG_F_REFEREE_OK (1U << 1)
#define RC_LOG_F_PEER_OK (1U << 2)
#define RC_LOG_F_PEER_HEALTHY (1U << 3)
#define RC_LOG_F_FAULT (1U << 4)
#define RC_LOG_F_IO_TIME_VALID (1U << 5)

#define RC_LOG_EV_NONE 0U
#define RC_LOG_EV_STARTED 1U
#define RC_LOG_EV_ROLE_CHANGED 2U
#define RC_LOG_EV_HEALTHY 3U
#define RC_LOG_EV_UNHEALTHY 4U
#define RC_LOG_EV_REFEREE_LOST 5U
#define RC_LOG_EV_REFEREE_BACK 6U
#define RC_LOG_EV_PEER_LOST 7U
#define RC_LOG_EV_PEER_BACK 8U
#define RC_LOG_EV_MODE_CHANGED 9U
#define RC_LOG_EV_FAULT_SET 10U
#define RC_LOG_EV_FAULT_CLEARED 11U
#define RC_LOG_EV_BIT_ITEM 12U
#define RC_LOG_EV_TCP_OUTPUT 13U
#define RC_LOG_EV_SLOT_LEARNED 14U

/* BIT result codes 0-2 match enum bit_result; 3 = not run yet. */
#define RC_LOG_BIT_NOT_RUN 3U

struct rc_log_record {
	uint8_t type;
	uint32_t seq;
	uint32_t io_time_ms;
	uint64_t mono_ms;
	uint64_t wall_ms;
	uint8_t slot;
	uint8_t role;
	uint8_t mode;
	uint8_t flags;
	uint8_t active_slot;
	uint8_t io_fail;
	uint16_t bit_results;
	uint16_t step;
	uint8_t mask;
	uint8_t event;
	uint32_t detail;
};

/* Writes RC_LOG_RECORD_SIZE bytes: magic, version, fields, zero reserve, CRC. */
void rc_log_encode(const struct rc_log_record *r, uint8_t *out);

/* Returns 0, or -1 for a wrong magic, version, type, or CRC. */
int rc_log_decode(const uint8_t *in, struct rc_log_record *r);

/* 2 bits per item, item 0 in the low bits; at most 8 items. */
uint16_t rc_log_pack_bits(const uint8_t *results, int n);
uint8_t rc_log_bit_result(uint16_t packed, int item);

#endif /* RC_LOGREC_H_ */
```

Create `common/logrec.c`:

```c
#include <string.h>

#include "rc/logrec.h"
#include "rc/proto.h"

static void put_u16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xFFU);
	b[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *b, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		b[i] = (uint8_t)(v >> (8 * i));
	}
}

static void put_u64(uint8_t *b, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		b[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint16_t get_u16(const uint8_t *b)
{
	return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t get_u32(const uint8_t *b)
{
	uint32_t v = 0U;

	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)b[i] << (8 * i);
	}
	return v;
}

static uint64_t get_u64(const uint8_t *b)
{
	uint64_t v = 0U;

	for (int i = 0; i < 8; i++) {
		v |= (uint64_t)b[i] << (8 * i);
	}
	return v;
}

void rc_log_encode(const struct rc_log_record *r, uint8_t *out)
{
	memset(out, 0, RC_LOG_RECORD_SIZE);
	put_u16(&out[0], RC_LOG_MAGIC);
	out[2] = RC_LOG_VERSION;
	out[3] = r->type;
	put_u32(&out[4], r->seq);
	put_u32(&out[8], r->io_time_ms);
	put_u64(&out[12], r->mono_ms);
	put_u64(&out[20], r->wall_ms);
	out[28] = r->slot;
	out[29] = r->role;
	out[30] = r->mode;
	out[31] = r->flags;
	out[32] = r->active_slot;
	out[33] = r->io_fail;
	put_u16(&out[34], r->bit_results);
	put_u16(&out[36], r->step);
	out[38] = r->mask;
	out[39] = r->event;
	put_u32(&out[40], r->detail);
	put_u16(&out[62], rc_crc16(out, 62U));
}

int rc_log_decode(const uint8_t *in, struct rc_log_record *r)
{
	if ((get_u16(&in[0]) != RC_LOG_MAGIC) || (in[2] != RC_LOG_VERSION) ||
	    ((in[3] != RC_LOG_SNAPSHOT) && (in[3] != RC_LOG_EVENT)) ||
	    (get_u16(&in[62]) != rc_crc16(in, 62U))) {
		return -1;
	}
	r->type = in[3];
	r->seq = get_u32(&in[4]);
	r->io_time_ms = get_u32(&in[8]);
	r->mono_ms = get_u64(&in[12]);
	r->wall_ms = get_u64(&in[20]);
	r->slot = in[28];
	r->role = in[29];
	r->mode = in[30];
	r->flags = in[31];
	r->active_slot = in[32];
	r->io_fail = in[33];
	r->bit_results = get_u16(&in[34]);
	r->step = get_u16(&in[36]);
	r->mask = in[38];
	r->event = in[39];
	r->detail = get_u32(&in[40]);
	return 0;
}

uint16_t rc_log_pack_bits(const uint8_t *results, int n)
{
	uint16_t packed = 0U;

	for (int i = 0; (i < n) && (i < 8); i++) {
		packed |= (uint16_t)((results[i] & 3U) << (2 * i));
	}
	return packed;
}

uint8_t rc_log_bit_result(uint16_t packed, int item)
{
	return (uint8_t)((packed >> (2 * item)) & 3U);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 9`.

- [ ] **Step 5: Commit**

```bash
git add common/include/rc/logrec.h common/logrec.c tests/host/test_logrec.c tests/host/CMakeLists.txt
git commit -m "feat: add the log record codec" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: I/O card time estimate

**Files:**
- Modify: `central/src/role.h`, `central/src/role.c`
- Test: `tests/host/test_role.c`

**Interfaces:**
- Consumes: `rc_status.io_time_ms` from Task 1.
- Produces: `struct role_state` gains `uint32_t io_time_ms; bool io_time_valid;`; `bool role_io_time(const struct role_state *s, int64_t now_ms, uint32_t *io_time_ms)`.

- [ ] **Step 1: Write the failing test**

Add to `tests/host/test_role.c` before `int main(void)`:

```c
static void test_io_time_estimate(void)
{
	struct role_state s;
	struct rc_status m = status(RC_SLOT_A, RC_ROLE_STANDBY);
	uint32_t io = 0U;

	role_init(&s, 0);
	CHECK(!role_io_time(&s, 50, &io));
	m.io_time_ms = 5000U;
	(void)role_on_status(&s, &m, 100);
	CHECK(role_io_time(&s, 130, &io) && (io == 5030U));
	m.io_time_ms = 0xFFFFFFF0U; /* wraps like the I/O card's 32-bit clock */
	(void)role_on_status(&s, &m, 200);
	CHECK(role_io_time(&s, 232, &io) && (io == 0x10U));
}
```

and the call `test_io_time_estimate();` before `return CHECK_DONE();`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/host 2>&1 | grep -m1 error`
Expected: `call to undeclared function 'role_io_time'` (or the GCC equivalent).

- [ ] **Step 3: Implement**

In `central/src/role.h`, add at the end of `struct role_state`:

```c
	uint32_t io_time_ms;
	bool io_time_valid;
```

and before `#endif`:

```c
/* I/O card time at now_ms: last STATUS time plus the time since; false before any STATUS. */
bool role_io_time(const struct role_state *s, int64_t now_ms, uint32_t *io_time_ms);
```

In `central/src/role.c` `role_on_status`, add after `s->io_fail = m->io_fail;`:

```c
	s->io_time_ms = m->io_time_ms;
	s->io_time_valid = true;
```

and at the end of the file:

```c
bool role_io_time(const struct role_state *s, int64_t now_ms, uint32_t *io_time_ms)
{
	if (!s->io_time_valid) {
		return false;
	}
	*io_time_ms = s->io_time_ms + (uint32_t)(now_ms - s->last_status_ms);
	return true;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 9`.

- [ ] **Step 5: Commit**

```bash
git add central/src/role.h central/src/role.c tests/host/test_role.c
git commit -m "feat: estimate the I/O card time between STATUS frames" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Ring log store

**Files:**
- Create: `central/src/logstore.h`, `central/src/logstore.c`, `tests/host/test_logstore.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `rc_log_encode`, `rc_log_decode`, `RC_LOG_RECORD_SIZE` from Task 2.
- Produces: `LOGSTORE_SLOTS` (163840); `struct logstore { int fd; uint32_t slots; uint32_t next_seq; }`; `int logstore_open(struct logstore *ls, const char *path, uint32_t slots)`, `int logstore_append(struct logstore *ls, struct rc_log_record *rec)`, `int logstore_close(struct logstore *ls)`, each returning 0 or `-errno`.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_logstore.c`:

```c
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "logstore.h"

static char path[64];

/* A unique path whose file does not exist yet. */
static void fresh_path(void)
{
	int fd;

	snprintf(path, sizeof(path), "/tmp/rc-logstore-XXXXXX");
	fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);
	unlink(path);
}

static struct rc_log_record snap(void)
{
	struct rc_log_record r;

	memset(&r, 0, sizeof(r));
	r.type = RC_LOG_SNAPSHOT;
	r.step = 7U;
	return r;
}

/* Seq stored in a slot, or -1 when the slot does not hold a valid record. */
static long seq_in_slot(uint32_t slot)
{
	uint8_t buf[RC_LOG_RECORD_SIZE];
	struct rc_log_record r;
	int fd = open(path, O_RDONLY);
	ssize_t n = pread(fd, buf, sizeof(buf), (off_t)slot * RC_LOG_RECORD_SIZE);

	close(fd);
	if ((n != (ssize_t)sizeof(buf)) || (rc_log_decode(buf, &r) != 0)) {
		return -1;
	}
	return (long)r.seq;
}

static void append_n(struct logstore *ls, int n)
{
	for (int i = 0; i < n; i++) {
		struct rc_log_record r = snap();

		CHECK(logstore_append(ls, &r) == 0);
	}
}

static void test_new_file(void)
{
	struct logstore ls;
	struct rc_log_record r = snap();
	struct stat st;

	fresh_path();
	CHECK(logstore_open(&ls, path, 8U) == 0);
	CHECK(stat(path, &st) == 0 && st.st_size == 8 * 64);
	CHECK(ls.next_seq == 0U);
	CHECK(logstore_append(&ls, &r) == 0);
	CHECK(r.seq == 0U);
	append_n(&ls, 2);
	CHECK(seq_in_slot(1U) == 1 && seq_in_slot(2U) == 2 && seq_in_slot(3U) == -1);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_wrap_and_resume(void)
{
	struct logstore ls;
	struct rc_log_record r = snap();

	fresh_path();
	CHECK(logstore_open(&ls, path, 4U) == 0);
	append_n(&ls, 6);
	CHECK(seq_in_slot(0U) == 4 && seq_in_slot(1U) == 5 && seq_in_slot(2U) == 2);
	CHECK(logstore_close(&ls) == 0);
	CHECK(logstore_open(&ls, path, 4U) == 0);
	CHECK(ls.next_seq == 6U);
	CHECK(logstore_append(&ls, &r) == 0);
	CHECK(r.seq == 6U && seq_in_slot(2U) == 6);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_corrupt_slot_ignored(void)
{
	struct logstore ls;
	uint8_t junk = 0xFFU;
	int fd;

	fresh_path();
	CHECK(logstore_open(&ls, path, 4U) == 0);
	append_n(&ls, 6); /* slots hold seq 4, 5, 2, 3 */
	CHECK(logstore_close(&ls) == 0);
	fd = open(path, O_WRONLY);
	CHECK(pwrite(fd, &junk, 1U, 64 + 20) == 1); /* damage seq 5 */
	close(fd);
	CHECK(logstore_open(&ls, path, 4U) == 0);
	CHECK(ls.next_seq == 5U);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_short_garbage_file_extended(void)
{
	struct logstore ls;
	struct stat st;
	uint8_t junk[100];
	int fd;

	fresh_path();
	memset(junk, 0xA5, sizeof(junk));
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	CHECK(write(fd, junk, sizeof(junk)) == (ssize_t)sizeof(junk));
	close(fd);
	CHECK(logstore_open(&ls, path, 4U) == 0);
	CHECK(stat(path, &st) == 0 && st.st_size == 4 * 64);
	CHECK(ls.next_seq == 0U);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_open_error_reported(void)
{
	struct logstore ls;

	CHECK(logstore_open(&ls, "/nonexistent-rc-dir/rc-log.bin", 4U) == -ENOENT);
	CHECK(ls.fd == -1);
}

int main(void)
{
	test_new_file();
	test_wrap_and_resume();
	test_corrupt_slot_ignored();
	test_short_garbage_file_extended();
	test_open_error_reported();
	return CHECK_DONE();
}
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_logstore test_logstore.c ${REPO}/central/src/logstore.c ${REPO}/common/logrec.c
	       ${REPO}/common/proto.c)
target_include_directories(test_logstore PRIVATE ${REPO}/common/include ${REPO}/central/src)
add_test(NAME logstore COMMAND test_logstore)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S tests/host -B build/host 2>&1 | grep -m2 -i "cannot find\|logstore.c"`
Expected: CMake reports that `central/src/logstore.c` cannot be found.

- [ ] **Step 3: Implement**

Create `central/src/logstore.h`:

```c
#ifndef LOGSTORE_H_
#define LOGSTORE_H_

#include <stdint.h>

#include "rc/logrec.h"

/* 10 MiB of 64-byte records. */
#define LOGSTORE_SLOTS 163840U

struct logstore {
	int fd;
	uint32_t slots;
	uint32_t next_seq;
};

/*
 * Opens or creates the ring file, extends it to slots records, and
 * continues after the highest valid seq. Returns 0 or -errno.
 */
int logstore_open(struct logstore *ls, const char *path, uint32_t slots);

/* Sets rec->seq and writes the record to slot seq mod slots. Returns 0 or -errno. */
int logstore_append(struct logstore *ls, struct rc_log_record *rec);

int logstore_close(struct logstore *ls);

#endif /* LOGSTORE_H_ */
```

Create `central/src/logstore.c`:

```c
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "logstore.h"

#define SCAN_RECORDS 1024U

static int open_failed(struct logstore *ls)
{
	int err = -errno;

	close(ls->fd);
	ls->fd = -1;
	return err;
}

int logstore_open(struct logstore *ls, const char *path, uint32_t slots)
{
	static uint8_t buf[SCAN_RECORDS * RC_LOG_RECORD_SIZE];
	off_t size = (off_t)slots * RC_LOG_RECORD_SIZE;
	struct stat st;
	bool found = false;
	uint32_t max_seq = 0U;

	ls->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (ls->fd < 0) {
		ls->fd = -1;
		return -errno;
	}
	if (fstat(ls->fd, &st) != 0) {
		return open_failed(ls);
	}
	if ((st.st_size < size) && (ftruncate(ls->fd, size) != 0)) {
		return open_failed(ls);
	}
	for (uint32_t first = 0U; first < slots; first += SCAN_RECORDS) {
		uint32_t n = ((slots - first) < SCAN_RECORDS) ? (slots - first) : SCAN_RECORDS;
		ssize_t got = pread(ls->fd, buf, (size_t)n * RC_LOG_RECORD_SIZE,
				    (off_t)first * RC_LOG_RECORD_SIZE);

		if (got < 0) {
			return open_failed(ls);
		}
		for (size_t i = 0U; ((i + 1U) * RC_LOG_RECORD_SIZE) <= (size_t)got; i++) {
			struct rc_log_record r;

			if ((rc_log_decode(&buf[i * RC_LOG_RECORD_SIZE], &r) == 0) &&
			    (!found || (r.seq > max_seq))) {
				max_seq = r.seq;
				found = true;
			}
		}
	}
	ls->slots = slots;
	ls->next_seq = found ? (max_seq + 1U) : 0U;
	return 0;
}

int logstore_append(struct logstore *ls, struct rc_log_record *rec)
{
	uint8_t buf[RC_LOG_RECORD_SIZE];
	off_t off = (off_t)(ls->next_seq % ls->slots) * RC_LOG_RECORD_SIZE;
	ssize_t w;

	/* The seq advances even if the write fails, so a loss shows as a gap. */
	rec->seq = ls->next_seq++;
	rc_log_encode(rec, buf);
	w = pwrite(ls->fd, buf, sizeof(buf), off);
	if (w < 0) {
		return -errno;
	}
	return ((size_t)w == sizeof(buf)) ? 0 : -EIO;
}

int logstore_close(struct logstore *ls)
{
	int r = close(ls->fd);

	ls->fd = -1;
	return (r == 0) ? 0 : -errno;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 10`.

- [ ] **Step 5: Commit**

```bash
git add central/src/logstore.h central/src/logstore.c tests/host/test_logstore.c tests/host/CMakeLists.txt
git commit -m "feat: add the ring log store" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Python record codec, checked against the C codec

**Files:**
- Create: `common/rclog.py`, `tests/host/logrec_sample.c`, `tests/host/test_rclog.py`, `.gitignore`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: the C codec from Task 2 (through the sample file).
- Produces: `rclog.RECORD_SIZE`, `MAGIC`, `VERSION`, `SNAPSHOT`, `EVENT`, `FIELDS`, `EVENTS` (code -> name), `FLAGS`, `BIT_ITEMS`, `BIT_RESULTS`, `ROLES`, `MODES`, `SLOTS`, `ACTIONS`; `crc16(data)`, `encode(rec) -> bytes`, `decode(buf) -> dict | None`, `decode_file(data) -> (records sorted by seq, invalid count)`, `flags(rec) -> dict`, `bit_results(rec) -> dict`, `detail_text(rec) -> str`, `io_valid(rec) -> bool`.

- [ ] **Step 1: Write the failing test**

Create `tests/host/logrec_sample.c`:

```c
#include <stdio.h>
#include <string.h>

#include "rc/logrec.h"
#include "rc/proto.h"

/* Writes three known records for the Python codec test: logrec_sample FILE */
int main(int argc, char **argv)
{
	struct rc_log_record r[3];
	uint8_t buf[RC_LOG_RECORD_SIZE];
	FILE *f;

	if (argc != 2) {
		fprintf(stderr, "usage: logrec_sample FILE\n");
		return 2;
	}
	memset(r, 0, sizeof(r));
	r[0].type = RC_LOG_SNAPSHOT;
	r[0].seq = 7U;
	r[0].io_time_ms = 123456U;
	r[0].mono_ms = 9876543210ULL;
	r[0].wall_ms = 1790000000123ULL;
	r[0].slot = RC_SLOT_A;
	r[0].role = RC_ROLE_ACTIVE;
	r[0].mode = RC_MODE_TEST;
	r[0].flags = RC_LOG_F_HEALTHY | RC_LOG_F_REFEREE_OK | RC_LOG_F_IO_TIME_VALID;
	r[0].active_slot = RC_SLOT_A;
	r[0].io_fail = 0x01U;
	r[0].bit_results = 0x0350U;
	r[0].step = 513U;
	r[0].mask = 0x55U;
	r[1] = r[0];
	r[1].type = RC_LOG_EVENT;
	r[1].seq = 8U;
	r[1].event = RC_LOG_EV_ROLE_CHANGED;
	r[1].detail = RC_ROLE_STANDBY;
	r[2] = r[0];
	r[2].type = RC_LOG_EVENT;
	r[2].seq = 9U;
	r[2].event = RC_LOG_EV_BIT_ITEM;
	r[2].detail = (3U << 8) | 1U;
	f = fopen(argv[1], "wb");
	if (f == NULL) {
		perror(argv[1]);
		return 1;
	}
	for (int i = 0; i < 3; i++) {
		rc_log_encode(&r[i], buf);
		if (fwrite(buf, 1U, sizeof(buf), f) != sizeof(buf)) {
			perror("write");
			fclose(f);
			return 1;
		}
	}
	if (fclose(f) != 0) {
		perror("close");
		return 1;
	}
	return 0;
}
```

Create `tests/host/test_rclog.py`:

```python
#!/usr/bin/env python3
"""Tests for common/rclog.py, including a log written by the C codec."""
import os
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "common"))
import rclog  # noqa: E402

SAMPLE = os.environ.get("RC_LOG_SAMPLE")


def rec(**kw):
    r = dict(type=rclog.SNAPSHOT, seq=1, io_time_ms=2, mono_ms=3, wall_ms=4, slot=0, role=2,
             mode=0, flags=0b100011, active_slot=0, io_fail=0, bit_results=0, step=5, mask=1,
             event=0, detail=0)
    r.update(kw)
    return r


class RclogTest(unittest.TestCase):
    def test_roundtrip(self):
        r = rec(seq=0x01020304, wall_ms=1790000000123)
        b = rclog.encode(r)
        self.assertEqual(len(b), rclog.RECORD_SIZE)
        self.assertEqual(b[:4], bytes([0x52, 0x4C, 1, rclog.SNAPSHOT]))
        self.assertEqual(rclog.decode(b), r)

    def test_crc_matches_c(self):
        self.assertEqual(rclog.crc16(b"123456789"), 0x29B1)

    def test_flipped_byte_rejected(self):
        b = bytearray(rclog.encode(rec()))
        b[20] ^= 1
        self.assertIsNone(rclog.decode(bytes(b)))

    def test_decode_file_skips_empty_and_counts_bad(self):
        data = (rclog.encode(rec(seq=9)) + bytes(64) + b"\x01" * 64 +
                rclog.encode(rec(seq=3)))
        records, invalid = rclog.decode_file(data)
        self.assertEqual([r["seq"] for r in records], [3, 9])
        self.assertEqual(invalid, 1)

    def test_helpers(self):
        r = rec(bit_results=0x0350, event=12, detail=(3 << 8) | 1, flags=0b100011)
        self.assertEqual(rclog.bit_results(r)["cross_link"], "fail")
        self.assertEqual(rclog.bit_results(r)["cpu_temp"], "not_run")
        self.assertEqual(rclog.detail_text(r), "supply_voltage=fail")
        f = rclog.flags(r)
        self.assertTrue(f["healthy"] and f["referee"] and f["io_time_valid"])
        self.assertFalse(f["peer"])
        self.assertTrue(rclog.io_valid(r))
        self.assertEqual(rclog.detail_text(rec(event=2, detail=1)), "standby")
        self.assertEqual(rclog.detail_text(rec(event=13, detail=(1 << 8) | 0x55)), "leds 0x55")

    @unittest.skipUnless(SAMPLE, "RC_LOG_SAMPLE not set")
    def test_c_written_sample(self):
        with open(SAMPLE, "rb") as f:
            data = f.read()
        records, invalid = rclog.decode_file(data)
        self.assertEqual(invalid, 0)
        self.assertEqual([r["seq"] for r in records], [7, 8, 9])
        r = records[0]
        self.assertEqual(r["io_time_ms"], 123456)
        self.assertEqual(r["mono_ms"], 9876543210)
        self.assertEqual(r["wall_ms"], 1790000000123)
        self.assertEqual((r["slot"], r["role"], r["mode"]), (0, 2, 1))
        self.assertEqual((r["io_fail"], r["step"], r["mask"]), (1, 513, 0x55))
        self.assertEqual(rclog.bit_results(r)["supply_voltage"], "fail")
        self.assertEqual(rclog.EVENTS[records[1]["event"]], "role_changed")
        self.assertEqual(rclog.detail_text(records[1]), "standby")
        self.assertEqual(rclog.detail_text(records[2]), "supply_voltage=fail")
        self.assertEqual(rclog.encode(r), data[:64])


if __name__ == "__main__":
    unittest.main()
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_executable(logrec_sample logrec_sample.c ${REPO}/common/logrec.c ${REPO}/common/proto.c)
target_include_directories(logrec_sample PRIVATE ${REPO}/common/include)
add_test(NAME logrec_sample COMMAND logrec_sample ${CMAKE_CURRENT_BINARY_DIR}/logrec_sample.bin)
set_tests_properties(logrec_sample PROPERTIES FIXTURES_SETUP log_sample)

add_test(NAME rclog COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_rclog.py)
set_tests_properties(rclog PROPERTIES FIXTURES_REQUIRED log_sample
		     ENVIRONMENT RC_LOG_SAMPLE=${CMAKE_CURRENT_BINARY_DIR}/logrec_sample.bin)
```

Create `.gitignore`:

```
logs/
__pycache__/
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tests/host/test_rclog.py 2>&1 | tail -1`
Expected: `ModuleNotFoundError: No module named 'rclog'`.

- [ ] **Step 3: Implement**

Create `common/rclog.py`:

```python
"""Codec for the 64-byte controller log record (see common/include/rc/logrec.h)."""
import binascii
import struct

RECORD_SIZE = 64
MAGIC = 0x4C52
VERSION = 1
SNAPSHOT = 1
EVENT = 2

# magic, version, type, seq, io_time, mono, wall, slot, role, mode, flags,
# active_slot, io_fail, bit_results, step, mask, event, detail, reserve, crc
FMT = "<HBBIIQQBBBBBBHHBBI18xH"
FIELDS = ("type", "seq", "io_time_ms", "mono_ms", "wall_ms", "slot", "role", "mode", "flags",
          "active_slot", "io_fail", "bit_results", "step", "mask", "event", "detail")
EVENTS = {0: "", 1: "started", 2: "role_changed", 3: "healthy", 4: "unhealthy",
          5: "referee_lost", 6: "referee_back", 7: "peer_lost", 8: "peer_back",
          9: "mode_changed", 10: "fault_set", 11: "fault_cleared", 12: "bit_item",
          13: "tcp_output", 14: "slot_learned"}
FLAGS = ("healthy", "referee", "peer", "peer_healthy", "fault", "io_time_valid")
BIT_ITEMS = ("io_link", "loop_timing", "cross_link", "supply_voltage", "cpu_temp")
BIT_RESULTS = ("pass", "fail", "error", "not_run")
ROLES = {0: "unknown", 1: "standby", 2: "active"}
MODES = {0: "operational", 1: "test"}
SLOTS = {0: "A", 1: "B"}
ACTIONS = {1: "leds", 2: "lamp_test", 3: "run_bit"}


def crc16(data):
    """CRC-16/CCITT-FALSE, the same as rc_crc16()."""
    return binascii.crc_hqx(data, 0xFFFF)


def encode(rec):
    raw = struct.pack(FMT, MAGIC, VERSION, *(rec[k] for k in FIELDS), 0)
    return raw[:62] + struct.pack("<H", crc16(raw[:62]))


def decode(buf):
    """Returns the record as a dict, or None for a wrong size, magic, version, type, or CRC."""
    if len(buf) != RECORD_SIZE:
        return None
    v = struct.unpack(FMT, buf)
    if v[0] != MAGIC or v[1] != VERSION or v[2] not in (SNAPSHOT, EVENT):
        return None
    if v[18] != crc16(buf[:62]):
        return None
    return dict(zip(FIELDS, v[2:18]))


def decode_file(data):
    """Returns (valid records sorted by seq, number of invalid slots that are not empty)."""
    records, invalid = [], 0
    empty = bytes(RECORD_SIZE)
    for off in range(0, len(data) - RECORD_SIZE + 1, RECORD_SIZE):
        chunk = data[off:off + RECORD_SIZE]
        rec = decode(chunk)
        if rec is not None:
            records.append(rec)
        elif chunk != empty:
            invalid += 1
    records.sort(key=lambda r: r["seq"])
    return records, invalid


def flags(rec):
    return {name: bool((rec["flags"] >> i) & 1) for i, name in enumerate(FLAGS)}


def io_valid(rec):
    return bool((rec["flags"] >> FLAGS.index("io_time_valid")) & 1)


def bit_results(rec):
    return {name: BIT_RESULTS[(rec["bit_results"] >> (2 * i)) & 3]
            for i, name in enumerate(BIT_ITEMS)}


def detail_text(rec):
    event, d = EVENTS.get(rec["event"], ""), rec["detail"]
    if event == "role_changed":
        return ROLES.get(d, str(d))
    if event == "mode_changed":
        return MODES.get(d, str(d))
    if event == "slot_learned":
        return SLOTS.get(d, str(d))
    if event == "bit_item":
        item = d >> 8
        name = BIT_ITEMS[item] if item < len(BIT_ITEMS) else str(item)
        return "%s=%s" % (name, BIT_RESULTS[d & 3])
    if event == "tcp_output":
        action = ACTIONS.get(d >> 8, str(d >> 8))
        return "leds 0x%02x" % (d & 0xFF) if action == "leds" else action
    return ""
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 12`, and `git status --short` shows no `__pycache__`.

- [ ] **Step 5: Commit**

```bash
git add common/rclog.py tests/host/logrec_sample.c tests/host/test_rclog.py tests/host/CMakeLists.txt .gitignore
git commit -m "feat: add the Python log record codec" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Log server

**Files:**
- Create: `central/rc_logd.py`, `central/rc-logd.service`, `tests/host/test_rc_logd.py`
- Modify: `tests/host/CMakeLists.txt`, `tools/deploy-central.sh`

**Interfaces:**
- Consumes: `rclog.decode_file` from Task 5.
- Produces: `rc_logd.make_server(path, port, host="") -> ThreadingHTTPServer`, `rc_logd.info(data) -> dict`, `rc_logd.flush_forever(path, stop)`; HTTP `GET /log`, `GET /info`.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_rc_logd.py`:

```python
#!/usr/bin/env python3
"""Tests for central/rc_logd.py against a temporary log file."""
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

sys.dont_write_bytecode = True
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "common"))
sys.path.insert(0, os.path.join(ROOT, "central"))
import rc_logd  # noqa: E402
import rclog  # noqa: E402


def rec(seq):
    return dict(type=rclog.SNAPSHOT, seq=seq, io_time_ms=seq, mono_ms=seq, wall_ms=seq, slot=0,
                role=2, mode=0, flags=0x20, active_slot=0, io_fail=0, bit_results=0, step=0,
                mask=1, event=0, detail=0)


class LogdTest(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp()
        os.close(fd)
        with open(self.path, "wb") as f:
            f.write(rclog.encode(rec(4)) + rclog.encode(rec(5)) + bytes(64) +
                    rclog.encode(rec(6)))
        self.server = rc_logd.make_server(self.path, 0, "127.0.0.1")
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        if os.path.exists(self.path):
            os.unlink(self.path)

    def test_info(self):
        with urllib.request.urlopen(self.base + "/info") as r:
            self.assertEqual(json.load(r), {"size": 256, "records": 3, "max_seq": 6})

    def test_log_is_the_file(self):
        with urllib.request.urlopen(self.base + "/log") as r, open(self.path, "rb") as f:
            self.assertEqual(r.read(), f.read())

    def test_unknown_path(self):
        with self.assertRaises(urllib.error.HTTPError) as c:
            urllib.request.urlopen(self.base + "/other")
        self.assertEqual(c.exception.code, 404)

    def test_missing_file(self):
        os.unlink(self.path)
        with self.assertRaises(urllib.error.HTTPError) as c:
            urllib.request.urlopen(self.base + "/log")
        self.assertEqual(c.exception.code, 503)

    def test_flush_runs_and_stops(self):
        stop = threading.Event()
        t = threading.Thread(target=rc_logd.flush_forever, args=(self.path, stop, 0.01))
        t.start()
        stop.wait(0.05)
        stop.set()
        t.join(1)
        self.assertFalse(t.is_alive())


if __name__ == "__main__":
    unittest.main()
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_test(NAME rc_logd COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_rc_logd.py)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tests/host/test_rc_logd.py 2>&1 | tail -1`
Expected: `ModuleNotFoundError: No module named 'rc_logd'`.

- [ ] **Step 3: Implement**

Create `central/rc_logd.py` (mode 755):

```python
#!/usr/bin/env python3
"""Serves the controller's ring log over HTTP and flushes it to disk.

Usage: rc_logd.py [--path FILE] [--port N]
GET /log returns the raw file; GET /info returns size, valid records, and max seq.
"""
import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "common"))
import rclog  # noqa: E402

DEFAULT_PATH = "/var/lib/rc-central/rc-log.bin"
DEFAULT_PORT = 8080
FLUSH_S = 2.0


def info(data):
    records, _ = rclog.decode_file(data)
    return {"size": len(data), "records": len(records),
            "max_seq": records[-1]["seq"] if records else None}


def make_handler(path):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path not in ("/log", "/info"):
                self.send_error(404)
                return
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError as e:
                self.send_error(503, "log unavailable: %s" % e.strerror)
                return
            if self.path == "/log":
                body, ctype = data, "application/octet-stream"
            else:
                body, ctype = json.dumps(info(data)).encode(), "application/json"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            sys.stderr.write("rc-logd: %s %s\n" % (self.address_string(), fmt % args))

    return Handler


def make_server(path, port, host=""):
    return ThreadingHTTPServer((host, port), make_handler(path))


def flush_forever(path, stop, period=FLUSH_S):
    """fsync on any handle writes out the daemon's cached records too."""
    failed = False
    while not stop.wait(period):
        try:
            fd = os.open(path, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            if failed:
                sys.stderr.write("rc-logd: flush ok\n")
                failed = False
        except OSError as e:
            if not failed:
                sys.stderr.write("rc-logd: flush failed: %s\n" % e)
                failed = True


def main(argv=None):
    p = argparse.ArgumentParser(description="Controller log server")
    p.add_argument("--path", default=DEFAULT_PATH)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = p.parse_args(argv)
    stop = threading.Event()
    threading.Thread(target=flush_forever, args=(args.path, stop), daemon=True).start()
    server = make_server(args.path, args.port)
    sys.stderr.write("rc-logd: serving %s on port %d\n" % (args.path, args.port))
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Create `central/rc-logd.service`:

```
[Unit]
Description=Redundant controller log server
After=rc-central.service

[Service]
ExecStart=/usr/bin/python3 -B /usr/local/lib/rc-central/rc_logd.py
Restart=always
RestartSec=2
Nice=10
IOSchedulingClass=idle

[Install]
WantedBy=multi-user.target
```

In `tools/deploy-central.sh`, add inside the remote script after the `sudo systemctl restart rc-central.service` line:

```
    sudo install -D -m 644 rc-src/common/rclog.py /usr/local/lib/rc-central/rclog.py
    sudo install -D -m 755 rc-src/central/rc_logd.py /usr/local/lib/rc-central/rc_logd.py
    sudo install -m 644 rc-src/central/rc-logd.service /etc/systemd/system/rc-logd.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now rc-logd.service
    sudo systemctl restart rc-logd.service
```

and change the final echo to:

```
    echo "$(hostname): rc-central $(systemctl is-active rc-central), rc-logd $(systemctl is-active rc-logd)"'
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 13`.

- [ ] **Step 5: Commit**

```bash
git add central/rc_logd.py central/rc-logd.service tests/host/test_rc_logd.py tests/host/CMakeLists.txt tools/deploy-central.sh
git commit -m "feat: add the log server" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Daemon logging

**Files:**
- Modify: `central/src/main.c`, `central/CMakeLists.txt`, `central/rc-central.service`

**Interfaces:**
- Consumes: Tasks 2, 3, 4.
- Produces: records in `/var/lib/rc-central/rc-log.bin`; console events `log_open`, `log_write_failed`, `log_write_ok`.

- [ ] **Step 1: Build files**

In `central/CMakeLists.txt`, change the executable line to:

```cmake
add_executable(rc-central src/main.c src/role.c src/bit.c src/cmd.c src/logstore.c
	       ../common/proto.c ../common/logrec.c)
```

In `central/rc-central.service`, add under `[Service]`:

```
StateDirectory=rc-central
```

- [ ] **Step 2: Daemon changes**

In `central/src/main.c`:

Add after `#include "cmd.h"`:

```c
#include "logstore.h"
#include "rc/logrec.h"
```

Add after `#define RUN_BIT_MIN_INTERVAL_MS 500`:

```c
#define LOG_PATH "/var/lib/rc-central/rc-log.bin"
#define LOG_SNAPSHOT_PERIOD_MS 100
```

Add at the end of `struct daemon`:

```c
	struct logstore log;
	bool log_open;
	bool log_failed;
```

Add after `now_ms()`:

```c
static int64_t wall_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}
```

Add after `send_run_bit()`:

```c
/* Appends one record; logging never stops control, so errors are only reported. */
static void log_record(struct daemon *d, int64_t t, uint8_t type, uint8_t event, uint32_t detail)
{
	struct rc_log_record r;
	uint8_t bits[BIT_ITEMS];
	uint32_t io_time = 0U;
	bool io_valid = role_io_time(&d->s, t, &io_time);
	int err;

	if (!d->log_open) {
		return;
	}
	for (int i = 0; i < BIT_ITEMS; i++) {
		bits[i] = d->cbit.valid ? (uint8_t)d->cbit.result[i] : RC_LOG_BIT_NOT_RUN;
	}
	memset(&r, 0, sizeof(r));
	r.type = type;
	r.io_time_ms = io_time;
	r.mono_ms = (uint64_t)t;
	r.wall_ms = (uint64_t)wall_now_ms();
	r.slot = d->s.slot;
	r.role = d->s.role;
	r.mode = d->s.mode;
	r.flags = (uint8_t)((d->health.healthy ? RC_LOG_F_HEALTHY : 0U) |
			    (d->s.referee_ok ? RC_LOG_F_REFEREE_OK : 0U) |
			    (d->s.peer_ok ? RC_LOG_F_PEER_OK : 0U) |
			    (d->s.peer_healthy ? RC_LOG_F_PEER_HEALTHY : 0U) |
			    (d->s.fault ? RC_LOG_F_FAULT : 0U) |
			    (io_valid ? RC_LOG_F_IO_TIME_VALID : 0U));
	r.active_slot = d->s.active_slot;
	r.io_fail = d->s.io_fail;
	r.bit_results = rc_log_pack_bits(bits, BIT_ITEMS);
	r.step = d->s.step;
	r.mask = role_current_mask(&d->s);
	r.event = event;
	r.detail = detail;
	err = logstore_append(&d->log, &r);
	if ((err != 0) && !d->log_failed) {
		printf("t=%lld event=log_write_failed error=\"%s\"\n", (long long)t, strerror(-err));
		d->log_failed = true;
	} else if ((err == 0) && d->log_failed) {
		printf("t=%lld event=log_write_ok\n", (long long)t);
		d->log_failed = false;
	}
}

static void log_role_events(struct daemon *d, unsigned ev, int64_t t)
{
	static const struct {
		unsigned bit;
		uint8_t event;
	} map[] = {
		{ROLE_EV_SLOT_LEARNED, RC_LOG_EV_SLOT_LEARNED},
		{ROLE_EV_ROLE_CHANGED, RC_LOG_EV_ROLE_CHANGED},
		{ROLE_EV_REFEREE_LOST, RC_LOG_EV_REFEREE_LOST},
		{ROLE_EV_REFEREE_BACK, RC_LOG_EV_REFEREE_BACK},
		{ROLE_EV_PEER_LOST, RC_LOG_EV_PEER_LOST},
		{ROLE_EV_PEER_BACK, RC_LOG_EV_PEER_BACK},
		{ROLE_EV_FAULT_SET, RC_LOG_EV_FAULT_SET},
		{ROLE_EV_FAULT_CLEARED, RC_LOG_EV_FAULT_CLEARED},
		{ROLE_EV_MODE_CHANGED, RC_LOG_EV_MODE_CHANGED},
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		uint32_t detail = 0U;

		if ((ev & map[i].bit) == 0U) {
			continue;
		}
		if (map[i].event == RC_LOG_EV_SLOT_LEARNED) {
			detail = d->s.slot;
		} else if (map[i].event == RC_LOG_EV_ROLE_CHANGED) {
			detail = d->s.role;
		} else if (map[i].event == RC_LOG_EV_MODE_CHANGED) {
			detail = d->s.mode;
		}
		log_record(d, t, RC_LOG_EVENT, map[i].event, detail);
	}
}
```

In `run_bit`, inside the loop that prints `event=bit item=...`, add after the `printf`:

```c
			log_record(d, t, RC_LOG_EVENT, RC_LOG_EV_BIT_ITEM,
				   ((uint32_t)i << 8) | (uint32_t)d->cbit.result[i]);
```

and inside the block that prints `healthy`/`unhealthy`, add after its `printf`:

```c
		log_record(d, t, RC_LOG_EVENT,
			   d->health.healthy ? RC_LOG_EV_HEALTHY : RC_LOG_EV_UNHEALTHY, 0U);
```

In `handle_line`, inside `if (res.action != CMD_ACT_NONE) {`, add after the `printf`:

```c
		log_record(d, t, RC_LOG_EVENT, RC_LOG_EV_TCP_OUTPUT,
			   ((uint32_t)res.action << 8) | res.leds);
```

In `main`, add `int64_t next_snap;` and `int err;` to the declarations, and after `bit_health_init(&d.health);`:

```c
	err = logstore_open(&d.log, LOG_PATH, LOGSTORE_SLOTS);
	if (err != 0) {
		fprintf(stderr, "error: log disabled: %s: %s\n", LOG_PATH, strerror(-err));
	} else {
		d.log_open = true;
		printf("t=%lld event=log_open next_seq=%lu\n", (long long)start,
		       (unsigned long)d.log.next_seq);
	}
	log_record(&d, start, RC_LOG_EVENT, RC_LOG_EV_STARTED, 0U);
	next_snap = start;
```

In the loop, change

```c
		log_events(ev, &d.s, t);
```

to

```c
		log_events(ev, &d.s, t);
		log_role_events(&d, ev, t);
		if (t >= next_snap) {
			log_record(&d, t, RC_LOG_SNAPSHOT, RC_LOG_EV_NONE, 0U);
			next_snap += LOG_SNAPSHOT_PERIOD_MS;
			if (next_snap <= t) {
				next_snap = t + LOG_SNAPSHOT_PERIOD_MS;
			}
		}
```

- [ ] **Step 3: Host tests still pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 13`. (`central/src/main.c` is Linux-only; it is built on the Pis in Task 10.)

- [ ] **Step 4: Commit**

```bash
git add central
git commit -m "feat: write snapshots and events to the ring log" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Viewer data functions

**Files:**
- Create: `tools/rcview.py` (data functions and constants only in this task), `tests/host/test_rcview.py`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `rclog` from Task 5.
- Produces: `COLUMNS`; `controller_name(records, fallback)`; `split_segments(records) -> [[rec]]`; `boot_time(segment) -> int`; `merge(logs) -> [{"boot_wall", "controllers", "rows": [(name, rec)]}]`; `filter_rows(rows, ctl=None, kind="events", events=None, io_from=None, io_to=None)`; `row_values(name, rec) -> list`; `to_csv(rows) -> str`; `timeline(rows) -> {"lanes", "markers"}`.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_rcview.py`:

```python
#!/usr/bin/env python3
"""Tests for tools/rcview.py: segments, merge, filters, CSV, timeline, and the API."""
import csv
import io
import json
import os
import sys
import threading
import unittest
import urllib.request

sys.dont_write_bytecode = True
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rcview  # noqa: E402
import rclog  # noqa: E402

BOOT = 1_790_000_000_000


def rec(seq, io, slot=0, role=2, kind=rclog.SNAPSHOT, event=0, detail=0, boot=BOOT, valid=True):
    return dict(type=kind, seq=seq, io_time_ms=io if valid else 0, mono_ms=seq,
                wall_ms=boot + io, slot=slot, role=role, mode=0,
                flags=(0x20 if valid else 0) | 0x01, active_slot=0, io_fail=0,
                bit_results=0, step=seq, mask=1, event=event, detail=detail)


def ev(seq, io, event, detail=0, slot=0, role=2, boot=BOOT):
    return rec(seq, io, slot=slot, role=role, kind=rclog.EVENT, event=event, detail=detail,
               boot=boot)


class DataTest(unittest.TestCase):
    def test_split_on_io_card_restart(self):
        segs = rcview.split_segments([rec(1, 1000), rec(2, 1100), rec(3, 50), rec(4, 150)])
        self.assertEqual([[r["seq"] for r in s] for s in segs], [[1, 2], [3, 4]])

    def test_records_without_io_time_join_next_segment(self):
        segs = rcview.split_segments([rec(1, 0, valid=False), rec(2, 500), rec(3, 600),
                                      rec(4, 0, valid=False)])
        self.assertEqual([[r["seq"] for r in s] for s in segs], [[1, 2, 3, 4]])

    def test_merge_interleaves_by_io_time(self):
        logs = {"A": [rec(1, 100), rec(2, 300)],
                "B": [rec(10, 200, slot=1), rec(11, 400, slot=1)]}
        segs = rcview.merge(logs)
        self.assertEqual(len(segs), 1)
        self.assertEqual([(n, r["seq"]) for n, r in segs[0]["rows"]],
                         [("A", 1), ("B", 10), ("A", 2), ("B", 11)])
        self.assertEqual(sorted(segs[0]["controllers"]), ["A", "B"])

    def test_merge_tolerates_small_clock_offset(self):
        logs = {"A": [rec(1, 100)], "B": [rec(10, 200, slot=1, boot=BOOT + 3000)]}
        self.assertEqual(len(rcview.merge(logs)), 1)

    def test_merge_matches_logs_that_start_at_different_times(self):
        # B's log starts an hour later but in the same I/O card run.
        logs = {"A": [rec(1, 100), rec(2, 3_600_100)], "B": [rec(10, 3_600_200, slot=1)]}
        self.assertEqual(len(rcview.merge(logs)), 1)

    def test_merge_separates_io_card_runs(self):
        logs = {"A": [rec(1, 100)], "B": [rec(10, 200, slot=1, boot=BOOT + 60_000)]}
        segs = rcview.merge(logs)
        self.assertEqual(len(segs), 2)
        self.assertLess(segs[0]["boot_wall"], segs[1]["boot_wall"])

    def test_controller_name(self):
        self.assertEqual(rcview.controller_name([rec(1, 1, slot=1)], "x.bin"), "B")
        self.assertEqual(rcview.controller_name([], "x.bin"), "x.bin")

    def test_filter_rows(self):
        rows = [("A", rec(1, 100)), ("A", ev(2, 150, 5)), ("B", ev(3, 200, 2, 1, slot=1)),
                ("B", rec(4, 300, slot=1))]
        self.assertEqual(len(rcview.filter_rows(rows)), 2)  # events by default
        self.assertEqual(len(rcview.filter_rows(rows, kind="all")), 4)
        self.assertEqual(len(rcview.filter_rows(rows, kind="snapshots")), 2)
        self.assertEqual(len(rcview.filter_rows(rows, kind="all", ctl={"B"})), 2)
        self.assertEqual(len(rcview.filter_rows(rows, events={"role_changed"})), 1)
        self.assertEqual(len(rcview.filter_rows(rows, kind="all", io_from=150, io_to=200)), 2)

    def test_csv(self):
        text = rcview.to_csv([("B", ev(3, 200, 2, 1, slot=1))])
        lines = list(csv.reader(io.StringIO(text)))
        self.assertEqual(lines[0], rcview.COLUMNS)
        row = dict(zip(lines[0], lines[1]))
        self.assertEqual((row["controller"], row["event"], row["detail"]),
                         ("B", "role_changed", "standby"))
        self.assertEqual(row["io_time_ms"], "200")

    def test_timeline(self):
        rows = [("A", rec(1, 100)), ("B", rec(2, 100, slot=1, role=1)),
                ("A", rec(3, 200)), ("A", ev(4, 250, 2, 1, role=1)), ("A", rec(5, 300, role=1)),
                ("A", rec(6, 5000, role=1))]
        tl = rcview.timeline(rows)
        self.assertEqual(tl["lanes"]["A"], [[100, 200, "active"], [250, 300, "standby"],
                                            [5000, 5000, "standby"]])
        self.assertEqual(tl["lanes"]["B"], [[100, 100, "standby"]])
        self.assertEqual(tl["markers"], [[250, "A", "role_changed"]])


if __name__ == "__main__":
    unittest.main()
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_test(NAME rcview COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_rcview.py)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tests/host/test_rcview.py 2>&1 | tail -1`
Expected: `ModuleNotFoundError: No module named 'rcview'`.

- [ ] **Step 3: Implement**

Create `tools/rcview.py` (mode 755) with the data part:

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 14`.

- [ ] **Step 5: Commit**

```bash
git add tools/rcview.py tests/host/test_rcview.py tests/host/CMakeLists.txt
git commit -m "feat: add log merge, filter, CSV, and timeline for the viewer" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: Viewer server and page

**Files:**
- Modify: `tools/rcview.py`, `tests/host/test_rcview.py`

**Interfaces:**
- Consumes: Task 8 functions.
- Produces: `PAGE_SIZE` (500); `parse_filter(qs) -> dict`; `make_server(segments, port, host="127.0.0.1")`; `download(hosts, save_dir) -> {path: bytes}`; `main(argv)`. HTTP: `GET /`, `/api/segments`, `/api/rows`, `/api/timeline`, `/api/csv`.

- [ ] **Step 1: Write the failing test**

Add to `tests/host/test_rcview.py` before `if __name__ == "__main__":`:

```python
class ApiTest(unittest.TestCase):
    def setUp(self):
        logs = {"A": [rec(1, 100), ev(2, 150, 5), rec(3, 200)],
                "B": [rec(10, 120, slot=1, role=1), ev(11, 160, 2, 2, slot=1)]}
        self.server = rcview.make_server(rcview.merge(logs), 0)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def get(self, path):
        with urllib.request.urlopen(self.base + path) as r:
            return r.headers.get_content_type(), r.read().decode()

    def test_page(self):
        ctype, body = self.get("/")
        self.assertEqual(ctype, "text/html")
        self.assertIn("/api/rows", body)
        self.assertNotIn("__EVENTS__", body)

    def test_segments(self):
        segs = json.loads(self.get("/api/segments")[1])
        self.assertEqual(len(segs), 1)
        self.assertEqual((segs[0]["rows"], segs[0]["io_from"], segs[0]["io_to"]), (5, 100, 200))

    def test_rows_filter_and_page(self):
        d = json.loads(self.get("/api/rows?seg=0&kind=all&ctl=A&page=0")[1])
        self.assertEqual(d["total"], 3)
        self.assertEqual(d["columns"], rcview.COLUMNS)
        self.assertEqual([r[1] for r in d["rows"]], [1, 2, 3])
        d = json.loads(self.get("/api/rows?seg=0")[1])  # events only by default
        self.assertEqual([r[3] for r in d["rows"]], ["referee_lost", "role_changed"])

    def test_timeline(self):
        tl = json.loads(self.get("/api/timeline?seg=0")[1])
        self.assertEqual(tl["markers"], [[150, "A", "referee_lost"], [160, "B", "role_changed"]])

    def test_csv(self):
        ctype, body = self.get("/api/csv?seg=0&kind=all")
        self.assertEqual(ctype, "text/csv")
        self.assertEqual(len(body.strip().splitlines()), 1 + 5)

    def test_bad_request(self):
        with self.assertRaises(urllib.error.HTTPError) as c:
            urllib.request.urlopen(self.base + "/api/rows?seg=9")
        self.assertEqual(c.exception.code, 400)
```

and add `import urllib.error` to the imports.

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tests/host/test_rcview.py 2>&1 | tail -3`
Expected: errors such as `AttributeError: module 'rcview' has no attribute 'make_server'`.

- [ ] **Step 3: Implement**

In `tools/rcview.py`, extend the imports to:

```python
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
```

add constants after `COLUMNS`:

```python
LOG_PORT = 8080
VIEW_PORT = 8765
DEFAULT_HOSTS = "192.168.45.50,192.168.45.176"
PAGE_SIZE = 500
```

and append to the end of the file:

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 14`.

- [ ] **Step 5: Commit**

```bash
git add tools/rcview.py tests/host/test_rcview.py
git commit -m "feat: add the viewer page and API" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 10: Deploy, experiments, and docs

**Files:**
- Modify: `docs/test-log.md`, `README.md`

**Precondition:** owner at the bench for experiments 2 and 3.

- [ ] **Step 1: Flash and deploy**

Run: `cd ~/ws/stm32f3-ws && . ~/ws/zp/zephyr-venv/bin/activate && west build -b stm32f3_disco@E -d build/rc-io-card ~/ws/redundant-controller/io-card >/dev/null && cd ~/ws/redundant-controller && tools/flash-remote.sh ~/ws/stm32f3-ws/build/rc-io-card/zephyr/zephyr.hex && tools/deploy-central.sh 2>&1 | grep -E "error|warning|rc-central "`
Expected: `Verified OK`; both Pis build without warnings and print `rc-central active, rc-logd active`.

Run: `for h in 192.168.45.50 192.168.45.176; do curl -s http://$h:8080/info; echo; done`
Expected: JSON with `size` 10485760 and a growing `records` count on each Pi.

- [ ] **Step 2: Experiment 1, logging load**

Wait 10 minutes. Then run `tools/rcview.py --no-browser --save logs` in the background for a few seconds and stop it, and decode the saved files:

Run: `python3 -c "import sys; sys.path.insert(0,'common'); import rclog, glob; [print(p, (lambda r: (len([x for x in r[0] if x['type']==1]), r[1], [b['seq']-a['seq'] for a,b in zip(r[0],r[0][1:]) if b['seq']-a['seq']!=1][:5]))(rclog.decode_file(open(p,'rb').read()))) for p in sorted(glob.glob('logs/*.bin'))[-2:]]"`
Expected per file: snapshot count about 10 per second of uptime since the deploy, 0 invalid slots, no seq gaps (`[]`). Also `journalctl -u rc-central | grep loop_timing` shows no fail.

- [ ] **Step 3: Experiment 2, failover in the viewer**

Start `tools/console-remote.sh 60` in the background, ask the owner to pull the Active's RX wire for about 10 s and reconnect it, then run `tools/rcview.py`.
Pass: in the viewer's default events view, the old Active shows `referee_lost`, `bit_item io_link=fail`, `unhealthy`; the other controller shows `role_changed active`; the I/O card time of that `role_changed` is within 5 ms of the console's `active:` line time plus the grant path (compare `t=` on the console with `io_time_ms`). Take a browser screenshot of the timeline and table around the handover for the post.

- [ ] **Step 4: Experiment 3, power loss**

Ask the owner to pull the Active's power and reconnect it after about 30 s. After it is back, run `tools/rcview.py`.
Pass: the pulled controller's last record before its `started` event has an I/O card time at most about 2000 ms before the other controller's `role_changed` to active.

- [ ] **Step 5: Experiment 4, download under load**

Run: `for i in 1 2 3 4 5; do for h in 192.168.45.50 192.168.45.176; do curl -s -o /dev/null -w "%{size_download} %{time_total}\n" http://$h:8080/log; done; done`
Then check both journals for the last 2 minutes.
Pass: ten downloads of 10485760 bytes; no `loop_timing result=fail`, no `role_changed`.

- [ ] **Step 6: Write the results**

Append a "Sub-project 3" section to `docs/test-log.md`: date, commits, the under-voltage note, and one table per experiment with observed values and pass/fail.

In `README.md`, add after the "Built-in test and commands" section:

````markdown
## Log and viewer

Each controller writes a status snapshot every 100 ms and an event record
at each event into a 10 MiB ring file (`/var/lib/rc-central/rc-log.bin`,
about 4.5 hours). Records carry the I/O card's clock, so both controllers'
logs merge on one timeline. `rc-logd` serves the file on port 8080.

```sh
tools/rcview.py                       # download both logs, open the viewer
tools/rcview.py --open logs/*.bin     # view saved logs
```

The viewer runs at http://127.0.0.1:8765: role timeline, filters, and CSV
export of the filtered rows.
````

- [ ] **Step 7: Commit**

```bash
git add docs/test-log.md README.md
git commit -m "docs: record logging and viewer experiments" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```
