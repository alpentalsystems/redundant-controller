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
			    .step = step, .healthy = 1U};

	return m;
}

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
	/* Keeps driving: the I/O card decides whether outputs are applied. */
	CHECK(role_may_drive(&s));
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

static void test_peer_without_referee_is_not_a_conflict(void)
{
	struct role_state s;
	struct rc_status ac = status(RC_SLOT_B, RC_ROLE_ACTIVE);
	struct rc_peer stale = {.seq = 1U, .slot = RC_SLOT_A, .role = RC_ROLE_ACTIVE,
				.referee_ok = 0U, .step = 3U};

	role_init(&s, 0);
	(void)role_on_status(&s, &ac, 100);
	/* Old Active lost its UART but still runs and claims Active without a referee. */
	for (int64_t t = 120; t < 5000; t += 20) {
		(void)role_on_peer(&s, &stale, t);
		CHECK(!s.fault);
	}
	CHECK(role_may_drive(&s));
}

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

static void test_status_age_covers_whole_period(void)
{
	struct role_state s;
	struct rc_status m = status(RC_SLOT_A, RC_ROLE_ACTIVE);

	role_init(&s, 0);
	for (int64_t t = 0; t <= 400; t += 20) {
		(void)role_on_status(&s, &m, t);
	}
	/* 150 ms gap, then STATUS again: a check at 1000 ms still sees it. */
	for (int64_t t = 550; t <= 1000; t += 20) {
		(void)role_on_status(&s, &m, t);
	}
	CHECK(role_take_status_age(&s, 1000) == 150);
	CHECK(role_take_status_age(&s, 1010) == 20); /* reset by the previous take; last STATUS at 990 */
	CHECK(role_take_status_age(&s, 1300) == 310); /* gap still open */
}

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
	test_peer_without_referee_is_not_a_conflict();
	test_test_mode_pauses_chaser();
	test_operator_mask_sent_and_refreshed();
	test_lamp_test_sequence();
	test_chaser_resumes_after_test_mode();
	test_new_active_keeps_test_mask();
	test_mode_change_clears_test_mask();
	test_status_and_peer_fields_recorded();
	test_status_age_covers_whole_period();
	test_io_time_estimate();
	return CHECK_DONE();
}
