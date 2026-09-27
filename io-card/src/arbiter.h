#ifndef ARBITER_H_
#define ARBITER_H_

#include <stdbool.h>
#include <stdint.h>

#include "rc/proto.h"

#define ARB_HB_TIMEOUT_MS 100
#define ARB_ELECTION_WINDOW_MS 1500
/* An unhealthy Active hands over only after the condition holds this long. */
#define ARB_HEALTH_HOLD_MS 1200

struct arb_slot {
	bool heard;
	int64_t last_rx_ms;
	uint8_t reported_role;
	bool healthy;
};

struct arbiter {
	struct arb_slot slot[2];
	uint8_t active;
	bool electing;
	int64_t election_start_ms;
	bool handover_pending;
	int64_t handover_since_ms;
};

void arb_init(struct arbiter *a);
void arb_on_heartbeat(struct arbiter *a, uint8_t slot, uint8_t reported_role, bool healthy,
		      int64_t now_ms);

/* Updates presence and the Active slot; returns true when the Active slot changed. */
bool arb_tick(struct arbiter *a, int64_t now_ms);

bool arb_present(const struct arbiter *a, uint8_t slot, int64_t now_ms);
uint8_t arb_active(const struct arbiter *a);

/* RC_ROLE_UNKNOWN while no slot is Active (election pending or nobody present). */
uint8_t arb_granted_role(const struct arbiter *a, uint8_t slot);

bool arb_accepts_outputs(const struct arbiter *a, uint8_t slot);
int64_t arb_last_rx_ms(const struct arbiter *a, uint8_t slot);

#endif /* ARBITER_H_ */
