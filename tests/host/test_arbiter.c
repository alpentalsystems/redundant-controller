#include "arbiter.h"
#include "check.h"

/* Advance time in 20 ms heartbeat periods from `from` to `to` (exclusive). */
static void run(struct arbiter *a, int64_t from, int64_t to, bool send_a, uint8_t role_a,
		bool send_b, uint8_t role_b)
{
	for (int64_t t = from; t < to; t += 20) {
		if (send_a) {
			arb_on_heartbeat(a, RC_SLOT_A, role_a, true, t);
		}
		if (send_b) {
			arb_on_heartbeat(a, RC_SLOT_B, role_b, true, t);
		}
		(void)arb_tick(a, t);
	}
}

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
	arb_on_heartbeat(&a, RC_SLOT_B, RC_ROLE_STANDBY, true, 2080);
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
		arb_on_heartbeat(&a, 2U, RC_ROLE_ACTIVE, true, t);
		arb_on_heartbeat(&a, RC_SLOT_NONE, RC_ROLE_ACTIVE, true, t);
		(void)arb_tick(&a, t);
	}
	CHECK(arb_active(&a) == RC_SLOT_NONE);
	CHECK(arb_granted_role(&a, 2U) == RC_ROLE_UNKNOWN);
	CHECK(!arb_accepts_outputs(&a, 2U));
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
	test_election_prefers_healthy();
	test_election_keeps_reported_active_when_both_unhealthy();
	test_healthy_beats_unhealthy_reported_active();
	test_unhealthy_active_hands_over_after_hold();
	test_recovered_slot_stays_standby();
	test_unhealthy_active_kept_when_other_unhealthy();
	test_unhealthy_active_kept_when_other_absent();
	test_common_mode_recovery_does_not_move_active();
	test_lost_active_hands_over_to_unhealthy_standby();
	return CHECK_DONE();
}
