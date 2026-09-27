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
	if (!role_may_drive(s) || ((now_ms - s->last_step_ms) < ROLE_CHASER_PERIOD_MS)) {
		return false;
	}
	s->step++;
	s->last_step_ms = now_ms;
	*mask = (uint8_t)(1U << (s->step % 8U));
	return true;
}
