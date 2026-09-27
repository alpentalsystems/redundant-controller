#ifndef ROLE_H_
#define ROLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "rc/proto.h"

#define ROLE_REFEREE_TIMEOUT_MS 100
#define ROLE_PEER_TIMEOUT_MS 100
#define ROLE_CHASER_PERIOD_MS 200
#define ROLE_LAMP_STEP_MS 1000

#define ROLE_EV_ROLE_CHANGED (1U << 0)
#define ROLE_EV_REFEREE_LOST (1U << 1)
#define ROLE_EV_REFEREE_BACK (1U << 2)
#define ROLE_EV_PEER_LOST (1U << 3)
#define ROLE_EV_PEER_BACK (1U << 4)
#define ROLE_EV_FAULT_SET (1U << 5)
#define ROLE_EV_FAULT_CLEARED (1U << 6)
#define ROLE_EV_SLOT_LEARNED (1U << 7)
#define ROLE_EV_MODE_CHANGED (1U << 8)

struct role_state {
	uint8_t slot;
	uint8_t role;
	bool referee_ok;
	int64_t last_status_ms;
	bool peer_ok;
	bool peer_seen;
	int64_t last_peer_ms;
	uint8_t peer_role;
	bool peer_referee_ok;
	uint16_t peer_step;
	bool fault;
	int64_t active_since_ms;
	uint16_t step;
	int64_t last_step_ms;
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
	int64_t max_status_gap_ms;
	uint32_t io_time_ms;
	uint16_t io_boot_id;
	bool io_time_valid;
};

void role_init(struct role_state *s, int64_t now_ms);
unsigned role_on_status(struct role_state *s, const struct rc_status *m, int64_t now_ms);
unsigned role_on_peer(struct role_state *s, const struct rc_peer *m, int64_t now_ms);
unsigned role_tick(struct role_state *s, int64_t now_ms);
bool role_may_drive(const struct role_state *s);

/* When driving and a step is due, advances the chaser and returns true with *mask set. */
bool role_chaser_due(struct role_state *s, int64_t now_ms, uint8_t *mask);

/* Operator output for test mode; takes effect at once. */
void role_set_test_mask(struct role_state *s, uint8_t mask, int64_t now_ms);

/* All on for ROLE_LAMP_STEP_MS, all off for ROLE_LAMP_STEP_MS, then the operator mask. */
void role_start_lamp_test(struct role_state *s, int64_t now_ms);

/* In test mode while driving: returns true with *mask set when the output is due. */
bool role_test_output_due(struct role_state *s, int64_t now_ms, uint8_t *mask);

/* The operator mask in test mode, else the chaser position. */
uint8_t role_current_mask(const struct role_state *s);

/*
 * Longest time without STATUS since the previous call, including the gap
 * still open at now_ms. BIT uses it so that every gap is seen within one
 * check period, whatever the check phase.
 */
int64_t role_take_status_age(struct role_state *s, int64_t now_ms);

/* I/O card time at now_ms: last STATUS time plus the time since; false before any STATUS. */
bool role_io_time(const struct role_state *s, int64_t now_ms, uint32_t *io_time_ms);

#endif /* ROLE_H_ */
