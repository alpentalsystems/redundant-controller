#include <string.h>

#include "role.h"

void role_init(struct role_state *s, int64_t now_ms)
{
	memset(s, 0, sizeof(*s));
	s->slot = RC_SLOT_NONE;
	s->role = RC_ROLE_UNKNOWN;
	s->peer_role = RC_ROLE_UNKNOWN;
	s->active_slot = RC_SLOT_NONE;
	s->last_status_ms = now_ms;
	s->last_peer_ms = now_ms;
}

static unsigned update_fault(struct role_state *s)
{
	/*
	 * Only a peer that claims Active after we became Active, while it still
	 * reaches the referee, is a conflict. A peer without a referee holds no grant.
	 */
	bool f = (s->role == RC_ROLE_ACTIVE) && s->peer_ok && (s->peer_role == RC_ROLE_ACTIVE) &&
		 s->peer_referee_ok && (s->last_peer_ms > s->active_since_ms);

	if (f == s->fault) {
		return 0U;
	}
	s->fault = f;
	return f ? ROLE_EV_FAULT_SET : ROLE_EV_FAULT_CLEARED;
}

unsigned role_on_status(struct role_state *s, const struct rc_status *m, int64_t now_ms)
{
	unsigned ev = 0U;

	if ((now_ms - s->last_status_ms) > s->max_status_gap_ms) {
		s->max_status_gap_ms = now_ms - s->last_status_ms;
	}
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

unsigned role_on_peer(struct role_state *s, const struct rc_peer *m, int64_t now_ms)
{
	unsigned ev = 0U;

	if ((s->slot != RC_SLOT_NONE) && (m->slot == s->slot)) {
		return 0U;
	}
	s->last_peer_ms = now_ms;
	s->peer_role = m->role;
	s->peer_referee_ok = (m->referee_ok != 0U);
	s->peer_step = m->step;
	s->peer_test_mask = m->test_mask;
	s->peer_healthy = (m->healthy != 0U);
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
	/* Referee replies are not required: the I/O card only applies outputs from its Active. */
	return (s->role == RC_ROLE_ACTIVE) && !s->fault;
}

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

int64_t role_take_status_age(struct role_state *s, int64_t now_ms)
{
	int64_t age = now_ms - s->last_status_ms;

	if (s->max_status_gap_ms > age) {
		age = s->max_status_gap_ms;
	}
	s->max_status_gap_ms = 0;
	return age;
}
