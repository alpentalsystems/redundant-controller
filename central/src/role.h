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
	int64_t active_since_ms;
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
