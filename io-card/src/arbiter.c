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

void arb_on_heartbeat(struct arbiter *a, uint8_t slot, uint8_t reported_role, bool healthy,
		      int64_t now_ms)
{
	if (!valid_slot(slot)) {
		return;
	}
	a->slot[slot].heard = true;
	a->slot[slot].last_rx_ms = now_ms;
	a->slot[slot].reported_role = reported_role;
	a->slot[slot].healthy = healthy;
}

bool arb_present(const struct arbiter *a, uint8_t slot, int64_t now_ms)
{
	if (!valid_slot(slot)) {
		return false;
	}
	return a->slot[slot].heard && ((now_ms - a->slot[slot].last_rx_ms) <= ARB_HB_TIMEOUT_MS);
}

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
