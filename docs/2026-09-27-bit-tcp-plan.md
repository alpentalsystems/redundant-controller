# Built-in Test and TCP Commands Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add PBIT/CBIT on both controllers and the I/O card, health-driven failover through the referee, a button-controlled test mode, and a TCP command server with a Python client.

**Architecture:** Pure C modules (arbiter, I/O card BIT, button debounce, controller BIT, role logic, command handler) are written test-first on the host. Thin platform layers (`io-card/src/main.c` on Zephyr, `central/src/main.c` on Linux) feed them readings and time and carry out their decisions. The UART and cross-link frames gain health, mode, BIT, and test-mask fields.

**Tech Stack:** C11, Zephyr 4.4.2 (STM32F3 Discovery rev E), Linux (Raspberry Pi OS, poll loop, BSD sockets), CMake/CTest host tests, Python 3 stdlib client.

**Spec:** `docs/2026-09-27-bit-tcp-design.md` (sub-project 1: `docs/2026-09-27-redundancy-core-design.md`)

## Global Constraints

- Zephyr v4.4.2, board target `stm32f3_disco@E`, flashing with OpenOCD.
- C11 with `-Wall -Wextra -Werror` everywhere; kernel-style tabs as in the existing code.
- Comments in English, short, plain ASCII.
- No customer names, part numbers, or contract details in code, docs, or commit messages.
- TCP port 5000, up to 4 clients, lines up to 128 bytes, text commands, one-line JSON replies.
- The mode changes only with the USER button; never over TCP.
- The I/O card makes every role decision; controllers never step down on their own.
- Controller BIT period 1 s, PBIT 1 s after start; recovery after 3 consecutive passes.
- Both Pis are under-voltage on the current bench; results carry that note.

## Design refinements (found while planning; approve with this plan)

1. **Health handover hold (1.2 s).** When the I/O card restarts or stalls, both controllers lose STATUS, both turn unhealthy, and they recover up to one BIT period (1 s) apart. Without a hold, whichever recovers first would take Active, so an I/O card restart would change roles (a regression of sub-project 1, experiment 4). The I/O card therefore moves Active for health only when "Active unhealthy and the other present and healthy" has held for 1.2 s. Handover target becomes **under 2.5 s** from the fault (up to 1.1 s to detect, 1.2 s hold, one heartbeat).
2. **Election order.** Reported Active and healthy (A first), then healthy (A first), then reported Active (A first), then present (A first). The third step keeps the previous Active after an I/O card restart while both controllers are still unhealthy.
3. **Supply voltage source.** The daemon reads `in0_lcrit_alarm` of the `rpi_volt` hwmon device (the kernel's view of the same under-voltage flag as `vcgencmd get_throttled` bit 0). Running `vcgencmd` would block the real-time loop for tens of milliseconds.
4. **RUN_BIT reply.** `RUN_BIT` replies `{"ok":true}` at once; the client then asks `BIT`. `rcctl run-bit` does both.
5. **Experiment 5 (regression).** Hold the STM32 in reset for 3 s while the system runs: no role change, both controllers healthy again afterwards.

Task 0 writes these into the spec.

## Review Focus

1. **Common-mode referee loss:** an I/O card restart or a stall of a few seconds must not move Active, even though both controllers turn unhealthy and recover at different times (Task 3 test, experiment 5).
2. **Misbehaving TCP clients:** a client that stops reading, sends an endless line, or is the fifth connection must never delay heartbeats; the loop stays non-blocking and drops only that client (Task 10 code, Task 8 tests for the line limit).
3. **sysfs reads failing:** a missing `rpi_volt` device or thermal zone gives item result `error`, never a crash or a stuck loop (Task 6 tests, Task 10 code).
4. **Failover in test mode:** the new Active keeps the operator mask and does not restart the chaser (Task 7 test, experiment 3).
5. **RUN_BIT from a controller that is not Active** is ignored by the I/O card (Task 9 code, checked on the bench in Task 9).

---

### Task 0: Spec refinements

**Files:**
- Modify: `docs/2026-09-27-bit-tcp-design.md`

- [ ] **Step 1: Apply the five refinements to the spec**

In `docs/2026-09-27-bit-tcp-design.md`:

Replace the "Arbitration changes" table and the timing paragraph under it with:

```markdown
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
```

In the controller BIT table, replace the supply voltage row with:

```markdown
| 3 | Supply voltage | `in0_lcrit_alarm` of the `rpi_volt` hwmon device is 0 (same flag as `vcgencmd get_throttled` bit 0, read without blocking) | no |
```

In the TCP table, replace the `RUN_BIT` row's test-mode cell with `runs controller BIT now, sends RUN_BIT, replies ok; the client then reads BIT`.

In Verification, change experiment 2's pass condition from "within 1.5 s" to "within 2.5 s", and add after experiment 4:

```markdown
5. **I/O card restart (regression):** hold the STM32 in reset for 3 s
   with OpenOCD while the system runs. Pass: no role change; both
   controllers report unhealthy, then healthy again.
```

- [ ] **Step 2: Commit**

```bash
git add docs/2026-09-27-bit-tcp-design.md
git commit -m "docs: refine BIT handover timing and election order" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 1: Protocol fields and RUN_BIT

**Files:**
- Modify: `common/include/rc/proto.h`, `common/proto.c`, `io-card/src/main.c:88-96`
- Test: `tests/host/test_proto.c`

**Interfaces:**
- Produces: `RC_MSG_RUN_BIT` (5), `RC_MODE_OPERATIONAL` (0), `RC_MODE_TEST` (1); new fields `rc_heartbeat.healthy`, `rc_status.mode`, `rc_status.io_fail`, `rc_peer.healthy`, `rc_peer.test_mask` (all `uint8_t`); `size_t rc_encode_run_bit(uint8_t *out, size_t out_size)`; `int rc_decode_run_bit(const struct rc_frame *f)` (0 or -1). Payload lengths: HEARTBEAT 6, STATUS 9, PEER 11, RUN_BIT 0.

- [ ] **Step 1: Write the failing tests**

In `tests/host/test_proto.c`, replace `test_heartbeat_roundtrip`, `test_status_roundtrip`, and `test_peer_roundtrip` with:

```c
static void test_heartbeat_roundtrip(void)
{
	struct rc_heartbeat in = {.seq = 0x01020304U, .role = RC_ROLE_ACTIVE, .healthy = 1U};
	struct rc_heartbeat out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_heartbeat(&in, buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD + 6U);
	CHECK(buf[3] == 0x04U); /* little-endian seq */
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_heartbeat(&f, &out) == 0);
	CHECK(out.seq == in.seq && out.role == in.role && out.healthy == 1U);
}

static void test_status_roundtrip(void)
{
	struct rc_status in = {.seq = 7U, .slot = RC_SLOT_B, .granted_role = RC_ROLE_STANDBY,
			       .active_slot = RC_SLOT_A, .mode = RC_MODE_TEST, .io_fail = 0x21U};
	struct rc_status out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_status(&in, buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD + 9U);
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_status(&f, &out) == 0);
	CHECK(out.seq == 7U && out.slot == RC_SLOT_B);
	CHECK(out.granted_role == RC_ROLE_STANDBY && out.active_slot == RC_SLOT_A);
	CHECK(out.mode == RC_MODE_TEST && out.io_fail == 0x21U);
}

static void test_peer_roundtrip(void)
{
	struct rc_peer in = {.seq = 0xFFFFFFFFU, .slot = RC_SLOT_A, .role = RC_ROLE_ACTIVE,
			     .referee_ok = 1U, .step = 0xBEEFU, .healthy = 1U, .test_mask = 0x55U};
	struct rc_peer out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_peer(&in, buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD + 11U);
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_peer(&f, &out) == 0);
	CHECK(out.seq == in.seq && out.slot == in.slot && out.role == in.role);
	CHECK(out.referee_ok == 1U && out.step == 0xBEEFU);
	CHECK(out.healthy == 1U && out.test_mask == 0x55U);
}

static void test_run_bit_roundtrip(void)
{
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_run_bit(buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD);
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_run_bit(&f) == 0);
	f.len = 1U;
	CHECK(rc_decode_run_bit(&f) == -1);
	f.type = RC_MSG_STATUS;
	f.len = 0U;
	CHECK(rc_decode_run_bit(&f) == -1);
}

static void test_decode_rejects_part1_lengths(void)
{
	struct rc_frame hb = {.type = RC_MSG_HEARTBEAT, .len = 5U};
	struct rc_frame st = {.type = RC_MSG_STATUS, .len = 7U};
	struct rc_frame pe = {.type = RC_MSG_PEER, .len = 9U};
	struct rc_heartbeat h;
	struct rc_status s;
	struct rc_peer p;

	CHECK(rc_decode_heartbeat(&hb, &h) == -1);
	CHECK(rc_decode_status(&st, &s) == -1);
	CHECK(rc_decode_peer(&pe, &p) == -1);
}
```

In `main()`, add after `test_decode_rejects_wrong_type_or_len();`:

```c
	test_run_bit_roundtrip();
	test_decode_rejects_part1_lengths();
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host 2>&1 | grep -m3 error`
Expected: compile errors such as `'struct rc_heartbeat' has no member named 'healthy'` and `implicit declaration of function 'rc_encode_run_bit'`.

- [ ] **Step 3: Implement**

In `common/include/rc/proto.h`, after `#define RC_MSG_PEER 4U` add:

```c
#define RC_MSG_RUN_BIT 5U
```

After `#define RC_SLOT_NONE 0xFFU` add:

```c
#define RC_MODE_OPERATIONAL 0U
#define RC_MODE_TEST 1U
```

Replace the three message structs with:

```c
struct rc_heartbeat {
	uint32_t seq;
	uint8_t role;
	uint8_t healthy;
};

struct rc_status {
	uint32_t seq;
	uint8_t slot;
	uint8_t granted_role;
	uint8_t active_slot;
	uint8_t mode;
	uint8_t io_fail;
};
```

and

```c
struct rc_peer {
	uint32_t seq;
	uint8_t slot;
	uint8_t role;
	uint8_t referee_ok;
	uint16_t step;
	uint8_t healthy;
	uint8_t test_mask;
};
```

Add after the `rc_encode_peer` declaration:

```c
size_t rc_encode_run_bit(uint8_t *out, size_t out_size);
```

and after the `rc_decode_peer` declaration:

```c
int rc_decode_run_bit(const struct rc_frame *f);
```

In `common/proto.c`, replace `rc_encode_heartbeat`, `rc_encode_status`, `rc_encode_peer`, `rc_decode_heartbeat`, `rc_decode_status`, and `rc_decode_peer` with:

```c
size_t rc_encode_heartbeat(const struct rc_heartbeat *m, uint8_t *out, size_t out_size)
{
	uint8_t p[6];

	put_u32(p, m->seq);
	p[4] = m->role;
	p[5] = m->healthy;
	return rc_frame_encode(RC_MSG_HEARTBEAT, p, sizeof(p), out, out_size);
}

size_t rc_encode_status(const struct rc_status *m, uint8_t *out, size_t out_size)
{
	uint8_t p[9];

	put_u32(p, m->seq);
	p[4] = m->slot;
	p[5] = m->granted_role;
	p[6] = m->active_slot;
	p[7] = m->mode;
	p[8] = m->io_fail;
	return rc_frame_encode(RC_MSG_STATUS, p, sizeof(p), out, out_size);
}
```

```c
size_t rc_encode_peer(const struct rc_peer *m, uint8_t *out, size_t out_size)
{
	uint8_t p[11];

	put_u32(p, m->seq);
	p[4] = m->slot;
	p[5] = m->role;
	p[6] = m->referee_ok;
	put_u16(&p[7], m->step);
	p[9] = m->healthy;
	p[10] = m->test_mask;
	return rc_frame_encode(RC_MSG_PEER, p, sizeof(p), out, out_size);
}

size_t rc_encode_run_bit(uint8_t *out, size_t out_size)
{
	return rc_frame_encode(RC_MSG_RUN_BIT, NULL, 0U, out, out_size);
}

int rc_decode_heartbeat(const struct rc_frame *f, struct rc_heartbeat *m)
{
	if ((f->type != RC_MSG_HEARTBEAT) || (f->len != 6U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->role = f->payload[4];
	m->healthy = f->payload[5];
	return 0;
}

int rc_decode_status(const struct rc_frame *f, struct rc_status *m)
{
	if ((f->type != RC_MSG_STATUS) || (f->len != 9U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->slot = f->payload[4];
	m->granted_role = f->payload[5];
	m->active_slot = f->payload[6];
	m->mode = f->payload[7];
	m->io_fail = f->payload[8];
	return 0;
}
```

```c
int rc_decode_peer(const struct rc_frame *f, struct rc_peer *m)
{
	if ((f->type != RC_MSG_PEER) || (f->len != 11U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->slot = f->payload[4];
	m->role = f->payload[5];
	m->referee_ok = f->payload[6];
	m->step = get_u16(&f->payload[7]);
	m->healthy = f->payload[9];
	m->test_mask = f->payload[10];
	return 0;
}

int rc_decode_run_bit(const struct rc_frame *f)
{
	return ((f->type == RC_MSG_RUN_BIT) && (f->len == 0U)) ? 0 : -1;
}
```

In `io-card/src/main.c` `handle_frame`, the STATUS struct is filled field by field; add the two new fields so no stack garbage is sent (Task 9 fills them with real values):

```c
		st.active_slot = arb_active(&arb);
		st.mode = RC_MODE_OPERATIONAL;
		st.io_fail = 0U;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 3`.

Run: `cd ~/ws/stm32f3-ws && . ~/ws/zp/zephyr-venv/bin/activate && west build -b stm32f3_disco@E -d build/rc-io-card ~/ws/redundant-controller/io-card 2>&1 | tail -1`
Expected: a memory usage table ending without errors (the firmware still builds).

- [ ] **Step 5: Commit**

```bash
git add common tests/host/test_proto.c io-card/src/main.c
git commit -m "feat: add health, mode, and BIT fields to the protocol" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Arbiter takes a health flag (structural)

**Files:**
- Modify: `io-card/src/arbiter.h`, `io-card/src/arbiter.c`, `io-card/src/main.c`, `tests/host/test_arbiter.c`

**Interfaces:**
- Produces: `void arb_on_heartbeat(struct arbiter *a, uint8_t slot, uint8_t reported_role, bool healthy, int64_t now_ms)`; `struct arb_slot` gains `bool healthy`. No behavior change in this task.

- [ ] **Step 1: Change the signature and callers**

In `io-card/src/arbiter.h`, add `bool healthy;` as the last member of `struct arb_slot` and change the declaration to:

```c
void arb_on_heartbeat(struct arbiter *a, uint8_t slot, uint8_t reported_role, bool healthy,
		      int64_t now_ms);
```

In `io-card/src/arbiter.c`, change the definition the same way and store the flag after `reported_role`:

```c
	a->slot[slot].healthy = healthy;
```

In `tests/host/test_arbiter.c`, change the two calls inside `run()` and the calls in `test_failover_timing` and `test_invalid_slot_ignored` to pass `true` before the time argument, for example:

```c
			arb_on_heartbeat(a, RC_SLOT_A, role_a, true, t);
```

In `io-card/src/main.c` `handle_frame`:

```c
		arb_on_heartbeat(&arb, l->slot, hb.role, true, now);
```

- [ ] **Step 2: Run the tests**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 3` (no behavior changed).

- [ ] **Step 3: Commit**

```bash
git add io-card/src tests/host/test_arbiter.c
git commit -m "refactor: pass a health flag to the arbiter" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Health-aware election and handover

**Files:**
- Modify: `io-card/src/arbiter.h`, `io-card/src/arbiter.c`
- Test: `tests/host/test_arbiter.c`

**Interfaces:**
- Consumes: `arb_on_heartbeat(..., bool healthy, ...)` from Task 2.
- Produces: `ARB_HEALTH_HOLD_MS` (1200); `struct arbiter` gains `bool handover_pending; int64_t handover_since_ms;`. Rules per Design refinements 1 and 2.

- [ ] **Step 1: Write the failing tests**

Add to `tests/host/test_arbiter.c` after `run()`:

```c
/* Both slots send every 20 ms with the given role and health. */
static void run_h(struct arbiter *a, int64_t from, int64_t to, uint8_t role_a, bool healthy_a,
		  uint8_t role_b, bool healthy_b)
{
	for (int64_t t = from; t < to; t += 20) {
		arb_on_heartbeat(a, RC_SLOT_A, role_a, healthy_a, t);
		arb_on_heartbeat(a, RC_SLOT_B, role_b, healthy_b, t);
		(void)arb_tick(a, t);
	}
}

/* Elects A with both healthy; returns at 2000 ms. */
static void elect_a(struct arbiter *a)
{
	arb_init(a);
	run_h(a, 0, 2000, RC_ROLE_UNKNOWN, true, RC_ROLE_UNKNOWN, true);
}

static void test_election_prefers_healthy(void)
{
	struct arbiter a;

	arb_init(&a);
	run_h(&a, 0, 2000, RC_ROLE_UNKNOWN, false, RC_ROLE_UNKNOWN, true);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_election_keeps_reported_active_when_both_unhealthy(void)
{
	struct arbiter a;

	arb_init(&a);
	run_h(&a, 0, 2000, RC_ROLE_STANDBY, false, RC_ROLE_ACTIVE, false);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_healthy_beats_unhealthy_reported_active(void)
{
	struct arbiter a;

	arb_init(&a);
	run_h(&a, 0, 2000, RC_ROLE_ACTIVE, false, RC_ROLE_STANDBY, true);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_unhealthy_active_hands_over_after_hold(void)
{
	struct arbiter a;

	elect_a(&a);
	CHECK(arb_active(&a) == RC_SLOT_A);
	/* A unhealthy from 2000 ms: the hold ends at 3200 ms. */
	run_h(&a, 2000, 3200, RC_ROLE_ACTIVE, false, RC_ROLE_STANDBY, true);
	CHECK(arb_active(&a) == RC_SLOT_A);
	run_h(&a, 3200, 3220, RC_ROLE_ACTIVE, false, RC_ROLE_STANDBY, true);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_recovered_slot_stays_standby(void)
{
	struct arbiter a;

	elect_a(&a);
	run_h(&a, 2000, 3300, RC_ROLE_ACTIVE, false, RC_ROLE_STANDBY, true);
	CHECK(arb_active(&a) == RC_SLOT_B);
	run_h(&a, 3300, 8000, RC_ROLE_STANDBY, true, RC_ROLE_ACTIVE, true);
	CHECK(arb_active(&a) == RC_SLOT_B);
}

static void test_unhealthy_active_kept_when_other_unhealthy(void)
{
	struct arbiter a;

	elect_a(&a);
	run_h(&a, 2000, 8000, RC_ROLE_ACTIVE, false, RC_ROLE_STANDBY, false);
	CHECK(arb_active(&a) == RC_SLOT_A);
}

static void test_unhealthy_active_kept_when_other_absent(void)
{
	struct arbiter a;

	arb_init(&a);
	run(&a, 0, 2000, true, RC_ROLE_UNKNOWN, false, 0U);
	CHECK(arb_active(&a) == RC_SLOT_A);
	for (int64_t t = 2000; t < 8000; t += 20) {
		arb_on_heartbeat(&a, RC_SLOT_A, RC_ROLE_ACTIVE, false, t);
		(void)arb_tick(&a, t);
	}
	CHECK(arb_active(&a) == RC_SLOT_A);
}

static void test_common_mode_recovery_does_not_move_active(void)
{
	struct arbiter a;

	elect_a(&a);
	/* Referee restart: both unhealthy, B recovers 800 ms before A. */
	run_h(&a, 2000, 3000, RC_ROLE_ACTIVE, false, RC_ROLE_STANDBY, false);
	run_h(&a, 3000, 3800, RC_ROLE_ACTIVE, false, RC_ROLE_STANDBY, true);
	run_h(&a, 3800, 8000, RC_ROLE_ACTIVE, true, RC_ROLE_STANDBY, true);
	CHECK(arb_active(&a) == RC_SLOT_A);
}

static void test_lost_active_hands_over_to_unhealthy_standby(void)
{
	struct arbiter a;

	elect_a(&a);
	for (int64_t t = 2000; t < 2300; t += 20) {
		arb_on_heartbeat(&a, RC_SLOT_B, RC_ROLE_STANDBY, false, t);
		(void)arb_tick(&a, t);
	}
	CHECK(arb_active(&a) == RC_SLOT_B);
}
```

Add the calls to `main()` before `return CHECK_DONE();`:

```c
	test_election_prefers_healthy();
	test_election_keeps_reported_active_when_both_unhealthy();
	test_healthy_beats_unhealthy_reported_active();
	test_unhealthy_active_hands_over_after_hold();
	test_recovered_slot_stays_standby();
	test_unhealthy_active_kept_when_other_unhealthy();
	test_unhealthy_active_kept_when_other_absent();
	test_common_mode_recovery_does_not_move_active();
	test_lost_active_hands_over_to_unhealthy_standby();
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `arbiter` fails with `FAIL ... arb_active(&a) == RC_SLOT_B` lines from `test_election_prefers_healthy`, `test_healthy_beats_unhealthy_reported_active`, `test_unhealthy_active_hands_over_after_hold`, and `test_recovered_slot_stays_standby`.

- [ ] **Step 3: Implement**

In `io-card/src/arbiter.h`, add after `ARB_ELECTION_WINDOW_MS`:

```c
/* An unhealthy Active hands over only after the condition holds this long. */
#define ARB_HEALTH_HOLD_MS 1200
```

and add to `struct arbiter`:

```c
	bool handover_pending;
	int64_t handover_since_ms;
```

In `io-card/src/arbiter.c`, replace `elect()` and `arb_tick()` with:

```c
static bool healthy(const struct arbiter *a, uint8_t slot, int64_t now_ms)
{
	return arb_present(a, slot, now_ms) && a->slot[slot].healthy;
}

static bool claims_active(const struct arbiter *a, uint8_t slot)
{
	return a->slot[slot].reported_role == RC_ROLE_ACTIVE;
}

static uint8_t elect(const struct arbiter *a, int64_t now_ms)
{
	static const uint8_t order[2] = {RC_SLOT_A, RC_SLOT_B};

	for (int i = 0; i < 2; i++) {
		if (healthy(a, order[i], now_ms) && claims_active(a, order[i])) {
			return order[i];
		}
	}
	for (int i = 0; i < 2; i++) {
		if (healthy(a, order[i], now_ms)) {
			return order[i];
		}
	}
	for (int i = 0; i < 2; i++) {
		if (arb_present(a, order[i], now_ms) && claims_active(a, order[i])) {
			return order[i];
		}
	}
	for (int i = 0; i < 2; i++) {
		if (arb_present(a, order[i], now_ms)) {
			return order[i];
		}
	}
	return RC_SLOT_NONE;
}

bool arb_tick(struct arbiter *a, int64_t now_ms)
{
	uint8_t before = a->active;

	if (a->active != RC_SLOT_NONE) {
		uint8_t o = other(a->active);

		if (!arb_present(a, a->active, now_ms)) {
			a->active = arb_present(a, o, now_ms) ? o : RC_SLOT_NONE;
			a->handover_pending = false;
		} else if (!a->slot[a->active].healthy && healthy(a, o, now_ms)) {
			if (!a->handover_pending) {
				a->handover_pending = true;
				a->handover_since_ms = now_ms;
			} else if ((now_ms - a->handover_since_ms) >= ARB_HEALTH_HOLD_MS) {
				a->active = o;
				a->handover_pending = false;
			}
		} else {
			a->handover_pending = false;
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 3`.

- [ ] **Step 5: Commit**

```bash
git add io-card/src/arbiter.h io-card/src/arbiter.c tests/host/test_arbiter.c
git commit -m "feat: hand Active from an unhealthy controller to a healthy one" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: I/O card BIT evaluation

**Files:**
- Create: `io-card/src/io_bit.h`, `io-card/src/io_bit.c`, `tests/host/test_io_bit.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Produces: `IO_BIT_LOOPBACK` (bit 0), `IO_BIT_LEDS` (1), `IO_BIT_SENSORS` (2), `IO_BIT_UART_A` (3), `IO_BIT_UART_B` (4), `IO_BIT_RESET` (5), `IO_BIT_UART_ERROR_LIMIT` (5); `struct io_bit_input`; `uint8_t io_bit_eval(const struct io_bit_input *in)` returning the fail mask.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_io_bit.c`:

```c
#include "check.h"
#include "io_bit.h"

static struct io_bit_input good(void)
{
	struct io_bit_input in = {.loop_high = true, .loop_low = false, .led_read = 0x10U,
				  .led_applied = 0x10U, .accel_ready = true, .magn_ready = true,
				  .uart_errors = {0U, 0U}, .watchdog_reset = false};

	return in;
}

static void test_all_pass(void)
{
	struct io_bit_input in = good();

	CHECK(io_bit_eval(&in) == 0U);
}

static void test_loopback(void)
{
	struct io_bit_input in = good();

	in.loop_high = false; /* jumper removed */
	CHECK(io_bit_eval(&in) == IO_BIT_LOOPBACK);
	in = good();
	in.loop_low = true; /* stuck high */
	CHECK(io_bit_eval(&in) == IO_BIT_LOOPBACK);
}

static void test_led_readback(void)
{
	struct io_bit_input in = good();

	in.led_read = 0x00U;
	CHECK(io_bit_eval(&in) == IO_BIT_LEDS);
}

static void test_sensors(void)
{
	struct io_bit_input in = good();

	in.accel_ready = false;
	CHECK(io_bit_eval(&in) == IO_BIT_SENSORS);
	in = good();
	in.magn_ready = false;
	CHECK(io_bit_eval(&in) == IO_BIT_SENSORS);
}

static void test_uart_error_limit(void)
{
	struct io_bit_input in = good();

	in.uart_errors[0] = IO_BIT_UART_ERROR_LIMIT - 1U;
	CHECK(io_bit_eval(&in) == 0U);
	in.uart_errors[0] = IO_BIT_UART_ERROR_LIMIT;
	CHECK(io_bit_eval(&in) == IO_BIT_UART_A);
	in = good();
	in.uart_errors[1] = IO_BIT_UART_ERROR_LIMIT;
	CHECK(io_bit_eval(&in) == IO_BIT_UART_B);
}

static void test_watchdog_reset(void)
{
	struct io_bit_input in = good();

	in.watchdog_reset = true;
	CHECK(io_bit_eval(&in) == IO_BIT_RESET);
}

int main(void)
{
	test_all_pass();
	test_loopback();
	test_led_readback();
	test_sensors();
	test_uart_error_limit();
	test_watchdog_reset();
	return CHECK_DONE();
}
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_io_bit test_io_bit.c ${REPO}/io-card/src/io_bit.c)
target_include_directories(test_io_bit PRIVATE ${REPO}/io-card/src)
add_test(NAME io_bit COMMAND test_io_bit)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S tests/host -B build/host 2>&1 | grep -m1 -i "cannot find\|no such"`
Expected: CMake reports that `io-card/src/io_bit.c` cannot be found.

- [ ] **Step 3: Implement**

Create `io-card/src/io_bit.h`:

```c
#ifndef IO_BIT_H_
#define IO_BIT_H_

#include <stdbool.h>
#include <stdint.h>

#define IO_BIT_LOOPBACK (1U << 0)
#define IO_BIT_LEDS (1U << 1)
#define IO_BIT_SENSORS (1U << 2)
#define IO_BIT_UART_A (1U << 3)
#define IO_BIT_UART_B (1U << 4)
#define IO_BIT_RESET (1U << 5)

/* A slot fails at this many CRC or length errors in one check period. */
#define IO_BIT_UART_ERROR_LIMIT 5U

struct io_bit_input {
	bool loop_high; /* loopback input read while the output drives high */
	bool loop_low;  /* loopback input read while the output drives low */
	uint8_t led_read;
	uint8_t led_applied;
	bool accel_ready;
	bool magn_ready;
	uint32_t uart_errors[2]; /* errors since the last check, per slot */
	bool watchdog_reset;
};

/* Returns the fail mask (IO_BIT_* bits). */
uint8_t io_bit_eval(const struct io_bit_input *in);

#endif /* IO_BIT_H_ */
```

Create `io-card/src/io_bit.c`:

```c
#include "io_bit.h"

uint8_t io_bit_eval(const struct io_bit_input *in)
{
	uint8_t fail = 0U;

	if (!in->loop_high || in->loop_low) {
		fail |= IO_BIT_LOOPBACK;
	}
	if (in->led_read != in->led_applied) {
		fail |= IO_BIT_LEDS;
	}
	if (!in->accel_ready || !in->magn_ready) {
		fail |= IO_BIT_SENSORS;
	}
	if (in->uart_errors[0] >= IO_BIT_UART_ERROR_LIMIT) {
		fail |= IO_BIT_UART_A;
	}
	if (in->uart_errors[1] >= IO_BIT_UART_ERROR_LIMIT) {
		fail |= IO_BIT_UART_B;
	}
	if (in->watchdog_reset) {
		fail |= IO_BIT_RESET;
	}
	return fail;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 4`.

- [ ] **Step 5: Commit**

```bash
git add io-card/src/io_bit.h io-card/src/io_bit.c tests/host/test_io_bit.c tests/host/CMakeLists.txt
git commit -m "feat: add I/O card BIT evaluation" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Mode button debounce

**Files:**
- Create: `io-card/src/mode.h`, `io-card/src/mode.c`, `tests/host/test_mode.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `RC_MODE_OPERATIONAL`, `RC_MODE_TEST` from Task 1.
- Produces: `MODE_DEBOUNCE_MS` (50); `struct mode_state { uint8_t mode; bool stable; bool raw; int64_t raw_since_ms; }`; `void mode_init(struct mode_state *m, bool pressed, int64_t now_ms)`; `bool mode_on_sample(struct mode_state *m, bool pressed, int64_t now_ms)` returning true when the mode toggled.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_mode.c`:

```c
#include "check.h"
#include "mode.h"

/* Samples the button every 1 ms in [from, to); returns the number of toggles. */
static int hold(struct mode_state *m, bool pressed, int64_t from, int64_t to)
{
	int toggles = 0;

	for (int64_t t = from; t < to; t++) {
		if (mode_on_sample(m, pressed, t)) {
			toggles++;
		}
	}
	return toggles;
}

static void test_press_toggles_once(void)
{
	struct mode_state m;

	mode_init(&m, false, 0);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
	CHECK(hold(&m, true, 0, 200) == 1);
	CHECK(m.mode == RC_MODE_TEST);
	CHECK(hold(&m, false, 200, 400) == 0);
	CHECK(hold(&m, true, 400, 600) == 1);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
}

static void test_bounce_counts_once(void)
{
	struct mode_state m;
	int toggles = 0;

	mode_init(&m, false, 0);
	for (int64_t t = 0; t < 40; t++) {
		if (mode_on_sample(&m, ((t / 5) % 2) == 0, t)) {
			toggles++;
		}
	}
	toggles += hold(&m, true, 40, 200);
	CHECK(toggles == 1);
	CHECK(m.mode == RC_MODE_TEST);
}

static void test_short_glitch_ignored(void)
{
	struct mode_state m;

	mode_init(&m, false, 0);
	CHECK(hold(&m, true, 0, 30) == 0);
	CHECK(hold(&m, false, 30, 200) == 0);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
}

static void test_held_at_boot_does_not_toggle(void)
{
	struct mode_state m;

	mode_init(&m, true, 0);
	CHECK(hold(&m, true, 0, 500) == 0);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
}

int main(void)
{
	test_press_toggles_once();
	test_bounce_counts_once();
	test_short_glitch_ignored();
	test_held_at_boot_does_not_toggle();
	return CHECK_DONE();
}
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_mode test_mode.c ${REPO}/io-card/src/mode.c)
target_include_directories(test_mode PRIVATE ${REPO}/common/include ${REPO}/io-card/src)
add_test(NAME mode COMMAND test_mode)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S tests/host -B build/host 2>&1 | grep -m1 -i "cannot find\|no such"`
Expected: CMake reports that `io-card/src/mode.c` cannot be found.

- [ ] **Step 3: Implement**

Create `io-card/src/mode.h`:

```c
#ifndef MODE_H_
#define MODE_H_

#include <stdbool.h>
#include <stdint.h>

#include "rc/proto.h"

#define MODE_DEBOUNCE_MS 50

struct mode_state {
	uint8_t mode;
	bool stable;
	bool raw;
	int64_t raw_since_ms;
};

void mode_init(struct mode_state *m, bool pressed, int64_t now_ms);

/* Feeds one button sample; returns true when a debounced press toggled the mode. */
bool mode_on_sample(struct mode_state *m, bool pressed, int64_t now_ms);

#endif /* MODE_H_ */
```

Create `io-card/src/mode.c`:

```c
#include "mode.h"

void mode_init(struct mode_state *m, bool pressed, int64_t now_ms)
{
	m->mode = RC_MODE_OPERATIONAL;
	m->stable = pressed;
	m->raw = pressed;
	m->raw_since_ms = now_ms;
}

bool mode_on_sample(struct mode_state *m, bool pressed, int64_t now_ms)
{
	if (pressed != m->raw) {
		m->raw = pressed;
		m->raw_since_ms = now_ms;
		return false;
	}
	if ((pressed == m->stable) || ((now_ms - m->raw_since_ms) < MODE_DEBOUNCE_MS)) {
		return false;
	}
	m->stable = pressed;
	if (!pressed) {
		return false;
	}
	m->mode = (m->mode == RC_MODE_TEST) ? RC_MODE_OPERATIONAL : RC_MODE_TEST;
	return true;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 5`.

- [ ] **Step 5: Commit**

```bash
git add io-card/src/mode.h io-card/src/mode.c tests/host/test_mode.c tests/host/CMakeLists.txt
git commit -m "feat: add debounced mode button logic" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Controller BIT and health

**Files:**
- Create: `central/src/bit.h`, `central/src/bit.c`, `tests/host/test_bit.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Produces: item indices `BIT_IO_LINK` 0, `BIT_LOOP_TIMING` 1, `BIT_CROSS_LINK` 2, `BIT_SUPPLY` 3, `BIT_CPU_TEMP` 4, `BIT_ITEMS` 5; `BIT_PERIOD_MS` 1000, `BIT_PBIT_DELAY_MS` 1000; `enum bit_result { BIT_PASS, BIT_FAIL, BIT_ERROR }`; `struct bit_input`, `struct bit_report { bool valid; int64_t at_ms; struct bit_input in; enum bit_result result[BIT_ITEMS]; }`, `struct bit_health { bool pbit_done; bool healthy; unsigned pass_run; }`; functions `bit_evaluate`, `bit_is_critical`, `bit_critical_pass`, `bit_item_name`, `bit_result_name`, `bit_item_value`, `bit_health_init`, `bit_health_update` (signatures below).

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_bit.c`:

```c
#include <string.h>

#include "bit.h"
#include "check.h"

static struct bit_input good(void)
{
	struct bit_input in = {.status_age_ms = 12, .link_errors = 0U, .max_hb_interval_ms = 21,
			       .peer_age_ms = 15, .undervoltage_ok = true, .undervoltage = false,
			       .temp_ok = true, .temp_mc = 48312};

	return in;
}

static enum bit_result item(const struct bit_input *in, int i)
{
	struct bit_report r;

	bit_evaluate(in, 1000, &r);
	return r.result[i];
}

static void test_all_pass(void)
{
	struct bit_input in = good();
	struct bit_report r;

	bit_evaluate(&in, 5000, &r);
	CHECK(r.valid && r.at_ms == 5000);
	for (int i = 0; i < BIT_ITEMS; i++) {
		CHECK(r.result[i] == BIT_PASS);
	}
	CHECK(bit_critical_pass(&r));
}

static void test_io_link_limits(void)
{
	struct bit_input in = good();

	in.status_age_ms = 100;
	CHECK(item(&in, BIT_IO_LINK) == BIT_PASS);
	in.status_age_ms = 101;
	CHECK(item(&in, BIT_IO_LINK) == BIT_FAIL);
	in = good();
	in.link_errors = 4U;
	CHECK(item(&in, BIT_IO_LINK) == BIT_PASS);
	in.link_errors = 5U;
	CHECK(item(&in, BIT_IO_LINK) == BIT_FAIL);
}

static void test_loop_timing_limit(void)
{
	struct bit_input in = good();

	in.max_hb_interval_ms = 50;
	CHECK(item(&in, BIT_LOOP_TIMING) == BIT_PASS);
	in.max_hb_interval_ms = 51;
	CHECK(item(&in, BIT_LOOP_TIMING) == BIT_FAIL);
}

static void test_cross_link_limit(void)
{
	struct bit_input in = good();

	in.peer_age_ms = 100;
	CHECK(item(&in, BIT_CROSS_LINK) == BIT_PASS);
	in.peer_age_ms = 101;
	CHECK(item(&in, BIT_CROSS_LINK) == BIT_FAIL);
}

static void test_supply_and_temp(void)
{
	struct bit_input in = good();

	in.undervoltage = true;
	CHECK(item(&in, BIT_SUPPLY) == BIT_FAIL);
	in.undervoltage_ok = false;
	CHECK(item(&in, BIT_SUPPLY) == BIT_ERROR);
	in = good();
	in.temp_mc = 79999;
	CHECK(item(&in, BIT_CPU_TEMP) == BIT_PASS);
	in.temp_mc = 80000;
	CHECK(item(&in, BIT_CPU_TEMP) == BIT_FAIL);
	in.temp_ok = false;
	CHECK(item(&in, BIT_CPU_TEMP) == BIT_ERROR);
}

static void test_only_link_and_timing_are_critical(void)
{
	struct bit_input in = good();
	struct bit_report r;

	CHECK(bit_is_critical(BIT_IO_LINK) && bit_is_critical(BIT_LOOP_TIMING));
	CHECK(!bit_is_critical(BIT_CROSS_LINK) && !bit_is_critical(BIT_SUPPLY));
	CHECK(!bit_is_critical(BIT_CPU_TEMP));
	in.peer_age_ms = 5000;
	in.undervoltage = true;
	in.temp_ok = false;
	bit_evaluate(&in, 0, &r);
	CHECK(bit_critical_pass(&r));
	in.status_age_ms = 500;
	bit_evaluate(&in, 0, &r);
	CHECK(!bit_critical_pass(&r));
}

static void test_names_and_values(void)
{
	struct bit_input in = good();
	struct bit_report r;
	char v[64];

	bit_evaluate(&in, 0, &r);
	CHECK(strcmp(bit_item_name(BIT_SUPPLY), "supply_voltage") == 0);
	CHECK(strcmp(bit_result_name(BIT_ERROR), "error") == 0);
	bit_item_value(&r, BIT_IO_LINK, v, sizeof(v));
	CHECK(strcmp(v, "status_age_ms=12 errors=0") == 0);
	bit_item_value(&r, BIT_CPU_TEMP, v, sizeof(v));
	CHECK(strcmp(v, "temp_mc=48312") == 0);
	in.undervoltage_ok = false;
	bit_evaluate(&in, 0, &r);
	bit_item_value(&r, BIT_SUPPLY, v, sizeof(v));
	CHECK(strcmp(v, "read error") == 0);
}

static struct bit_report report(bool critical_ok)
{
	struct bit_input in = good();
	struct bit_report r;

	if (!critical_ok) {
		in.status_age_ms = 1000;
	}
	bit_evaluate(&in, 0, &r);
	return r;
}

static void test_pbit_pass_is_healthy_at_once(void)
{
	struct bit_health h;
	struct bit_report ok = report(true);

	bit_health_init(&h);
	CHECK(!h.healthy && !h.pbit_done);
	CHECK(bit_health_update(&h, &ok));
	CHECK(h.healthy && h.pbit_done);
}

static void test_recovery_needs_three_passes(void)
{
	struct bit_health h;
	struct bit_report ok = report(true);
	struct bit_report bad = report(false);

	bit_health_init(&h);
	(void)bit_health_update(&h, &ok);
	CHECK(bit_health_update(&h, &bad));
	CHECK(!h.healthy);
	CHECK(!bit_health_update(&h, &ok));
	CHECK(!bit_health_update(&h, &ok));
	CHECK(bit_health_update(&h, &ok));
	CHECK(h.healthy);
}

static void test_pbit_fail_is_unhealthy(void)
{
	struct bit_health h;
	struct bit_report bad = report(false);
	struct bit_report ok = report(true);

	bit_health_init(&h);
	CHECK(!bit_health_update(&h, &bad));
	CHECK(!h.healthy && h.pbit_done);
	(void)bit_health_update(&h, &ok);
	(void)bit_health_update(&h, &ok);
	CHECK(!h.healthy);
	CHECK(bit_health_update(&h, &ok));
}

int main(void)
{
	test_all_pass();
	test_io_link_limits();
	test_loop_timing_limit();
	test_cross_link_limit();
	test_supply_and_temp();
	test_only_link_and_timing_are_critical();
	test_names_and_values();
	test_pbit_pass_is_healthy_at_once();
	test_recovery_needs_three_passes();
	test_pbit_fail_is_unhealthy();
	return CHECK_DONE();
}
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_bit test_bit.c ${REPO}/central/src/bit.c)
target_include_directories(test_bit PRIVATE ${REPO}/central/src)
add_test(NAME bit COMMAND test_bit)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S tests/host -B build/host 2>&1 | grep -m1 -i "cannot find\|no such"`
Expected: CMake reports that `central/src/bit.c` cannot be found.

- [ ] **Step 3: Implement**

Create `central/src/bit.h`:

```c
#ifndef BIT_H_
#define BIT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIT_IO_LINK 0
#define BIT_LOOP_TIMING 1
#define BIT_CROSS_LINK 2
#define BIT_SUPPLY 3
#define BIT_CPU_TEMP 4
#define BIT_ITEMS 5

#define BIT_STATUS_MAX_AGE_MS 100
#define BIT_LINK_ERROR_LIMIT 5U
#define BIT_MAX_HB_INTERVAL_MS 50
#define BIT_PEER_MAX_AGE_MS 100
#define BIT_TEMP_LIMIT_MC 80000
#define BIT_RECOVERY_PASSES 3U
#define BIT_PERIOD_MS 1000
#define BIT_PBIT_DELAY_MS 1000

enum bit_result { BIT_PASS, BIT_FAIL, BIT_ERROR };

struct bit_input {
	int64_t status_age_ms;
	uint32_t link_errors; /* CRC and length errors since the last check */
	int64_t max_hb_interval_ms;
	int64_t peer_age_ms;
	bool undervoltage_ok; /* false: the flag could not be read */
	bool undervoltage;
	bool temp_ok; /* false: the temperature could not be read */
	int32_t temp_mc;
};

struct bit_report {
	bool valid;
	int64_t at_ms;
	struct bit_input in;
	enum bit_result result[BIT_ITEMS];
};

struct bit_health {
	bool pbit_done;
	bool healthy;
	unsigned pass_run;
};

void bit_evaluate(const struct bit_input *in, int64_t now_ms, struct bit_report *out);
bool bit_is_critical(int item);
bool bit_critical_pass(const struct bit_report *r);
const char *bit_item_name(int item);
const char *bit_result_name(enum bit_result r);

/* Writes the measured value as text, e.g. "status_age_ms=12 errors=0". */
void bit_item_value(const struct bit_report *r, int item, char *out, size_t out_size);

void bit_health_init(struct bit_health *h);

/* Applies one check; returns true when healthy changed. The first check is PBIT. */
bool bit_health_update(struct bit_health *h, const struct bit_report *r);

#endif /* BIT_H_ */
```

Create `central/src/bit.c`:

```c
#include <stdio.h>

#include "bit.h"

static const char *const names[BIT_ITEMS] = {"io_link", "loop_timing", "cross_link",
					     "supply_voltage", "cpu_temp"};

static enum bit_result pass_if(bool ok)
{
	return ok ? BIT_PASS : BIT_FAIL;
}

void bit_evaluate(const struct bit_input *in, int64_t now_ms, struct bit_report *out)
{
	out->valid = true;
	out->at_ms = now_ms;
	out->in = *in;
	out->result[BIT_IO_LINK] = pass_if((in->status_age_ms <= BIT_STATUS_MAX_AGE_MS) &&
					   (in->link_errors < BIT_LINK_ERROR_LIMIT));
	out->result[BIT_LOOP_TIMING] = pass_if(in->max_hb_interval_ms <= BIT_MAX_HB_INTERVAL_MS);
	out->result[BIT_CROSS_LINK] = pass_if(in->peer_age_ms <= BIT_PEER_MAX_AGE_MS);
	out->result[BIT_SUPPLY] = in->undervoltage_ok ? pass_if(!in->undervoltage) : BIT_ERROR;
	out->result[BIT_CPU_TEMP] = in->temp_ok ? pass_if(in->temp_mc < BIT_TEMP_LIMIT_MC)
						: BIT_ERROR;
}

bool bit_is_critical(int item)
{
	return (item == BIT_IO_LINK) || (item == BIT_LOOP_TIMING);
}

bool bit_critical_pass(const struct bit_report *r)
{
	for (int i = 0; i < BIT_ITEMS; i++) {
		if (bit_is_critical(i) && (r->result[i] != BIT_PASS)) {
			return false;
		}
	}
	return true;
}

const char *bit_item_name(int item)
{
	return ((item >= 0) && (item < BIT_ITEMS)) ? names[item] : "unknown";
}

const char *bit_result_name(enum bit_result r)
{
	switch (r) {
	case BIT_PASS:
		return "pass";
	case BIT_FAIL:
		return "fail";
	case BIT_ERROR:
		return "error";
	}
	return "unknown";
}

void bit_item_value(const struct bit_report *r, int item, char *out, size_t out_size)
{
	const struct bit_input *in = &r->in;

	switch (item) {
	case BIT_IO_LINK:
		snprintf(out, out_size, "status_age_ms=%lld errors=%u", (long long)in->status_age_ms,
			 (unsigned)in->link_errors);
		break;
	case BIT_LOOP_TIMING:
		snprintf(out, out_size, "max_interval_ms=%lld", (long long)in->max_hb_interval_ms);
		break;
	case BIT_CROSS_LINK:
		snprintf(out, out_size, "peer_age_ms=%lld", (long long)in->peer_age_ms);
		break;
	case BIT_SUPPLY:
		if (in->undervoltage_ok) {
			snprintf(out, out_size, "undervoltage=%d", in->undervoltage ? 1 : 0);
		} else {
			snprintf(out, out_size, "read error");
		}
		break;
	case BIT_CPU_TEMP:
		if (in->temp_ok) {
			snprintf(out, out_size, "temp_mc=%ld", (long)in->temp_mc);
		} else {
			snprintf(out, out_size, "read error");
		}
		break;
	default:
		snprintf(out, out_size, "-");
		break;
	}
}

void bit_health_init(struct bit_health *h)
{
	h->pbit_done = false;
	h->healthy = false;
	h->pass_run = 0U;
}

bool bit_health_update(struct bit_health *h, const struct bit_report *r)
{
	bool before = h->healthy;
	bool ok = bit_critical_pass(r);

	if (!h->pbit_done) {
		h->pbit_done = true;
		h->healthy = ok;
		h->pass_run = ok ? BIT_RECOVERY_PASSES : 0U;
	} else if (!ok) {
		h->healthy = false;
		h->pass_run = 0U;
	} else {
		if (h->pass_run < BIT_RECOVERY_PASSES) {
			h->pass_run++;
		}
		if (h->pass_run >= BIT_RECOVERY_PASSES) {
			h->healthy = true;
		}
	}
	return h->healthy != before;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 6`.

- [ ] **Step 5: Commit**

```bash
git add central/src/bit.h central/src/bit.c tests/host/test_bit.c tests/host/CMakeLists.txt
git commit -m "feat: add controller BIT items and health hysteresis" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Test mode in the role logic

**Files:**
- Modify: `central/src/role.h`, `central/src/role.c`
- Test: `tests/host/test_role.c`

**Interfaces:**
- Consumes: `rc_status.mode`, `rc_status.io_fail`, `rc_peer.healthy`, `rc_peer.test_mask`, `RC_MODE_*` from Task 1.
- Produces: `ROLE_EV_MODE_CHANGED` (bit 8), `ROLE_LAMP_STEP_MS` (1000); `struct role_state` gains `uint8_t active_slot, mode, io_fail, test_mask, peer_test_mask, out_mask; bool peer_healthy, lamp_active; int64_t lamp_start_ms, last_out_ms;`; `void role_set_test_mask(struct role_state *s, uint8_t mask, int64_t now_ms)`; `void role_start_lamp_test(struct role_state *s, int64_t now_ms)`; `bool role_test_output_due(struct role_state *s, int64_t now_ms, uint8_t *mask)`; `uint8_t role_current_mask(const struct role_state *s)`. `role_chaser_due` returns false outside operational mode.

- [ ] **Step 1: Write the failing tests**

In `tests/host/test_role.c`, set `.healthy = 1U` in the `peer()` helper's initializer, and add after it:

```c
static struct rc_status status_mode(uint8_t slot, uint8_t granted, uint8_t mode)
{
	struct rc_status m = status(slot, granted);

	m.mode = mode;
	return m;
}

/* Slot A granted Active at time t in operational mode. */
static void make_active(struct role_state *s, int64_t t)
{
	struct rc_status m = status(RC_SLOT_A, RC_ROLE_ACTIVE);

	role_init(s, 0);
	(void)role_on_status(s, &m, t);
}
```

Add the tests:

```c
static void test_test_mode_pauses_chaser(void)
{
	struct role_state s;
	struct rc_status m = status_mode(RC_SLOT_A, RC_ROLE_ACTIVE, RC_MODE_TEST);
	uint8_t mask = 0xAAU;
	unsigned ev;

	make_active(&s, 0);
	ev = role_on_status(&s, &m, 1000);
	CHECK((ev & ROLE_EV_MODE_CHANGED) != 0U);
	CHECK(s.mode == RC_MODE_TEST);
	CHECK(!role_chaser_due(&s, 1500, &mask));
	CHECK(role_test_output_due(&s, 1500, &mask));
	CHECK(mask == 0x00U);
}

static void test_operator_mask_sent_and_refreshed(void)
{
	struct role_state s;
	struct rc_status m = status_mode(RC_SLOT_A, RC_ROLE_ACTIVE, RC_MODE_TEST);
	uint8_t mask = 0U;

	make_active(&s, 0);
	(void)role_on_status(&s, &m, 1000);
	(void)role_test_output_due(&s, 1000, &mask);
	role_set_test_mask(&s, 0x55U, 1050);
	CHECK(role_test_output_due(&s, 1050, &mask));
	CHECK(mask == 0x55U);
	CHECK(!role_test_output_due(&s, 1100, &mask));
	CHECK(role_test_output_due(&s, 1250, &mask)); /* refresh every chaser period */
	CHECK(mask == 0x55U);
	CHECK(role_current_mask(&s) == 0x55U);
}

static void test_lamp_test_sequence(void)
{
	struct role_state s;
	struct rc_status m = status_mode(RC_SLOT_A, RC_ROLE_ACTIVE, RC_MODE_TEST);
	uint8_t mask = 0U;

	make_active(&s, 0);
	(void)role_on_status(&s, &m, 1000);
	role_set_test_mask(&s, 0x0FU, 1000);
	(void)role_test_output_due(&s, 1000, &mask);
	role_start_lamp_test(&s, 2000);
	CHECK(role_test_output_due(&s, 2000, &mask) && (mask == 0xFFU));
	CHECK(role_test_output_due(&s, 3000, &mask) && (mask == 0x00U));
	CHECK(role_test_output_due(&s, 4000, &mask) && (mask == 0x0FU));
	CHECK(!s.lamp_active);
}

static void test_chaser_resumes_after_test_mode(void)
{
	struct role_state s;
	struct rc_status test = status_mode(RC_SLOT_A, RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct rc_status op = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	uint8_t mask = 0U;
	uint16_t paused;

	make_active(&s, 0);
	CHECK(role_chaser_due(&s, 0, &mask));
	paused = s.step;
	(void)role_on_status(&s, &test, 100);
	CHECK((role_on_status(&s, &op, 5000) & ROLE_EV_MODE_CHANGED) != 0U);
	CHECK(role_chaser_due(&s, 5000, &mask));
	CHECK(s.step == (uint16_t)(paused + 1U));
	CHECK(mask == (uint8_t)(1U << (s.step % 8U)));
	CHECK(role_current_mask(&s) == mask);
}

static void test_new_active_keeps_test_mask(void)
{
	struct role_state s;
	struct rc_status sb = status_mode(RC_SLOT_B, RC_ROLE_STANDBY, RC_MODE_TEST);
	struct rc_status act = status_mode(RC_SLOT_B, RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct rc_peer p = peer(RC_SLOT_A, RC_ROLE_ACTIVE, 42U);
	uint8_t mask = 0U;

	role_init(&s, 0);
	(void)role_on_status(&s, &sb, 10);
	p.test_mask = 0x55U;
	(void)role_on_peer(&s, &p, 20);
	CHECK(!role_test_output_due(&s, 30, &mask)); /* a Standby never drives */
	(void)role_on_status(&s, &act, 200);
	CHECK(role_test_output_due(&s, 200, &mask));
	CHECK(mask == 0x55U);
	CHECK(s.step == 42U);
}

static void test_mode_change_clears_test_mask(void)
{
	struct role_state s;
	struct rc_status test = status_mode(RC_SLOT_A, RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct rc_status op = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	uint8_t mask = 0xAAU;

	make_active(&s, 0);
	(void)role_on_status(&s, &test, 1000);
	role_set_test_mask(&s, 0x55U, 1000);
	(void)role_on_status(&s, &op, 2000);
	(void)role_on_status(&s, &test, 3000);
	CHECK(s.test_mask == 0U);
	CHECK(role_test_output_due(&s, 3000, &mask));
	CHECK(mask == 0x00U);
}

static void test_status_and_peer_fields_recorded(void)
{
	struct role_state s;
	struct rc_status m = status(RC_SLOT_A, RC_ROLE_ACTIVE);
	struct rc_peer p = peer(RC_SLOT_B, RC_ROLE_STANDBY, 0U);

	role_init(&s, 0);
	CHECK(s.active_slot == RC_SLOT_NONE);
	m.io_fail = 0x21U;
	m.active_slot = RC_SLOT_A;
	(void)role_on_status(&s, &m, 10);
	CHECK(s.io_fail == 0x21U && s.active_slot == RC_SLOT_A);
	(void)role_on_peer(&s, &p, 20);
	CHECK(s.peer_healthy);
	p.healthy = 0U;
	(void)role_on_peer(&s, &p, 40);
	CHECK(!s.peer_healthy);
}
```

Add the calls to `main()` before `return CHECK_DONE();`:

```c
	test_test_mode_pauses_chaser();
	test_operator_mask_sent_and_refreshed();
	test_lamp_test_sequence();
	test_chaser_resumes_after_test_mode();
	test_new_active_keeps_test_mask();
	test_mode_change_clears_test_mask();
	test_status_and_peer_fields_recorded();
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build/host 2>&1 | grep -m3 error`
Expected: compile errors such as `'ROLE_EV_MODE_CHANGED' undeclared` and `implicit declaration of function 'role_test_output_due'`.

- [ ] **Step 3: Implement**

In `central/src/role.h`, add after `ROLE_CHASER_PERIOD_MS`:

```c
#define ROLE_LAMP_STEP_MS 1000
```

after `ROLE_EV_SLOT_LEARNED`:

```c
#define ROLE_EV_MODE_CHANGED (1U << 8)
```

add these members at the end of `struct role_state`:

```c
	uint8_t active_slot;
	uint8_t mode;
	uint8_t io_fail;
	uint8_t test_mask;
	uint8_t peer_test_mask;
	bool peer_healthy;
	bool lamp_active;
	int64_t lamp_start_ms;
	uint8_t out_mask;
	int64_t last_out_ms;
```

and these declarations before `#endif`:

```c
/* Operator output for test mode; takes effect at once. */
void role_set_test_mask(struct role_state *s, uint8_t mask, int64_t now_ms);

/* All on for ROLE_LAMP_STEP_MS, all off for ROLE_LAMP_STEP_MS, then the operator mask. */
void role_start_lamp_test(struct role_state *s, int64_t now_ms);

/* In test mode while driving: returns true with *mask set when the output is due. */
bool role_test_output_due(struct role_state *s, int64_t now_ms, uint8_t *mask);

/* The operator mask in test mode, else the chaser position. */
uint8_t role_current_mask(const struct role_state *s);
```

In `central/src/role.c`, in `role_init` add after `s->peer_role = RC_ROLE_UNKNOWN;`:

```c
	s->active_slot = RC_SLOT_NONE;
```

Replace `role_on_status` with:

```c
unsigned role_on_status(struct role_state *s, const struct rc_status *m, int64_t now_ms)
{
	unsigned ev = 0U;

	s->last_status_ms = now_ms;
	s->active_slot = m->active_slot;
	s->io_fail = m->io_fail;
	if (!s->referee_ok) {
		s->referee_ok = true;
		ev |= ROLE_EV_REFEREE_BACK;
	}
	if ((s->slot == RC_SLOT_NONE) && ((m->slot == RC_SLOT_A) || (m->slot == RC_SLOT_B))) {
		s->slot = m->slot;
		ev |= ROLE_EV_SLOT_LEARNED;
	}
	if (m->mode != s->mode) {
		s->mode = m->mode;
		s->test_mask = 0U;
		s->lamp_active = false;
		/* Output at once in the new mode; the chaser resumes from its paused step. */
		s->last_out_ms = now_ms - ROLE_CHASER_PERIOD_MS;
		s->last_step_ms = now_ms - ROLE_CHASER_PERIOD_MS;
		ev |= ROLE_EV_MODE_CHANGED;
	}
	if ((m->granted_role != RC_ROLE_UNKNOWN) && (m->granted_role != s->role)) {
		if ((m->granted_role == RC_ROLE_ACTIVE) && s->peer_seen) {
			s->step = s->peer_step;
			if (s->mode == RC_MODE_TEST) {
				s->test_mask = s->peer_test_mask;
			}
		}
		if (m->granted_role == RC_ROLE_ACTIVE) {
			s->last_step_ms = now_ms - ROLE_CHASER_PERIOD_MS;
			s->last_out_ms = now_ms - ROLE_CHASER_PERIOD_MS;
			s->active_since_ms = now_ms;
		}
		s->role = m->granted_role;
		ev |= ROLE_EV_ROLE_CHANGED;
	}
	return ev | update_fault(s);
}
```

In `role_on_peer`, add after `s->peer_step = m->step;`:

```c
	s->peer_test_mask = m->test_mask;
	s->peer_healthy = (m->healthy != 0U);
```

Replace `role_chaser_due` and add the new functions at the end of the file:

```c
bool role_chaser_due(struct role_state *s, int64_t now_ms, uint8_t *mask)
{
	if (!role_may_drive(s) || (s->mode != RC_MODE_OPERATIONAL) ||
	    ((now_ms - s->last_step_ms) < ROLE_CHASER_PERIOD_MS)) {
		return false;
	}
	s->step++;
	s->last_step_ms = now_ms;
	*mask = (uint8_t)(1U << (s->step % 8U));
	return true;
}

void role_set_test_mask(struct role_state *s, uint8_t mask, int64_t now_ms)
{
	s->test_mask = mask;
	s->lamp_active = false;
	s->last_out_ms = now_ms - ROLE_CHASER_PERIOD_MS;
}

void role_start_lamp_test(struct role_state *s, int64_t now_ms)
{
	s->lamp_active = true;
	s->lamp_start_ms = now_ms;
	s->last_out_ms = now_ms - ROLE_CHASER_PERIOD_MS;
}

static uint8_t test_output(struct role_state *s, int64_t now_ms)
{
	if (s->lamp_active) {
		int64_t elapsed = now_ms - s->lamp_start_ms;

		if (elapsed < ROLE_LAMP_STEP_MS) {
			return 0xFFU;
		}
		if (elapsed < (2 * ROLE_LAMP_STEP_MS)) {
			return 0x00U;
		}
		s->lamp_active = false;
	}
	return s->test_mask;
}

bool role_test_output_due(struct role_state *s, int64_t now_ms, uint8_t *mask)
{
	uint8_t want;

	if (!role_may_drive(s) || (s->mode != RC_MODE_TEST)) {
		return false;
	}
	want = test_output(s, now_ms);
	if ((want == s->out_mask) && ((now_ms - s->last_out_ms) < ROLE_CHASER_PERIOD_MS)) {
		return false;
	}
	s->out_mask = want;
	s->last_out_ms = now_ms;
	*mask = want;
	return true;
}

uint8_t role_current_mask(const struct role_state *s)
{
	if (s->mode == RC_MODE_TEST) {
		return s->test_mask;
	}
	return (uint8_t)(1U << (s->step % 8U));
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 6`.

- [ ] **Step 5: Commit**

```bash
git add central/src/role.h central/src/role.c tests/host/test_role.c
git commit -m "feat: add test mode output and lamp test to the role logic" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: TCP command handler

**Files:**
- Create: `central/src/cmd.h`, `central/src/cmd.c`, `tests/host/test_cmd.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `struct bit_report`, `bit_item_name`, `bit_result_name`, `bit_is_critical`, `bit_item_value`, `BIT_ITEMS` from Task 6; `RC_ROLE_*`, `RC_SLOT_*`, `RC_MODE_*` from Task 1.
- Produces: `CMD_LINE_MAX` (128), `CMD_REPLY_MAX` (2048); `enum cmd_action { CMD_ACT_NONE, CMD_ACT_SET_LEDS, CMD_ACT_LAMP_TEST, CMD_ACT_RUN_BIT }`; `struct cmd_view`, `struct cmd_result { enum cmd_action action; uint8_t leds; }`, `struct cmd_linebuf`, `enum cmd_feed { CMD_FEED_NONE, CMD_FEED_LINE, CMD_FEED_TOO_LONG }`; `cmd_linebuf_init`, `cmd_linebuf_feed`, `cmd_handle`, `cmd_too_long` (signatures below).

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_cmd.c`:

```c
#include <string.h>

#include "check.h"
#include "cmd.h"
#include "rc/proto.h"

static char out[CMD_REPLY_MAX];

static struct cmd_view view(uint8_t role, uint8_t mode)
{
	struct cmd_view v = {.slot = RC_SLOT_A, .role = role, .mode = mode,
			     .active_slot = (role == RC_ROLE_ACTIVE) ? RC_SLOT_A : RC_SLOT_B,
			     .healthy = true, .referee_ok = true, .peer_ok = true,
			     .peer_healthy = true, .fault = false, .step = 123U, .mask = 0x55U,
			     .io_fail = 0U, .pbit = NULL, .cbit = NULL, .now_ms = 5000};

	return v;
}

static const char *run(const char *line, const struct cmd_view *v, struct cmd_result *res)
{
	size_t n = cmd_handle(line, v, res, out, sizeof(out));

	CHECK(n == strlen(out));
	CHECK((n > 0U) && (out[n - 1U] == '\n'));
	return out;
}

static void test_status(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	run("STATUS", &v, &res);
	CHECK(res.action == CMD_ACT_NONE);
	CHECK(strstr(out, "\"ok\":true") != NULL);
	CHECK(strstr(out, "\"role\":\"active\"") != NULL);
	CHECK(strstr(out, "\"mode\":\"test\"") != NULL);
	CHECK(strstr(out, "\"step\":123") != NULL);
	CHECK(strstr(out, "\"mask\":85") != NULL);
	v = view(RC_ROLE_STANDBY, RC_MODE_OPERATIONAL);
	run("  STATUS \r", &v, &res);
	CHECK(strstr(out, "\"role\":\"standby\"") != NULL);
	CHECK(strstr(out, "\"mode\":\"operational\"") != NULL);
}

static void test_leds_in_test_mode(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	CHECK(strcmp(run("LEDS 0x55", &v, &res), "{\"ok\":true}\n") == 0);
	CHECK(res.action == CMD_ACT_SET_LEDS && res.leds == 0x55U);
	run("LEDS ff", &v, &res);
	CHECK(res.action == CMD_ACT_SET_LEDS && res.leds == 0xFFU);
}

static void test_leds_bad_values(void)
{
	static const char *const bad[] = {"LEDS 0x1FF", "LEDS zz", "LEDS", "LEDS -1", "LEDS 0x"};
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(strcmp(run(bad[i], &v, &res), "{\"ok\":false,\"error\":\"bad value\"}\n") == 0);
		CHECK(res.action == CMD_ACT_NONE);
	}
}

static void test_output_commands_refused(void)
{
	struct cmd_view op = view(RC_ROLE_ACTIVE, RC_MODE_OPERATIONAL);
	struct cmd_view sb = view(RC_ROLE_STANDBY, RC_MODE_TEST);
	struct cmd_result res;

	CHECK(strcmp(run("LEDS 0x55", &op, &res),
		     "{\"ok\":false,\"error\":\"operational mode\"}\n") == 0);
	CHECK(res.action == CMD_ACT_NONE);
	CHECK(strcmp(run("LAMP_TEST", &op, &res),
		     "{\"ok\":false,\"error\":\"operational mode\"}\n") == 0);
	CHECK(strcmp(run("RUN_BIT", &sb, &res),
		     "{\"ok\":false,\"error\":\"not active\",\"active\":\"B\"}\n") == 0);
	CHECK(res.action == CMD_ACT_NONE);
}

static void test_lamp_and_run_bit_actions(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	run("LAMP_TEST", &v, &res);
	CHECK(res.action == CMD_ACT_LAMP_TEST);
	run("RUN_BIT", &v, &res);
	CHECK(res.action == CMD_ACT_RUN_BIT);
}

static void test_unknown_commands(void)
{
	static const char *const unknown[] = {"HELLO", "", "status", "STATUS now", "LAMP_TEST 1"};
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		CHECK(strcmp(run(unknown[i], &v, &res),
			     "{\"ok\":false,\"error\":\"unknown command\"}\n") == 0);
		CHECK(res.action == CMD_ACT_NONE);
	}
}

static void test_bit_reply(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_OPERATIONAL);
	struct bit_input in = {.status_age_ms = INT64_MAX, .link_errors = UINT32_MAX,
			       .max_hb_interval_ms = INT64_MAX, .peer_age_ms = INT64_MAX,
			       .undervoltage_ok = true, .undervoltage = true,
			       .temp_ok = true, .temp_mc = INT32_MAX};
	struct bit_report r;
	struct cmd_result res;

	run("BIT", &v, &res);
	CHECK(strstr(out, "\"pbit\":null,\"cbit\":null") != NULL);
	bit_evaluate(&in, 4000, &r);
	v.pbit = &r;
	v.cbit = &r;
	run("BIT", &v, &res);
	CHECK(strstr(out, "\"age_ms\":1000") != NULL);
	CHECK(strstr(out, "{\"name\":\"io_link\",\"result\":\"fail\",\"critical\":true,") != NULL);
	CHECK(strstr(out, "\"name\":\"supply_voltage\",\"result\":\"fail\",\"critical\":false") !=
	      NULL);
	CHECK(strstr(out, ",\"io_fail\":0}\n") != NULL); /* not truncated */
}

static void test_linebuf(void)
{
	struct cmd_linebuf lb;
	char line[CMD_LINE_MAX + 1U];
	const char *in = "STATUS\r\n";

	cmd_linebuf_init(&lb);
	for (size_t i = 0; i < strlen(in) - 1U; i++) {
		CHECK(cmd_linebuf_feed(&lb, in[i], line) == CMD_FEED_NONE);
	}
	CHECK(cmd_linebuf_feed(&lb, '\n', line) == CMD_FEED_LINE);
	CHECK(strcmp(line, "STATUS") == 0);
	for (size_t i = 0; i < CMD_LINE_MAX; i++) {
		CHECK(cmd_linebuf_feed(&lb, 'x', line) == CMD_FEED_NONE);
	}
	CHECK(cmd_linebuf_feed(&lb, '\n', line) == CMD_FEED_LINE);
	CHECK(strlen(line) == CMD_LINE_MAX);
	for (size_t i = 0; i < CMD_LINE_MAX; i++) {
		(void)cmd_linebuf_feed(&lb, 'x', line);
	}
	CHECK(cmd_linebuf_feed(&lb, 'x', line) == CMD_FEED_TOO_LONG);
	CHECK(cmd_too_long(out, sizeof(out)) == strlen(out));
	CHECK(strcmp(out, "{\"ok\":false,\"error\":\"line too long\"}\n") == 0);
}

int main(void)
{
	test_status();
	test_leds_in_test_mode();
	test_leds_bad_values();
	test_output_commands_refused();
	test_lamp_and_run_bit_actions();
	test_unknown_commands();
	test_bit_reply();
	test_linebuf();
	return CHECK_DONE();
}
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_executable(test_cmd test_cmd.c ${REPO}/central/src/cmd.c ${REPO}/central/src/bit.c)
target_include_directories(test_cmd PRIVATE ${REPO}/common/include ${REPO}/central/src)
add_test(NAME cmd COMMAND test_cmd)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S tests/host -B build/host 2>&1 | grep -m1 -i "cannot find\|no such"`
Expected: CMake reports that `central/src/cmd.c` cannot be found.

- [ ] **Step 3: Implement**

Create `central/src/cmd.h`:

```c
#ifndef CMD_H_
#define CMD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bit.h"

#define CMD_LINE_MAX 128U
#define CMD_REPLY_MAX 2048U

enum cmd_action { CMD_ACT_NONE, CMD_ACT_SET_LEDS, CMD_ACT_LAMP_TEST, CMD_ACT_RUN_BIT };

/* The daemon state a command can see. */
struct cmd_view {
	uint8_t slot;
	uint8_t role;
	uint8_t mode;
	uint8_t active_slot;
	bool healthy;
	bool referee_ok;
	bool peer_ok;
	bool peer_healthy;
	bool fault;
	uint16_t step;
	uint8_t mask;
	uint8_t io_fail;
	const struct bit_report *pbit;
	const struct bit_report *cbit;
	int64_t now_ms;
};

struct cmd_result {
	enum cmd_action action;
	uint8_t leds;
};

struct cmd_linebuf {
	char buf[CMD_LINE_MAX];
	size_t n;
};

enum cmd_feed { CMD_FEED_NONE, CMD_FEED_LINE, CMD_FEED_TOO_LONG };

void cmd_linebuf_init(struct cmd_linebuf *lb);

/*
 * Feeds one received byte. On CMD_FEED_LINE, line (CMD_LINE_MAX + 1 bytes)
 * holds the line without its LF or CRLF. CMD_FEED_TOO_LONG: more than
 * CMD_LINE_MAX bytes before LF.
 */
enum cmd_feed cmd_linebuf_feed(struct cmd_linebuf *lb, char c, char *line);

/* Handles one line; writes a JSON reply ending in '\n' and returns its length. */
size_t cmd_handle(const char *line, const struct cmd_view *v, struct cmd_result *res, char *out,
		  size_t out_size);

size_t cmd_too_long(char *out, size_t out_size);

#endif /* CMD_H_ */
```

Create `central/src/cmd.c`:

```c
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"
#include "rc/proto.h"

struct out {
	char *buf;
	size_t size;
	size_t n;
};

/* Appends to the reply; replies are sized to fit CMD_REPLY_MAX. */
static void put(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct out *o, const char *fmt, ...)
{
	va_list ap;
	int w;

	if ((o->n + 1U) >= o->size) {
		return;
	}
	va_start(ap, fmt);
	w = vsnprintf(&o->buf[o->n], o->size - o->n, fmt, ap);
	va_end(ap);
	if (w < 0) {
		o->buf[o->n] = '\0';
		return;
	}
	o->n += (size_t)w;
	if (o->n >= o->size) {
		o->n = o->size - 1U;
	}
}

static const char *slot_str(uint8_t slot)
{
	return (slot == RC_SLOT_A) ? "A" : ((slot == RC_SLOT_B) ? "B" : "none");
}

static const char *role_str(uint8_t role)
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

static const char *mode_str(uint8_t mode)
{
	return (mode == RC_MODE_TEST) ? "test" : "operational";
}

static const char *tf(bool b)
{
	return b ? "true" : "false";
}

static size_t reply_error(const char *msg, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":false,\"error\":\"%s\"}\n", msg);
	return o.n;
}

static size_t reply_ok(char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":true}\n");
	return o.n;
}

static size_t reply_not_active(const struct cmd_view *v, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":false,\"error\":\"not active\",\"active\":\"%s\"}\n",
	    slot_str(v->active_slot));
	return o.n;
}

static size_t reply_status(const struct cmd_view *v, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o,
	    "{\"ok\":true,\"slot\":\"%s\",\"role\":\"%s\",\"mode\":\"%s\",\"healthy\":%s,"
	    "\"active\":\"%s\",\"referee\":%s,\"peer\":%s,\"peer_healthy\":%s,\"fault\":%s,"
	    "\"step\":%u,\"mask\":%u,\"io_fail\":%u}\n",
	    slot_str(v->slot), role_str(v->role), mode_str(v->mode), tf(v->healthy),
	    slot_str(v->active_slot), tf(v->referee_ok), tf(v->peer_ok), tf(v->peer_healthy),
	    tf(v->fault), (unsigned)v->step, (unsigned)v->mask, (unsigned)v->io_fail);
	return o.n;
}

static void put_report(struct out *o, const char *key, const struct bit_report *r,
		       int64_t now_ms)
{
	if ((r == NULL) || !r->valid) {
		put(o, "\"%s\":null", key);
		return;
	}
	put(o, "\"%s\":{\"age_ms\":%lld,\"items\":[", key, (long long)(now_ms - r->at_ms));
	for (int i = 0; i < BIT_ITEMS; i++) {
		char value[64];

		bit_item_value(r, i, value, sizeof(value));
		put(o, "%s{\"name\":\"%s\",\"result\":\"%s\",\"critical\":%s,\"value\":\"%s\"}",
		    (i == 0) ? "" : ",", bit_item_name(i), bit_result_name(r->result[i]),
		    tf(bit_is_critical(i)), value);
	}
	put(o, "]}");
}

static size_t reply_bit(const struct cmd_view *v, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":true,\"healthy\":%s,", tf(v->healthy));
	put_report(&o, "pbit", v->pbit, v->now_ms);
	put(&o, ",");
	put_report(&o, "cbit", v->cbit, v->now_ms);
	put(&o, ",\"io_fail\":%u}\n", (unsigned)v->io_fail);
	return o.n;
}

/* One or two hex digits, optional 0x prefix. */
static bool parse_mask(const char *s, uint8_t *v)
{
	size_t len;

	if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
		s += 2;
	}
	len = strlen(s);
	if ((len == 0U) || (len > 2U)) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (isxdigit((unsigned char)s[i]) == 0) {
			return false;
		}
	}
	*v = (uint8_t)strtoul(s, NULL, 16);
	return true;
}

void cmd_linebuf_init(struct cmd_linebuf *lb)
{
	lb->n = 0U;
}

enum cmd_feed cmd_linebuf_feed(struct cmd_linebuf *lb, char c, char *line)
{
	if (c == '\n') {
		size_t n = lb->n;

		if ((n > 0U) && (lb->buf[n - 1U] == '\r')) {
			n--;
		}
		memcpy(line, lb->buf, n);
		line[n] = '\0';
		lb->n = 0U;
		return CMD_FEED_LINE;
	}
	if (lb->n == CMD_LINE_MAX) {
		lb->n = 0U;
		return CMD_FEED_TOO_LONG;
	}
	lb->buf[lb->n++] = c;
	return CMD_FEED_NONE;
}

size_t cmd_handle(const char *line, const struct cmd_view *v, struct cmd_result *res, char *out,
		  size_t out_size)
{
	char word[16] = "";
	char arg[CMD_LINE_MAX + 1U] = "";
	char extra;
	int n = sscanf(line, " %15s %128s %c", word, arg, &extra);
	bool leds;
	bool simple;

	res->action = CMD_ACT_NONE;
	res->leds = 0U;
	if ((n == 1) && (strcmp(word, "STATUS") == 0)) {
		return reply_status(v, out, out_size);
	}
	if ((n == 1) && (strcmp(word, "BIT") == 0)) {
		return reply_bit(v, out, out_size);
	}
	leds = ((n == 1) || (n == 2)) && (strcmp(word, "LEDS") == 0);
	simple = (n == 1) && ((strcmp(word, "LAMP_TEST") == 0) || (strcmp(word, "RUN_BIT") == 0));
	if (!leds && !simple) {
		return reply_error("unknown command", out, out_size);
	}
	if (v->role != RC_ROLE_ACTIVE) {
		return reply_not_active(v, out, out_size);
	}
	if (v->mode != RC_MODE_TEST) {
		return reply_error("operational mode", out, out_size);
	}
	if (leds) {
		if ((n != 2) || !parse_mask(arg, &res->leds)) {
			return reply_error("bad value", out, out_size);
		}
		res->action = CMD_ACT_SET_LEDS;
	} else if (strcmp(word, "LAMP_TEST") == 0) {
		res->action = CMD_ACT_LAMP_TEST;
	} else {
		res->action = CMD_ACT_RUN_BIT;
	}
	return reply_ok(out, out_size);
}

size_t cmd_too_long(char *out, size_t out_size)
{
	return reply_error("line too long", out, out_size);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 7`.

- [ ] **Step 5: Commit**

```bash
git add central/src/cmd.h central/src/cmd.c tests/host/test_cmd.c tests/host/CMakeLists.txt
git commit -m "feat: add TCP command handler with JSON replies" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: I/O card firmware integration

**Files:**
- Modify: `west.yml`, `io-card/prj.conf`, `io-card/CMakeLists.txt`, `io-card/src/leds.h`, `io-card/src/leds.c`, `io-card/src/main.c`

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces: firmware that reports `mode` and `io_fail` in STATUS, uses heartbeat `healthy`, answers RUN_BIT from the Active, and logs `mode:` and `io_bit:` lines. `int leds_get_mask(void)` (mask, or a negative error).

- [ ] **Step 1: Build configuration**

In `west.yml`, add `- hal_st` to `name-allowlist` after `- cmsis_6` (the LSM303AGR drivers depend on it).

Replace `io-card/prj.conf` with:

```
CONFIG_GPIO=y
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_RING_BUFFER=y
CONFIG_WATCHDOG=y
CONFIG_LOG=y
CONFIG_SENSOR=y
CONFIG_HWINFO=y
CONFIG_MAIN_STACK_SIZE=2048
```

In `io-card/CMakeLists.txt`, add `src/io_bit.c` and `src/mode.c` to `target_sources`.

- [ ] **Step 2: LED readback**

In `io-card/src/leds.h`, add before `#endif`:

```c
/* Reads the ring back from the pins; returns the mask or a negative error. */
int leds_get_mask(void);
```

In `io-card/src/leds.c`, add at the end:

```c
int leds_get_mask(void)
{
	int mask = 0;

	for (int i = 0; i < 8; i++) {
		int v = gpio_pin_get_dt(&ring[i]);

		if (v < 0) {
			return v;
		}
		mask |= (v & 1) << i;
	}
	return mask;
}
```

- [ ] **Step 3: Main loop changes**

In `io-card/src/main.c`:

Add includes after `#include <zephyr/device.h>`:

```c
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
```

and after `#include "arbiter.h"`:

```c
#include "io_bit.h"
```

and after `#include "leds.h"`:

```c
#include "mode.h"
```

Add after `#define RX_BUF_SIZE 256U`:

```c
#define IO_BIT_PERIOD_MS 1000
/* Loopback jumper: PD12 (output) to PD13 (input). */
#define LOOP_OUT_PIN 12
#define LOOP_IN_PIN 13
```

Add `uint32_t errors_at_check;` as the last member of `struct link`.

Add after `static uint8_t applied_mask;`:

```c
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct device *const gpiod = DEVICE_DT_GET(DT_NODELABEL(gpiod));
static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(lsm303agr_accel));
static const struct device *const magn = DEVICE_DT_GET(DT_NODELABEL(lsm303agr_magn));
static struct mode_state mode;
static uint8_t io_fail;
static bool watchdog_reset;
static bool bit_requested;
```

Replace `handle_frame` with:

```c
static void handle_frame(struct link *l, const struct rc_frame *f, int64_t now)
{
	struct rc_heartbeat hb;
	struct rc_set_outputs so;

	if (rc_decode_heartbeat(f, &hb) == 0) {
		struct rc_status st;
		uint8_t out[RC_FRAME_MAX];
		size_t n;

		arb_on_heartbeat(&arb, l->slot, hb.role, hb.healthy != 0U, now);
		st.seq = hb.seq;
		st.slot = l->slot;
		st.granted_role = arb_granted_role(&arb, l->slot);
		st.active_slot = arb_active(&arb);
		st.mode = mode.mode;
		st.io_fail = io_fail;
		n = rc_encode_status(&st, out, sizeof(out));
		send_frame(l->dev, out, n);
	} else if (rc_decode_set_outputs(f, &so) == 0) {
		if (arb_accepts_outputs(&arb, l->slot)) {
			set_outputs(so.mask);
		} else {
			l->outputs_rejected++;
		}
	} else if (rc_decode_run_bit(f) == 0) {
		if (arb_accepts_outputs(&arb, l->slot)) {
			bit_requested = true;
		} else {
			LOG_WRN("slot %c: RUN_BIT ignored, not Active", slot_name(l->slot));
		}
	} else {
		LOG_WRN("slot %c: unexpected frame type %u len %u", slot_name(l->slot), f->type,
			f->len);
	}
}
```

Add before `static int links_init(void)`:

```c
/* Drives the loopback output and reads the input: 1, 0, or a negative error. */
static int loop_read(int level)
{
	int err = gpio_pin_set(gpiod, LOOP_OUT_PIN, level);

	if (err != 0) {
		return err;
	}
	k_busy_wait(20);
	return gpio_pin_get(gpiod, LOOP_IN_PIN);
}

static void bit_run(int64_t now, bool pbit)
{
	struct io_bit_input in = {0};
	int led = leds_get_mask();
	uint8_t fail;

	in.loop_high = (loop_read(1) == 1);
	in.loop_low = (loop_read(0) != 0);
	in.led_read = (led < 0) ? (uint8_t)~applied_mask : (uint8_t)led;
	in.led_applied = applied_mask;
	in.accel_ready = device_is_ready(accel);
	in.magn_ready = device_is_ready(magn);
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		struct link *l = &links[i];
		uint32_t total = l->parser.crc_errors + l->parser.len_errors;

		in.uart_errors[i] = total - l->errors_at_check;
		l->errors_at_check = total;
	}
	in.watchdog_reset = watchdog_reset;
	fail = io_bit_eval(&in);
	if (pbit) {
		LOG_INF("t=%lld io_bit: pbit fail=0x%02x", now, fail);
	} else if (fail != io_fail) {
		LOG_INF("t=%lld io_bit: fail=0x%02x", now, fail);
	}
	io_fail = fail;
}

static int io_init(void)
{
	uint32_t cause;
	int err;

	if (!gpio_is_ready_dt(&button) || !device_is_ready(gpiod)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err == 0) {
		err = gpio_pin_configure(gpiod, LOOP_OUT_PIN, GPIO_OUTPUT_INACTIVE);
	}
	if (err == 0) {
		err = gpio_pin_configure(gpiod, LOOP_IN_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
	}
	if (err != 0) {
		return err;
	}
	err = hwinfo_get_reset_cause(&cause);
	if (err != 0) {
		LOG_ERR("reset cause unavailable: %d", err);
	} else {
		watchdog_reset = (cause & RESET_WATCHDOG) != 0U;
		err = hwinfo_clear_reset_cause();
		if (err != 0) {
			LOG_ERR("reset cause clear failed: %d", err);
		}
	}
	mode_init(&mode, gpio_pin_get_dt(&button) == 1, k_uptime_get());
	return 0;
}
```

Replace `main` with:

```c
int main(void)
{
	int64_t last_bit;
	bool button_error = false;
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
	err = io_init();
	if (err != 0) {
		LOG_ERR("button or loopback pins not ready: %d", err);
		return 0;
	}
	wdt_ch = wdt_start();
	if (wdt_ch < 0) {
		LOG_ERR("watchdog start failed: %d", wdt_ch);
		return 0;
	}
	arb_init(&arb);
	LOG_INF("io-card ready: A=USART2 PA2/PA3, B=UART4 PC10/PC11, loopback PD12->PD13");
	last_bit = k_uptime_get();
	bit_run(last_bit, true);

	for (;;) {
		int64_t now = k_uptime_get();
		uint8_t before = arb_active(&arb);
		int b;

		for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
			struct link *l = &links[i];
			uint8_t byte;

			while (ring_buf_get(&l->rb, &byte, 1U) == 1U) {
				struct rc_frame f;

				if (!rc_parser_feed(&l->parser, byte, &f)) {
					continue;
				}
				do {
					handle_frame(l, &f, now);
				} while (rc_parser_next(&l->parser, &f));
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
		b = gpio_pin_get_dt(&button);
		if (b < 0) {
			if (!button_error) {
				LOG_ERR("button read failed: %d", b);
				button_error = true;
			}
		} else {
			button_error = false;
			if (mode_on_sample(&mode, b == 1, now)) {
				LOG_INF("t=%lld mode: %s", now,
					(mode.mode == RC_MODE_TEST) ? "test" : "operational");
			}
		}
		if (bit_requested || ((now - last_bit) >= IO_BIT_PERIOD_MS)) {
			bit_run(now, false);
			last_bit = now;
			bit_requested = false;
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

- [ ] **Step 4: Build**

Run: `cd ~/ws/stm32f3-ws && . ~/ws/zp/zephyr-venv/bin/activate && west build -b stm32f3_disco@E -d build/rc-io-card ~/ws/redundant-controller/io-card 2>&1 | grep -E "FLASH|RAM|error"`
Expected: FLASH and RAM usage lines, no `error`.

- [ ] **Step 5: Flash and check the boot log (bench)**

Ask the owner to place a jumper wire from PD12 to PD13 (P2 header). Then:

Run: `tools/flash-remote.sh ~/ws/stm32f3-ws/build/rc-io-card/zephyr/zephyr.hex && tools/console-remote.sh 4`
Expected: `Verified OK`, then `io-card ready: ... loopback PD12->PD13` and `io_bit: pbit fail=0x00` (0x01 if the jumper is missing). The controllers still run the sub-project 1 daemon, so `unexpected frame type 1 len 5` warnings appear until Task 10 is deployed.

Ask the owner to press the blue USER button twice. Run `tools/console-remote.sh 8` while they do.
Expected: `mode: test` then `mode: operational`.

- [ ] **Step 6: Commit**

```bash
git add west.yml io-card
git commit -m "feat: add BIT, mode button, and RUN_BIT to the I/O card" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 10: Controller daemon integration

**Files:**
- Modify: `central/src/main.c` (full replacement below), `central/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1, 6, 7, 8.
- Produces: daemon with PBIT/CBIT, health in heartbeats, TCP server on port 5000, log events `pbit`, `bit`, `healthy`/`unhealthy`, `mode_changed`, `tcp_*`.

- [ ] **Step 1: CMake**

In `central/CMakeLists.txt`, change the executable line to:

```cmake
add_executable(rc-central src/main.c src/role.c src/bit.c src/cmd.c ../common/proto.c)
```

- [ ] **Step 2: Replace `central/src/main.c`**

```c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "bit.h"
#include "cmd.h"
#include "rc/proto.h"
#include "role.h"

#define SERIAL_DEV "/dev/serial0"
#define PEER_IFACE "eth0"
#define PEER_GROUP "ff02::1"
#define PEER_PORT 47000
#define HEARTBEAT_PERIOD_MS 20
#define TCP_PORT 5000
#define TCP_CLIENTS 4
#define HWMON_DIR "/sys/class/hwmon"
#define TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"

struct client {
	int fd;
	struct cmd_linebuf lb;
};

struct daemon {
	int serial;
	int peer;
	int listener;
	struct sockaddr_in6 group;
	struct rc_parser parser;
	struct role_state s;
	struct bit_health health;
	struct bit_report pbit;
	struct bit_report cbit;
	uint32_t errors_at_check;
	int64_t last_hb_ms;
	int64_t max_hb_interval_ms;
	uint32_t hb_seq;
	uint32_t peer_seq;
	bool peer_send_ok;
	bool peer_recv_ok;
	char uv_path[96];
	struct client clients[TCP_CLIENTS];
};

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

/* Returns the socket, or -1 (logged) if the cross-link cannot be set up. */
static int open_peer(struct sockaddr_in6 *group)
{
	unsigned int ifindex = if_nametoindex(PEER_IFACE);
	struct sockaddr_in6 local = {.sin6_family = AF_INET6, .sin6_port = htons(PEER_PORT),
				     .sin6_addr = IN6ADDR_ANY_INIT};
	int one = 1;
	int zero = 0;
	const char *step = NULL;
	int fd;

	if (ifindex == 0U) {
		fprintf(stderr, "error: cross-link disabled: if_nametoindex " PEER_IFACE ": %s\n",
			strerror(errno));
		return -1;
	}
	fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		step = "socket";
	} else if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
		step = "SO_REUSEADDR";
	} else if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, PEER_IFACE, sizeof(PEER_IFACE)) != 0) {
		step = "SO_BINDTODEVICE";
	} else if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		step = "bind";
	} else if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex)) != 0) {
		step = "IPV6_MULTICAST_IF";
	} else if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &zero, sizeof(zero)) != 0) {
		step = "IPV6_MULTICAST_LOOP";
	}
	if (step != NULL) {
		fprintf(stderr, "error: cross-link disabled: %s: %s\n", step, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
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

/* Returns the listening socket, or -1 (logged): commands are not needed for control. */
static int open_listener(void)
{
	struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(TCP_PORT),
				    .sin6_addr = IN6ADDR_ANY_INIT};
	int one = 1;
	int zero = 0;
	const char *step = NULL;
	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);

	if (fd < 0) {
		step = "socket";
	} else if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
		step = "SO_REUSEADDR";
	} else if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)) != 0) {
		step = "IPV6_V6ONLY";
	} else if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		step = "bind";
	} else if (listen(fd, TCP_CLIENTS) != 0) {
		step = "listen";
	}
	if (step != NULL) {
		fprintf(stderr, "error: tcp server disabled: %s: %s\n", step, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

static bool read_long(const char *path, long *v)
{
	FILE *f = fopen(path, "r");
	bool ok;

	if (f == NULL) {
		return false;
	}
	ok = (fscanf(f, "%ld", v) == 1);
	fclose(f);
	return ok;
}

/* Finds the under-voltage flag of the rpi_volt hwmon device. */
static int find_undervoltage_path(char *path, size_t size)
{
	for (int i = 0; i < 16; i++) {
		char name_path[64];
		char name[32] = "";
		FILE *f;

		snprintf(name_path, sizeof(name_path), HWMON_DIR "/hwmon%d/name", i);
		f = fopen(name_path, "r");
		if (f == NULL) {
			continue;
		}
		if (fgets(name, sizeof(name), f) == NULL) {
			name[0] = '\0';
		}
		fclose(f);
		if (strncmp(name, "rpi_volt", 8) == 0) {
			snprintf(path, size, HWMON_DIR "/hwmon%d/in0_lcrit_alarm", i);
			return 0;
		}
	}
	path[0] = '\0';
	return -1;
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
		{ROLE_EV_MODE_CHANGED, "mode_changed"},
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((ev & names[i].bit) != 0U) {
			printf("t=%lld slot=%c role=%s mode=%s step=%u event=%s\n", (long long)t,
			       slot_name(s->slot), role_name(s->role),
			       (s->mode == RC_MODE_TEST) ? "test" : "operational",
			       (unsigned)s->step, names[i].name);
		}
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

static void send_outputs(struct daemon *d, uint8_t mask)
{
	struct rc_set_outputs so = {.mask = mask};
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_encode_set_outputs(&so, out, sizeof(out));

	write_all(d->serial, out, n);
}

static void send_run_bit(struct daemon *d)
{
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_encode_run_bit(out, sizeof(out));

	write_all(d->serial, out, n);
}

static void run_bit(struct daemon *d, int64_t t)
{
	struct bit_report prev = d->cbit;
	struct bit_input in;
	uint32_t errors = d->parser.crc_errors + d->parser.len_errors;
	long v = 0;

	in.status_age_ms = t - d->s.last_status_ms;
	in.link_errors = errors - d->errors_at_check;
	in.max_hb_interval_ms = d->max_hb_interval_ms;
	in.peer_age_ms = d->s.peer_seen ? (t - d->s.last_peer_ms) : INT64_MAX;
	in.undervoltage_ok = (d->uv_path[0] != '\0') && read_long(d->uv_path, &v);
	in.undervoltage = in.undervoltage_ok && (v != 0);
	in.temp_ok = read_long(TEMP_PATH, &v);
	in.temp_mc = in.temp_ok ? (int32_t)v : 0;
	d->errors_at_check = errors;
	d->max_hb_interval_ms = 0;

	bit_evaluate(&in, t, &d->cbit);
	if (!d->health.pbit_done) {
		d->pbit = d->cbit;
		printf("t=%lld event=pbit result=%s\n", (long long)t,
		       bit_critical_pass(&d->pbit) ? "pass" : "fail");
	}
	for (int i = 0; i < BIT_ITEMS; i++) {
		if (!prev.valid || (prev.result[i] != d->cbit.result[i])) {
			char value[64];

			bit_item_value(&d->cbit, i, value, sizeof(value));
			printf("t=%lld event=bit item=%s result=%s value=\"%s\"\n", (long long)t,
			       bit_item_name(i), bit_result_name(d->cbit.result[i]), value);
		}
	}
	if (bit_health_update(&d->health, &d->cbit) || !prev.valid) {
		printf("t=%lld slot=%c event=%s\n", (long long)t, slot_name(d->s.slot),
		       d->health.healthy ? "healthy" : "unhealthy");
	}
}

static void fill_view(const struct daemon *d, int64_t t, struct cmd_view *v)
{
	v->slot = d->s.slot;
	v->role = d->s.role;
	v->mode = d->s.mode;
	v->active_slot = d->s.active_slot;
	v->healthy = d->health.healthy;
	v->referee_ok = d->s.referee_ok;
	v->peer_ok = d->s.peer_ok;
	v->peer_healthy = d->s.peer_healthy;
	v->fault = d->s.fault;
	v->step = d->s.step;
	v->mask = role_current_mask(&d->s);
	v->io_fail = d->s.io_fail;
	v->pbit = &d->pbit;
	v->cbit = &d->cbit;
	v->now_ms = t;
}

static size_t handle_line(struct daemon *d, const char *line, int64_t t, char *out)
{
	struct cmd_view v;
	struct cmd_result res;
	size_t len;

	fill_view(d, t, &v);
	len = cmd_handle(line, &v, &res, out, CMD_REPLY_MAX);
	switch (res.action) {
	case CMD_ACT_SET_LEDS:
		role_set_test_mask(&d->s, res.leds, t);
		break;
	case CMD_ACT_LAMP_TEST:
		role_start_lamp_test(&d->s, t);
		break;
	case CMD_ACT_RUN_BIT:
		run_bit(d, t);
		send_run_bit(d);
		break;
	case CMD_ACT_NONE:
		break;
	}
	if (res.action != CMD_ACT_NONE) {
		printf("t=%lld event=tcp_command line=\"%s\"\n", (long long)t, line);
	}
	return len;
}

static void client_close(struct client *c, int64_t t, const char *why)
{
	printf("t=%lld event=tcp_disconnect reason=\"%s\"\n", (long long)t, why);
	close(c->fd);
	c->fd = -1;
}

static bool client_send(struct client *c, const char *buf, size_t n)
{
	ssize_t w = send(c->fd, buf, n, MSG_NOSIGNAL);

	return (w >= 0) && ((size_t)w == n);
}

static void on_accept(struct daemon *d, int64_t t)
{
	int fd = accept4(d->listener, NULL, NULL, SOCK_NONBLOCK);

	if (fd < 0) {
		if ((errno != EAGAIN) && (errno != EINTR)) {
			printf("t=%lld event=tcp_accept_failed error=\"%s\"\n", (long long)t,
			       strerror(errno));
		}
		return;
	}
	for (int i = 0; i < TCP_CLIENTS; i++) {
		if (d->clients[i].fd < 0) {
			d->clients[i].fd = fd;
			cmd_linebuf_init(&d->clients[i].lb);
			printf("t=%lld event=tcp_connect\n", (long long)t);
			return;
		}
	}
	printf("t=%lld event=tcp_rejected reason=full\n", (long long)t);
	close(fd);
}

/* A misbehaving client is dropped; it never blocks the control loop. */
static void on_client(struct daemon *d, struct client *c, int64_t t)
{
	char buf[256];
	ssize_t n = recv(c->fd, buf, sizeof(buf), 0);

	if (n == 0) {
		client_close(c, t, "closed");
		return;
	}
	if (n < 0) {
		if (errno != EAGAIN) {
			client_close(c, t, strerror(errno));
		}
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		char line[CMD_LINE_MAX + 1U];
		char out[CMD_REPLY_MAX];
		enum cmd_feed r = cmd_linebuf_feed(&c->lb, buf[i], line);
		size_t len;

		if (r == CMD_FEED_NONE) {
			continue;
		}
		if (r == CMD_FEED_TOO_LONG) {
			len = cmd_too_long(out, sizeof(out));
			/* The client is dropped either way; a failed send changes nothing. */
			(void)client_send(c, out, len);
			client_close(c, t, "line too long");
			return;
		}
		len = handle_line(d, line, t, out);
		if (!client_send(c, out, len)) {
			client_close(c, t, "send failed");
			return;
		}
	}
}

static unsigned on_serial(struct daemon *d, int64_t t)
{
	uint8_t buf[64];
	ssize_t n = read(d->serial, buf, sizeof(buf));
	unsigned ev = 0U;

	if ((n < 0) && (errno != EAGAIN)) {
		die("serial read");
	}
	for (ssize_t i = 0; i < n; i++) {
		struct rc_frame f;
		struct rc_status st;

		if (!rc_parser_feed(&d->parser, buf[i], &f)) {
			continue;
		}
		do {
			if (rc_decode_status(&f, &st) == 0) {
				ev |= role_on_status(&d->s, &st, t);
			}
		} while (rc_parser_next(&d->parser, &f));
	}
	return ev;
}

static unsigned on_peer(struct daemon *d, int64_t t)
{
	uint8_t buf[RC_FRAME_MAX];
	ssize_t n = recv(d->peer, buf, sizeof(buf), 0);
	struct rc_parser pp;
	unsigned ev = 0U;

	/* The cross-link is monitoring only: a receive failure never stops control. */
	if ((n < 0) && (errno != EAGAIN)) {
		if (d->peer_recv_ok) {
			printf("t=%lld event=peer_recv_failed error=%s\n", (long long)t,
			       strerror(errno));
			d->peer_recv_ok = false;
		}
	} else if ((n >= 0) && !d->peer_recv_ok) {
		printf("t=%lld event=peer_recv_ok\n", (long long)t);
		d->peer_recv_ok = true;
	}
	rc_parser_init(&pp);
	for (ssize_t i = 0; i < n; i++) {
		struct rc_frame f;
		struct rc_peer pm;

		if (!rc_parser_feed(&pp, buf[i], &f)) {
			continue;
		}
		do {
			if (rc_decode_peer(&f, &pm) == 0) {
				ev |= role_on_peer(&d->s, &pm, t);
			}
		} while (rc_parser_next(&pp, &f));
	}
	return ev;
}

static void send_heartbeat(struct daemon *d, int64_t t)
{
	uint8_t healthy = d->health.healthy ? 1U : 0U;
	struct rc_heartbeat hb = {.seq = d->hb_seq++, .role = d->s.role, .healthy = healthy};
	struct rc_peer pm = {.seq = d->peer_seq++, .slot = d->s.slot, .role = d->s.role,
			     .referee_ok = d->s.referee_ok ? 1U : 0U, .step = d->s.step,
			     .healthy = healthy, .test_mask = d->s.test_mask};
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_encode_heartbeat(&hb, out, sizeof(out));

	if ((t - d->last_hb_ms) > d->max_hb_interval_ms) {
		d->max_hb_interval_ms = t - d->last_hb_ms;
	}
	d->last_hb_ms = t;
	write_all(d->serial, out, n);
	n = rc_encode_peer(&pm, out, sizeof(out));
	/* The cross-link is monitoring only: a send failure never stops control. */
	if (d->peer < 0) {
		return;
	}
	if (sendto(d->peer, out, n, 0, (struct sockaddr *)&d->group, sizeof(d->group)) < 0) {
		if ((errno != EAGAIN) && d->peer_send_ok) {
			printf("t=%lld event=peer_send_failed error=%s\n", (long long)t,
			       strerror(errno));
			d->peer_send_ok = false;
		}
	} else if (!d->peer_send_ok) {
		printf("t=%lld event=peer_send_ok\n", (long long)t);
		d->peer_send_ok = true;
	}
}

int main(void)
{
	static struct daemon d;
	int64_t start;
	int64_t next_hb;
	int64_t next_bit;

	setvbuf(stdout, NULL, _IOLBF, 0);
	d.serial = open_serial();
	d.peer = open_peer(&d.group);
	d.listener = open_listener();
	for (int i = 0; i < TCP_CLIENTS; i++) {
		d.clients[i].fd = -1;
	}
	if (find_undervoltage_path(d.uv_path, sizeof(d.uv_path)) != 0) {
		fprintf(stderr, "error: rpi_volt hwmon not found; supply_voltage reports error\n");
	}
	d.peer_send_ok = true;
	d.peer_recv_ok = true;
	rc_parser_init(&d.parser);
	start = now_ms();
	role_init(&d.s, start);
	bit_health_init(&d.health);
	d.last_hb_ms = start;
	next_hb = start;
	next_bit = start + BIT_PBIT_DELAY_MS;
	printf("t=%lld rc-central started\n", (long long)start);

	for (;;) {
		struct pollfd fds[3 + TCP_CLIENTS];
		int64_t wait = next_hb - now_ms();
		int64_t t;
		unsigned ev = 0U;
		uint8_t mask;

		fds[0] = (struct pollfd){.fd = d.serial, .events = POLLIN};
		fds[1] = (struct pollfd){.fd = d.peer, .events = POLLIN};
		fds[2] = (struct pollfd){.fd = d.listener, .events = POLLIN};
		for (int i = 0; i < TCP_CLIENTS; i++) {
			fds[3 + i] = (struct pollfd){.fd = d.clients[i].fd, .events = POLLIN};
		}
		if (poll(fds, 3 + TCP_CLIENTS, (wait > 0) ? (int)wait : 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			die("poll");
		}
		t = now_ms();
		if ((fds[0].revents & POLLIN) != 0) {
			ev |= on_serial(&d, t);
		}
		if ((fds[1].revents & POLLIN) != 0) {
			ev |= on_peer(&d, t);
		}
		ev |= role_tick(&d.s, t);
		log_events(ev, &d.s, t);

		if (t >= next_bit) {
			run_bit(&d, t);
			next_bit += BIT_PERIOD_MS;
			if (next_bit <= t) {
				next_bit = t + BIT_PERIOD_MS;
			}
		}
		if (t >= next_hb) {
			send_heartbeat(&d, t);
			next_hb += HEARTBEAT_PERIOD_MS;
			if (next_hb <= t) {
				next_hb = t + HEARTBEAT_PERIOD_MS;
			}
		}
		if ((fds[2].revents & POLLIN) != 0) {
			on_accept(&d, t);
		}
		for (int i = 0; i < TCP_CLIENTS; i++) {
			if ((d.clients[i].fd >= 0) &&
			    ((fds[3 + i].revents & (POLLIN | POLLHUP | POLLERR)) != 0)) {
				on_client(&d, &d.clients[i], t);
			}
		}
		if (role_chaser_due(&d.s, t, &mask) || role_test_output_due(&d.s, t, &mask)) {
			send_outputs(&d, mask);
		}
	}
	return 0;
}
```

- [ ] **Step 3: Host tests still pass**

Run: `cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 7`.

- [ ] **Step 4: Deploy and check PBIT (bench)**

Run: `tools/deploy-central.sh`
Expected: both Pis build with `-Werror` and print `rc-central active`.

Run: `for h in 192.168.45.50 192.168.45.176; do ssh -i ~/.ssh/id_ed25519_alpental_pi alpental@$h "journalctl -u rc-central -b --since '-1 min' -o cat | grep -E 'event=(pbit|bit|healthy|unhealthy)'"; done`
Expected per Pi: `event=pbit result=pass`, five `event=bit` lines with `supply_voltage result=fail` (under-voltage bench) and the others `pass`, and `event=healthy`.

- [ ] **Step 5: Check TCP by hand (bench)**

Run: `printf 'STATUS\nBIT\nLEDS 0x55\n' | nc -w 1 192.168.45.50 5000`
Expected: a STATUS line with `"mode":"operational"`, a BIT line with both `pbit` and `cbit`, and `{"ok":false,"error":"operational mode"}` (or `not active` if rc-a is Standby).

Run: `python3 -c "print('x'*200)" | nc -w 1 192.168.45.50 5000`
Expected: `{"ok":false,"error":"line too long"}` and the connection closes; the journal shows `tcp_disconnect reason="line too long"`, and no role change.

- [ ] **Step 6: Commit**

```bash
git add central
git commit -m "feat: add BIT, health, and TCP server to the controller daemon" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 11: rcctl client

**Files:**
- Create: `tools/rcctl.py`, `tests/host/test_rcctl.py`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: the TCP interface from Tasks 8 and 10.
- Produces: `parse_host(text) -> (host, port)`, `request(addr, line) -> dict`, `safe_request`, `query_all(hosts, line) -> {addr: dict}`, `send_to_active(hosts, line) -> (addr, dict)`, CLI `rcctl.py [--hosts H1,H2] status|bit|leds <hex>|lamp|run-bit`.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_rcctl.py`:

```python
#!/usr/bin/env python3
"""Tests for tools/rcctl.py against fake controllers on localhost."""
import json
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import rcctl  # noqa: E402

ACTIVE = {"ok": True, "role": "active", "slot": "B"}
STANDBY = {"ok": True, "role": "standby", "slot": "A"}
NOT_ACTIVE = {"ok": False, "error": "not active", "active": "B"}
OK = {"ok": True}


class FakeController:
    """Answers each line with replies[first word], recording the lines."""

    def __init__(self, replies):
        self.replies = replies
        self.lines = []
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen()
        self.addr = "127.0.0.1:%d" % self.sock.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn, conn.makefile("rwb") as f:
                for raw in f:
                    line = raw.decode().strip()
                    self.lines.append(line)
                    word = line.split()[0] if line else ""
                    f.write((json.dumps(self.replies[word]) + "\n").encode())
                    f.flush()

    def close(self):
        self.sock.close()


class RcctlTest(unittest.TestCase):
    def test_parse_host(self):
        self.assertEqual(rcctl.parse_host("10.0.0.1"), ("10.0.0.1", 5000))
        self.assertEqual(rcctl.parse_host("127.0.0.1:5123"), ("127.0.0.1", 5123))

    def test_command_goes_to_active(self):
        a = FakeController({"STATUS": STANDBY, "LEDS": NOT_ACTIVE})
        b = FakeController({"STATUS": ACTIVE, "LEDS": OK})
        try:
            host, reply = rcctl.send_to_active([a.addr, b.addr], "LEDS 0x55")
        finally:
            a.close()
            b.close()
        self.assertEqual(host, b.addr)
        self.assertEqual(reply, OK)
        self.assertNotIn("LEDS 0x55", a.lines)

    def test_retries_other_after_not_active(self):
        a = FakeController({"STATUS": dict(ACTIVE, slot="A"), "LEDS": NOT_ACTIVE})
        b = FakeController({"STATUS": STANDBY, "LEDS": OK})
        try:
            host, reply = rcctl.send_to_active([a.addr, b.addr], "LEDS 0x55")
        finally:
            a.close()
            b.close()
        self.assertIn("LEDS 0x55", a.lines)
        self.assertEqual(host, b.addr)
        self.assertEqual(reply, OK)

    def test_unreachable_controller_is_reported(self):
        b = FakeController({"STATUS": ACTIVE})
        dead = FakeController({})
        dead.close()
        try:
            out = rcctl.query_all([dead.addr, b.addr], "STATUS")
        finally:
            b.close()
        self.assertFalse(out[dead.addr]["ok"])
        self.assertEqual(out[b.addr], ACTIVE)


if __name__ == "__main__":
    unittest.main()
```

Append to `tests/host/CMakeLists.txt`:

```cmake
add_test(NAME rcctl COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_rcctl.py)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tests/host/test_rcctl.py 2>&1 | tail -1`
Expected: `ModuleNotFoundError: No module named 'rcctl'`.

- [ ] **Step 3: Implement**

Create `tools/rcctl.py` (mode 755):

```python
#!/usr/bin/env python3
"""Upper-controller client for the redundant controller.

Usage: rcctl.py [--hosts H1,H2] status|bit|leds <hex>|lamp|run-bit
Hosts come from --hosts, then RC_HOSTS, then the bench defaults.
"""
import argparse
import json
import os
import socket
import sys
import time

PORT = 5000
TIMEOUT_S = 2.0
DEFAULT_HOSTS = "192.168.45.50,192.168.45.176"


def parse_host(text):
    host, sep, port = text.rpartition(":")
    if not sep:
        return text, PORT
    return host, int(port)


def request(addr, line):
    host, port = parse_host(addr)
    with socket.create_connection((host, port), timeout=TIMEOUT_S) as s:
        s.sendall((line + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                raise ConnectionError("connection closed")
            buf += chunk
    return json.loads(buf)


def safe_request(addr, line):
    try:
        return request(addr, line)
    except (OSError, ValueError) as e:
        return {"ok": False, "error": str(e)}


def query_all(hosts, line):
    return {h: safe_request(h, line) for h in hosts}


def send_to_active(hosts, line):
    """Sends line to the controller reporting Active; tries the other once on 'not active'."""
    status = query_all(hosts, "STATUS")
    order = sorted(hosts, key=lambda h: status[h].get("role") != "active")
    host, reply = order[0], {"ok": False, "error": "no controllers"}
    for host in order[:2]:
        reply = safe_request(host, line)
        if reply.get("error") != "not active":
            break
    return host, reply


def print_status(replies):
    keys = ["slot", "role", "mode", "healthy", "active", "referee", "peer", "peer_healthy",
            "fault", "step", "mask", "io_fail", "error"]
    hosts = list(replies)
    print("%-13s" % "" + "".join("%-22s" % h for h in hosts))
    for k in keys:
        if any(k in r for r in replies.values()):
            print("%-13s" % k + "".join("%-22s" % replies[h].get(k, "") for h in hosts))


def print_bit(replies):
    for host, r in replies.items():
        if not r.get("ok"):
            print("== %s error: %s" % (host, r.get("error")))
            continue
        print("== %s healthy=%s io_fail=0x%02x" % (host, r["healthy"], r["io_fail"]))
        for key in ("pbit", "cbit"):
            rep = r.get(key)
            if rep is None:
                print("  %s: not run yet" % key)
                continue
            print("  %s (age %d ms)" % (key, rep["age_ms"]))
            for it in rep["items"]:
                print("    %-15s %-6s %-9s %s" % (it["name"], it["result"],
                                                 "critical" if it["critical"] else "",
                                                 it["value"]))


def main(argv=None):
    p = argparse.ArgumentParser(description="Redundant controller client")
    p.add_argument("--hosts", default=os.environ.get("RC_HOSTS", DEFAULT_HOSTS),
                   help="comma-separated controller addresses (host or host:port)")
    p.add_argument("command", choices=["status", "bit", "leds", "lamp", "run-bit"])
    p.add_argument("value", nargs="?", help="LED mask in hex for leds, e.g. 0x55")
    args = p.parse_args(argv)
    hosts = args.hosts.split(",")

    if args.command in ("status", "bit"):
        replies = query_all(hosts, args.command.upper())
        (print_status if args.command == "status" else print_bit)(replies)
        return 0 if all(r.get("ok") for r in replies.values()) else 1
    if args.command == "leds":
        if args.value is None:
            p.error("leds needs a value, e.g. 0x55")
        line = "LEDS " + args.value
    elif args.command == "lamp":
        line = "LAMP_TEST"
    else:
        line = "RUN_BIT"
    host, reply = send_to_active(hosts, line)
    print("%s: %s" % (host, json.dumps(reply)))
    if args.command == "run-bit" and reply.get("ok"):
        time.sleep(0.2)
        print_bit({host: safe_request(host, "BIT")})
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S tests/host -B build/host >/dev/null && cmake --build build/host >/dev/null && ctest --test-dir build/host --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 8`.

Run (bench): `tools/rcctl.py status && tools/rcctl.py bit`
Expected: a two-column status table (one Active, one Standby, both `healthy True`) and BIT items for both Pis.

- [ ] **Step 5: Commit**

```bash
git add tools/rcctl.py tests/host/test_rcctl.py tests/host/CMakeLists.txt
git commit -m "feat: add rcctl client for the TCP interface" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

### Task 12: Experiments, test log, and README

**Files:**
- Modify: `tools/collect.sh`, `docs/test-log.md`, `README.md`

**Precondition:** Tasks 9-11 deployed; PD12-PD13 jumper in place; owner at the bench for experiments 2-4.

- [ ] **Step 1: Widen the log filter**

In `tools/collect.sh`, change the journal grep pattern to:

```
event=(role_changed|referee_|peer_|fault|pbit|bit|healthy|unhealthy|mode_changed|tcp_command)
```

and the console grep to `grep -E 'active:|ready|io_bit|mode:'`.

- [ ] **Step 2: Experiment 1, PBIT (3 runs)**

Restart both daemons (`sudo systemctl restart rc-central` on each Pi) three times. After each, run `tools/collect.sh 1 2`.
Pass: each Pi logs `event=pbit result=pass` and `event=healthy`; `supply_voltage` shows `fail` (under-voltage bench), all other items `pass`.

- [ ] **Step 3: Experiment 2, unhealthy handover (3 runs)**

Start `tools/console-remote.sh 60` in the background. Ask the owner to disconnect the Active Pi's RX wire (Pi pin 10) and note the time. Then run `tools/collect.sh 2 1`.
Pass: the Active logs `event=bit item=io_link result=fail` and `event=unhealthy`; the I/O card logs `active: X -> Y` within 2.5 s of the unhealthy event. Compute the time from the Active's `referee_lost` to the I/O card switch from the logs. After the owner reconnects the wire: `event=healthy` about 3 s later, and the Pi stays Standby. Alternate which Pi is Active.

- [ ] **Step 4: Experiment 3, test mode (3 runs)**

1. Owner presses USER; the console shows `mode: test` and the LEDs go dark.
2. `tools/rcctl.py leds 0x55`: alternate LEDs light.
3. Stop the Active: `ssh ... sudo systemctl stop rc-central`. The new Active keeps 0x55 (check that `tools/rcctl.py status` shows `mask 85` on the new Active).
4. `tools/rcctl.py lamp`: all on 1 s, off 1 s, back to 0x55.
5. Owner presses USER; the chaser resumes; `tools/rcctl.py leds 0x0f` prints `operational mode` and exits 1.
6. Start the stopped daemon again.

Pass: every step as described.

- [ ] **Step 5: Experiment 4, I/O card BIT (1 run)**

Ask the owner to remove the PD12-PD13 jumper, wait 3 s, and put it back.
Pass: the console shows `io_bit: fail=0x01` within 1 s and `io_bit: fail=0x00` after it is back; `tools/rcctl.py bit` shows `io_fail=0x01` while it is out; no `active:` line.

- [ ] **Step 6: Experiment 5, I/O card restart (3 runs)**

Run: `ssh -i ~/.ssh/id_ed25519_alpental_pi seokyoungjeong@seoks-macbook-air '/opt/homebrew/bin/openocd -f board/stm32f3discovery.cfg -c "init; reset halt; sleep 3000; resume; exit"'`, then `tools/collect.sh 1 3`.
Pass: both Pis log `referee_lost`, `unhealthy`, `referee_back`, and `healthy`; the I/O card elects the previous Active (`- -> <previous>`); no other `active:` line.

- [ ] **Step 7: Write the results**

Append a "Sub-project 2" section to `docs/test-log.md` with the date, firmware and daemon commit, the under-voltage note, and one table per experiment (run, observed values, pass/fail), plus a short paragraph per experiment on anything unexpected.

In `README.md`, add after the "Wiring" section:

````markdown
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
````

- [ ] **Step 8: Commit**

```bash
git add tools/collect.sh docs/test-log.md README.md
git commit -m "docs: record BIT and test mode experiments" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```
