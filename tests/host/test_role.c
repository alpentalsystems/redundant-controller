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
