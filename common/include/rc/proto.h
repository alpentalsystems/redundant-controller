#ifndef RC_PROTO_H_
#define RC_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RC_SYNC 0xA5U
#define RC_MAX_PAYLOAD 16U
/* sync, type, len, crc (2) */
#define RC_FRAME_OVERHEAD 5U
#define RC_FRAME_MAX (RC_FRAME_OVERHEAD + RC_MAX_PAYLOAD)

#define RC_MSG_HEARTBEAT 1U
#define RC_MSG_STATUS 2U
#define RC_MSG_SET_OUTPUTS 3U
#define RC_MSG_PEER 4U
#define RC_MSG_RUN_BIT 5U

#define RC_ROLE_UNKNOWN 0U
#define RC_ROLE_STANDBY 1U
#define RC_ROLE_ACTIVE 2U

#define RC_SLOT_A 0U
#define RC_SLOT_B 1U
#define RC_SLOT_NONE 0xFFU

#define RC_MODE_OPERATIONAL 0U
#define RC_MODE_TEST 1U

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection. */
uint16_t rc_crc16(const uint8_t *data, size_t len);

struct rc_frame {
	uint8_t type;
	uint8_t len;
	uint8_t payload[RC_MAX_PAYLOAD];
};

/* Returns the frame length, or 0 if the payload or buffer is invalid. */
size_t rc_frame_encode(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out,
		       size_t out_size);

#define RC_PARSER_QUEUE 4U

struct rc_parser {
	uint8_t buf[RC_FRAME_MAX];
	uint8_t n;
	struct rc_frame queue[RC_PARSER_QUEUE];
	uint8_t q_head;
	uint8_t q_count;
	uint32_t crc_errors;
	uint32_t len_errors;
	uint32_t queue_overflows;
};

void rc_parser_init(struct rc_parser *p);

/*
 * Feeds one byte; returns true when *out holds a complete, valid frame.
 * A bad candidate frame drops only its sync byte, so frames that follow a
 * false sync are still found. Call rc_parser_next() until it returns false
 * to collect further frames completed by the same byte.
 */
bool rc_parser_feed(struct rc_parser *p, uint8_t byte, struct rc_frame *out);
bool rc_parser_next(struct rc_parser *p, struct rc_frame *out);

struct rc_heartbeat {
	uint32_t seq;
	uint8_t role;
	uint8_t healthy;
};

struct rc_status {
	uint32_t seq;
	uint8_t slot;
	uint8_t granted_role;
	uint8_t active_slot;
	uint8_t mode;
	uint8_t io_fail;
	uint32_t io_time_ms;
};

struct rc_set_outputs {
	uint8_t mask;
};

struct rc_peer {
	uint32_t seq;
	uint8_t slot;
	uint8_t role;
	uint8_t referee_ok;
	uint16_t step;
	uint8_t healthy;
	uint8_t test_mask;
};

size_t rc_encode_heartbeat(const struct rc_heartbeat *m, uint8_t *out, size_t out_size);
size_t rc_encode_status(const struct rc_status *m, uint8_t *out, size_t out_size);
size_t rc_encode_set_outputs(const struct rc_set_outputs *m, uint8_t *out, size_t out_size);
size_t rc_encode_peer(const struct rc_peer *m, uint8_t *out, size_t out_size);
size_t rc_encode_run_bit(uint8_t *out, size_t out_size);

/* Return 0 on success, -1 if the frame type or length does not match. */
int rc_decode_heartbeat(const struct rc_frame *f, struct rc_heartbeat *m);
int rc_decode_status(const struct rc_frame *f, struct rc_status *m);
int rc_decode_set_outputs(const struct rc_frame *f, struct rc_set_outputs *m);
int rc_decode_peer(const struct rc_frame *f, struct rc_peer *m);
int rc_decode_run_bit(const struct rc_frame *f);

#endif /* RC_PROTO_H_ */
