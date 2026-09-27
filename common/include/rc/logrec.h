#ifndef RC_LOGREC_H_
#define RC_LOGREC_H_

#include <stdint.h>

#define RC_LOG_RECORD_SIZE 64U
#define RC_LOG_MAGIC 0x4C52U
#define RC_LOG_VERSION 1U

#define RC_LOG_SNAPSHOT 1U
#define RC_LOG_EVENT 2U

#define RC_LOG_F_HEALTHY (1U << 0)
#define RC_LOG_F_REFEREE_OK (1U << 1)
#define RC_LOG_F_PEER_OK (1U << 2)
#define RC_LOG_F_PEER_HEALTHY (1U << 3)
#define RC_LOG_F_FAULT (1U << 4)
#define RC_LOG_F_IO_TIME_VALID (1U << 5)

#define RC_LOG_EV_NONE 0U
#define RC_LOG_EV_STARTED 1U
#define RC_LOG_EV_ROLE_CHANGED 2U
#define RC_LOG_EV_HEALTHY 3U
#define RC_LOG_EV_UNHEALTHY 4U
#define RC_LOG_EV_REFEREE_LOST 5U
#define RC_LOG_EV_REFEREE_BACK 6U
#define RC_LOG_EV_PEER_LOST 7U
#define RC_LOG_EV_PEER_BACK 8U
#define RC_LOG_EV_MODE_CHANGED 9U
#define RC_LOG_EV_FAULT_SET 10U
#define RC_LOG_EV_FAULT_CLEARED 11U
#define RC_LOG_EV_BIT_ITEM 12U
#define RC_LOG_EV_TCP_OUTPUT 13U
#define RC_LOG_EV_SLOT_LEARNED 14U

/* BIT result codes 0-2 match enum bit_result; 3 = not run yet. */
#define RC_LOG_BIT_NOT_RUN 3U

struct rc_log_record {
	uint8_t type;
	uint32_t seq;
	uint32_t io_time_ms;
	uint64_t mono_ms;
	uint64_t wall_ms;
	uint8_t slot;
	uint8_t role;
	uint8_t mode;
	uint8_t flags;
	uint8_t active_slot;
	uint8_t io_fail;
	uint16_t bit_results;
	uint16_t step;
	uint8_t mask;
	uint8_t event;
	uint32_t detail;
};

/* Writes RC_LOG_RECORD_SIZE bytes: magic, version, fields, zero reserve, CRC. */
void rc_log_encode(const struct rc_log_record *r, uint8_t *out);

/* Returns 0, or -1 for a wrong magic, version, type, or CRC. */
int rc_log_decode(const uint8_t *in, struct rc_log_record *r);

/* 2 bits per item, item 0 in the low bits; at most 8 items. */
uint16_t rc_log_pack_bits(const uint8_t *results, int n);
uint8_t rc_log_bit_result(uint16_t packed, int item);

#endif /* RC_LOGREC_H_ */
